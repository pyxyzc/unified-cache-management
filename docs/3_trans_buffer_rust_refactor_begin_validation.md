# TransBuffer Rust 重构 Ascend 验证指南

本文档记录如何在真实 Ascend/NPU 环境中验证 `TransBuffer` Rust 元数据核心重构的正确性。

## 1. 验证目标

本次验证关注：

- `RUNTIME_ENVIRONMENT=ascend` 下可以正常构建 `cachestore`。
- Rust `TransBuffer` FFI 符号已经进入 `libcachestore.so`。
- `TransBuffer` 原有 C++ public API 行为保持兼容。
- `UCCacheTransBufferTest` 覆盖的元数据语义在真实 Ascend buffer 初始化路径下通过。
- `LoadQueue` / `DumpQueue` / `TransManager` 调用 `TransBuffer` 的集成路径没有新增失败。
- 底层 Ascend trans 数据搬运链路正常，避免把设备环境问题误判为 Rust 元数据问题。

## 2. 前置环境检查

在 Ascend 机器上先确认驱动、设备节点和 CANN 环境可用：

```bash
npu-smi info
ls -l /dev/davinci* /dev/ascend*
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0
```

如果 `npu-smi info` 失败，或者设备节点不可见，后续 `TransBuffer::Setup()` 中的设备初始化通常会失败。

典型失败表现：

```text
aclrtSetDeviceImpl: open device 0 failed
runtime result = 507899
```

这类失败发生在 Ascend runtime / device 初始化阶段，不代表 Rust 元数据状态机错误。

## 3. 配置 Ascend 构建目录

建议使用独立 build 目录，避免复用本地 `simu` 构建缓存：

```bash
cmake -S . -B /tmp/ucm-ascend-transbuffer-verify \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
```

如果 Ascend toolkit 不在默认路径，额外指定：

```bash
-DASCEND_ROOT=/path/to/Ascend/ascend-toolkit/latest
```

`-Wno-error=unused-result` 用于绕开已有 Posix 测试代码中的无关 `system()` warning，不影响 `TransBuffer` 验证。

## 4. 构建验证目标

构建 `cachestore`：

```bash
cmake --build /tmp/ucm-ascend-transbuffer-verify --target cachestore -- -j8
```

构建 store/shared 单测：

```bash
cmake --build /tmp/ucm-ascend-transbuffer-verify --target ucmstore.test ucmshared.test -- -j8
```

构建日志中应能看到：

```text
Building Rust CacheStore core
Built target ucm_cache_store_rs
Building CXX object ucm/shared/trans/ascend/...
Built target cachestore
Built target ucmstore.test
```

其中 `ucm/shared/trans/ascend/...` 表示当前构建使用的是 Ascend trans 后端。

## 5. 符号和依赖检查

确认 Rust `TransBuffer` FFI 符号进入 `libcachestore.so`：

```bash
nm -D /tmp/ucm-ascend-transbuffer-verify/ucm/store/cache/libcachestore.so \
  | grep ucm_cache_store_trans_buffer
```

预期至少包含：

```text
ucm_cache_store_trans_buffer_new
ucm_cache_store_trans_buffer_free
ucm_cache_store_trans_buffer_get
ucm_cache_store_trans_buffer_exist
ucm_cache_store_trans_buffer_acquire
ucm_cache_store_trans_buffer_release
ucm_cache_store_trans_buffer_ready
ucm_cache_store_trans_buffer_mark_ready
ucm_cache_store_trans_buffer_mark_not_ready
```

确认 Ascend 动态库依赖可以解析：

```bash
ldd /tmp/ucm-ascend-transbuffer-verify/ucm/store/cache/libcachestore.so \
  | grep -E 'ascend|libascendcl|not found'
```

预期：

- 能看到 `libascendcl.so`。
- 不出现 `not found`。

## 6. 核心单测验证

优先运行 `TransBuffer` 自身测试：

```bash
ctest --test-dir /tmp/ucm-ascend-transbuffer-verify \
  -R UCCacheTransBufferTest \
  --output-on-failure
```

该测试覆盖：

