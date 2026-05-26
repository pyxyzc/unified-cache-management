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
#include <atomic>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include "logger/logger.h"
#include "posix_shm.h"
#include "trans/buffer.h"
#include "trans/device.h"

namespace UC::CacheStore {

static constexpr size_t nHashTableBucket = 16411;
static constexpr auto invalidIndex = std::numeric_limits<size_t>::max();

static inline size_t Hash(const Detail::BlockId& blockId, size_t shard)
{
    static UC::Detail::BlockIdHasher blockIdHasher;
    static std::hash<size_t> shardHasher;
    constexpr auto goldenSection = 0x9e3779b97f4a7c15ULL;
    size_t h1 = blockIdHasher(blockId);
    size_t h2 = shardHasher(shard);
    return (h1 ^ (h2 + goldenSection + (h1 << 6) + (h1 >> 2))) % nHashTableBucket;
}

using BufferMetaNode = Rs::TransBufferMetaNode;

static_assert(sizeof(Detail::BlockId) == sizeof(Rs::BlockId));
static_assert(alignof(Detail::BlockId) == alignof(Rs::BlockId));
static_assert(offsetof(BufferMetaNode, block) == 0);

class BufferStrategy {
protected:
    struct BaseConfig {
        int32_t deviceId{-1};
        size_t nodeSize{0};
        size_t totalSize{0};
        size_t reservedNumber{0};
    };
    BaseConfig base_;

public:
    BufferStrategy(int32_t deviceId, size_t nodeSize, size_t totalSize, size_t reservedNumber)
        : base_({deviceId, nodeSize, totalSize, reservedNumber})
    {
    }
    virtual ~BufferStrategy() = default;
    virtual Status Setup() = 0;
    virtual void BucketLock(size_t iBucket) = 0;
    virtual bool BucketTryLock(size_t iBucket) = 0;
    virtual void BucketUnlock(size_t iBucket) = 0;
    virtual void NodeLock(size_t iNode) = 0;
    virtual void NodeUnlock(size_t iNode) = 0;
    virtual size_t& FirstAt(size_t iBucket) = 0;
    virtual size_t FetchNode(bool allowReserved) = 0;
    virtual void* DataAt(size_t iNode) = 0;
    virtual BufferMetaNode* MetaAt(size_t iNode) = 0;
};

class LocalBufferStrategy : public BufferStrategy {
    struct BufferHeader {
        size_t buckets[nHashTableBucket];
        size_t freeHead;
        size_t nodeSize;
        size_t nNode;
    };
    struct LocalMutex {
        pthread_mutex_t mutex;
        ~LocalMutex() { pthread_mutex_destroy(&mutex); }
        void Init()
        {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
            pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
            pthread_mutex_init(&mutex, &attr);
            pthread_mutexattr_destroy(&attr);
        }
        void Lock() { pthread_mutex_lock(&mutex); }
        bool TryLock() { return pthread_mutex_trylock(&mutex) == 0; }
        void Unlock() { pthread_mutex_unlock(&mutex); }
    };
    struct LocalLock {
        pthread_spinlock_t lock;
        ~LocalLock() { pthread_spin_destroy(&lock); }
        void Init() { pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE); }
        void Lock() { pthread_spin_lock(&lock); }
        bool TryLock() { return pthread_spin_trylock(&lock) == 0; }
        void Unlock() { pthread_spin_unlock(&lock); }
    };

