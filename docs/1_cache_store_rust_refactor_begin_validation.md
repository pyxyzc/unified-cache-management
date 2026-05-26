# UCM CacheStore Rust CMake Integration And Unit Tests

本文档说明两件事：

- `ucm/store/cache/rs` 中的 Rust 版 `ucm-cache-store` 如何嵌入到整个 CMake 工程。
- 如何构建并运行 UCM 自带的 C++/GoogleTest 单测，验证 Rust CacheStore 改造后的正确性。

## 1. 工程入口

项目根目录的 `CMakeLists.txt` 是总入口，关键配置如下：

```cmake
option(BUILD_UCM_STORE "build ucm store module." ON)
option(BUILD_UNIT_TESTS "build all unit test suits." OFF)
set(RUNTIME_ENVIRONMENT "simu" CACHE STRING "runtime: simu, ascend, musa or cuda.")

if(BUILD_UNIT_TESTS)
    enable_testing()
endif()

add_subdirectory(ucm)
if(BUILD_UNIT_TESTS)
    add_subdirectory(test)
endif()
```

含义：

- `BUILD_UCM_STORE=ON`：默认构建 UCM store 模块。
- `BUILD_UNIT_TESTS=ON`：启用 CTest，并注册 GoogleTest 单测。
- `RUNTIME_ENVIRONMENT=simu`：使用模拟设备后端，适合本地验证，不依赖 CUDA/Ascend/MUSA。

## 2. Rust CacheStore 如何嵌入 CMake

Rust 版 CacheStore 位于：

```text
ucm/store/cache/rs/
```

C++ 封装和 FFI 头文件位于：

```text
ucm/store/cache/cc/
```

核心嵌入逻辑在：

```text
ucm/store/cache/CMakeLists.txt
```

### 2.1 查找 cargo

```cmake
find_program(CARGO_EXECUTABLE cargo REQUIRED)
```

如果环境中没有 `cargo`，CMake 配置阶段会直接失败。因此运行单测前需要确保 Rust 工具链可用：

```bash
cargo --version
rustc --version
```

### 2.2 设置 Rust crate 和 target 目录

```cmake
set(UCM_CACHE_STORE_RS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rs)
set(UCM_CACHE_STORE_RS_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/rust-target)
```

这里将 Rust 的 `CARGO_TARGET_DIR` 指向 CMake build 目录下的 `rust-target`，避免污染源码目录，也便于 CMake 管理构建产物。

### 2.3 根据 CMake build type 选择 Rust profile

```cmake
if(CMAKE_BUILD_TYPE_LOWER STREQUAL "debug")
    set(UCM_CACHE_STORE_RS_PROFILE debug)
    set(UCM_CACHE_STORE_RS_BUILD_ARGS build --manifest-path ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml)
else()
    set(UCM_CACHE_STORE_RS_PROFILE release)
    set(UCM_CACHE_STORE_RS_BUILD_ARGS build --manifest-path ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml --release)
endif()
```

对应关系：

| CMake build type | Cargo 命令 | Rust 产物目录 |
| --- | --- | --- |
| `Debug` | `cargo build` | `rust-target/debug` |
| 非 `Debug`，例如 `Release` | `cargo build --release` | `rust-target/release` |

### 2.4 生成 Rust 静态库

CMake 期望 Rust crate 输出静态库：

```cmake
set(UCM_CACHE_STORE_RS_LIB
    ${UCM_CACHE_STORE_RS_TARGET_DIR}/${UCM_CACHE_STORE_RS_PROFILE}/libucm_cache_store_rs.a)
```

对应 Rust crate 需要在 `Cargo.toml` 中配置为可生成 staticlib，例如：

```toml
[lib]
crate-type = ["staticlib"]
```

### 2.5 用 add_custom_command 调 cargo

