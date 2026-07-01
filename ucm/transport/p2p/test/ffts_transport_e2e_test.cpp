#include "core/ffts_transport.h"
#include <acl/acl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <runtime/config.h>
#include <runtime/dev.h>
#include <gtest/gtest.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace transport {
namespace {

constexpr int kDeviceCount = 8;
constexpr size_t kPageSize = 4096;
constexpr size_t kCopySize = 4096;
constexpr uint16_t kMaxReadyLanes = 8;

const char* StatusName(Status status)
{
    switch (status) {
        case Status::Ok:
            return "Ok";
        case Status::InvalidArgument:
            return "InvalidArgument";
        case Status::Failed:
            return "Failed";
    }
    return "Unknown";
}

std::string CurrentSocName()
{
    const char* soc_name = aclrtGetSocName();
    if (soc_name == nullptr) { return "unknown"; }
    return soc_name;
}

const char* FftsModeName(int mode)
{
    switch (mode) {
        case RT_MODE_NO_FFTS:
            return "NO_FFTS";
        case RT_MODE_FFTS:
            return "FFTS";
        case RT_MODE_FFTS_PLUS:
            return "FFTS_PLUS";
        default:
            return "UNKNOWN";
    }
}

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

class PhaseTimings {
public:
    using Clock = std::chrono::steady_clock;

    explicit PhaseTimings(std::string title) : title_(std::move(title)) {}

    void Start(const char* name)
    {
        Stop();
        current_name_ = name;
        current_start_ = Clock::now();
        running_ = true;
    }

    void Stop()
    {
        if (!running_) { return; }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - current_start_);
        records_.push_back(Record{current_name_, elapsed.count()});
        running_ = false;
    }

    double PhaseMs(const char* name) const
    {
        for (const auto& record : records_) {
            if (std::strcmp(record.name, name) == 0) {
                return ToMilliseconds(record.elapsed_us);
            }
        }
        return 0.0;
    }

    double TotalMs() const
    {
        int64_t total_us = 0;
        for (const auto& record : records_) { total_us += record.elapsed_us; }
        return ToMilliseconds(total_us);
    }

    void Print()
    {
        Stop();
        if (printed_) { return; }
        printed_ = true;

        std::ios old_state(nullptr);
        old_state.copyfmt(std::cerr);
        std::cerr << "\n[FftsTransportE2ETest] " << title_ << "\n";
        for (const auto& record : records_) {
            std::cerr << "  " << std::left << std::setw(24) << record.name << std::right
                      << std::fixed << std::setprecision(3)
                      << ToMilliseconds(record.elapsed_us) << " ms\n";
        }
        std::cerr << "  " << std::left << std::setw(24) << "total" << std::right
                  << std::fixed << std::setprecision(3) << TotalMs() << " ms\n";
        std::cerr.copyfmt(old_state);
    }

private:
    struct Record {
        const char* name = "";
        int64_t elapsed_us = 0;
    };

    static double ToMilliseconds(int64_t us)
    {
        return static_cast<double>(us) / 1000.0;
    }

    std::string title_;
    std::vector<Record> records_;
    Clock::time_point current_start_;
    const char* current_name_ = "";
    bool running_ = false;
    bool printed_ = false;
};

void PrintBenchmarkComparison(const char* title, const PhaseTimings& ffts,
                              const PhaseTimings& batch)
{
    const auto ffts_execute_ms = ffts.PhaseMs("execute_threads");
    const auto batch_copy_ms = batch.PhaseMs("memcpy_batch");

    std::ios old_state(nullptr);
    old_state.copyfmt(std::cerr);
    std::cerr << "\n[FftsTransportE2ETest] " << title << " comparison\n";
    std::cerr << "  " << std::left << std::setw(28) << "ffts_transport.total"
              << std::right << std::fixed << std::setprecision(3) << ffts.TotalMs()
              << " ms\n";
    std::cerr << "  " << std::left << std::setw(28) << "aclrtMemcpyBatch.total"
              << std::right << std::fixed << std::setprecision(3) << batch.TotalMs()
              << " ms\n";
    std::cerr << "  " << std::left << std::setw(28) << "ffts.execute_threads"
              << std::right << std::fixed << std::setprecision(3) << ffts_execute_ms
              << " ms\n";
    std::cerr << "  " << std::left << std::setw(28) << "batch.memcpy_batch"
              << std::right << std::fixed << std::setprecision(3) << batch_copy_ms
              << " ms\n";
    if (batch_copy_ms > 0.0) {
        std::cerr << "  " << std::left << std::setw(28) << "execute/batch ratio"
                  << std::right << std::fixed << std::setprecision(3)
                  << (ffts_execute_ms / batch_copy_ms) << "\n";
    }
    std::cerr.copyfmt(old_state);
}

