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
    Custom,
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

struct InitAttrs {
    virtual ~InitAttrs();
};

struct Transfer {
    Opcode opcode = Opcode::Send;
    void* local_addr = nullptr;
    PeerID target_id = kInvalidPeerID;
    uint64_t remote_addr = 0;
    uint64_t length = 0;
};

class Transport {
   public:
    virtual ~Transport();

    virtual const char* name() const = 0;
    virtual Status init(const InitAttrs& options);
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
