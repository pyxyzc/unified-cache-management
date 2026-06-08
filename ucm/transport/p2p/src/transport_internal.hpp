#pragma once

#include "transport.hpp"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

namespace transport::detail {

std::string ToString(MemoryType type);
MemoryType MemoryTypeFromString(const std::string& value);
uint64_t PtrToU64(const void* ptr);
void* U64ToPtr(uint64_t value);
Metadata PackKV(const std::map<std::string, std::string>& kv);
std::map<std::string, std::string> UnpackKV(const Metadata& metadata);
uint64_t ToU64(const std::map<std::string, std::string>& kv,
               const std::string& key,
               uint64_t default_value = 0);

class InMemoryRegistry {
   public:
    Status Register(const MemoryRegion& memory);
    Status Unregister(const MemoryRegion& memory);
    const MemoryRegion* Find(uint64_t address, uint64_t length) const;
    const std::unordered_map<MemoryHandle, MemoryRegion>& memories() const;

   private:
    MemoryHandle next_ = 1;
    std::unordered_map<MemoryHandle, MemoryRegion> memories_;
};

}  // namespace transport::detail
