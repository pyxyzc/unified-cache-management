#include "core/ffts_dispatcher.h"
#include <algorithm>
#include <limits>
#include "logger/logger.h"

#if __has_include("runtime/rt_ffts_plus.h")
#include "runtime/rt_ffts_plus.h"
#elif __has_include("rt_external_ffts.h")
#include "rt_external_ffts.h"
#else
#error "FFTS Plus header was not found. Configure Ascend FFTS include directories in CMake."
#endif

namespace transport {
namespace {

constexpr uint32_t kFftsSdmaFp32AtomicMoveSqe = 0x1E70;
constexpr uint16_t kFftsContextMaxNum = 128;
constexpr uint8_t kFftsCommunicationTask = 0x5A;

static_assert(sizeof(rtFftsPlusComCtx_t) == 128, "rtFftsPlusComCtx_t must be 128 bytes");
static_assert(sizeof(rtFftsPlusSdmaCtx_t) == 128, "rtFftsPlusSdmaCtx_t must be 128 bytes");

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

void BuildSdmaCtx(void* dst, const void* src, size_t size, rtFftsPlusSdmaCtx_t* ctx)
{
    constexpr uint32_t kShift = 32;
    constexpr uint64_t kLowMask = 0xFFFFFFFFULL;

    const uint64_t src_addr = PtrToU64(src);
    const uint64_t dst_addr = PtrToU64(dst);

    ctx->contextType = RT_CTX_TYPE_SDMA;
    ctx->threadDim = 1;
    ctx->sdmaSqeHeader = kFftsSdmaFp32AtomicMoveSqe;
    ctx->sourceAddressBaseL = static_cast<uint32_t>(src_addr & kLowMask);
    ctx->sourceAddressBaseH = static_cast<uint32_t>(src_addr >> kShift);
    ctx->sourceAddressOffset = 0;
    ctx->destinationAddressBaseL = static_cast<uint32_t>(dst_addr & kLowMask);
    ctx->destinationAddressBaseH = static_cast<uint32_t>(dst_addr >> kShift);
    ctx->destinationAddressOffset = 0;
    ctx->nonTailDataLength = static_cast<uint32_t>(size);
    ctx->tailDataLength = static_cast<uint32_t>(size);
}

}  // namespace

struct FftsDispatcher::Impl {
    std::vector<rtFftsPlusComCtx_t> contexts;
    uint16_t ready_context_num = 0;
    bool completed = false;

    void Reset()
    {
        contexts.clear();
        ready_context_num = 0;
        completed = false;
    }

    Status AddMemcpy(void* dst, const void* src, size_t size)
    {
        if (completed) { return Status::Failed; }
        if (dst == nullptr || src == nullptr || size == 0 ||
            size > std::numeric_limits<uint32_t>::max()) {
            return Status::InvalidArgument;
        }
        if (contexts.size() >= std::numeric_limits<uint16_t>::max()) {
            return Status::InvalidArgument;
        }

        rtFftsPlusComCtx_t com_ctx{};
        auto* sdma_ctx = reinterpret_cast<rtFftsPlusSdmaCtx_t*>(&com_ctx);
        BuildSdmaCtx(dst, src, size, sdma_ctx);
        contexts.push_back(com_ctx);
        return Status::Ok;
    }

    Status AddDependency(uint32_t predecessor_id, uint32_t successor_id)
    {
        if (predecessor_id >= contexts.size() || successor_id >= contexts.size()) {
            return Status::InvalidArgument;
        }
        auto& predecessor = contexts[predecessor_id];
        auto& successor = contexts[successor_id];
        if (predecessor.successorNum >= RT_CTX_SUCCESSOR_NUM ||
            successor.predCntInit >= std::numeric_limits<uint8_t>::max()) {
            return Status::InvalidArgument;
        }

        predecessor.successorList[predecessor.successorNum] = static_cast<uint16_t>(successor_id);
        predecessor.successorNum++;
        successor.predCntInit++;
        successor.predCnt++;
        return Status::Ok;
    }
};

FftsDispatcher::FftsDispatcher() : impl_(std::make_unique<Impl>()) {}

FftsDispatcher::~FftsDispatcher() = default;

Status FftsDispatcher::BuildCopies(const std::vector<FftsCopySpec>& copies,
                                   uint16_t max_ready_lanes)
{
    impl_->Reset();
    if (copies.empty() || max_ready_lanes == 0) { return Status::InvalidArgument; }

    impl_->contexts.reserve(copies.size());
    const auto lane_count =
        static_cast<uint16_t>(std::min<size_t>(copies.size(), max_ready_lanes));
    std::vector<int32_t> last_task_id(lane_count, -1);

    for (size_t i = 0; i < copies.size(); ++i) {
        auto status = impl_->AddMemcpy(copies[i].dst, copies[i].src, copies[i].size);
        if (status != Status::Ok) { return status; }

        const size_t lane = i % lane_count;
        const auto task_id = static_cast<uint32_t>(impl_->contexts.size() - 1);
        if (last_task_id[lane] >= 0) {
            status = impl_->AddDependency(static_cast<uint32_t>(last_task_id[lane]), task_id);
            if (status != Status::Ok) { return status; }
        }
        last_task_id[lane] = static_cast<int32_t>(task_id);
    }

    impl_->ready_context_num = lane_count;
    return Status::Ok;
}

Status FftsDispatcher::Launch(void* stream)
{
    if (stream == nullptr || impl_->contexts.empty() || impl_->ready_context_num == 0 ||
        impl_->ready_context_num > impl_->contexts.size()) {
        return Status::InvalidArgument;
    }

    rtFftsPlusSqe_t sqe{};
    sqe.fftsType = RT_FFTS_PLUS_TYPE;
    sqe.totalContextNum = static_cast<uint16_t>(impl_->contexts.size());
    sqe.readyContextNum = impl_->ready_context_num;
    sqe.preloadContextNum = std::min<uint16_t>(impl_->ready_context_num, kFftsContextMaxNum);
    sqe.timeout = 0;
    sqe.subType = kFftsCommunicationTask;

    rtFftsPlusTaskInfo_t task{};
    task.fftsPlusSqe = &sqe;
    task.descBuf = impl_->contexts.data();
    task.descBufLen = sizeof(rtFftsPlusComCtx_t) * impl_->contexts.size();
    task.descAddrType = RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST;
    task.argsHandleInfoNum = 0;
    task.argsHandleInfoPtr = nullptr;

    impl_->completed = true;
    const auto ret =
        rtFftsPlusTaskLaunchWithFlag(&task, reinterpret_cast<rtStream_t>(stream), 0);
    if (ret == RT_ERROR_NONE) { return Status::Ok; }
    UC_ERROR("FFTS failed to launch FFTS plus task ret={}", static_cast<int32_t>(ret));
    return Status::Failed;
}

size_t FftsDispatcher::ContextCount() const noexcept { return impl_->contexts.size(); }

uint16_t FftsDispatcher::ReadyContextNum() const noexcept
{
    return impl_->ready_context_num;
}

}  // namespace transport
