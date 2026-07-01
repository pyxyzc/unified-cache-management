#include "core/ffts_transport.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "core/ffts_engine.h"
#include "logger/logger.h"
#include "transport_common.h"

namespace transport {
namespace {

constexpr uint32_t kMetadataVersion = 1;

bool SameMemoryRegion(const MemoryRegion& left, const MemoryRegion& right)
{
    return left.addr == right.addr && left.length == right.length && left.type == right.type &&
           left.device_id == right.device_id;
}

bool ContainsRange(uint64_t base, uint64_t size, uint64_t addr, uint64_t length)
{
    if (length == 0 || addr < base) { return false; }
    const auto offset = addr - base;
    return offset <= size && length <= size - offset;
}

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

bool FitsSizeT(uint64_t value)
{
    if constexpr (sizeof(size_t) >= sizeof(uint64_t)) {
        (void)value;
        return true;
    } else {
        return value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    }
}

Status ResolveDeviceIds(const std::vector<int>& configured, std::vector<int>& device_ids)
{
    device_ids.clear();
    if (configured.empty()) {
        const auto status = FftsEngine::DiscoverDeviceIds(device_ids);
        if (status != Status::Ok) { return status; }
    } else {
        device_ids = configured;
    }
    if (device_ids.empty()) { return Status::Failed; }

    std::unordered_set<int> seen;
    for (const auto device_id : device_ids) {
        if (device_id < 0 || !seen.emplace(device_id).second) {
            return Status::InvalidArgument;
        }
    }
    std::sort(device_ids.begin(), device_ids.end());
    return Status::Ok;
}

}  // namespace

struct FftsTransport::Impl {
    struct MemoryRecord {
        MemoryRegion region;
        std::unordered_map<int, FftsMemoryRegistration> registrations;
    };

    struct ResolvedAddress {
        const MemoryRecord* record = nullptr;
        uint64_t offset = 0;
    };

    std::shared_mutex mutex;
    std::unordered_map<int, std::unique_ptr<FftsEngine>> engines;
    std::vector<int> device_ids;
    std::unordered_map<MemoryHandle, MemoryRecord> memories;
    MemoryHandle next_handle = 1;
    bool initialized = false;
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
    FftsTransport::EngineHooks hooks;
#endif

    FftsEngine* FindEngine(int device_id) const
    {
        const auto it = engines.find(device_id);
        return it == engines.end() ? nullptr : it->second.get();
    }

