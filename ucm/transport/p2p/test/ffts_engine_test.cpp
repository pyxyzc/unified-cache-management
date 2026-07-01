#include "core/ffts_engine.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>

namespace transport {
namespace {

FftsCopySpec MakeCopy(std::array<std::uint8_t, 8>& dst, std::array<std::uint8_t, 8>& src,
                      size_t size = 1)
{
    return FftsCopySpec{dst.data(), src.data(), size};
}

TEST(FftsEngineTest, RejectsInvalidOptionsWithoutTouchingRuntime)
{
    FftsEngine engine;

    FftsEngineOptions options;
    options.device_id = -1;
    options.max_ready_lanes = 8;
    EXPECT_EQ(Status::InvalidArgument, engine.Init(options));

    options.device_id = 0;
    options.max_ready_lanes = 0;
    EXPECT_EQ(Status::InvalidArgument, engine.Init(options));
}

TEST(FftsEngineTest, UninitializedSubmitAndSynchronizeFail)
{
    FftsEngine engine;
    std::array<std::uint8_t, 8> src{};
    std::array<std::uint8_t, 8> dst{};

    EXPECT_EQ(Status::Failed, engine.Submit({MakeCopy(dst, src)}));
    EXPECT_EQ(Status::Failed, engine.Synchronize());
    EXPECT_EQ(Status::Ok, engine.Shutdown());
}

TEST(FftsEngineTest, RejectsInvalidMemoryRegistrationInputs)
{
    FftsEngine engine;
    std::array<std::uint8_t, 8> memory{};
    FftsMemoryRegistration registration;

    EXPECT_EQ(Status::InvalidArgument,
              engine.RegisterHostMemory(nullptr, memory.size(), registration));
    EXPECT_EQ(Status::InvalidArgument,
              engine.RegisterHostMemory(memory.data(), 0, registration));
    EXPECT_EQ(Status::InvalidArgument,
              engine.MapRegisteredHostMemory(nullptr, memory.size(), registration));
    EXPECT_EQ(Status::InvalidArgument,
              engine.MapRegisteredHostMemory(memory.data(), 0, registration));
    EXPECT_EQ(Status::InvalidArgument,
              engine.RegisterDeviceMemory(nullptr, memory.size(), registration));
    EXPECT_EQ(Status::InvalidArgument,
              engine.RegisterDeviceMemory(memory.data(), 0, registration));
}

TEST(FftsEngineTest, UninitializedHostMemoryRegistrationFails)
{
    FftsEngine engine;
    std::array<std::uint8_t, 8> memory{};
    FftsMemoryRegistration registration;

    EXPECT_EQ(Status::Failed,
              engine.RegisterHostMemory(memory.data(), memory.size(), registration));
    EXPECT_EQ(Status::Failed,
              engine.MapRegisteredHostMemory(memory.data(), memory.size(), registration));
    EXPECT_EQ(nullptr, registration.origin_addr);
    EXPECT_EQ(nullptr, registration.ffts_addr);
    EXPECT_EQ(0U, registration.size);
    EXPECT_FALSE(registration.requires_unregister);
}

TEST(FftsEngineTest, DeviceMemoryRegistrationIsNoopMapping)
{
    FftsEngine engine;
    std::array<std::uint8_t, 8> memory{};
    FftsMemoryRegistration registration;

    EXPECT_EQ(Status::Ok,
              engine.RegisterDeviceMemory(memory.data(), memory.size(), registration));
    EXPECT_EQ(memory.data(), registration.origin_addr);
    EXPECT_EQ(memory.data(), registration.ffts_addr);
    EXPECT_EQ(memory.size(), registration.size);
    EXPECT_FALSE(registration.requires_unregister);
    EXPECT_EQ(Status::Ok, engine.UnregisterDeviceMemory(registration));
}

}  // namespace
}  // namespace transport
