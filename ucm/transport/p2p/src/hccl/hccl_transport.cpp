#include "hccl/hccl_transport.h"

#include <arpa/inet.h>
#include <hccl/hccl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace transport {
namespace {

constexpr HcclDataType kByteDataType = HCCL_DATA_TYPE_UINT8;

class SocketFd {
   public:
    SocketFd() = default;
    explicit SocketFd(int fd) : fd_(fd) {}
    ~SocketFd() { reset(); }

    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;

    int get() const { return fd_; }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

   private:
    int fd_ = -1;
};

Status ToStatus(HcclResult result) {
    return result == HCCL_SUCCESS ? Status::Ok : Status::Failed;
}

Status ToOsStatus() {
    return Status::Failed;
}

Status BuildSockaddr(const TcpEndpoint& endpoint, sockaddr_in& addr) {
    if (endpoint.host.empty() || endpoint.port == 0) {
        return Status::InvalidArgument;
    }

    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) != 1) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

Status SendAll(int fd, const void* data, size_t bytes) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < bytes) {
        const auto ret = ::send(fd, ptr + sent, bytes - sent, MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ToOsStatus();
        }
        if (ret == 0) {
            return Status::Failed;
        }
        sent += static_cast<size_t>(ret);
    }
    return Status::Ok;
}

Status RecvAll(int fd, void* data, size_t bytes) {
    auto* ptr = static_cast<uint8_t*>(data);
    size_t received = 0;
    while (received < bytes) {
        const auto ret = ::recv(fd, ptr + received, bytes - received, 0);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ToOsStatus();
        }
        if (ret == 0) {
            return Status::Failed;
        }
        received += static_cast<size_t>(ret);
    }
    return Status::Ok;
}

Status GenerateRootInfo(HcclRootInfo& root_info) {
    return ToStatus(HcclGetRootInfo(&root_info));
}

}  // namespace

HcclTransport::~HcclTransport() {
    (void)shutdown();
}

const char* HcclTransport::name() const {
    return "hccl";
}

Status HcclTransport::init(const InitAttrs& options) {
    const auto* attrs = dynamic_cast<const HcclInitAttrs*>(&options);
    return attrs == nullptr ? Status::InvalidArgument : init(*attrs);
}

Status HcclTransport::init(const HcclInitAttrs& options) {
    if (ready()) {
        return Status::AlreadyExists;
    }
    if (options.rank_count == 0) {
        return Status::InvalidArgument;
    }
    if (options.rank >= options.rank_count) {
        return Status::InvalidArgument;
    }
    if (options.root_info_rank >= options.rank_count) {
        return Status::InvalidArgument;
    }
    if (options.bootstrap_role == HcclBootstrapRole::Root && options.rank != options.root_info_rank) {
        return Status::InvalidArgument;
    }
    if (options.bootstrap_role == HcclBootstrapRole::Client && options.rank == options.root_info_rank) {
        return Status::InvalidArgument;
    }

    sockaddr_in addr;
    auto status = BuildSockaddr(options.bootstrap_endpoint, addr);
    if (status != Status::Ok) {
        return status;
    }

    HcclRootInfo root_info;
    if (options.bootstrap_role == HcclBootstrapRole::Root) {
        status = GenerateRootInfo(root_info);
        if (status != Status::Ok) {
            return status;
        }

        SocketFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd.get() < 0) {
            return ToOsStatus();
        }
        int reuse = 1;
        (void)::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(listen_fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            return ToOsStatus();
        }
        if (::listen(listen_fd.get(), static_cast<int>(options.rank_count - 1)) < 0) {
            return ToOsStatus();
        }
        for (uint32_t i = 1; i < options.rank_count; ++i) {
            SocketFd peer_fd(::accept(listen_fd.get(), nullptr, nullptr));
            if (peer_fd.get() < 0) {
                return ToOsStatus();
            }
            status = SendAll(peer_fd.get(), &root_info, sizeof(root_info));
            if (status != Status::Ok) {
                return status;
            }
        }
    } else {
        SocketFd fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd.get() < 0) {
            return ToOsStatus();
        }
        if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            return ToOsStatus();
        }
        status = RecvAll(fd.get(), &root_info, sizeof(root_info));
        if (status != Status::Ok) {
            return status;
        }
    }

    return initHcclComm(root_info, options);
}

Status HcclTransport::shutdown() {
    if (comm_ == nullptr) {
        return Status::Ok;
    }

    const auto status = ToStatus(HcclCommDestroy(comm_));
    if (status != Status::Ok) {
        return status;
    }
    comm_ = nullptr;
    rank_ = 0;
    rank_count_ = 0;
    root_info_rank_ = 0;
    return Status::Ok;
}

Status HcclTransport::submitTransfer(const Transfer& request) {
    (void)request;
    return Status::InvalidArgument;
}

Status HcclTransport::submitCollective(const HcclCollectiveTransfer& transfer) {
    switch (transfer.op) {
        case HcclCollectiveOp::Broadcast:
            return submitBroadcast(transfer);
    }
    return Status::InvalidArgument;
}

bool HcclTransport::ready() const noexcept {
    return comm_ != nullptr;
}

uint32_t HcclTransport::rank() const noexcept {
    return rank_;
}

uint32_t HcclTransport::rankCount() const noexcept {
    return rank_count_;
}

uint32_t HcclTransport::rootInfoRank() const noexcept {
    return root_info_rank_;
}

HcclComm HcclTransport::raw() const noexcept {
    return comm_;
}

Status HcclTransport::ensureReady() const {
    return ready() ? Status::Ok : Status::Failed;
}

Status HcclTransport::initHcclComm(const HcclRootInfo& root_info, const HcclInitAttrs& options) {
    HcclComm comm = nullptr;
    const auto status = ToStatus(HcclCommInitRootInfo(options.rank_count, &root_info, options.rank, &comm));
    if (status != Status::Ok) {
        return status;
    }
    comm_ = comm;
    rank_ = options.rank;
    rank_count_ = options.rank_count;
    root_info_rank_ = options.root_info_rank;
    return Status::Ok;
}

Status HcclTransport::submitBroadcast(const HcclCollectiveTransfer& transfer) {
    auto status = ensureReady();
    if (status != Status::Ok) {
        return status;
    }
    if (transfer.root_rank >= rank_count_) {
        return Status::InvalidArgument;
    }
    if (transfer.length == 0) {
        return Status::Ok;
    }
    if (transfer.buffer == nullptr || transfer.stream == nullptr) {
        return Status::InvalidArgument;
    }

    return ToStatus(HcclBroadcast(transfer.buffer,
                                  transfer.length,
                                  kByteDataType,
                                  transfer.root_rank,
                                  comm_,
                                  transfer.stream));
}

}  // namespace transport