    bool HasEngine(int device_id) const
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return std::find(device_ids.begin(), device_ids.end(), device_id) !=
                   device_ids.end();
        }
#endif
        return FindEngine(device_id) != nullptr;
    }

    Status ShutdownEngine(int device_id)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) { return hooks.shutdown ? hooks.shutdown(device_id) : Status::Failed; }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed : engine->Shutdown();
    }

    Status RegisterHostOnEngine(int device_id, void* host, size_t size,
                                FftsMemoryRegistration& registration)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return hooks.register_host ? hooks.register_host(device_id, host, size, registration)
                                       : Status::Failed;
        }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed
                                 : engine->RegisterHostMemory(host, size, registration);
    }

    Status UnregisterHostOnEngine(int device_id, const FftsMemoryRegistration& registration)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return hooks.unregister_host ? hooks.unregister_host(device_id, registration)
                                         : Status::Failed;
        }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed : engine->UnregisterHostMemory(registration);
    }

    Status RegisterDeviceOnEngine(int device_id, void* device, size_t size,
                                  FftsMemoryRegistration& registration)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return hooks.register_device ? hooks.register_device(device_id, device, size,
                                                                 registration)
                                         : Status::Failed;
        }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed
                                 : engine->RegisterDeviceMemory(device, size, registration);
    }

    Status UnregisterDeviceOnEngine(int device_id, const FftsMemoryRegistration& registration)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return hooks.unregister_device ? hooks.unregister_device(device_id, registration)
                                           : Status::Failed;
        }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed : engine->UnregisterDeviceMemory(registration);
    }

    Status SubmitToEngine(int device_id, const std::vector<FftsCopySpec>& copies)
    {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (hooks.init) {
            return hooks.submit ? hooks.submit(device_id, copies) : Status::Failed;
        }
#endif
        auto* engine = FindEngine(device_id);
        return engine == nullptr ? Status::Failed : engine->Submit(copies);
    }

    std::vector<ResolvedAddress> ResolveCandidates(uint64_t addr, uint64_t length) const
    {
        std::vector<ResolvedAddress> matches;
        for (const auto& item : memories) {
            const auto& record = item.second;
            const auto base = PtrToU64(record.region.addr);
            if (!ContainsRange(base, record.region.length, addr, length)) { continue; }

            matches.push_back(ResolvedAddress{&record, addr - base});
        }
        return matches;
    }

    Status ResolveFftsAddress(const MemoryRecord& record, int device_id, uint64_t offset,
                              void*& out) const
    {
        const auto it = record.registrations.find(device_id);
        if (it == record.registrations.end()) { return Status::InvalidArgument; }

        out = reinterpret_cast<void*>(PtrToU64(it->second.ffts_addr) + offset);
        return Status::Ok;
    }

    Status BuildLocalDeviceHostCopy(const ResolvedAddress& dst, const ResolvedAddress& src,
                                    uint64_t length, FftsCopySpec& copy, int& device_id) const
    {
        if (dst.record->region.type == src.record->region.type) {
            return Status::InvalidArgument;
        }

        const auto* device_record =
            dst.record->region.type == MemoryType::Device ? dst.record : src.record;
        const auto candidate_device_id = device_record->region.device_id;
        if (candidate_device_id < 0 || !HasEngine(candidate_device_id)) {
            return Status::InvalidArgument;
        }

        void* dst_ptr = nullptr;
        void* src_ptr = nullptr;
        auto status = ResolveFftsAddress(*dst.record, candidate_device_id, dst.offset, dst_ptr);
        if (status != Status::Ok) { return status; }
        status = ResolveFftsAddress(*src.record, candidate_device_id, src.offset, src_ptr);
        if (status != Status::Ok) { return status; }

        copy = FftsCopySpec{dst_ptr, src_ptr, static_cast<size_t>(length)};
        device_id = candidate_device_id;
        return Status::Ok;
    }

    Status BuildLocalDeviceDeviceCopy(const ResolvedAddress& dst, const ResolvedAddress& src,
                                      uint64_t length, FftsCopySpec& copy, int& device_id) const
    {
        if (dst.record->region.type != MemoryType::Device ||
            src.record->region.type != MemoryType::Device) {
            return Status::InvalidArgument;
        }

        const auto candidate_device_id = dst.record->region.device_id;
        if (candidate_device_id < 0 || candidate_device_id != src.record->region.device_id ||
            !HasEngine(candidate_device_id)) {
            return Status::InvalidArgument;
        }

        void* dst_ptr = nullptr;
        void* src_ptr = nullptr;
        auto status = ResolveFftsAddress(*dst.record, candidate_device_id, dst.offset, dst_ptr);
        if (status != Status::Ok) { return status; }
        status = ResolveFftsAddress(*src.record, candidate_device_id, src.offset, src_ptr);
        if (status != Status::Ok) { return status; }

        copy = FftsCopySpec{dst_ptr, src_ptr, static_cast<size_t>(length)};
        device_id = candidate_device_id;
        return Status::Ok;
    }

    Status BuildCopy(OperationDirect direct, uint64_t dst_addr, uint64_t src_addr,
                     uint64_t length, FftsCopySpec& copy, int& device_id) const
    {
        const auto dst_candidates = ResolveCandidates(dst_addr, length);
        const auto src_candidates = ResolveCandidates(src_addr, length);
        if (dst_candidates.empty() || src_candidates.empty()) {
            return Status::InvalidArgument;
        }

        for (const auto& dst : dst_candidates) {
            for (const auto& src : src_candidates) {
                const auto status =
                    direct == OperationDirect::LocalDeviceHost
                        ? BuildLocalDeviceHostCopy(dst, src, length, copy, device_id)
                        : BuildLocalDeviceDeviceCopy(dst, src, length, copy, device_id);
                if (status == Status::Ok) { return Status::Ok; }
            }
        }

        return Status::InvalidArgument;
    }

    Status UnregisterRecord(const MemoryRecord& record)
    {
        Status result = Status::Ok;
        for (const auto& item : record.registrations) {
            if (!HasEngine(item.first)) {
                result = Status::Failed;
                continue;
            }

            const auto status = record.region.type == MemoryType::Host
                                    ? UnregisterHostOnEngine(item.first, item.second)
                                    : UnregisterDeviceOnEngine(item.first, item.second);
            if (status != Status::Ok) { result = status; }
        }
        return result;
    }

    Status RegisterHostMemory(const MemoryRegion& memory, MemoryRecord& record)
    {
        for (const auto device_id : device_ids) {
            if (!HasEngine(device_id)) {
                (void)UnregisterRecord(record);
                return Status::Failed;
            }

            FftsMemoryRegistration registration;
            const auto status = RegisterHostOnEngine(
                device_id, memory.addr, static_cast<size_t>(memory.length), registration);
            if (status != Status::Ok) {
                (void)UnregisterRecord(record);
                return status;
            }
            record.registrations.emplace(device_id, registration);
        }
        return Status::Ok;
    }

    Status RegisterDeviceMemory(const MemoryRegion& memory, MemoryRecord& record)
    {
        if (memory.device_id < 0) { return Status::InvalidArgument; }

        if (!HasEngine(memory.device_id)) { return Status::InvalidArgument; }

        FftsMemoryRegistration registration;
        const auto status = RegisterDeviceOnEngine(
            memory.device_id, memory.addr, static_cast<size_t>(memory.length), registration);
        if (status != Status::Ok) { return status; }

        record.registrations.emplace(memory.device_id, registration);
        return Status::Ok;
    }
};

