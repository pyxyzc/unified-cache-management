#include "core/ffts_engine.h"
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

std::string CurrentSocName()
{
    const char* soc_name = aclrtGetSocName();
    if (soc_name == nullptr) { return "unknown"; }
    return soc_name;
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

DevicePtr AllocateDevice(size_t size)
{
    void* raw = nullptr;
    const auto ret = aclrtMalloc(&raw, size, ACL_MEM_TYPE_HIGH_BAND_WIDTH);
    EXPECT_EQ(ACL_SUCCESS, ret) << "aclrtMalloc failed, ret=" << static_cast<int>(ret);
    return DevicePtr(raw);
}

HostPtr AllocatePageAlignedHost(size_t size)
{
    const size_t aligned_size = RoundUpToPage(size);
    void* raw = nullptr;
#if defined(_WIN32)
    raw = _aligned_malloc(aligned_size, kPageSize);
    if (raw == nullptr) { return HostPtr(nullptr); }
#else
    if (posix_memalign(&raw, kPageSize, aligned_size) != 0) {
        return HostPtr(nullptr);
    }
#endif
    std::memset(raw, 0, aligned_size);
    return HostPtr(raw);
}

std::vector<std::uint8_t> MakePattern(size_t size, std::uint8_t seed)
{
    std::vector<std::uint8_t> pattern(size);
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>((i * 131U + seed) & 0xffU);
    }
    return pattern;
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

::testing::AssertionResult InitEngine(FftsEngine& engine,
                                      uint16_t ready_lanes = kDefaultReadyLanes)
{
    FftsEngineOptions options;
    options.device_id = kDeviceId;
    options.max_ready_lanes = ready_lanes;
    const auto status = engine.Init(options);
    if (status != Status::Ok) {
        return ::testing::AssertionFailure()
               << "FftsEngine::Init failed: " << StatusName(status);
    }
    return ::testing::AssertionSuccess();
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

void CopyPatternToHost(void* host, const std::vector<std::uint8_t>& data)
{
    std::memcpy(host, data.data(), data.size());
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

class HostRegistrationGuard {
public:
    HostRegistrationGuard(FftsEngine& engine, FftsMemoryRegistration registration)
        : engine_(&engine), registration_(registration)
    {
    }

    ~HostRegistrationGuard() { (void)Reset(); }

    HostRegistrationGuard(const HostRegistrationGuard&) = delete;
    HostRegistrationGuard& operator=(const HostRegistrationGuard&) = delete;

    const FftsMemoryRegistration& Get() const noexcept { return registration_; }

    Status Reset()
    {
        if (engine_ == nullptr || !registration_.requires_unregister) {
            registration_ = {};
            return Status::Ok;
        }
        const auto status = engine_->UnregisterHostMemory(registration_);
        registration_ = {};
        return status;
    }

private:
    FftsEngine* engine_;
    FftsMemoryRegistration registration_;
};

class FftsEngineCopyTest : public ::testing::Test {
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

TEST_F(FftsEngineCopyTest, DeviceToDeviceCopyMovesBytes)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    auto src_device = AllocateDevice(kCopySize);
    auto dst_device = AllocateDevice(kCopySize);
    ASSERT_NE(nullptr, src_device.get());
    ASSERT_NE(nullptr, dst_device.get());

    const auto expected = MakePattern(kCopySize, 17);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    const auto submit_status =
        engine.Submit({FftsCopySpec{dst_device.get(), src_device.get(), kCopySize}});
    ASSERT_EQ(Status::Ok, submit_status) << "FftsEngine::Submit failed: "
                                         << StatusName(submit_status);
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, HostToDeviceCopyMovesBytesWhenHostMappingIsAvailable)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    auto src_host = AllocatePageAlignedHost(kCopySize);
    auto dst_device = AllocateDevice(kCopySize);
    ASSERT_NE(nullptr, src_host.get());
    ASSERT_NE(nullptr, dst_device.get());

    const auto expected = MakePattern(kCopySize, 29);
    CopyPatternToHost(src_host.get(), expected);
    ASSERT_TRUE(ZeroDevice(dst_device.get(), kCopySize));

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    FftsMemoryRegistration src_registration;
    const auto register_status =
        engine.RegisterHostMemory(src_host.get(), kCopySize, src_registration);
    if (register_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterHostMemory status=" << StatusName(register_status);
    }
    HostRegistrationGuard src_guard(engine, src_registration);

    const auto submit_status = engine.Submit(
        {FftsCopySpec{dst_device.get(), src_guard.Get().ffts_addr, kCopySize}});
    ASSERT_EQ(Status::Ok, submit_status) << "FftsEngine::Submit failed: "
                                         << StatusName(submit_status);
    EXPECT_TRUE(DeviceEquals(dst_device.get(), expected));
    EXPECT_EQ(Status::Ok, src_guard.Reset());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, DeviceToHostCopyMovesBytesWhenHostMappingIsAvailable)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    auto src_device = AllocateDevice(kCopySize);
    auto dst_host = AllocatePageAlignedHost(kCopySize);
    ASSERT_NE(nullptr, src_device.get());
    ASSERT_NE(nullptr, dst_host.get());

    const auto expected = MakePattern(kCopySize, 43);
    ASSERT_TRUE(CopyHostToDevice(src_device.get(), expected));

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    FftsMemoryRegistration dst_registration;
    const auto register_status =
        engine.RegisterHostMemory(dst_host.get(), kCopySize, dst_registration);
    if (register_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterHostMemory status=" << StatusName(register_status);
    }
    HostRegistrationGuard dst_guard(engine, dst_registration);

    const auto submit_status = engine.Submit(
        {FftsCopySpec{dst_guard.Get().ffts_addr, src_device.get(), kCopySize}});
    ASSERT_EQ(Status::Ok, submit_status) << "FftsEngine::Submit failed: "
                                         << StatusName(submit_status);
    EXPECT_TRUE(HostEquals(dst_host.get(), expected));
    EXPECT_EQ(Status::Ok, dst_guard.Reset());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, MixedBatchCopiesMoveBytesAcrossDirections)
{
    const auto ffts_plus = FftsPlusAvailable();
    if (!ffts_plus) { GTEST_SKIP() << ffts_plus.message(); }

    auto d2d_src = AllocateDevice(kCopySize);
    auto d2d_dst = AllocateDevice(kCopySize);
    auto h2d_dst = AllocateDevice(kCopySize);
    auto d2h_src = AllocateDevice(kCopySize);
    auto chained_src = AllocateDevice(kCopySize);
    auto chained_dst = AllocateDevice(kCopySize);
    auto h2d_src = AllocatePageAlignedHost(kCopySize);
    auto d2h_dst = AllocatePageAlignedHost(kCopySize);

    ASSERT_NE(nullptr, d2d_src.get());
    ASSERT_NE(nullptr, d2d_dst.get());
    ASSERT_NE(nullptr, h2d_dst.get());
    ASSERT_NE(nullptr, d2h_src.get());
    ASSERT_NE(nullptr, chained_src.get());
    ASSERT_NE(nullptr, chained_dst.get());
    ASSERT_NE(nullptr, h2d_src.get());
    ASSERT_NE(nullptr, d2h_dst.get());

    const auto d2d_expected = MakePattern(kCopySize, 57);
    const auto h2d_expected = MakePattern(kCopySize, 71);
    const auto d2h_expected = MakePattern(kCopySize, 89);
    const auto chained_expected = MakePattern(kCopySize, 103);

    ASSERT_TRUE(CopyHostToDevice(d2d_src.get(), d2d_expected));
    ASSERT_TRUE(CopyHostToDevice(d2h_src.get(), d2h_expected));
    ASSERT_TRUE(CopyHostToDevice(chained_src.get(), chained_expected));
    CopyPatternToHost(h2d_src.get(), h2d_expected);
    ASSERT_TRUE(ZeroDevice(d2d_dst.get(), kCopySize));
    ASSERT_TRUE(ZeroDevice(h2d_dst.get(), kCopySize));
    ASSERT_TRUE(ZeroDevice(chained_dst.get(), kCopySize));

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine, 2));

    FftsMemoryRegistration h2d_src_registration;
    const auto h2d_register_status =
        engine.RegisterHostMemory(h2d_src.get(), kCopySize, h2d_src_registration);
    if (h2d_register_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterHostMemory status=" << StatusName(h2d_register_status);
    }
    HostRegistrationGuard h2d_src_guard(engine, h2d_src_registration);

    FftsMemoryRegistration d2h_dst_registration;
    const auto d2h_register_status =
        engine.RegisterHostMemory(d2h_dst.get(), kCopySize, d2h_dst_registration);
    if (d2h_register_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterHostMemory status=" << StatusName(d2h_register_status);
    }
    HostRegistrationGuard d2h_dst_guard(engine, d2h_dst_registration);

    const std::vector<FftsCopySpec> copies = {
        FftsCopySpec{d2d_dst.get(), d2d_src.get(), kCopySize},
        FftsCopySpec{h2d_dst.get(), h2d_src_guard.Get().ffts_addr, kCopySize},
        FftsCopySpec{d2h_dst_guard.Get().ffts_addr, d2h_src.get(), kCopySize},
        FftsCopySpec{chained_dst.get(), chained_src.get(), kCopySize},
    };
    const auto submit_status = engine.Submit(copies);
    ASSERT_EQ(Status::Ok, submit_status) << "FftsEngine::Submit failed: "
                                         << StatusName(submit_status);

    EXPECT_TRUE(DeviceEquals(d2d_dst.get(), d2d_expected));
    EXPECT_TRUE(DeviceEquals(h2d_dst.get(), h2d_expected));
    EXPECT_TRUE(HostEquals(d2h_dst.get(), d2h_expected));
    EXPECT_TRUE(DeviceEquals(chained_dst.get(), chained_expected));

    EXPECT_EQ(Status::Ok, d2h_dst_guard.Reset());
    EXPECT_EQ(Status::Ok, h2d_src_guard.Reset());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, HostMemoryRegistrationReturnsMappedPointerWhenSupported)
{
    auto host = AllocatePageAlignedHost(kCopySize);
    ASSERT_NE(nullptr, host.get());

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    FftsMemoryRegistration registration;
    const auto register_status = engine.RegisterHostMemory(host.get(), kCopySize, registration);
    if (register_status != Status::Ok) {
        GTEST_SKIP() << "Host memory mapping is unavailable on soc=" << CurrentSocName()
                     << ", RegisterHostMemory status=" << StatusName(register_status);
    }
    HostRegistrationGuard guard(engine, registration);

    EXPECT_EQ(host.get(), guard.Get().origin_addr);
    EXPECT_NE(nullptr, guard.Get().ffts_addr);
    EXPECT_EQ(kCopySize, guard.Get().size);
    EXPECT_TRUE(guard.Get().requires_unregister);

    EXPECT_EQ(Status::Ok, guard.Reset());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, DeviceMemoryRegistrationMapsDevicePointerWithoutUnregister)
{
    auto device = AllocateDevice(kCopySize);
    ASSERT_NE(nullptr, device.get());

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    FftsMemoryRegistration registration;
    ASSERT_EQ(Status::Ok,
              engine.RegisterDeviceMemory(device.get(), kCopySize, registration));
    EXPECT_EQ(device.get(), registration.origin_addr);
    EXPECT_EQ(device.get(), registration.ffts_addr);
    EXPECT_EQ(kCopySize, registration.size);
    EXPECT_FALSE(registration.requires_unregister);
    EXPECT_EQ(Status::Ok, engine.UnregisterDeviceMemory(registration));
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, RejectsInvalidSubmissionsAfterRuntimeInit)
{
    auto src_device = AllocateDevice(kCopySize);
    auto dst_device = AllocateDevice(kCopySize);
    ASSERT_NE(nullptr, src_device.get());
    ASSERT_NE(nullptr, dst_device.get());

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    EXPECT_EQ(Status::InvalidArgument, engine.Submit({}));
    EXPECT_EQ(Status::InvalidArgument,
              engine.Submit({FftsCopySpec{nullptr, src_device.get(), 1}}));
    EXPECT_EQ(Status::InvalidArgument,
              engine.Submit({FftsCopySpec{dst_device.get(), nullptr, 1}}));
    EXPECT_EQ(Status::InvalidArgument,
              engine.Submit({FftsCopySpec{dst_device.get(), src_device.get(), 0}}));
    EXPECT_EQ(Status::InvalidArgument,
              engine.Submit({FftsCopySpec{
                  dst_device.get(), src_device.get(),
                  static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1}}));
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, RejectsMalformedHostUnregisterRequests)
{
    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    FftsMemoryRegistration registration;
    registration.requires_unregister = true;
    registration.origin_addr = nullptr;
    registration.size = kCopySize;
    EXPECT_EQ(Status::InvalidArgument, engine.UnregisterHostMemory(registration));

    registration.origin_addr = reinterpret_cast<void*>(kPageSize);
    registration.size = 0;
    EXPECT_EQ(Status::InvalidArgument, engine.UnregisterHostMemory(registration));

    registration = {};
    EXPECT_EQ(Status::Ok, engine.UnregisterHostMemory(registration));
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST_F(FftsEngineCopyTest, RuntimeLifecycleHandlesSynchronizeShutdownAndPostShutdownSubmit)
{
    auto src_device = AllocateDevice(kCopySize);
    auto dst_device = AllocateDevice(kCopySize);
    ASSERT_NE(nullptr, src_device.get());
    ASSERT_NE(nullptr, dst_device.get());

    FftsEngine engine;
    ASSERT_TRUE(InitEngine(engine));

    EXPECT_EQ(Status::Ok, engine.Synchronize());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
    EXPECT_EQ(Status::Failed, engine.Synchronize());
    EXPECT_EQ(Status::Failed,
              engine.Submit({FftsCopySpec{dst_device.get(), src_device.get(), 1}}));
}

}  // namespace
}  // namespace transport