```cmake
add_custom_command(
    OUTPUT ${UCM_CACHE_STORE_RS_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${UCM_CACHE_STORE_RS_TARGET_DIR}
            ${CARGO_EXECUTABLE} ${UCM_CACHE_STORE_RS_BUILD_ARGS}
    WORKING_DIRECTORY ${UCM_CACHE_STORE_RS_DIR}
    DEPENDS
        ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml
        ${UCM_CACHE_STORE_RS_DIR}/.cargo/config.toml
        ${UCM_CACHE_STORE_RS_DIR}/src/lib.rs
    COMMENT "Building Rust CacheStore core"
    VERBATIM)
```

构建 `cachestore` 时，如果 Rust 输入文件有变化，CMake 会重新触发 cargo 构建。构建日志中出现下面这行，说明 Rust core 已进入 CMake 构建链路：

```text
Building Rust CacheStore core
```

### 2.6 将 Rust 静态库导入 CMake target

```cmake
add_custom_target(ucm_cache_store_rs DEPENDS ${UCM_CACHE_STORE_RS_LIB})
add_library(ucm_cache_store_rs_static STATIC IMPORTED GLOBAL)
set_target_properties(ucm_cache_store_rs_static PROPERTIES
    IMPORTED_LOCATION ${UCM_CACHE_STORE_RS_LIB})
add_dependencies(ucm_cache_store_rs_static ucm_cache_store_rs)
```

这里分两步：

- `ucm_cache_store_rs`：负责触发 cargo 构建。
- `ucm_cache_store_rs_static`：把 cargo 生成的 `libucm_cache_store_rs.a` 包装成 CMake 可链接的 imported target。

### 2.7 链接进 C++ cachestore

```cmake
file(GLOB_RECURSE UCM_CACHE_STORE_CC_SOURCE_FILES "./cc/*.cc")
add_library(cachestore SHARED ${UCM_CACHE_STORE_CC_SOURCE_FILES})
target_include_directories(cachestore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cc)
target_link_libraries(cachestore PUBLIC storeintf trans infra_logger ucm_cache_store_rs_static pthread dl m)
```

最终依赖关系是：

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

因此，只要 `ucmstore.test` 链接并运行到 `cachestore`，就会覆盖 Rust CacheStore 通过 C++ FFI 暴露出来的行为。

## 3. UCM 单测目标

当前 CMake 工程中主要有两个 C++ 单测目标：

| Target | 路径 | 覆盖范围 |
| --- | --- | --- |
| `ucmstore.test` | `ucm/store/test` | store、cache、posix、fake、pcstore 等 |
| `ucmshared.test` | `ucm/shared/test` | shared infra、metrics、trans 等 |

`ucmstore.test` 的链接关系：

```cmake
target_link_libraries(ucmstore.test PRIVATE
    posixstore cachestore fakestore pcstore
    gtest_main gtest gmock
)
```

其中 `cachestore` 已经链接了 Rust 静态库，所以 `ucmstore.test` 是验证 Rust CacheStore 嵌入效果的主要单测入口。

## 4. 从零构建并运行单测

以下命令均在项目根目录执行：

```bash
cd /home/yp/unified-cache-management-rust/unified-cache-management
```

