#include "core/ffts_transport.h"
#include <acl/acl.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <runtime/config.h>
#include <runtime/dev.h>
#include <gtest/gtest.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace transport {
namespace {

constexpr int kDeviceId = 0;
constexpr size_t kPageSize = 4096;
constexpr size_t kCopySize = 4096;
constexpr uint16_t kDefaultReadyLanes = 8;

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

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
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

::testing::AssertionResult FftsPlusAvailable()
{
    const auto soc_name = CurrentSocName();
    int ffts_mode = RT_MODE_NO_FFTS;
    const auto cap_ret = rtGetDeviceCapability(kDeviceId, RT_MODULE_TYPE_TSCPU,
                                               FEATURE_TYPE_FFTS_MODE, &ffts_mode);
    if (cap_ret != 0) {
        return ::testing::AssertionFailure()
               << "Cannot query FFTS mode on soc=" << soc_name
               << ", rtGetDeviceCapability ret=" << static_cast<int>(cap_ret);
    }
    if (ffts_mode != RT_MODE_FFTS_PLUS) {
        return ::testing::AssertionFailure()
               << "FFTS plus is not available on soc=" << soc_name
               << ", mode=" << FftsModeName(ffts_mode) << "(" << ffts_mode << ")";
    }
    return ::testing::AssertionSuccess();
}

struct DeviceDeleter {
    void operator()(void* ptr) const
    {
        if (ptr != nullptr) { (void)aclrtFree(ptr); }
    }
};

struct HostDeleter {
    void operator()(void* ptr) const
    {
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }
};

using DevicePtr = std::unique_ptr<void, DeviceDeleter>;
using HostPtr = std::unique_ptr<void, HostDeleter>;

size_t RoundUpToPage(size_t size)
{
    return ((size + kPageSize - 1) / kPageSize) * kPageSize;
}

::testing::AssertionResult AllocateDevice(size_t size, DevicePtr& out)
{
    void* raw = nullptr;
    const auto ret = aclrtMalloc(&raw, size, ACL_MEM_TYPE_HIGH_BAND_WIDTH);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMalloc failed, ret=" << static_cast<int>(ret);
    }
    out.reset(raw);
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult AllocatePageAlignedHost(size_t size, HostPtr& out)
{
    const size_t aligned_size = RoundUpToPage(size);
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
    out.reset(raw);
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

::testing::AssertionResult CopyHostToDevice(void* device,
                                            const std::vector<std::uint8_t>& data)
{
    const auto ret = aclrtMemcpy(device, data.size(), data.data(), data.size(),
                                 ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpy(H2D) failed, ret=" << static_cast<int>(ret);
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult ZeroDevice(void* device, size_t size)
{
    const std::vector<std::uint8_t> zeros(size, 0);
    return CopyHostToDevice(device, zeros);
}

void CopyPatternToHost(void* host, const std::vector<std::uint8_t>& data)
{
    std::memcpy(host, data.data(), data.size());
}

::testing::AssertionResult DeviceEquals(void* device,
                                        const std::vector<std::uint8_t>& expected)
{
    std::vector<std::uint8_t> actual(expected.size(), 0);
    const auto ret = aclrtMemcpy(actual.data(), actual.size(), device, expected.size(),
                                 ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_SUCCESS) {
        return ::testing::AssertionFailure()
               << "aclrtMemcpy(D2H) failed, ret=" << static_cast<int>(ret);
    }
    if (actual == expected) { return ::testing::AssertionSuccess(); }

    size_t mismatch = 0;
    while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) {
        ++mismatch;
    }
    return ::testing::AssertionFailure()
           << "device buffer mismatch at offset=" << mismatch
           << ", expected=" << static_cast<int>(expected[mismatch])
           << ", actual=" << static_cast<int>(actual[mismatch]);
}

::testing::AssertionResult HostEquals(const void* host,
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
           << "host buffer mismatch at offset=" << mismatch
           << ", expected=" << static_cast<int>(expected[mismatch])
           << ", actual=" << static_cast<int>(actual[mismatch]);
}

::testing::AssertionResult InitTransport(FftsTransport& transport,
                                         uint16_t ready_lanes = kDefaultReadyLanes)
{
    FftsInitAttrs attrs;
    attrs.device_ids = {kDeviceId};
    attrs.max_ready_lanes = ready_lanes;
    const auto status = transport.Init(attrs);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsTransport::Init failed: " << StatusName(status);
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult RegisterDeviceRegion(FftsTransport& transport, void* device,
                                                size_t size, MemoryHandle& handle)
{
    const auto status =
        transport.RegisterMemory(MemoryRegion{device, size, MemoryType::Device, kDeviceId},
                                 handle);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Device) failed: " << StatusName(status);
    }
    if (handle == kInvalidMemoryHandle) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Device) returned invalid handle";
    }
    return ::testing::AssertionSuccess();
}

