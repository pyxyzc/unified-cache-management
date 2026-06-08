#pragma once

#include "transport.hpp"

#include <map>
#include <memory>
#include <string>

namespace transport {

struct HixlInitAttrs {
    std::string local_engine;
    std::map<std::string, std::string> options;
    int32_t connect_timeout_ms = 1000;
    int32_t transfer_timeout_ms = 1000;
};

struct HixlPeerAttrs {
    std::string remote_engine;
};

class HixlTransport final : public Transport {
   public:
    HixlTransport();
    ~HixlTransport() override;

    HixlTransport(const HixlTransport&) = delete;
    HixlTransport& operator=(const HixlTransport&) = delete;

    const char* name() const override;
    Status init() override;
    Status init(const HixlInitAttrs& options);
    Status shutdown() override;
    Status registerMemory(const MemoryRegion& memory) override;
    Status unregisterMemory(const MemoryRegion& memory) override;
    Status exportMetadata(Metadata& out) const override;
    Status importMetadata(PeerID peer, const Metadata& metadata) override;
    Status submitTransfer(const Transfer& request) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
