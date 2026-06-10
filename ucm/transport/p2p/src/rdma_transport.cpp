#include "rdma_transport.hpp"

#include "transport_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>

namespace transport {
namespace {

constexpr int kDefaultRdmaAccess =
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

constexpr uint8_t kDefaultRdmaSl = 0;
constexpr uint8_t kDefaultRdmaSrcPathBits = 0;
constexpr uint8_t kDefaultRdmaMinRnrTimer = 12;
constexpr uint8_t kDefaultRdmaTimeout = 14;
constexpr uint8_t kDefaultRdmaRetryCount = 7;
constexpr uint8_t kDefaultRdmaRnrRetry = 7;
constexpr uint8_t kDefaultRdmaMaxDestReadAtomic = 1;
constexpr uint8_t kDefaultRdmaMaxReadAtomic = 1;
constexpr uint32_t kRdmaMetadataMagic = 0x414d4452;  // "RDMA" in little-endian byte order.
constexpr uint32_t kRdmaMetadataVersion = 1;

bool FitsU32(size_t value) {
    return value <= UINT32_MAX;
}

bool AppendU8(Metadata& out, uint8_t value) {
    out.push_back(value);
    return true;
}

bool AppendU16(Metadata& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffU));
    out.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    return true;
}

bool AppendU32(Metadata& out, uint32_t value) {
    for (size_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
    }
    return true;
}

bool AppendU64(Metadata& out, uint64_t value) {
    for (size_t shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
    }
    return true;
}

bool AppendString(Metadata& out, const std::string& value) {
    if (!FitsU32(value.size())) {
        return false;
    }
    AppendU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return true;
}

bool ReadU8(const Metadata& input, size_t& offset, uint8_t& value) {
    if (offset >= input.size()) {
        return false;
    }
    value = input[offset++];
    return true;
}

bool ReadU16(const Metadata& input, size_t& offset, uint16_t& value) {
    if (input.size() - offset < 2) {
        return false;
    }
    value = static_cast<uint16_t>(input[offset]) |
            static_cast<uint16_t>(static_cast<uint16_t>(input[offset + 1]) << 8U);
    offset += 2;
    return true;
}

bool ReadU32(const Metadata& input, size_t& offset, uint32_t& value) {
    if (input.size() - offset < 4) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(input[offset + i]) << (i * 8);
    }
    offset += 4;
    return true;
}

bool ReadU64(const Metadata& input, size_t& offset, uint64_t& value) {
    if (input.size() - offset < 8) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(input[offset + i]) << (i * 8);
    }
    offset += 8;
    return true;
}

bool ReadString(const Metadata& input, size_t& offset, std::string& value) {
    uint32_t length = 0;
    if (!ReadU32(input, offset, length) || input.size() - offset < length) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(input.data() + offset), length);
    offset += length;
    return true;
}

std::string GidToString(const ibv_gid& gid) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint8_t byte : gid.raw) {
        os << std::setw(2) << static_cast<unsigned>(byte);
    }
    return os.str();
}

bool ParseGid(const std::string& text, ibv_gid& gid) {
    std::string compact;
    compact.reserve(text.size());
    for (char ch : text) {
        if (ch == ':' || ch == '-') {
            continue;
        }
        compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (compact.size() != 32) {
        return false;
    }
    for (size_t i = 0; i < compact.size(); i += 2) {
        const auto byte = compact.substr(i, 2);
        char* end = nullptr;
        const auto value = std::strtoul(byte.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || value > 0xffUL) {
            return false;
        }
        gid.raw[i / 2] = static_cast<uint8_t>(value);
    }
    return true;
}

}  // namespace

struct RdmaTransport::Impl {
    struct LocalReceiveBuffer {
        std::vector<uint8_t> buffer;
        void* memory_region = nullptr;
        uint32_t lkey = 0;
    };

    struct LocalQueuePair {
        uint8_t port = 0;
        uint16_t lid = 0;
        std::string gid;
        uint32_t qp_num = 0;
        uint32_t psn = 0;
        PeerID connected_peer = kInvalidPeerID;
        void* completion_queue = nullptr;
        void* queue_pair = nullptr;
        std::vector<LocalReceiveBuffer> receive_pool;
    };

    struct LocalDevice {
        std::string name;
        uint8_t port_count = 0;
        void* context = nullptr;
        void* protection_domain = nullptr;
        std::vector<LocalQueuePair> queue_pairs;
    };

    struct LocalMemoryRecord {
        MemoryRegion region;
        std::vector<void*> native_handles;
        std::vector<RdmaMemoryAttrs> attrs;
    };

    struct PeerMemoryRegistration {
        RdmaMemoryAttrs attrs;
    };

    struct PeerMemoryRecord {
        uint64_t remote_address = 0;
        uint64_t length = 0;
        std::vector<PeerMemoryRegistration> registrations;
    };

    struct PeerState {
        RdmaPeerAttrs attrs;
        std::vector<RdmaPeerDeviceAttrs> devices;
        std::unordered_map<MemoryHandle, PeerMemoryRecord> memories;
        bool connected = false;
    };