Status RegisterHostRegion(FftsTransport& transport, void* host, size_t size,
                          MemoryHandle& handle)
{
    return transport.RegisterMemory(MemoryRegion{host, size, MemoryType::Host, -1}, handle);
}

::testing::AssertionResult HostRegistrationSucceeded(Status status, MemoryHandle handle)
{
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Host) failed on soc=" << CurrentSocName()
               << ", status=" << StatusName(status);
    }
    if (handle == kInvalidMemoryHandle) {
        return ::testing::AssertionFailure()
               << "RegisterMemory(Host) returned invalid handle";
    }
    return ::testing::AssertionSuccess();
}

Segment SegmentFor(void* local_addr, const void* remote_addr, uint64_t length)
{
    return Segment{local_addr, PtrToU64(remote_addr), length};
}

class FftsTransportCopyTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const auto init_ret = aclInit(nullptr);
        ASSERT_TRUE(init_ret == ACL_SUCCESS || init_ret == ACL_ERROR_REPEAT_INITIALIZE)
            << "aclInit failed: " << static_cast<int>(init_ret);

        uint32_t device_count = 0;
        ASSERT_EQ(aclrtGetDeviceCount(&device_count), ACL_SUCCESS);
        ASSERT_GT(device_count, 0U);
        ASSERT_EQ(aclrtSetDevice(kDeviceId), ACL_SUCCESS);
    }

    static void TearDownTestSuite()
    {
        (void)aclrtResetDevice(kDeviceId);
        (void)aclFinalize();
    }
};

