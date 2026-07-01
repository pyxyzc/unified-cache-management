# aclrtHostRegisterV2 Tests

这个目录是独立于 `unified-cache-management` 的小测试工程，用来观察
`aclrtHostRegisterV2` 对同一个 host pointer 的重复注册行为。

默认行为参考项目里的 FFTS/Ascend 调用方式：

- host buffer 使用 `posix_memalign(..., 4096, size)` 申请，保证 4K 对齐。
- 默认 flag 是 `ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED`。
- 注册成功后，如果包含 `ACL_HOST_REG_MAPPED`，会顺手调用一次
  `aclrtHostGetDevicePointer` 打印映射后的 device pointer。
- 清理阶段调用 `aclrtHostUnregister`，如果第二次注册也成功，会按成功次数做
  best-effort unregister，并把每次返回值打印出来。

## 测试内容

1. 同一个进程内，`aclrtSetDevice(0)` 后注册一个 host pointer，然后
   `aclrtSetDevice(1)`，再注册同一个 host pointer。
   - `case1a`：第一次注册后不 unregister，直接切 device 后第二次注册。
   - `case1b`：第一次注册后先 unregister，再切 device 后第二次注册。
2. 同一个进程内，`aclrtSetDevice(0)` 后对同一个 host pointer 连续调用两次
   `aclrtHostRegisterV2`。
   - `case2a`：第一次注册后不 unregister，直接第二次注册。
   - `case2b`：第一次注册后先 unregister，再第二次注册。

如果机器上少于 2 张卡，第 1 个测试会自动跳过，第 2 个测试仍然执行。

## 编译

```bash
cd /path/to/aclrt_host_register_v2_tests
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j
```

如果环境变量里已经有 `ASCEND_ROOT`、`ASCEND_HOME`、`ASCEND_TOOLKIT_HOME` 或
`ASCEND_INSTALL_PATH`，通常可以省略 `-DASCEND_ROOT=...`。

## 运行

```bash
./build/host_register_v2_probe
```

可选参数：

```bash
./build/host_register_v2_probe --device0 0 --device1 1 --size 4096 --flags 0x10000002
```

`--flags 0x10000002` 等价于默认的
`ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED`。

官方接口参考：
https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_2128.html
