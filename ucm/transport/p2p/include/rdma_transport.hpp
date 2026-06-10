#pragma once

#include "transport.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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

struct RdmaInitAttrs : public InitAttrs {
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
    Status init(const InitAttrs& options) override;
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
