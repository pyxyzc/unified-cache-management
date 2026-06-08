#pragma once

#include "transport.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace transport {

struct RdmaPeerAttrs {
    std::string gid;
    std::string ip;
    uint16_t port = 0;
    uint32_t qp_num = 0;
    uint32_t psn = 0;
    uint32_t rkey = 0;
    uint64_t vaddr = 0;
};

struct RdmaPeerQueuePair {
    uint8_t port = 0;
    uint16_t lid = 0;
    std::string gid;
    uint32_t qp_num = 0;
    uint32_t psn = 0;
};

struct RdmaPeerDeviceAttrs {
    std::string name;
    uint8_t port_count = 0;
    std::vector<RdmaPeerQueuePair> queue_pairs;
};

struct RdmaMemoryAttrs {
    uint32_t lkey = 0;
    uint32_t rkey = 0;
};

struct RdmaReceiveMessage {
    PeerID peer = kInvalidPeerID;
    const void* data = nullptr;
    uint32_t length = 0;
};

using RdmaReceiveCallback = std::function<Status(const RdmaReceiveMessage& message)>;

struct RdmaInitAttrs {
    std::string gid;
    std::string ip;
    uint16_t port = 0;
    uint32_t qp_num = 0;
    uint32_t psn = 0;
    uint32_t rkey = 0;
    uint64_t vaddr = 0;
    uint32_t recv_depth = 128;
    uint32_t recv_buffer_size = 4096;
    uint8_t gid_index = 0;
    RdmaReceiveCallback receive_callback;
    std::map<std::string, std::string> options;
};

class RdmaTransport final : public Transport {
   public:
    RdmaTransport();
    ~RdmaTransport() override;

    RdmaTransport(const RdmaTransport&) = delete;
    RdmaTransport& operator=(const RdmaTransport&) = delete;

    const char* name() const override;
    Status init() override;
    Status init(const RdmaInitAttrs& options);
    Status shutdown() override;
    Status registerMemory(const MemoryRegion& memory) override;
    Status unregisterMemory(const MemoryRegion& memory) override;
    Status exportMetadata(Metadata& out) const override;
    Status importMetadata(PeerID peer, const Metadata& metadata) override;
    Status connectPeer(PeerID peer) override;
    Status submitTransfer(const Transfer& transfer) override;

    const RdmaPeerAttrs* peer(PeerID id) const;

   private:
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
    std::mt19937 rng_;
    std::atomic<uint64_t> next_wr_id_{1};
};

}  // namespace transport
