#include "core/transport_manager.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace transport {
namespace {

bool TransportMatches(const std::string& requested, const std::string& protocol) {
    return requested.empty() || requested == protocol;
}

}  // namespace

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

Status TransportManager::installTransport(const std::string& protocol, const InitAttrs& options) {
    if (protocol_map_.find(protocol) != protocol_map_.end()) {
        return Status::AlreadyExists;
    }

    TransportPtr transport;
    if (protocol == "hixl") {
        transport = std::make_shared<HixlTransport>();
    } else if (protocol == "rdma") {
        transport = std::make_shared<RdmaTransport>();
    } else {
        return Status::InvalidArgument;
    }

    const auto status = transport->init(options);
    if (status != Status::Ok) {
        return status;
    }

    return installInitializedTransport(protocol, std::move(transport));
}

Status TransportManager::installInitializedTransport(const std::string& protocol, TransportPtr transport) {
    if (protocol.empty() || !transport) {
        return Status::InvalidArgument;
    }
    if (protocol_map_.find(protocol) != protocol_map_.end()) {
        return Status::AlreadyExists;
    }

    protocol_map_[protocol] = transport.get();
    transports_.push_back(InstalledTransport{protocol, std::move(transport)});
    return Status::Ok;
}

Status TransportManager::createChannel(const std::vector<TcpEndpoint>& endpoints,
                                       std::vector<PeerID>& peers,
                                       const std::string& transport_name) {
    if (!transport_name.empty() && protocol_map_.find(transport_name) == protocol_map_.end()) {
        return Status::NotFound;
    }

    peers.assign(endpoints.size(), kInvalidPeerID);
    std::vector<Status> statuses(endpoints.size(), Status::Ok);
    std::vector<std::thread> workers;
    workers.reserve(endpoints.size());

    for (size_t i = 0; i < endpoints.size(); ++i) {
        workers.emplace_back([this, &endpoints, &peers, &statuses, &transport_name, i]() {
            statuses[i] = createSingleChannel(endpoints[i], peers[i], transport_name);
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    for (const auto status : statuses) {
        if (status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}

Status TransportManager::createSingleChannel(const TcpEndpoint& endpoint,
                                             PeerID& peer,
                                             const std::string& transport_name) {
    peer = kInvalidPeerID;
    const auto endpoint_key = endpointKey(endpoint);
    if (endpoint_key == endpointKey(config_.endpoint)) {
        return Status::Ok;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
        const auto existing = peers_by_endpoint_.find(endpoint_key);
        if (existing != peers_by_endpoint_.end() && peerConnected(existing->second, transport_name)) {
            peer = existing->second;
            return Status::Ok;
        }
    }

    TcpControlPlane channel;

    if (endpointKey(config_.endpoint) < endpoint_key) {
        const auto status = channel.connect(endpoint);
        if (status != Status::Ok) {
            return status;
        }
    } else {
        const auto status = waitAcceptedChannel(endpoint, channel);
        if (status != Status::Ok) {
            return status;
        }
    }

    return runChannelHandshake(channel, peer, transport_name);
}

Status TransportManager::runChannelHandshake(TcpControlPlane& channel,
                                             PeerID& peer,
                                             const std::string& transport_name) {
    Metadata local;
    auto status = exportInstalledMetadata(local, transport_name);
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

    status = establishPeerFromMetadata(remote, peer, transport_name);
    if (status != Status::Ok) {
        channel.closeConnection();
        return status;
    }

    channel.closeConnection();
    return Status::Ok;
}

Status TransportManager::waitAcceptedChannel(const TcpEndpoint& endpoint, TcpControlPlane& channel) {
    std::unique_lock<std::mutex> lock(accepted_mutex_);
    const auto ready = [this, &endpoint]() {
        const auto it = accepted_channels_by_host_.find(endpoint.host);
        return it != accepted_channels_by_host_.end() && !it->second.empty();
    };

    if (!accepted_cv_.wait_for(lock, std::chrono::seconds(30), ready)) {
        return Status::Failed;
    }

    auto& queue = accepted_channels_by_host_[endpoint.host];
    channel = std::move(queue.front());
    queue.pop_front();
    if (queue.empty()) {
        accepted_channels_by_host_.erase(endpoint.host);
    }
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
    TcpControlPlane channel;
    TcpEndpoint remote;
    auto status = control_.accept(channel, &remote);
    if (status != Status::Ok) {
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(accepted_mutex_);
        accepted_channels_by_host_[remote.host].push_back(std::move(channel));
    }
    accepted_cv_.notify_all();
    return Status::Ok;
}

Status TransportManager::shutdown() {
    stopChannelListener();

    Status result = Status::Ok;
    for (auto& item : transports_) {
        const auto status = item.transport->shutdown();
        if (status != Status::Ok && result == Status::Ok) {
            result = status;
        }
    }
    control_.close();
    {
        std::lock_guard<std::mutex> lock(accepted_mutex_);
        accepted_channels_by_host_.clear();
    }
    peer_states_.clear();
    peers_by_endpoint_.clear();
    protocol_map_.clear();
    transports_.clear();
    return result;
}

Status TransportManager::registerMemory(const std::string& transport_name, const MemoryRegion& memory) {
    if (transport_name.empty()) {
        if (transports_.empty()) {
            return Status::NotFound;
        }
        Status result = Status::Ok;
        for (const auto& item : transports_) {
            const auto status = item.transport->registerMemory(memory);
            if (status != Status::Ok && result == Status::Ok) {
                result = status;
            }
        }
        return result;
    }

    const auto it = protocol_map_.find(transport_name);
    if (it == protocol_map_.end()) {
        return Status::NotFound;
    }
    return it->second->registerMemory(memory);
}

Status TransportManager::unregisterMemory(const std::string& transport_name, const MemoryRegion& memory) {
    if (transport_name.empty()) {
        if (transports_.empty()) {
            return Status::NotFound;
        }
        Status result = Status::Ok;
        for (const auto& item : transports_) {
            const auto status = item.transport->unregisterMemory(memory);
            if (status != Status::Ok && result == Status::Ok) {
                result = status;
            }
        }
        return result;
    }

    const auto it = protocol_map_.find(transport_name);
    return it == protocol_map_.end() ? Status::NotFound : it->second->unregisterMemory(memory);
}

Status TransportManager::submitTransfer(TransferType type, const Transfer& transfer) {
    auto* transport = selectTransport(type, transfer.opcode);
    return transport == nullptr ? Status::NotFound : transport->submitTransfer(transfer);
}

Status TransportManager::exportInstalledMetadata(Metadata& out, const std::string& transport_name) const {
    if (transports_.size() > UINT32_MAX) {
        return Status::InvalidArgument;
    }

    uint32_t selected_count = 0;
    for (const auto& item : transports_) {
        if (TransportMatches(transport_name, item.protocol)) {
            ++selected_count;
        }
    }
    if (selected_count == 0) {
        return Status::NotFound;
    }

    out.clear();
    if (!appendString(out, config_.endpoint.host) ||
        !appendU32(out, static_cast<uint32_t>(config_.endpoint.port)) ||
        !appendU32(out, selected_count)) {
        return Status::InvalidArgument;
    }

    for (const auto& item : transports_) {
        if (!TransportMatches(transport_name, item.protocol)) {
            continue;
        }
        Metadata metadata;
        const auto status = item.transport->exportMetadata(metadata);
        if (status != Status::Ok) {
            return status;
        }
        if (!appendMetadataRecord(out, item.protocol, metadata)) {
            return Status::InvalidArgument;
        }
    }
    return Status::Ok;
}

Status TransportManager::establishPeerFromMetadata(const Metadata& metadata,
                                                   PeerID& peer,
                                                   const std::string& transport_name) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
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
    PeerState* state = nullptr;
    if (existing != peers_by_endpoint_.end()) {
        if (peerConnected(existing->second, transport_name)) {
            peer = existing->second;
            return Status::Ok;
        }
        peer = existing->second;
        state = &peer_states_[peer];
    } else {
        peer = allocatePeerID();
        state = &peer_states_[peer];
        state->endpoint = remote_endpoint;
        peers_by_endpoint_[endpoint_key] = peer;
    }

    bool imported = false;
    for (const auto& record : records) {
        if (!TransportMatches(transport_name, record.first)) {
            continue;
        }
        const auto it = protocol_map_.find(record.first);
        if (it == protocol_map_.end()) {
            continue;
        }

        const auto status = it->second->importMetadata(peer, record.second);
        if (status != Status::Ok) {
            if (state->connected_protocols.empty()) {
                peer_states_.erase(peer);
                peers_by_endpoint_.erase(endpoint_key);
            }
            peer = kInvalidPeerID;
            return status;
        }
        imported = true;
    }

    if (!imported) {
        if (state->connected_protocols.empty()) {
            peer_states_.erase(peer);
            peers_by_endpoint_.erase(endpoint_key);
        }
        peer = kInvalidPeerID;
        return Status::NotFound;
    }

    const auto status = establishDataPlane(peer, transport_name);
    if (status != Status::Ok) {
        if (state->connected_protocols.empty()) {
            peer_states_.erase(peer);
            peers_by_endpoint_.erase(endpoint_key);
        }
        peer = kInvalidPeerID;
        return status;
    }

    return Status::Ok;
}

Status TransportManager::establishDataPlane(PeerID peer, const std::string& transport_name) {
    auto state_it = peer_states_.find(peer);
    if (state_it == peer_states_.end()) {
        return Status::NotFound;
    }

    bool connected = false;
    for (const auto& item : transports_) {
        if (!TransportMatches(transport_name, item.protocol)) {
            continue;
        }
        const auto status = item.transport->connectPeer(peer);
        if (status != Status::Ok) {
            return status;
        }
        state_it->second.connected_protocols.insert(item.protocol);
        connected = true;
    }
    return connected ? Status::Ok : Status::NotFound;
}

Transport* TransportManager::selectTransport(TransferType type, Opcode opcode) const {
    const char* protocol = nullptr;
    switch (type) {
        case TransferType::D2D:
        case TransferType::RD2D:
            return nullptr;
        case TransferType::D2H:
        case TransferType::RD2H:
            if (opcode == Opcode::Send) {
                protocol = "rdma";
            } else if (opcode == Opcode::Read || opcode == Opcode::Write) {
                protocol = "hixl";
            }
            break;
    }

    if (protocol == nullptr) {
        return nullptr;
    }
    const auto it = protocol_map_.find(protocol);
    return it == protocol_map_.end() ? nullptr : it->second;
}

bool TransportManager::peerConnected(PeerID peer, const std::string& transport_name) const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    const auto it = peer_states_.find(peer);
    if (it == peer_states_.end()) {
        return false;
    }
    if (!transport_name.empty()) {
        return it->second.connected_protocols.find(transport_name) != it->second.connected_protocols.end();
    }
    if (transports_.empty()) {
        return false;
    }
    for (const auto& item : transports_) {
        if (it->second.connected_protocols.find(item.protocol) == it->second.connected_protocols.end()) {
            return false;
        }
    }
    return true;
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
