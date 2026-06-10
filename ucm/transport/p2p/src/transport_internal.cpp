#include "transport_internal.hpp"

#include <sstream>

namespace transport::detail {

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

}  // namespace transport::detail
