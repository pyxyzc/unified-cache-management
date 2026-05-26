# Ascend Trans Rust 重构总结

## 1. 相关 Prompt

本次只总结 `ucm/shared/trans/ascend` 模块相关的重构。

相关用户需求是：

> 是否可以把 `trans` 模块单独拿出来 Ascend 相关的 `trans`，然后做一个简单的 Rust 重构 trans 模块的更换，然后在这个环境验证。

这个需求的核心含义是：

- 不继续扩大到整个 UCM。
- 不重写 CacheStore。
- 只针对 `ucm/shared/trans/ascend` 做一个小范围 Rust 替换。
- 尽量保持现有 C++ `UC::Trans` 对外接口不变。
- 在当前环境中完成可执行的构建、链接和基础运行验证。

## 2. 现有 Ascend Trans 结构

Ascend trans 当前位于：

```text
ucm/shared/trans/ascend/
```

原始核心文件：

```text
ucm/shared/trans/ascend/CMakeLists.txt
ucm/shared/trans/ascend/ascend_device.cc
ucm/shared/trans/ascend/ascend_stream.cc
ucm/shared/trans/ascend/ascend_stream.h
ucm/shared/trans/ascend/ascend_buffer.cc
ucm/shared/trans/ascend/ascend_buffer.h
```

顶层 `ucm/shared/trans/CMakeLists.txt` 会根据 `RUNTIME_ENVIRONMENT` 选择 runtime 实现：

```cmake
if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    add_subdirectory(ascend)
endif()
```

因此 Ascend trans 本身已经是一个相对独立的 runtime 分支，适合单独做 Rust 化试点。

对外接口仍由 C++ 头文件定义：

```text
ucm/shared/trans/device.h
ucm/shared/trans/stream.h
ucm/shared/trans/buffer.h
```

也就是说，上游代码仍然通过这些 C++ 接口使用 trans：

- `UC::Trans::Device`
- `UC::Trans::Stream`
- `UC::Trans::Buffer`

## 3. 设计选择

本次没有把 `UC::Trans::Device` / `Stream` / `Buffer` 直接改成 Rust 实现。

原因：

- 这些类是 C++ 抽象接口，直接跨 Rust 实现 C++ 虚表 ABI 风险高。
- CacheStore、Python `ucmtrans`、其他 C++ 调用方都依赖这些 C++ 接口。
- Ascend trans 内部大部分逻辑是对 AscendCL ACL C API 的薄封装，更适合先迁移 ACL 调用层。

采用的方案是：

```text
上游 C++ / Python / CacheStore
        |
        v
UC::Trans::Device / Stream / Buffer  C++ 接口保持不变
        |
        v
AscendDevice / AscendStream / AscendBuffer  C++ 类保持不变
        |
        v
Rust FFI wrapper
        |
        v
AscendCL ACL C API
```

这样可以做到：

- 对外接口不变。
- 只替换 Ascend runtime 内部 ACL 调用薄层。
- 不影响 `simu`、`cuda`、`musa`、`maca` 等其他 runtime。
- 当前环境可以通过 CMake 单独验证 Ascend trans 链接路径。

## 4. 新增文件

### 4.1 Rust crate

新增目录：

```text
ucm/shared/trans/ascend/rs/
```

新增文件：

```text
ucm/shared/trans/ascend/rs/Cargo.toml
ucm/shared/trans/ascend/rs/Cargo.lock
ucm/shared/trans/ascend/rs/.cargo/config.toml
ucm/shared/trans/ascend/rs/src/lib.rs
```

`Cargo.toml` 关键配置：

```toml
[lib]
crate-type = ["staticlib"]

[profile.release]
panic = "abort"

[profile.dev]
panic = "abort"
```

含义：

- Rust 产物编译为 static library。
- 链接进 C++ `trans` 静态库。
- panic 直接 abort，避免 panic 穿越 C ABI。

`.cargo/config.toml` 关键配置：

```toml
[build]
rustflags = ["-C", "relocation-model=pic"]
```

含义：

- Rust staticlib 生成 PIC 代码。
- 便于后续被链接进 shared object，例如 `ucmtrans` 或 `libcachestore.so`。

