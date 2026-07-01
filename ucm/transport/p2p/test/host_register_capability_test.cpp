#include <acl/acl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <gtest/gtest.h>

namespace transport {
namespace {

constexpr int kDeviceId = 0;
constexpr size_t kPageSize = 4096;
constexpr size_t kBufferSize = 4096;

struct FreeDeleter {
    void operator()(void* ptr) const
    {
        std::free(ptr);
    }
};

using HostBuffer = std::unique_ptr<void, FreeDeleter>;

const char* AclErrorHint(aclError ret)
{
    switch (ret) {
        case ACL_SUCCESS:
            return "ACL_SUCCESS";
        case ACL_ERROR_REPEAT_INITIALIZE:
            return "ACL_ERROR_REPEAT_INITIALIZE";
        case ACL_ERROR_RT_PARAM_INVALID:
            return "ACL_ERROR_RT_PARAM_INVALID";
        case ACL_ERROR_RT_FEATURE_NOT_SUPPORT:
            return "ACL_ERROR_RT_FEATURE_NOT_SUPPORT";
        default:
            return "unknown acl error";
    }
}

std::string CurrentSocVersion()
{
    const char* soc_version = aclrtGetSocName();
    if (soc_version == nullptr) {
        return "unknown";
    }
    return soc_version;
}

HostBuffer AllocatePageAlignedHostBuffer()
{
    void* ptr = nullptr;
    if (posix_memalign(&ptr, kPageSize, kBufferSize) != 0) {
        return HostBuffer(nullptr);
    }
    std::memset(ptr, 0xab, kBufferSize);
    return HostBuffer(ptr);
}

std::string HostRegisterFailureMessage(const char* api, uint32_t flags, aclError ret)
{
    return std::string(api) + " failed on soc=" + CurrentSocVersion() +
           ", flags=" + std::to_string(flags) + ", ret=" + std::to_string(ret) +
           "(" + AclErrorHint(ret) + ")";
}

class AclDeviceGuard {
public:
    aclError Init()
    {
        init_ret_ = aclInit(nullptr);
        if ((init_ret_ != ACL_SUCCESS) && (init_ret_ != ACL_ERROR_REPEAT_INITIALIZE)) {
            return init_ret_;
        }
        initialized_here_ = (init_ret_ == ACL_SUCCESS);

        uint32_t device_count = 0;
        auto ret = aclrtGetDeviceCount(&device_count);
        if (ret != ACL_SUCCESS) {
            return ret;
        }
        if (device_count == 0U) {
            return ACL_ERROR_RT_NO_DEVICE;
        }

        ret = aclrtSetDevice(kDeviceId);
        if (ret == ACL_SUCCESS) {
            device_set_ = true;
        }
        return ret;
    }

    ~AclDeviceGuard()
    {
        if (device_set_) {
            (void)aclrtResetDevice(kDeviceId);
        }
        if (initialized_here_) {
            (void)aclFinalize();
        }
    }

private:
    aclError init_ret_ = ACL_SUCCESS;
    bool initialized_here_ = false;
    bool device_set_ = false;
};

class HostRegisterCapabilityTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto ret = acl_.Init();
        ASSERT_TRUE((ret == ACL_SUCCESS) || (ret == ACL_ERROR_REPEAT_INITIALIZE))
            << "ACL device init failed, ret=" << static_cast<int>(ret)
            << "(" << AclErrorHint(ret) << ")";
    }

private:
    AclDeviceGuard acl_;
};

TEST_F(HostRegisterCapabilityTest, MappedHostRegisterV2IsSupported)
{
    auto host = AllocatePageAlignedHostBuffer();
    ASSERT_NE(nullptr, host.get());

    const uint32_t flags = ACL_HOST_REG_MAPPED;
    const auto register_ret = aclrtHostRegisterV2(host.get(), kBufferSize, flags);
    ASSERT_EQ(ACL_SUCCESS, register_ret)
        << HostRegisterFailureMessage("aclrtHostRegisterV2(MAPPED)", flags, register_ret);

    void* device_ptr = nullptr;
    const auto pointer_ret = aclrtHostGetDevicePointer(host.get(), &device_ptr, 0);
    EXPECT_EQ(ACL_SUCCESS, pointer_ret)
        << "aclrtHostGetDevicePointer failed on soc=" << CurrentSocVersion()
        << ", ret=" << static_cast<int>(pointer_ret) << "(" << AclErrorHint(pointer_ret)
        << ")";
    EXPECT_NE(nullptr, device_ptr);

    EXPECT_EQ(ACL_SUCCESS, aclrtHostUnregister(host.get()));
}

TEST_F(HostRegisterCapabilityTest, PinnedHostRegisterV2FlagIsAccepted)
{
    auto host = AllocatePageAlignedHostBuffer();
    ASSERT_NE(nullptr, host.get());

    const uint32_t flags = ACL_HOST_REG_PINNED;
    const auto register_ret = aclrtHostRegisterV2(host.get(), kBufferSize, flags);
    ASSERT_EQ(ACL_SUCCESS, register_ret)
        << HostRegisterFailureMessage("aclrtHostRegisterV2(PINNED)", flags, register_ret);

    EXPECT_EQ(ACL_SUCCESS, aclrtHostUnregister(host.get()));
}

TEST_F(HostRegisterCapabilityTest, MappedPinnedHostRegisterV2IsSupported)
{
    auto host = AllocatePageAlignedHostBuffer();
    ASSERT_NE(nullptr, host.get());

    const uint32_t flags = ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED;
    const auto register_ret = aclrtHostRegisterV2(host.get(), kBufferSize, flags);
    ASSERT_EQ(ACL_SUCCESS, register_ret)
        << HostRegisterFailureMessage("aclrtHostRegisterV2(MAPPED|PINNED)", flags,
                                      register_ret);

    void* device_ptr = nullptr;
    const auto pointer_ret = aclrtHostGetDevicePointer(host.get(), &device_ptr, 0);
    EXPECT_EQ(ACL_SUCCESS, pointer_ret)
        << "aclrtHostGetDevicePointer failed on soc=" << CurrentSocVersion()
        << ", ret=" << static_cast<int>(pointer_ret) << "(" << AclErrorHint(pointer_ret)
        << ")";
    EXPECT_NE(nullptr, device_ptr);

    EXPECT_EQ(ACL_SUCCESS, aclrtHostUnregister(host.get()));
}

}  // namespace
}  // namespace transport
