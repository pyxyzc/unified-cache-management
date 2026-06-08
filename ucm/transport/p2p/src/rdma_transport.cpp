#include "rdma_transport.hpp"

#include "transport_internal.hpp"

#include <algorithm>
#include <array>
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

#if defined(TRANSPORT_ENABLE_RDMA)
#include <infiniband/verbs.h>
#endif

namespace transport {
namespace {

constexpr int kDefaultRdmaAccess =
#if defined(TRANSPORT_ENABLE_RDMA)
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
#else
    0;
#endif

constexpr uint8_t kDefaultRdmaSl = 0;
constexpr uint8_t kDefaultRdmaSrcPathBits = 0;
constexpr uint8_t kDefaultRdmaMinRnrTimer = 12;
constexpr uint8_t kDefaultRdmaTimeout = 14;
constexpr uint8_t kDefaultRdmaRetryCount = 7;
constexpr uint8_t kDefaultRdmaRnrRetry = 7;
constexpr uint8_t kDefaultRdmaMaxDestReadAtomic = 1;
constexpr uint8_t kDefaultRdmaMaxReadAtomic = 1;

#if defined(TRANSPORT_ENABLE_RDMA)
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
#endif

}  // namespace

RdmaTransport::RdmaTransport() : rng_(std::random_device{}()) {}

RdmaTransport::~RdmaTransport() {
    (void)shutdown();
}

const char* RdmaTransport::name() const {
    return "rdma";
}

Status RdmaTransport::init() {
    return init(RdmaInitAttrs{});
}

Status RdmaTransport::init(const RdmaInitAttrs& options) {
    local_ = options;
    return discoverLocalDevices();
}

Status RdmaTransport::shutdown() {
    for (auto& item : memories_) {
        for (auto& native_handle : item.second.native_handles) {
#if defined(TRANSPORT_ENABLE_RDMA)
            if (native_handle != nullptr) {
                (void)ibv_dereg_mr(static_cast<ibv_mr*>(native_handle));
            }
#endif
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
#if defined(TRANSPORT_ENABLE_RDMA)
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
#endif
        record.native_handles.push_back(native_handle);
        record.attrs.push_back(attrs);
    }

    const auto handle = next_memory_handle_++;
    memories_.emplace(handle, record);
    return Status::Ok;
}

Status RdmaTransport::unregisterMemory(const MemoryRegion& memory) {
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
#if defined(TRANSPORT_ENABLE_RDMA)
    for (auto& native_handle : it->second.native_handles) {
        if (native_handle != nullptr && ibv_dereg_mr(static_cast<ibv_mr*>(native_handle)) != 0) {
            return Status::Failed;
        }
        native_handle = nullptr;
    }
#endif
    memories_.erase(it);
    return Status::Ok;
}

Status RdmaTransport::exportMetadata(Metadata& out) const {
    std::map<std::string, std::string> kv;
    kv["transport"] = name();
    kv["gid"] = local_.gid;
    kv["ip"] = local_.ip;
    kv["port"] = std::to_string(local_.port);
    kv["qp_num"] = std::to_string(local_.qp_num);
    kv["psn"] = std::to_string(local_.psn);
    kv["rkey"] = std::to_string(local_.rkey);
    kv["vaddr"] = std::to_string(local_.vaddr);
    kv["recv_depth"] = std::to_string(local_.recv_depth);
    kv["recv_buffer_size"] = std::to_string(local_.recv_buffer_size);
    kv["device_count"] = std::to_string(devices_.size());
    kv["memory_count"] = std::to_string(memories_.size());

    for (size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        const auto prefix = "device." + std::to_string(device_index) + ".";
        const auto& device = devices_[device_index];
        kv[prefix + "name"] = device.name;
        kv[prefix + "port_count"] = std::to_string(device.port_count);
        kv[prefix + "qp_count"] = std::to_string(device.queue_pairs.size());
        for (size_t qp_index = 0; qp_index < device.queue_pairs.size(); ++qp_index) {
            const auto qp_prefix = prefix + "qp." + std::to_string(qp_index) + ".";
            const auto& qp = device.queue_pairs[qp_index];
            kv[qp_prefix + "port"] = std::to_string(qp.port);
            kv[qp_prefix + "lid"] = std::to_string(qp.lid);
            kv[qp_prefix + "gid"] = qp.gid;
            kv[qp_prefix + "qp_num"] = std::to_string(qp.qp_num);
            kv[qp_prefix + "psn"] = std::to_string(qp.psn);
        }
    }

    size_t index = 0;
    for (const auto& item : memories_) {
        const auto prefix = "memory." + std::to_string(index) + ".";
        kv[prefix + "handle"] = std::to_string(item.first);
        kv[prefix + "addr"] = std::to_string(detail::PtrToU64(item.second.region.addr));
        kv[prefix + "length"] = std::to_string(item.second.region.length);
        kv[prefix + "registration_count"] = std::to_string(item.second.attrs.size());
        for (size_t reg_index = 0; reg_index < item.second.attrs.size(); ++reg_index) {
            const auto reg_prefix = prefix + "registration." + std::to_string(reg_index) + ".";
            const auto& attrs = item.second.attrs[reg_index];
            kv[reg_prefix + "lkey"] = std::to_string(attrs.lkey);
            kv[reg_prefix + "rkey"] = std::to_string(attrs.rkey);
        }
        ++index;
    }
    out = detail::PackKV(kv);
    return Status::Ok;
}

Status RdmaTransport::importMetadata(PeerID peer, const Metadata& metadata) {
    const auto kv = detail::UnpackKV(metadata);
    PeerState state;

    const auto gid_it = kv.find("gid");
    const auto ip_it = kv.find("ip");
    if (gid_it != kv.end()) {
        state.attrs.gid = gid_it->second;
    }
    if (ip_it != kv.end()) {
        state.attrs.ip = ip_it->second;
    }
    state.attrs.port = static_cast<uint16_t>(detail::ToU64(kv, "port"));
    state.attrs.qp_num = static_cast<uint32_t>(detail::ToU64(kv, "qp_num"));
    state.attrs.psn = static_cast<uint32_t>(detail::ToU64(kv, "psn"));
    state.attrs.rkey = static_cast<uint32_t>(detail::ToU64(kv, "rkey"));
    state.attrs.vaddr = detail::ToU64(kv, "vaddr");

    const auto device_count = detail::ToU64(kv, "device_count");
    state.devices.reserve(static_cast<size_t>(device_count));
    for (uint64_t device_index = 0; device_index < device_count; ++device_index) {
        const auto prefix = "device." + std::to_string(device_index) + ".";
        RdmaPeerDeviceAttrs device;
        const auto name_it = kv.find(prefix + "name");
        if (name_it != kv.end()) {
            device.name = name_it->second;
        }
        device.port_count = static_cast<uint8_t>(detail::ToU64(kv, prefix + "port_count"));

        const auto qp_count = detail::ToU64(kv, prefix + "qp_count");
        device.queue_pairs.reserve(static_cast<size_t>(qp_count));
        for (uint64_t qp_index = 0; qp_index < qp_count; ++qp_index) {
            const auto qp_prefix = prefix + "qp." + std::to_string(qp_index) + ".";
            RdmaPeerQueuePair qp;
            qp.port = static_cast<uint8_t>(detail::ToU64(kv, qp_prefix + "port"));
            qp.lid = static_cast<uint16_t>(detail::ToU64(kv, qp_prefix + "lid"));
            const auto gid_it = kv.find(qp_prefix + "gid");
            if (gid_it != kv.end()) {
                qp.gid = gid_it->second;
            }
            qp.qp_num = static_cast<uint32_t>(detail::ToU64(kv, qp_prefix + "qp_num"));
            qp.psn = static_cast<uint32_t>(detail::ToU64(kv, qp_prefix + "psn"));
            device.queue_pairs.push_back(qp);
        }
        state.devices.push_back(std::move(device));
    }

    const auto memory_count = detail::ToU64(kv, "memory_count");
    for (uint64_t memory_index = 0; memory_index < memory_count; ++memory_index) {
        const auto prefix = "memory." + std::to_string(memory_index) + ".";
        const auto handle = detail::ToU64(kv, prefix + "handle");
        if (handle == kInvalidMemoryHandle) {
            continue;
        }

        PeerMemoryRecord memory;
        memory.remote_address = detail::ToU64(kv, prefix + "addr");
        memory.length = detail::ToU64(kv, prefix + "length");

        const auto registration_count = detail::ToU64(kv, prefix + "registration_count");
        memory.registrations.reserve(static_cast<size_t>(registration_count));
        for (uint64_t reg_index = 0; reg_index < registration_count; ++reg_index) {
            const auto reg_prefix = prefix + "registration." + std::to_string(reg_index) + ".";
            PeerMemoryRegistration registration;
            registration.attrs.rkey = static_cast<uint32_t>(detail::ToU64(kv, reg_prefix + "rkey"));
            registration.attrs.lkey = static_cast<uint32_t>(detail::ToU64(kv, reg_prefix + "lkey"));
            memory.registrations.push_back(registration);
        }

        state.memories.emplace(handle, std::move(memory));
    }

    peers_[peer] = std::move(state);
    return Status::Ok;
}

Status RdmaTransport::connectPeer(PeerID peer) {
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
    const auto local_it = findLocalMemory(transfer.local_address, transfer.length);
    if (local_it == memories_.end()) {
        return Status::NotFound;
    }
    if (devices_.empty() || local_it->second.attrs.empty()) {
        return Status::NotFound;
    }

    PeerMemoryLookup remote_memory;
    if (transfer.opcode == Opcode::Read || transfer.opcode == Opcode::Write) {
        remote_memory = findPeerMemory(transfer.target_id, transfer.target_address);
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
    const auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : &it->second.attrs;
}

Status RdmaTransport::discoverLocalDevices() {
    cleanupDevices();
    topology_.clear();

#if defined(TRANSPORT_ENABLE_RDMA)
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
#else
    LocalDevice device;
    device.name = "stub0";
    device.port_count = 1;
    LocalQueuePair qp;
    qp.port = 1;
    qp.lid = 1;
    qp.gid = "00000000000000000000000000000001";
    qp.qp_num = 1;
    qp.psn = local_.psn;
    device.queue_pairs.push_back(qp);
    devices_.push_back(std::move(device));
    topology_["rdma.device_count"] = "1";
    topology_["rdma.device.0.name"] = "stub0";
    topology_["rdma.device.0.port_count"] = "1";
    topology_["rdma.device.0.qp_count"] = "1";
    return Status::Ok;
#endif
}

Status RdmaTransport::initQueuePairs(LocalDevice& device, size_t device_index) {
    device.queue_pairs.clear();

#if defined(TRANSPORT_ENABLE_RDMA)
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
#if defined(TRANSPORT_ENABLE_RDMA)
            for (auto& buffer : local_qp.receive_pool) {
                if (buffer.memory_region != nullptr) {
                    (void)ibv_dereg_mr(static_cast<ibv_mr*>(buffer.memory_region));
                }
            }
#endif
            (void)ibv_destroy_qp(qp);
            (void)ibv_destroy_cq(cq);
            continue;
        }
        device.queue_pairs.push_back(local_qp);
    }

    return device.queue_pairs.empty() ? Status::NotFound : Status::Ok;
#else
    (void)device;
    (void)device_index;
    return Status::Ok;
#endif
}

Status RdmaTransport::connectQueuePair(LocalQueuePair& local_qp,
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

#if defined(TRANSPORT_ENABLE_RDMA)
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
#else
    (void)peer_qp;
#endif

    local_qp.connected_peer = peer;
    return Status::Ok;
}

Status RdmaTransport::initReceivePool(LocalDevice& device, LocalQueuePair& qp) {
    qp.receive_pool.clear();
    if (local_.recv_depth == 0 || local_.recv_buffer_size == 0) {
        return Status::Ok;
    }

#if defined(TRANSPORT_ENABLE_RDMA)
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
#else
    (void)device;
    (void)qp;
#endif
    return Status::Ok;
}

Status RdmaTransport::postReceive(LocalQueuePair& qp, LocalReceiveBuffer& buffer) {
#if defined(TRANSPORT_ENABLE_RDMA)
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
#else
    (void)qp;
    (void)buffer;
    return Status::Ok;
#endif
}

Status RdmaTransport::pollCompletion(LocalQueuePair& qp, uint64_t expected_wr_id) {
#if defined(TRANSPORT_ENABLE_RDMA)
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
#else
    (void)qp;
    (void)expected_wr_id;
    return Status::Failed;
#endif
}

Status RdmaTransport::submitTransferOnQueuePair(LocalQueuePair& qp,
                                                const LocalMemoryRecord& local_memory,
                                                const RdmaMemoryAttrs& local_attrs,
                                                const RdmaMemoryAttrs& remote_attrs,
                                                const Transfer& transfer) {
#if defined(TRANSPORT_ENABLE_RDMA)
    auto* native_qp = static_cast<ibv_qp*>(qp.queue_pair);
    if (native_qp == nullptr) {
        return Status::InvalidArgument;
    }

    ibv_sge sge{};
    sge.addr = static_cast<uintptr_t>(transfer.local_address);
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
        wr.wr.rdma.remote_addr = transfer.target_address;
        wr.wr.rdma.rkey = remote_attrs.rkey;
    } else if (transfer.opcode == Opcode::Write) {
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.wr.rdma.remote_addr = transfer.target_address;
        wr.wr.rdma.rkey = remote_attrs.rkey;
    } else {
        return Status::InvalidArgument;
    }

    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(native_qp, &wr, &bad) != 0) {
        return Status::Failed;
    }
    return pollCompletion(qp, wr_id);
