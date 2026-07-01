#include "core/ffts_transport.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

namespace transport {
namespace {

constexpr int kDeviceCount = 8;
constexpr size_t kCopySize = 16;
constexpr uint16_t kMaxReadyLanes = 8;

struct SubmittedBatch {
    int device_id = -1;
    std::vector<FftsCopySpec> copies;
};

struct ConcurrentEngineState {
    std::mutex mutex;
    std::vector<std::pair<int, uint16_t>> inits;
    std::vector<SubmittedBatch> submissions;
    std::atomic<int> entered_submits{0};
    std::atomic<int> in_flight_submits{0};
    std::atomic<int> max_in_flight_submits{0};
};

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

void UpdateMax(std::atomic<int>& max_value, int candidate)
{
    int observed = max_value.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !max_value.compare_exchange_weak(observed, candidate, std::memory_order_relaxed)) {}
}

FftsTransport::EngineHooks MakeHooks(const std::shared_ptr<ConcurrentEngineState>& state)
{
    FftsTransport::EngineHooks hooks;
    hooks.init = [state](int device_id, uint16_t max_ready_lanes) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->inits.emplace_back(device_id, max_ready_lanes);
        return Status::Ok;
    };
    hooks.shutdown = [](int) { return Status::Ok; };
    hooks.register_host = [](int, void*, size_t, FftsMemoryRegistration&) {
        return Status::Failed;
    };
    hooks.unregister_host = [](int, const FftsMemoryRegistration&) { return Status::Ok; };
    hooks.register_device = [](int, void* device, size_t size,
                               FftsMemoryRegistration& registration) {
        registration.origin_addr = device;
        registration.ffts_addr = device;
        registration.size = size;
        registration.requires_unregister = false;
        return Status::Ok;
    };
    hooks.unregister_device = [](int, const FftsMemoryRegistration&) { return Status::Ok; };
    hooks.submit = [state](int device_id, const std::vector<FftsCopySpec>& copies) {
        const auto active = state->in_flight_submits.fetch_add(1, std::memory_order_acq_rel) + 1;
        UpdateMax(state->max_in_flight_submits, active);
        state->entered_submits.fetch_add(1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->submissions.push_back(SubmittedBatch{device_id, copies});
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (state->entered_submits.load(std::memory_order_acquire) < kDeviceCount &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        state->in_flight_submits.fetch_sub(1, std::memory_order_acq_rel);
        return Status::Ok;
    };
    return hooks;
}

Segment MakeSegment(void* src, void* dst)
{
    return Segment{src, PtrToU64(dst), kCopySize};
}

TEST(FftsTransportE2ETest, ConcurrentExecuteSubmitsToEightDevices)
{
    auto state = std::make_shared<ConcurrentEngineState>();
    FftsTransport transport(MakeHooks(state));

    FftsInitAttrs attrs;
    attrs.max_ready_lanes = kMaxReadyLanes;
    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        attrs.device_ids.push_back(device_id);
    }
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ASSERT_EQ(static_cast<size_t>(kDeviceCount), state->inits.size());
        for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
            EXPECT_EQ(device_id, state->inits[device_id].first);
            EXPECT_EQ(kMaxReadyLanes, state->inits[device_id].second);
        }
    }

    std::array<std::array<std::uint8_t, kCopySize>, kDeviceCount> src_buffers{};
    std::array<std::array<std::uint8_t, kCopySize>, kDeviceCount> dst_buffers{};
    std::array<MemoryHandle, kDeviceCount> src_handles{};
    std::array<MemoryHandle, kDeviceCount> dst_handles{};
    std::fill(src_handles.begin(), src_handles.end(), kInvalidMemoryHandle);
    std::fill(dst_handles.begin(), dst_handles.end(), kInvalidMemoryHandle);

    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        ASSERT_EQ(Status::Ok,
                  transport.RegisterMemory(
                      MemoryRegion{src_buffers[device_id].data(), kCopySize,
                                   MemoryType::Device, device_id},
                      src_handles[device_id]));
        ASSERT_EQ(Status::Ok,
                  transport.RegisterMemory(
                      MemoryRegion{dst_buffers[device_id].data(), kCopySize,
                                   MemoryType::Device, device_id},
                      dst_handles[device_id]));
    }

    std::mutex start_mutex;
    std::condition_variable start_cv;
    int ready_threads = 0;
    bool start = false;
    std::array<Status, kDeviceCount> results{};
    results.fill(Status::Failed);
    std::vector<std::thread> workers;
    workers.reserve(kDeviceCount);

    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        workers.emplace_back([&, device_id]() {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                ++ready_threads;
                if (ready_threads == kDeviceCount) { start_cv.notify_one(); }
                start_cv.wait(lock, [&start]() { return start; });
            }

            Operation op;
            op.opcode = Opcode::Write;
            op.direct = OperationDirect::LocalDeviceDevice;
            op.ops.push_back(
                MakeSegment(src_buffers[device_id].data(), dst_buffers[device_id].data()));
            results[device_id] = transport.Execute(op);
        });
    }

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_cv.wait(lock, [&ready_threads]() { return ready_threads == kDeviceCount; });
        start = true;
    }
    start_cv.notify_all();

    for (auto& worker : workers) { worker.join(); }

    for (const auto result : results) { EXPECT_EQ(Status::Ok, result); }

    std::array<int, kDeviceCount> submissions_by_device{};
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ASSERT_EQ(static_cast<size_t>(kDeviceCount), state->submissions.size());
        for (const auto& submission : state->submissions) {
            ASSERT_GE(submission.device_id, 0);
            ASSERT_LT(submission.device_id, kDeviceCount);
            ++submissions_by_device[submission.device_id];

            ASSERT_EQ(1U, submission.copies.size());
            const auto device_id = submission.device_id;
            EXPECT_EQ(dst_buffers[device_id].data(), submission.copies[0].dst);
            EXPECT_EQ(static_cast<const void*>(src_buffers[device_id].data()),
                      submission.copies[0].src);
            EXPECT_EQ(kCopySize, submission.copies[0].size);
        }
    }

    for (const auto count : submissions_by_device) { EXPECT_EQ(1, count); }
    EXPECT_GT(state->max_in_flight_submits.load(std::memory_order_acquire), 1)
        << "Execute calls were serialized before reaching submit";

    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handles[device_id]));
        EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src_handles[device_id]));
    }
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

}  // namespace
}  // namespace transport
