#include "core/ffts_engine.h"
#include <acl/acl.h>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "logger/logger.h"

namespace transport {
namespace {

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

Status AclStatus(aclError ret, const char* expr)
{
    if (ret == ACL_SUCCESS) { return Status::Ok; }
    UC_ERROR("FFTS failed to call {} ret={}", expr, static_cast<int32_t>(ret));
    return Status::Failed;
}

class DeviceContext {
public:
    DeviceContext() = default;
    ~DeviceContext() = default;

    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    bool IsInitialized() const noexcept { return initialized_; }
    int DeviceId() const noexcept { return device_id_; }
    aclrtStream Stream() const noexcept { return stream_; }

    void MarkInitialized(int device_id, aclrtStream stream) noexcept
    {
        device_id_ = device_id;
        stream_ = stream;
        initialized_ = true;
    }

    void Reset() noexcept
    {
        stream_ = nullptr;
        device_id_ = -1;
        initialized_ = false;
    }

private:
    aclrtStream stream_ = nullptr;
    int device_id_ = -1;
    bool initialized_ = false;
};

}  // namespace

struct FftsEngine::Impl {
    std::mutex mutex;
    DeviceContext device_context;
    uint16_t max_ready_lanes = 8;

    template <typename Fn>
    Status RunOnDevice(int device_id, Fn&& fn)
    {
        int32_t previous_device = -1;
        bool should_restore = false;
        const auto enter_status = EnterDevice(device_id, previous_device, should_restore);
        if (enter_status != Status::Ok) { return enter_status; }

        const auto status = std::forward<Fn>(fn)();
        RestoreDevice(previous_device, should_restore);
        return status;
    }

    Status EnterDevice(int device_id, int32_t& previous_device, bool& should_restore)
    {
        previous_device = -1;
        should_restore = false;

        int32_t current_device = -1;
        const auto get_ret = aclrtGetDevice(&current_device);
        if (get_ret == ACL_SUCCESS) {
            previous_device = current_device;
            if (current_device == device_id) { return Status::Ok; }
            should_restore = true;
        }

        const auto set_ret = aclrtSetDevice(device_id);
        if (set_ret != ACL_SUCCESS) {
            UC_ERROR("FFTS failed to enter device context device_id={} ret={}", device_id,
                     static_cast<int32_t>(set_ret));
            should_restore = false;
            return Status::Failed;
        }
        return Status::Ok;
    }

    void RestoreDevice(int32_t previous_device, bool should_restore)
    {
        if (!should_restore) { return; }
        const auto ret = aclrtSetDevice(previous_device);
        if (ret != ACL_SUCCESS) {
            UC_ERROR("FFTS failed to restore device context device_id={} ret={}",
                     previous_device, static_cast<int32_t>(ret));
        }
    }

    Status MapHostOnCurrentDevice(void* host, size_t size, bool requires_unregister,
                                  FftsMemoryRegistration& registration)
    {
        void* device = nullptr;
        const auto ret = aclrtHostGetDevicePointer(host, &device, 0);
        if (ret != ACL_SUCCESS) {
            UC_ERROR("FFTS failed to get mapped device pointer addr=0x{:x} size={} ret={}",
                     PtrToU64(host), size, static_cast<int32_t>(ret));
            return Status::Failed;
        }

        if (device == nullptr) {
            UC_ERROR("FFTS host registration returned null device pointer addr=0x{:x} size={}",
                     PtrToU64(host), size);
            return Status::Failed;
        }

        registration.origin_addr = host;
        registration.ffts_addr = device;
        registration.size = size;
        registration.requires_unregister = requires_unregister;
        return Status::Ok;
    }

