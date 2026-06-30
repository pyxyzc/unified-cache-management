#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "core/ffts_dispatcher.h"
#include "core/transport.h"

namespace transport {

struct FftsEngineOptions {
    int device_id = -1;
    uint16_t max_ready_lanes = 8;
};

struct FftsMemoryRegistration {
    void* origin_addr = nullptr;
    void* ffts_addr = nullptr;
    size_t size = 0;
    bool requires_unregister = false;
};

class FftsEngine final {
public:
    FftsEngine();
    ~FftsEngine();

    FftsEngine(const FftsEngine&) = delete;
    FftsEngine& operator=(const FftsEngine&) = delete;

    static Status DiscoverDeviceIds(std::vector<int>& device_ids);

    Status Init(const FftsEngineOptions& options);
    Status Shutdown();
    Status RegisterHostMemory(void* host, size_t size, FftsMemoryRegistration& registration);
    Status UnregisterHostMemory(const FftsMemoryRegistration& registration);
    Status RegisterDeviceMemory(void* device, size_t size, FftsMemoryRegistration& registration);
    Status UnregisterDeviceMemory(const FftsMemoryRegistration& registration);
    Status Submit(const std::vector<FftsCopySpec>& copies);
    Status Synchronize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