### 4.2 C++ FFI 头

新增文件：

```text
ucm/shared/trans/ascend/ascend_rs.h
```

该文件声明 C++ 可调用的 Rust FFI 函数。

## 5. Rust 侧封装内容

Rust 实现文件：

```text
ucm/shared/trans/ascend/rs/src/lib.rs
```

Rust 侧通过 `extern "C"` 声明 AscendCL ACL API，并导出 UCM 自己的 FFI wrapper。

### 5.1 Device 相关

封装：

```text
aclrtSetDevice
```

导出：

```text
ucm_ascend_trans_set_device
```

### 5.2 Stream 相关

封装：

```text
aclrtCreateStreamWithConfig
aclrtDestroyStream
aclrtSynchronizeStream
aclrtStreamWaitEvent
```

导出：

```text
ucm_ascend_trans_create_stream
ucm_ascend_trans_destroy_stream
ucm_ascend_trans_synchronize_stream
ucm_ascend_trans_stream_wait_event
```

### 5.3 Copy 相关

封装：

```text
aclrtMemcpy
aclrtMemcpyAsync
```

导出：

```text
ucm_ascend_trans_device_to_host
ucm_ascend_trans_device_to_host_async
ucm_ascend_trans_host_to_device
ucm_ascend_trans_host_to_device_async
```

### 5.4 Callback report 相关

封装：

```text
aclrtSubscribeReport
aclrtUnSubscribeReport
aclrtProcessReport
aclrtLaunchCallback
```

导出：

```text
ucm_ascend_trans_subscribe_report
ucm_ascend_trans_unsubscribe_report
ucm_ascend_trans_process_report
ucm_ascend_trans_launch_callback
```

### 5.5 Buffer 相关

封装：

```text
aclrtMalloc
aclrtFree
aclrtMallocHost
aclrtFreeHost
aclrtHostRegister
aclrtHostRegisterV2
aclrtHostGetDevicePointer
aclrtHostUnregister
```

导出：

```text
ucm_ascend_trans_malloc_device
ucm_ascend_trans_free_device
ucm_ascend_trans_malloc_host
ucm_ascend_trans_free_host
ucm_ascend_trans_host_register
ucm_ascend_trans_host_register_v2
ucm_ascend_trans_host_get_device_pointer
ucm_ascend_trans_host_unregister
```

## 6. 修改文件

### 6.1 `ucm/shared/trans/ascend/CMakeLists.txt`

修改内容：

- 查找 `cargo`。
- 设置 Rust crate 路径。
- 设置 Rust target 输出目录。
- 使用 `add_custom_command` 构建 Rust staticlib。
- 新增 imported target：

```text
ucm_ascend_trans_rs_static
```

- 将 Rust staticlib 链接进 Ascend `trans` target。
- 将 `Cargo.toml`、`Cargo.lock`、`.cargo/config.toml`、`src/lib.rs` 加入 CMake 依赖。

效果：

```text
cmake --build ... --target trans
```

会自动先构建 Rust crate，再链接 C++ Ascend trans。

### 6.2 `ucm/shared/trans/ascend/ascend_device.cc`

修改内容：

原来直接调用：

```cpp
aclrtSetDevice(deviceId)
```

现在改为：

```cpp
ucm_ascend_trans_set_device(deviceId)
```

`Device::Setup` 的对外行为不变，仍然返回 `UC::Status`。

### 6.3 `ucm/shared/trans/ascend/ascend_stream.cc`

修改内容：

以下 ACL 调用改为走 Rust wrapper：

- stream 创建：

```cpp
ucm_ascend_trans_create_stream(...)
```

- stream 销毁：

```cpp
ucm_ascend_trans_destroy_stream(...)
```

- callback report：

```cpp
ucm_ascend_trans_subscribe_report(...)
ucm_ascend_trans_unsubscribe_report(...)
ucm_ascend_trans_process_report(...)
ucm_ascend_trans_launch_callback(...)
```

- 同步 / 异步拷贝：

```cpp
ucm_ascend_trans_device_to_host(...)
ucm_ascend_trans_device_to_host_async(...)
ucm_ascend_trans_host_to_device(...)
ucm_ascend_trans_host_to_device_async(...)
```

