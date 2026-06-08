#include "transport_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace transport {

TransportManager::TransportManager(TransportManagerConfig config) : config_(std::move(config)) {
    if (config_.endpoint.port != 0) {
        if (control_.listen(config_.endpoint, config_.tcp_backlog) == Status::Ok) {
            (void)startChannelListener();
        }
    }
}

TransportManager::~TransportManager() {
    (void)shutdown();
}

Status TransportManager::installTransport(TransportPtr transport) {
    if (!transport || transport->name() == nullptr || std::string(transport->name()).empty()) {
        return Status::InvalidArgument;
    }

    const auto protocol = std::string(transport->name());
    if (protocol_map_.find(protocol) != protocol_map_.end()) {
        return Status::AlreadyExists;
    }

    protocol_map_[protocol] = transport.get();
    transports_.push_back(std::move(transport));
    return Status::Ok;
}

Status TransportManager::createChannel(const TcpEndpoint& endpoint, PeerID& peer) {
    peer = kInvalidPeerID;
    const auto endpoint_key = endpointKey(endpoint);
    const auto existing = peers_by_endpoint_.find(endpoint_key);
    if (existing != peers_by_endpoint_.end() && peerConnected(existing->second)) {
        peer = existing->second;
        return Status::Ok;
    }

    TcpControlPlane channel;
    auto status = channel.connect(endpoint);
    if (status != Status::Ok) {
        return status;
    }

    Metadata local;
    status = exportInstalledMetadata(local);
    if (status != Status::Ok) {
        channel.closeConnection();
        return status;
    }

    status = channel.sendMetadata(local);
    if (status != Status::Ok) {
        channel.closeConnection();
        return status;
    }

    Metadata remote;
    status = channel.receiveMetadata(remote);
    if (status != Status::Ok) {
        channel.closeConnection();
        return status;
    }

    status = onPeerMetadataReceived(remote, peer);
    if (status != Status::Ok) {
        channel.closeConnection();
        return status;
    }

    channel.closeConnection();
    return Status::Ok;
}

Status TransportManager::startChannelListener() {
    if (listening_.exchange(true)) {
        return Status::AlreadyExists;
    }

    listener_thread_ = std::thread([this]() {
        while (listening_) {
            const auto status = handleAcceptedChannel();
            if (status != Status::Ok && !listening_) {
                break;
            }
        }
    });
    return Status::Ok;
}

void TransportManager::stopChannelListener() {
    if (!listening_.exchange(false)) {
        return;
    }
    control_.close();
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
}

Status TransportManager::handleAcceptedChannel() {
    auto status = control_.accept();
    if (status != Status::Ok) {
        return status;
    }

    Metadata remote;
    status = control_.receiveMetadata(remote);
    if (status != Status::Ok) {
        control_.closeConnection();
        return status;
    }

    PeerID peer = kInvalidPeerID;
    status = onPeerMetadataReceived(remote, peer);
    if (status != Status::Ok) {
        control_.closeConnection();
        return status;
    }

    Metadata local;
    status = exportInstalledMetadata(local);
    if (status == Status::Ok) {
        status = control_.sendMetadata(local);
    }
    control_.closeConnection();
    return status;
}

Status TransportManager::shutdown() {
    stopChannelListener();

    Status result = Status::Ok;
    for (auto& item : transports_) {
        const auto status = item->shutdown();
        if (status != Status::Ok && result == Status::Ok) {
            result = status;
        }
    }
    control_.close();
    peer_states_.clear();
    peers_by_endpoint_.clear();
    protocol_map_.clear();
    transports_.clear();
    return result;
}

Status TransportManager::registerMemory(const std::string& transport_name, const MemoryRegion& memory) {
    const auto it = protocol_map_.find(transport_name);
    if (it == protocol_map_.end()) {
        return Status::NotFound;
    }
    return it->second->registerMemory(memory);
}

Status TransportManager::unregisterMemory(const std::string& transport_name, const MemoryRegion& memory) {
    const auto it = protocol_map_.find(transport_name);
    return it == protocol_map_.end() ? Status::NotFound : it->second->unregisterMemory(memory);
}

Status TransportManager::submitTransfer(const std::string& transport_name, const Transfer& transfer) {
    const auto it = protocol_map_.find(transport_name);
    return it == protocol_map_.end() ? Status::NotFound : it->second->submitTransfer(transfer);
}

Status TransportManager::exportInstalledMetadata(Metadata& out) const {
    if (transports_.size() > UINT32_MAX) {
        return Status::InvalidArgument;
    }

    out.clear();
    if (!appendString(out, config_.endpoint.host) ||
        !appendU32(out, static_cast<uint32_t>(config_.endpoint.port)) ||
        !appendU32(out, static_cast<uint32_t>(transports_.size()))) {
        return Status::InvalidArgument;
    }

    for (const auto& transport : transports_) {
        Metadata metadata;
        const auto status = transport->exportMetadata(metadata);
        if (status != Status::Ok) {
            return status;
        }
        if (!appendMetadataRecord(out, transport->name(), metadata)) {
            return Status::InvalidArgument;
        }
    }
    return Status::Ok;
}