#else
    (void)qp;
    (void)local_memory;
    (void)local_attrs;
    (void)remote_attrs;
    (void)transfer;
    return Status::Failed;
#endif
}

uint64_t RdmaTransport::allocateWrID() {
    auto id = next_wr_id_.fetch_add(1, std::memory_order_relaxed);
    if (id != 0) {
        return id;
    }
    return next_wr_id_.fetch_add(1, std::memory_order_relaxed);
}

Status RdmaTransport::selectTopology(const LocalMemoryRecord& local_memory,
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

Status RdmaTransport::selectQueuePair(const PeerState& peer,
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

std::unordered_map<MemoryHandle, RdmaTransport::LocalMemoryRecord>::iterator RdmaTransport::findLocalMemory(
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

RdmaTransport::PeerMemoryLookup RdmaTransport::findPeerMemory(PeerID peer, uint64_t address) const {
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

size_t RdmaTransport::randomIndex(size_t count) {
    if (count <= 1) {
        return 0;
    }
    std::uniform_int_distribution<size_t> dist(0, count - 1);
    return dist(rng_);
}

void RdmaTransport::cleanupDevices() {
    for (auto& device : devices_) {
#if defined(TRANSPORT_ENABLE_RDMA)
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
#endif
        device.protection_domain = nullptr;
        device.context = nullptr;
        device.queue_pairs.clear();
    }
    devices_.clear();
}

}  // namespace transport