    struct PeerMemoryLookup {
        MemoryHandle handle = kInvalidMemoryHandle;
        const PeerMemoryRecord* record = nullptr;
    };

    struct TransferPlan {
        size_t device_index = 0;
        size_t qp_index = 0;
        MemoryHandle remote_memory = kInvalidMemoryHandle;
    };

    Status init(const RdmaInitAttrs& options);
    Status shutdown();
    Status registerMemory(const MemoryRegion& memory);
    Status unregisterMemory(const MemoryRegion& memory);
    Status exportMetadata(Metadata& out) const;
    Status importMetadata(PeerID peer, const Metadata& metadata);
    Status connectPeer(PeerID peer);
    Status submitTransfer(const Transfer& transfer);
    const RdmaPeerAttrs* peer(PeerID id) const;

    Status discoverLocalDevices();
    Status initQueuePairs(LocalDevice& device, size_t device_index);
    Status connectQueuePair(LocalQueuePair& local_qp, const RdmaPeerQueuePair& peer_qp, PeerID peer);
    Status initReceivePool(LocalDevice& device, LocalQueuePair& qp);
    Status postReceive(LocalQueuePair& qp, LocalReceiveBuffer& buffer);
    Status pollCompletion(LocalQueuePair& qp, uint64_t expected_wr_id);
    Status submitTransferOnQueuePair(LocalQueuePair& qp,
                                     const LocalMemoryRecord& local_memory,
                                     const RdmaMemoryAttrs& local_attrs,
                                     const RdmaMemoryAttrs& remote_attrs,
                                     const Transfer& transfer);
    Status selectTopology(const LocalMemoryRecord& local_memory,
                          const PeerState& peer,
                          const PeerMemoryLookup* remote_memory,
                          const Transfer& transfer,
                          TransferPlan& plan);
    Status selectQueuePair(const PeerState& peer, PeerID peer_id, size_t device_index, size_t& qp_index);
    std::unordered_map<MemoryHandle, LocalMemoryRecord>::iterator findLocalMemory(uint64_t address, uint64_t length);
    PeerMemoryLookup findPeerMemory(PeerID peer, uint64_t address) const;
    size_t randomIndex(size_t count);
    uint64_t allocateWrID();
    void cleanupDevices();

    RdmaInitAttrs local_;
    std::map<std::string, std::string> topology_;
    std::vector<LocalDevice> devices_;
    MemoryHandle next_memory_handle_ = 1;
    std::unordered_map<MemoryHandle, LocalMemoryRecord> memories_;
    std::unordered_map<PeerID, PeerState> peers_;
    std::mt19937 rng_{std::random_device{}()};
    std::atomic<uint64_t> next_wr_id_{1};
};

RdmaTransport::RdmaTransport() : impl_(std::make_unique<Impl>()) {}

RdmaTransport::~RdmaTransport() {
    (void)shutdown();
}

const char* RdmaTransport::name() const {
    return "rdma";
}

Status RdmaTransport::init(const InitAttrs& options) {
    const auto* attrs = dynamic_cast<const RdmaInitAttrs*>(&options);
    return attrs == nullptr ? Status::InvalidArgument : init(*attrs);
}

Status RdmaTransport::init(const RdmaInitAttrs& options) {
    return impl_->init(options);
}

Status RdmaTransport::Impl::init(const RdmaInitAttrs& options) {
    local_ = options;
    return discoverLocalDevices();
}

Status RdmaTransport::shutdown() {
    return impl_->shutdown();
}

Status RdmaTransport::Impl::shutdown() {
    for (auto& item : memories_) {
        for (auto& native_handle : item.second.native_handles) {
            if (native_handle != nullptr) {
                (void)ibv_dereg_mr(static_cast<ibv_mr*>(native_handle));
            }
            native_handle = nullptr;
        }
    }
    memories_.clear();
    next_memory_handle_ = 1;
    cleanupDevices();
    peers_.clear();
    local_ = RdmaInitAttrs{};
    topology_.clear();
    return Status::Ok;
}

Status RdmaTransport::registerMemory(const MemoryRegion& memory) {
    return impl_->registerMemory(memory);
}

Status RdmaTransport::Impl::registerMemory(const MemoryRegion& memory) {
    if (memory.addr == nullptr || memory.length == 0) {
        return Status::InvalidArgument;
    }

    if (devices_.empty()) {
        return Status::NotFound;
    }
    if (findLocalMemory(detail::PtrToU64(memory.addr), memory.length) != memories_.end()) {
        return Status::AlreadyExists;
    }

    LocalMemoryRecord record;
    record.region = memory;
    record.native_handles.reserve(devices_.size());
    record.attrs.reserve(devices_.size());

    for (size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        auto& device = devices_[device_index];

        RdmaMemoryAttrs attrs;
        void* native_handle = nullptr;
        auto* pd = static_cast<ibv_pd*>(device.protection_domain);
        auto* mr = ibv_reg_mr(pd, memory.addr, static_cast<size_t>(memory.length), kDefaultRdmaAccess);
        if (mr == nullptr) {
            for (auto& registered : record.native_handles) {
                if (registered != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(registered));
                }
            }
            return Status::Failed;
        }
        native_handle = mr;
        attrs.lkey = mr->lkey;
        attrs.rkey = mr->rkey;
        record.native_handles.push_back(native_handle);
        record.attrs.push_back(attrs);
    }

    const auto handle = next_memory_handle_++;
    memories_.emplace(handle, record);
    return Status::Ok;
}

