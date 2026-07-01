#include "core/ffts_transport.h"
#include <acl/acl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <runtime/config.h>
#include <runtime/dev.h>
#include <gtest/gtest.h>

namespace transport {
namespace {

constexpr int kDeviceCount = 8;
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

    DeviceAllocation(DeviceAllocation&& other) noexcept
        : device_id_(other.device_id_), ptr_(other.ptr_)
    {
        other.device_id_ = -1;
        other.ptr_ = nullptr;
    }

    DeviceAllocation& operator=(DeviceAllocation&& other) noexcept
    {
        if (this == &other) { return *this; }
        Reset();
        device_id_ = other.device_id_;
        ptr_ = other.ptr_;
        other.device_id_ = -1;
        other.ptr_ = nullptr;
        return *this;
    }

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

    static void TearDownTestSuite()
    {
        for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
            (void)aclrtResetDevice(device_id);
        }
        (void)aclFinalize();
    }
};

TEST_F(FftsTransportE2ETest, ConcurrentDeviceToDeviceCopiesMoveBytesOnEightDevices)
{
    uint32_t available_devices = 0;
    ASSERT_EQ(aclrtGetDeviceCount(&available_devices), ACL_SUCCESS);
    if (available_devices < static_cast<uint32_t>(kDeviceCount)) {
        GTEST_SKIP() << "Need " << kDeviceCount << " devices for FFTS transport E2E, found "
                     << available_devices;
    }

    std::vector<int> device_ids;
    device_ids.reserve(kDeviceCount);
    for (int device_id = 0; device_id < kDeviceCount; ++device_id) {
        const auto ffts_plus = FftsPlusAvailable(device_id);
        if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }
        device_ids.push_back(device_id);
    }

    std::array<DeviceAllocation, kDeviceCount> src_devices;
    std::array<DeviceAllocation, kDeviceCount> dst_devices;
    std::array<std::vector<std::uint8_t>, kDeviceCount> expected;
    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        ASSERT_TRUE(src_devices[index].Allocate(device_id, kCopySize));
        ASSERT_TRUE(dst_devices[index].Allocate(device_id, kCopySize));

        expected[index] = MakePattern(kCopySize, static_cast<std::uint8_t>(17 + index * 19));
        ASSERT_TRUE(CopyHostToDevice(device_id, src_devices[index].Get(), expected[index]));
        ASSERT_TRUE(ZeroDevice(device_id, dst_devices[index].Get(), kCopySize));
    }

    FftsTransport transport;
    FftsInitAttrs attrs;
    attrs.device_ids = device_ids;
    attrs.max_ready_lanes = kMaxReadyLanes;
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<MemoryHandle, kDeviceCount> src_handles{};
    std::array<MemoryHandle, kDeviceCount> dst_handles{};
    std::fill(src_handles.begin(), src_handles.end(), kInvalidMemoryHandle);
    std::fill(dst_handles.begin(), dst_handles.end(), kInvalidMemoryHandle);

    for (int index = 0; index < kDeviceCount; ++index) {
        const auto device_id = device_ids[index];
        ASSERT_TRUE(RegisterDeviceRegion(transport, device_id, src_devices[index].Get(),
                                         kCopySize, src_handles[index]));
        ASSERT_TRUE(RegisterDeviceRegion(transport, device_id, dst_devices[index].Get(),
                                         kCopySize, dst_handles[index]));
    }

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
        EXPECT_EQ(Status::Ok, results[index])
            << "Execute failed on device=" << device_ids[index]
            << ", status=" << StatusName(results[index]);
    }

    for (int index = 0; index < kDeviceCount; ++index) {
        EXPECT_TRUE(DeviceEquals(device_ids[index], dst_devices[index].Get(), expected[index]));
    }

    for (int index = 0; index < kDeviceCount; ++index) {
        EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handles[index]));
        EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src_handles[index]));
    }
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

}  // namespace
}  // namespace transport
