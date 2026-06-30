option(UCM_ENABLE_P2P_FFTS_TRANSPORT "build Ascend FFTS p2p transport support" ON)

add_library(p2p_transport STATIC
    src/one_sided/control/metadata_channel.cpp
    src/one_sided/core/transport_common.cpp
    src/one_sided/core/transport_manager.cpp
)

target_include_directories(p2p_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include/one_sided
)
target_include_directories(p2p_transport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/one_sided/core
)
target_link_libraries(p2p_transport PUBLIC
    infra_status
    infra_logger
)

if(UCM_ENABLE_P2P_FFTS_TRANSPORT)
    if(NOT RUNTIME_ENVIRONMENT STREQUAL "ascend")
        message(FATAL_ERROR
            "UCM_ENABLE_P2P_FFTS_TRANSPORT requires RUNTIME_ENVIRONMENT=ascend")
    endif()

    set(ASCEND_ARCH_DIR "${ASCEND_ROOT}/aarch64-linux" CACHE PATH
        "Path to Ascend architecture-specific directory")
    set(ASCEND_PKG_INCLUDE_DIR "${ASCEND_ARCH_DIR}/pkg_inc" CACHE PATH
        "Path to Ascend package include directory")
    set(ASCEND_FFTS_INCLUDE_DIR "${ASCEND_ARCH_DIR}/pkg_inc/runtime" CACHE PATH
        "Path to Ascend FFTS include directory")
    set(ASCEND_RUNTIME_LIBRARY "${ASCEND_ARCH_DIR}/lib64/libruntime.so" CACHE FILEPATH
        "Path to Ascend runtime library")

    if(NOT EXISTS "${ASCEND_RUNTIME_LIBRARY}")
        message(FATAL_ERROR
            "UCM_ENABLE_P2P_FFTS_TRANSPORT requires libruntime: ${ASCEND_RUNTIME_LIBRARY}")
    endif()
    if(NOT EXISTS "${ASCEND_FFTS_INCLUDE_DIR}/runtime/rt_ffts_plus.h"
        AND NOT EXISTS "${ASCEND_FFTS_INCLUDE_DIR}/rt_external_ffts.h")
        message(FATAL_ERROR
            "UCM_ENABLE_P2P_FFTS_TRANSPORT requires FFTS headers in: ${ASCEND_FFTS_INCLUDE_DIR}")
    endif()

    target_sources(p2p_transport PRIVATE
        src/one_sided/core/ffts_dispatcher.cpp
        src/one_sided/core/ffts_engine.cpp
        src/one_sided/core/ffts_transport.cpp
    )
    target_compile_definitions(p2p_transport PRIVATE
        TRANSPORT_P2P_ENABLE_FFTS=1
    )
    target_include_directories(p2p_transport PRIVATE
        "${ASCEND_FFTS_INCLUDE_DIR}"
        "${ASCEND_PKG_INCLUDE_DIR}"
    )
    target_link_libraries(p2p_transport PRIVATE
        Ascend::ascendcl
        "${ASCEND_RUNTIME_LIBRARY}"
    )
endif()