Status RdmaTransport::unregisterMemory(const MemoryRegion& memory) {
    return impl_->unregisterMemory(memory);
}

Status RdmaTransport::Impl::unregisterMemory(const MemoryRegion& memory) {
    auto it = memories_.end();
    const auto address = detail::PtrToU64(memory.addr);
    for (auto candidate = memories_.begin(); candidate != memories_.end(); ++candidate) {
        const auto candidate_address = detail::PtrToU64(candidate->second.region.addr);
        if (candidate_address == address) {
            it = candidate;
            break;
        }
    }
    if (it == memories_.end()) {
        return Status::NotFound;
    }
    for (auto& native_handle : it->second.native_handles) {
        if (native_handle != nullptr && ibv_dereg_mr(static_cast<ibv_mr*>(native_handle)) != 0) {
            return Status::Failed;
        }
        native_handle = nullptr;
    }
    memories_.erase(it);
    return Status::Ok;
}

Status RdmaTransport::exportMetadata(Metadata& out) const {
    return impl_->exportMetadata(out);
}

Status RdmaTransport::Impl::exportMetadata(Metadata& out) const {
    if (!FitsU32(devices_.size()) || !FitsU32(memories_.size())) {
        return Status::InvalidArgument;
    }

    Metadata encoded;
    AppendU32(encoded, kRdmaMetadataMagic);
    AppendU32(encoded, kRdmaMetadataVersion);
    if (!AppendString(encoded, local_.gid) || !AppendString(encoded, local_.ip)) {
        return Status::InvalidArgument;
    }
    AppendU16(encoded, local_.port);
    AppendU32(encoded, local_.qp_num);
    AppendU32(encoded, local_.psn);
    AppendU32(encoded, local_.rkey);
    AppendU64(encoded, local_.vaddr);
    AppendU32(encoded, local_.recv_depth);
    AppendU32(encoded, local_.recv_buffer_size);
    AppendU32(encoded, static_cast<uint32_t>(devices_.size()));

    for (size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        const auto& device = devices_[device_index];
        if (!FitsU32(device.queue_pairs.size()) || !AppendString(encoded, device.name)) {
            return Status::InvalidArgument;
        }
        AppendU8(encoded, device.port_count);
        AppendU32(encoded, static_cast<uint32_t>(device.queue_pairs.size()));
        for (size_t qp_index = 0; qp_index < device.queue_pairs.size(); ++qp_index) {
            const auto& qp = device.queue_pairs[qp_index];
            AppendU8(encoded, qp.port);
            AppendU16(encoded, qp.lid);
            if (!AppendString(encoded, qp.gid)) {
                return Status::InvalidArgument;
            }
            AppendU32(encoded, qp.qp_num);
            AppendU32(encoded, qp.psn);
        }
    }

    AppendU32(encoded, static_cast<uint32_t>(memories_.size()));
    for (const auto& item : memories_) {
        if (!FitsU32(item.second.attrs.size())) {
            return Status::InvalidArgument;
        }
        AppendU64(encoded, item.first);
        AppendU64(encoded, detail::PtrToU64(item.second.region.addr));
        AppendU64(encoded, item.second.region.length);
        AppendU32(encoded, static_cast<uint32_t>(item.second.attrs.size()));
        for (size_t reg_index = 0; reg_index < item.second.attrs.size(); ++reg_index) {
            const auto& attrs = item.second.attrs[reg_index];
            AppendU32(encoded, attrs.lkey);
            AppendU32(encoded, attrs.rkey);
        }
    }
    out = std::move(encoded);
    return Status::Ok;
}

Status RdmaTransport::importMetadata(PeerID peer, const Metadata& metadata) {
    return impl_->importMetadata(peer, metadata);
}

