#include "hixl_transport.hpp"

#include "transport_internal.hpp"

#include <cstring>
#include <map>
#include <string>

#if defined(TRANSPORT_ENABLE_HIXL)
#include "hixl/hixl.h"
#endif

namespace transport {

struct HixlTransport::Impl {
    struct Peer {
        std::string remote_engine;
    };

    std::string local_engine;
    std::map<std::string, std::string> options;
    int32_t connect_timeout_ms = 1000;
    int32_t transfer_timeout_ms = 1000;
    detail::InMemoryRegistry registry;
    std::unordered_map<PeerID, Peer> peers;

#if defined(TRANSPORT_ENABLE_HIXL)
    hixl::Hixl hixl;
    std::unordered_map<uint64_t, hixl::MemHandle> native_handles;
#else
    std::unordered_map<uint64_t, void*> native_handles;
#endif
};

HixlTransport::HixlTransport() : impl_(std::make_unique<Impl>()) {}

HixlTransport::~HixlTransport() = default;

const char* HixlTransport::name() const {
    return "hixl";
}

Status HixlTransport::init() {
    return init(HixlInitAttrs{});
}

Status HixlTransport::init(const HixlInitAttrs& options) {
    if (options.local_engine.empty()) {
        return Status::InvalidArgument;
    }
    impl_->local_engine = options.local_engine;
    impl_->options = options.options;
    impl_->connect_timeout_ms = options.connect_timeout_ms;
    impl_->transfer_timeout_ms = options.transfer_timeout_ms;

#if defined(TRANSPORT_ENABLE_HIXL)
    std::map<hixl::AscendString, hixl::AscendString> hixl_options;
    for (const auto& item : impl_->options) {
        hixl_options.emplace(item.first.c_str(), item.second.c_str());
    }
    return impl_->hixl.Initialize(impl_->local_engine.c_str(), hixl_options) == hixl::SUCCESS
               ? Status::Ok
               : Status::Failed;
#else
    return Status::Ok;
#endif
}

Status HixlTransport::shutdown() {
#if defined(TRANSPORT_ENABLE_HIXL)
    for (const auto& item : impl_->peers) {
        (void)impl_->hixl.Disconnect(item.second.remote_engine.c_str(), impl_->connect_timeout_ms);
    }
    for (const auto& item : impl_->native_handles) {
        (void)impl_->hixl.DeregisterMem(item.second);
    }
    impl_->hixl.Finalize();
#endif
    impl_->peers.clear();
    impl_->native_handles.clear();
    impl_->registry = detail::InMemoryRegistry{};
    return Status::Ok;
}

Status HixlTransport::registerMemory(const MemoryRegion& memory) {
    const auto status = impl_->registry.Register(memory);
    if (status != Status::Ok) {
        return status;
    }
    const auto address = detail::PtrToU64(memory.addr);

#if defined(TRANSPORT_ENABLE_HIXL)
    hixl::MemDesc desc{};
    desc.addr = static_cast<uintptr_t>(address);
    desc.len = static_cast<size_t>(memory.length);
    hixl::MemHandle handle = nullptr;
    const auto type = memory.type == MemoryType::Device ? hixl::MEM_DEVICE : hixl::MEM_HOST;
    if (impl_->hixl.RegisterMem(desc, type, handle) != hixl::SUCCESS) {
        (void)impl_->registry.Unregister(memory);
        return Status::Failed;
    }
    impl_->native_handles[address] = handle;
#endif
    return Status::Ok;
}

Status HixlTransport::unregisterMemory(const MemoryRegion& memory) {
    const auto address = detail::PtrToU64(memory.addr);
#if defined(TRANSPORT_ENABLE_HIXL)
    const auto it = impl_->native_handles.find(address);
    if (it != impl_->native_handles.end()) {
        if (impl_->hixl.DeregisterMem(it->second) != hixl::SUCCESS) {
            return Status::Failed;
        }
        impl_->native_handles.erase(it);
    }
#endif
    return impl_->registry.Unregister(memory);
}

Status HixlTransport::exportMetadata(Metadata& out) const {
    std::map<std::string, std::string> kv;
    kv["transport"] = name();
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

#if defined(TRANSPORT_ENABLE_HIXL)
    if (impl_->hixl.Connect(peer_state.remote_engine.c_str(), impl_->connect_timeout_ms) != hixl::SUCCESS) {
        return Status::Failed;
    }
#endif
    impl_->peers[peer] = std::move(peer_state);
    return Status::Ok;
}

Status HixlTransport::submitTransfer(const Transfer& request) {
    if (request.opcode == Opcode::Send) {
        return Status::InvalidArgument;
    }
    const auto* local = impl_->registry.Find(request.local_address, request.length);
    if (local == nullptr || local->addr == nullptr || request.length == 0) {
        return Status::InvalidArgument;
    }
    const auto peer_it = impl_->peers.find(request.target_id);
    if (peer_it == impl_->peers.end()) {
        return Status::NotFound;
    }

#if defined(TRANSPORT_ENABLE_HIXL)
    hixl::TransferOpDesc desc{
        static_cast<uintptr_t>(request.local_address),
        static_cast<uintptr_t>(request.target_address),
        static_cast<size_t>(request.length),
    };
    const auto op = request.opcode == Opcode::Read ? hixl::READ : hixl::WRITE;
    return impl_->hixl.TransferSync(peer_it->second.remote_engine.c_str(), op, {desc},
                                    impl_->transfer_timeout_ms) == hixl::SUCCESS
               ? Status::Ok
               : Status::Failed;
#else
    (void)peer_it;
    if (request.opcode == Opcode::Read) {
        std::memcpy(detail::U64ToPtr(request.local_address),
                    detail::U64ToPtr(request.target_address),
                    static_cast<size_t>(request.length));
    } else {
        std::memcpy(detail::U64ToPtr(request.target_address),
                    detail::U64ToPtr(request.local_address),
                    static_cast<size_t>(request.length));
    }
    return Status::Ok;
#endif
}

}  // namespace transport
