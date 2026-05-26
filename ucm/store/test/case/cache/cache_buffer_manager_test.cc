/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include <gtest/gtest.h>
#include "cache/cc/buffer_manager.h"
#include "detail/mock_store.h"
#include "detail/random.h"
#include "detail/types_helper.h"

class UCCacheBufferManagerTest : public testing::Test {
protected:
    static UC::Expected<std::vector<uint8_t>> AllMiss(const UC::Detail::BlockId* blocks, size_t num)
    {
        std::vector<uint8_t> founds(num, false);
        return founds;
    }
    static UC::Expected<std::vector<uint8_t>> AllHit(const UC::Detail::BlockId* blocks, size_t num)
    {
        std::vector<uint8_t> founds(num, true);
        return founds;
    }
    static UC::CacheStore::Config MakeLocalCacheConfig(UC::Test::Detail::MockStore& backend,
                                                       UC::Test::Detail::Random& rd)
    {
        UC::CacheStore::Config config;
        config.storeBackend = &backend;
        config.deviceId = 0;
        config.tensorSizes = {4096};
        config.shardSize = 4096;
        config.blockSize = config.shardSize;
        config.bufferCapacity = config.shardSize * 1024;
        config.uniqueId = rd.RandomString(10);
        config.shareBufferEnable = false;
        return config;
    }
    static void PutLocalHit(UC::CacheStore::BufferManager& bufferMgr,
                            const UC::Detail::BlockId& block)
    {
        auto handle = bufferMgr.GetTransBuffer()->Get(block, 0, true);
        ASSERT_TRUE(handle);
        handle.MarkReady();
    }
};

TEST_F(UCCacheBufferManagerTest, Lookup)
{
    UC::Test::Detail::MockStore backend;
    UC::Test::Detail::Random rd;
    UC::CacheStore::BufferManager bufferMgr;
    UC::CacheStore::Config config;
    config.storeBackend = &backend;
    config.deviceId = 0;
    size_t tensorSize = 4096;
    config.tensorSizes = {4096};
    config.shardSize = tensorSize;
    config.blockSize = config.shardSize;
    config.deviceId = 0;
    config.bufferCapacity = config.shardSize * 1024;
    config.uniqueId = rd.RandomString(10);
    config.shareBufferEnable = true;
    ASSERT_TRUE(bufferMgr.Setup(config).Success());
    std::vector<UC::Detail::BlockId> blocks(3);
    std::for_each(blocks.begin(), blocks.end(), [](auto& block) {
        block = UC::Test::Detail::TypesHelper::MakeBlockIdRandomly();
    });
    EXPECT_CALL(backend, LookupOnPrefix).WillOnce(testing::Return(-1));
    EXPECT_CALL(backend, Lookup).WillOnce(testing::Invoke(AllMiss));
    {
        auto foundIdx = bufferMgr.LookupOnPrefix(blocks.data(), blocks.size()).Value();
        ASSERT_EQ(foundIdx, -1);
        auto founds = bufferMgr.Lookup(blocks.data(), blocks.size()).Value();
        ASSERT_EQ(founds.size(), blocks.size());
        std::for_each(founds.begin(), founds.end(), [](auto found) { ASSERT_FALSE(found); });
    }
    EXPECT_CALL(backend, LookupOnPrefix).WillOnce(testing::Invoke([](auto, size_t num) {
        return static_cast<ssize_t>(num) - 1;
    }));
    EXPECT_CALL(backend, Lookup).WillOnce(testing::Invoke(AllHit));
    {
        auto foundIdx = bufferMgr.LookupOnPrefix(blocks.data(), blocks.size()).Value();
        ASSERT_EQ(foundIdx, 2);
        auto founds = bufferMgr.Lookup(blocks.data(), blocks.size()).Value();
        ASSERT_EQ(founds.size(), blocks.size());
        std::for_each(founds.begin(), founds.end(), [](auto found) { ASSERT_TRUE(found); });
    }
}

