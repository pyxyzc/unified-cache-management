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

    Status Init(int device_id)
    {
        if (device_id < 0) { return Status::InvalidArgument; }
        if (initialized_) { return Status::Ok; }

        return RunOnDevice(device_id, [this, device_id]() {
            auto status = AclStatus(aclrtCreateStream(&stream_), "aclrtCreateStream(ffts)");
            if (status != Status::Ok) { return status; }

            device_id_ = device_id;
            initialized_ = true;
            return Status::Ok;
        });
    }

    Status Shutdown()
    {
        Status result = Status::Ok;
        if (stream_ != nullptr) {
            bool ran_on_device = false;
            const auto status = RunOnDevice(device_id_, [this, &ran_on_device]() {
                ran_on_device = true;
                const auto destroy_status =
                    AclStatus(aclrtDestroyStream(stream_), "aclrtDestroyStream(ffts)");
                stream_ = nullptr;
                return destroy_status;
            });
            if (!ran_on_device) { return status; }
            if (status != Status::Ok) { result = status; }
        }
        device_id_ = -1;
        initialized_ = false;
        return result;
    }

    Status RegisterHostMemory(void* host, size_t size, FftsMemoryRegistration& registration)
    {
        registration = {};
        if (host == nullptr || size == 0) { return Status::InvalidArgument; }
        if (!initialized_) { return Status::Failed; }

        return RunOnDevice(device_id_, [host, size, &registration]() {
            void* device = nullptr;
            auto ret = aclrtHostRegisterV2(host, size, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
            if (ret != ACL_SUCCESS) {
                UC_ERROR("FFTS failed to register host memory addr=0x{:x} size={} ret={}",
                         PtrToU64(host), size, static_cast<int32_t>(ret));
                return Status::Failed;
            }

            ret = aclrtHostGetDevicePointer(host, &device, 0);
            if (ret != ACL_SUCCESS) {
                UC_ERROR("FFTS failed to get mapped device pointer addr=0x{:x} size={} ret={}",
                         PtrToU64(host), size, static_cast<int32_t>(ret));
                (void)aclrtHostUnregister(host);
                return Status::Failed;
            }

            if (device == nullptr) {
                UC_ERROR("FFTS host registration returned null device pointer addr=0x{:x} "
                         "size={}",
                         PtrToU64(host), size);
                (void)aclrtHostUnregister(host);
                return Status::Failed;
            }

            registration.origin_addr = host;
            registration.ffts_addr = device;
            registration.size = size;
            registration.requires_unregister = true;
            return Status::Ok;
        });
    }

    Status UnregisterHostMemory(const FftsMemoryRegistration& registration)
    {
        if (!registration.requires_unregister) { return Status::Ok; }
        if (registration.origin_addr == nullptr || registration.size == 0) {
            return Status::InvalidArgument;
        }
        if (!initialized_) { return Status::Failed; }

        return RunOnDevice(device_id_, [&registration]() {
            return AclStatus(aclrtHostUnregister(registration.origin_addr),
                             "aclrtHostUnregister(ffts)");
        });
    }

    Status RegisterDeviceMemory(void* device, size_t size,
                                FftsMemoryRegistration& registration) const
    {
        registration = {};
        if (device == nullptr || size == 0) { return Status::InvalidArgument; }

        registration.origin_addr = device;
        registration.ffts_addr = device;
        registration.size = size;
        registration.requires_unregister = false;
        return Status::Ok;
    }

    Status UnregisterDeviceMemory(const FftsMemoryRegistration& registration) const
    {
        (void)registration;
        return Status::Ok;
    }

    Status Synchronize()
    {
        if (!initialized_ || stream_ == nullptr) { return Status::Failed; }

        return RunOnDevice(device_id_, [this]() { return SynchronizeOnCurrentDevice(); });
    }

    Status Submit(const std::vector<FftsCopySpec>& copies, uint16_t max_ready_lanes)
    {
        if (!initialized_ || stream_ == nullptr) { return Status::Failed; }

        return RunOnDevice(device_id_, [this, &copies, max_ready_lanes]() {
            FftsDispatcher dispatcher;
            auto status = dispatcher.BuildCopies(copies, max_ready_lanes);
            if (status != Status::Ok) { return status; }

            status = dispatcher.Launch(stream_);
            if (status != Status::Ok) { return status; }
            return SynchronizeOnCurrentDevice();
        });
    }

    bool IsInitialized() const noexcept { return initialized_; }

private:
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

    Status SynchronizeOnCurrentDevice()
    {
        return AclStatus(aclrtSynchronizeStream(stream_), "aclrtSynchronizeStream(ffts)");
    }

    aclrtStream stream_ = nullptr;
    int device_id_ = -1;
    bool initialized_ = false;
};

}  // namespace

struct FftsEngine::Impl {
    std::mutex mutex;
    DeviceContext device_context;
    uint16_t max_ready_lanes = 8;
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

    auto status = impl_->device_context.Init(options.device_id);
    if (status != Status::Ok) { return status; }

    impl_->max_ready_lanes = options.max_ready_lanes;
    return Status::Ok;
}

Status FftsEngine::Shutdown()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->device_context.Shutdown();
    impl_->max_ready_lanes = 8;
    return status;
}

Status FftsEngine::RegisterHostMemory(void* host, size_t size,
                                      FftsMemoryRegistration& registration)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device_context.RegisterHostMemory(host, size, registration);
}

Status FftsEngine::UnregisterHostMemory(const FftsMemoryRegistration& registration)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device_context.UnregisterHostMemory(registration);
}

Status FftsEngine::RegisterDeviceMemory(void* device, size_t size,
                                        FftsMemoryRegistration& registration)
{
    return impl_->device_context.RegisterDeviceMemory(device, size, registration);
}

Status FftsEngine::UnregisterDeviceMemory(const FftsMemoryRegistration& registration)
{
    return impl_->device_context.UnregisterDeviceMemory(registration);
}

Status FftsEngine::Submit(const std::vector<FftsCopySpec>& copies)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device_context.Submit(copies, impl_->max_ready_lanes);
}

Status FftsEngine::Synchronize()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device_context.Synchronize();
}

}  // namespace transport
