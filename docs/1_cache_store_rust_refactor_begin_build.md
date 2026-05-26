# UCM CacheStore Rust Ascend Build Notes

本文档记录在 Ascend 环境下构建 UCM 单测时的关键日志，以及这些日志对应的含义。重点说明：

- 如何确认当前构建走的是 Ascend 后端。
- 如何确认 Rust 版 `ucm-cache-store` 已经进入 CMake 构建链路。
- 为什么增量构建时可能看不到 `cargo build` 的完整输出。
- 常见命令输入问题。

## 1. 正确的构建命令

在项目根目录执行：

```bash
cmake --build build --target ucmstore.test ucmshared.test --parallel 8
```

注意：这里的 `--build`、`--target`、`--parallel` 必须使用英文半角 `--`。

错误示例：

```bash
cmake ——build build --target ucmstore.test ucmshared.test --parallel 8
```

上面命令里的 `——build` 使用了中文/全角破折号，CMake 不会把它识别成 `--build` 子命令，因此会报错。

对应错误日志：

```text
CMake Warning:
  Ignoring extra path from command line:

   "8"

CMake Error: Unknown argument --parallel
CMake Error: Run 'cmake --help' for all supported options.
```

另一个错误示例：

```bash
cmake ——build build --target ucmstore.test ucmshared.test
```

对应错误日志：

```text
CMake Warning:
  Ignoring extra path from command line:

   "ucmshared.test"

CMake Error: Unknown argument --target
CMake Error: Run 'cmake --help' for all supported options.
```

原因相同：`——build` 不是合法的 CMake 参数。

## 2. Ascend 后端确认

你的构建日志中出现了：

```text
[ 45%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_buffer.cc.o
[ 45%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_device.cc.o
[ 47%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_stream.cc.o
```

这说明当前 `build` 目录对应的 CMake 配置使用的是 Ascend 后端。

CMake 中的选择逻辑位于：

```text
ucm/shared/trans/CMakeLists.txt
```

关键逻辑：

```cmake
if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    add_subdirectory(ascend)
endif()
```

Ascend 后端实现位于：

```text
ucm/shared/trans/ascend/
```

其 CMake target 会链接 Ascend runtime：

```cmake
set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")

add_library(Ascend::ascendcl UNKNOWN IMPORTED)
set_target_properties(Ascend::ascendcl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ASCEND_ROOT}/include"
    IMPORTED_LOCATION "${ASCEND_ROOT}/lib64/libascendcl.so"
)

target_link_libraries(trans PUBLIC
    fmt
    Ascend::ascendcl
)
```

因此，只要日志中出现 `ucm/shared/trans/ascend/...`，就可以确认本次构建不是 `simu`，而是 Ascend 版本。

如果需要从空 build 目录显式配置 Ascend，可以使用：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
```

如果 Ascend toolkit 不在默认路径，可以额外指定：

```bash
-DASCEND_ROOT=/path/to/Ascend/ascend-toolkit/latest
```

## 3. Rust CacheStore 构建链路确认

你的日志中出现了：

```text
[  1%] Built target ucm_cache_store_rs
```

这行就是 Rust 版 CacheStore 进入 CMake 构建链路的证据。

对应 CMake target 定义在：

```text
ucm/store/cache/CMakeLists.txt
```

关键逻辑：

```cmake
add_custom_target(ucm_cache_store_rs DEPENDS ${UCM_CACHE_STORE_RS_LIB})
add_library(ucm_cache_store_rs_static STATIC IMPORTED GLOBAL)
set_target_properties(ucm_cache_store_rs_static PROPERTIES
    IMPORTED_LOCATION ${UCM_CACHE_STORE_RS_LIB})
add_dependencies(ucm_cache_store_rs_static ucm_cache_store_rs)
```

完整依赖链路是：

```text
ucm/store/cache/rs/src/lib.rs
        |
        v
cargo build -> libucm_cache_store_rs.a
        |
        v
ucm_cache_store_rs_static
        |
        v
libcachestore.so
        |
        v
ucmstore.test
```

你的日志后续也出现了：

```text
[ 81%] Linking CXX shared library libcachestore.so
[ 81%] Built target cachestore
```

这说明 `cachestore` 已经完成链接。由于 `cachestore` 链接了 `ucm_cache_store_rs_static`，所以 Rust 静态库已经被包含进 `libcachestore.so` 的构建链路。

后续日志：

```text
[100%] Linking CXX executable ucmstore.test
[100%] Built target ucmstore.test
```

说明 `ucmstore.test` 已经链接完成。由于 `ucmstore.test` 链接 `cachestore`，因此 store/cache 相关单测会覆盖 Rust CacheStore 通过 C++ FFI 暴露出来的行为。

## 4. 为什么没看到 cargo build 输出

首次构建或 Rust 输入文件变化后，通常会看到类似：

```text
[  3%] Building Rust CacheStore core
   Compiling ucm_cache_store_rs ...
    Finished `release` profile [optimized] target(s) in ...