Status RdmaTransport::Impl::importMetadata(PeerID peer, const Metadata& metadata) {
    PeerState state;
    size_t offset = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    if (!ReadU32(metadata, offset, magic) || !ReadU32(metadata, offset, version) ||
        magic != kRdmaMetadataMagic || version != kRdmaMetadataVersion) {
        return Status::InvalidArgument;
    }

    uint32_t recv_depth = 0;
    uint32_t recv_buffer_size = 0;
    uint32_t device_count = 0;
    if (!ReadString(metadata, offset, state.attrs.gid) ||
        !ReadString(metadata, offset, state.attrs.ip) ||
        !ReadU16(metadata, offset, state.attrs.port) ||
        !ReadU32(metadata, offset, state.attrs.qp_num) ||
        !ReadU32(metadata, offset, state.attrs.psn) ||
        !ReadU32(metadata, offset, state.attrs.rkey) ||
        !ReadU64(metadata, offset, state.attrs.vaddr) ||
        !ReadU32(metadata, offset, recv_depth) ||
        !ReadU32(metadata, offset, recv_buffer_size) ||
        !ReadU32(metadata, offset, device_count)) {
        return Status::InvalidArgument;
    }

    state.devices.reserve(device_count);
    for (uint32_t device_index = 0; device_index < device_count; ++device_index) {
        RdmaPeerDeviceAttrs device;
        uint32_t qp_count = 0;
        if (!ReadString(metadata, offset, device.name) ||
            !ReadU8(metadata, offset, device.port_count) ||
            !ReadU32(metadata, offset, qp_count)) {
            return Status::InvalidArgument;
        }

        device.queue_pairs.reserve(qp_count);
        for (uint32_t qp_index = 0; qp_index < qp_count; ++qp_index) {
            RdmaPeerQueuePair qp;
            if (!ReadU8(metadata, offset, qp.port) ||
                !ReadU16(metadata, offset, qp.lid) ||
                !ReadString(metadata, offset, qp.gid) ||
                !ReadU32(metadata, offset, qp.qp_num) ||
                !ReadU32(metadata, offset, qp.psn)) {
                return Status::InvalidArgument;
            }
            device.queue_pairs.push_back(qp);
        }
        state.devices.push_back(std::move(device));
    }

    uint32_t memory_count = 0;
    if (!ReadU32(metadata, offset, memory_count)) {
        return Status::InvalidArgument;
    }
    for (uint32_t memory_index = 0; memory_index < memory_count; ++memory_index) {
        uint64_t handle = kInvalidMemoryHandle;
        if (!ReadU64(metadata, offset, handle)) {
            return Status::InvalidArgument;
        }
        if (handle == kInvalidMemoryHandle) {
            return Status::InvalidArgument;
        }

        PeerMemoryRecord memory;
        uint32_t registration_count = 0;
        if (!ReadU64(metadata, offset, memory.remote_address) ||
            !ReadU64(metadata, offset, memory.length) ||
            !ReadU32(metadata, offset, registration_count)) {
            return Status::InvalidArgument;
        }
        if (memory.length == 0 || registration_count != device_count) {
            return Status::InvalidArgument;
        }

        memory.registrations.reserve(registration_count);
        for (uint32_t reg_index = 0; reg_index < registration_count; ++reg_index) {
            PeerMemoryRegistration registration;
            if (!ReadU32(metadata, offset, registration.attrs.lkey) ||
                !ReadU32(metadata, offset, registration.attrs.rkey)) {
                return Status::InvalidArgument;
            }
            memory.registrations.push_back(registration);
        }

        if (!state.memories.emplace(handle, std::move(memory)).second) {
            return Status::InvalidArgument;
        }
    }

    if (offset != metadata.size()) {
        return Status::InvalidArgument;
    }
    peers_[peer] = std::move(state);
    return Status::Ok;
}

Status RdmaTransport::connectPeer(PeerID peer) {
    return impl_->connectPeer(peer);
}

Status RdmaTransport::Impl::connectPeer(PeerID peer) {
    const auto peer_it = peers_.find(peer);
    if (peer_it == peers_.end()) {
        return Status::NotFound;
    }
    auto& peer_state = peer_it->second;
    if (peer_state.connected) {
        return Status::Ok;
    }
    if (devices_.empty() || peer_state.devices.empty()) {
        return Status::NotFound;
    }

    for (size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        if (device_index >= peer_state.devices.size()) {
            return Status::NotFound;
        }
        auto& local_device = devices_[device_index];
        const auto& peer_device = peer_state.devices[device_index];
        if (local_device.queue_pairs.empty() || peer_device.queue_pairs.empty()) {
            return Status::NotFound;
        }

        const auto qp_count = std::min(local_device.queue_pairs.size(), peer_device.queue_pairs.size());
        for (size_t qp_index = 0; qp_index < qp_count; ++qp_index) {
            auto& local_qp = local_device.queue_pairs[qp_index];
            if (local_qp.connected_peer == peer) {
                continue;
            }
            if (local_qp.connected_peer != kInvalidPeerID) {
                return Status::AlreadyExists;
            }
            const auto status = connectQueuePair(local_qp, peer_device.queue_pairs[qp_index], peer);
            if (status != Status::Ok) {
                return status;
            }
        }
    }

    peer_state.connected = true;
    return Status::Ok;
}

Status RdmaTransport::submitTransfer(const Transfer& transfer) {
    return impl_->submitTransfer(transfer);
}

