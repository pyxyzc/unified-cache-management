#include <acl/acl.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef ACL_HOST_REG_MAPPED
#define ACL_HOST_REG_MAPPED 0x2UL
#endif

#ifndef ACL_HOST_REG_PINNED
#define ACL_HOST_REG_PINNED 0x10000000UL
#endif

namespace {

constexpr size_t kPageSize = 4096;

struct Options {
    int device0 = 0;
    int device1 = 1;
    size_t size = kPageSize;
    uint32_t flags = ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED;
};

std::string AclErrorHint(aclError ret)
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
        case ACL_ERROR_RT_NO_DEVICE:
            return "ACL_ERROR_RT_NO_DEVICE";
        case 507910:
            return "ACL_ERROR_HOST_MEMORY_ALREADY_REGISTERED";
        case 507911:
            return "ACL_ERROR_HOST_MEMORY_NOT_REGISTERED";
        default:
            return "unknown";
    }
}

std::string FormatAclRet(aclError ret)
{
    std::ostringstream os;
    os << static_cast<int64_t>(ret) << " (" << AclErrorHint(ret) << ")";
    return os.str();
}

std::string FlagsToString(uint32_t flags)
{
    std::vector<std::string> names;
    if ((flags & ACL_HOST_REG_MAPPED) != 0U) {
        names.emplace_back("ACL_HOST_REG_MAPPED");
    }
    if ((flags & ACL_HOST_REG_PINNED) != 0U) {
        names.emplace_back("ACL_HOST_REG_PINNED");
    }
    if (names.empty()) {
        return std::to_string(flags);
    }

    std::ostringstream os;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0U) {
            os << " | ";
        }
        os << names[i];
    }
    os << " (" << flags << ")";
    return os.str();
}

void PrintUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --device0 N       First Ascend device id. Default: 0\n"
        << "  --device1 N       Second Ascend device id. Default: 1\n"
        << "  --size BYTES      Host buffer size. Default: 4096\n"
        << "  --flags HEX       aclrtHostRegisterV2 flag value. Default: 0x10000002\n"
        << "  --help            Show this help.\n"
        << "\n"
        << "The default flags are ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED.\n";
}

bool ParseInt(const char* text, int* out)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 0);
    if (end == text || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool ParseSize(const char* text, size_t* out)
{
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || value == 0ULL) {
        return false;
    }
    *out = static_cast<size_t>(value);
    return true;
}

