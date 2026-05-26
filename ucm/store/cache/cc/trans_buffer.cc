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
#include "trans_buffer.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <unistd.h>
#include "logger/logger.h"
#include "trans/buffer.h"
#include "trans/device.h"

namespace UC::CacheStore {

static_assert(sizeof(Detail::BlockId) == sizeof(Rs::BlockId));
static_assert(alignof(Detail::BlockId) == alignof(Rs::BlockId));

namespace {

struct HostBufferHandle {
    std::shared_ptr<void> data;
};

static const Rs::BlockId* ToRsBlockId(const Detail::BlockId& block)
{
    return reinterpret_cast<const Rs::BlockId*>(&block);
}

static void SetRsStatus(Rs::Status* status, const Status& value)
{
    if (!status) { return; }
    status->code = value.Success() ? Rs::STATUS_OK : Rs::STATUS_ERROR;
    std::memset(status->message, 0, Rs::MESSAGE_CAPACITY);
    if (value.Success()) { return; }
    const auto message = value.ToString();
    const auto size = std::min(message.size(), Rs::MESSAGE_CAPACITY - 1);
    std::memcpy(status->message, message.data(), size);
}

static void MakeLocalHostBuffer(void*, int32_t deviceId, size_t size, bool ioDirect,
                                void** outData, void** outHandle, Rs::Status* status)
{
    if (!outData || !outHandle) {
        SetRsStatus(status, Status::InvalidParam("invalid host buffer output"));
        return;
    }
    *outData = nullptr;
    *outHandle = nullptr;
    try {
        Trans::Device device;
        auto s = device.Setup(deviceId);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Failed({}) to setup device({}).", s, deviceId);
            SetRsStatus(status, s);
            return;
        }
        auto buffer = device.MakeBuffer();
        if (!buffer) [[unlikely]] {
            UC_ERROR("Failed to make buffer on device({}).", deviceId);
            SetRsStatus(status, Status::Error("failed to make device buffer"));
            return;
        }
        auto data =
            ioDirect ? buffer->MakeHostBuffer4DirectIo(size) : buffer->MakeHostBuffer(size);
        if (!data) [[unlikely]] {
            UC_ERROR("Failed to make pinned({}) for device({}).", size, deviceId);
            SetRsStatus(status, Status::OutOfMemory());
            return;
        }
        auto handle = std::make_unique<HostBufferHandle>();
        handle->data = std::move(data);
        *outData = handle->data.get();
        *outHandle = handle.release();
        SetRsStatus(status, Status::OK());
    } catch (const std::exception& e) {
        SetRsStatus(status, Status::Error(e.what()));
    }
}

static void FreeLocalHostBuffer(void*, void* handle)
{
    delete static_cast<HostBufferHandle*>(handle);
}

static void RegisterSharedHostBuffer(void*, int32_t deviceId, void* data, size_t size,
                                     void** outDeviceData, Rs::Status* status)
{
    if (!data || !outDeviceData) {
        SetRsStatus(status, Status::InvalidParam("invalid shared host buffer"));
        return;
    }
    *outDeviceData = nullptr;
    Trans::Device device;
    auto s = device.Setup(deviceId);
    if (s.Failure()) [[unlikely]] {
        UC_ERROR("Failed({}) to setup device({}).", s, deviceId);
        SetRsStatus(status, s);
        return;
    }
    s = Trans::Buffer::RegisterHostBuffer(data, size, outDeviceData);
    if (s.Failure()) [[unlikely]] {
        UC_ERROR("Failed({}) to register buffer({}) to device({}).", s, size, deviceId);
        SetRsStatus(status, s);
        return;
    }
    SetRsStatus(status, Status::OK());
}

static void UnregisterSharedHostBuffer(void*, void* data)
{
    if (data) { (void)Trans::Buffer::UnregisterHostBuffer(data); }
}

static size_t PageSize(void*)
{
    const auto pageSize = sysconf(_SC_PAGESIZE);
    return pageSize > 0 ? static_cast<size_t>(pageSize) : 4096;
}

static size_t SharedMutexSize(void*) { return sizeof(pthread_mutex_t); }
static size_t SharedMutexAlign(void*) { return alignof(pthread_mutex_t); }

static bool SharedMutexInit(void*, void* lock)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) { return false; }
    bool ok = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
              pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 &&
              pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP) == 0 &&
              pthread_mutex_init(static_cast<pthread_mutex_t*>(lock), &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    return ok;
}

static void SharedMutexLock(void*, void* lock)
{
    auto* mutex = static_cast<pthread_mutex_t*>(lock);
    const auto rc = pthread_mutex_lock(mutex);
    if (rc == EOWNERDEAD) { (void)pthread_mutex_consistent(mutex); }
}

static bool SharedMutexTryLock(void*, void* lock)
{
    auto* mutex = static_cast<pthread_mutex_t*>(lock);
    const auto rc = pthread_mutex_trylock(mutex);
    if (rc == EOWNERDEAD) {
        (void)pthread_mutex_consistent(mutex);
        return true;
    }
    return rc == 0;
}

static void SharedMutexUnlock(void*, void* lock)
{
    (void)pthread_mutex_unlock(static_cast<pthread_mutex_t*>(lock));
}

