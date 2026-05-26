# TransBuffer Rust 激进重构开发记录

## 1. 开发脉络

此前的重构先从 CacheStore 引入 Rust core 开始，把配置校验和 task id 生成这类边界清晰的逻辑迁到 Rust。随后，Ascend Trans 模块做了一次较小范围的试点，将 ACL 调用薄层通过 Rust FFI 包装起来，同时保留现有 C++ `Trans::Device` / `Trans::Stream` / `Trans::Buffer` 对外接口。

在 TransBuffer 的上一轮重构中，Rust 已经接管了 hash 之后的 metadata 状态机，包括 `get/exist`、引用计数、ready/owner 语义、节点分配和链表移动。但当时 C++ 仍然保留 local/shared/watcher strategy、buffer 分配、共享内存布局和锁。

本轮工作是在这个基础上继续收缩 C++ 职责：在保持 C++ public API 和上游调用关系不变的前提下，尽可能把 `TransBuffer` 的策略选择、metadata、共享内存布局和生命周期迁到 Rust。

## 2. 本次相关 Prompt

用户在上一轮 TransBuffer 元数据核心迁移完成后继续提出：

```text
我们进一步修改TransBuffer模块，希望尽可能多得使用Rust重构，你有什么主意吗？
```

随后确认采用“激进路线”，并选择共享内存兼容策略为“版本升级”。

本次需求的核心含义是：

- 不只迁移 metadata 状态机。
- 尽量删除 C++ `BufferStrategy` 层。
- Rust 接管 local/shared/watcher 三类 TransBuffer 策略。
- Rust 定义新的共享内存布局和稳定 hash。
- C++ 仍保留 `TransBuffer` public API，避免影响 `LoadQueue`、`DumpQueue`、
  `TransManager`、`BufferManager`。
- 不迁移底层 `UC::Trans::Device` / `UC::Trans::Buffer`，避免扩大到设备后端。

## 3. 方案决策

### 3.1 选择“激进路线”

最终选择：

```text
Rust 接管 TransBuffer local/shared/watcher 策略；
C++ 只保留 public API wrapper 和设备/OS 能力回调。
```

与上一轮只迁移元数据状态机的方案相比，本次进一步迁移：

- bucket 数组。
- metadata 数组。
- local buffer 的 free head。
- local bucket/node lock。
- shared buffer header 布局。
- shared buffer mmap/unmap/shm_open/shm_unlink 生命周期。
- watcher 映射逻辑。
- `DataAt` 地址计算。
- hash 计算。

仍然保留在 C++ 的内容：

- `UC::CacheStore::TransBuffer` public class。
- `TransBuffer::Handle` RAII 语义。
- `UC::Trans::Device` 初始化。
- host buffer 分配和 DirectIO buffer 分配。
- shared host buffer 注册/反注册。
- process-shared pthread mutex / spinlock 的初始化和操作。

这样做的原因：

- C++ public API 已被多个 CacheStore 子模块直接使用，保留它可以避免联动重构。
- `Trans::Device` 和 `Trans::Buffer` 是现有多后端抽象，迁移它们会扩大到 Ascend/CUDA/Simu 等后端。
- pthread robust mutex 和 process-shared spinlock 已由 C++ 稳定使用，本次只把锁内存布局交给 Rust，具体系统调用仍通过 C++ 回调执行。

### 3.2 共享内存采用版本升级

上一轮为了兼容旧共享内存，保留了 C++ hash 和旧 `uc_shm_cache_` 布局。

本次用户选择“版本升级”，因此新实现不再尝试读取旧布局，而是使用新的共享内存命名和 layout：

```text
uc_shm_cache_rs2_
```

Rust header 使用新的 magic：

```text
SbR2
```

这表示：

- 新旧进程不应混用同一个共享内存文件。
- Rust 可以定义稳定、明确的 header offset。
- 后续迭代可以继续在 Rust 中演进布局，而不被旧 C++ flexible array 布局限制。

### 3.3 Hash 改为 Rust 稳定 hash