    bool ioDirect_{false};
    BufferHeader header_;
    LocalMutex bucketLocks_[nHashTableBucket];
    std::unique_ptr<LocalLock[]> nodeLocks_;
    std::unique_ptr<BufferMetaNode[]> meta_;
    std::shared_ptr<void> data_;

public:
    LocalBufferStrategy(int32_t deviceId, size_t nodeSize, size_t totalSize, size_t reservedNumber,
                        bool ioDirect)
        : BufferStrategy(deviceId, nodeSize, totalSize, reservedNumber), ioDirect_(ioDirect)
    {
    }
    Status Setup() override
    {
        const auto deviceId = base_.deviceId;
        const auto totalSize = base_.totalSize;
        const auto nodeSize = base_.nodeSize;
        auto nNode = totalSize / nodeSize;
        try {
            nodeLocks_ = std::make_unique<LocalLock[]>(nNode);
            meta_ = std::make_unique<BufferMetaNode[]>(nNode);
            for (size_t i = 0; i < nHashTableBucket; i++) { bucketLocks_[i].Init(); }
            for (size_t i = 0; i < nNode; i++) { nodeLocks_[i].Init(); }
        } catch (const std::exception& e) {
            UC_ERROR("Failed({}) to alloc buffer.", e.what());
            return Status::Error(e.what());
        }
        Trans::Device device;
        auto s = device.Setup(deviceId);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Failed({}) to setup device({}).", s, deviceId);
            return s;
        }
        auto buffer = device.MakeBuffer();
        if (!buffer) [[unlikely]] {
            UC_ERROR("Failed to make buffer on device({}).", deviceId);
            return Status::Error();
        }
        data_ = ioDirect_ ? buffer->MakeHostBuffer4DirectIo(nodeSize * nNode)
                          : buffer->MakeHostBuffer(nodeSize * nNode);
        if (!data_) [[unlikely]] {
            UC_ERROR("Failed to make pinned({}) for device({}).", nodeSize * nNode, deviceId);
            return Status::OutOfMemory();
        }
        for (size_t i = 0; i < nHashTableBucket; i++) { header_.buckets[i] = invalidIndex; }
        for (size_t i = 0; i < nNode; i++) { meta_[i].Init(); }
        header_.freeHead = 0;
        header_.nodeSize = nodeSize;
        header_.nNode = nNode;
        return Status::OK();
    }
    void BucketLock(size_t iBucket) override { bucketLocks_[iBucket].Lock(); }
    bool BucketTryLock(size_t iBucket) override { return bucketLocks_[iBucket].TryLock(); }
    void BucketUnlock(size_t iBucket) override { bucketLocks_[iBucket].Unlock(); }
    void NodeLock(size_t iNode) override { nodeLocks_[iNode].Lock(); }
    void NodeUnlock(size_t iNode) override { nodeLocks_[iNode].Unlock(); }
    size_t& FirstAt(size_t iBucket) override { return header_.buckets[iBucket]; }
    size_t FetchNode(bool allowReserved) override
    {
        const auto limit = header_.nNode - (allowReserved ? 0 : base_.reservedNumber);
        if (header_.freeHead >= limit) { header_.freeHead = 0; }
        return header_.freeHead++;
    }
    void* DataAt(size_t iNode) override
    {
        return ((std::byte*)data_.get()) + header_.nodeSize * iNode;
    }
    BufferMetaNode* MetaAt(size_t iNode) override { return meta_.get() + iNode; }
};

class SharedBufferStrategy : public BufferStrategy {
protected:
    struct ShareMutex {
        pthread_mutex_t mutex;
        ~ShareMutex() = delete;
        void Init()
        {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
            pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
            pthread_mutex_init(&mutex, &attr);
            pthread_mutexattr_destroy(&attr);
        }
        void Lock() { pthread_mutex_lock(&mutex); }
        bool TryLock() { return pthread_mutex_trylock(&mutex) == 0; }
        void Unlock() { pthread_mutex_unlock(&mutex); }
    };
    struct ShareLock {
        pthread_spinlock_t lock;
        ~ShareLock() = delete;
        void Init() { pthread_spin_init(&lock, PTHREAD_PROCESS_SHARED); }
        void Lock() { pthread_spin_lock(&lock); }
        bool TryLock() { return pthread_spin_trylock(&lock) == 0; }
        void Unlock() { pthread_spin_unlock(&lock); }
    };
    static constexpr size_t sharedBufferMagic = (('S' << 16) | ('b' << 8) | 1);
    struct BufferHeader {
        std::atomic<size_t> magic;
        ShareLock lock;
        size_t nNode;
        size_t freeHead;
        size_t buckets[nHashTableBucket];
        ShareMutex bucketLocks[nHashTableBucket];
        ShareLock nodeLocks[0];
    };

    BufferHeader* header_{nullptr};
    BufferMetaNode* meta_{nullptr};
    std::byte* data_{nullptr};
    std::byte* dataOnDevice_{nullptr};
    const std::string& uuid_;
    std::string shmName_;
    size_t nodeSize_{0};
    size_t nNode_{0};
    void* addrress_{nullptr};
    size_t totalSize_{0};

