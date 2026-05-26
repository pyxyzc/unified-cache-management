# CacheStore Rust 重构总结

## 1. 任务背景

用户要求：使用 Rust 重构 UCM 的 `CacheStore` 模块，并尽量不要修改 UCM 的上下游调用关系。

这意味着重构目标不是重写整个调用链，而是在保持现有插件 ABI、Python 入口和 PipelineStore 调用方式不变的前提下，把 `CacheStore` 内部逐步迁移到 Rust。

## 2. 现有结构梳理

当前 `CacheStore` 是一个 C++ 动态库插件：

- 插件库：`ucm/store/cache/libcachestore.so`
- 入口符号：`extern "C" UC::StoreV1* MakeCacheStore()`
- 抽象接口：`UC::StoreV1`
- 上游加载方：`ucm/store/pipeline/cpy/pipeline_store.py.cc`
- Python 配置入口：`ucm/store/pipeline/connector.py`

关键文件：

- `ucm/store/cache/cc/cache_store.cc`
  - `CacheStore` 主类，实现 `StoreV1`。
  - 原本负责配置解析、配置校验、buffer manager 初始化、trans manager 初始化。

- `ucm/store/cache/cc/buffer_manager.h`
  - 负责 Lookup / LookupOnPrefix。
  - 在本地 cache miss 时 fallback 到后端 store。

- `ucm/store/cache/cc/trans_buffer.{h,cc}`
  - 负责 host 侧缓存 buffer。
  - 包含本地 buffer、共享内存 buffer、buffer metadata、引用计数、ready 状态等逻辑。

- `ucm/store/cache/cc/load_queue.{h,cc}` 和 `dump_queue.{h,cc}`
  - 负责异步 Load/Dump 队列。
  - 调用现有 `Trans` 抽象完成设备和 host 之间的数据传输。

- `ucm/store/cache/cc/trans_task.h`
  - 定义 `TransTask`。
  - 原本用 C++ `static std::atomic<size_t>` 生成 task id。

## 3. 关键判断

为了最大限度保持 UCM 上下游不变，采用了“C++ 门面 + Rust 核心”的方案。

保留 C++ 门面的原因：

- `StoreV1` 是 C++ 虚接口，不适合直接由 Rust 实现并暴露为 C++ ABI。
- `Status`、`Expected`、`Dictionary`、`TaskDesc` 等都是 C++ 类型，直接跨 Rust 边界风险较高。
- Python PipelineStore 通过 `dlopen + dlsym` 加载 `MakeCacheStore`，保留这个符号可以避免改 Python 和 connector。
- 现有 `Trans` 设备传输层覆盖 `simu/cuda/ascend/musa/maca`，首轮不宜迁移，避免扩大风险。

因此本次重构的边界是：

- C++ 继续实现 `StoreV1` 和导出 `MakeCacheStore`。
- Rust 只接管 CacheStore 内部可稳定通过 C ABI 表达的逻辑。
- Rust 和 C++ 之间只传递 C ABI 安全的 plain data。

## 4. 本次实际改动

### 4.1 新增 Rust crate

新增目录：

```text
ucm/store/cache/rs/
```

主要文件：

```text
ucm/store/cache/rs/Cargo.toml
ucm/store/cache/rs/Cargo.lock
ucm/store/cache/rs/.cargo/config.toml
ucm/store/cache/rs/src/lib.rs
```

`Cargo.toml` 中将 crate 类型设置为：

```toml
crate-type = ["staticlib"]
```

同时设置：

```toml
panic = "abort"
```

这样可以避免 Rust panic 穿越 C ABI 边界。

`.cargo/config.toml` 中设置：

```toml
rustflags = ["-C", "relocation-model=pic"]
```

这样 Rust staticlib 可以安全链接进 `libcachestore.so`。

### 4.2 Rust 目前负责的逻辑

Rust 文件：

```text
ucm/store/cache/rs/src/lib.rs
```

目前 Rust 侧实现了：

- `CacheStoreConfigView`
  - C++ 传给 Rust 的配置视图。

- `CacheStoreFfiStatus`
  - Rust 返回给 C++ 的轻量状态结构。

- `CacheStoreCore`
  - Rust 侧 CacheStore core 状态，目前保存 `trans_enabled`。

- 配置校验逻辑
  - 检查 store backend 是否存在。
  - 检查 `device_id`。
  - 检查 `unique_id`。
  - 检查 CPU affinity core 范围。
  - 检查 tensor size、shard size、block size。
  - 检查 buffer capacity。
  - 检查 waiting/running queue depth。
  - 检查 stream number。

- task id 生成
  - 使用 Rust `AtomicUsize` 生成递增 task id。

Rust 暴露的 C ABI 函数：

```text
ucm_cache_store_core_new
ucm_cache_store_core_free
ucm_cache_store_core_trans_enabled
ucm_cache_store_next_task_id
```

### 4.3 新增 C ABI 桥接头

新增文件：

```text
ucm/store/cache/cc/cache_store_rs.h
```

该文件定义 C++ 侧可见的 Rust ABI：

- `Rs::Status`
- `Rs::ConfigView`
- opaque `Rs::Core`
- Rust FFI 函数声明

这里刻意没有把 C++ 的复杂类型直接传给 Rust，而是把 C++ 配置解析后的结果转成 `Rs::ConfigView`。

### 4.4 修改 CacheStore 主流程

