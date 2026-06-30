#include "core/ffts_dispatcher.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace transport {
namespace {

FftsCopySpec MakeCopy(std::array<std::uint8_t, 8>& dst, std::array<std::uint8_t, 8>& src,
                      size_t size = 1)
{
    return FftsCopySpec{dst.data(), src.data(), size};
}

TEST(FftsDispatcherTest, RejectsInvalidBuildInputs)
{
    FftsDispatcher dispatcher;
    std::array<std::uint8_t, 8> src{};
    std::array<std::uint8_t, 8> dst{};

    EXPECT_EQ(Status::InvalidArgument, dispatcher.BuildCopies({}, 8));
    EXPECT_EQ(Status::InvalidArgument, dispatcher.BuildCopies({MakeCopy(dst, src)}, 0));
    EXPECT_EQ(Status::InvalidArgument,
              dispatcher.BuildCopies({FftsCopySpec{nullptr, src.data(), 1}}, 8));
    EXPECT_EQ(Status::InvalidArgument,
              dispatcher.BuildCopies({FftsCopySpec{dst.data(), nullptr, 1}}, 8));
    EXPECT_EQ(Status::InvalidArgument,
              dispatcher.BuildCopies({FftsCopySpec{dst.data(), src.data(), 0}}, 8));
    EXPECT_EQ(Status::InvalidArgument,
              dispatcher.BuildCopies({FftsCopySpec{dst.data(), src.data(),
                                                   static_cast<size_t>(
                                                       std::numeric_limits<uint32_t>::max()) +
                                                       1}},
                                     8));
}

TEST(FftsDispatcherTest, TracksContextAndReadyCounts)
{
    FftsDispatcher dispatcher;
    std::array<std::uint8_t, 8> src{};
    std::array<std::uint8_t, 8> dst{};
    std::vector<FftsCopySpec> copies;
    for (size_t i = 0; i < 5; ++i) { copies.push_back(MakeCopy(dst, src)); }

    EXPECT_EQ(Status::Ok, dispatcher.BuildCopies(copies, 3));
    EXPECT_EQ(5U, dispatcher.ContextCount());
    EXPECT_EQ(3U, dispatcher.ReadyContextNum());

    EXPECT_EQ(Status::Ok, dispatcher.BuildCopies({MakeCopy(dst, src)}, 8));
    EXPECT_EQ(1U, dispatcher.ContextCount());
    EXPECT_EQ(1U, dispatcher.ReadyContextNum());
}

TEST(FftsDispatcherTest, LaunchRejectsInvalidStateOrStream)
{
    FftsDispatcher dispatcher;
    std::array<std::uint8_t, 8> src{};
    std::array<std::uint8_t, 8> dst{};

    EXPECT_EQ(Status::InvalidArgument, dispatcher.Launch(nullptr));
    ASSERT_EQ(Status::Ok, dispatcher.BuildCopies({MakeCopy(dst, src)}, 8));
    EXPECT_EQ(Status::InvalidArgument, dispatcher.Launch(nullptr));
}

}  // namespace
}  // namespace transport
