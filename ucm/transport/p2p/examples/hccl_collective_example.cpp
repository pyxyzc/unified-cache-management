/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include "hccl/hccl_transport.h"

#include <acl/acl.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string endpoint_ip = "127.0.0.1";
    uint16_t endpoint_port = 8085;
    uint32_t rank = 0;
    uint32_t rank_count = 2;
    uint32_t root_info_rank = 0;
    uint32_t root_rank = 0;
    int32_t device_id = 0;
    size_t bytes = 4096;
};

const char* StatusName(transport::Status status) {
    switch (status) {
        case transport::Status::Ok:
            return "Ok";
        case transport::Status::InvalidArgument:
            return "InvalidArgument";
        case transport::Status::NotFound:
            return "NotFound";
        case transport::Status::AlreadyExists:
            return "AlreadyExists";
        case transport::Status::Failed:
            return "Failed";
    }
    return "Unknown";
}

uint8_t ExpectedByte(size_t index) {
    return static_cast<uint8_t>((index * 131 + 17) & 0xff);
}

bool ParseUint32(const char* text, uint32_t& value) {
    char* end = nullptr;
    const auto parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseInt32(const char* text, int32_t& value) {
    char* end = nullptr;
    const auto parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
}

bool ParseSize(const char* text, size_t& value) {
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << key << std::endl;
            return false;
        }
        const char* value = argv[++i];
        if (key == "--endpoint-ip") {
            args.endpoint_ip = value;
        } else if (key == "--endpoint-port") {
            uint32_t port = 0;
            if (!ParseUint32(value, port) || port == 0 || port > 65535) {
                std::cerr << "invalid endpoint port: " << value << std::endl;
                return false;
            }
            args.endpoint_port = static_cast<uint16_t>(port);
        } else if (key == "--rank") {
            if (!ParseUint32(value, args.rank)) {
                std::cerr << "invalid rank: " << value << std::endl;
                return false;
            }
        } else if (key == "--rank-count") {
            if (!ParseUint32(value, args.rank_count)) {
                std::cerr << "invalid rank count: " << value << std::endl;
                return false;
            }
        } else if (key == "--root-info-rank") {
            if (!ParseUint32(value, args.root_info_rank)) {
                std::cerr << "invalid root info rank: " << value << std::endl;
                return false;
            }
        } else if (key == "--root-rank") {
            if (!ParseUint32(value, args.root_rank)) {
                std::cerr << "invalid root rank: " << value << std::endl;
                return false;
            }
        } else if (key == "--device") {
            if (!ParseInt32(value, args.device_id)) {
                std::cerr << "invalid device id: " << value << std::endl;
                return false;
            }
        } else if (key == "--bytes") {
            if (!ParseSize(value, args.bytes)) {
                std::cerr << "invalid bytes: " << value << std::endl;
                return false;
            }
        } else {
            std::cerr << "unknown argument: " << key << std::endl;
            return false;
        }
    }
    if (args.rank_count == 0 || args.rank >= args.rank_count || args.root_info_rank >= args.rank_count ||
        args.root_rank >= args.rank_count || args.bytes == 0 || args.device_id < 0) {
        std::cerr << "invalid rank/device/bytes configuration" << std::endl;
        return false;
    }
    return true;
}

void FillInput(const Args& args, std::vector<uint8_t>& host) {
    for (size_t i = 0; i < host.size(); ++i) {
        host[i] = args.rank == args.root_rank ? ExpectedByte(i) : 0;
    }
}

