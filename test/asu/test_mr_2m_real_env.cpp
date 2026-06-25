// SPDX-License-Identifier: Apache-2.0
// tests/kv/test_mr_2m_real_env.cpp
//
// Manual real-environment probe for the 2MiB MR-registration hypothesis.
// This test requires a real Ascend/CANN/HCCP environment. It does not need a
// CPU KV server: it only initializes UbV2ResourceManager, allocates device
// memory through the repo's AllocDeviceMemory wrapper, and calls
// RegisterExportableSeg.
//
// Recommended run:
//   ./scripts/build.sh
//   bash scripts/test_mr_2m_real_env.sh --device-id 0
//
// Useful expectation flags:
//   --expect-small fail
//   --expect-subrange-register fail
//
// Interpretation:
//   - large-full registration must pass; otherwise the local HCCP/Lmem path is
//     not healthy enough for this probe.
//   - small-standalone and large-subrange registration are probes. Different
//     CANN/driver versions may accept or reject them, so they are not fatal
//     unless an --expect-* flag is supplied.
//   - successful large-full registration plus an unaligned address inside that
//     MR supports the intended access model: register a large MR, then use
//     base + offset as the KVCache VA in data-plane operations.

#include "host/ub/ub_resource.h"
#include "host/ub/ub_status.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr uint64_t k2MiB = 2ULL * 1024 * 1024;
constexpr uint64_t k64KiB = 64ULL * 1024;

enum class Expect {
    Any,
    Pass,
    Fail,
};

struct Args {
    uint32_t deviceId{0};
    uint64_t smallBytes{k64KiB};
    uint64_t largeBytes{k2MiB};
    uint64_t subOffset{12345};
    uint32_t access{0xE};
    uint32_t tokenValue{4242};
    bool cacheable{false};
    bool probeSmall{true};
    bool probeSubrangeRegister{true};
    bool skipTokenIdAlloc{false};
    Expect expectSmall{Expect::Any};
    Expect expectSubrangeRegister{Expect::Any};
};

uint64_t RoundUp2MiB(uint64_t bytes)
{
    if (bytes == 0) return 0;
    return ((bytes - 1) / k2MiB + 1) * k2MiB;
}

const char* YesNo(bool v)
{
    return v ? "yes" : "no";
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --device-id <n>                 ACL logic device id, default 0\n"
        "  --small-bytes <n>               small standalone allocation, default 65536\n"
        "  --large-bytes <n>               large allocation, default 2097152\n"
        "  --sub-offset <n>                offset inside large MR, default 12345\n"
        "  --access <hex|dec>              LocalSegSpec access, default 0xE\n"
        "  --token-value <n>               tokenValue, default 4242\n"
        "  --cacheable 0|1                 LocalSegSpec cacheable, default 0\n"
        "  --skip-token-id-alloc           InitConfig.skipTokenIdAlloc=true\n"
        "  --no-small                     skip small standalone registration probe\n"
        "  --no-subrange-register          skip direct subrange registration probe\n"
        "  --expect-small any|pass|fail    default any\n"
        "  --expect-subrange-register any|pass|fail, default any\n"
        "  -h, --help                      show this help\n",
        argv0);
}

bool ParseU64(const char* s, uint64_t* out)
{
    if (s == nullptr || out == nullptr) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 0);
    if (end == s || *end != '\0') return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

