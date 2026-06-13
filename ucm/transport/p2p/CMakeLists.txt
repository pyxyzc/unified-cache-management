add_library(ucm_transport_p2p_core STATIC
    src/core/transport.cpp
)

target_compile_features(ucm_transport_p2p_core PUBLIC cxx_std_17)

target_include_directories(ucm_transport_p2p_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

option(BUILD_UCM_P2P_HCCL "build HCCL p2p transport implementation." OFF)
option(BUILD_UCM_P2P_HCCL_EXAMPLE "build HCCL p2p transport example." OFF)

if(BUILD_UCM_P2P_HCCL OR BUILD_UCM_P2P_HCCL_EXAMPLE)
    if(NOT RUNTIME_ENVIRONMENT STREQUAL "ascend")
        message(FATAL_ERROR "HCCL p2p transport requires -DRUNTIME_ENVIRONMENT=ascend")
    endif()

    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(Ascend_ROOT $ENV{ASCEND_HOME_PATH})
    elseif(DEFINED ENV{ASCEND_CUSTOM_PATH})
        set(Ascend_ROOT $ENV{ASCEND_CUSTOM_PATH}/latest)
    else()
        set(Ascend_ROOT /usr/local/Ascend/ascend-toolkit/latest)
    endif()

    find_path(P2P_HCCL_INCLUDE_DIR
        NAMES hccl/hccl.h
        HINTS
            ${Ascend_ROOT}/include
            ${Ascend_ROOT}/aarch64-linux/include
            ${Ascend_ROOT}/arm64-linux/include
        NO_DEFAULT_PATH
    )

    find_path(P2P_HCCL_ACL_INCLUDE_DIR
        NAMES acl/acl.h
        HINTS
            ${Ascend_ROOT}/include
            ${Ascend_ROOT}/aarch64-linux/include
            ${Ascend_ROOT}/arm64-linux/include
        NO_DEFAULT_PATH
    )

    find_library(P2P_HCCL_LIBRARY
        NAMES hccl
        HINTS
            ${Ascend_ROOT}/lib64
            ${Ascend_ROOT}/aarch64-linux/lib64
            ${Ascend_ROOT}/arm64-linux/lib64
        NO_DEFAULT_PATH
    )

    find_library(P2P_HCCL_ACL_RT_LIBRARY
        NAMES acl_rt
        HINTS
            ${Ascend_ROOT}/lib64
            ${Ascend_ROOT}/aarch64-linux/lib64
            ${Ascend_ROOT}/arm64-linux/lib64
        NO_DEFAULT_PATH
    )

    if(NOT P2P_HCCL_INCLUDE_DIR OR NOT P2P_HCCL_ACL_INCLUDE_DIR OR NOT P2P_HCCL_LIBRARY OR
       NOT P2P_HCCL_ACL_RT_LIBRARY)
        message(FATAL_ERROR "Ascend CANN HCCL headers/libraries required for ucm_transport_hccl were not found")
    endif()

    add_library(ucm_transport_hccl STATIC
        src/hccl/hccl_transport.cpp
    )

    target_compile_features(ucm_transport_hccl PUBLIC cxx_std_17)
    target_compile_definitions(ucm_transport_hccl PUBLIC UCM_TRANSPORT_ENABLE_HCCL)

    target_include_directories(ucm_transport_hccl
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_include_directories(ucm_transport_hccl SYSTEM
        PUBLIC
            ${P2P_HCCL_INCLUDE_DIR}
            ${P2P_HCCL_ACL_INCLUDE_DIR}
    )

    target_link_libraries(ucm_transport_hccl
        PUBLIC
            ucm_transport_p2p_core
            ${P2P_HCCL_LIBRARY}
            ${P2P_HCCL_ACL_RT_LIBRARY}
    )

    if(BUILD_UCM_P2P_HCCL_EXAMPLE)
        add_executable(ucm_transport_hccl_example
            examples/hccl_collective_example.cpp
        )
        target_link_libraries(ucm_transport_hccl_example
            PRIVATE
                ucm_transport_hccl
        )
    endif()
endif()