TEST_F(FftsTransportCopyTest, DeviceToDeviceWriteMovesBytes)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    DevicePtr src_device;
    DevicePtr dst_device;
    ASSERT_TRUE(AllocateDevice(kCopySize, src_device));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst_device));

    const auto expected = MakePattern(kCopySize, 17);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, src_device.get(), kCopySize, src_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst_device.get(), kCopySize, dst_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(SegmentFor(src_device.get(), dst_device.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, DeviceToDeviceReadMovesBytes)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    DevicePtr src_device;
    DevicePtr dst_device;
    ASSERT_TRUE(AllocateDevice(kCopySize, src_device));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst_device));

    const auto expected = MakePattern(kCopySize, 29);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, src_device.get(), kCopySize, src_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst_device.get(), kCopySize, dst_handle));

    Operation op;
    op.opcode = Opcode::Read;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(SegmentFor(dst_device.get(), src_device.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, HostToDeviceWriteMovesBytes)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    HostPtr src_host;
    DevicePtr dst_device;
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, src_host));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst_device));

    const auto expected = MakePattern(kCopySize, 43);
    CopyPatternToHost(src_host.get(), expected);
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle host_handle = kInvalidMemoryHandle;
    MemoryHandle device_handle = kInvalidMemoryHandle;
    const auto host_status =
        RegisterHostRegion(transport, src_host.get(), kCopySize, host_handle);
    if (host_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(host_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(host_status, host_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst_device.get(), kCopySize, device_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceHost;
    op.ops.push_back(SegmentFor(src_host.get(), dst_device.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(device_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(host_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, DeviceToHostReadMovesBytes)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    DevicePtr src_device;
    HostPtr dst_host;
    ASSERT_TRUE(AllocateDevice(kCopySize, src_device));
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, dst_host));

    const auto expected = MakePattern(kCopySize, 57);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle device_handle = kInvalidMemoryHandle;
    MemoryHandle host_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, src_device.get(), kCopySize, device_handle));
    const auto host_status =
        RegisterHostRegion(transport, dst_host.get(), kCopySize, host_handle);
    if (host_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(host_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(host_status, host_handle));

    Operation op;
    op.opcode = Opcode::Read;
    op.direct = OperationDirect::LocalDeviceHost;
    op.ops.push_back(SegmentFor(dst_host.get(), src_device.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(HostEquals(dst_host.get(), expected));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(host_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(device_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, LocalDeviceHostWriteCanMoveBothDirectionsInOneBatch)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    HostPtr h2d_src_host;
    DevicePtr h2d_dst_device;
    DevicePtr d2h_src_device;
    HostPtr d2h_dst_host;
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, h2d_src_host));
    ASSERT_TRUE(AllocateDevice(kCopySize, h2d_dst_device));
    ASSERT_TRUE(AllocateDevice(kCopySize, d2h_src_device));
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, d2h_dst_host));

    const auto h2d_expected = MakePattern(kCopySize, 71);
    const auto d2h_expected = MakePattern(kCopySize, 89);
    CopyPatternToHost(h2d_src_host.get(), h2d_expected);
    ASSERT_TRUE(ZeroDevice(h2d_dst_device.get(), kCopySize));
    ASSERT_TRUE(CopyHostToDevice(d2h_src_device.get(), d2h_expected));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport, 2));

    MemoryHandle h2d_host_handle = kInvalidMemoryHandle;
    MemoryHandle h2d_device_handle = kInvalidMemoryHandle;
    MemoryHandle d2h_device_handle = kInvalidMemoryHandle;
    MemoryHandle d2h_host_handle = kInvalidMemoryHandle;

    const auto h2d_host_status =
        RegisterHostRegion(transport, h2d_src_host.get(), kCopySize, h2d_host_handle);
    if (h2d_host_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(h2d_host_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(h2d_host_status, h2d_host_handle));
    const auto d2h_host_status =
        RegisterHostRegion(transport, d2h_dst_host.get(), kCopySize, d2h_host_handle);
    if (d2h_host_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(d2h_host_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(d2h_host_status, d2h_host_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, h2d_dst_device.get(), kCopySize,
                                     h2d_device_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, d2h_src_device.get(), kCopySize,
                                     d2h_device_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceHost;
    op.ops.push_back(SegmentFor(h2d_src_host.get(), h2d_dst_device.get(), kCopySize));
    op.ops.push_back(SegmentFor(d2h_src_device.get(), d2h_dst_host.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(h2d_dst_device.get(), h2d_expected));
    EXPECT_TRUE(HostEquals(d2h_dst_host.get(), d2h_expected));

    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(d2h_host_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(d2h_device_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(h2d_device_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(h2d_host_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, DeviceDeviceBatchMovesMultipleSegments)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    DevicePtr src0;
    DevicePtr dst0;
    DevicePtr src1;
    DevicePtr dst1;
    ASSERT_TRUE(AllocateDevice(kCopySize, src0));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst0));
    ASSERT_TRUE(AllocateDevice(kCopySize, src1));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst1));

    const auto expected0 = MakePattern(kCopySize, 103);
    const auto expected1 = MakePattern(kCopySize, 127);
    ASSERT_TRUE(CopyHostToDevice(src0.get(), expected0));
    ASSERT_TRUE(CopyHostToDevice(src1.get(), expected1));
    ASSERT_TRUE(ZeroDevice(dst0.get(), kCopySize));
    ASSERT_TRUE(ZeroDevice(dst1.get(), kCopySize));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport, 2));

    MemoryHandle src0_handle = kInvalidMemoryHandle;
    MemoryHandle dst0_handle = kInvalidMemoryHandle;
    MemoryHandle src1_handle = kInvalidMemoryHandle;
    MemoryHandle dst1_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, src0.get(), kCopySize, src0_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst0.get(), kCopySize, dst0_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, src1.get(), kCopySize, src1_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst1.get(), kCopySize, dst1_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(SegmentFor(src0.get(), dst0.get(), kCopySize));
    op.ops.push_back(SegmentFor(src1.get(), dst1.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(dst0.get(), expected0));
    EXPECT_TRUE(DeviceEquals(dst1.get(), expected1));

    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst1_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src1_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst0_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src0_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, ExecuteRejectsInvalidCopyRequests)
{
    DevicePtr registered_src;
    DevicePtr registered_dst;
    DevicePtr unregistered;
    HostPtr host0;
    HostPtr host1;
    ASSERT_TRUE(AllocateDevice(kCopySize, registered_src));
    ASSERT_TRUE(AllocateDevice(kCopySize, registered_dst));
    ASSERT_TRUE(AllocateDevice(kCopySize, unregistered));
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, host0));
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, host1));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, registered_src.get(), kCopySize, src_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, registered_dst.get(), kCopySize, dst_handle));

    Operation empty;
    empty.opcode = Opcode::Write;
    empty.direct = OperationDirect::LocalDeviceDevice;
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(empty));

    Operation unsupported;
    unsupported.opcode = Opcode::Write;
    unsupported.direct = OperationDirect::RemoteDeviceHost;
    unsupported.ops.push_back(SegmentFor(registered_src.get(), registered_dst.get(), 1));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(unsupported));

    Operation null_local;
    null_local.opcode = Opcode::Write;
    null_local.direct = OperationDirect::LocalDeviceDevice;
    null_local.ops.push_back(Segment{nullptr, PtrToU64(registered_dst.get()), 1});
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(null_local));

    Operation zero_length;
    zero_length.opcode = Opcode::Write;
    zero_length.direct = OperationDirect::LocalDeviceDevice;
    zero_length.ops.push_back(SegmentFor(registered_src.get(), registered_dst.get(), 0));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(zero_length));

    Operation unregistered_addr;
    unregistered_addr.opcode = Opcode::Write;
    unregistered_addr.direct = OperationDirect::LocalDeviceDevice;
    unregistered_addr.ops.push_back(SegmentFor(unregistered.get(), registered_dst.get(), 1));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(unregistered_addr));

    Operation out_of_bounds;
    out_of_bounds.opcode = Opcode::Write;
    out_of_bounds.direct = OperationDirect::LocalDeviceDevice;
    out_of_bounds.ops.push_back(
        SegmentFor(registered_src.get(), registered_dst.get(), kCopySize + 1));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(out_of_bounds));

    MemoryHandle host0_handle = kInvalidMemoryHandle;
    MemoryHandle host1_handle = kInvalidMemoryHandle;
    const auto host0_status =
        RegisterHostRegion(transport, host0.get(), kCopySize, host0_handle);
    if (host0_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(host0_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(host0_status, host0_handle));
    const auto host1_status =
        RegisterHostRegion(transport, host1.get(), kCopySize, host1_handle);
    if (host1_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(host1_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(host1_status, host1_handle));

    Operation host_host;
    host_host.opcode = Opcode::Write;
    host_host.direct = OperationDirect::LocalDeviceHost;
    host_host.ops.push_back(SegmentFor(host0.get(), host1.get(), 1));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(host_host));

    Operation host_device_with_device_device_direct;
    host_device_with_device_device_direct.opcode = Opcode::Write;
    host_device_with_device_device_direct.direct = OperationDirect::LocalDeviceDevice;
    host_device_with_device_device_direct.ops.push_back(
        SegmentFor(host0.get(), registered_dst.get(), 1));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(host_device_with_device_device_direct));

    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(host1_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(host0_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handle));
    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(src_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, UnregisterMakesMemoryUnavailableForExecute)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    DevicePtr src_device;
    DevicePtr dst_device;
    ASSERT_TRUE(AllocateDevice(kCopySize, src_device));
    ASSERT_TRUE(AllocateDevice(kCopySize, dst_device));

    const auto expected = MakePattern(kCopySize, 149);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, src_device.get(), kCopySize, src_handle));
    ASSERT_TRUE(RegisterDeviceRegion(transport, dst_device.get(), kCopySize, dst_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(SegmentFor(src_device.get(), dst_device.get(), kCopySize));

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));

    ASSERT_EQ(Status::Ok, transport.UnregisterMemory(src_handle));
    EXPECT_EQ(Status::InvalidArgument, transport.Execute(op));

    EXPECT_EQ(Status::Ok, transport.UnregisterMemory(dst_handle));
    EXPECT_EQ(Status::Ok, transport.Shutdown());
}