static size_t SharedSpinSize(void*) { return sizeof(pthread_spinlock_t); }
static size_t SharedSpinAlign(void*) { return alignof(pthread_spinlock_t); }

static bool SharedSpinInit(void*, void* lock)
{
    return pthread_spin_init(static_cast<pthread_spinlock_t*>(lock), PTHREAD_PROCESS_SHARED) == 0;
}

static void SharedSpinLock(void*, void* lock)
{
    (void)pthread_spin_lock(static_cast<pthread_spinlock_t*>(lock));
}

static bool SharedSpinTryLock(void*, void* lock)
{
    return pthread_spin_trylock(static_cast<pthread_spinlock_t*>(lock)) == 0;
}

static void SharedSpinUnlock(void*, void* lock)
{
    (void)pthread_spin_unlock(static_cast<pthread_spinlock_t*>(lock));
}

static Rs::TransBufferCallbacks MakeCallbacks()
{
    return {
        nullptr,
        &MakeLocalHostBuffer,
        &FreeLocalHostBuffer,
        &RegisterSharedHostBuffer,
        &UnregisterSharedHostBuffer,
        &PageSize,
        &SharedMutexSize,
        &SharedMutexAlign,
        &SharedMutexInit,
        &SharedMutexLock,
        &SharedMutexTryLock,
        &SharedMutexUnlock,
        &SharedSpinSize,
        &SharedSpinAlign,
        &SharedSpinInit,
        &SharedSpinLock,
        &SharedSpinTryLock,
        &SharedSpinUnlock,
    };
}

}  // namespace

TransBuffer::~TransBuffer() { ResetCore(); }

void TransBuffer::ResetCore() noexcept
{
    if (core_) {
        Rs::ucm_cache_store_trans_buffer_free(core_);
        core_ = nullptr;
    }
}

Status TransBuffer::Setup(const Config& config)
{
    ResetCore();
    Rs::TransBufferConfigView view{
        config.shareBufferEnable,
        config.cacheLoadBackendOnly,
        config.ioDirect,
        reinterpret_cast<const uint8_t*>(config.uniqueId.data()),
        config.uniqueId.size(),
        config.deviceId,
        config.shardSize,
        config.bufferCapacity,
        config.loadExclusiveBufferNumber,
    };
    auto callbacks = MakeCallbacks();
    Rs::Status rsStatus{};
    core_ = Rs::ucm_cache_store_trans_buffer_new(&view, &callbacks, &rsStatus);
    auto s = Rs::ToUcStatus(rsStatus);
    if (s.Failure()) [[unlikely]] { return s; }
    if (!core_) [[unlikely]] { return Status::Error("failed to create rust trans buffer core"); }
    return Status::OK();
}

TransBuffer::Handle TransBuffer::Get(const Detail::BlockId& blockId, size_t shardIdx,
                                     bool allowReserved, bool isLoad)
{
    if (!core_) [[unlikely]] { return {}; }
    Rs::TransBufferGetResult result{};
    Rs::Status rsStatus{};
    Rs::ucm_cache_store_trans_buffer_get(core_, ToRsBlockId(blockId), shardIdx, allowReserved,
                                         isLoad, &result, &rsStatus);
    auto s = Rs::ToUcStatus(rsStatus);
    if (s.Failure()) [[unlikely]] {
        UC_ERROR("Failed({}) to get trans buffer shard({}).", s, shardIdx);
        return {};
    }
    return Handle(this, result.index, result.owner);
}

bool TransBuffer::Exist(const Detail::BlockId& blockId, size_t shardIdx)
{
    if (!core_) [[unlikely]] { return false; }
    bool exist = false;
    Rs::Status rsStatus{};
    Rs::ucm_cache_store_trans_buffer_exist(core_, ToRsBlockId(blockId), shardIdx, &exist,
                                           &rsStatus);
    auto s = Rs::ToUcStatus(rsStatus);
    if (s.Failure()) [[unlikely]] {
        UC_ERROR("Failed({}) to check trans buffer shard({}).", s, shardIdx);
        return false;
    }
    return exist;
}

void* TransBuffer::DataAt(Index pos)
{
    return core_ ? Rs::ucm_cache_store_trans_buffer_data_at(core_, pos) : nullptr;
}

void TransBuffer::Acquire(Index pos)
{
    if (core_) { Rs::ucm_cache_store_trans_buffer_acquire(core_, pos); }
}

void TransBuffer::Release(Index pos)
{
    if (core_) { Rs::ucm_cache_store_trans_buffer_release(core_, pos); }
}

bool TransBuffer::Ready(Index pos)
{
    return core_ ? Rs::ucm_cache_store_trans_buffer_ready(core_, pos) : false;
}

void TransBuffer::MarkReady(Index pos)
{
    if (core_) { Rs::ucm_cache_store_trans_buffer_mark_ready(core_, pos); }
}

void TransBuffer::MarkNotReady(Index pos)
{
    if (core_) { Rs::ucm_cache_store_trans_buffer_mark_not_ready(core_, pos); }
}

}  // namespace UC::CacheStore