bool ParseU32(const char* text, uint32_t* out)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (end == text || *end != '\0' ||
        value > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool ParseArgs(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--device0") {
            const char* value = require_value("--device0");
            if (value == nullptr || !ParseInt(value, &options->device0)) {
                return false;
            }
        } else if (arg == "--device1") {
            const char* value = require_value("--device1");
            if (value == nullptr || !ParseInt(value, &options->device1)) {
                return false;
            }
        } else if (arg == "--size") {
            const char* value = require_value("--size");
            if (value == nullptr || !ParseSize(value, &options->size)) {
                return false;
            }
        } else if (arg == "--flags") {
            const char* value = require_value("--flags");
            if (value == nullptr || !ParseU32(value, &options->flags)) {
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

class PageAlignedHostBuffer {
public:
    explicit PageAlignedHostBuffer(size_t size) : size_(size)
    {
        if (posix_memalign(&ptr_, kPageSize, size_) == 0) {
            std::memset(ptr_, 0xab, size_);
        } else {
            ptr_ = nullptr;
        }
    }

    ~PageAlignedHostBuffer()
    {
        std::free(ptr_);
    }

    PageAlignedHostBuffer(const PageAlignedHostBuffer&) = delete;
    PageAlignedHostBuffer& operator=(const PageAlignedHostBuffer&) = delete;

    void* data() const { return ptr_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    void* ptr_ = nullptr;
    size_t size_ = 0;
};

class AclRuntime {
public:
    aclError Init()
    {
        init_ret_ = aclInit(nullptr);
        if (init_ret_ != ACL_SUCCESS && init_ret_ != ACL_ERROR_REPEAT_INITIALIZE) {
            return init_ret_;
        }
        initialized_here_ = (init_ret_ == ACL_SUCCESS);
        return ACL_SUCCESS;
    }

    aclError SetDevice(int device)
    {
        const aclError ret = aclrtSetDevice(device);
        if (ret == ACL_SUCCESS &&
            std::find(devices_set_.begin(), devices_set_.end(), device) == devices_set_.end()) {
            devices_set_.push_back(device);
        }
        return ret;
    }

    ~AclRuntime()
    {
        for (auto it = devices_set_.rbegin(); it != devices_set_.rend(); ++it) {
            (void)aclrtResetDevice(*it);
        }
        if (initialized_here_) {
            (void)aclFinalize();
        }
    }

private:
    aclError init_ret_ = ACL_SUCCESS;
    bool initialized_here_ = false;
    std::vector<int> devices_set_;
};

struct RegisterResult {
    aclError ret = ACL_SUCCESS;
    bool registered = false;
};

RegisterResult RegisterCurrentDevice(const char* label, void* host, size_t size, uint32_t flags)
{
    RegisterResult result;
    result.ret = aclrtHostRegisterV2(host, size, flags);
    result.registered = (result.ret == ACL_SUCCESS);
    std::cout << "  " << label << " register ret=" << FormatAclRet(result.ret) << "\n";

    if (result.registered && ((flags & ACL_HOST_REG_MAPPED) != 0U)) {
        void* device_ptr = nullptr;
        const aclError map_ret = aclrtHostGetDevicePointer(host, &device_ptr, 0);
        std::cout << "  " << label << " get-device-pointer ret="
                  << FormatAclRet(map_ret) << ", device_ptr=" << device_ptr << "\n";
    }
    return result;
}

bool UnregisterOnDevice(AclRuntime& runtime, int device, const char* label, void* host)
{
    const aclError set_ret = runtime.SetDevice(device);
    std::cout << "  " << label << " set-device(" << device << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }

    const aclError ret = aclrtHostUnregister(host);
    std::cout << "  " << label << " unregister ret=" << FormatAclRet(ret) << "\n";
    return ret == ACL_SUCCESS;
}

bool RunCrossDeviceWithoutUnregister(AclRuntime& runtime, const Options& options)
{
    std::cout << "\n[case1a] device " << options.device0 << " register, then device "
              << options.device1 << " register without unregister\n";

    PageAlignedHostBuffer host(options.size);
    if (!host) {
        std::cerr << "  FAIL: posix_memalign failed\n";
        return false;
    }
    std::cout << "  host=" << host.data() << ", size=" << host.size() << "\n";

    aclError set_ret = runtime.SetDevice(options.device0);
    std::cout << "  first set-device(" << options.device0 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }
    const RegisterResult first =
        RegisterCurrentDevice("first/device0", host.data(), host.size(), options.flags);
    if (!first.registered) {
        return false;
    }

    set_ret = runtime.SetDevice(options.device1);
    std::cout << "  second set-device(" << options.device1 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        UnregisterOnDevice(runtime, options.device0, "cleanup/device0", host.data());
        return false;
    }

    const RegisterResult second =
        RegisterCurrentDevice("second/device1", host.data(), host.size(), options.flags);

    if (second.registered) {
        UnregisterOnDevice(runtime, options.device1, "cleanup/device1", host.data());
    }
    UnregisterOnDevice(runtime, options.device0, "cleanup/device0", host.data());
    return true;
}

bool RunCrossDeviceAfterUnregister(AclRuntime& runtime, const Options& options)
{
    std::cout << "\n[case1b] device " << options.device0 << " register, unregister, then device "
              << options.device1 << " register same host pointer\n";

    PageAlignedHostBuffer host(options.size);
    if (!host) {
        std::cerr << "  FAIL: posix_memalign failed\n";
        return false;
    }
    std::cout << "  host=" << host.data() << ", size=" << host.size() << "\n";

    aclError set_ret = runtime.SetDevice(options.device0);
    std::cout << "  first set-device(" << options.device0 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }

    const RegisterResult first =
        RegisterCurrentDevice("first/device0", host.data(), host.size(), options.flags);
    if (!first.registered) {
        return false;
    }

    if (!UnregisterOnDevice(runtime, options.device0, "between/device0", host.data())) {
        return false;
    }

    set_ret = runtime.SetDevice(options.device1);
    std::cout << "  second set-device(" << options.device1 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }

    const RegisterResult second =
        RegisterCurrentDevice("second/device1", host.data(), host.size(), options.flags);

    if (second.registered) {
        UnregisterOnDevice(runtime, options.device1, "cleanup/device1", host.data());
    }
    return true;
}