TEST_F(FftsTransportCopyTest, ShutdownReleasesRegisteredMemoryAndIsIdempotent)
{
    HostPtr host;
    DevicePtr device;
    ASSERT_TRUE(AllocatePageAlignedHost(kCopySize, host));
    ASSERT_TRUE(AllocateDevice(kCopySize, device));

    FftsTransport transport;
    ASSERT_TRUE(InitTransport(transport));

    MemoryHandle host_handle = kInvalidMemoryHandle;
    MemoryHandle device_handle = kInvalidMemoryHandle;
    ASSERT_TRUE(RegisterDeviceRegion(transport, device.get(), kCopySize, device_handle));
    const auto host_status = RegisterHostRegion(transport, host.get(), kCopySize, host_handle);
    if (host_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterMemory(Host) status=" << StatusName(host_status);
    }
    ASSERT_TRUE(HostRegistrationSucceeded(host_status, host_handle));

    EXPECT_EQ(Status::Ok, transport.Shutdown());
    EXPECT_EQ(Status::Ok, transport.Shutdown());

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceHost;
    op.ops.push_back(SegmentFor(host.get(), device.get(), 1));
    EXPECT_EQ(Status::Failed, transport.Execute(op));

    MemoryHandle after_shutdown_handle = kInvalidMemoryHandle;
    EXPECT_EQ(Status::Failed,
              transport.RegisterMemory(
                  MemoryRegion{device.get(), kCopySize, MemoryType::Device, kDeviceId},
                  after_shutdown_handle));
    EXPECT_EQ(kInvalidMemoryHandle, after_shutdown_handle);
}

}  // namespace
}  // namespace transport