本次不再依赖 C++ `std::hash`。

Rust 使用稳定 FNV-1a 风格 hash：

```text
BlockId bytes + shard little-endian bytes -> hash -> 16411 buckets
```

这样做的收益：

- hash 行为不再依赖 C++ 标准库实现。
- shared buffer 多进程之间只要使用同一 Rust 版本，就能稳定定位 bucket。
- `TransBuffer` 的 metadata 行为整体由 Rust 自洽控制。

## 4. 模块现状

### 4.1 C++ 仍暴露原 public API

文件：

```text
ucm/store/cache/cc/trans_buffer.h
ucm/store/cache/cc/trans_buffer.cc
```

外部调用方仍然使用：

- `TransBuffer::Setup`
- `TransBuffer::Get`
- `TransBuffer::Exist`
- `TransBuffer::Handle::Data`
- `Handle::Ready`
- `Handle::MarkReady`

`Handle` 的复制、移动和析构仍然通过 `Acquire/Release` 维护引用计数。

### 4.2 Rust 现在拥有 TransBuffer core

文件：

```text
ucm/store/cache/rs/src/trans_buffer.rs
```

Rust 侧核心类型包括：

- `CacheStoreTransBufferCore`
- `Strategy`
- `LocalStrategy`
- `SharedStrategy`
- `SharedLayout`
- `SharedHeader`
- `CacheStoreTransBufferMetaNode`
- `CacheStoreTransBufferConfigView`
- `CacheStoreTransBufferCallbacks`

Rust 侧负责：

- 根据配置选择 local/shared/watcher。
- 初始化 bucket、metadata、free head。
- 执行 `get/exist/acquire/release/ready/mark_ready/mark_not_ready`。
- 执行节点分配、移动、摘链。
- 计算稳定 hash。
- 计算 `DataAt` 地址。
- 创建、加载、清理新版 shared memory。
- watcher 只映射 header + metadata，不返回数据指针。

### 4.3 C++ 变为薄回调层

C++ 侧现在提供给 Rust 的能力包括：

- `makeLocalHostBuffer`
- `freeLocalHostBuffer`
- `registerSharedHostBuffer`
- `unregisterSharedHostBuffer`
- `pageSize`
- `sharedMutexSize`
- `sharedMutexAlign`
- `sharedMutexInit`
- `sharedMutexLock`
- `sharedMutexTryLock`
- `sharedMutexUnlock`
- `sharedSpinSize`
- `sharedSpinAlign`
- `sharedSpinInit`
- `sharedSpinLock`
- `sharedSpinTryLock`
- `sharedSpinUnlock`

也就是说，C++ 不再维护 `BufferStrategy` 虚类层，只向 Rust 提供设备资源和 OS 锁能力。

## 5. 实施路径

### 5.1 先重写 Rust 模块

本次首先替换：

```text
ucm/store/cache/rs/src/trans_buffer.rs
```

主要步骤：

1. 删除 `CacheStoreTransBufferStrategyView` 回调式 strategy。
2. 新增 config view 和 callbacks view。
3. 新增 `LocalStrategy`，使用 Rust `AtomicBool` spin lock 和 `AtomicUsize` free head。
4. 新增 `SharedStrategy`，由 Rust 负责 POSIX shm、mmap、layout offset、header magic。
5. 新增 `SharedLayout`，集中计算 header、lock、bucket、meta、data 的 offset。
6. 新增 `ucm_cache_store_trans_buffer_data_at`，让 C++ wrapper 不再持有 strategy。
7. 保留原 `get/exist/alloc/move_to/remove` 的行为语义。

### 5.2 再更新 FFI 头

修改：

```text
ucm/store/cache/cc/cache_store_rs.h
```

主要变化：

- 删除 `TransBufferStrategyView`。
- 新增 `TransBufferConfigView`。
- 新增 `TransBufferCallbacks`。
- `ucm_cache_store_trans_buffer_new` 改为接收 config + callbacks。
- 新增 `ucm_cache_store_trans_buffer_data_at`。

