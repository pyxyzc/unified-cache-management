# NPU 环境下验证 trans/Ascend 模块

本文档总结如何在真实 NPU 环境下验证 `ucm/shared/trans/ascend` 模块的 Rust 重构结果，以及当前环境已完成的验证情况。

## 验证目标

- 确认 Ascend trans 模块可以在 `RUNTIME_ENVIRONMENT=ascend` 下正常构建。
- 确认 C++ trans 调用链可以进入 Rust ACL FFI 包装层。
- 在有可用 NPU 的机器上执行真实 H2D/D2H 数据拷贝，验证 `aclrtMemcpy`、stream、event、callback 等路径。
- 尽量不改变 UCM 上下游调用关系，仅验证 trans 模块替换后的行为。

## NPU 环境前置检查

在 NPU 机器上先确认驱动、设备节点和 CANN 环境可用：

```bash
npu-smi info
ls -l /dev/davinci* /dev/ascend*
source /usr/local/Ascend/ascend-toolkit/set_env.sh
```

如果 `npu-smi info` 失败，或者设备节点不可见，后续真实 ACL 执行通常会失败。

## 构建命令

建议单独使用一个构建目录：

```bash
cmake -S . -B /tmp/ucm-npu-trans-verify \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_UNIT_TESTS=ON

cmake --build /tmp/ucm-npu-trans-verify --target trans -- -j8
cmake --build /tmp/ucm-npu-trans-verify --target ucmtrans -- -j8
cmake --build /tmp/ucm-npu-trans-verify --target ucmshared.test -- -j8
```

## 单测验证

优先跑真实数据拷贝用例：

```bash
/tmp/ucm-npu-trans-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.CopyDataWithCE
```

如果该用例通过，说明至少完成了设备初始化、stream 创建、host/device buffer 分配、H2D/D2H 拷贝和同步等关键路径验证。

如需扩大范围，可以运行全部 trans 相关用例：

```bash
/tmp/ucm-npu-trans-verify/ucm/shared/test/ucmshared.test \
  --gtest_filter=UCTransUnitTest.*
```

## Python 冒烟验证

可以用 `ucmtrans` Python 扩展验证模块加载、设备初始化和 stream 创建：

```bash
PYTHONPATH=/tmp/ucm-npu-trans-verify/ucm/shared/trans python - <<'PY'
import ucmtrans

d = ucmtrans.Device()
d.Setup(0)
s = d.MakeStream()
s.Synchronized()

print("ucmtrans ascend setup ok")
PY
```

这个测试不等价于完整数据拷贝测试，但可以快速确认 Python 扩展、Ascend runtime 和 trans 基础调用链可用。

## 符号检查

构建后可以检查 Rust FFI 符号是否进入最终产物：

```bash
nm -D /tmp/ucm-npu-trans-verify/ucm/shared/trans/ucmtrans*.so | grep ucm_ascend_trans
```

也可以检查动态库依赖是否能解析到 Ascend ACL：

```bash
ldd /tmp/ucm-npu-trans-verify/ucm/shared/trans/ucmtrans*.so | grep -E 'ascend|not found|libascendcl'
```

预期不应出现 `not found`。

## 当前环境已完成验证

当前开发环境没有可用 NPU 设备，因此只能完成构建、链接、导入和失败路径验证。

已通过的验证：

- `cargo fmt --manifest-path ucm/shared/trans/ascend/rs/Cargo.toml --check`
- `cargo build --manifest-path ucm/shared/trans/ascend/rs/Cargo.toml --release`
- `cmake -S . -B /tmp/ucm-ascend-trans-rust-build -DRUNTIME_ENVIRONMENT=ascend -DBUILD_UNIT_TESTS=OFF`
- `cmake --build /tmp/ucm-ascend-trans-rust-build --target trans -- -j8`
- `cmake --build /tmp/ucm-ascend-trans-rust-build --target cachestore -- -j8`
- `cmake --build /tmp/ucm-ascend-trans-rust-build --target ucmtrans -- -j8`
- `nm -D` 确认最终动态库中存在 `ucm_ascend_trans_*` 相关符号。
- `ldd` 确认 `libascendcl.so` 可解析，未发现 `not found`。
- `PYTHONPATH=/tmp/ucm-ascend-trans-rust-build/ucm/shared/trans python -c "import ucmtrans; print(ucmtrans.project, ucmtrans.build_type)"` 可以成功导入。

当前环境未通过真实 NPU 执行：

- `npu-smi info` 失败，报错包含 `DrvMngGetConsoleLogLevel failed. (ret=4)` 和 `dcmi module initialize failed. ret is -8005`。
- Python 中执行 `ucmtrans.Device().Setup(0)` 可以进入 ACL 调用链，但 ACL 返回 `507899`，原因是当前环境没有可用 NPU 驱动或设备。

因此，当前环境已经证明构建、链接、Python 导入和 Rust FFI 调用链成立；真实的数据搬运验证仍需要在可用 NPU 机器上执行 `UCTransUnitTest.CopyDataWithCE`。

## 通过标准

在 NPU 机器上建议以以下结果作为 trans/Ascend 重构验证通过标准：

- `npu-smi info` 正常显示设备。
- `trans`、`ucmtrans`、`ucmshared.test` 构建成功。
- `UCTransUnitTest.CopyDataWithCE` 通过。
- `UCTransUnitTest.*` 中 trans 相关用例没有新增失败。
- Python 冒烟测试可以完成 `Device.Setup(0)`、`MakeStream()` 和 `Synchronized()`。
- `ldd` 不出现 `not found`，`nm` 可以看到 `ucm_ascend_trans_*` 符号。