- stream 同步：

```cpp
ucm_ascend_trans_synchronize_stream(...)
```

- event 等待：

```cpp
ucm_ascend_trans_stream_wait_event(...)
```

`AscendStream` 类和 `UC::Trans::Stream` 接口保持不变。

### 6.4 `ucm/shared/trans/ascend/ascend_buffer.cc`

修改内容：

以下 ACL 调用改为走 Rust wrapper：

- device memory 分配 / 释放：

```cpp
ucm_ascend_trans_malloc_device(...)
ucm_ascend_trans_free_device(...)
```

- host memory 分配 / 释放：

```cpp
ucm_ascend_trans_malloc_host(...)
ucm_ascend_trans_free_host(...)
```

- host register / unregister：

```cpp
ucm_ascend_trans_host_register(...)
ucm_ascend_trans_host_register_v2(...)
ucm_ascend_trans_host_get_device_pointer(...)
ucm_ascend_trans_host_unregister(...)
```

`AscendBuffer` 类和 `UC::Trans::Buffer` 接口保持不变。

### 6.5 `.gitignore`

新增：

```gitignore
**/target/**
```

原因：

- 忽略 Cargo 构建产物。
- 避免提交 `rs/target/`。

## 7. 保持不变的内容

本次 Ascend trans Rust 重构保持以下内容不变：

- `UC::Trans::Device` 对外接口。
- `UC::Trans::Stream` 对外接口。
- `UC::Trans::Buffer` 对外接口。
- Python `ucmtrans` 的绑定接口。
- CacheStore 调用 trans 的方式。
- `RUNTIME_ENVIRONMENT=ascend` 的 CMake runtime 选择方式。
- 其他 runtime，例如 `simu`、`cuda`、`musa`、`maca`。

## 8. 当前环境检查

当前环境存在 Ascend CANN 头文件和库：

```text
/usr/local/Ascend/cann-8.5.1/aarch64-linux/include/acl/acl.h
/usr/local/Ascend/cann-8.5.1/aarch64-linux/include/acl/acl_rt.h
/usr/local/Ascend/cann-8.5.1/aarch64-linux/lib64/libascendcl.so
```

`/usr/local/Ascend/ascend-toolkit/latest` 最终指向当前 CANN 安装路径，因此 CMake 默认 `ASCEND_ROOT` 可以解析到 Ascend toolkit。

但当前环境没有可用 NPU 设备节点。执行：

```bash
npu-smi info
```

返回：

```text
DrvMngGetConsoleLogLevel failed. (ret=4)
dcmi module initialize failed. ret is -8005
```

因此当前环境适合做构建、链接、符号和 import 验证，但无法完成真实 H2D / D2H 数据拷贝验证。

## 9. 验证结果

### 9.1 Rust 格式检查

执行：

```bash
cargo fmt --manifest-path ucm/shared/trans/ascend/rs/Cargo.toml --check
```

结果：通过。

### 9.2 Rust release 构建

执行：

```bash
cargo build --manifest-path ucm/shared/trans/ascend/rs/Cargo.toml --release
```

结果：通过。

### 9.3 Ascend CMake 配置

执行：

```bash
cmake -S . -B /tmp/ucm-ascend-trans-rust-build \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_UNIT_TESTS=OFF
```

结果：通过。

说明：

- CMake 进入 `ucm/shared/trans/ascend`。
- Rust Ascend trans crate 纳入构建图。
- 缺少 HF3FS 依赖导致 `ds3fsstore` 被跳过，这是既有 optional 依赖行为，与 Ascend trans 无关。

### 9.4 构建 Ascend trans

执行：

```bash
cmake --build /tmp/ucm-ascend-trans-rust-build --target trans -- -j8
```

结果：通过。

构建中可见：

```text
Building Rust Ascend trans core
Built target ucm_ascend_trans_rs
Built target trans
```

### 9.5 构建 Python `ucmtrans`

执行：

```bash
cmake --build /tmp/ucm-ascend-trans-rust-build --target ucmtrans -- -j8
```

结果：通过。

产物：

```text
ucmtrans.cpython-311-aarch64-linux-gnu.so
```

### 9.6 Python import 验证

执行：

