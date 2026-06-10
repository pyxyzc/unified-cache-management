#include "tcp_transport.hpp"

#include "transport_log.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace transport {
namespace {

using Socket = int;
constexpr Socket kInvalidSocket = -1;

void CloseSocket(Socket socket) {
    if (socket != kInvalidSocket) {
        close(socket);
    }
}

uint32_t HostToNetwork32(uint32_t value) {
    return htonl(value);
}

uint32_t NetworkToHost32(uint32_t value) {
    return ntohl(value);
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

TcpEndpoint EndpointFromSockaddr(const sockaddr_storage& storage, socklen_t length) {
    TcpEndpoint endpoint;
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&storage),
                    length,
                    host,
                    sizeof(host),
                    service,
                    sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        endpoint.host = host;
        endpoint.port = static_cast<uint16_t>(std::stoi(service));
    }
    return endpoint;
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

TcpControlPlane::TcpControlPlane(TcpControlPlane&& other) noexcept : impl_(std::move(other.impl_)) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    other.impl_ = std::make_unique<Impl>();
}

TcpControlPlane& TcpControlPlane::operator=(TcpControlPlane&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
        if (!impl_) {
            impl_ = std::make_unique<Impl>();
        }
        other.impl_ = std::make_unique<Impl>();
    }
    return *this;
}

Status TcpControlPlane::listen(const TcpEndpoint& endpoint, int backlog) {
    if (endpoint.port == 0) {
        return Status::InvalidArgument;
    }
    log::Message(log::Level::Debug, "tcp") << "listen begin endpoint=" << endpoint.host
                                           << ':' << endpoint.port
                                           << " backlog=" << backlog;
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    if (getaddrinfo(host, port.c_str(), &hints, &results) != 0) {
        log::Message(log::Level::Error, "tcp") << "listen getaddrinfo failed endpoint="
                                               << endpoint.host << ':' << endpoint.port;
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
            log::Message(log::Level::Debug, "tcp") << "listen ok endpoint=" << endpoint.host
                                                   << ':' << endpoint.port
                                                   << " socket=" << candidate;
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    if (status != Status::Ok) {
        log::Message(log::Level::Error, "tcp") << "listen failed endpoint=" << endpoint.host
                                               << ':' << endpoint.port;
    }
    return status;
}

Status TcpControlPlane::accept(TcpControlPlane& channel, TcpEndpoint* remote) {
    if (impl_->listen_socket == kInvalidSocket) {
        return Status::InvalidArgument;
    }

    sockaddr_storage remote_addr{};
    socklen_t remote_len = sizeof(remote_addr);
    const auto accepted = ::accept(impl_->listen_socket,
                                   reinterpret_cast<sockaddr*>(&remote_addr),
                                   &remote_len);
    if (accepted == kInvalidSocket) {
        log::Message(log::Level::Error, "tcp") << "accept failed";
        return Status::Failed;
    }

    channel.close();
    channel.impl_->socket = accepted;
    if (remote != nullptr) {
        *remote = EndpointFromSockaddr(remote_addr, remote_len);
        log::Message(log::Level::Debug, "tcp") << "accept ok remote=" << remote->host
                                               << ':' << remote->port
                                               << " socket=" << accepted;
    } else {
        log::Message(log::Level::Debug, "tcp") << "accept ok socket=" << accepted;
    }
    return Status::Ok;
}

Status TcpControlPlane::connect(const TcpEndpoint& endpoint) {
    if (endpoint.host.empty() || endpoint.port == 0) {
        return Status::InvalidArgument;
    }
    log::Message(log::Level::Debug, "tcp") << "connect begin endpoint=" << endpoint.host
                                           << ':' << endpoint.port;
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
        log::Message(log::Level::Error, "tcp") << "connect getaddrinfo failed endpoint="
                                               << endpoint.host << ':' << endpoint.port;
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
            log::Message(log::Level::Debug, "tcp") << "connect ok endpoint=" << endpoint.host
                                                   << ':' << endpoint.port
                                                   << " socket=" << candidate;
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    if (status != Status::Ok) {
        log::Message(log::Level::Error, "tcp") << "connect failed endpoint=" << endpoint.host
                                               << ':' << endpoint.port;
    }
    return status;
}

Status TcpControlPlane::sendMetadata(const Metadata& metadata) const {
    if (impl_->socket == kInvalidSocket || metadata.size() > UINT32_MAX) {
        return Status::InvalidArgument;
    }
    log::Message(log::Level::Debug, "tcp") << "send metadata "
                                           << log::metadataSummary(metadata);
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
        log::Message(log::Level::Debug, "tcp") << "receive metadata "
                                               << log::metadataSummary(metadata);
        return Status::Ok;
    }
    status = RecvAll(impl_->socket, metadata.data(), metadata.size());
    if (status == Status::Ok) {
        log::Message(log::Level::Debug, "tcp") << "receive metadata "
                                               << log::metadataSummary(metadata);
    }
    return status;
}

bool TcpControlPlane::connected() const {
    return impl_->socket != kInvalidSocket;
}

void TcpControlPlane::closeConnection() {
    log::Message(log::Level::Trace, "tcp") << "close connection socket=" << impl_->socket;
    CloseSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
}

void TcpControlPlane::close() {
    log::Message(log::Level::Trace, "tcp") << "close socket=" << impl_->socket
                                           << " listen_socket=" << impl_->listen_socket;
    CloseSocket(impl_->socket);
    CloseSocket(impl_->listen_socket);
    impl_->socket = kInvalidSocket;
    impl_->listen_socket = kInvalidSocket;
}

}  // namespace transport