    Status SynchronizeOnCurrentDevice()
    {
        return AclStatus(aclrtSynchronizeStream(device_context.Stream()),
                         "aclrtSynchronizeStream(ffts)");
    }
};

FftsEngine::FftsEngine() : impl_(std::make_unique<Impl>()) {}

FftsEngine::~FftsEngine() { (void)Shutdown(); }

Status FftsEngine::DiscoverDeviceIds(std::vector<int>& device_ids)
{
    device_ids.clear();

    uint32_t count = 0;
    const auto ret = aclrtGetDeviceCount(&count);
    if (ret != ACL_SUCCESS) {
        UC_ERROR("FFTS failed to discover device count ret={}", static_cast<int32_t>(ret));
        return Status::Failed;
    }
    if (count == 0) { return Status::Failed; }

    device_ids.reserve(count);
    for (uint32_t i = 0; i < count; ++i) { device_ids.push_back(static_cast<int>(i)); }
    return Status::Ok;
}

Status FftsEngine::Init(const FftsEngineOptions& options)
{
    if (options.device_id < 0 || options.max_ready_lanes == 0) {
        return Status::InvalidArgument;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->device_context.IsInitialized()) { return Status::Ok; }

    aclrtStream stream = nullptr;
    auto status = impl_->RunOnDevice(options.device_id, [&stream]() {
        return AclStatus(aclrtCreateStream(&stream), "aclrtCreateStream(ffts)");
    });
    if (status != Status::Ok) { return status; }

    impl_->device_context.MarkInitialized(options.device_id, stream);
    impl_->max_ready_lanes = options.max_ready_lanes;
    return Status::Ok;
}

Status FftsEngine::Shutdown()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);

    Status status = Status::Ok;
    if (impl_->device_context.Stream() != nullptr) {
        bool ran_on_device = false;
        status = impl_->RunOnDevice(impl_->device_context.DeviceId(),
                                    [this, &ran_on_device]() {
                                        ran_on_device = true;
                                        return AclStatus(
                                            aclrtDestroyStream(impl_->device_context.Stream()),
                                            "aclrtDestroyStream(ffts)");
                                    });
        if (!ran_on_device) { return status; }
    }

    impl_->device_context.Reset();
    impl_->max_ready_lanes = 8;
    return status;
}

Status FftsEngine::RegisterHostMemory(void* host, size_t size,
                                      FftsMemoryRegistration& registration)
{
    registration = {};
    if (host == nullptr || size == 0) { return Status::InvalidArgument; }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device_context.IsInitialized()) { return Status::Failed; }

    return impl_->RunOnDevice(impl_->device_context.DeviceId(),
                              [this, host, size, &registration]() {
                                  auto ret = aclrtHostRegisterV2(
                                      host, size,
                                      ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
                                  if (ret != ACL_SUCCESS) {
                                      UC_ERROR("FFTS failed to register host memory addr=0x{:x} "
                                               "size={} ret={}",
                                               PtrToU64(host), size,
                                               static_cast<int32_t>(ret));
                                      return Status::Failed;
                                  }

                                  const auto status = impl_->MapHostOnCurrentDevice(
                                      host, size, true, registration);
                                  if (status != Status::Ok) {
                                      (void)aclrtHostUnregister(host);
                                      registration = {};
                                  }
                                  return status;
                              });
}

Status FftsEngine::MapRegisteredHostMemory(void* host, size_t size,
                                           FftsMemoryRegistration& registration)
{
    registration = {};
    if (host == nullptr || size == 0) { return Status::InvalidArgument; }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device_context.IsInitialized()) { return Status::Failed; }

    return impl_->RunOnDevice(impl_->device_context.DeviceId(),
                              [this, host, size, &registration]() {
                                  return impl_->MapHostOnCurrentDevice(host, size, false,
                                                                       registration);
                              });
}

Status FftsEngine::UnregisterHostMemory(const FftsMemoryRegistration& registration)
{
    if (!registration.requires_unregister) { return Status::Ok; }
    if (registration.origin_addr == nullptr || registration.size == 0) {
        return Status::InvalidArgument;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device_context.IsInitialized()) { return Status::Failed; }

    return impl_->RunOnDevice(impl_->device_context.DeviceId(), [&registration]() {
        return AclStatus(aclrtHostUnregister(registration.origin_addr),
                         "aclrtHostUnregister(ffts)");
    });
}

Status FftsEngine::RegisterDeviceMemory(void* device, size_t size,
                                        FftsMemoryRegistration& registration)
{
    registration = {};
    if (device == nullptr || size == 0) { return Status::InvalidArgument; }

    registration.origin_addr = device;
    registration.ffts_addr = device;
    registration.size = size;
    registration.requires_unregister = false;
    return Status::Ok;
}

Status FftsEngine::UnregisterDeviceMemory(const FftsMemoryRegistration& registration)
{
    (void)registration;
    return Status::Ok;
}

Status FftsEngine::Submit(const std::vector<FftsCopySpec>& copies)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device_context.IsInitialized() || impl_->device_context.Stream() == nullptr) {
        return Status::Failed;
    }

    return impl_->RunOnDevice(impl_->device_context.DeviceId(), [this, &copies]() {
        FftsDispatcher dispatcher;
        auto status = dispatcher.BuildCopies(copies, impl_->max_ready_lanes);
        if (status != Status::Ok) { return status; }

        status = dispatcher.Launch(impl_->device_context.Stream());
        if (status != Status::Ok) { return status; }
        return impl_->SynchronizeOnCurrentDevice();
    });
}

Status FftsEngine::Synchronize()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device_context.IsInitialized() || impl_->device_context.Stream() == nullptr) {
        return Status::Failed;
    }

    return impl_->RunOnDevice(impl_->device_context.DeviceId(),
                              [this]() { return impl_->SynchronizeOnCurrentDevice(); });
}

}  // namespace transport
