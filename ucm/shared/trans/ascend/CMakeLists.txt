set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")
add_library(Ascend::ascendcl UNKNOWN IMPORTED)
set_target_properties(Ascend::ascendcl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ASCEND_ROOT}/include"
    IMPORTED_LOCATION "${ASCEND_ROOT}/lib64/libascendcl.so"
)

find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(UCM_ASCEND_TRANS_RS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rs)
set(UCM_ASCEND_TRANS_RS_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/rust-target)
if(CMAKE_BUILD_TYPE_LOWER STREQUAL "debug")
    set(UCM_ASCEND_TRANS_RS_PROFILE debug)
    set(UCM_ASCEND_TRANS_RS_BUILD_ARGS build --manifest-path ${UCM_ASCEND_TRANS_RS_DIR}/Cargo.toml)
else()
    set(UCM_ASCEND_TRANS_RS_PROFILE release)
    set(UCM_ASCEND_TRANS_RS_BUILD_ARGS build --manifest-path ${UCM_ASCEND_TRANS_RS_DIR}/Cargo.toml --release)
endif()
set(UCM_ASCEND_TRANS_RS_LIB
    ${UCM_ASCEND_TRANS_RS_TARGET_DIR}/${UCM_ASCEND_TRANS_RS_PROFILE}/libucm_ascend_trans_rs.a)

add_custom_command(
    OUTPUT ${UCM_ASCEND_TRANS_RS_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${UCM_ASCEND_TRANS_RS_TARGET_DIR}
            ${CARGO_EXECUTABLE} ${UCM_ASCEND_TRANS_RS_BUILD_ARGS}
    WORKING_DIRECTORY ${UCM_ASCEND_TRANS_RS_DIR}
    DEPENDS
        ${UCM_ASCEND_TRANS_RS_DIR}/Cargo.toml
        ${UCM_ASCEND_TRANS_RS_DIR}/Cargo.lock
        ${UCM_ASCEND_TRANS_RS_DIR}/.cargo/config.toml
        ${UCM_ASCEND_TRANS_RS_DIR}/src/lib.rs
    COMMENT "Building Rust Ascend trans core"
    VERBATIM)

add_custom_target(ucm_ascend_trans_rs DEPENDS ${UCM_ASCEND_TRANS_RS_LIB})
add_library(ucm_ascend_trans_rs_static STATIC IMPORTED GLOBAL)
set_target_properties(ucm_ascend_trans_rs_static PROPERTIES
    IMPORTED_LOCATION ${UCM_ASCEND_TRANS_RS_LIB})
add_dependencies(ucm_ascend_trans_rs_static ucm_ascend_trans_rs)

add_library(trans STATIC
    ascend_device.cc
    ascend_buffer.cc
    ascend_stream.cc
)
if(ASCEND_SUPPORTS_REGISTER_PIN)
    target_compile_definitions(trans PRIVATE ASCEND_SUPPORTS_REGISTER_PIN=1)
endif()
target_link_libraries(trans PUBLIC
    fmt
    ucm_ascend_trans_rs_static
    Ascend::ascendcl
)