bool RunCrossDeviceSamePointer(AclRuntime& runtime, const Options& options,
                               uint32_t device_count)
{
    std::cout << "\n[case1] same process, same host pointer, device "
              << options.device0 << " then device " << options.device1 << "\n";

    if (options.device0 >= static_cast<int>(device_count) ||
        options.device1 >= static_cast<int>(device_count)) {
        std::cout << "  SKIP: device_count=" << device_count << "\n";
        return true;
    }

    bool ok = true;
    ok = RunCrossDeviceWithoutUnregister(runtime, options) && ok;
    ok = RunCrossDeviceAfterUnregister(runtime, options) && ok;
    return ok;
}

bool RunDuplicateRegisterWithoutUnregister(AclRuntime& runtime, const Options& options)
{
    std::cout << "\n[case2a] device " << options.device0
              << " register same host pointer twice without unregister\n";

    PageAlignedHostBuffer host(options.size);
    if (!host) {
        std::cerr << "  FAIL: posix_memalign failed\n";
        return false;
    }
    std::cout << "  host=" << host.data() << ", size=" << host.size() << "\n";

    aclError set_ret = runtime.SetDevice(options.device0);
    std::cout << "  set-device(" << options.device0 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }

    const RegisterResult first =
        RegisterCurrentDevice("first", host.data(), host.size(), options.flags);
    if (!first.registered) {
        return false;
    }

    const RegisterResult second =
        RegisterCurrentDevice("second", host.data(), host.size(), options.flags);

    if (second.registered) {
        UnregisterOnDevice(runtime, options.device0, "cleanup/second", host.data());
    }
    UnregisterOnDevice(runtime, options.device0, "cleanup/first", host.data());
    return true;
}

bool RunDuplicateRegisterAfterUnregister(AclRuntime& runtime, const Options& options)
{
    std::cout << "\n[case2b] device " << options.device0
              << " register, unregister, then register same host pointer again\n";

    PageAlignedHostBuffer host(options.size);
    if (!host) {
        std::cerr << "  FAIL: posix_memalign failed\n";
        return false;
    }
    std::cout << "  host=" << host.data() << ", size=" << host.size() << "\n";

    aclError set_ret = runtime.SetDevice(options.device0);
    std::cout << "  set-device(" << options.device0 << ") ret="
              << FormatAclRet(set_ret) << "\n";
    if (set_ret != ACL_SUCCESS) {
        return false;
    }

    const RegisterResult first =
        RegisterCurrentDevice("first", host.data(), host.size(), options.flags);
    if (!first.registered) {
        return false;
    }

    if (!UnregisterOnDevice(runtime, options.device0, "between/first", host.data())) {
        return false;
    }

    const RegisterResult second =
        RegisterCurrentDevice("second", host.data(), host.size(), options.flags);

    if (second.registered) {
        UnregisterOnDevice(runtime, options.device0, "cleanup/second", host.data());
    }
    return true;
}

bool RunDuplicateRegisterSameDevice(AclRuntime& runtime, const Options& options,
                                    uint32_t device_count)
{
    std::cout << "\n[case2] same process, same device, register same host pointer twice\n";

    if (options.device0 >= static_cast<int>(device_count)) {
        std::cout << "  SKIP: device_count=" << device_count << "\n";
        return true;
    }

    bool ok = true;
    ok = RunDuplicateRegisterWithoutUnregister(runtime, options) && ok;
    ok = RunDuplicateRegisterAfterUnregister(runtime, options) && ok;
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::cout << "aclrtHostRegisterV2 probe\n";
    std::cout << "  flags=" << FlagsToString(options.flags) << "\n";
    std::cout << "  buffer_size=" << options.size << "\n";

    AclRuntime runtime;
    const aclError init_ret = runtime.Init();
    std::cout << "aclInit ret=" << FormatAclRet(init_ret) << "\n";
    if (init_ret != ACL_SUCCESS) {
        return 1;
    }

    const char* soc_name = aclrtGetSocName();
    std::cout << "soc=" << (soc_name == nullptr ? "unknown" : soc_name) << "\n";

    uint32_t device_count = 0;
    const aclError count_ret = aclrtGetDeviceCount(&device_count);
    std::cout << "aclrtGetDeviceCount ret=" << FormatAclRet(count_ret)
              << ", count=" << device_count << "\n";
    if (count_ret != ACL_SUCCESS || device_count == 0U) {
        return 1;
    }

    bool ok = true;
    ok = RunCrossDeviceSamePointer(runtime, options, device_count) && ok;
    ok = RunDuplicateRegisterSameDevice(runtime, options, device_count) && ok;
    return ok ? 0 : 1;
}
