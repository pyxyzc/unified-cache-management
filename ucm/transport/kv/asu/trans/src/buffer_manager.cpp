/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
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
#include "buffer_manager.h"
#include <utility>
#include <vector>

namespace UC::ASU {
namespace {

Status ConvertPoolStatus(const UC::Status& status)
{
    if (status.Success()) { return Status::OK(); }

    StatusCode code = StatusCode::INTERNAL_ERROR;
    if (status == UC::Status::InvalidParam()) {
        code = StatusCode::INVALID_ARGUMENT;
    } else if (status == UC::Status::Retry()) {
        code = StatusCode::RESOURCE_BUSY;
    }
    return Status::Error(code, status.ToString());
}

Status ConvertMemoryType(MemoryType type, UC::BufferPool::MemoryType& poolType)
{
    switch (type) {
        case MemoryType::HOST:
            poolType = UC::BufferPool::MemoryType::HOST;
            return Status::OK();
        case MemoryType::HOST_PINNED:
            poolType = UC::BufferPool::MemoryType::HOST_PINNED;
            return Status::OK();
        case MemoryType::ASCEND_DEVICE:
            poolType = UC::BufferPool::MemoryType::ASCEND_DEVICE;
            return Status::OK();
        default:
            return Status::Error(StatusCode::INVALID_ARGUMENT, "unsupported memory type");
    }
}

}  // namespace

bool IsTransportBufferReady(const ScatterGatherEntry& sge)
{
    return sge.local_addr != 0 && sge.device_addr != 0 && sge.length != 0 &&
           sge.slot_index != UINT32_MAX;
}

BufferManager::~BufferManager()
{
    if (provider_ && memHandle_) {
        std::vector<TransProvider::UnregisterMemoryDesc> descs{
            {nullptr, memHandle_}
        };
        provider_->UnregisterMemory(descs);
    }
    memHandle_ = nullptr;
    provider_ = nullptr;
    tokenId_ = 0;
    bufferPool_.Reset();
    memoryType_ = MemoryType::HOST;
}

Status BufferManager::Init(std::string name, MemoryType type, std::size_t slot_capacity,
                           std::size_t slot_num, TransProvider* provider)
{
    UC::BufferPool::MemoryType poolType;
    auto typeStatus = ConvertMemoryType(type, poolType);
    if (!typeStatus.ok()) { return typeStatus; }

    auto poolStatus = bufferPool_.Init(std::move(name), poolType, slot_capacity, slot_num);
    if (poolStatus.Failure()) { return ConvertPoolStatus(poolStatus); }
    memoryType_ = type;

    if (provider) {
        provider_ = provider;
        auto regStatus = RegisterMemory();
        if (!regStatus.ok()) {
            provider_ = nullptr;
            memHandle_ = nullptr;
            tokenId_ = 0;
            bufferPool_.Reset();
            memoryType_ = MemoryType::HOST;
            return regStatus;
        }
    }

    return Status::OK();
}

Status BufferManager::RegisterMemory()
{
    const auto providerMemType = memoryType_ == MemoryType::HOST
                                     ? TransProvider::MemType::MEM_HOST
                                     : TransProvider::MemType::MEM_DEVICE;
    std::vector<TransProvider::RegisterMemoryDesc> descs{
        {providerMemType, reinterpret_cast<std::uintptr_t>(bufferPool_.GetDeviceAddr()),
         bufferPool_.GetTotalSize(),
         reinterpret_cast<std::uintptr_t>(bufferPool_.GetLocalAddr())}
    };
    std::vector<TransProvider::MemHandle> memHandles;
    auto regStatus = provider_->RegisterMemory(nullptr, descs, memHandles);
    if (!regStatus.ok() || memHandles.empty()) {
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             bufferPool_.GetName() +
                                 ": failed to register memory: " + regStatus.message);
    }

    auto tokenStatus = provider_->GetMemTokenId(memHandles[0], tokenId_);
    if (!tokenStatus.ok()) {
        std::vector<TransProvider::UnregisterMemoryDesc> unregDescs{
            {nullptr, memHandles[0]}
        };
        provider_->UnregisterMemory(unregDescs);
        tokenId_ = 0;
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             bufferPool_.GetName() +
                                 ": failed to get token id: " + tokenStatus.message);
    }

    memHandle_ = memHandles[0];
    return Status::OK();
}

Status BufferManager::Allocate(std::size_t size, ScatterGatherEntry& sge)
{
    if (!bufferPool_.IsInitialized()) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "buffer pool not initialized");
    }

    UC::BufferPool::Slot slot;
    auto poolStatus = bufferPool_.Allocate(size, slot);
    if (poolStatus.Failure()) { return ConvertPoolStatus(poolStatus); }

    sge.local_addr = reinterpret_cast<std::uint64_t>(slot.local_addr);
    sge.device_addr = reinterpret_cast<std::uint64_t>(slot.device_addr);
    sge.length = static_cast<std::uint32_t>(slot.length);
    sge.tokenId = tokenId_;
    sge.slot_index = slot.slot_index;
    sge.memory_type = memoryType_;
    return Status::OK();
}

Status BufferManager::Free(std::uint32_t slot_index)
{
    if (!bufferPool_.IsInitialized()) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "buffer pool not initialized");
    }
    return ConvertPoolStatus(bufferPool_.Free(slot_index));
}

bool BufferManager::IsValidPointer(const void* ptr) const
{
    return bufferPool_.IsValidPointer(ptr);
}

}  // namespace UC::ASU