```bash
PYTHONPATH=/tmp/ucm-ascend-trans-rust-build/ucm/shared/trans \
python -c "import ucmtrans; print(ucmtrans.project, ucmtrans.build_type)"
```

结果：

- `ucmtrans` import 成功。
- Ascend runtime 输出了 driver/platform 相关日志。
- Python 进程正常退出。

### 9.7 Device setup 调用链验证

执行：

```bash
PYTHONPATH=/tmp/ucm-ascend-trans-rust-build/ucm/shared/trans python - <<'PY'
import ucmtrans
try:
    d = ucmtrans.Device()
    d.Setup(0)
    print('device setup ok')
except Exception as e:
    print('device setup failed:', e)
PY
```

结果：

```text
device setup failed: 507899, 507899
```

意义：

- Python 调到了 `ucmtrans.Device().Setup(0)`。
- C++ `Device::Setup` 调到了 Rust wrapper。
- Rust wrapper 调到了 Ascend ACL `aclrtSetDevice`。
- 最终失败来自当前环境 driver / device 不可用，而不是链接或符号缺失。

### 9.8 符号检查

对构建出的 `libcachestore.so` 执行符号检查：

```bash
nm -D /tmp/ucm-ascend-trans-rust-build/ucm/store/cache/libcachestore.so \
  | rg 'ucm_ascend_trans'
```

确认以下 Rust FFI 符号已链接进最终依赖链：

```text
ucm_ascend_trans_create_stream
ucm_ascend_trans_destroy_stream
ucm_ascend_trans_device_to_host
ucm_ascend_trans_device_to_host_async
ucm_ascend_trans_free_device
ucm_ascend_trans_free_host
ucm_ascend_trans_host_get_device_pointer
ucm_ascend_trans_host_register
ucm_ascend_trans_host_register_v2
ucm_ascend_trans_host_to_device
ucm_ascend_trans_host_to_device_async
ucm_ascend_trans_host_unregister
ucm_ascend_trans_launch_callback
ucm_ascend_trans_malloc_device
ucm_ascend_trans_malloc_host
ucm_ascend_trans_process_report
ucm_ascend_trans_set_device
ucm_ascend_trans_stream_wait_event
ucm_ascend_trans_subscribe_report
ucm_ascend_trans_synchronize_stream
ucm_ascend_trans_unsubscribe_report
```

### 9.9 动态库依赖检查

执行：

```bash
ldd /tmp/ucm-ascend-trans-rust-build/ucm/store/cache/libcachestore.so \
  | rg 'ascend|not found|libascendcl'
```

结果：

- `libascendcl.so` 能解析到 Ascend toolkit 路径。
- 未观察到 `not found`。

## 10. 当前变更文件清单

### 修改文件

```text
.gitignore
ucm/shared/trans/ascend/CMakeLists.txt
ucm/shared/trans/ascend/ascend_buffer.cc
ucm/shared/trans/ascend/ascend_device.cc
ucm/shared/trans/ascend/ascend_stream.cc
```

### 新增文件

```text
ucm/shared/trans/ascend/ascend_rs.h
ucm/shared/trans/ascend/rs/Cargo.toml
ucm/shared/trans/ascend/rs/Cargo.lock
ucm/shared/trans/ascend/rs/.cargo/config.toml
ucm/shared/trans/ascend/rs/src/lib.rs
```

## 11. 结论

Ascend trans 的 Rust 化试点已经完成到“可构建、可链接、可 import、可进入 Rust wrapper 调用链”的阶段。

当前实现的性质是：

- C++ `UC::Trans` 对外接口保持不变。
- C++ Ascend trans 类保持不变。
- Rust 只替换 AscendCL ACL 调用薄层。
- 其他 runtime 不受影响。
- 当前环境可以验证构建和调用链，但无法验证真实 NPU 数据拷贝。

如果后续切换到可用 Ascend 设备环境，建议继续验证：

1. `ucmshared.test` 中 trans 相关用例。
2. Python `ucmtrans` 的 H2D / D2H copy。
3. CacheStore 在 `RUNTIME_ENVIRONMENT=ascend` 下的 Load / Dump。
4. PipelineStore 最小端到端路径。