Status RdmaTransport::Impl::submitTransfer(const Transfer& transfer) {
    if (transfer.opcode != Opcode::Send &&
        transfer.opcode != Opcode::Read &&
        transfer.opcode != Opcode::Write) {
        return Status::InvalidArgument;
    }
    if (transfer.length == 0 || transfer.length > UINT32_MAX) {
        return Status::InvalidArgument;
    }
    const auto peer_it = peers_.find(transfer.target_id);
    if (peer_it == peers_.end()) {
        return Status::NotFound;
    }
    if (!peer_it->second.connected) {
        return Status::InvalidArgument;
    }
    const auto local_address = detail::PtrToU64(transfer.local_addr);
    const auto local_it = findLocalMemory(local_address, transfer.length);
    if (local_it == memories_.end()) {
        return Status::NotFound;
    }
    if (devices_.empty() || local_it->second.attrs.empty()) {
        return Status::NotFound;
    }

    PeerMemoryLookup remote_memory;
    if (transfer.opcode == Opcode::Read || transfer.opcode == Opcode::Write) {
        remote_memory = findPeerMemory(transfer.target_id, transfer.remote_addr);
        if (remote_memory.record == nullptr) {
            return Status::NotFound;
        }
    }

    TransferPlan plan;
    auto status = selectTopology(local_it->second,
                                 peer_it->second,
                                 remote_memory.record == nullptr ? nullptr : &remote_memory,
                                 transfer,
                                 plan);
    if (status != Status::Ok) {
        return status;
    }

    RdmaMemoryAttrs remote_attrs;
    if (remote_memory.record != nullptr) {
        remote_attrs = remote_memory.record->registrations[plan.device_index].attrs;
    }

    return submitTransferOnQueuePair(devices_[plan.device_index].queue_pairs[plan.qp_index],
                                     local_it->second,
                                     local_it->second.attrs[plan.device_index],
                                     remote_attrs,
                                     transfer);
}

const RdmaPeerAttrs* RdmaTransport::peer(PeerID id) const {
    return impl_->peer(id);
}

const RdmaPeerAttrs* RdmaTransport::Impl::peer(PeerID id) const {
    const auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : &it->second.attrs;
}

Status RdmaTransport::Impl::discoverLocalDevices() {
    cleanupDevices();
    topology_.clear();

    int count = 0;
    ibv_device** device_list = ibv_get_device_list(&count);
    if (device_list == nullptr) {
        return Status::Failed;
    }

    for (int i = 0; i < count; ++i) {
        ibv_device* native_device = device_list[i];
        if (native_device == nullptr) {
            continue;
        }

        ibv_context* context = ibv_open_device(native_device);
        if (context == nullptr) {
            continue;
        }

        ibv_device_attr device_attr{};
        if (ibv_query_device(context, &device_attr) != 0) {
            (void)ibv_close_device(context);
            continue;
        }

        ibv_pd* pd = ibv_alloc_pd(context);
        if (pd == nullptr) {
            (void)ibv_close_device(context);
            continue;
        }

        LocalDevice device;
        device.name = ibv_get_device_name(native_device);
        device.port_count = device_attr.phys_port_cnt;
        device.context = context;
        device.protection_domain = pd;
        const auto device_index = devices_.size();
        const auto qp_status = initQueuePairs(device, device_index);
        if (qp_status != Status::Ok) {
            (void)ibv_dealloc_pd(pd);
            (void)ibv_close_device(context);
            continue;
        }
        devices_.push_back(device);

        const auto prefix = "rdma.device." + std::to_string(device_index) + ".";
        topology_[prefix + "name"] = device.name;
        topology_[prefix + "port_count"] = std::to_string(device.port_count);
        topology_[prefix + "qp_count"] = std::to_string(device.queue_pairs.size());
        for (size_t qp_index = 0; qp_index < device.queue_pairs.size(); ++qp_index) {
            const auto qp_prefix = prefix + "qp." + std::to_string(qp_index) + ".";
            topology_[qp_prefix + "port"] = std::to_string(device.queue_pairs[qp_index].port);
            topology_[qp_prefix + "lid"] = std::to_string(device.queue_pairs[qp_index].lid);
            topology_[qp_prefix + "gid"] = device.queue_pairs[qp_index].gid;
            topology_[qp_prefix + "qp_num"] = std::to_string(device.queue_pairs[qp_index].qp_num);
            topology_[qp_prefix + "psn"] = std::to_string(device.queue_pairs[qp_index].psn);
        }
    }

    ibv_free_device_list(device_list);
    topology_["rdma.device_count"] = std::to_string(devices_.size());
    return devices_.empty() ? Status::NotFound : Status::Ok;
}