- 首次 `Get` 返回 owner。
- 同一 block/shard 的二次 `Get` 命中同一 buffer，返回非 owner。
- `Ready` / `MarkReady` 状态共享。
- backend-only load 复用 idle ready cache entry，并将 ready 置 false。
- in-flight load coalesce。
- reserved buffer 只在允许时参与分配。
- 多 batch、多 block、多 shard 反复插入和命中。
- `shareBufferEnable=false/true` 两种参数化场景。

通过标准：

```text
100% tests passed
```

## 7. CacheStore 传输链路验证

在 `TransBuffer` 自身测试通过后，运行调用链更长的 CacheStore 传输测试：

```bash
ctest --test-dir /tmp/ucm-ascend-transbuffer-verify \
  -R 'UCCacheTransManagerTest|UCCacheLoadQueueTest|UCCacheDumpQueueTest|UCCacheBufferManagerTest' \
  --output-on-failure
```

这些测试覆盖：

- `TransManager` 提交 Load/Dump 任务。
- `LoadQueue` 调用 `TransBuffer::Get(..., allowReserved=true, isLoad=true)`。
- `DumpQueue` 调用 `TransBuffer::Get(...)`、`Ready()`、`MarkReady()`。
- `BufferManager` 调用 `TransBuffer::Exist(...)`。
- Rust metadata core 与 C++ `Handle` RAII 引用计数配合。

通过标准：

- 上述用例无新增失败。
- 如果存在失败，优先确认是否发生在设备初始化、Ascend memcpy、backend store I/O，而不是 `TransBuffer` owner/ready/reference 断言。

## 8. 底层 Ascend trans 验证

为了排除设备搬运链路本身的问题，建议运行 shared trans 测试：

```bash
/tmp/ucm-ascend-transbuffer-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.*
```

至少应通过真实数据拷贝用例：

```bash
/tmp/ucm-ascend-transbuffer-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.CopyDataWithCE
```

这可以确认：

- `Device.Setup(0)` 成功。
- stream 创建和同步成功。
- host/device buffer 分配成功。
- H2D / D2H 数据拷贝成功。

如果这些底层测试失败，应先修复 Ascend runtime、驱动、设备可见性或 ACL 依赖问题，再判断 `TransBuffer` 重构结果。

## 9. 推荐完整验证顺序

建议按以下顺序执行：

```bash
npu-smi info
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0

cmake -S . -B /tmp/ucm-ascend-transbuffer-verify \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result

cmake --build /tmp/ucm-ascend-transbuffer-verify --target cachestore -- -j8
cmake --build /tmp/ucm-ascend-transbuffer-verify --target ucmstore.test ucmshared.test -- -j8

nm -D /tmp/ucm-ascend-transbuffer-verify/ucm/store/cache/libcachestore.so \
  | grep ucm_cache_store_trans_buffer

ldd /tmp/ucm-ascend-transbuffer-verify/ucm/store/cache/libcachestore.so \
  | grep -E 'ascend|libascendcl|not found'

ctest --test-dir /tmp/ucm-ascend-transbuffer-verify \
  -R UCCacheTransBufferTest \
  --output-on-failure

ctest --test-dir /tmp/ucm-ascend-transbuffer-verify \
  -R 'UCCacheTransManagerTest|UCCacheLoadQueueTest|UCCacheDumpQueueTest|UCCacheBufferManagerTest' \
  --output-on-failure

/tmp/ucm-ascend-transbuffer-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.*
```

## 10. 最终通过标准

可以认为 `TransBuffer` Rust 重构在 Ascend 环境下验证通过，当且仅当：

- `npu-smi info` 正常显示设备。
- `cachestore`、`ucmstore.test`、`ucmshared.test` 构建成功。
- `libcachestore.so` 中存在 `ucm_cache_store_trans_buffer_*` 符号。
- `ldd libcachestore.so` 不出现 `not found`。
- `UCCacheTransBufferTest` 全部通过。
- CacheStore 传输相关测试无新增失败。
- `UCTransUnitTest.*` 至少核心数据拷贝用例通过。
