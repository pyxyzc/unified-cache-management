#include "transport_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace transport::log {
namespace {

Level parseLevel(const char* text) {
    if (text == nullptr || *text == '\0') {
        return Level::Info;
    }
    std::string value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "0" || value == "error" || value == "err") {
        return Level::Error;
    }
    if (value == "1" || value == "info") {
        return Level::Info;
    }
    if (value == "2" || value == "debug") {
        return Level::Debug;
    }
    if (value == "3" || value == "trace") {
        return Level::Trace;
    }
    if (value == "off" || value == "none" || value == "quiet") {
        return static_cast<Level>(-1);
    }
    return Level::Info;
}

Level configuredLevel() {
    static const Level level = parseLevel(std::getenv("TRANSPORT_LOG_LEVEL"));
    return level;
}

const char* levelName(Level level) {
    switch (level) {
        case Level::Error:
            return "error";
        case Level::Info:
            return "info";
        case Level::Debug:
            return "debug";
        case Level::Trace:
            return "trace";
    }
    return "unknown";
}

size_t metadataByteLimit() {
    const char* text = std::getenv("TRANSPORT_LOG_METADATA_BYTES");
    if (text == nullptr || *text == '\0') {
        return 512;
    }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<size_t>(value) : 512;
}

std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

bool enabled(Level level) {
    return static_cast<int>(level) <= static_cast<int>(configuredLevel());
}

Message::Message(Level level, const char* component)
    : level_(level), component_(component), enabled_(enabled(level)) {}

Message::~Message() {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(outputMutex());
    std::cerr << "[transport " << levelName(level_) << "] [" << component_ << "] "
              << stream_.str() << '\n';
}

std::string metadataSummary(const Metadata& metadata) {
    std::ostringstream out;
    const auto limit = std::min(metadata.size(), metadataByteLimit());
    out << "size=" << metadata.size() << " data=\"";
    for (size_t i = 0; i < limit; ++i) {
        const auto byte = metadata[i];
        switch (byte) {
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            default:
                if (std::isprint(byte) != 0) {
                    out << static_cast<char>(byte);
                } else {
                    out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned>(byte) << std::dec;
                }
                break;
        }
    }
    if (limit < metadata.size()) {
        out << "...";
    }
    out << '"';
    return out.str();
}

}  // namespace transport::log
