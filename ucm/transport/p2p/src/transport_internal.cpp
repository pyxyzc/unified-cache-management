#include "transport_internal.hpp"

#include <sstream>

namespace transport::detail {

std::string ToString(MemoryType type) {
    return type == MemoryType::Device ? "device" : "host";
}

MemoryType MemoryTypeFromString(const std::string& value) {
    return value == "device" ? MemoryType::Device : MemoryType::Host;
}

uint64_t PtrToU64(const void* ptr) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

void* U64ToPtr(uint64_t value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

Metadata PackKV(const std::map<std::string, std::string>& kv) {
    std::ostringstream os;
    for (const auto& item : kv) {
        os << item.first << '=' << item.second << '\n';
    }
    const auto text = os.str();
    return Metadata(text.begin(), text.end());
}

std::map<std::string, std::string> UnpackKV(const Metadata& metadata) {
    std::map<std::string, std::string> kv;
    std::string text(metadata.begin(), metadata.end());
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        kv.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return kv;
}

uint64_t ToU64(const std::map<std::string, std::string>& kv,
               const std::string& key,
               uint64_t default_value) {
    const auto it = kv.find(key);
    if (it == kv.end()) {
        return default_value;
    }
    try {
        return static_cast<uint64_t>(std::stoull(it->second, nullptr, 0));
    } catch (...) {
        return default_value;
    }
}

Status InMemoryRegistry::Register(const MemoryRegion& memory) {
    if (memory.addr == nullptr || memory.length == 0) {
        return Status::InvalidArgument;
    }
    const auto address = PtrToU64(memory.addr);
    for (const auto& item : memories_) {
        if (PtrToU64(item.second.addr) == address) {
            return Status::AlreadyExists;
        }
    }
    const auto handle = next_++;
    memories_.emplace(handle, memory);
    return Status::Ok;
}

Status InMemoryRegistry::Unregister(const MemoryRegion& memory) {
    const auto address = PtrToU64(memory.addr);
    for (auto it = memories_.begin(); it != memories_.end(); ++it) {
        if (PtrToU64(it->second.addr) == address) {
            memories_.erase(it);
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

const MemoryRegion* InMemoryRegistry::Find(uint64_t address, uint64_t length) const {
    for (const auto& item : memories_) {
        const auto begin = PtrToU64(item.second.addr);
        if (address < begin) {
            continue;
        }
        const auto offset = address - begin;
        if (offset <= item.second.length && length <= item.second.length - offset) {
            return &item.second;
        }
    }
    return nullptr;
}

const std::unordered_map<MemoryHandle, MemoryRegion>& InMemoryRegistry::memories() const {
    return memories_;
}

}  // namespace transport::detail