TEST_F(UCCacheBufferManagerTest, LookupMergesLocalAndBackendResults)
{
    UC::Test::Detail::MockStore backend;
    UC::Test::Detail::Random rd;
    UC::CacheStore::BufferManager bufferMgr;
    auto config = MakeLocalCacheConfig(backend, rd);
    ASSERT_TRUE(bufferMgr.Setup(config).Success());

    std::vector<UC::Detail::BlockId> blocks(4);
    std::for_each(blocks.begin(), blocks.end(), [](auto& block) {
        block = UC::Test::Detail::TypesHelper::MakeBlockIdRandomly();
    });
    PutLocalHit(bufferMgr, blocks[0]);
    PutLocalHit(bufferMgr, blocks[2]);

    EXPECT_CALL(backend, Lookup(testing::_, 2))
        .WillOnce(testing::Invoke([&blocks](const UC::Detail::BlockId* miss, size_t num) {
            EXPECT_EQ(num, 2);
            EXPECT_EQ(miss[0], blocks[1]);
            EXPECT_EQ(miss[1], blocks[3]);
            return UC::Expected<std::vector<uint8_t>>(std::vector<uint8_t>{true, false});
        }));

    auto founds = bufferMgr.Lookup(blocks.data(), blocks.size()).Value();
    ASSERT_EQ(founds.size(), blocks.size());
    EXPECT_TRUE(founds[0]);
    EXPECT_TRUE(founds[1]);
    EXPECT_TRUE(founds[2]);
    EXPECT_FALSE(founds[3]);
}

TEST_F(UCCacheBufferManagerTest, LookupOnPrefixMapsBackendMissesToOriginalIndex)
{
    UC::Test::Detail::MockStore backend;
    UC::Test::Detail::Random rd;
    UC::CacheStore::BufferManager bufferMgr;
    auto config = MakeLocalCacheConfig(backend, rd);
    ASSERT_TRUE(bufferMgr.Setup(config).Success());

    std::vector<UC::Detail::BlockId> blocks(4);
    std::for_each(blocks.begin(), blocks.end(), [](auto& block) {
        block = UC::Test::Detail::TypesHelper::MakeBlockIdRandomly();
    });
    PutLocalHit(bufferMgr, blocks[0]);
    PutLocalHit(bufferMgr, blocks[2]);

    EXPECT_CALL(backend, LookupOnPrefix(testing::_, 2))
        .WillOnce(testing::Invoke([&blocks](const UC::Detail::BlockId* miss, size_t num) {
            EXPECT_EQ(num, 2);
            EXPECT_EQ(miss[0], blocks[1]);
            EXPECT_EQ(miss[1], blocks[3]);
            return UC::Expected<ssize_t>(static_cast<ssize_t>(0));
        }));

    ASSERT_EQ(bufferMgr.LookupOnPrefix(blocks.data(), blocks.size()).Value(), 2);
}

TEST_F(UCCacheBufferManagerTest, LookupOnPrefixAllLocalHitsSkipsBackend)
{
    UC::Test::Detail::MockStore backend;
    UC::Test::Detail::Random rd;
    UC::CacheStore::BufferManager bufferMgr;
    auto config = MakeLocalCacheConfig(backend, rd);
    ASSERT_TRUE(bufferMgr.Setup(config).Success());

    std::vector<UC::Detail::BlockId> blocks(3);
    std::for_each(blocks.begin(), blocks.end(), [](auto& block) {
        block = UC::Test::Detail::TypesHelper::MakeBlockIdRandomly();
    });
    for (const auto& block : blocks) { PutLocalHit(bufferMgr, block); }

    EXPECT_CALL(backend, LookupOnPrefix).Times(0);
    ASSERT_EQ(bufferMgr.LookupOnPrefix(blocks.data(), blocks.size()).Value(), 2);
}

TEST_F(UCCacheBufferManagerTest, BackendOnlyLookupBypassesCache)
{
    UC::Test::Detail::MockStore backend;
    UC::Test::Detail::Random rd;
    UC::CacheStore::BufferManager bufferMgr;
    UC::CacheStore::Config config;
    config.storeBackend = &backend;
    config.deviceId = 0;
    config.tensorSizes = {4096};
    config.shardSize = 4096;
    config.blockSize = config.shardSize;
    config.bufferCapacity = config.shardSize * 1024;
    config.uniqueId = rd.RandomString(10);
    config.shareBufferEnable = false;
    config.cacheLoadBackendOnly = true;
    ASSERT_TRUE(bufferMgr.Setup(config).Success());

    auto block = UC::Test::Detail::TypesHelper::MakeBlockIdRandomly();
    {
        auto handle = bufferMgr.GetTransBuffer()->Get(block, 0);
        ASSERT_TRUE(handle);
        handle.MarkReady();
        ASSERT_TRUE(bufferMgr.GetTransBuffer()->Exist(block, 0));
    }

    EXPECT_CALL(backend, Lookup).WillOnce(testing::Invoke(AllMiss));
    auto founds = bufferMgr.Lookup(&block, 1).Value();
    ASSERT_EQ(founds.size(), 1);
    ASSERT_FALSE(founds[0]);

    EXPECT_CALL(backend, LookupOnPrefix).WillOnce(testing::Return(-1));
    ASSERT_EQ(bufferMgr.LookupOnPrefix(&block, 1).Value(), -1);
}