FftsTransport::FftsTransport() : impl_(std::make_unique<Impl>()) {}

#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
FftsTransport::FftsTransport(EngineHooks hooks) : impl_(std::make_unique<Impl>())
{
    impl_->hooks = std::move(hooks);
}
#endif

FftsTransport::~FftsTransport() { (void)Shutdown(); }

const char* FftsTransport::Name() const { return kFftsTransportProtocol; }

Status FftsTransport::Init(const InitAttrs& options)
{
    auto* attrs = dynamic_cast<const FftsInitAttrs*>(&options);
    if (attrs == nullptr || attrs->max_ready_lanes == 0) { return Status::InvalidArgument; }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    if (impl_->initialized) { return Status::Ok; }

    std::vector<int> device_ids;
    auto status = ResolveDeviceIds(attrs->device_ids, device_ids);
    if (status != Status::Ok) { return status; }

    std::unordered_map<int, std::unique_ptr<FftsEngine>> engines;
    for (const auto device_id : device_ids) {
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
        if (impl_->hooks.init) {
            status = impl_->hooks.init(device_id, attrs->max_ready_lanes);
            if (status != Status::Ok) { return status; }
            continue;
        }
#endif
        FftsEngineOptions engine_options;
        engine_options.device_id = device_id;
        engine_options.max_ready_lanes = attrs->max_ready_lanes;

        auto engine = std::make_unique<FftsEngine>();
        status = engine->Init(engine_options);
        if (status != Status::Ok) { return status; }

        engines.emplace(device_id, std::move(engine));
    }

    impl_->device_ids = std::move(device_ids);
    impl_->engines = std::move(engines);
    impl_->initialized = true;
    return Status::Ok;
}

Status FftsTransport::Shutdown()
{
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    Status result = Status::Ok;
    for (const auto& item : impl_->memories) {
        const auto status = impl_->UnregisterRecord(item.second);
        if (status != Status::Ok) { result = status; }
    }
    impl_->memories.clear();
    impl_->next_handle = 1;

    for (const auto device_id : impl_->device_ids) {
        const auto status = impl_->ShutdownEngine(device_id);
        if (status != Status::Ok) { result = status; }
    }
    impl_->engines.clear();
    impl_->device_ids.clear();
    impl_->initialized = false;
    return result;
}