```

你这次看到的是：

```text
[  1%] Built target ucm_cache_store_rs
```

这表示 CMake 判断 Rust 产物已经是最新的，因此没有重新调用 `cargo build`。这是正常的增量构建行为，不代表 Rust 版 CacheStore 没有参与构建。

判断标准是：

- 看到 `Building Rust CacheStore core`：本次构建重新执行了 cargo。
- 看到 `Built target ucm_cache_store_rs`：Rust target 已存在且被 CMake 检查过，本次不需要重建。

如果想强制观察 cargo 重新执行，可以让 Rust 输入文件时间戳变化后再构建：

```bash
touch ucm/store/cache/rs/src/lib.rs
cmake --build build --target ucmstore.test --parallel 8
```

此时通常会重新出现：

```text
Building Rust CacheStore core
Compiling ucm_cache_store_rs ...
```

## 5. 本次构建关键日志摘录

### 5.1 Rust target 已进入构建链路

```text
[  1%] Built target ucm_cache_store_rs
```

解释：Rust CacheStore 对应的 CMake target 已被构建系统处理。这里是增量构建，所以显示 `Built target`，没有重新打印 cargo 编译过程。

### 5.2 Ascend trans 后端正在构建

```text
[ 45%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_buffer.cc.o
[ 45%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_device.cc.o
[ 47%] Building CXX object ucm/shared/trans/ascend/CMakeFiles/trans.dir/ascend_stream.cc.o
[ 62%] Linking CXX static library libtrans.a
[ 62%] Built target trans
```

解释：`RUNTIME_ENVIRONMENT=ascend` 生效，`trans` target 使用 Ascend 实现，并链接成 `libtrans.a`。

### 5.3 C++ cachestore 链接完成

```text
[ 62%] Building CXX object ucm/store/cache/CMakeFiles/cachestore.dir/cc/cache_store.cc.o
[ 66%] Building CXX object ucm/store/cache/CMakeFiles/cachestore.dir/cc/dump_queue.cc.o
[ 66%] Building CXX object ucm/store/cache/CMakeFiles/cachestore.dir/cc/load_queue.cc.o
[ 67%] Building CXX object ucm/store/cache/CMakeFiles/cachestore.dir/cc/trans_buffer.cc.o
[ 81%] Linking CXX shared library libcachestore.so
[ 81%] Built target cachestore
```

解释：`cachestore` 的 C++ 封装代码被编译，并最终链接为 `libcachestore.so`。该 target 链接了 Rust 静态库 `ucm_cache_store_rs_static`。

### 5.4 store 单测链接完成

```text
[ 83%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_buffer_manager_test.cc.o
[ 84%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_load_queue_test.cc.o
[ 84%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_dump_queue_test.cc.o
[ 86%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_posix_shm_test.cc.o
[ 90%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_trans_buffer_test.cc.o
[ 90%] Building CXX object ucm/store/test/CMakeFiles/ucmstore.test.dir/case/cache/cache_trans_manager_test.cc.o
[100%] Linking CXX executable ucmstore.test
[100%] Built target ucmstore.test
```

解释：`ucmstore.test` 包含 cache/store 相关 GoogleTest 用例，并链接 `cachestore`。这是验证 Rust CacheStore 嵌入后行为的核心单测可执行文件。

### 5.5 shared 单测链接完成

```text
[100%] Linking CXX executable ucmshared.test
[100%] Built target ucmshared.test
```

解释：`ucmshared.test` 构建完成，用于验证 shared infra、metrics、trans 等基础模块。

## 6. 下一步运行单测

构建完成后，运行：

```bash
ctest --test-dir build --output-on-failure
```

如果只想优先验证 cache/store 相关用例：

```bash
ctest --test-dir build -R 'Cache|Store|Posix|PCStore' --output-on-failure
```

如果只想看已注册的测试列表：

```bash
ctest --test-dir build -N
```

## 7. 结论

根据本次日志，可以确认：

- 当前构建使用的是 Ascend 后端，因为日志中出现了 `ucm/shared/trans/ascend/...`。
- Rust 版 CacheStore 已进入 CMake 构建链路，因为日志中出现了 `Built target ucm_cache_store_rs`。
- 本次没有看到完整 `cargo build` 输出，是因为 Rust 静态库已经是最新产物，CMake 执行了增量构建。
- `libcachestore.so` 和 `ucmstore.test` 已经成功链接，后续可以通过 `ctest` 执行单测验证。
