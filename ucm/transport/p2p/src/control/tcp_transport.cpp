#include "control/tcp_transport.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

#include "logger/logger.h"

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

size_t MetadataByteLimit() {
    const char* text = std::getenv("TRANSPORT_LOG_METADATA_BYTES");
    if (text == nullptr || *text == '\0') {
        return 512;
    }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<size_t>(value) : 512;
}

std::string MetadataSummary(const Metadata& metadata) {
    std::ostringstream out;
    const auto limit = std::min(metadata.size(), MetadataByteLimit());
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
    UC_DEBUG("transport tcp listen begin endpoint={}:{} backlog={}", endpoint.host, endpoint.port, backlog);
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    if (getaddrinfo(host, port.c_str(), &hints, &results) != 0) {
        UC_ERROR("transport tcp listen getaddrinfo failed endpoint={}:{}", endpoint.host, endpoint.port);
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
            UC_DEBUG("transport tcp listen ok endpoint={}:{} socket={}", endpoint.host, endpoint.port, candidate);
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    if (status != Status::Ok) {
        UC_ERROR("transport tcp listen failed endpoint={}:{}", endpoint.host, endpoint.port);
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
        UC_ERROR("transport tcp accept failed");
        return Status::Failed;
    }

    channel.close();
    channel.impl_->socket = accepted;
    if (remote != nullptr) {
        *remote = EndpointFromSockaddr(remote_addr, remote_len);
        UC_DEBUG("transport tcp accept ok remote={}:{} socket={}", remote->host, remote->port, accepted);
    } else {
        UC_DEBUG("transport tcp accept ok socket={}", accepted);
    }
    return Status::Ok;
}

Status TcpControlPlane::connect(const TcpEndpoint& endpoint) {
    if (endpoint.host.empty() || endpoint.port == 0) {
        return Status::InvalidArgument;
    }
    UC_DEBUG("transport tcp connect begin endpoint={}:{}", endpoint.host, endpoint.port);
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const auto port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
        UC_ERROR("transport tcp connect getaddrinfo failed endpoint={}:{}", endpoint.host, endpoint.port);
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
            UC_DEBUG("transport tcp connect ok endpoint={}:{} socket={}", endpoint.host, endpoint.port, candidate);
            break;
        }
        CloseSocket(candidate);
    }

    freeaddrinfo(results);
    if (status != Status::Ok) {
        UC_ERROR("transport tcp connect failed endpoint={}:{}", endpoint.host, endpoint.port);
    }
    return status;
}

Status TcpControlPlane::sendMetadata(const Metadata& metadata) const {
    if (impl_->socket == kInvalidSocket || metadata.size() > UINT32_MAX) {
        return Status::InvalidArgument;
    }
    UC_DEBUG("transport tcp send metadata {}", MetadataSummary(metadata));
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
        UC_DEBUG("transport tcp receive metadata {}", MetadataSummary(metadata));
        return Status::Ok;
    }
    status = RecvAll(impl_->socket, metadata.data(), metadata.size());
    if (status == Status::Ok) {
        UC_DEBUG("transport tcp receive metadata {}", MetadataSummary(metadata));
    }
    return status;
}

bool TcpControlPlane::connected() const {
    return impl_->socket != kInvalidSocket;
}

void TcpControlPlane::closeConnection() {
    UC_DEBUG("transport tcp close connection socket={}", impl_->socket);
    CloseSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
}

void TcpControlPlane::close() {
    UC_DEBUG("transport tcp close socket={} listen_socket={}", impl_->socket, impl_->listen_socket);
    CloseSocket(impl_->socket);
    CloseSocket(impl_->listen_socket);
    impl_->socket = kInvalidSocket;
    impl_->listen_socket = kInvalidSocket;
}

}  // namespace transport
