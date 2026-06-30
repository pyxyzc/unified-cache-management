#include "core/ffts_transport.h"
#include "core/transport_manager.h"
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

namespace transport {
namespace {

struct SubmittedBatch {
    int device_id = -1;
    std::vector<FftsCopySpec> copies;
};

struct FakeEngineState {
    std::vector<std::pair<int, uint16_t>> inits;
    std::vector<int> shutdowns;
    std::vector<std::pair<int, void*>> host_registrations;
    std::vector<std::pair<int, void*>> device_registrations;
    std::vector<SubmittedBatch> submissions;
};

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

uintptr_t HostVisibleOffset(int device_id)
{
    return static_cast<uintptr_t>(device_id + 1) * 0x100000U;
}

void* HostVisibleAddress(void* host, int device_id)
{
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(host) +
                                   HostVisibleOffset(device_id));
}

FftsTransport::EngineHooks MakeHooks(const std::shared_ptr<FakeEngineState>& state)
{
    FftsTransport::EngineHooks hooks;
    hooks.init = [state](int device_id, uint16_t max_ready_lanes) {
        state->inits.emplace_back(device_id, max_ready_lanes);
        return Status::Ok;
    };
    hooks.shutdown = [state](int device_id) {
        state->shutdowns.push_back(device_id);
        return Status::Ok;
    };
    hooks.register_host = [state](int device_id, void* host, size_t size,
                                  FftsMemoryRegistration& registration) {
        state->host_registrations.emplace_back(device_id, host);
        registration.origin_addr = host;
        registration.ffts_addr = HostVisibleAddress(host, device_id);
        registration.size = size;
        registration.requires_unregister = true;
        return Status::Ok;
    };
    hooks.unregister_host = [](int, const FftsMemoryRegistration&) { return Status::Ok; };
    hooks.register_device = [state](int device_id, void* device, size_t size,
                                    FftsMemoryRegistration& registration) {
        state->device_registrations.emplace_back(device_id, device);
        registration.origin_addr = device;
        registration.ffts_addr = device;
        registration.size = size;
        registration.requires_unregister = false;
        return Status::Ok;
    };
    hooks.unregister_device = [](int, const FftsMemoryRegistration&) { return Status::Ok; };
    hooks.submit = [state](int device_id, const std::vector<FftsCopySpec>& copies) {
        state->submissions.push_back(SubmittedBatch{device_id, copies});
        return Status::Ok;
    };
    return hooks;
}

void AppendU32(Metadata& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

TEST(FftsTransportTest, RejectsInvalidAttrsBeforeRuntimeUse)
{
    FftsTransport transport;
    FftsInitAttrs attrs;

    attrs.max_ready_lanes = 0;
    EXPECT_EQ(Status::InvalidArgument, transport.Init(attrs));

    attrs.max_ready_lanes = 8;
    attrs.device_ids = {-1};
    EXPECT_EQ(Status::InvalidArgument, transport.Init(attrs));

    attrs.device_ids = {0, 0};
    EXPECT_EQ(Status::InvalidArgument, transport.Init(attrs));
}

TEST(FftsTransportTest, ImportMetadataValidatesVersionOnlyEncoding)
{
    FftsTransport transport;
    Metadata metadata;

    AppendU32(metadata, 1);
    EXPECT_EQ(Status::Ok, transport.ImportMetadata("peer", metadata));

    metadata.clear();
    AppendU32(metadata, 2);
    EXPECT_EQ(Status::InvalidArgument, transport.ImportMetadata("peer", metadata));

    metadata.clear();
    AppendU32(metadata, 1);
    AppendU32(metadata, 0);
    EXPECT_EQ(Status::InvalidArgument, transport.ImportMetadata("peer", metadata));
}

TEST(FftsTransportTest, RoutesLocalDeviceDirectsToFfts)
{
    EXPECT_STREQ(kFftsTransportProtocol,
                 SelectTransportForDirectForTest(OperationDirect::LocalDeviceHost));
    EXPECT_STREQ(kFftsTransportProtocol,
                 SelectTransportForDirectForTest(OperationDirect::LocalDeviceDevice));
    EXPECT_EQ(nullptr, SelectTransportForDirectForTest(OperationDirect::RemoteDeviceHost));
}

TEST(FftsTransportTest, LocalDeviceHostUsesDeviceSpecificHostRegistration)
{
    auto state = std::make_shared<FakeEngineState>();
    FftsTransport transport(MakeHooks(state));
    FftsInitAttrs attrs;
    attrs.device_ids = {0, 1};
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<std::uint8_t, 16> host{};
    std::array<std::uint8_t, 16> device{};
    MemoryHandle host_handle = kInvalidMemoryHandle;
    MemoryHandle device_handle = kInvalidMemoryHandle;
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{host.data(), host.size(), MemoryType::Host, -1}, host_handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{device.data(), device.size(), MemoryType::Device, 1},
                  device_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceHost;
    op.ops.push_back(Segment{host.data(), PtrToU64(device.data()), 8});

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    ASSERT_EQ(2U, state->host_registrations.size());
    ASSERT_EQ(1U, state->device_registrations.size());
    ASSERT_EQ(1U, state->submissions.size());
    EXPECT_EQ(1, state->submissions[0].device_id);
    ASSERT_EQ(1U, state->submissions[0].copies.size());
    EXPECT_EQ(device.data(), state->submissions[0].copies[0].dst);
    EXPECT_EQ(static_cast<const void*>(HostVisibleAddress(host.data(), 1)),
              state->submissions[0].copies[0].src);
    EXPECT_EQ(8U, state->submissions[0].copies[0].size);
}

TEST(FftsTransportTest, LocalDeviceDeviceSubmitsSameDeviceIdentityCopy)
{
    auto state = std::make_shared<FakeEngineState>();
    FftsTransport transport(MakeHooks(state));
    FftsInitAttrs attrs;
    attrs.device_ids = {0};
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<std::uint8_t, 16> src{};
    std::array<std::uint8_t, 16> dst{};
    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{src.data(), src.size(), MemoryType::Device, 0}, src_handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{dst.data(), dst.size(), MemoryType::Device, 0}, dst_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(Segment{src.data(), PtrToU64(dst.data()), 8});

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    ASSERT_EQ(1U, state->submissions.size());
    EXPECT_EQ(0, state->submissions[0].device_id);
    ASSERT_EQ(1U, state->submissions[0].copies.size());
    EXPECT_EQ(dst.data(), state->submissions[0].copies[0].dst);
    EXPECT_EQ(static_cast<const void*>(src.data()), state->submissions[0].copies[0].src);
    EXPECT_EQ(8U, state->submissions[0].copies[0].size);
}

TEST(FftsTransportTest, LocalDeviceDeviceRejectsCrossDeviceCopy)
{
    auto state = std::make_shared<FakeEngineState>();
    FftsTransport transport(MakeHooks(state));
    FftsInitAttrs attrs;
    attrs.device_ids = {0, 1};
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<std::uint8_t, 16> src{};
    std::array<std::uint8_t, 16> dst{};
    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{src.data(), src.size(), MemoryType::Device, 0}, src_handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{dst.data(), dst.size(), MemoryType::Device, 1}, dst_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(Segment{src.data(), PtrToU64(dst.data()), 8});

    EXPECT_EQ(Status::InvalidArgument, transport.Execute(op));
    EXPECT_TRUE(state->submissions.empty());
}

TEST(FftsTransportTest, LocalDeviceDeviceRejectsHostHost)
{
    auto state = std::make_shared<FakeEngineState>();
    FftsTransport transport(MakeHooks(state));
    FftsInitAttrs attrs;
    attrs.device_ids = {0};
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<std::uint8_t, 16> src{};
    std::array<std::uint8_t, 16> dst{};
    MemoryHandle src_handle = kInvalidMemoryHandle;
    MemoryHandle dst_handle = kInvalidMemoryHandle;
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{src.data(), src.size(), MemoryType::Host, -1}, src_handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{dst.data(), dst.size(), MemoryType::Host, -1}, dst_handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(Segment{src.data(), PtrToU64(dst.data()), 8});

    EXPECT_EQ(Status::InvalidArgument, transport.Execute(op));
    EXPECT_TRUE(state->submissions.empty());
}

TEST(FftsTransportTest, LocalDeviceDeviceGroupsBatchByDevice)
{
    auto state = std::make_shared<FakeEngineState>();
    FftsTransport transport(MakeHooks(state));
    FftsInitAttrs attrs;
    attrs.device_ids = {1, 0};
    ASSERT_EQ(Status::Ok, transport.Init(attrs));

    std::array<std::uint8_t, 16> src0{};
    std::array<std::uint8_t, 16> dst0{};
    std::array<std::uint8_t, 16> src1{};
    std::array<std::uint8_t, 16> dst1{};
    MemoryHandle handle = kInvalidMemoryHandle;
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{src0.data(), src0.size(), MemoryType::Device, 0}, handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{dst0.data(), dst0.size(), MemoryType::Device, 0}, handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{src1.data(), src1.size(), MemoryType::Device, 1}, handle));
    ASSERT_EQ(Status::Ok,
              transport.RegisterMemory(
                  MemoryRegion{dst1.data(), dst1.size(), MemoryType::Device, 1}, handle));

    Operation op;
    op.opcode = Opcode::Write;
    op.direct = OperationDirect::LocalDeviceDevice;
    op.ops.push_back(Segment{src0.data(), PtrToU64(dst0.data()), 8});
    op.ops.push_back(Segment{src1.data(), PtrToU64(dst1.data()), 8});

    ASSERT_EQ(Status::Ok, transport.Execute(op));
    ASSERT_EQ(2U, state->submissions.size());
    EXPECT_EQ(0, state->submissions[0].device_id);
    ASSERT_EQ(1U, state->submissions[0].copies.size());
    EXPECT_EQ(dst0.data(), state->submissions[0].copies[0].dst);
    EXPECT_EQ(static_cast<const void*>(src0.data()), state->submissions[0].copies[0].src);
    EXPECT_EQ(1, state->submissions[1].device_id);
    ASSERT_EQ(1U, state->submissions[1].copies.size());
    EXPECT_EQ(dst1.data(), state->submissions[1].copies[0].dst);
    EXPECT_EQ(static_cast<const void*>(src1.data()), state->submissions[1].copies[0].src);
}

}  // namespace
}  // namespace transport
