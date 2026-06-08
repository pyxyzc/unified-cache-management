#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace transport {

using PeerID = uint64_t;
using MemoryHandle = uint64_t;
using Metadata = std::vector<uint8_t>;

constexpr PeerID kInvalidPeerID = 0;
constexpr MemoryHandle kInvalidMemoryHandle = 0;

enum class Opcode {
    Send,
    Read,
    Write,
};

enum class Status {
    Ok,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    Failed,
};

enum class MemoryType {
    Host,
    Device,
};

struct MemoryRegion {
    void* addr = nullptr;
    uint64_t length = 0;
    MemoryType type = MemoryType::Host;
    int device_id = -1;
};

struct Transfer {
    Opcode opcode = Opcode::Send;
    uint64_t local_address = 0;
    PeerID target_id = kInvalidPeerID;
    uint64_t target_address = 0;
    uint64_t length = 0;
};

class Transport {
   public:
    virtual ~Transport();

    virtual const char* name() const = 0;
    virtual Status init();
    virtual Status shutdown();

    virtual Status registerMemory(const MemoryRegion& memory);
    virtual Status unregisterMemory(const MemoryRegion& memory);
    virtual Status exportMetadata(Metadata& out) const;
    virtual Status importMetadata(PeerID peer, const Metadata& metadata);
    virtual Status connectPeer(PeerID peer);
    virtual Status submitTransfer(const Transfer& request);
};

using TransportPtr = std::shared_ptr<Transport>;

}  // namespace transport