bool VerifyOutput(const std::vector<uint8_t>& host) {
    for (size_t i = 0; i < host.size(); ++i) {
        if (host[i] != ExpectedByte(i)) {
            std::cerr << "verify failed at byte " << i << ": got " << static_cast<int>(host[i])
                      << ", expected " << static_cast<int>(ExpectedByte(i)) << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckAcl(aclError ret, const char* api) {
    if (ret == ACL_SUCCESS) {
        return true;
    }
    std::cerr << api << " failed: " << static_cast<int>(ret) << std::endl;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        std::cerr << "usage: " << argv[0]
                  << " --rank N --rank-count N --root-info-rank N --root-rank N"
                  << " --endpoint-ip IP --endpoint-port PORT --device N --bytes N" << std::endl;
        return 2;
    }

    if (!CheckAcl(aclInit(nullptr), "aclInit")) {
        return 1;
    }
    bool acl_initialized = true;
    bool device_set = false;
    aclrtStream stream = nullptr;
    void* device_buffer = nullptr;

    auto cleanup = [&]() {
        if (stream != nullptr) {
            (void)aclrtDestroyStream(stream);
        }
        if (device_buffer != nullptr) {
            (void)aclrtFree(device_buffer);
        }
        if (device_set) {
            (void)aclrtResetDevice(args.device_id);
        }
        if (acl_initialized) {
            (void)aclFinalize();
        }
    };

    if (!CheckAcl(aclrtSetDevice(args.device_id), "aclrtSetDevice")) {
        cleanup();
        return 1;
    }
    device_set = true;

    if (!CheckAcl(aclrtCreateStream(&stream), "aclrtCreateStream")) {
        cleanup();
        return 1;
    }
    if (!CheckAcl(aclrtMalloc(&device_buffer, args.bytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc")) {
        cleanup();
        return 1;
    }

    std::vector<uint8_t> host(args.bytes);
    FillInput(args, host);
    if (!CheckAcl(aclrtMemcpy(device_buffer, args.bytes, host.data(), args.bytes, ACL_MEMCPY_HOST_TO_DEVICE),
                  "aclrtMemcpy host-to-device")) {
        cleanup();
        return 1;
    }

    transport::HcclInitAttrs config;
    config.bootstrap_endpoint = {args.endpoint_ip, args.endpoint_port};
    config.bootstrap_role = args.rank == args.root_info_rank ? transport::HcclBootstrapRole::Root
                                                             : transport::HcclBootstrapRole::Client;
    config.rank_count = args.rank_count;
    config.rank = args.rank;
    config.root_info_rank = args.root_info_rank;

    auto hccl_transport = std::make_unique<transport::HcclTransport>();
    auto status = hccl_transport->init(config);
    if (status != transport::Status::Ok) {
        std::cerr << "rank " << args.rank << " init failed: " << StatusName(status) << std::endl;
        hccl_transport.reset();
        cleanup();
        return 1;
    }

    transport::HcclCollectiveTransfer transfer;
    transfer.op = transport::HcclCollectiveOp::Broadcast;
    transfer.buffer = device_buffer;
    transfer.length = args.bytes;
    transfer.root_rank = args.root_rank;
    transfer.stream = stream;

    status = hccl_transport->submitCollective(transfer);
    if (status != transport::Status::Ok) {
        std::cerr << "rank " << args.rank << " collective failed: " << StatusName(status) << std::endl;
        (void)hccl_transport->shutdown();
        hccl_transport.reset();
        cleanup();
        return 1;
    }

    if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream")) {
        (void)hccl_transport->shutdown();
        hccl_transport.reset();
        cleanup();
        return 1;
    }

    std::memset(host.data(), 0, host.size());
    if (!CheckAcl(aclrtMemcpy(host.data(), args.bytes, device_buffer, args.bytes, ACL_MEMCPY_DEVICE_TO_HOST),
                  "aclrtMemcpy device-to-host")) {
        (void)hccl_transport->shutdown();
        hccl_transport.reset();
        cleanup();
        return 1;
    }

    if (!VerifyOutput(host)) {
        (void)hccl_transport->shutdown();
        hccl_transport.reset();
        cleanup();
        return 1;
    }

    std::cout << "rank " << args.rank << " collective verify ok, bytes=" << args.bytes
              << ", rootRank=" << args.root_rank << std::endl;

    status = hccl_transport->shutdown();
    hccl_transport.reset();
    cleanup();
    if (status != transport::Status::Ok) {
        std::cerr << "rank " << args.rank << " shutdown failed: " << StatusName(status) << std::endl;
        return 1;
    }
    return 0;
}
