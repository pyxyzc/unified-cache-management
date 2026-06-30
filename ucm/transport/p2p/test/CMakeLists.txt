cmake_minimum_required(VERSION 3.18)
project(p2p_ffts_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(P2P_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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
find_package(GTest QUIET)
if(GTest_FOUND)
    set(P2P_FFTS_GTEST_MAIN GTest::gtest_main)
else()
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE INTERNAL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
    set(P2P_FFTS_GTEST_MAIN gtest_main)
endif()

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
        "${CMAKE_CURRENT_LIST_DIR}/stubs"
        "${P2P_ROOT}/src/one_sided/core"
        "${ASCEND_ACL_INCLUDE_DIR}"
        "${ASCEND_PKG_INCLUDE_DIR}"
        "${ASCEND_FFTS_INCLUDE_DIR}"
)
target_link_libraries(p2p_ffts_under_test
    PRIVATE
        "${ASCENDCL_LIBRARY}"
        "${ASCEND_RUNTIME_LIBRARY}"
        Threads::Threads
)

add_executable(p2p_ffts.test
    ffts_dispatcher_test.cpp
    ffts_engine_test.cpp
    ffts_transport_test.cpp
)
target_compile_definitions(p2p_ffts.test PRIVATE
    TRANSPORT_P2P_ENABLE_FFTS_TESTING=1
)
target_link_libraries(p2p_ffts.test PRIVATE
    p2p_ffts_under_test
    "${P2P_FFTS_GTEST_MAIN}"
)

include(GoogleTest)
gtest_discover_tests(p2p_ffts.test)
