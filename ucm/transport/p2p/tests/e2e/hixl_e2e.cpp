#include "core/transport_manager.h"
#include "test_common.h"

#include <acl/acl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace transport;

namespace {

constexpr uint32_t kRemoteMemoryReady = 1;

struct SharedRemoteMemory {
    volatile uint32_t ready = 0;
    uint32_t reserved = 0;
    volatile uint64_t addr = 0;
    volatile uint64_t length = 0;
};

const char* envText(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

int envInt(const char* name, int fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtol(text, &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<int>(value) : fallback;
}

uint64_t envU64(const char* name, uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<uint64_t>(value) : fallback;
}

class SharedMemoryMapping {
   public:
    SharedMemoryMapping(const char* name, bool create) : name_(name), owner_(create) {
        const int flags = create ? O_CREAT | O_RDWR : O_RDWR;
        fd_ = shm_open(name_, flags, 0600);
        if (fd_ < 0) {
            return;
        }
        if (create && ftruncate(fd_, sizeof(SharedRemoteMemory)) != 0) {
            close();
            return;
        }
        void* mapped = mmap(nullptr, sizeof(SharedRemoteMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) {
            close();
            return;
        }
        data_ = static_cast<SharedRemoteMemory*>(mapped);
        if (create) {
            data_->ready = 0;
            data_->addr = 0;
            data_->length = 0;
        }
    }

    ~SharedMemoryMapping() {
        close();
    }

    SharedMemoryMapping(const SharedMemoryMapping&) = delete;
    SharedMemoryMapping& operator=(const SharedMemoryMapping&) = delete;

    bool ok() const {
        return data_ != nullptr;
    }

    SharedRemoteMemory* get() const {
        return data_;
    }

    void close() {
        if (data_ != nullptr) {
            (void)munmap(data_, sizeof(SharedRemoteMemory));
            data_ = nullptr;
        }
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
        if (owner_ && name_ != nullptr) {
            (void)shm_unlink(name_);
            owner_ = false;
        }
    }

   private:
    const char* name_ = nullptr;
    bool owner_ = false;
    int fd_ = -1;
    SharedRemoteMemory* data_ = nullptr;
};

class AscendDeviceBuffer {
   public:
    explicit AscendDeviceBuffer(size_t size) : size_(size) {
        if (aclrtMalloc(&ptr_, size_, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_ERROR_NONE) {
            ptr_ = nullptr;
        }
    }

    ~AscendDeviceBuffer() {
        reset();
    }

    void reset() {
        if (ptr_ != nullptr) {
            (void)aclrtFree(ptr_);
            ptr_ = nullptr;
        }
    }

    AscendDeviceBuffer(const AscendDeviceBuffer&) = delete;
    AscendDeviceBuffer& operator=(const AscendDeviceBuffer&) = delete;

    void* data() const {
        return ptr_;
    }

    bool ok() const {
        return ptr_ != nullptr;
    }

   private:
    size_t size_ = 0;
    void* ptr_ = nullptr;
};

struct HixlE2eConfig {
    std::string host = envText("HIXL_TEST_HOST", "110.138.0.3");
    uint16_t server_tcp_port = test::envPort("TRANSPORT_TEST_PORT_A", 4501);
    uint16_t client_tcp_port = test::envPort("TRANSPORT_TEST_PORT_B", 4502);
    uint16_t server_hixl_port = test::envPort("HIXL_TEST_PORT_A", 5501);
    uint16_t client_hixl_port = test::envPort("HIXL_TEST_PORT_B", 5502);
    int server_device_id = envInt("HIXL_TEST_DEVICE_A", 4);
    int client_device_id = envInt("HIXL_TEST_DEVICE_B", 5);
    int connect_timeout_ms = envInt("HIXL_TEST_CONNECT_TIMEOUT_MS", 10000);
    int transfer_timeout_ms = envInt("HIXL_TEST_TRANSFER_TIMEOUT_MS", 10000);
    size_t bytes = static_cast<size_t>(envU64("HIXL_TEST_BYTES", 16));
    std::string shm_name = envText("HIXL_TEST_SHM_NAME", "/hixl_e2e_remote_memory");
};

HixlInitAttrs makeHixlAttrs(const HixlE2eConfig& config, bool server) {
    HixlInitAttrs attrs;
    attrs.local_engine = config.host + ":" +
                         std::to_string(server ? config.server_hixl_port : config.client_hixl_port);
    attrs.device_id = server ? config.server_device_id : config.client_device_id;
    attrs.connect_timeout_ms = config.connect_timeout_ms;
    attrs.transfer_timeout_ms = config.transfer_timeout_ms;
    return attrs;
}

TransportManagerConfig makeManagerConfig(const HixlE2eConfig& config, bool server) {
    TransportManagerConfig manager_config;
    manager_config.endpoint = TcpEndpoint{config.host, server ? config.server_tcp_port : config.client_tcp_port};
    return manager_config;
}

TcpEndpoint peerEndpoint(const HixlE2eConfig& config, bool server) {
    return TcpEndpoint{config.host, server ? config.client_tcp_port : config.server_tcp_port};
}

Status connectManagerPeer(TransportManager& manager, const TcpEndpoint& endpoint, PeerID& peer) {
    const auto attempts = envInt("HIXL_TEST_CONNECT_ATTEMPTS", 60);
    const auto interval_ms = envInt("HIXL_TEST_CONNECT_RETRY_MS", 1000);
    Status last_status = Status::Failed;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        std::vector<PeerID> peers;
        last_status = manager.createChannel(std::vector<TcpEndpoint>{endpoint}, peers);
        if (last_status == Status::Ok && peers.size() == 1 && peers[0] != kInvalidPeerID) {
            peer = peers[0];
            return Status::Ok;
        }
        std::cerr << "[HIXL e2e] manager channel attempt " << attempt
                  << " failed: " << test::statusName(last_status) << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return last_status;
}

bool waitRemoteMemory(SharedRemoteMemory* memory, uint64_t& addr, uint64_t& length) {
    const auto attempts = envInt("HIXL_TEST_SHM_ATTEMPTS", 60);
    const auto interval_ms = envInt("HIXL_TEST_SHM_RETRY_MS", 1000);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (memory->ready == kRemoteMemoryReady) {
            addr = memory->addr;
            length = memory->length;
            return addr != 0 && length != 0;
        }
        std::cerr << "[HIXL e2e] waiting for shm remote memory attempt " << attempt << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return false;
}

void publishRemoteMemory(SharedRemoteMemory* memory, uint64_t addr, uint64_t length) {
    memory->addr = addr;
    memory->length = length;
    (void)msync(memory, sizeof(SharedRemoteMemory), MS_SYNC);
    memory->ready = kRemoteMemoryReady;
    (void)msync(memory, sizeof(SharedRemoteMemory), MS_SYNC);
}

int runServer() {
    HixlE2eConfig config;
    if (config.bytes == 0 || config.bytes > 256) {
        std::cerr << "HIXL_TEST_BYTES must be in [1, 256]\n";
        return 1;
    }

    const auto manager_config = makeManagerConfig(config, true);
    const auto hixl_attrs = makeHixlAttrs(config, true);
    std::cerr << "[HIXL e2e server] manager_listen=" << manager_config.endpoint.host << ':'
              << manager_config.endpoint.port << " peer_tcp=" << config.host << ':'
              << config.client_tcp_port << " hixl_engine=\"" << hixl_attrs.local_engine
              << "\" device_id=" << hixl_attrs.device_id << " bytes=" << config.bytes << '\n';

    TransportManager manager(manager_config);
    SharedMemoryMapping shm(config.shm_name.c_str(), true);
    if (!test::expectTrue(shm.ok(), "server create shm remote memory exchange")) {
        return 1;
    }

    if (!test::expectOk(manager.installTransport("hixl", hixl_attrs), "server install HIXL")) {
        return 1;
    }

    std::array<char, 256> host_write{};
    std::array<char, 256> host_read{};
    std::memcpy(host_write.data(), "hixl-e2e", 8);

    MemoryRegion host_write_region;
    host_write_region.addr = host_write.data();
    host_write_region.length = config.bytes;
    host_write_region.type = MemoryType::Host;
    MemoryRegion host_read_region;
    host_read_region.addr = host_read.data();
    host_read_region.length = config.bytes;
    host_read_region.type = MemoryType::Host;

    if (!test::expectOk(manager.registerMemory("hixl", host_write_region), "server register host write memory") ||
        !test::expectOk(manager.registerMemory("hixl", host_read_region), "server register host read memory")) {
        return 1;
    }

    PeerID peer = kInvalidPeerID;
    if (!test::expectOk(connectManagerPeer(manager, peerEndpoint(config, true), peer),
                        "server create manager channel to client")) {
        return 1;
    }

    uint64_t remote_addr = 0;
    uint64_t remote_length = 0;
    if (!test::expectTrue(waitRemoteMemory(shm.get(), remote_addr, remote_length),
                          "server receive client device address from shm")) {
        return 1;
    }
    if (!test::expectTrue(remote_length >= config.bytes, "server remote device memory length is enough")) {
        return 1;
    }
    std::cerr << "[HIXL e2e server] shm remote_addr=0x" << std::hex << remote_addr
              << std::dec << " length=" << remote_length << '\n';

    Transfer transfer;
    transfer.opcode = Opcode::Write;
    transfer.local_addr = host_write.data();
    transfer.target_id = peer;
    transfer.remote_addr = remote_addr;
    transfer.length = config.bytes;
    if (!test::expectOk(manager.submitTransfer(TransferType::D2H, transfer),
                        "server manager routes HIXL write to client device memory")) {
        return 1;
    }

    transfer.opcode = Opcode::Read;
    transfer.local_addr = host_read.data();
    if (!test::expectOk(manager.submitTransfer(TransferType::D2H, transfer),
                        "server manager routes HIXL read from client device memory")) {
        return 1;
    }

    if (!test::expectTrue(std::memcmp(host_write.data(), host_read.data(), config.bytes) == 0,
                          "server read returned bytes written to client device")) {
        return 1;
    }

    if (!test::expectOk(manager.unregisterMemory("hixl", host_read_region), "server unregister host read memory") ||
        !test::expectOk(manager.unregisterMemory("hixl", host_write_region),
                        "server unregister host write memory") ||
        !test::expectOk(manager.shutdown(), "server shutdown manager")) {
        return 1;
    }

    std::cout << "[ PASS ] hixl manager server transferred client device memory\n";
    return 0;
}

int runClient() {
    HixlE2eConfig config;
    if (config.bytes == 0 || config.bytes > 256) {
        std::cerr << "HIXL_TEST_BYTES must be in [1, 256]\n";
        return 1;
    }

    const auto manager_config = makeManagerConfig(config, false);
    const auto hixl_attrs = makeHixlAttrs(config, false);
    std::cerr << "[HIXL e2e client] manager_listen=" << manager_config.endpoint.host << ':'
              << manager_config.endpoint.port << " peer_tcp=" << config.host << ':'
              << config.server_tcp_port << " hixl_engine=\"" << hixl_attrs.local_engine
              << "\" device_id=" << hixl_attrs.device_id << " bytes=" << config.bytes << '\n';

    TransportManager manager(manager_config);
    if (!test::expectOk(manager.installTransport("hixl", hixl_attrs), "client install HIXL")) {
        return 1;
    }

    AscendDeviceBuffer device_buffer(config.bytes);
    if (!test::expectTrue(device_buffer.ok(), "client allocate Ascend device memory")) {
        return 1;
    }

    MemoryRegion device_region;
    device_region.addr = device_buffer.data();
    device_region.length = config.bytes;
    device_region.type = MemoryType::Device;
    device_region.device_id = hixl_attrs.device_id;
    if (!test::expectOk(manager.registerMemory("hixl", device_region), "client register device memory")) {
        return 1;
    }

    SharedMemoryMapping shm(config.shm_name.c_str(), false);
    if (!test::expectTrue(shm.ok(), "client open shm remote memory exchange")) {
        return 1;
    }
    publishRemoteMemory(shm.get(), reinterpret_cast<uint64_t>(device_region.addr), device_region.length);
    std::cerr << "[HIXL e2e client] published shm device_addr=0x" << std::hex
              << reinterpret_cast<uint64_t>(device_region.addr) << std::dec
              << " length=" << device_region.length << '\n';

    PeerID peer = kInvalidPeerID;
    if (!test::expectOk(connectManagerPeer(manager, peerEndpoint(config, false), peer),
                        "client create manager channel to server")) {
        return 1;
    }

    std::cerr << "[HIXL e2e client] manager channel ready peer=" << peer
              << ", waiting for server transfer\n";
    std::string hold_line;
    if (envText("HIXL_TEST_WAIT_STDIN", "0") == std::string("1")) {
        std::getline(std::cin, hold_line);
    } else {
        // Keep the client process alive long enough for the server to issue HIXL transfers.
        const auto hold_ms = envInt("HIXL_TEST_CLIENT_HOLD_MS", 30000);
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }

    if (!test::expectOk(manager.unregisterMemory("hixl", device_region), "client unregister device memory")) {
        return 1;
    }
    device_buffer.reset();
    if (!test::expectOk(manager.shutdown(), "client shutdown manager")) {
        return 1;
    }

    std::cout << "[ PASS ] hixl manager client served device memory\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : envText("HIXL_TEST_ROLE", "");
    if (mode == "server" || mode == "A") {
        return runServer();
    }
    if (mode == "client" || mode == "B") {
        return runClient();
    }

    std::cerr << "usage: " << argv[0] << " server|client\n";
    return 2;
}
