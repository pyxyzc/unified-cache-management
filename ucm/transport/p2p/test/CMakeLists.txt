cmake_minimum_required(VERSION 3.18)
project(p2p_ffts_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(P2P_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(REPO_ROOT "${P2P_ROOT}/../../.." ABSOLUTE)
set(UCM_ROOT "${REPO_ROOT}/ucm")
list(APPEND CMAKE_MODULE_PATH "${UCM_ROOT}/shared/vendor")

option(DOWNLOAD_DEPENDENCE "download dependence by cmake." ON)

if(NOT DEFINED ASCEND_ROOT)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Path to Ascend root directory")
    elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME})
        set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}" CACHE PATH "Path to Ascend root directory")
    else()
        set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH
            "Path to Ascend root directory")
    endif()
endif()

set(ASCEND_ARCH_DIR "${ASCEND_ROOT}/aarch64-linux" CACHE PATH
    "Path to Ascend architecture-specific directory")
set(ASCEND_PKG_INCLUDE_DIR "${ASCEND_ARCH_DIR}/pkg_inc" CACHE PATH
    "Path to Ascend package include directory")
set(ASCEND_FFTS_INCLUDE_DIR "${ASCEND_ARCH_DIR}/pkg_inc/runtime" CACHE PATH
    "Path to Ascend FFTS include directory")

find_path(ASCEND_ACL_INCLUDE_DIR
    NAMES acl/acl.h
    HINTS
        "${ASCEND_ROOT}/include"
        "${ASCEND_ARCH_DIR}/include"
    NO_DEFAULT_PATH
)
if(NOT ASCEND_ACL_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find acl/acl.h under ASCEND_ROOT=${ASCEND_ROOT}")
endif()

if(NOT EXISTS "${ASCEND_PKG_INCLUDE_DIR}/runtime/rt_ffts_plus.h"
    AND NOT EXISTS "${ASCEND_FFTS_INCLUDE_DIR}/runtime/rt_ffts_plus.h"
    AND NOT EXISTS "${ASCEND_FFTS_INCLUDE_DIR}/rt_external_ffts.h")
    message(FATAL_ERROR
        "Cannot find FFTS headers under ASCEND_ARCH_DIR=${ASCEND_ARCH_DIR}")
endif()

find_library(ASCENDCL_LIBRARY
    NAMES ascendcl
    HINTS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ARCH_DIR}/lib64"
        "${ASCEND_ARCH_DIR}/devlib"
    NO_DEFAULT_PATH
)
if(NOT ASCENDCL_LIBRARY)
    message(FATAL_ERROR "Cannot find libascendcl under ASCEND_ROOT=${ASCEND_ROOT}")
endif()

find_library(ASCEND_RUNTIME_LIBRARY
    NAMES runtime
    HINTS
        "${ASCEND_ARCH_DIR}/lib64"
        "${ASCEND_ROOT}/lib64"
    NO_DEFAULT_PATH
)
if(NOT ASCEND_RUNTIME_LIBRARY)
    message(FATAL_ERROR "Cannot find libruntime under ASCEND_ROOT=${ASCEND_ROOT}")
endif()

find_package(Threads REQUIRED)
include("${UCM_ROOT}/shared/vendor/dep-fmt.cmake")
include("${UCM_ROOT}/shared/vendor/dep-spdlog.cmake")
include("${UCM_ROOT}/shared/vendor/dep-zlib.cmake")
include("${UCM_ROOT}/shared/vendor/dep-googletest.cmake")

add_library(infra_logger STATIC
    "${UCM_ROOT}/shared/infra/logger/logger.cc"
    "${UCM_ROOT}/shared/infra/logger/cc/spdlog_logger.cc"
)
target_include_directories(infra_logger PUBLIC
    "${UCM_ROOT}/shared/infra"
    "${UCM_ROOT}/shared/infra/logger"
    "${UCM_ROOT}/shared/infra/logger/cc"
)
target_link_libraries(infra_logger PUBLIC
    fmt
    spdlog
    zlibstatic
)

add_library(p2p_ffts_under_test STATIC
    "${P2P_ROOT}/src/one_sided/core/ffts_dispatcher.cpp"
    "${P2P_ROOT}/src/one_sided/core/ffts_engine.cpp"
    "${P2P_ROOT}/src/one_sided/core/ffts_transport.cpp"
    "${P2P_ROOT}/src/one_sided/core/transport_common.cpp"
)
target_compile_definitions(p2p_ffts_under_test PRIVATE
    TRANSPORT_P2P_ENABLE_FFTS=1
    TRANSPORT_P2P_ENABLE_FFTS_TESTING=1
)
target_include_directories(p2p_ffts_under_test
    PUBLIC
        "${P2P_ROOT}/include/one_sided"
    PRIVATE
        "${P2P_ROOT}/src/one_sided/core"
        "${UCM_ROOT}/shared/infra"
        "${ASCEND_ACL_INCLUDE_DIR}"
        "${ASCEND_PKG_INCLUDE_DIR}"
        "${ASCEND_FFTS_INCLUDE_DIR}"
)
target_link_libraries(p2p_ffts_under_test
    PRIVATE
        "${ASCENDCL_LIBRARY}"
        "${ASCEND_RUNTIME_LIBRARY}"
        infra_logger
        Threads::Threads
)

add_executable(p2p_ffts.test
    ffts_dispatcher_test.cpp
    ffts_engine_copy_test.cpp
    ffts_engine_test.cpp
    ffts_transport_test.cpp
)
target_compile_definitions(p2p_ffts.test PRIVATE
    TRANSPORT_P2P_ENABLE_FFTS_TESTING=1
)
target_include_directories(p2p_ffts.test PRIVATE
    "${ASCEND_ACL_INCLUDE_DIR}"
)
target_link_libraries(p2p_ffts.test PRIVATE
    p2p_ffts_under_test
    "${ASCENDCL_LIBRARY}"
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(p2p_ffts.test)
