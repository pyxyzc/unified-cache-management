# TransBuffer Rust 重构开发记录

## 1. Prompt 与目标

本次工作的用户 prompt 主要包括：

```text
使用 Rust 重构 TransBuffer 模块
```

随后进入计划和实施阶段：

```text
实施这个计划
```

目标是在尽量不改变 UCM 上下游调用关系的前提下，将 `UC::CacheStore::TransBuffer`
中适合 Rust 化的核心逻辑迁移到 Rust。最终选择的是“元数据核心”重构，而不是完整迁移
buffer 分配、共享内存和设备注册逻辑。

## 2. 关键方案决策

### 2.1 重构范围

选择方案：

```text
Rust 接管 hash 之后的 node 元数据、引用计数、ready/owner、get/exist 状态机；
C++ 保留 buffer 分配、设备注册、pthread/shm 策略。
```

不在本轮迁移的内容：

- `Trans::Device` 和 `Trans::Buffer` 设备侧资源管理。
- pinned host buffer / DirectIO buffer 分配。
- POSIX shm 创建、mmap、清理和注册。
- pthread robust mutex / spinlock 的跨进程锁语义。
- `LoadQueue`、`DumpQueue`、`BufferManager` 对外行为。

原因：

- 现有 `TransBuffer` 同时处理资源层和元数据层，完整迁移风险较大。
- 共享内存布局和跨进程锁需要保持兼容，不能简单替换为 Rust `Mutex`。
- C++ `TransBuffer` public API 已被 `LoadQueue`、`DumpQueue`、`BufferManager` 使用，保持接口不变可以降低改动面。

### 2.2 Hash 兼容策略

选择方案：

```text
保留 C++ bucket hash，Rust 通过 C ABI 回调获取 bucket。
```

原因：

- 当前 bucket hash 使用 C++ `std::hash<std::string_view>` 和 shard hash 组合。
- 如果直接改成 Rust hash，同名共享内存中的已有条目可能无法被新进程命中。
- 保留 C++ hash 可以让 Rust 只接管 hash 之后的元数据状态机，避免共享内存兼容性风险。

## 3. 原实现结构

目标文件：

```text
ucm/store/cache/cc/trans_buffer.h
ucm/store/cache/cc/trans_buffer.cc
```

原 `TransBuffer` 主要职责：

- 根据 `BlockId + shard` 计算 bucket。
- 在 bucket 链表中查找已有 node。
- 维护 `reference` 引用计数。
- 维护 `ready` 状态。
- 在未命中时从 ring-like `freeHead` 中取可复用 node。
- 处理 reserved buffer 区域。
- 处理 backend-only load 下的 ready entry 复用语义。
- 将 node 移动到新 bucket 链表，或从旧 bucket 链表移除。

原 `BufferStrategy` 及派生类职责：

- `LocalBufferStrategy`：本地锁、本地元数据、本地 host buffer。
- `SharedBufferStrategy`：共享内存 header、共享 metadata、跨进程锁、设备注册。
- `SharedBufferWatcherStrategy`：只观察共享 buffer metadata，不返回数据指针。

本次保留 `BufferStrategy` 资源层，只把元数据状态机迁移到 Rust。

## 4. 实施内容

### 4.1 新增 Rust TransBuffer 核心

新增文件：

```text
ucm/store/cache/rs/src/trans_buffer.rs
```

Rust 侧新增核心类型：

- `CacheStoreTransBufferMetaNode`
- `CacheStoreTransBufferStrategyView`
- `CacheStoreTransBufferGetResult`
- `CacheStoreTransBufferCore`

Rust core 通过 `CacheStoreTransBufferStrategyView` 调用 C++ strategy：

- `bucket_of`
- `bucket_lock`
- `bucket_try_lock`
- `bucket_unlock`
- `node_lock`
- `node_unlock`
- `first_at`
- `fetch_node`
- `meta_at`

Rust 侧实现的核心行为：

- `get`
- `exist`
- `find_at`
- `alloc`
- `move_to`
- `remove`
- `acquire`
- `release`
- `ready`
- `mark_ready`
- `mark_not_ready`

### 4.2 扩展 C ABI

修改文件：

```text
ucm/store/cache/cc/cache_store_rs.h
```

新增 C ABI 声明：

