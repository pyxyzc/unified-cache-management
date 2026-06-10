#include "hixl/hixl_transport.h"

#include "transport_internal.h"

#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include "logger/logger.h"

#include "acl/acl.h"
#include "hixl/hixl.h"

namespace transport {
namespace {

std::string EngineHost(const std::string& engine) {
    const auto delimiter = engine.rfind(':');
    return delimiter == std::string::npos ? engine : engine.substr(0, delimiter);
}

std::string EnginePort(const std::string& engine) {
    const auto delimiter = engine.rfind(':');
    return delimiter == std::string::npos ? "" : engine.substr(delimiter + 1);
}

const char* MemoryTypeName(MemoryType type) {
    return type == MemoryType::Device ? "Device" : "Host";
}

const char* OpcodeName(Opcode opcode) {
    switch (opcode) {
        case Opcode::Send:
            return "Send";
        case Opcode::Read:
            return "Read";
        case Opcode::Write:
            return "Write";
        case Opcode::Custom:
            return "Custom";
    }
    return "Unknown";
}

void PrintHixlEndpointDebug(const char* step, const std::string& local_engine, int32_t device_id) {
    UC_DEBUG("transport hixl {} local_engine=\"{}\" host=\"{}\" port=\"{}\" device_id={}",
             step,
             local_engine,
             EngineHost(local_engine),
             EnginePort(local_engine),
             device_id);
}

}  // namespace

struct HixlTransport::Impl {
    struct Peer {
        std::string remote_engine;
        bool connected = false;
    };

    struct LocalMemoryRecord {
        MemoryRegion region;
        hixl::MemHandle native_handle = nullptr;
    };

    std::string local_engine;
    std::map<std::string, std::string> options;
    int32_t connect_timeout_ms = 1000;
    int32_t transfer_timeout_ms = 1000;
    int32_t device_id = 0;
    bool initialized = false;
    std::unordered_map<PeerID, Peer> peers;
    aclrtContext acl_context = nullptr;
    hixl::Hixl hixl;
    std::unordered_map<uint64_t, LocalMemoryRecord> memories;