Status FftsTransport::RegisterMemory(const MemoryRegion& memory, MemoryHandle& handle)
{
    handle = kInvalidMemoryHandle;
    if (memory.addr == nullptr || memory.length == 0) { return Status::InvalidArgument; }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->initialized) { return Status::Failed; }

    for (const auto& item : impl_->memories) {
        if (SameMemoryRegion(item.second.region, memory)) {
            handle = item.first;
            return Status::Ok;
        }
    }

    Impl::MemoryRecord record;
    record.region = memory;

    Status status = Status::InvalidArgument;
    if (memory.type == MemoryType::Host) {
        status = impl_->RegisterHostMemory(memory, record);
    } else if (memory.type == MemoryType::Device) {
        status = impl_->RegisterDeviceMemory(memory, record);
    }
    if (status != Status::Ok) {
        UC_ERROR("FFTS transport failed to register memory addr=0x{:x} length={} status={}",
                 detail::PtrToU64(memory.addr), memory.length, static_cast<int>(status));
        return status;
    }

    handle = impl_->next_handle++;
    if (handle == kInvalidMemoryHandle) { handle = impl_->next_handle++; }
    impl_->memories.emplace(handle, std::move(record));
    return Status::Ok;
}

Status FftsTransport::UnregisterMemory(MemoryHandle handle)
{
    if (handle == kInvalidMemoryHandle) { return Status::InvalidArgument; }

    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    const auto it = impl_->memories.find(handle);
    if (it == impl_->memories.end()) { return Status::Failed; }

    const auto status = impl_->UnregisterRecord(it->second);
    if (status != Status::Ok) { return status; }
    impl_->memories.erase(it);
    return Status::Ok;
}

Status FftsTransport::ExportMetadata(const ManagerID& manager_id, Metadata& out)
{
    return detail::AppendU32(out, kMetadataVersion) ? Status::Ok : Status::InvalidArgument;
}

Status FftsTransport::ImportMetadata(const ManagerID& manager_id, const Metadata& metadata)
{
    size_t offset = 0;
    uint32_t version = 0;
    if (!detail::ReadU32(metadata, offset, version)) {
        return Status::InvalidArgument;
    }
    return version == kMetadataVersion && offset == metadata.size() ? Status::Ok
                                                                    : Status::InvalidArgument;
}

Status FftsTransport::Execute(const Operation& request)
{
    const auto supported_direct = request.direct == OperationDirect::LocalDeviceHost ||
                                  request.direct == OperationDirect::LocalDeviceDevice;
    if (!supported_direct || request.ops.empty()) {
        return Status::InvalidArgument;
    }

    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->initialized) { return Status::Failed; }

    std::unordered_map<int, std::vector<FftsCopySpec>> copies_by_device;
    for (const auto& segment : request.ops) {
        if (segment.local_addr == nullptr || segment.length == 0 ||
            !FitsSizeT(segment.length)) {
            return Status::InvalidArgument;
        }

        const auto local_addr = detail::PtrToU64(segment.local_addr);
        const auto remote_addr = segment.remote_addr;
        const auto dst_addr = request.opcode == Opcode::Read ? local_addr : remote_addr;
        const auto src_addr = request.opcode == Opcode::Read ? remote_addr : local_addr;

        FftsCopySpec copy;
        int device_id = -1;
        const auto status =
            impl_->BuildCopy(request.direct, dst_addr, src_addr, segment.length, copy, device_id);
        if (status != Status::Ok) { return status; }

        copies_by_device[device_id].push_back(copy);
    }

    for (const auto device_id : impl_->device_ids) {
        const auto copy_it = copies_by_device.find(device_id);
        if (copy_it == copies_by_device.end()) { continue; }

        if (!impl_->HasEngine(device_id)) { return Status::Failed; }

        const auto status = impl_->SubmitToEngine(device_id, copy_it->second);
        if (status != Status::Ok) { return status; }
    }
    return Status::Ok;
}

TransportPtr MakeFftsTransport() { return std::make_shared<FftsTransport>(); }

}  // namespace transport