- `ucm_cache_store_trans_buffer_new`
- `ucm_cache_store_trans_buffer_free`
- `ucm_cache_store_trans_buffer_get`
- `ucm_cache_store_trans_buffer_exist`
- `ucm_cache_store_trans_buffer_acquire`
- `ucm_cache_store_trans_buffer_release`
- `ucm_cache_store_trans_buffer_ready`
- `ucm_cache_store_trans_buffer_mark_ready`
- `ucm_cache_store_trans_buffer_mark_not_ready`

同时新增 C++ 可见的 ABI struct：

- `Rs::TransBufferMetaNode`
- `Rs::TransBufferStrategyView`
- `Rs::TransBufferGetResult`
- opaque `Rs::TransBufferCore`

### 4.3 改造 C++ TransBuffer 外观

修改文件：

```text
ucm/store/cache/cc/trans_buffer.h
ucm/store/cache/cc/trans_buffer.cc
```

核心变化：

- `TransBuffer` 增加 `Rs::TransBufferCore* core_`。
- `TransBuffer::~TransBuffer()` 释放 Rust core。
- `Setup()` 仍先创建并 setup C++ strategy，再用 strategy callback 创建 Rust core。
- `Get()` 改为调用 `ucm_cache_store_trans_buffer_get`。
- `Exist()` 改为调用 `ucm_cache_store_trans_buffer_exist`。
- `Acquire/Release/Ready/MarkReady/MarkNotReady` 改为调用 Rust core。
- C++ 原 `ExistAt/FindAt/Alloc/MoveTo/Remove` 元数据逻辑删除。

保留内容：

- C++ `TransBuffer::Handle` RAII 语义不变。
- C++ `BufferStrategy` 抽象不变。
- local/shared/watcher buffer 资源管理不变。
- public API 不变。

### 4.4 CMake 依赖更新

修改文件：

```text
ucm/store/cache/CMakeLists.txt
```

将 Rust 源文件依赖从只跟踪 `src/lib.rs` 改为：

```cmake
file(GLOB_RECURSE UCM_CACHE_STORE_RS_SOURCE_FILES
    CONFIGURE_DEPENDS
    ${UCM_CACHE_STORE_RS_DIR}/src/*.rs)
```

这样新增 `src/trans_buffer.rs` 后，CMake 构建能正确触发 Rust staticlib 重建。

## 5. Rust 单测覆盖

新增 Rust fake strategy 测试，覆盖以下场景：

- 首次 `get` 返回 owner，二次 `get` 返回非 owner，并共享 ready 状态。
- backend-only load 复用 idle ready entry，并将 ready 置 false。
- backend-only load 对 in-flight entry 进行 coalesce。
- reserved node 只有在 `allowReserved = true` 时可被使用。
- node 移动到新 bucket 时正确从旧链表摘除。

原有 lookup 相关 Rust 测试继续保留。

## 6. 验证结果

已执行并通过：

```bash
cargo fmt --manifest-path ucm/store/cache/rs/Cargo.toml --check
```

```bash
cargo test --manifest-path ucm/store/cache/rs/Cargo.toml
```

结果：

```text
13 passed; 0 failed
```

已执行并通过：

```bash
cmake --build build --target cachestore -- -j8
```

结果：

```text
[100%] Built target cachestore
```

## 7. 集成测试说明

曾尝试运行：

```bash
ctest --test-dir build -R UCCacheTransBufferTest --output-on-failure
```

当前本地环境中 12 个参数化用例均失败，失败点一致为设备 0 / Ascend runtime 初始化失败：

```text
aclrtSetDeviceImpl: open device 0 failed
runtime result = 507899
```

这些失败发生在 `TransBuffer::Setup()` 设备初始化阶段，不是 Rust 元数据状态机断言失败。

建议在具备可用设备运行时的环境中重新执行：

```bash
ctest --test-dir build -R UCCacheTransBufferTest --output-on-failure
```

## 8. 最终结构总结

重构后边界如下：

```text
C++ TransBuffer public API
  -> Rust TransBufferCore 元数据状态机
      -> C ABI strategy callbacks
          -> C++ BufferStrategy
              -> Local / Shared / Watcher resource management
```

这次重构的核心收益：

- `TransBuffer` 的复杂元数据状态机从 C++ 迁移到 Rust。
- C++ 调用者和 public API 保持兼容。
- 共享内存、设备资源和 pthread 锁语义保持在原 C++ 实现中。
- Rust 单测可在无设备环境中验证核心元数据行为。
