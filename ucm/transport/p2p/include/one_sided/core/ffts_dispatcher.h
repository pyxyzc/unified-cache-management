#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "core/transport.h"

namespace transport {

struct FftsCopySpec {
    void* dst = nullptr;
    const void* src = nullptr;
    size_t size = 0;
};

class FftsDispatcher final {
public:
    FftsDispatcher();
    ~FftsDispatcher();

    FftsDispatcher(const FftsDispatcher&) = delete;
    FftsDispatcher& operator=(const FftsDispatcher&) = delete;

    Status BuildCopies(const std::vector<FftsCopySpec>& copies, uint16_t max_ready_lanes);
    Status Launch(void* stream);
    size_t ContextCount() const noexcept;
    uint16_t ReadyContextNum() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
