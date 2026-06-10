#pragma once

#include "transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace transport {

struct TcpEndpoint {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
};

class TcpControlPlane {
   public:
    TcpControlPlane();
    ~TcpControlPlane();

    TcpControlPlane(const TcpControlPlane&) = delete;
    TcpControlPlane& operator=(const TcpControlPlane&) = delete;
    TcpControlPlane(TcpControlPlane&&) noexcept;
    TcpControlPlane& operator=(TcpControlPlane&&) noexcept;

    Status listen(const TcpEndpoint& endpoint, int backlog = 16);
    Status accept(TcpControlPlane& channel, TcpEndpoint* remote = nullptr);
    Status connect(const TcpEndpoint& endpoint);
    Status sendMetadata(const Metadata& metadata) const;
    Status receiveMetadata(Metadata& metadata) const;
    bool connected() const;
    void closeConnection();
    void close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport
