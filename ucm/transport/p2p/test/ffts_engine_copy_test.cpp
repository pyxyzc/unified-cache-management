#include "core/ffts_engine.h"
#include <acl/acl.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

namespace transport {
namespace {

constexpr int kDeviceId = 0;
constexpr size_t kCopySize = 4096;

struct DeviceDeleter {
    void operator()(void* ptr) const
    {
        if (ptr != nullptr) { (void)aclrtFree(ptr); }
    }
};

using DevicePtr = std::unique_ptr<void, DeviceDeleter>;

std::vector<std::uint8_t> MakePattern(size_t size)
{
    std::vector<std::uint8_t> pattern(size);
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>((i * 131U + 17U) & 0xffU);
    }
    return pattern;
}

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
    void* src_raw = nullptr;
    ASSERT_EQ(aclrtMalloc(&src_raw, kCopySize, ACL_MEM_TYPE_HIGH_BAND_WIDTH), ACL_SUCCESS);
    DevicePtr src_device(src_raw);

    void* dst_raw = nullptr;
    ASSERT_EQ(aclrtMalloc(&dst_raw, kCopySize, ACL_MEM_TYPE_HIGH_BAND_WIDTH), ACL_SUCCESS);
    DevicePtr dst_device(dst_raw);

    const auto expected = MakePattern(kCopySize);
    const std::vector<std::uint8_t> zeros(kCopySize, 0);
    std::vector<std::uint8_t> actual(kCopySize, 0);

    ASSERT_EQ(aclrtMemcpy(src_device.get(), kCopySize, expected.data(), expected.size(),
                          ACL_MEMCPY_HOST_TO_DEVICE),
              ACL_SUCCESS);
    ASSERT_EQ(aclrtMemcpy(dst_device.get(), kCopySize, zeros.data(), zeros.size(),
                          ACL_MEMCPY_HOST_TO_DEVICE),
              ACL_SUCCESS);

    FftsEngine engine;
    FftsEngineOptions options;
    options.device_id = kDeviceId;
    options.max_ready_lanes = 8;
    ASSERT_EQ(Status::Ok, engine.Init(options));

    EXPECT_EQ(Status::Ok,
              engine.Submit({FftsCopySpec{dst_device.get(), src_device.get(), kCopySize}}));
    EXPECT_EQ(Status::Ok, engine.Shutdown());

    ASSERT_EQ(aclrtMemcpy(actual.data(), actual.size(), dst_device.get(), kCopySize,
                          ACL_MEMCPY_DEVICE_TO_HOST),
              ACL_SUCCESS);
    EXPECT_EQ(expected, actual);
}

}  // namespace
}  // namespace transport
