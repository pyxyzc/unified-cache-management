#include "transport_manager.hpp"
#include "test_common.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace transport;

int main() {
    const uint16_t port_a = test::envPort("TRANSPORT_TEST_PORT_A", 45001);
    const uint16_t port_b = test::envPort("TRANSPORT_TEST_PORT_B", 45002);

    TransportManagerConfig config_a;
    config_a.endpoint = TcpEndpoint{"127.0.0.1", port_a};
    TransportManagerConfig config_b;
    config_b.endpoint = TcpEndpoint{"127.0.0.1", port_b};

    TransportManager manager_a(config_a);
    TransportManager manager_b(config_b);

    RdmaInitAttrs rdma_attrs_a;
    rdma_attrs_a.psn = 101;
    rdma_attrs_a.receive_callback = [](const RdmaReceiveMessage&) { return Status::Ok; };
    RdmaInitAttrs rdma_attrs_b;
    rdma_attrs_b.psn = 202;
    rdma_attrs_b.receive_callback = [](const RdmaReceiveMessage&) { return Status::Ok; };

    if (!test::expectOk(manager_a.installTransport("rdma", rdma_attrs_a), "install RDMA transport A") ||
        !test::expectOk(manager_b.installTransport("rdma", rdma_attrs_b), "install RDMA transport B")) {
        return 1;
    }

    std::array<char, 16> local{};
    std::array<char, 16> remote{};

    MemoryRegion local_region;
    local_region.addr = local.data();
    local_region.length = local.size();
    MemoryRegion remote_region;
    remote_region.addr = remote.data();
    remote_region.length = remote.size();

    if (!test::expectOk(manager_a.registerMemory("rdma", local_region), "register A RDMA local memory") ||
        !test::expectOk(manager_b.registerMemory("rdma", remote_region), "register B RDMA remote memory")) {
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<PeerID> peers_a;
    std::vector<PeerID> peers_b;
    Status status_a = Status::Failed;
    Status status_b = Status::Failed;
    std::thread connect_a([&]() {
        status_a = manager_a.createChannel(std::vector<TcpEndpoint>{config_b.endpoint}, peers_a);
    });
    std::thread connect_b([&]() {
        status_b = manager_b.createChannel(std::vector<TcpEndpoint>{config_a.endpoint}, peers_b);
    });
    connect_a.join();
    connect_b.join();

    if (!test::expectOk(status_a, "A createChannel(B) over TCP") ||
        !test::expectOk(status_b, "B createChannel(A) over TCP") ||
        !test::expectTrue(peers_a.size() == 1 && peers_a[0] != kInvalidPeerID, "A peer id allocated") ||
        !test::expectTrue(peers_b.size() == 1 && peers_b[0] != kInvalidPeerID, "B peer id allocated")) {
        return 1;
    }
    const PeerID peer_a = peers_a[0];

    Transfer transfer;
    transfer.opcode = Opcode::Send;
    transfer.local_addr = local.data();
    transfer.target_id = peer_a;
    transfer.remote_addr = reinterpret_cast<uint64_t>(remote.data());
    transfer.length = local.size();
    if (!test::expectOk(manager_a.submitTransfer(TransferType::RD2H, transfer),
                        "manager RD2H routes send through RDMA")) {
        return 1;
    }

    if (!test::expectOk(manager_a.unregisterMemory("rdma", local_region), "unregister A RDMA local memory") ||
        !test::expectOk(manager_b.unregisterMemory("rdma", remote_region), "unregister B RDMA remote memory") ||
        !test::expectOk(manager_a.shutdown(), "shutdown manager A") ||
        !test::expectOk(manager_b.shutdown(), "shutdown manager B")) {
        return 1;
    }

    std::cout << "[ PASS ] manager tcp rdma e2e\n";
    return 0;
}
