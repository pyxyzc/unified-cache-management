# TransBuffer Rust 激进重构 Ascend 验证记录

## 1. 验证目标

本文记录 `TransBuffer` 进一步 Rust 化之后，在 Ascend 环境中的正确性验证过程和结果。

本轮重构后的模块边界是：

- Rust 接管 local/shared/watcher strategy。
- Rust 接管 metadata、bucket、free head、稳定 hash 和 shared memory layout。
- C++ 保留 `TransBuffer` public API。
- C++ 通过回调提供 Ascend host buffer 分配、shared host buffer 注册、pthread shared lock 等底层能力。

验证目标包括：

- Rust 单元测试通过。
- `cachestore` 和 `ucmstore.test` 能在 Ascend 构建目录中重新构建。
- `TransBuffer` 参数化测试覆盖 local/shared 两种 buffer 模式并全部通过。
- `BufferManager`、`LoadQueue`、`DumpQueue`、`TransManager` 集成链路全部通过。
- 测试失败日志中故意模拟 backend 错误的场景不被误判为重构问题。

## 2. 环境准备

在 Ascend 机器上先确认设备和 CANN 环境：

```bash
npu-smi info
ls -l /dev/davinci* /dev/ascend*
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0
```

如果这里失败，`TransBuffer::Setup()` 可能在 C++ 设备回调中失败，常见错误是：

```text
aclrtSetDeviceImpl: open device 0 failed
runtime result = 507899
```

这种错误属于 Ascend runtime 或设备可见性问题，不代表 Rust metadata 或 shared layout 逻辑错误。

## 3. 构建方式

建议使用独立 Ascend 构建目录：

```bash
cmake -S . -B /tmp/ucm-ascend-transbuffer-rs-verify \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
```

重构后需要重新构建 Rust staticlib、`cachestore` 和 store 单测：

```bash
cmake --build /tmp/ucm-ascend-transbuffer-rs-verify \
  --target ucm_cache_store_rs cachestore ucmstore.test -- -j8
```

本地开发目录中也执行了基础验证：

```bash
cargo fmt --manifest-path ucm/store/cache/rs/Cargo.toml --check
cargo test --manifest-path ucm/store/cache/rs/Cargo.toml
cmake --build build --target cachestore -- -j8
cmake --build build --target ucmstore.test -- -j8
```

Rust 单元测试结果：

```text
15 passed; 0 failed
```

## 4. TransBuffer 核心测试

执行命令：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/test/ucmstore.test \
  --gtest_filter=SharedCondition/UCCacheTransBufferTest.*
```

实际结果：

```text
[==========] Running 12 tests from 1 test suite.
[  PASSED  ] 12 tests.
```

通过的场景包括：

- `GetFirstNode`
- `BackendOnlyLoadReusesIdleCacheEntry`
- `BackendOnlyReservedGetDoesNotBypassWithoutLoadFlag`
- `BackendOnlyLoadCoalescesInFlightEntry`
- `GetReservedNode`
- `InsertDifferentDataRepeatedly`

每个场景都覆盖了 local 和 shared 两种参数化配置。

这组测试验证了：

- 首次 `Get` 返回 owner。
- 重复 `Get` 命中同一个 block/shard。
- `Ready` / `MarkReady` 状态共享。
- backend-only load 可以复用 idle ready entry，并将 ready 置回 false。
- in-flight load 可以 coalesce。
- reserved buffer 语义保持兼容。
- 多 batch、多 block、多 shard 重复插入和查询正常。
- shared memory 新布局在 Ascend 进程中可创建、注册、访问和释放。

## 5. CacheStore 集成测试

执行命令：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/test/ucmstore.test \
  --gtest_filter='UCCacheTransManagerTest.*:UCCacheLoadQueueTest.*:UCCacheDumpQueueTest.*:UCCacheBufferManagerTest.*'
```

实际结果：

```text
[==========] Running 14 tests from 4 test suites.
[  PASSED  ] 14 tests.
```

各测试集结果：

- `UCCacheBufferManagerTest`：5 个测试全部通过。
- `UCCacheDumpQueueTest`：2 个测试全部通过。
- `UCCacheLoadQueueTest`：3 个测试全部通过。
- `UCCacheTransManagerTest`：4 个测试全部通过。

这组测试验证了：

- `BufferManager` 通过重构后的 `TransBuffer::Exist` 合并本地 cache 命中和 backend lookup 结果。
- `LookupOnPrefix` 能正确把 backend miss 映射回原始 block index。
- backend-only lookup 可以绕过 cache。
- `DumpQueue` 可以通过 `TransBuffer::Get` 获取 cache buffer，完成 D2H、`MarkReady` 和 backend dump。
- `LoadQueue` 可以通过 owner/coalesce/reserved load 路径完成 backend load 和 H2D。
- `TransManager` 的 dump-then-load 组合路径在真实 Ascend runtime 下通过。

测试日志中出现的以下错误是用例主动模拟 backend 失败的预期路径：

```text
Failed(-1) to submit dump task(...) to backend.
Failed(-1) to submit load task(...) to backend.
Failed(-1) to wait backend(...) for task(...).
```

这些日志对应的用例最终都是 `[ OK ]`，不属于验证失败。

## 6. 通过标准

可以认为本轮 `TransBuffer` Rust 激进重构在 Ascend 环境中验证通过，当满足：

- Rust 单元测试全部通过。
- Ascend 构建目录中的 `cachestore` 和 `ucmstore.test` 构建成功。
- `UCCacheTransBufferTest` 参数化测试全部通过。
- `UCCacheBufferManagerTest`、`UCCacheLoadQueueTest`、`UCCacheDumpQueueTest`、`UCCacheTransManagerTest` 全部通过。
- 日志中的 backend 故障模拟均对应通过用例，不产生新增失败。

当前记录满足以上条件。

## 7. 后续补充验证

如果需要进一步扩大覆盖，可以追加执行底层 Ascend Trans 测试：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.*
```

也可以检查 `libcachestore.so` 中的 Rust FFI 符号：

```bash
nm -D /tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/cache/libcachestore.so \
  | grep ucm_cache_store_trans_buffer
```

预期包含 `ucm_cache_store_trans_buffer_data_at` 以及 `get/exist/acquire/release/ready` 等符号。
