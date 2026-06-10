#pragma once

#include "core/transport.h"

#include <cstdint>
#include <map>
#include <string>

namespace transport::detail {

uint64_t PtrToU64(const void* ptr);
void* U64ToPtr(uint64_t value);
Metadata PackKV(const std::map<std::string, std::string>& kv);
std::map<std::string, std::string> UnpackKV(const Metadata& metadata);

}  // namespace transport::detail