bool ParseU32(const char* s, uint32_t* out)
{
    uint64_t v = 0;
    if (!ParseU64(s, &v) || v > 0xFFFFFFFFULL) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool ParseExpect(const char* s, Expect* out)
{
    if (s == nullptr || out == nullptr) return false;
    const std::string v(s);
    if (v == "any") {
        *out = Expect::Any;
    } else if (v == "pass") {
        *out = Expect::Pass;
    } else if (v == "fail") {
        *out = Expect::Fail;
    } else {
        return false;
    }
    return true;
}

bool ParseArgs(int argc, char** argv, Args* args)
{
    if (args == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        const std::string opt(argv[i]);
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            Usage(argv[0]);
            return false;
        } else if (opt == "--device-id") {
            const char* v = needValue("--device-id");
            if (v == nullptr || !ParseU32(v, &args->deviceId)) return false;
        } else if (opt == "--small-bytes") {
            const char* v = needValue("--small-bytes");
            if (v == nullptr || !ParseU64(v, &args->smallBytes)) return false;
        } else if (opt == "--large-bytes") {
            const char* v = needValue("--large-bytes");
            if (v == nullptr || !ParseU64(v, &args->largeBytes)) return false;
        } else if (opt == "--sub-offset") {
            const char* v = needValue("--sub-offset");
            if (v == nullptr || !ParseU64(v, &args->subOffset)) return false;
        } else if (opt == "--access") {
            const char* v = needValue("--access");
            if (v == nullptr || !ParseU32(v, &args->access)) return false;
        } else if (opt == "--token-value") {
            const char* v = needValue("--token-value");
            if (v == nullptr || !ParseU32(v, &args->tokenValue)) return false;
        } else if (opt == "--cacheable") {
            const char* v = needValue("--cacheable");
            if (v == nullptr) return false;
            if (std::strcmp(v, "0") == 0) args->cacheable = false;
            else if (std::strcmp(v, "1") == 0) args->cacheable = true;
            else return false;
        } else if (opt == "--skip-token-id-alloc") {
            args->skipTokenIdAlloc = true;
        } else if (opt == "--no-small") {
            args->probeSmall = false;
        } else if (opt == "--no-subrange-register") {
            args->probeSubrangeRegister = false;
        } else if (opt == "--expect-small") {
            const char* v = needValue("--expect-small");
            if (v == nullptr || !ParseExpect(v, &args->expectSmall)) return false;
        } else if (opt == "--expect-subrange-register") {
            const char* v = needValue("--expect-subrange-register");
            if (v == nullptr || !ParseExpect(v, &args->expectSubrangeRegister)) return false;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", opt.c_str());
            Usage(argv[0]);
            return false;
        }
    }

    if (args->smallBytes == 0 || args->largeBytes == 0) {
        std::fprintf(stderr, "smallBytes/largeBytes must be non-zero\n");
        return false;
    }
    if (args->largeBytes < k2MiB) {
        std::fprintf(stderr, "largeBytes must be >= 2MiB for this probe\n");
        return false;
    }
    if (args->subOffset >= args->largeBytes) {
        std::fprintf(stderr, "subOffset must be inside largeBytes\n");
        return false;
    }
    if (args->smallBytes > args->largeBytes - args->subOffset) {
        std::fprintf(stderr, "smallBytes must fit after subOffset inside largeBytes\n");
        return false;
    }
    return true;
}

std::string StatusString(const ::umc::comm::UbStatus& st)
{
    std::string out = ::umc::comm::UbErrorCodeToString(st.Code());
    if (!st.Message().empty()) {
        out += ": ";
        out += st.Message();
    }
    return out;
}

void PrintStatus(const char* label, const ::umc::comm::UbStatus& st)
{
    std::fprintf(stderr, "[mr-2m-real] %-28s %s (%s)\n",
                 label, st.IsOk() ? "PASS" : "FAIL", StatusString(st).c_str());
}

bool CheckExpectation(const char* label, bool ok, Expect expect)
{
    if (expect == Expect::Any) return true;
    const bool wanted = (expect == Expect::Pass);
    if (ok == wanted) return true;
    std::fprintf(stderr, "[mr-2m-real] expectation mismatch: %s got=%s wanted=%s\n",
                 label, ok ? "pass" : "fail", wanted ? "pass" : "fail");
    return false;
}

struct DeviceBuffer {
    void* ptr{nullptr};
    uint64_t size{0};

    ~DeviceBuffer()
    {
        if (ptr != nullptr) {
            (void)::umc::comm::FreeDeviceMemory(ptr);
        }
    }

    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
};

::umc::comm::UbStatus Alloc(const char* label, uint64_t bytes, DeviceBuffer* out)
{
    if (out == nullptr) {
        return ::umc::comm::UbStatus(::umc::comm::UbErrorCode::InvalidArgument, "out null");
    }
    out->size = bytes;
    auto st = ::umc::comm::AllocDeviceMemory(&out->ptr, bytes);
    PrintStatus(label, st);
    if (st.IsOk()) {
        std::fprintf(stderr, "[mr-2m-real] %-28s ptr=%p bytes=%llu rounded2m=%llu\n",
                     label, out->ptr,
                     static_cast<unsigned long long>(bytes),
                     static_cast<unsigned long long>(RoundUp2MiB(bytes)));
    }
    return st;
}