::testing::AssertionResult SetDevice(int device_id)
{
    const auto ret = aclrtSetDevice(device_id);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtSetDevice(" << device_id << ") failed, ret="
               << static_cast<int>(ret);
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult FftsPlusAvailable(int device_id)
{
    const auto soc_name = CurrentSocName();
    int ffts_mode = RT_MODE_NO_FFTS;
    const auto cap_ret = rtGetDeviceCapability(device_id, RT_MODULE_TYPE_TSCPU,
                                               FEATURE_TYPE_FFTS_MODE, &ffts_mode);
    if (cap_ret != 0) {
        return ::testing::AssertionFailure()
               << "Cannot query FFTS mode on device=" << device_id << ", soc=" << soc_name
               << ", rtGetDeviceCapability ret=" << static_cast<int>(cap_ret);
    }
    if (ffts_mode != RT_MODE_FFTS_PLUS) {
        return ::testing::AssertionFailure()
               << "FFTS plus is not available on device=" << device_id << ", soc="
               << soc_name << ", mode=" << FftsModeName(ffts_mode) << "("
               << ffts_mode << ")";
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult CollectFftsDeviceIds(std::vector<int>& device_ids)
{
    device_ids.clear();

    uint32_t available_devices = 0;
    const auto count_ret = aclrtGetDeviceCount(&available_devices);
    if (count_ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtGetDeviceCount failed, ret=" << static_cast<int>(count_ret);
    }
    if (available_devices < static_cast<uint32_t>(kDeviceCount)) {
        return ::testing::AssertionFailure()
               << "Need " << kDeviceCount << " devices for FFTS transport E2E, found "
               << available_devices;
    }

    device_ids.reserve(kDeviceCount);
    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        const auto ffts_plus = FftsPlusAvailable(device_id);
        if (!ffts_plus) { return ffts_plus; }
        device_ids.push_back(device_id);
    }
    return ::testing::AssertionSuccess();
}

std::vector<std::uint8_t> MakePattern(size_t size, std::uint8_t seed)
{
    std::vector<std::uint8_t> pattern(size);
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>((i * 131U + seed) & 0xffU);
    }
    return pattern;
}

class DeviceAllocation {
public:
    DeviceAllocation() = default;
    ~DeviceAllocation() { Reset(); }

    DeviceAllocation(const DeviceAllocation&) = delete;
    DeviceAllocation& operator=(const DeviceAllocation&) = delete;

    ::testing::AssertionResult Allocate(int device_id, size_t size)
    {
        Reset();
        const auto set_result = SetDevice(device_id);
        if (!set_result) { return set_result; }

        void* raw = nullptr;
        const auto ret = aclrtMalloc(&raw, size, ACL_MEM_TYPE_HIGH_BAND_WIDTH);
        if (ret != ACL_SUCCESS) {
            return ::testing::AssertionFailure()
                   << "aclrtMalloc failed on device=" << device_id
                   << ", ret=" << static_cast<int>(ret);
        }

        device_id_ = device_id;
        ptr_ = raw;
        return ::testing::AssertionSuccess();
    }

    void* Get() const noexcept { return ptr_; }

private:
    void Reset()
    {
        if (ptr_ == nullptr) { return; }
        (void)aclrtSetDevice(device_id_);
        (void)aclrtFree(ptr_);
        ptr_ = nullptr;
        device_id_ = -1;
    }

    int device_id_ = -1;
    void* ptr_ = nullptr;
};

class HostAllocation {
public:
    HostAllocation() = default;
    ~HostAllocation() { Reset(); }

    HostAllocation(const HostAllocation&) = delete;
    HostAllocation& operator=(const HostAllocation&) = delete;

    ::testing::AssertionResult Allocate(size_t size)
    {
        Reset();
        const size_t aligned_size = ((size + kPageSize - 1) / kPageSize) * kPageSize;
        void* raw = nullptr;
#if defined(_WIN32)
        raw = _aligned_malloc(aligned_size, kPageSize);
        if (raw == nullptr) {
            return ::testing::AssertionFailure() << "_aligned_malloc failed";
        }
#else
        if (posix_memalign(&raw, kPageSize, aligned_size) != 0) {
            return ::testing::AssertionFailure() << "posix_memalign failed";
        }
#endif
        std::memset(raw, 0, aligned_size);
        ptr_ = raw;
        return ::testing::AssertionSuccess();
    }

    void* Get() const noexcept { return ptr_; }

private:
    void Reset()
    {
        if (ptr_ == nullptr) { return; }
#if defined(_WIN32)
        _aligned_free(ptr_);
#else
        std::free(ptr_);
#endif
        ptr_ = nullptr;
    }

    void* ptr_ = nullptr;
};

::testing::AssertionResult CopyHostToDevice(int device_id, void* device,
                                            const std::vector<std::uint8_t>& data)
{
    const auto set_result = SetDevice(device_id);
    if (!set_result) { return set_result; }

    const auto ret = aclrtMemcpy(device, data.size(), data.data(), data.size(),
                                 ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpy(H2D) failed on device=" << device_id
               << ", ret=" << static_cast<int>(ret);
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult ZeroDevice(int device_id, void* device, size_t size)
{
    const std::vector<std::uint8_t> zeros(size, 0);
    return CopyHostToDevice(device_id, device, zeros);
}

void CopyPatternToHost(void* host, const std::vector<std::uint8_t>& data)
{
    std::memcpy(host, data.data(), data.size());
}

::testing::AssertionResult DeviceEquals(int device_id, void* device,
                                        const std::vector<std::uint8_t>& expected)
{
    const auto set_result = SetDevice(device_id);
    if (!set_result) { return set_result; }

    std::vector<std::uint8_t> actual(expected.size(), 0);
    const auto ret = aclrtMemcpy(actual.data(), actual.size(), device, expected.size(),
                                 ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpy(D2H) failed on device=" << device_id
               << ", ret=" << static_cast<int>(ret);
    }
    if (actual == expected) { return ::testing::AssertionSuccess(); }

    size_t mismatch = 0;
    while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) {
        ++mismatch;
    }
    return ::testing::AssertionFailure()
           << "device=" << device_id << " buffer mismatch at offset=" << mismatch
           << ", expected=" << static_cast<int>(expected[mismatch])
           << ", actual=" << static_cast<int>(actual[mismatch]);
}

::testing::AssertionResult HostEquals(int device_id, const void* host,
                                      const std::vector<std::uint8_t>& expected)
{
    const auto* begin = static_cast<const std::uint8_t*>(host);
    const std::vector<std::uint8_t> actual(begin, begin + expected.size());
    if (actual == expected) { return ::testing::AssertionSuccess(); }

    size_t mismatch = 0;
    while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) {
        ++mismatch;
    }
    return ::testing::AssertionFailure()
           << "device=" << device_id << " host buffer mismatch at offset=" << mismatch
           << ", expected=" << static_cast<int>(expected[mismatch])
           << ", actual=" << static_cast<int>(actual[mismatch]);
}

Segment SegmentFor(void* local_addr, const void* remote_addr, uint64_t length)
{
    return Segment{local_addr, PtrToU64(remote_addr), length};
}

::testing::AssertionResult RegisterDeviceRegion(FftsTransport& transport, int device_id,
                                                void* device, size_t size,
                                                MemoryHandle& handle)
{
    const auto status =
        transport.RegisterMemory(MemoryRegion{device, size, MemoryType::Device, device_id},
                                 handle);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Device) failed on device=" << device_id
               << ", status=" << StatusName(status);
    }
    if (handle == kInvalidMemoryHandle) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Device) returned invalid handle on device=" << device_id;
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RegisterHostRegion(FftsTransport& transport, void* host,
                                              size_t size, MemoryHandle& handle)
{
    const auto status =
        transport.RegisterMemory(MemoryRegion{host, size, MemoryType::Host, -1}, handle);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Host) failed, status=" << StatusName(status);
    }
    if (handle == kInvalidMemoryHandle) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Host) returned invalid handle";
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RunFftsDeviceToDevice(const std::vector<int>& device_ids,
                                                 PhaseTimings& timings)
{
    timings.Start("allocate_and_seed");
    std::array<DeviceAllocation, kDeviceCount> src_devices;
    std::array<DeviceAllocation, kDeviceCount> dst_devices;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = src_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }
        result = dst_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(17 + index * 19));
        result = CopyHostToDevice(device_id, src_devices[index].Get(), expected[index]);
        if (!result) { return result; }
        result = ZeroDevice(device_id, dst_devices[index].Get(), kCopySize);
        if (!result) { return result; }
    }

    timings.Start("transport_init");
    FftsTransport transport;
    FftsInitAttrs attrs;
    attrs.device_ids = device_ids;
    attrs.max_ready_lanes = kMaxReadyLanes;
    auto status = transport.Init(attrs);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Init failed, status=" << StatusName(status);
    }

    timings.Start("register_memory");
    std::array<MemoryHandle, kDeviceCount> src_handles{};
    std::array<MemoryHandle, kDeviceCount> dst_handles{};
    std::fill(src_handles.begin(), src_handles.end(), kInvalidMemoryHandle);
    std::fill(dst_handles.begin(), dst_handles.end(), kInvalidMemoryHandle);
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = RegisterDeviceRegion(transport, device_id, src_devices[index].Get(),
                                           kCopySize, src_handles[index]);
        if (!result) { return result; }
        result = RegisterDeviceRegion(transport, device_id, dst_devices[index].Get(),
                                      kCopySize, dst_handles[index]);
        if (!result) { return result; }
    }

    timings.Start("execute_threads");
    std::array<Status, kDeviceCount> results{};
    results.fill(Status::Failed);
    std::vector<std::thread> workers;
    workers.reserve(kDeviceCount);
    for (int index = 0; index < kDeviceCount; ++index) {
        workers.emplace_back([&, index]() {
            Operation op;
            op.opcode = Opcode::Write;
            op.direct = OperationDirect::LocalDeviceDevice;
            op.ops.push_back(SegmentFor(src_devices[index].Get(), dst_devices[index].Get(),
                                        kCopySize));
            results[index] = transport.Execute(op);
        });
    }
    for (auto& worker : workers) { worker.join(); }
    for (int index = 0; index < kDeviceCount; ++index) {
        if (results[index] != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "Execute failed on device=" << device_ids[index]
                   << ", status=" << StatusName(results[index]);
        }
    }

    timings.Start("verify");
    for (int index = 0; index < kDeviceCount; ++index) {
        auto result = DeviceEquals(device_ids[index], dst_devices[index].Get(),
                                   expected[index]);
        if (!result) { return result; }
    }

    timings.Start("unregister_shutdown");
    for (int index = 0; index < kDeviceCount; ++index) {
        status = transport.UnregisterMemory(dst_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(dst) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(status);
        }
        status = transport.UnregisterMemory(src_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(src) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(status);
        }
    }
    status = transport.Shutdown();
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Shutdown failed, status=" << StatusName(status);
    }
    timings.Stop();
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RunFftsHostToDevice(const std::vector<int>& device_ids,
                                               PhaseTimings& timings)
{
    timings.Start("allocate_and_seed");
    std::array<HostAllocation, kDeviceCount> src_hosts;
    std::array<DeviceAllocation, kDeviceCount> dst_devices;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = src_hosts[index].Allocate(kCopySize);
        if (!result) { return result; }
        result = dst_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(29 + index * 19));
        CopyPatternToHost(src_hosts[index].Get(), expected[index]);
        result = ZeroDevice(device_id, dst_devices[index].Get(), kCopySize);
        if (!result) { return result; }
    }

    timings.Start("transport_init");
    FftsTransport transport;
    FftsInitAttrs attrs;
    attrs.device_ids = device_ids;
    attrs.max_ready_lanes = kMaxReadyLanes;
    auto status = transport.Init(attrs);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Init failed, status=" << StatusName(status);
    }

    timings.Start("register_memory");
    std::array<MemoryHandle, kDeviceCount> host_handles{};
    std::array<MemoryHandle, kDeviceCount> device_handles{};
    std::fill(host_handles.begin(), host_handles.end(), kInvalidMemoryHandle);
    std::fill(device_handles.begin(), device_handles.end(), kInvalidMemoryHandle);
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = RegisterHostRegion(transport, src_hosts[index].Get(), kCopySize,
                                         host_handles[index]);
        if (!result) { return result; }
        result = RegisterDeviceRegion(transport, device_id, dst_devices[index].Get(),
                                      kCopySize, device_handles[index]);
        if (!result) { return result; }
    }

    timings.Start("execute_threads");
    std::array<Status, kDeviceCount> results{};
    results.fill(Status::Failed);
    std::vector<std::thread> workers;
    workers.reserve(kDeviceCount);
    for (int index = 0; index < kDeviceCount; ++index) {
        workers.emplace_back([&, index]() {
            Operation op;
            op.opcode = Opcode::Write;
            op.direct = OperationDirect::LocalDeviceHost;
            op.ops.push_back(SegmentFor(src_hosts[index].Get(), dst_devices[index].Get(),
                                        kCopySize));
            results[index] = transport.Execute(op);
        });
    }
    for (auto& worker : workers) { worker.join(); }
    for (int index = 0; index < kDeviceCount; ++index) {
        if (results[index] != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "Execute(H2D) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(results[index]);
        }
    }

    timings.Start("verify");
    for (int index = 0; index < kDeviceCount; ++index) {
        auto result = DeviceEquals(device_ids[index], dst_devices[index].Get(),
                                   expected[index]);
        if (!result) { return result; }
    }

    timings.Start("unregister_shutdown");
    for (int index = 0; index < kDeviceCount; ++index) {
        status = transport.UnregisterMemory(device_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(device) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(status);
        }
        status = transport.UnregisterMemory(host_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(host) failed, status=" << StatusName(status);
        }
    }
    status = transport.Shutdown();
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Shutdown failed, status=" << StatusName(status);
    }
    timings.Stop();
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RunFftsDeviceToHost(const std::vector<int>& device_ids,
                                               PhaseTimings& timings)
{
    timings.Start("allocate_and_seed");
    std::array<DeviceAllocation, kDeviceCount> src_devices;
    std::array<HostAllocation, kDeviceCount> dst_hosts;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = src_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }
        result = dst_hosts[index].Allocate(kCopySize);
        if (!result) { return result; }

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(43 + index * 19));
        result = CopyHostToDevice(device_id, src_devices[index].Get(), expected[index]);
        if (!result) { return result; }
    }

    timings.Start("transport_init");
    FftsTransport transport;
    FftsInitAttrs attrs;
    attrs.device_ids = device_ids;
    attrs.max_ready_lanes = kMaxReadyLanes;
    auto status = transport.Init(attrs);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Init failed, status=" << StatusName(status);
    }

    timings.Start("register_memory");
    std::array<MemoryHandle, kDeviceCount> device_handles{};
    std::array<MemoryHandle, kDeviceCount> host_handles{};
    std::fill(device_handles.begin(), device_handles.end(), kInvalidMemoryHandle);
    std::fill(host_handles.begin(), host_handles.end(), kInvalidMemoryHandle);
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = RegisterDeviceRegion(transport, device_id, src_devices[index].Get(),
                                           kCopySize, device_handles[index]);
        if (!result) { return result; }
        result = RegisterHostRegion(transport, dst_hosts[index].Get(), kCopySize,
                                    host_handles[index]);
        if (!result) { return result; }
    }

    timings.Start("execute_threads");
    std::array<Status, kDeviceCount> results{};
    results.fill(Status::Failed);
    std::vector<std::thread> workers;
    workers.reserve(kDeviceCount);
    for (int index = 0; index < kDeviceCount; ++index) {
        workers.emplace_back([&, index]() {
            Operation op;
            op.opcode = Opcode::Read;
            op.direct = OperationDirect::LocalDeviceHost;
            op.ops.push_back(SegmentFor(dst_hosts[index].Get(), src_devices[index].Get(),
                                        kCopySize));
            results[index] = transport.Execute(op);
        });
    }
    for (auto& worker : workers) { worker.join(); }
    for (int index = 0; index < kDeviceCount; ++index) {
        if (results[index] != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "Execute(D2H) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(results[index]);
        }
    }

    timings.Start("verify");
    for (int index = 0; index < kDeviceCount; ++index) {
        auto result = HostEquals(device_ids[index], dst_hosts[index].Get(), expected[index]);
        if (!result) { return result; }
    }

    timings.Start("unregister_shutdown");
    for (int index = 0; index < kDeviceCount; ++index) {
        status = transport.UnregisterMemory(host_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(host) failed, status=" << StatusName(status);
        }
        status = transport.UnregisterMemory(device_handles[index]);
        if (status != Status::Ok) {
            return ::testing::AssertionFailure()
                   << "UnregisterMemory(device) failed on device=" << device_ids[index]
                   << ", status=" << StatusName(status);
        }
    }
    status = transport.Shutdown();
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Shutdown failed, status=" << StatusName(status);
    }
    timings.Stop();
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RunAclMemcpyBatchHostToDevice(const std::vector<int>& device_ids,
                                                         PhaseTimings& timings)
{
    timings.Start("allocate_and_seed");
    std::array<HostAllocation, kDeviceCount> src_hosts;
    std::array<DeviceAllocation, kDeviceCount> dst_devices;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = src_hosts[index].Allocate(kCopySize);
        if (!result) { return result; }
        result = dst_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(61 + index * 19));
        CopyPatternToHost(src_hosts[index].Get(), expected[index]);
        result = ZeroDevice(device_id, dst_devices[index].Get(), kCopySize);
        if (!result) { return result; }
    }

    std::array<void*, kDeviceCount> dsts{};
    std::array<size_t, kDeviceCount> dest_max{};
    std::array<void*, kDeviceCount> srcs{};
    std::array<size_t, kDeviceCount> sizes{};
    std::array<aclrtMemcpyBatchAttr, kDeviceCount> attrs{};
    std::array<size_t, kDeviceCount> attr_indexes{};
    for (int index = 0; index < kDeviceCount; ++index) {
        dsts[index] = dst_devices[index].Get();
        dest_max[index] = kCopySize;
        srcs[index] = src_hosts[index].Get();
        sizes[index] = kCopySize;
        attr_indexes[index] = static_cast<size_t>(index);
        attrs[index] = {};
        attrs[index].dstLoc.id = static_cast<uint32_t>(device_ids[index]);
        attrs[index].dstLoc.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        attrs[index].srcLoc.id = 0;
        attrs[index].srcLoc.type = ACL_MEM_LOCATION_TYPE_HOST;
    }

    timings.Start("memcpy_batch");
    size_t fail_index = std::numeric_limits<size_t>::max();
    const auto ret = aclrtMemcpyBatch(dsts.data(), dest_max.data(), srcs.data(), sizes.data(),
                                      kDeviceCount, attrs.data(), attr_indexes.data(),
                                      kDeviceCount, &fail_index);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpyBatch(H2D) failed, ret=" << static_cast<int>(ret)
               << ", fail_index=" << fail_index;
    }

    timings.Start("verify");
    for (int index = 0; index < kDeviceCount; ++index) {
        auto result = DeviceEquals(device_ids[index], dst_devices[index].Get(),
                                   expected[index]);
        if (!result) { return result; }
    }
    timings.Stop();
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RunAclMemcpyBatchDeviceToHost(const std::vector<int>& device_ids,
                                                         PhaseTimings& timings)
{
    timings.Start("allocate_and_seed");
    std::array<DeviceAllocation, kDeviceCount> src_devices;
    std::array<HostAllocation, kDeviceCount> dst_hosts;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        auto result = src_devices[index].Allocate(device_id, kCopySize);
        if (!result) { return result; }
        result = dst_hosts[index].Allocate(kCopySize);
        if (!result) { return result; }

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(79 + index * 19));
        result = CopyHostToDevice(device_id, src_devices[index].Get(), expected[index]);
        if (!result) { return result; }
    }

    std::array<void*, kDeviceCount> dsts{};
    std::array<size_t, kDeviceCount> dest_max{};
    std::array<void*, kDeviceCount> srcs{};
    std::array<size_t, kDeviceCount> sizes{};
    std::array<aclrtMemcpyBatchAttr, kDeviceCount> attrs{};
    std::array<size_t, kDeviceCount> attr_indexes{};
    for (int index = 0; index < kDeviceCount; ++index) {
        dsts[index] = dst_hosts[index].Get();
        dest_max[index] = kCopySize;
        srcs[index] = src_devices[index].Get();
        sizes[index] = kCopySize;
        attr_indexes[index] = static_cast<size_t>(index);
        attrs[index] = {};
        attrs[index].dstLoc.id = 0;
        attrs[index].dstLoc.type = ACL_MEM_LOCATION_TYPE_HOST;
        attrs[index].srcLoc.id = static_cast<uint32_t>(device_ids[index]);
        attrs[index].srcLoc.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    }

    timings.Start("memcpy_batch");
    size_t fail_index = std::numeric_limits<size_t>::max();
    const auto ret = aclrtMemcpyBatch(dsts.data(), dest_max.data(), srcs.data(), sizes.data(),
                                      kDeviceCount, attrs.data(), attr_indexes.data(),
                                      kDeviceCount, &fail_index);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpyBatch(D2H) failed, ret=" << static_cast<int>(ret)
               << ", fail_index=" << fail_index;
    }

    timings.Start("verify");
    for (int index = 0; index < kDeviceCount; ++index) {
        auto result = HostEquals(device_ids[index], dst_hosts[index].Get(), expected[index]);
        if (!result) { return result; }
    }
    timings.Stop();
    return ::testing::AssertionSuccess();
}

