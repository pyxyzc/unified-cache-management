# TransBuffer Rust 激进重构调试记录

## 1. 问题现象

在 Ascend 构建目录中执行 CacheStore 集成测试：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/test/ucmstore.test \
  --gtest_filter='UCCacheTransManagerTest.*:UCCacheLoadQueueTest.*:UCCacheDumpQueueTest.*:UCCacheBufferManagerTest.*'
```

初始结果是：

```text
[==========] Running 14 tests from 4 test suites.
[  PASSED  ] 4 tests.
[  FAILED  ] 10 tests.
```

失败集中在：

- `UCCacheBufferManagerTest.*`
- `UCCacheDumpQueueTest.*`
- `UCCacheLoadQueueTest.*`

同时，`UCCacheTransManagerTest.*` 全部通过。

典型失败位置：

```text
bufferMgr.Setup(config).Success()
Actual: false
Expected: true
```

以及：

```text
auto s = buffer.Setup(config);
ASSERT_EQ(s, UC::Status::OK());
```

由于 `buffer.Setup(config)` 提前失败，后续 mock backend 的 `Load`、`Dump`、`Wait` 没有被调用，于是出现了 mock call count 失败。

## 2. 初步判断

`UCCacheTransManagerTest.*` 已经全部通过，这说明：

- Ascend device 可以初始化。
- stream 创建和同步链路基本可用。
- host/device copy 集成路径基本可用。

因此问题不像是 Ascend runtime、驱动或底层 Trans 后端不可用。

失败发生在 `BufferManager::Setup`、`LoadQueue` / `DumpQueue` 测试中的 `TransBuffer::Setup`，因此排查重点转向 Rust TransBuffer config 校验和 setup 行为。

## 3. 关键线索

查看失败用例构造的配置后，发现这些测试普遍使用：

```cpp
config.shardSize = 4096 或 32768;
config.bufferCapacity = config.shardSize * 1024;
config.loadExclusiveBufferNumber 使用默认值;
```

默认值来自：

```text
ucm/store/cache/cc/global_config.h
```

其中：

```cpp
size_t loadExclusiveBufferNumber{1024};
```

因此这些用例中的节点数为：

```text
n_node = bufferCapacity / shardSize = 1024
reserved_number = loadExclusiveBufferNumber = 1024
```

也就是 `reserved_number == n_node`。

## 4. 根因

Rust 新实现中的 config 校验原本写成：

```rust
if n_node <= self.reserved_number {
    return Err(...);
}
```

这会拒绝 `reserved_number == n_node`。

但旧 C++ `TransBuffer` setup 阶段并不拒绝这个配置。旧逻辑只在分配节点时使用：

```cpp
const auto limit = header_.nNode - (allowReserved ? 0 : base_.reservedNumber);
if (header_.freeHead >= limit) { header_.freeHead = 0; }
return header_.freeHead++;
```

当 `reservedNumber == nNode` 时，旧实现仍允许 setup 成功。

也就是说，这次失败不是 runtime 失败，而是 Rust 新增校验比旧 C++ 更严格，破坏了已有测试配置的兼容性。

## 5. 修复方式

将 Rust 校验改为只拒绝 `reserved_number > n_node`：

```rust
if n_node < self.reserved_number {
    return Err(...);
}
```

修改位置：

```text
ucm/store/cache/rs/src/trans_buffer.rs
```

同时新增 Rust 单元测试覆盖兼容场景：

```rust
fn reserved_number_may_match_node_count_for_legacy_configs()
```

该测试构造：

```text
total_size = NODE_SIZE * RESERVED
reserved_number = RESERVED
```

并断言 Rust core 创建成功。

## 6. 修复后本地验证

执行：

```bash
cargo fmt --manifest-path ucm/store/cache/rs/Cargo.toml --check
cargo test --manifest-path ucm/store/cache/rs/Cargo.toml
```

结果：

```text
15 passed; 0 failed
```

继续执行：

```bash
cmake --build build --target cachestore -- -j8
cmake --build build --target ucmstore.test -- -j8
```

结果均通过。

## 7. 修复后 Ascend 验证

重新构建 Ascend 验证目录：

```bash
cmake --build /tmp/ucm-ascend-transbuffer-rs-verify \
  --target ucm_cache_store_rs cachestore ucmstore.test -- -j8
```

再次执行 CacheStore 集成测试：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/test/ucmstore.test \
  --gtest_filter='UCCacheTransManagerTest.*:UCCacheLoadQueueTest.*:UCCacheDumpQueueTest.*:UCCacheBufferManagerTest.*'
```

结果：

```text
[==========] Running 14 tests from 4 test suites.
[  PASSED  ] 14 tests.
```

随后执行 TransBuffer 核心测试：

```bash
/tmp/ucm-ascend-transbuffer-rs-verify/ucm/store/test/ucmstore.test \
  --gtest_filter=SharedCondition/UCCacheTransBufferTest.*
```

结果：

```text
[==========] Running 12 tests from 1 test suite.
[  PASSED  ] 12 tests.
```

## 8. 调试结论

本次问题的本质是兼容性校验过严：

- 旧 C++ 允许 `reserved_number == n_node`。
- Rust 初版错误地要求 `reserved_number < n_node`。
- 多个既有测试刚好依赖默认 `loadExclusiveBufferNumber == 1024` 和 `n_node == 1024`。
- 调整为只拒绝 `reserved_number > n_node` 后，行为恢复与旧实现一致。

修复后，TransBuffer 自身测试和 CacheStore 集成测试都在 Ascend 环境中通过，说明这个问题已经闭环。