Status TransportManager::onPeerMetadataReceived(const Metadata& metadata, PeerID& peer) {
    peer = kInvalidPeerID;
    if (metadata.size() < sizeof(uint32_t)) {
        return Status::InvalidArgument;
    }

    size_t offset = 0;
    TcpEndpoint remote_endpoint;
    uint32_t remote_port = 0;
    uint32_t count = 0;
    if (!readString(metadata, offset, remote_endpoint.host) ||
        !readU32(metadata, offset, remote_port) ||
        !readU32(metadata, offset, count)) {
        return Status::InvalidArgument;
    }
    if (remote_port > UINT16_MAX) {
        return Status::InvalidArgument;
    }
    remote_endpoint.port = static_cast<uint16_t>(remote_port);

    std::vector<std::pair<std::string, Metadata>> records;
    records.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        Metadata transport_metadata;
        if (!readMetadataRecord(metadata, offset, name, transport_metadata)) {
            return Status::InvalidArgument;
        }
        records.emplace_back(std::move(name), std::move(transport_metadata));
    }

    if (offset != metadata.size()) {
        return Status::InvalidArgument;
    }

    const auto endpoint_key = endpointKey(remote_endpoint);
    const auto existing = peers_by_endpoint_.find(endpoint_key);
    if (existing != peers_by_endpoint_.end()) {
        if (peerConnected(existing->second)) {
            peer = existing->second;
            return Status::Ok;
        }
        return Status::AlreadyExists;
    }

    peer = allocatePeerID();
    auto& state = peer_states_[peer];
    state.endpoint = remote_endpoint;

    for (const auto& record : records) {
        const auto it = protocol_map_.find(record.first);
        if (it == protocol_map_.end()) {
            continue;
        }

        const auto status = it->second->importMetadata(peer, record.second);
        if (status != Status::Ok) {
            peer_states_.erase(peer);
            peer = kInvalidPeerID;
            return status;
        }
    }

    state.metadata_imported = true;

    const auto status = establishDataPlane(peer);
    if (status != Status::Ok) {
        peer_states_.erase(peer);
        peer = kInvalidPeerID;
        return status;
    }

    state.data_connected = true;
    peers_by_endpoint_[endpoint_key] = peer;
    return Status::Ok;
}

Status TransportManager::establishDataPlane(PeerID peer) {
    for (const auto& transport : transports_) {
        const auto status = transport->connectPeer(peer);
        if (status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}

bool TransportManager::peerConnected(PeerID peer) const {
    const auto it = peer_states_.find(peer);
    return it != peer_states_.end() && it->second.metadata_imported && it->second.data_connected;
}

PeerID TransportManager::allocatePeerID() {
    while (next_peer_id_ == kInvalidPeerID || peer_states_.find(next_peer_id_) != peer_states_.end()) {
        ++next_peer_id_;
    }
    return next_peer_id_++;
}

std::string TransportManager::endpointKey(const TcpEndpoint& endpoint) const {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

bool TransportManager::appendU64(Metadata& out, uint64_t value) const {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
    return true;
}

bool TransportManager::readU64(const Metadata& input, size_t& offset, uint64_t& value) const {
    if (offset > input.size() || input.size() - offset < sizeof(uint64_t)) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value = (value << 8) | input[offset + i];
    }
    offset += sizeof(uint64_t);
    return true;
}

bool TransportManager::appendU32(Metadata& out, uint32_t value) const {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
    return true;
}

bool TransportManager::readU32(const Metadata& input, size_t& offset, uint32_t& value) const {
    if (offset > input.size() || input.size() - offset < sizeof(uint32_t)) {
        return false;
    }
    value = (static_cast<uint32_t>(input[offset]) << 24) |
            (static_cast<uint32_t>(input[offset + 1]) << 16) |
            (static_cast<uint32_t>(input[offset + 2]) << 8) |
            static_cast<uint32_t>(input[offset + 3]);
    offset += sizeof(uint32_t);
    return true;
}

bool TransportManager::appendString(Metadata& out, const std::string& value) const {
    if (value.size() > UINT32_MAX) {
        return false;
    }
    appendU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return true;
}

bool TransportManager::readString(const Metadata& input, size_t& offset, std::string& value) const {
    uint32_t size = 0;
    if (!readU32(input, offset, size) || offset > input.size() || input.size() - offset < size) {
        return false;
    }
    value.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                 input.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return true;
}

bool TransportManager::appendMetadataRecord(Metadata& out,
                                            const std::string& name,
                                            const Metadata& metadata) const {
    if (name.size() > UINT32_MAX || metadata.size() > UINT32_MAX) {
        return false;
    }

    appendU32(out, static_cast<uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    appendU32(out, static_cast<uint32_t>(metadata.size()));
    out.insert(out.end(), metadata.begin(), metadata.end());
    return true;
}

bool TransportManager::readMetadataRecord(const Metadata& input,
                                          size_t& offset,
                                          std::string& name,
                                          Metadata& metadata) const {
    uint32_t name_size = 0;
    if (!readU32(input, offset, name_size) || offset > input.size() || input.size() - offset < name_size) {
        return false;
    }
    name.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                input.begin() + static_cast<std::ptrdiff_t>(offset + name_size));
    offset += name_size;

    uint32_t metadata_size = 0;
    if (!readU32(input, offset, metadata_size) || offset > input.size() ||
        input.size() - offset < metadata_size) {
        return false;
    }
    metadata.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                    input.begin() + static_cast<std::ptrdiff_t>(offset + metadata_size));
    offset += metadata_size;
    return true;
}

}  // namespace transport
