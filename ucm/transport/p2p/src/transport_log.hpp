#pragma once

#include "transport.hpp"

#include <cstddef>
#include <sstream>
#include <string>

namespace transport::log {

enum class Level {
    Error = 0,
    Info = 1,
    Debug = 2,
    Trace = 3,
};

class Message {
   public:
    Message(Level level, const char* component);
    ~Message();

    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    template <typename T>
    Message& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

   private:
    Level level_;
    const char* component_;
    bool enabled_ = false;
    std::ostringstream stream_;
};

bool enabled(Level level);
std::string metadataSummary(const Metadata& metadata);

}  // namespace transport::log
