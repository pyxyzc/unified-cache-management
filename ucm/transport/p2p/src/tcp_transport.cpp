#include "tcp_transport.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace transport {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;

struct WinsockRuntime {
    WinsockRuntime() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockRuntime() {
        if (ok) {
            WSACleanup();
        }
    }
    bool ok = false;
};

bool EnsureSocketRuntime() {
    static WinsockRuntime runtime;
    return runtime.ok;
}

void CloseSocket(Socket socket) {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;

bool EnsureSocketRuntime() {
    return true;
}

void CloseSocket(Socket socket) {
    if (socket != kInvalidSocket) {
        close(socket);
    }
}
#endif

uint32_t HostToNetwork32(uint32_t value) {
#if defined(_WIN32)
    return htonl(value);
#else
    return htonl(value);
#endif
}

uint32_t NetworkToHost32(uint32_t value) {
#if defined(_WIN32)
    return ntohl(value);
#else
    return ntohl(value);
#endif
}

Status SendAll(Socket socket, const void* data, size_t length) {
    const auto* cursor = static_cast<const char*>(data);
    while (length > 0) {
        const auto chunk = static_cast<int>(std::min<size_t>(length, 64 * 1024));
        const int sent = send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return Status::Failed;
        }
        cursor += sent;
        length -= static_cast<size_t>(sent);
    }
    return Status::Ok;
}

Status RecvAll(Socket socket, void* data, size_t length) {
    auto* cursor = static_cast<char*>(data);
    while (length > 0) {
        const auto chunk = static_cast<int>(std::min<size_t>(length, 64 * 1024));
        const int received = recv(socket, cursor, chunk, 0);
        if (received <= 0) {
            return Status::Failed;
        }
        cursor += received;
        length -= static_cast<size_t>(received);
    }
    return Status::Ok;
}

}  // namespace

struct TcpControlPlane::Impl {
    Socket listen_socket = kInvalidSocket;
    Socket socket = kInvalidSocket;
};

TcpControlPlane::TcpControlPlane() : impl_(std::make_unique<Impl>()) {}

TcpControlPlane::~TcpControlPlane() {
    close();
}

Status TcpControlPlane::listen(const TcpEndpoint& endpoint, int backlog) {
    if (!EnsureSocketRuntime() || endpoint.port == 0) {
        return Status::InvalidArgument;
    }
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    if (getaddrinfo(host, port.c_str(), &hints, &results) != 0) {
        return Status::Failed;
    }

    Status status = Status::Failed;
    for (auto* item = results; item != nullptr; item = item->ai_next) {
        const auto candidate = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (candidate == kInvalidSocket) {
            continue;
        }
        int yes = 1;
        (void)setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(candidate, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0 &&
            ::listen(candidate, backlog) == 0) {
            impl_->listen_socket = candidate;
            status = Status::Ok;
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    return status;
}

Status TcpControlPlane::accept() {
    if (impl_->listen_socket == kInvalidSocket) {
        return Status::InvalidArgument;
    }
    if (impl_->socket != kInvalidSocket) {
        CloseSocket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    impl_->socket = ::accept(impl_->listen_socket, nullptr, nullptr);
    return impl_->socket == kInvalidSocket ? Status::Failed : Status::Ok;
}

Status TcpControlPlane::connect(const TcpEndpoint& endpoint) {
    if (!EnsureSocketRuntime() || endpoint.host.empty() || endpoint.port == 0) {
        return Status::InvalidArgument;
    }
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
        return Status::Failed;
    }

    Status status = Status::Failed;
    for (auto* item = results; item != nullptr; item = item->ai_next) {
        const auto candidate = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (candidate == kInvalidSocket) {
            continue;
        }
        if (::connect(candidate, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0) {
            impl_->socket = candidate;
            status = Status::Ok;
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    return status;
}

Status TcpControlPlane::sendMetadata(const Metadata& metadata) const {
    if (impl_->socket == kInvalidSocket || metadata.size() > UINT32_MAX) {
        return Status::InvalidArgument;
    }
    const uint32_t length = HostToNetwork32(static_cast<uint32_t>(metadata.size()));
    auto status = SendAll(impl_->socket, &length, sizeof(length));
    if (status != Status::Ok || metadata.empty()) {
        return status;
    }
    return SendAll(impl_->socket, metadata.data(), metadata.size());
}

Status TcpControlPlane::receiveMetadata(Metadata& metadata) const {
    if (impl_->socket == kInvalidSocket) {
        return Status::InvalidArgument;
    }
    uint32_t network_length = 0;
    auto status = RecvAll(impl_->socket, &network_length, sizeof(network_length));
    if (status != Status::Ok) {
        return status;
    }
    const auto length = NetworkToHost32(network_length);
    metadata.assign(length, 0);
    if (length == 0) {
        return Status::Ok;
    }
    return RecvAll(impl_->socket, metadata.data(), metadata.size());
}

bool TcpControlPlane::connected() const {
    return impl_->socket != kInvalidSocket;
}

void TcpControlPlane::closeConnection() {
    CloseSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
}

void TcpControlPlane::close() {
    CloseSocket(impl_->socket);
    CloseSocket(impl_->listen_socket);
    impl_->socket = kInvalidSocket;
    impl_->listen_socket = kInvalidSocket;
}

}  // namespace transport
