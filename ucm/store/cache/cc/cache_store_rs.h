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
#include <sys/types.h>

namespace UC::CacheStore::Rs {

inline constexpr int32_t STATUS_OK = 0;
inline constexpr int32_t STATUS_ERROR = 1;
inline constexpr int32_t STATUS_INVALID_PARAM = 2;
inline constexpr size_t MESSAGE_CAPACITY = 256;

struct Status {
    int32_t code;
    char message[MESSAGE_CAPACITY];
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

extern "C" Core* ucm_cache_store_core_new(const ConfigView* config, Status* status);
extern "C" void ucm_cache_store_core_free(Core* core);
extern "C" bool ucm_cache_store_core_trans_enabled(const Core* core);
extern "C" size_t ucm_cache_store_next_task_id();

}  // namespace UC::CacheStore::Rs

#endif