    size_t MetaOffset() const noexcept { return sizeof(BufferHeader) + sizeof(ShareLock) * nNode_; }
    size_t DataOffset() const noexcept
    {
        static const auto pageSize = sysconf(_SC_PAGESIZE);
        const auto size = MetaOffset() + sizeof(BufferMetaNode) * nNode_;
        return (size + pageSize - 1) & ~(pageSize - 1);
    }
    size_t DataSize() const noexcept { return nodeSize_ * nNode_; }
    static const std::string& ShmPrefix() noexcept
    {
        static std::string prefix{"uc_shm_cache_"};
        return prefix;
    }
    static void CleanUpShmFileExceptMe(const std::string& me)
    {
        namespace fs = std::filesystem;
        std::string_view prefix = ShmPrefix();
        fs::path shmDir = "/dev/shm";
        if (!fs::exists(shmDir)) { return; }
        const auto now = fs::file_time_type::clock::now();
        const auto keepThreshold = std::chrono::minutes(10);
        for (const auto& entry : fs::directory_iterator(shmDir)) {
            const auto& path = entry.path();
            const auto& name = path.filename().string();
            if (!entry.is_regular_file() || name.compare(0, prefix.size(), prefix) != 0 ||
                name == me) {
                continue;
            }
            try {
                const auto lwt = fs::last_write_time(path);
                if (now - lwt <= keepThreshold) { continue; }
                fs::remove(path);
            } catch (...) {
            }
        }
    }
    static Status MmapShmFile(PosixShm& shmFile, const size_t size, void*& addr,
                              bool needTrunc = true)
    {
        auto s = Status::OK();
        if (needTrunc) {
            s = shmFile.Truncate(size);
            if (s.Failure()) [[unlikely]] {
                UC_ERROR("Failed({}) to trunc file({}) with size({}).", s, shmFile.ShmName(), size);
                return s;
            }
        }
        s = shmFile.MMap(addr, size, true, true, true);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Failed({}) to mmap file({}) with size({}).", s, shmFile.ShmName(), size);
            return s;
        }
        return Status::OK();
    }
    static Status WaitShmHeaderReady(BufferHeader* header)
    {
        constexpr auto retryInterval = std::chrono::milliseconds(100);
        constexpr auto maxTryTime = 100;
        auto tryTime = 0;
        do {
            if (header->magic == sharedBufferMagic) { break; }
            if (tryTime > maxTryTime) { return Status::Retry(); }
            std::this_thread::sleep_for(retryInterval);
            tryTime++;
        } while (true);
        return Status::OK();
    }
    Status InitShmBuffer(PosixShm& shmFile)
    {
        auto s = MmapShmFile(shmFile, totalSize_, addrress_);
        if (s.Failure()) [[unlikely]] { return s; }
        header_ = static_cast<BufferHeader*>(addrress_);
        meta_ = (BufferMetaNode*)(static_cast<std::byte*>(addrress_) + MetaOffset());
        header_->lock.Init();
        header_->nNode = nNode_;
        header_->freeHead = 0;
        for (size_t i = 0; i < nHashTableBucket; i++) {
            header_->buckets[i] = invalidIndex;
            header_->bucketLocks[i].Init();
        }
        for (size_t i = 0; i < nNode_; i++) {
            header_->nodeLocks[i].Init();
            meta_[i].Init();
        }
        header_->magic = sharedBufferMagic;
        return Status::OK();
    }
    Status LoadShmBuffer(PosixShm& shmFile)
    {
        auto s = shmFile.ShmOpen(PosixShm::OpenFlag::READ_WRITE);
        if (s.Failure()) {
            UC_ERROR("Failed({}) to open file({}).", s, shmFile.ShmName());
            return s;
        }
        s = MmapShmFile(shmFile, totalSize_, addrress_, false);
        if (s.Failure()) [[unlikely]] { return s; }
        header_ = static_cast<BufferHeader*>(addrress_);
        s = WaitShmHeaderReady(header_);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Shm file({}) not ready.", shmFile.ShmName());
            return s;
        }
        meta_ = (BufferMetaNode*)(static_cast<std::byte*>(addrress_) + MetaOffset());
        return Status::OK();
    }
    Status RegisterBuffer(int32_t deviceId)
    {
        data_ = static_cast<std::byte*>(addrress_) + DataOffset();
        Trans::Device device;
        auto s = device.Setup(deviceId);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Failed({}) to setup device({}).", s, deviceId);
            return s;
        }
        const auto dataSize = DataSize();
        s = Trans::Buffer::RegisterHostBuffer((void*)data_, dataSize, (void**)&dataOnDevice_);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Failed({}) to register buffer({}) to device({}).", s, dataSize, deviceId);
            return s;
        }
        return Status::OK();
    }

