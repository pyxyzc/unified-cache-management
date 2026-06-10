#pragma once

#include "hixl_transport.hpp"
#include "rdma_transport.hpp"
#include "tcp_transport.hpp"
#include "transport.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace transport {

enum class TransferType {
    D2D,
    RD2D,
    D2H,
    RD2H,
};

struct TransportManagerConfig {
    TcpEndpoint endpoint;
    int tcp_backlog = 16;
};

class TransportManager {
   public:
    explicit TransportManager(TransportManagerConfig config = {});
    ~TransportManager();

    TransportManager(const TransportManager&) = delete;
    TransportManager& operator=(const TransportManager&) = delete;

    Status installTransport(const std::string& protocol, const InitAttrs& options);

    Status createChannel(const std::vector<TcpEndpoint>& endpoints, std::vector<PeerID>& peers);
    void stopChannelListener();
    Status shutdown();

    Status registerMemory(const std::string& transport_name, const MemoryRegion& memory);
    Status unregisterMemory(const std::string& transport_name, const MemoryRegion& memory);

    Status submitTransfer(TransferType type, const Transfer& transfer);

   private:
    struct InstalledTransport {
        std::string protocol;
        TransportPtr transport;
    };

    struct PeerState {
        bool metadata_imported = false;
        bool data_connected = false;
        uint64_t epoch = 0;
        TcpEndpoint endpoint;
    };

    Status exportInstalledMetadata(Metadata& out) const;
    Status installInitializedTransport(const std::string& protocol, TransportPtr transport);
    Status createSingleChannel(const TcpEndpoint& endpoint, PeerID& peer);
    Status runChannelHandshake(TcpControlPlane& channel, PeerID& peer);
    Status waitAcceptedChannel(const TcpEndpoint& endpoint, TcpControlPlane& channel);
    Status establishPeerFromMetadata(const Metadata& metadata, PeerID& peer);
    Status startChannelListener();
    Status handleAcceptedChannel();
    Status establishDataPlane(PeerID peer);
    Transport* selectTransport(TransferType type, Opcode opcode) const;
    bool peerConnected(PeerID peer) const;
    PeerID allocatePeerID();
    std::string endpointKey(const TcpEndpoint& endpoint) const;
    bool appendU64(Metadata& out, uint64_t value) const;
    bool readU64(const Metadata& input, size_t& offset, uint64_t& value) const;
    bool appendU32(Metadata& out, uint32_t value) const;
    bool readU32(const Metadata& input, size_t& offset, uint32_t& value) const;
    bool appendString(Metadata& out, const std::string& value) const;
    bool readString(const Metadata& input, size_t& offset, std::string& value) const;
    bool appendMetadataRecord(Metadata& out, const std::string& name, const Metadata& metadata) const;
    bool readMetadataRecord(const Metadata& input,
                            size_t& offset,
                            std::string& name,
                            Metadata& metadata) const;

    TransportManagerConfig config_;
    TcpControlPlane control_;
    std::atomic<bool> listening_{false};
    std::thread listener_thread_;
    std::mutex accepted_mutex_;
    std::condition_variable accepted_cv_;
    std::unordered_map<std::string, std::deque<TcpControlPlane>> accepted_channels_by_host_;
    mutable std::recursive_mutex peer_mutex_;
    PeerID next_peer_id_ = 1;
    std::unordered_map<PeerID, PeerState> peer_states_;
    std::unordered_map<std::string, PeerID> peers_by_endpoint_;
    std::unordered_map<std::string, Transport*> protocol_map_;
    std::vector<InstalledTransport> transports_;
};

}  // namespace transport
