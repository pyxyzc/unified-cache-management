#pragma once

#include "control/tcp_transport.h"
#include "core/transport.h"

#include <acl/acl.h>
#include <hccl/hccl_types.h>

#include <cstddef>
#include <cstdint>

namespace transport {

enum class HcclBootstrapRole {
    Root,
    Client,
};

enum class HcclCollectiveOp {
    Broadcast,
};

struct HcclInitAttrs : public InitAttrs {
    TcpEndpoint bootstrap_endpoint;
    HcclBootstrapRole bootstrap_role = HcclBootstrapRole::Root;
    uint32_t rank_count = 2;
    uint32_t rank = 0;
    uint32_t root_info_rank = 0;
};

struct HcclCollectiveTransfer {
    HcclCollectiveOp op = HcclCollectiveOp::Broadcast;
    void* buffer = nullptr;
    uint64_t length = 0;
    uint32_t root_rank = 0;
    aclrtStream stream = nullptr;
};

class HcclTransport final : public Transport {
   public:
    HcclTransport() = default;
    ~HcclTransport() override;

    HcclTransport(const HcclTransport&) = delete;
    HcclTransport& operator=(const HcclTransport&) = delete;
    HcclTransport(HcclTransport&&) = delete;
    HcclTransport& operator=(HcclTransport&&) = delete;

    const char* name() const override;
    Status init(const InitAttrs& options) override;
    Status init(const HcclInitAttrs& options);
    Status shutdown() override;
    Status submitTransfer(const Transfer& request) override;

    Status submitCollective(const HcclCollectiveTransfer& transfer);

    bool ready() const noexcept;
    uint32_t rank() const noexcept;
    uint32_t rankCount() const noexcept;
    uint32_t rootInfoRank() const noexcept;
    HcclComm raw() const noexcept;

   private:
    Status ensureReady() const;
    Status initHcclComm(const HcclRootInfo& root_info, const HcclInitAttrs& options);
    Status submitBroadcast(const HcclCollectiveTransfer& transfer);

    HcclComm comm_ = nullptr;
    uint32_t rank_ = 0;
    uint32_t rank_count_ = 0;
    uint32_t root_info_rank_ = 0;
};

}  // namespace transport