public:
    SharedBufferStrategy(const std::string& uuid, int32_t deviceId, size_t nodeSize,
                         size_t totalSize, size_t reservedNumber)
        : BufferStrategy(deviceId, nodeSize, totalSize, reservedNumber), uuid_(uuid)
    {
    }
    ~SharedBufferStrategy() override
    {
        if (data_) { Trans::Buffer::UnregisterHostBuffer(data_); }
        if (addrress_) { PosixShm::MUnmap(addrress_, totalSize_); }
        PosixShm{shmName_}.ShmUnlink();
    }
    Status Setup() override
    {
        const auto& uuid = uuid_;
        const auto deviceId = base_.deviceId;
        const auto nodeSize = base_.nodeSize;
        const auto totalSize = base_.totalSize;
        shmName_ = ShmPrefix() + uuid;
        nodeSize_ = nodeSize;
        nNode_ = totalSize / nodeSize;
        CleanUpShmFileExceptMe(shmName_);
        PosixShm shmFile{shmName_};
        const auto dataOffset = DataOffset();
        totalSize_ = dataOffset + DataSize();
        const auto flags =
            PosixShm::OpenFlag::CREATE | PosixShm::OpenFlag::EXCL | PosixShm::OpenFlag::READ_WRITE;
        auto s = shmFile.ShmOpen(flags);
        if (s.Success()) {
            s = InitShmBuffer(shmFile);
        } else if (s == Status::DuplicateKey()) {
            s = LoadShmBuffer(shmFile);
        } else {
            UC_ERROR("Failed({}) to open file({}) with flags({}).", s, shmName_, flags);
            return s;
        }
        return RegisterBuffer(deviceId);
    }
    void BucketLock(size_t iBucket) override { header_->bucketLocks[iBucket].Lock(); }
    bool BucketTryLock(size_t iBucket) override { return header_->bucketLocks[iBucket].TryLock(); }
    void BucketUnlock(size_t iBucket) override { header_->bucketLocks[iBucket].Unlock(); }
    void NodeLock(size_t iNode) override { header_->nodeLocks[iNode].Lock(); }
    void NodeUnlock(size_t iNode) override { header_->nodeLocks[iNode].Unlock(); }
    size_t& FirstAt(size_t iBucket) override { return header_->buckets[iBucket]; }
    size_t FetchNode(bool allowReserved) override
    {
        const auto limit = header_->nNode - (allowReserved ? 0 : base_.reservedNumber);
        header_->lock.Lock();
        if (header_->freeHead >= limit) { header_->freeHead = 0; }
        const auto iNode = header_->freeHead++;
        header_->lock.Unlock();
        return iNode;
    }
    void* DataAt(size_t iNode) override { return data_ + nodeSize_ * iNode; }
    BufferMetaNode* MetaAt(size_t iNode) override { return meta_ + iNode; }
};

class SharedBufferWatcherStrategy : public SharedBufferStrategy {
public:
    SharedBufferWatcherStrategy(const std::string& uuid) : SharedBufferStrategy(uuid, -1, 0, 0, 0)
    {
    }
    Status Setup() override
    {
        shmName_ = ShmPrefix() + uuid_;
        CleanUpShmFileExceptMe(shmName_);
        PosixShm shmFile{shmName_};
        auto s = shmFile.ShmOpen(PosixShm::OpenFlag::READ_WRITE);
        if (s.Failure()) {
            UC_ERROR("Failed({}) to open file({}).", s, shmFile.ShmName());
            return s;
        }
        void* addr = nullptr;
        auto size = sizeof(BufferHeader);
        s = MmapShmFile(shmFile, size, addr, false);
        if (s.Failure()) [[unlikely]] { return s; }
        auto header = static_cast<BufferHeader*>(addr);
        s = WaitShmHeaderReady(header);
        if (s.Failure()) [[unlikely]] {
            UC_ERROR("Shm file({}) not ready.", shmFile.ShmName());
            return s;
        }
        nNode_ = header->nNode;
        shmFile.MUnmap(addr, size);
        totalSize_ = DataOffset();
        s = MmapShmFile(shmFile, totalSize_, addrress_, false);
        if (s.Failure()) [[unlikely]] { return s; }
        header_ = static_cast<BufferHeader*>(addrress_);
        meta_ = (BufferMetaNode*)(static_cast<std::byte*>(addrress_) + MetaOffset());
        return Status::OK();
    }
    void* DataAt(size_t iNode) override { return nullptr; }
};

