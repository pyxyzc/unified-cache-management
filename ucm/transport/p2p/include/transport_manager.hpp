#pragma once

#include "tcp_transport.hpp"
#include "transport.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>

namespace transport {

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

    Status installTransport(TransportPtr transport);
    Status createChannel(const TcpEndpoint& endpoint, PeerID& peer);
    void stopChannelListener();
    Status shutdown();

    Status registerMemory(const std::string& transport_name, const MemoryRegion& memory);
    Status unregisterMemory(const std::string& transport_name, const MemoryRegion& memory);

    Status submitTransfer(const std::string& transport_name, const Transfer& transfer);

   private:
    struct PeerState {
        bool metadata_imported = false;
        bool data_connected = false;
        uint64_t epoch = 0;
        TcpEndpoint endpoint;
    };

    Status exportInstalledMetadata(Metadata& out) const;
    Status onPeerMetadataReceived(const Metadata& metadata, PeerID& peer);
    Status startChannelListener();
    Status handleAcceptedChannel();
    Status establishDataPlane(PeerID peer);
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
    PeerID next_peer_id_ = 1;
    std::unordered_map<PeerID, PeerState> peer_states_;
    std::unordered_map<std::string, PeerID> peers_by_endpoint_;
    std::unordered_map<std::string, Transport*> protocol_map_;
    std::vector<TransportPtr> transports_;
};

}  // namespace transport