Status RdmaTransport::Impl::initQueuePairs(LocalDevice& device, size_t device_index) {
    device.queue_pairs.clear();

    auto* context = static_cast<ibv_context*>(device.context);
    auto* pd = static_cast<ibv_pd*>(device.protection_domain);
    if (context == nullptr || pd == nullptr || device.port_count == 0) {
        return Status::InvalidArgument;
    }

    for (uint8_t port = 1; port <= device.port_count; ++port) {
        ibv_cq* cq = ibv_create_cq(context, 128, nullptr, nullptr, 0);
        if (cq == nullptr) {
            continue;
        }

        ibv_qp_init_attr qp_init{};
        qp_init.send_cq = cq;
        qp_init.recv_cq = cq;
        qp_init.qp_type = IBV_QPT_RC;
        qp_init.cap.max_send_wr = 128;
        qp_init.cap.max_recv_wr = 128;
        qp_init.cap.max_send_sge = 1;
        qp_init.cap.max_recv_sge = 1;

        ibv_qp* qp = ibv_create_qp(pd, &qp_init);
        if (qp == nullptr) {
            (void)ibv_destroy_cq(cq);
            continue;
        }

        ibv_qp_attr qp_attr{};
        qp_attr.qp_state = IBV_QPS_INIT;
        qp_attr.pkey_index = 0;
        qp_attr.port_num = port;
        qp_attr.qp_access_flags = kDefaultRdmaAccess;
        const int qp_attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
        if (ibv_modify_qp(qp, &qp_attr, qp_attr_mask) != 0) {
            (void)ibv_destroy_qp(qp);
            (void)ibv_destroy_cq(cq);
            continue;
        }

        LocalQueuePair local_qp;
        local_qp.port = port;
        local_qp.qp_num = qp->qp_num;
        local_qp.psn = local_.psn + static_cast<uint32_t>(device_index + port - 1);
        local_qp.completion_queue = cq;
        local_qp.queue_pair = qp;

        ibv_port_attr port_attr{};
        if (ibv_query_port(context, port, &port_attr) == 0) {
            local_qp.lid = port_attr.lid;
        }
        ibv_gid gid{};
        if (ibv_query_gid(context, port, local_.gid_index, &gid) == 0) {
            local_qp.gid = GidToString(gid);
        }

        const auto recv_status = initReceivePool(device, local_qp);
        if (recv_status != Status::Ok) {
            for (auto& buffer : local_qp.receive_pool) {
                if (buffer.memory_region != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(buffer.memory_region));
                }
            }
            (void)ibv_destroy_qp(qp);
            (void)ibv_destroy_cq(cq);
            continue;
        }
        device.queue_pairs.push_back(local_qp);
    }

    return device.queue_pairs.empty() ? Status::NotFound : Status::Ok;
}

Status RdmaTransport::Impl::connectQueuePair(LocalQueuePair& local_qp,
                                       const RdmaPeerQueuePair& peer_qp,
                                       PeerID peer) {
    if (local_qp.connected_peer == peer) {
        return Status::Ok;
    }
    if (local_qp.connected_peer != kInvalidPeerID) {
        return Status::AlreadyExists;
    }
    if (peer_qp.qp_num == 0) {
        return Status::InvalidArgument;
    }

    auto* native_qp = static_cast<ibv_qp*>(local_qp.queue_pair);
    if (native_qp == nullptr) {
        return Status::InvalidArgument;
    }

    ibv_qp_attr rtr{};
    rtr.qp_state = IBV_QPS_RTR;
    rtr.path_mtu = IBV_MTU_1024;
    rtr.dest_qp_num = peer_qp.qp_num;
    rtr.rq_psn = peer_qp.psn;
    rtr.max_dest_rd_atomic = kDefaultRdmaMaxDestReadAtomic;
    rtr.min_rnr_timer = kDefaultRdmaMinRnrTimer;
    rtr.ah_attr.dlid = peer_qp.lid;
    rtr.ah_attr.sl = kDefaultRdmaSl;
    rtr.ah_attr.src_path_bits = kDefaultRdmaSrcPathBits;
    rtr.ah_attr.port_num = local_qp.port;

    if (!peer_qp.gid.empty()) {
        ibv_gid remote_gid{};
        if (!ParseGid(peer_qp.gid, remote_gid)) {
            return Status::InvalidArgument;
        }
        rtr.ah_attr.is_global = 1;
        rtr.ah_attr.grh.dgid = remote_gid;
        rtr.ah_attr.grh.sgid_index = local_.gid_index;
        rtr.ah_attr.grh.hop_limit = 1;
    }

    const int rtr_mask = IBV_QP_STATE |
                         IBV_QP_AV |
                         IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN |
                         IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC |
                         IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(native_qp, &rtr, rtr_mask) != 0) {
        return Status::Failed;
    }

    ibv_qp_attr rts{};
    rts.qp_state = IBV_QPS_RTS;
    rts.timeout = kDefaultRdmaTimeout;
    rts.retry_cnt = kDefaultRdmaRetryCount;
    rts.rnr_retry = kDefaultRdmaRnrRetry;
    rts.sq_psn = local_qp.psn;
    rts.max_rd_atomic = kDefaultRdmaMaxReadAtomic;

    const int rts_mask = IBV_QP_STATE |
                         IBV_QP_TIMEOUT |
                         IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY |
                         IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(native_qp, &rts, rts_mask) != 0) {
        return Status::Failed;
    }

    local_qp.connected_peer = peer;
    return Status::Ok;
}