::umc::comm::UbStatus RegisterSeg(::umc::comm::UbV2ResourceManager& mgr,
                                  const char* label,
                                  void* base,
                                  uint64_t bytes,
                                  const Args& args,
                                  ::umc::comm::UbV2ResourceManager::ExportedSeg* out)
{
    ::umc::comm::LocalSegSpec spec{};
    spec.baseVa = base;
    spec.size = bytes;
    spec.access = args.access;
    spec.tokenValue = args.skipTokenIdAlloc ? 0u : args.tokenValue;
    spec.cacheable = args.cacheable;

    auto st = mgr.RegisterExportableSeg(spec, out);
    PrintStatus(label, st);
    std::fprintf(stderr,
                 "[mr-2m-real] %-28s base=%p bytes=%llu access=0x%x cacheable=%s tokenValue=%u\n",
                 label, base, static_cast<unsigned long long>(bytes), args.access,
                 YesNo(args.cacheable), spec.tokenValue);
    if (st.IsOk() && out != nullptr) {
        std::fprintf(stderr,
                     "[mr-2m-real] %-28s memKeySize=%u tokenId=%u tokenValue=%u\n",
                     label, out->memKeySize, out->tokenId, out->tokenValue);
    }
    return st;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        return 2;
    }

    std::fprintf(stderr,
                 "[mr-2m-real] config device=%u small=%llu large=%llu subOffset=%llu "
                 "access=0x%x cacheable=%s skipTokenIdAlloc=%s\n",
                 args.deviceId,
                 static_cast<unsigned long long>(args.smallBytes),
                 static_cast<unsigned long long>(args.largeBytes),
                 static_cast<unsigned long long>(args.subOffset),
                 args.access, YesNo(args.cacheable), YesNo(args.skipTokenIdAlloc));

    ::umc::comm::UbV2ResourceManager mgr;
    ::umc::comm::UbV2ResourceManager::InitConfig cfg{};
    cfg.deviceId = args.deviceId;
    cfg.profile = ::umc::comm::TransportProfile::Ubc;
    cfg.connMode = ::umc::comm::JettyConnMode::Rc;
    cfg.probeUboeCapability = false;
    cfg.skipTokenIdAlloc = args.skipTokenIdAlloc;

    auto st = mgr.Init(cfg);
    PrintStatus("UbV2ResourceManager::Init", st);
    if (st.IsError()) {
        return 10;
    }

    bool ok = true;

    if (args.probeSmall) {
        DeviceBuffer small;
        st = Alloc("small alloc", args.smallBytes, &small);
        if (st.IsOk()) {
            ::umc::comm::UbV2ResourceManager::ExportedSeg smallSeg;
            auto rs = RegisterSeg(mgr, "small register", small.ptr, args.smallBytes, args, &smallSeg);
            ok = CheckExpectation("small register", rs.IsOk(), args.expectSmall) && ok;
        } else {
            ok = CheckExpectation("small alloc/register", false, args.expectSmall) && ok;
        }
    }

    DeviceBuffer large;
    st = Alloc("large alloc", args.largeBytes, &large);
    if (st.IsError()) {
        return 20;
    }

    {
        ::umc::comm::UbV2ResourceManager::ExportedSeg largeSeg;
        st = RegisterSeg(mgr, "large full register", large.ptr, args.largeBytes, args, &largeSeg);
        if (st.IsError()) {
            std::fprintf(stderr, "[mr-2m-real] large full registration is required for a valid probe\n");
            return 21;
        }

        const auto base = reinterpret_cast<uintptr_t>(large.ptr);
        const auto subVa = base + static_cast<uintptr_t>(args.subOffset);
        std::fprintf(stderr,
                     "[mr-2m-real] kv-cache subrange model: registered=[0x%llx,0x%llx) "
                     "subVa=0x%llx subBytes=%llu subVa2MiBAligned=%s\n",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(base + args.largeBytes),
                     static_cast<unsigned long long>(subVa),
                     static_cast<unsigned long long>(args.smallBytes),
                     YesNo((subVa % k2MiB) == 0));
    }

    if (args.probeSubrangeRegister) {
        const auto sub = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(large.ptr) + static_cast<uintptr_t>(args.subOffset));
        ::umc::comm::UbV2ResourceManager::ExportedSeg subSeg;
        auto rs = RegisterSeg(mgr, "subrange direct register", sub, args.smallBytes, args, &subSeg);
        ok = CheckExpectation("subrange direct register", rs.IsOk(), args.expectSubrangeRegister) && ok;
    }

    std::fprintf(stderr, "[mr-2m-real] result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 30;
}

