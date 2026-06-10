#include "core/transport.h"

namespace transport {

InitAttrs::~InitAttrs() = default;
Transport::~Transport() = default;

Status Transport::init(const InitAttrs& options) {
    (void)options;
    return Status::Ok;
}

Status Transport::shutdown() {
    return Status::Ok;
}

Status Transport::registerMemory(const MemoryRegion& memory) {
    (void)memory;
    return Status::Ok;
}

Status Transport::unregisterMemory(const MemoryRegion& memory) {
    (void)memory;
    return Status::Ok;
}

Status Transport::exportMetadata(Metadata& out) const {
    (void)out;
    return Status::Ok;
}

Status Transport::importMetadata(PeerID peer, const Metadata& metadata) {
    (void)peer;
    (void)metadata;
    return Status::Ok;
}

Status Transport::connectPeer(PeerID peer) {
    (void)peer;
    return Status::Ok;
}

Status Transport::submitTransfer(const Transfer& request) {
    (void)request;
    return Status::Ok;
}

}  // namespace transport