Status RdmaTransport::Impl::initReceivePool(LocalDevice& device, LocalQueuePair& qp) {
    qp.receive_pool.clear();
    if (local_.recv_depth == 0 || local_.recv_buffer_size == 0) {
        return Status::Ok;
    }

    auto* pd = static_cast<ibv_pd*>(device.protection_domain);
    if (pd == nullptr) {
        return Status::InvalidArgument;
    }

    qp.receive_pool.reserve(local_.recv_depth);
    for (uint32_t i = 0; i < local_.recv_depth; ++i) {
        LocalReceiveBuffer buffer;
        buffer.buffer.resize(local_.recv_buffer_size);
        auto* mr = ibv_reg_mr(pd,
                              buffer.buffer.data(),
                              buffer.buffer.size(),
                              IBV_ACCESS_LOCAL_WRITE);
        if (mr == nullptr) {
            for (auto& registered : qp.receive_pool) {
                if (registered.memory_region != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(registered.memory_region));
                }
                registered.memory_region = nullptr;
            }
            return Status::Failed;
        }
        buffer.memory_region = mr;
        buffer.lkey = mr->lkey;
        qp.receive_pool.push_back(std::move(buffer));
    }

    for (auto& buffer : qp.receive_pool) {
        const auto status = postReceive(qp, buffer);
        if (status != Status::Ok) {
            for (auto& registered : qp.receive_pool) {
                if (registered.memory_region != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(registered.memory_region));
                }
                registered.memory_region = nullptr;
            }
            return status;
        }
    }
    return Status::Ok;
}