修改文件：

```text
ucm/store/cache/cc/cache_store.cc
```

主要变化：

- 删除原来的 C++ `CheckConfig` / `CheckSizeConfig` 实现。
- 新增 `SetupCore`。
- 新增 `MakeConfigView`。
- 新增 `ResetCore`。
- `Setup()` 中先调用 Rust 创建 core 并执行配置校验。
- `transEnable_` 改为从 Rust core 读取。
- 析构时释放 Rust core。

改造后的流程：

```text
Dictionary
  -> C++ ParseConfig
  -> C++ MakeConfigView
  -> Rust validate_config
  -> Rust CacheStoreCore
  -> C++ BufferManager/TransManager 初始化
```

这样做保持了 `StoreV1::Setup(const Dictionary&)` 的外部接口不变。

### 4.5 修改任务 id 生成

修改文件：

```text
ucm/store/cache/cc/trans_task.h
```

原逻辑：

```cpp
static std::atomic<size_t> id{1};
return id.fetch_add(1, std::memory_order_relaxed);
```

改为：

```cpp
return Rs::ucm_cache_store_next_task_id();
```

这样 task id 的生成逻辑已经迁移到 Rust。

### 4.6 修改 CMake 构建

修改文件：

```text
ucm/store/cache/CMakeLists.txt
```

新增逻辑：

- 查找 `cargo`。
- 设置 Rust 构建目录到 CMake binary dir 下的 `rust-target`。
- 用 `add_custom_command` 构建 Rust staticlib。
- 声明 imported static library target。
- 将 Rust staticlib 链接进 `cachestore`。

`cachestore` 仍然是原来的 C++ shared library，只是内部静态链接了 Rust 产物。

### 4.7 修改 .gitignore

修改文件：

```text
.gitignore
```

新增：

```gitignore
**/target/**
```

避免提交 Cargo 构建产物。

## 5. 保持不变的内容

以下内容保持不变：

- `libcachestore.so` 文件名。
- `MakeCacheStore` 入口符号。
- `StoreV1` 接口。
- Python `UcmPipelineStore` 调用方式。
- Pipeline 配置字符串，例如 `Cache|Posix`、`Cache|Empty`。
- vLLM/SGLang connector 配置。
- `Lookup`、`LookupOnPrefix`、`Load`、`Dump`、`Check`、`Wait` 对外方法。
- `BufferManager`、`TransBuffer`、`LoadQueue`、`DumpQueue` 的核心传输逻辑。

## 6. 验证结果

已执行并通过：

```bash
cargo fmt --manifest-path ucm/store/cache/rs/Cargo.toml --check
```

```bash
cargo build --manifest-path ucm/store/cache/rs/Cargo.toml --release
```

```bash
cmake --build /tmp/ucm-cache-rust-build --target cachestore -- -j8
```

动态符号检查：

```bash
nm -D /tmp/ucm-cache-rust-build/ucm/store/cache/libcachestore.so | rg 'MakeCacheStore|ucm_cache_store'
```

确认：

- `MakeCacheStore` 仍然导出。
- Rust FFI 符号已经链接进 `libcachestore.so`。

完整 `ucmstore.test` 目标构建未通过，但失败点在既有 Posix 测试代码：

```text
ucm/store/test/case/detail/path_base.h
ucm/store/test/case/posix/posix_file_test.cc
```

失败原因是 `system()` 返回值未使用，在 `-Werror=unused-result` 下被当成错误。这个问题与本次 CacheStore Rust 改造无关。

## 7. 当前变更文件

修改文件：

```text
.gitignore
ucm/store/cache/CMakeLists.txt
ucm/store/cache/cc/cache_store.cc
ucm/store/cache/cc/trans_task.h
```

新增文件：

```text
ucm/store/cache/cc/cache_store_rs.h
ucm/store/cache/rs/Cargo.toml
ucm/store/cache/rs/Cargo.lock
ucm/store/cache/rs/.cargo/config.toml
ucm/store/cache/rs/src/lib.rs
```

## 8. 后续重构建议

本次是第一阶段 Rust 化，主要建立 Rust 接入方式和稳定边界。

建议后续按以下顺序继续：

1. 迁移 `BufferManager` 的 Lookup 策略。
   - 风险较低。
   - 可以继续保留 C++ `StoreV1` 后端调用。

2. 迁移 `TransBuffer` 的纯元数据逻辑。
   - 例如 hash、reference、ready 状态管理。
   - 共享内存布局需要保持 ABI 兼容。

3. 谨慎评估共享内存和锁实现迁移。
   - 当前依赖 pthread robust mutex、spinlock、POSIX shm。
   - Rust 侧需要明确跨进程锁语义，不能简单替换成普通 `Mutex`。

4. 最后再考虑 Load/Dump 队列。
   - 这部分涉及线程、SPSC 队列、设备 stream、backend task 等，迁移风险最高。

## 9. 总结

本次重构完成了 CacheStore Rust 化的基础架构搭建：Rust 已经进入 `libcachestore.so` 的构建和运行路径，但对 UCM 上下游保持兼容。

当前方案的重点是稳住边界：

- 对外仍是 C++ `StoreV1` 插件。
- 对内开始引入 Rust core。
- C++ 和 Rust 之间只走 C ABI。
- 现有 Python、PipelineStore 和模型框架集成路径无需修改。