### 4.1 配置 CMake

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=simu \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
```

参数说明：

| 参数 | 作用 |
| --- | --- |
| `-S .` | 源码目录为当前目录 |
| `-B build` | 构建目录为 `build` |
| `-DCMAKE_BUILD_TYPE=Release` | 使用 Release 构建，同时 Rust 使用 `cargo build --release` |
| `-DBUILD_UNIT_TESTS=ON` | 打开 UCM C++ 单测 |
| `-DRUNTIME_ENVIRONMENT=simu` | 使用模拟设备后端 |
| `-DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result` | 避免现有测试代码中 `system()` 返回值未使用导致 `-Werror` 编译失败 |

说明：如果后续修复了测试代码中 `system()` 返回值未使用的问题，可以去掉 `-DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result`。

### 4.2 构建单测 target

```bash
cmake --build build --target ucmstore.test ucmshared.test --parallel 8
```

期望看到类似日志：

```text
Building Rust CacheStore core
Compiling ucm_cache_store_rs ...
Built target cachestore
Built target ucmstore.test
Built target ucmshared.test
```

如果只关心 store/cache 相关验证，也可以只构建：

```bash
cmake --build build --target ucmstore.test --parallel 8
```

### 4.3 执行全部 CTest 单测

```bash
ctest --test-dir build --output-on-failure
```

成功时会看到类似输出：

```text
100% tests passed, 0 tests failed out of 75
```

`--output-on-failure` 会在失败时打印具体失败用例日志，便于定位问题。

## 5. 常用过滤执行方式

### 5.1 只跑 cache 相关测试

```bash
ctest --test-dir build -R 'Cache' --output-on-failure
```

### 5.2 只跑 store/posix/pcstore 相关测试

```bash
ctest --test-dir build -R 'Store|Posix|PCStore' --output-on-failure
```

### 5.3 只跑某一个用例

示例：

```bash
ctest --test-dir build -R 'UCCacheTransManagerTest.DumpThenLoad' --output-on-failure
```

### 5.4 列出全部已注册用例

```bash
ctest --test-dir build -N
```

## 6. 单独运行 GoogleTest 可执行文件

CTest 是推荐入口。如果需要直接调试某个测试二进制，也可以运行：

```bash
./build/ucm/store/test/ucmstore.test
./build/ucm/shared/test/ucmshared.test
```

使用 GoogleTest 过滤：

```bash
./build/ucm/store/test/ucmstore.test --gtest_filter='UCCache*'
```

## 7. 清理和重新构建

### 7.1 增量重新构建

修改 Rust 或 C++ 代码后，一般直接重新执行：

```bash
cmake --build build --target ucmstore.test --parallel 8
ctest --test-dir build -R 'Cache' --output-on-failure
```

### 7.2 完整清理后重来

```bash
rm -rf build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=simu \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
cmake --build build --target ucmstore.test ucmshared.test --parallel 8
ctest --test-dir build --output-on-failure
```

注意：`rm -rf build` 会删除本地构建目录，只在确认不需要保留当前 build 产物时执行。

## 8. 常见问题

### 8.1 CMake 配置时报 cargo not found

原因：系统环境中没有 Rust 工具链，或 `cargo` 不在 `PATH`。

检查：

```bash
cargo --version
which cargo
```

### 8.2 构建时报 system() unused-result

现象：

```text
error: ignoring return value of 'int system(const char*)' declared with attribute 'warn_unused_result' [-Werror=unused-result]
```

原因：项目全局开启了 `-Werror`，部分已有测试代码调用 `system()` 后没有处理返回值。

临时处理方式是在 CMake 配置中加入：

```bash
-DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
```

长期处理方式是在测试代码中显式检查 `system()` 返回值。

### 8.3 ds3fsstore 被跳过

配置阶段可能看到：

```text
ds3fsstore: Skipping build - required HF3FS dependencies not found
```

这是因为当前环境缺少 HF3FS 相关头文件或库。该模块被跳过不影响 `ucmstore.test` 和 `ucmshared.test` 的基础单测验证。

### 8.4 依赖下载失败

CMake 默认 `DOWNLOAD_DEPENDENCE=ON`，会通过 `FetchContent` 下载：

- fmt
- spdlog
- pybind11
- zlib
- googletest

如果网络不可用，配置阶段会失败。可以切换到有网络的环境，或提前准备这些依赖并按项目现有 vendor 逻辑接入。

## 9. 本次验证结果

在当前环境中执行：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=simu \
  -DCMAKE_CXX_FLAGS_RELEASE=-Wno-error=unused-result
cmake --build build --target ucmstore.test ucmshared.test --parallel 8
ctest --test-dir build --output-on-failure
```

验证结果：

```text
100% tests passed, 0 tests failed out of 75
Total Test time (real) = 21.83 sec
```