Status RdmaTransport::Impl::postReceive(LocalQueuePair& qp, LocalReceiveBuffer& buffer) {
    auto* native_qp = static_cast<ibv_qp*>(qp.queue_pair);
    if (native_qp == nullptr || buffer.buffer.empty()) {
        return Status::InvalidArgument;
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uintptr_t>(buffer.buffer.data());
    sge.length = static_cast<uint32_t>(buffer.buffer.size());
    sge.lkey = buffer.lkey;

    ibv_recv_wr wr{};
    wr.wr_id = reinterpret_cast<uintptr_t>(&buffer);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(native_qp, &wr, &bad) == 0 ? Status::Ok : Status::Failed;
}

Status RdmaTransport::Impl::pollCompletion(LocalQueuePair& qp, uint64_t expected_wr_id) {
    auto* cq = static_cast<ibv_cq*>(qp.completion_queue);
    if (cq == nullptr) {
        return Status::InvalidArgument;
    }

    ibv_wc wc{};
    for (;;) {
        const int polled = ibv_poll_cq(cq, 1, &wc);
        if (polled < 0) {
            return Status::Failed;
        }
        if (polled == 0) {
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            return Status::Failed;
        }
        if (wc.opcode == IBV_WC_RECV) {
            auto* buffer = reinterpret_cast<LocalReceiveBuffer*>(wc.wr_id);
            if (buffer != nullptr) {
                Status callback_status = Status::Ok;
                if (local_.receive_callback) {
                    RdmaReceiveMessage message;
                    message.peer = qp.connected_peer;
                    message.data = buffer->buffer.data();
                    message.length = wc.byte_len;
                    callback_status = local_.receive_callback(message);
                }
                const auto repost_status = postReceive(qp, *buffer);
                if (repost_status != Status::Ok) {
                    return repost_status;
                }
                if (callback_status != Status::Ok) {
                    return callback_status;
                }
            }
            continue;
        }
        if (wc.wr_id == expected_wr_id) {
            return Status::Ok;
        }
    }
}

Status RdmaTransport::Impl::submitTransferOnQueuePair(LocalQueuePair& qp,
                                                const LocalMemoryRecord& local_memory,
                                                const RdmaMemoryAttrs& local_attrs,
                                                const RdmaMemoryAttrs& remote_attrs,
                                                const Transfer& transfer) {
    auto* native_qp = static_cast<ibv_qp*>(qp.queue_pair);
    if (native_qp == nullptr) {
        return Status::InvalidArgument;
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uintptr_t>(transfer.local_addr);
    sge.length = static_cast<uint32_t>(transfer.length);
    sge.lkey = local_attrs.lkey;

    const auto wr_id = allocateWrID();
    ibv_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    if (transfer.opcode == Opcode::Send) {
        wr.opcode = IBV_WR_SEND;
    } else if (transfer.opcode == Opcode::Read) {
        wr.opcode = IBV_WR_RDMA_READ;
        wr.wr.rdma.remote_addr = transfer.remote_addr;
        wr.wr.rdma.rkey = remote_attrs.rkey;
    } else if (transfer.opcode == Opcode::Write) {
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.wr.rdma.remote_addr = transfer.remote_addr;
        wr.wr.rdma.rkey = remote_attrs.rkey;
    } else {
        return Status::InvalidArgument;
    }

    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(native_qp, &wr, &bad) != 0) {
        return Status::Failed;
    }
    return pollCompletion(qp, wr_id);
}

uint64_t RdmaTransport::Impl::allocateWrID() {
    auto id = next_wr_id_.fetch_add(1, std::memory_order_relaxed);
    if (id != 0) {
        return id;
    }
    return next_wr_id_.fetch_add(1, std::memory_order_relaxed);
}

Status RdmaTransport::Impl::selectTopology(const LocalMemoryRecord& local_memory,
                                     const PeerState& peer,
                                     const PeerMemoryLookup* remote_memory,
                                     const Transfer& transfer,
                                     TransferPlan& plan) {
    std::vector<size_t> candidates;
    const bool needs_remote_memory = transfer.opcode == Opcode::Read || transfer.opcode == Opcode::Write;

    for (size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        if (device_index >= local_memory.attrs.size()) {
            continue;
        }
        if (device_index >= peer.devices.size()) {
            continue;
        }
        if (devices_[device_index].queue_pairs.empty() || peer.devices[device_index].queue_pairs.empty()) {
            continue;
        }
        if (needs_remote_memory &&
            (remote_memory == nullptr ||
             remote_memory->record == nullptr ||
             device_index >= remote_memory->record->registrations.size())) {
            continue;
        }
        candidates.push_back(device_index);
    }

    if (candidates.empty()) {
        return Status::NotFound;
    }

    plan.device_index = candidates[randomIndex(candidates.size())];
    plan.remote_memory = remote_memory == nullptr ? kInvalidMemoryHandle : remote_memory->handle;
    return selectQueuePair(peer, transfer.target_id, plan.device_index, plan.qp_index);
}

Status RdmaTransport::Impl::selectQueuePair(const PeerState& peer,
                                      PeerID peer_id,
                                      size_t device_index,
                                      size_t& qp_index) {
    if (device_index >= devices_.size() || device_index >= peer.devices.size()) {
        return Status::InvalidArgument;
    }

    auto& local_device = devices_[device_index];
    const auto& peer_device = peer.devices[device_index];
    const auto qp_count = std::min(local_device.queue_pairs.size(), peer_device.queue_pairs.size());

    std::vector<size_t> candidates;
    for (size_t i = 0; i < qp_count; ++i) {
        if (local_device.queue_pairs[i].connected_peer == peer_id) {
            candidates.push_back(i);
        }
    }

    if (candidates.empty()) {
        return Status::NotFound;
    }

    qp_index = candidates[randomIndex(candidates.size())];
    return Status::Ok;
}

std::unordered_map<MemoryHandle, RdmaTransport::Impl::LocalMemoryRecord>::iterator RdmaTransport::Impl::findLocalMemory(
    uint64_t address,
    uint64_t length) {
    for (auto it = memories_.begin(); it != memories_.end(); ++it) {
        const auto begin = detail::PtrToU64(it->second.region.addr);
        if (address < begin) {
            continue;
        }
        const auto offset = address - begin;
        if (offset <= it->second.region.length && length <= it->second.region.length - offset) {
            return it;
        }
    }
    return memories_.end();
}

RdmaTransport::Impl::PeerMemoryLookup RdmaTransport::Impl::findPeerMemory(PeerID peer, uint64_t address) const {
    const auto peer_it = peers_.find(peer);
    if (peer_it == peers_.end()) {
        return {};
    }
    for (const auto& item : peer_it->second.memories) {
        const auto& memory = item.second;
        if (address >= memory.remote_address && address < memory.remote_address + memory.length) {
            return {item.first, &memory};
        }
    }
    return {};
}

size_t RdmaTransport::Impl::randomIndex(size_t count) {
    if (count <= 1) {
        return 0;
    }
    std::uniform_int_distribution<size_t> dist(0, count - 1);
    return dist(rng_);
}

void RdmaTransport::Impl::cleanupDevices() {
    for (auto& device : devices_) {
        for (auto& qp : device.queue_pairs) {
            for (auto& buffer : qp.receive_pool) {
                if (buffer.memory_region != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(buffer.memory_region));
                }
                buffer.memory_region = nullptr;
            }
            if (qp.queue_pair != nullptr) {
                (void)ibv_destroy_qp(static_cast<ibv_qp*>(qp.queue_pair));
            }
            if (qp.completion_queue != nullptr) {
                (void)ibv_destroy_cq(static_cast<ibv_cq*>(qp.completion_queue));
            }
            qp.queue_pair = nullptr;
            qp.completion_queue = nullptr;
            qp.receive_pool.clear();
        }
        if (device.protection_domain != nullptr) {
            (void)ibv_dealloc_pd(static_cast<ibv_pd*>(device.protection_domain));
        }
        if (device.context != nullptr) {
            (void)ibv_close_device(static_cast<ibv_context*>(device.context));
        }
        device.protection_domain = nullptr;
        device.context = nullptr;
        device.queue_pairs.clear();
    }
    devices_.clear();
}

}  // namespace transport