### 5.3 最后收缩 C++ TransBuffer

修改：

```text
ucm/store/cache/cc/trans_buffer.h
ucm/store/cache/cc/trans_buffer.cc
```

主要变化：

- 删除 C++ `BufferStrategy` 前向声明和成员。
- 删除 `LocalBufferStrategy`。
- 删除 `SharedBufferStrategy`。
- 删除 `SharedBufferWatcherStrategy`。
- `Setup()` 只构造 Rust config view 和 callbacks，然后创建 Rust core。
- `DataAt()` 改为调用 Rust FFI。
- C++ 文件保留 host buffer 分配、shared buffer 注册、pthread 锁回调。

## 6. 新共享内存布局

新版 Rust shared header 记录所有关键 offset：

- magic
- n_node
- free_head
- node_size
- bucket_count
- bucket_lock_size / align
- node_lock_size / align
- fetch_lock_offset
- bucket_locks_offset
- node_locks_offset
- buckets_offset
- meta_offset
- data_offset
- total_size

布局顺序：

```text
SharedHeader
fetch spin lock
bucket mutex locks
node spin locks
buckets[16411]
meta[n_node]
page aligned data[n_node * node_size]
```

其中：

- lock 的 size/align 来自 C++ pthread 回调。
- offset 由 Rust 统一计算。
- data offset 由 Rust 按 page size 对齐。
- watcher 只 mmap 到 `data_offset`，因此不会映射 data 区。

## 7. 当前验证结果

已通过 Rust 格式检查：

```bash
cargo fmt --manifest-path ucm/store/cache/rs/Cargo.toml --check
```

已通过 Rust 单元测试：

```bash
cargo test --manifest-path ucm/store/cache/rs/Cargo.toml
```

结果：

```text
14 passed; 0 failed
```

已通过 cachestore 构建：

```bash
cmake --build build --target cachestore -- -j8
```

结果：

```text
Built target cachestore
```

尝试运行 C++ TransBuffer 参数化测试：

```bash
build/ucm/store/test/ucmstore.test \
  --gtest_filter=SharedCondition/UCCacheTransBufferTest.*
```

当前环境下测试未通过，失败原因发生在 Ascend device 0 初始化阶段：

```text
aclrtSetDeviceImpl: open device 0 failed
runtime result = 507899
```

该失败发生在 C++ 设备回调 `MakeLocalHostBuffer` / `RegisterSharedHostBuffer` 中，即
`Trans::Device::Setup(0)` 失败，不是 Rust TransBuffer 状态机或链接错误。

## 8. 本次重构后的边界

现在 `TransBuffer` 的边界可以理解为：

```text
LoadQueue / DumpQueue / BufferManager / TransManager
        |
        v
C++ TransBuffer public API
        |
        v
Rust TransBuffer core
        |
        +--> Rust local/shared/watcher metadata + layout + shm lifecycle
        |
        +--> C++ callbacks
                 |
                 +--> Trans::Device / Trans::Buffer
                 +--> pthread process-shared locks
```

相比上一轮“Rust 运行在 C++ strategy 之上”的边界，现在 Rust 已经成为 TransBuffer strategy owner，C++ 只提供底层设备资源和系统锁能力。

## 9. 后续建议

后续如果继续推进，可以考虑：

- 在 Ascend 真实设备环境补跑 `UCCacheTransBufferTest` 和 CacheStore 传输链路测试。
- 为新版 shared layout 增加独立 C++/Rust 集成测试，专门覆盖 owner + watcher 双实例。
- 将 POSIX shm 的少量 raw FFI 封装成 Rust 内部小模块，降低 `trans_buffer.rs` 文件长度。
- 在文档中明确旧共享内存前缀 `uc_shm_cache_` 与新共享内存前缀 `uc_shm_cache_rs2_` 不兼容，避免混合部署。
- 如果未来允许引入依赖，可以用 `libc` crate 替换当前手写 POSIX FFI 类型和常量。