static const Detail::BlockId& ToDetailBlockId(const Rs::BlockId* block)
{
    return *reinterpret_cast<const Detail::BlockId*>(block);
}

static const Rs::BlockId* ToRsBlockId(const Detail::BlockId& block)
{
    return reinterpret_cast<const Rs::BlockId*>(&block);
}

static size_t StrategyBucketOf(void*, const Rs::BlockId* block, size_t shard)
{
    return Hash(ToDetailBlockId(block), shard);
}

static void StrategyBucketLock(void* ctx, size_t iBucket)
{
    static_cast<BufferStrategy*>(ctx)->BucketLock(iBucket);
}

static bool StrategyBucketTryLock(void* ctx, size_t iBucket)
{
    return static_cast<BufferStrategy*>(ctx)->BucketTryLock(iBucket);
}

static void StrategyBucketUnlock(void* ctx, size_t iBucket)
{
    static_cast<BufferStrategy*>(ctx)->BucketUnlock(iBucket);
}

static void StrategyNodeLock(void* ctx, size_t iNode)
{
    static_cast<BufferStrategy*>(ctx)->NodeLock(iNode);
}

static void StrategyNodeUnlock(void* ctx, size_t iNode)
{
    static_cast<BufferStrategy*>(ctx)->NodeUnlock(iNode);
}

static size_t* StrategyFirstAt(void* ctx, size_t iBucket)
{
    return &static_cast<BufferStrategy*>(ctx)->FirstAt(iBucket);
}

static size_t StrategyFetchNode(void* ctx, bool allowReserved)
{
    return static_cast<BufferStrategy*>(ctx)->FetchNode(allowReserved);
}

static Rs::TransBufferMetaNode* StrategyMetaAt(void* ctx, size_t iNode)
{
    return static_cast<BufferStrategy*>(ctx)->MetaAt(iNode);
}

static Rs::TransBufferStrategyView MakeStrategyView(BufferStrategy* strategy)
{
    return {
        strategy,
        &StrategyBucketOf,
        &StrategyBucketLock,
        &StrategyBucketTryLock,
        &StrategyBucketUnlock,
        &StrategyNodeLock,
        &StrategyNodeUnlock,
        &StrategyFirstAt,
        &StrategyFetchNode,
        &StrategyMetaAt,
    };
}

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
    strategy_.reset();
    bypassHitOnLoad_ = config.cacheLoadBackendOnly;
    try {
        if (!config.shareBufferEnable) {
            strategy_ = std::make_shared<LocalBufferStrategy>(
                config.deviceId, config.shardSize, config.bufferCapacity,
                config.loadExclusiveBufferNumber, config.ioDirect);
        } else if (config.deviceId >= 0) {
            strategy_ = std::make_shared<SharedBufferStrategy>(
                config.uniqueId, config.deviceId, config.shardSize, config.bufferCapacity,
                config.loadExclusiveBufferNumber);
        } else {
            strategy_ = std::make_shared<SharedBufferWatcherStrategy>(config.uniqueId);
        }
    } catch (const std::exception& e) {
        return Status::Error(fmt::format("failed({}) to make buffer strategy", e.what()));
    }
    auto s = strategy_->Setup();
    if (s.Failure()) [[unlikely]] {
        strategy_.reset();
        return s;
    }
    auto view = MakeStrategyView(strategy_.get());
    Rs::Status rsStatus{};
    core_ = Rs::ucm_cache_store_trans_buffer_new(&view, bypassHitOnLoad_, &rsStatus);
    s = Rs::ToUcStatus(rsStatus);
    if (s.Failure()) [[unlikely]] {
        strategy_.reset();
        return s;
    }
    if (!core_) [[unlikely]] {
        strategy_.reset();
        return Status::Error("failed to create rust trans buffer core");
    }
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

void* TransBuffer::DataAt(Index pos) { return strategy_->DataAt(pos); }

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