class FftsTransportE2ETest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const auto init_ret = aclInit(nullptr);
        ASSERT_TRUE(init_ret == ACL_SUCCESS || init_ret == ACL_ERROR_REPEAT_INITIALIZE)
            << "aclInit failed: " << static_cast<int>(init_ret);

        uint32_t device_count = 0;
        ASSERT_EQ(aclrtGetDeviceCount(&device_count), ACL_SUCCESS);
        ASSERT_GT(device_count, 0U);
    }

    static void TearDownTestSuite() { (void)aclFinalize(); }
};

TEST_F(FftsTransportE2ETest, DeviceToDeviceCopiesMoveBytesOnEightDevices)
{
    PhaseTimings timings("D2D ffts_transport");

    timings.Start("capability_check");
    std::vector<int> device_ids;
    const auto device_result = CollectFftsDeviceIds(device_ids);
    if (!device_result) { GTEST_SKIP() << device_result.message(); }

    ASSERT_TRUE(RunFftsDeviceToDevice(device_ids, timings));
    timings.Print();
}

TEST_F(FftsTransportE2ETest, HostToDeviceCopiesBenchmarkAgainstAclrtMemcpyBatch)
{
    PhaseTimings ffts_timings("H2D ffts_transport");

    ffts_timings.Start("capability_check");
    std::vector<int> device_ids;
    const auto device_result = CollectFftsDeviceIds(device_ids);
    if (!device_result) { GTEST_SKIP() << device_result.message(); }

    ASSERT_TRUE(RunFftsHostToDevice(device_ids, ffts_timings));
    ffts_timings.Print();

    PhaseTimings batch_timings("H2D aclrtMemcpyBatch");
    ASSERT_TRUE(RunAclMemcpyBatchHostToDevice(device_ids, batch_timings));
    batch_timings.Print();

    PrintBenchmarkComparison("H2D", ffts_timings, batch_timings);
}

TEST_F(FftsTransportE2ETest, DeviceToHostCopiesBenchmarkAgainstAclrtMemcpyBatch)
{
    PhaseTimings ffts_timings("D2H ffts_transport");

    ffts_timings.Start("capability_check");
    std::vector<int> device_ids;
    const auto device_result = CollectFftsDeviceIds(device_ids);
    if (!device_result) { GTEST_SKIP() << device_result.message(); }

    ASSERT_TRUE(RunFftsDeviceToHost(device_ids, ffts_timings));
    ffts_timings.Print();

    PhaseTimings batch_timings("D2H aclrtMemcpyBatch");
    ASSERT_TRUE(RunAclMemcpyBatchDeviceToHost(device_ids, batch_timings));
    batch_timings.Print();

    PrintBenchmarkComparison("D2H", ffts_timings, batch_timings);
}

}  // namespace
}  // namespace transport
