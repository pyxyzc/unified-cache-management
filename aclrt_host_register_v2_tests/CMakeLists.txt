cmake_minimum_required(VERSION 3.16)

project(aclrt_host_register_v2_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_ROOT "" CACHE PATH "Path to Ascend toolkit root")

if(NOT ASCEND_ROOT)
    set(_ascend_root_candidates
        "$ENV{ASCEND_ROOT}"
        "$ENV{ASCEND_HOME}"
        "$ENV{ASCEND_TOOLKIT_HOME}"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest"
        "/usr/local/Ascend/ascend-toolkit/latest"
    )
    foreach(_candidate IN LISTS _ascend_root_candidates)
        if(_candidate AND EXISTS "${_candidate}")
            set(ASCEND_ROOT "${_candidate}" CACHE PATH "Path to Ascend toolkit root" FORCE)
            break()
        endif()
    endforeach()
endif()

find_path(ACL_INCLUDE_DIR
    NAMES acl/acl.h
    HINTS
        "${ASCEND_ROOT}/include"
        "$ENV{ASCEND_ROOT}/include"
        "$ENV{ASCEND_HOME}/include"
        "$ENV{ASCEND_TOOLKIT_HOME}/include"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest/include"
        "/usr/local/Ascend/ascend-toolkit/latest/include"
)

find_library(ASCENDCL_LIBRARY
    NAMES ascendcl
    HINTS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/lib64/stub"
        "$ENV{ASCEND_ROOT}/lib64"
        "$ENV{ASCEND_HOME}/lib64"
        "$ENV{ASCEND_TOOLKIT_HOME}/lib64"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest/lib64"
        "/usr/local/Ascend/ascend-toolkit/latest/lib64"
)

if(NOT ACL_INCLUDE_DIR)
    message(FATAL_ERROR
        "Cannot find acl/acl.h. Pass -DASCEND_ROOT=/path/to/ascend-toolkit/latest")
endif()

if(NOT ASCENDCL_LIBRARY)
    message(FATAL_ERROR
        "Cannot find libascendcl.so. Pass -DASCEND_ROOT=/path/to/ascend-toolkit/latest")
endif()

add_executable(host_register_v2_probe
    src/host_register_v2_probe.cpp
)

target_include_directories(host_register_v2_probe PRIVATE
    "${ACL_INCLUDE_DIR}"
)

target_link_libraries(host_register_v2_probe PRIVATE
    "${ASCENDCL_LIBRARY}"
)

if(UNIX)
    target_link_libraries(host_register_v2_probe PRIVATE pthread dl)
endif()

message(STATUS "ACL include dir: ${ACL_INCLUDE_DIR}")
message(STATUS "AscendCL library: ${ASCENDCL_LIBRARY}")
