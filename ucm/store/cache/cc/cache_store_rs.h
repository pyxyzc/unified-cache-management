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
#ifndef UNIFIEDCACHE_CACHE_STORE_CC_CACHE_STORE_RS_H
#define UNIFIEDCACHE_CACHE_STORE_CC_CACHE_STORE_RS_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <sys/types.h>
#include <utility>
#include "status/status.h"

namespace UC::CacheStore::Rs {

inline constexpr int32_t STATUS_OK = 0;
inline constexpr int32_t STATUS_ERROR = 1;
inline constexpr int32_t STATUS_INVALID_PARAM = 2;
inline constexpr size_t MESSAGE_CAPACITY = 256;

struct Status {
    int32_t code;
    char message[MESSAGE_CAPACITY];
};

struct BlockId {
    uint8_t bytes[16];
};

struct TransBufferMetaNode {
    BlockId block;
    size_t shard;
    size_t reference;
    size_t hash;
    size_t prev;
    size_t next;
    bool ready;

    void Init() noexcept
    {
        reference = 0;
        hash = std::numeric_limits<size_t>::max();
        prev = std::numeric_limits<size_t>::max();
        next = std::numeric_limits<size_t>::max();
        ready = false;
    }
};

struct TransBufferStrategyView {
    void* ctx;
    size_t (*bucketOf)(void* ctx, const BlockId* block, size_t shard);
    void (*bucketLock)(void* ctx, size_t iBucket);
    bool (*bucketTryLock)(void* ctx, size_t iBucket);
    void (*bucketUnlock)(void* ctx, size_t iBucket);
    void (*nodeLock)(void* ctx, size_t iNode);
    void (*nodeUnlock)(void* ctx, size_t iNode);
    size_t* (*firstAt)(void* ctx, size_t iBucket);
    size_t (*fetchNode)(void* ctx, bool allowReserved);
    TransBufferMetaNode* (*metaAt)(void* ctx, size_t iNode);
};

struct TransBufferGetResult {
    size_t index;
    bool owner;
};

struct ConfigView {
    bool storeBackendPresent;
    const uint8_t* uniqueIdPtr;
    size_t uniqueIdLen;
    int32_t deviceId;
    const size_t* tensorSizesPtr;
    size_t tensorSizesLen;
    size_t shardSize;
    size_t blockSize;
    const ssize_t* cpuAffinityCoresPtr;
    size_t cpuAffinityCoresLen;
    size_t bufferCapacity;
    size_t loadExclusiveBufferNumber;
    size_t waitingQueueDepth;
    size_t runningQueueDepth;
    size_t streamNumber;
    size_t cpuSetSize;
};

struct Core;
struct TransBufferCore;

extern "C" Core* ucm_cache_store_core_new(const ConfigView* config, Status* status);
extern "C" void ucm_cache_store_core_free(Core* core);
extern "C" bool ucm_cache_store_core_trans_enabled(const Core* core);
extern "C" size_t ucm_cache_store_next_task_id();
extern "C" TransBufferCore* ucm_cache_store_trans_buffer_new(
    const TransBufferStrategyView* strategy, bool bypassHitOnLoad, Status* status);
extern "C" void ucm_cache_store_trans_buffer_free(TransBufferCore* core);
extern "C" void ucm_cache_store_trans_buffer_get(const TransBufferCore* core,
                                                 const BlockId* block, size_t shard,
                                                 bool allowReserved, bool isLoad,
                                                 TransBufferGetResult* out, Status* status);
extern "C" void ucm_cache_store_trans_buffer_exist(const TransBufferCore* core,
                                                   const BlockId* block, size_t shard, bool* out,
                                                   Status* status);
extern "C" void ucm_cache_store_trans_buffer_acquire(const TransBufferCore* core, size_t pos);
extern "C" void ucm_cache_store_trans_buffer_release(const TransBufferCore* core, size_t pos);
extern "C" bool ucm_cache_store_trans_buffer_ready(const TransBufferCore* core, size_t pos);
extern "C" void ucm_cache_store_trans_buffer_mark_ready(const TransBufferCore* core, size_t pos);
extern "C" void ucm_cache_store_trans_buffer_mark_not_ready(const TransBufferCore* core,
                                                            size_t pos);
extern "C" void ucm_cache_store_lookup_collect_misses(const BlockId* blocks,
                                                       const uint8_t* localHits, size_t num,
                                                       BlockId* missBlocks,
                                                       size_t* missIndices, size_t* missCount,
                                                       Status* status);
extern "C" void ucm_cache_store_lookup_merge(uint8_t* results, size_t num,
                                             const size_t* missIndices, size_t missCount,
                                             const uint8_t* backendHits, size_t backendHitsLen,
                                             Status* status);
extern "C" void ucm_cache_store_lookup_prefix_result(size_t num, const size_t* missIndices,
                                                     size_t missCount, ssize_t backendPrefix,
                                                     ssize_t* outPrefix, Status* status);

inline std::string MessageFrom(const Status& status)
{
    size_t len = 0;
    while (len < MESSAGE_CAPACITY && status.message[len] != '\0') { ++len; }
    return std::string{status.message, len};
}

inline UC::Status ToUcStatus(const Status& status)
{
    if (status.code == STATUS_OK) { return UC::Status::OK(); }
    auto message = MessageFrom(status);
    if (status.code == STATUS_INVALID_PARAM) {
        return UC::Status::InvalidParam(std::move(message));
    }
    return UC::Status::Error(std::move(message));
}

}  // namespace UC::CacheStore::Rs

#endif
