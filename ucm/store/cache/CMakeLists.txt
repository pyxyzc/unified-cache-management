find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(UCM_CACHE_STORE_RS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rs)
set(UCM_CACHE_STORE_RS_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/rust-target)
file(GLOB_RECURSE UCM_CACHE_STORE_RS_SOURCE_FILES
    CONFIGURE_DEPENDS
    ${UCM_CACHE_STORE_RS_DIR}/src/*.rs)
if(CMAKE_BUILD_TYPE_LOWER STREQUAL "debug")
    set(UCM_CACHE_STORE_RS_PROFILE debug)
    set(UCM_CACHE_STORE_RS_BUILD_ARGS build --manifest-path ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml)
else()
    set(UCM_CACHE_STORE_RS_PROFILE release)
    set(UCM_CACHE_STORE_RS_BUILD_ARGS build --manifest-path ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml --release)
endif()
set(UCM_CACHE_STORE_RS_LIB
    ${UCM_CACHE_STORE_RS_TARGET_DIR}/${UCM_CACHE_STORE_RS_PROFILE}/libucm_cache_store_rs.a)

add_custom_command(
    OUTPUT ${UCM_CACHE_STORE_RS_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${UCM_CACHE_STORE_RS_TARGET_DIR}
            ${CARGO_EXECUTABLE} ${UCM_CACHE_STORE_RS_BUILD_ARGS}
    WORKING_DIRECTORY ${UCM_CACHE_STORE_RS_DIR}
    DEPENDS
        ${UCM_CACHE_STORE_RS_DIR}/Cargo.toml
        ${UCM_CACHE_STORE_RS_DIR}/Cargo.lock
        ${UCM_CACHE_STORE_RS_DIR}/.cargo/config.toml
        ${UCM_CACHE_STORE_RS_SOURCE_FILES}
    COMMENT "Building Rust CacheStore core"
    VERBATIM)

add_custom_target(ucm_cache_store_rs DEPENDS ${UCM_CACHE_STORE_RS_LIB})
add_library(ucm_cache_store_rs_static STATIC IMPORTED GLOBAL)
set_target_properties(ucm_cache_store_rs_static PROPERTIES
    IMPORTED_LOCATION ${UCM_CACHE_STORE_RS_LIB})
add_dependencies(ucm_cache_store_rs_static ucm_cache_store_rs)

file(GLOB_RECURSE UCM_CACHE_STORE_CC_SOURCE_FILES "./cc/*.cc")
add_library(cachestore SHARED ${UCM_CACHE_STORE_CC_SOURCE_FILES})
target_include_directories(cachestore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cc)
target_link_libraries(cachestore PUBLIC storeintf trans infra_logger ucm_cache_store_rs_static pthread dl m)
file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS cachestore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
