#pragma once

#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
#include <functional>
#include "core/ffts_engine.h"
#endif
#include <cstdint>
#include <memory>
#include <vector>
#include "core/transport.h"

namespace transport {

inline constexpr const char* kFftsTransportProtocol = "ffts";

struct FftsInitAttrs final : InitAttrs {
    std::vector<int> device_ids;
    uint16_t max_ready_lanes = 8;
};

class FftsTransport final : public Transport {
public:
    FftsTransport();
#ifdef TRANSPORT_P2P_ENABLE_FFTS_TESTING
    struct EngineHooks {
        std::function<Status(int, uint16_t)> init;
        std::function<Status(int)> shutdown;
        std::function<Status(int, void*, size_t, FftsMemoryRegistration&)> register_host;
        std::function<Status(int, const FftsMemoryRegistration&)> unregister_host;
        std::function<Status(int, void*, size_t, FftsMemoryRegistration&)> register_device;
        std::function<Status(int, const FftsMemoryRegistration&)> unregister_device;
        std::function<Status(int, const std::vector<FftsCopySpec>&)> submit;
    };

    explicit FftsTransport(EngineHooks hooks);
#endif
    ~FftsTransport() override;

    FftsTransport(const FftsTransport&) = delete;
    FftsTransport& operator=(const FftsTransport&) = delete;

    const char* Name() const override;
    Status Init(const InitAttrs& options) override;
    Status Shutdown() override;

    Status RegisterMemory(const MemoryRegion& memory, MemoryHandle& handle) override;
    Status UnregisterMemory(MemoryHandle handle) override;
    Status ExportMetadata(const ManagerID& manager_id, Metadata& out) override;
    Status ImportMetadata(const ManagerID& manager_id, const Metadata& metadata) override;
    Status Execute(const Operation& request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TransportPtr MakeFftsTransport();

}  // namespace transport