    std::unordered_map<uint64_t, LocalMemoryRecord>::iterator findMemory(uint64_t address, uint64_t length);
    Status activateAclContext();
};

HixlTransport::HixlTransport() : impl_(std::make_unique<Impl>()) {}

HixlTransport::~HixlTransport() = default;

std::unordered_map<uint64_t, HixlTransport::Impl::LocalMemoryRecord>::iterator HixlTransport::Impl::findMemory(
    uint64_t address,
    uint64_t length) {
    for (auto it = memories.begin(); it != memories.end(); ++it) {
        const auto begin = detail::PtrToU64(it->second.region.addr);
        if (address < begin) {
            continue;
        }
        const auto offset = address - begin;
        if (offset <= it->second.region.length && length <= it->second.region.length - offset) {
            return it;
        }
    }
    return memories.end();
}

Status HixlTransport::Impl::activateAclContext() {
    PrintHixlEndpointDebug("activateAclContext", local_engine, device_id);
    if (acl_context != nullptr) {
        const auto status = aclrtSetCurrentContext(acl_context);
        if (status != ACL_ERROR_NONE) {
            UC_ERROR("transport hixl activate ACL context failed: aclrtSetCurrentContext returned {}",
                     static_cast<int>(status));
            return Status::Failed;
        }
        return Status::Ok;
    }
    const auto status = aclrtSetDevice(device_id);
    if (status != ACL_ERROR_NONE) {
        UC_ERROR("transport hixl activate ACL context failed: aclrtSetDevice({}) returned {}",
                 device_id,
                 static_cast<int>(status));
        return Status::Failed;
    }
    return Status::Ok;
}

const char* HixlTransport::name() const {
    return "hixl";
}

Status HixlTransport::init(const InitAttrs& options) {
    const auto* attrs = dynamic_cast<const HixlInitAttrs*>(&options);
    return attrs == nullptr ? Status::InvalidArgument : init(*attrs);
}

Status HixlTransport::init(const HixlInitAttrs& options) {
    if (options.local_engine.empty()) {
        return Status::InvalidArgument;
    }
    impl_->local_engine = options.local_engine;
    impl_->options = options.options;
    impl_->connect_timeout_ms = options.connect_timeout_ms;
    impl_->transfer_timeout_ms = options.transfer_timeout_ms;
    impl_->device_id = options.device_id;

    PrintHixlEndpointDebug("init begin", impl_->local_engine, impl_->device_id);
    UC_DEBUG("transport hixl init timeouts connect_ms={} transfer_ms={} option_count={}",
             impl_->connect_timeout_ms,
             impl_->transfer_timeout_ms,
             impl_->options.size());
    for (const auto& item : impl_->options) {
        UC_DEBUG("transport hixl init option {}={}", item.first, item.second);
    }

    const auto set_device_status = aclrtSetDevice(impl_->device_id);
    if (set_device_status != ACL_ERROR_NONE) {
        UC_ERROR("transport hixl init failed: aclrtSetDevice({}) returned {}",
                 impl_->device_id,
                 static_cast<int>(set_device_status));
        return Status::Failed;
    }
    std::map<hixl::AscendString, hixl::AscendString> hixl_options;
    for (const auto& item : impl_->options) {
        hixl_options.emplace(item.first.c_str(), item.second.c_str());
    }
    UC_DEBUG("transport hixl calling Initialize local_engine=\"{}\" listen_port=\"{}\" device_id={}",
             impl_->local_engine,
             EnginePort(impl_->local_engine),
             impl_->device_id);
    const auto init_status = impl_->hixl.Initialize(impl_->local_engine.c_str(), hixl_options);
    if (init_status != hixl::SUCCESS) {
        UC_ERROR("transport hixl init failed: Initialize(\"{}\") returned {}",
                 impl_->local_engine,
                 static_cast<int>(init_status));
        (void)aclrtResetDevice(impl_->device_id);
        return Status::Failed;
    }
    const auto context_status = aclrtGetCurrentContext(&impl_->acl_context);
    if (context_status != ACL_ERROR_NONE) {
        UC_ERROR("transport hixl init failed: aclrtGetCurrentContext returned {}",
                 static_cast<int>(context_status));
        impl_->hixl.Finalize();
        (void)aclrtResetDevice(impl_->device_id);
        return Status::Failed;
    }
    impl_->initialized = true;
    UC_DEBUG("transport hixl init ok local_engine=\"{}\" device_id={} acl_context={}",
             impl_->local_engine,
             impl_->device_id,
             impl_->acl_context);
    return Status::Ok;
}

Status HixlTransport::shutdown() {
    UC_DEBUG("transport hixl shutdown local_engine=\"{}\" device_id={} peer_count={} memory_count={}",
             impl_->local_engine,
             impl_->device_id,
             impl_->peers.size(),
             impl_->memories.size());
    if (impl_->initialized) {
        if (impl_->activateAclContext() != Status::Ok) {
            return Status::Failed;
        }
        for (const auto& item : impl_->peers) {
            (void)impl_->hixl.Disconnect(item.second.remote_engine.c_str(), impl_->connect_timeout_ms);
        }
        for (const auto& item : impl_->memories) {
            if (item.second.native_handle != nullptr) {
                (void)impl_->hixl.DeregisterMem(item.second.native_handle);
            }
        }
        impl_->hixl.Finalize();
        (void)aclrtResetDevice(impl_->device_id);
        impl_->initialized = false;
        impl_->acl_context = nullptr;
    }
    impl_->peers.clear();
    impl_->memories.clear();
    return Status::Ok;
}

Status HixlTransport::registerMemory(const MemoryRegion& memory) {
    if (memory.addr == nullptr || memory.length == 0) {
        return Status::InvalidArgument;
    }
    const auto address = detail::PtrToU64(memory.addr);
    if (impl_->memories.find(address) != impl_->memories.end()) {
        return Status::AlreadyExists;
    }

    Impl::LocalMemoryRecord record;
    record.region = memory;
    if (impl_->activateAclContext() != Status::Ok) {
        return Status::Failed;
    }
    UC_DEBUG("transport hixl registerMemory local_engine=\"{}\" addr=0x{:x} length={} type={} "
             "region_device_id={} transport_device_id={}",
             impl_->local_engine,
             address,
             memory.length,
             MemoryTypeName(memory.type),
             memory.device_id,
             impl_->device_id);
    hixl::MemDesc desc{};
    desc.addr = static_cast<uintptr_t>(address);
    desc.len = static_cast<size_t>(memory.length);
    hixl::MemHandle handle = nullptr;
    const auto type = memory.type == MemoryType::Device ? hixl::MEM_DEVICE : hixl::MEM_HOST;
    if (impl_->hixl.RegisterMem(desc, type, handle) != hixl::SUCCESS) {
        return Status::Failed;
    }
    UC_DEBUG("transport hixl registerMemory ok handle={}", handle);
    record.native_handle = handle;
    impl_->memories.emplace(address, record);
    return Status::Ok;
}

Status HixlTransport::unregisterMemory(const MemoryRegion& memory) {
    const auto address = detail::PtrToU64(memory.addr);
    const auto it = impl_->memories.find(address);
    if (it == impl_->memories.end()) {
        return Status::NotFound;
    }
    if (impl_->activateAclContext() != Status::Ok) {
        return Status::Failed;
    }
    UC_DEBUG("transport hixl unregisterMemory local_engine=\"{}\" addr=0x{:x} length={} type={} handle={}",
             impl_->local_engine,
             address,
             it->second.region.length,
             MemoryTypeName(it->second.region.type),
             it->second.native_handle);
    if (it->second.native_handle != nullptr) {
        if (impl_->hixl.DeregisterMem(it->second.native_handle) != hixl::SUCCESS) {
            return Status::Failed;
        }
        it->second.native_handle = nullptr;
    }
    impl_->memories.erase(it);
    return Status::Ok;
}

Status HixlTransport::exportMetadata(Metadata& out) const {
    std::map<std::string, std::string> kv;
    kv["local_engine"] = impl_->local_engine;
    out = detail::PackKV(kv);
    return Status::Ok;
}

Status HixlTransport::importMetadata(PeerID peer, const Metadata& metadata) {
    const auto kv = detail::UnpackKV(metadata);
    const auto engine_it = kv.find("local_engine");
    if (engine_it == kv.end() || engine_it->second.empty()) {
        return Status::InvalidArgument;
    }

    Impl::Peer peer_state;
    peer_state.remote_engine = engine_it->second;

    UC_DEBUG("transport hixl importMetadata peer={} remote_engine=\"{}\" remote_host=\"{}\" remote_port=\"{}\"",
             peer,
             peer_state.remote_engine,
             EngineHost(peer_state.remote_engine),
             EnginePort(peer_state.remote_engine));
    impl_->peers[peer] = std::move(peer_state);
    return Status::Ok;
}

Status HixlTransport::connectPeer(PeerID peer) {
    const auto peer_it = impl_->peers.find(peer);
    if (peer_it == impl_->peers.end()) {
        return Status::NotFound;
    }
    auto& peer_state = peer_it->second;
    if (peer_state.connected) {
        return Status::Ok;
    }
    if (impl_->activateAclContext() != Status::Ok) {
        return Status::Failed;
    }
    UC_DEBUG("transport hixl connectPeer peer={} local_engine=\"{}\" remote_engine=\"{}\" remote_port=\"{}\" "
             "timeout_ms={}",
             peer,
             impl_->local_engine,
             peer_state.remote_engine,
             EnginePort(peer_state.remote_engine),
             impl_->connect_timeout_ms);
    const auto connect_status = impl_->hixl.Connect(peer_state.remote_engine.c_str(), impl_->connect_timeout_ms);
    if (connect_status != hixl::SUCCESS) {
        UC_ERROR("transport hixl connect failed: Connect(\"{}\") returned {}",
                 peer_state.remote_engine,
                 static_cast<int>(connect_status));
        return Status::Failed;
    }
    peer_state.connected = true;
    return Status::Ok;
}

Status HixlTransport::submitTransfer(const Transfer& request) {
    if (request.opcode != Opcode::Read && request.opcode != Opcode::Write) {
        return Status::InvalidArgument;
    }
    const auto local_address = detail::PtrToU64(request.local_addr);
    const auto local_it = impl_->findMemory(local_address, request.length);
    if (local_it == impl_->memories.end() || request.length == 0) {
        return Status::InvalidArgument;
    }
    const auto peer_it = impl_->peers.find(request.target_id);
    if (peer_it == impl_->peers.end()) {
        return Status::NotFound;
    }
    if (!peer_it->second.connected) {
        return Status::InvalidArgument;
    }

    if (impl_->activateAclContext() != Status::Ok) {
        return Status::Failed;
    }
    if (request.remote_addr == 0) {
        return Status::InvalidArgument;
    }
    hixl::TransferOpDesc desc{
        static_cast<uintptr_t>(local_address),
        static_cast<uintptr_t>(request.remote_addr),
        static_cast<size_t>(request.length),
    };
    const auto op = request.opcode == Opcode::Read ? hixl::READ : hixl::WRITE;
    UC_DEBUG("transport hixl submitTransfer opcode={} peer={} remote_engine=\"{}\" local_addr=0x{:x} "
             "remote_addr=0x{:x} length={} timeout_ms={}",
             OpcodeName(request.opcode),
             request.target_id,
             peer_it->second.remote_engine,
             local_address,
             request.remote_addr,
             request.length,
             impl_->transfer_timeout_ms);
    const auto transfer_status = impl_->hixl.TransferSync(peer_it->second.remote_engine.c_str(), op, {desc},
                                                          impl_->transfer_timeout_ms);
    if (transfer_status != hixl::SUCCESS) {
        UC_ERROR("transport hixl transfer failed: TransferSync(\"{}\", {}) returned {}",
                 peer_it->second.remote_engine,
                 OpcodeName(request.opcode),
                 static_cast<int>(transfer_status));
        return Status::Failed;
    }
    return Status::Ok;
}

}  // namespace transport
