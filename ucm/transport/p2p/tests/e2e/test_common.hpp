#pragma once

#include "transport.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace transport::test {

inline const char* statusName(Status status) {
    switch (status) {
        case Status::Ok:
            return "Ok";
        case Status::InvalidArgument:
            return "InvalidArgument";
        case Status::NotFound:
            return "NotFound";
        case Status::AlreadyExists:
            return "AlreadyExists";
        case Status::Failed:
            return "Failed";
    }
    return "Unknown";
}

inline bool expectOk(Status status, const char* step) {
    if (status == Status::Ok) {
        return true;
    }
    std::cerr << step << " failed: " << statusName(status) << '\n';
    return false;
}

inline bool expectStatus(Status actual, Status expected, const char* step) {
    if (actual == expected) {
        return true;
    }
    std::cerr << step << " failed: got " << statusName(actual)
              << ", expected " << statusName(expected) << '\n';
    return false;
}

inline bool expectTrue(bool value, const char* step) {
    if (value) {
        return true;
    }
    std::cerr << step << " failed\n";
    return false;
}

inline bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

inline uint16_t envPort(const char* name, uint16_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    const auto value = std::strtoul(text, nullptr, 10);
    if (value == 0 || value > UINT16_MAX) {
        return fallback;
    }
    return static_cast<uint16_t>(value);
}

}  // namespace transport::test
