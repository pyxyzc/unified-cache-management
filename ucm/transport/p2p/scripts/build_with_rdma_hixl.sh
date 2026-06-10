#!/usr/bin/env bash
# export HIXL_INCLUDE_DIR=/usr/local/Ascend/cann-8.5.1/include
# export ASCEND_ROOT=/usr/local/Ascend/cann-8.5.1
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:-}"
LDFLAGS="${LDFLAGS:-}"
HIXL_LIBS="${HIXL_LIBS:--lcann_hixl}"
ASCEND_LIBS="${ASCEND_LIBS:--lascendcl -lmetadef}"
EXTRA_LIBS="${EXTRA_LIBS:--lrt}"

mkdir -p "${BUILD_DIR}"

HIXL_HEADER_FOUND=0
INCLUDES=(
  "-I${ROOT_DIR}/include"
  "-I${ROOT_DIR}/src"
)

LIB_DIRS=()

add_include_dir() {
  local dir="$1"
  if [[ -d "${dir}" ]]; then
    INCLUDES+=("-I${dir}")
  fi
}

add_lib_dir() {
  local dir="$1"
  if [[ -d "${dir}" ]]; then
    LIB_DIRS+=("${dir}")
    LDFLAGS+=" -L${dir} -Wl,-rpath-link,${dir} -Wl,-rpath,${dir}"
  fi
}

add_hixl_include_dir() {
  local dir="$1"
  if [[ -f "${dir}/hixl/hixl.h" ]]; then
    INCLUDES+=("-I${dir}")
    HIXL_HEADER_FOUND=1
  fi
}

if [[ -n "${HIXL_ROOT:-}" ]]; then
  add_hixl_include_dir "${HIXL_ROOT}/include"
  add_hixl_include_dir "${HIXL_ROOT}"
  add_hixl_include_dir "${HIXL_ROOT}/aarch64-linux/include"
  add_lib_dir "${HIXL_ROOT}/lib"
  add_lib_dir "${HIXL_ROOT}/lib64"
  add_lib_dir "${HIXL_ROOT}/aarch64-linux/lib64"
fi
if [[ -n "${HIXL_INCLUDE_DIR:-}" ]]; then
  add_hixl_include_dir "${HIXL_INCLUDE_DIR}"
fi
if [[ -n "${HIXL_LIB_DIR:-}" ]]; then
  add_lib_dir "${HIXL_LIB_DIR}"
fi
if [[ -n "${ASCEND_ROOT:-}" ]]; then
  add_include_dir "${ASCEND_ROOT}/include"
  add_include_dir "${ASCEND_ROOT}/aarch64-linux/include"
  add_lib_dir "${ASCEND_ROOT}/lib64"
  add_lib_dir "${ASCEND_ROOT}/aarch64-linux/lib64"
fi
if [[ -n "${ASCEND_INCLUDE_DIR:-}" ]]; then
  add_include_dir "${ASCEND_INCLUDE_DIR}"
fi
if [[ -n "${ASCEND_LIB_DIRS:-}" ]]; then
  IFS=':' read -r -a ascend_dirs <<< "${ASCEND_LIB_DIRS}"
  for dir in "${ascend_dirs[@]}"; do
    add_lib_dir "${dir}"
  done
fi

if [[ "${HIXL_HEADER_FOUND}" -eq 0 ]]; then
  for dir in \
    /usr/include \
    /usr/local/include \
    /usr/local/hixl/include \
    /opt/hixl/include; do
    add_hixl_include_dir "${dir}"
  done
fi

if [[ "${HIXL_HEADER_FOUND}" -eq 0 ]]; then
  cat >&2 <<'EOF'
Cannot find hixl/hixl.h.

Set one of:
  HIXL_ROOT=/path/to/hixl-install-prefix
  HIXL_ROOT=/path/to/hixl-source-root
  HIXL_INCLUDE_DIR=/path/to/directory-containing-hixl-folder
EOF
  exit 1
fi

COMMON_FLAGS=(
  -std=c++17
  -Wall
  -Wextra
  -Wpedantic
)

SOURCES=(
  "${ROOT_DIR}/src/transport.cpp"
  "${ROOT_DIR}/src/transport_internal.cpp"
  "${ROOT_DIR}/src/transport_log.cpp"
  "${ROOT_DIR}/src/tcp_transport.cpp"
  "${ROOT_DIR}/src/hixl_transport.cpp"
  "${ROOT_DIR}/src/rdma_transport.cpp"
  "${ROOT_DIR}/src/transport_manager.cpp"
)

OBJECTS=()
for src in "${SOURCES[@]}"; do
  obj="${BUILD_DIR}/$(basename "${src}" .cpp).o"
  "${CXX}" "${COMMON_FLAGS[@]}" ${CXXFLAGS} "${INCLUDES[@]}" -c "${src}" -o "${obj}"
  OBJECTS+=("${obj}")
done

ar rcs "${BUILD_DIR}/libtransport.a" "${OBJECTS[@]}"

TESTS=(
  hixl_e2e
  rdma_e2e
)

for test_name in "${TESTS[@]}"; do
  "${CXX}" "${COMMON_FLAGS[@]}" ${CXXFLAGS} "${INCLUDES[@]}" \
    "${ROOT_DIR}/tests/e2e/${test_name}.cpp" \
    "${BUILD_DIR}/libtransport.a" \
    -o "${BUILD_DIR}/${test_name}" \
    ${LDFLAGS} -libverbs ${HIXL_LIBS} ${ASCEND_LIBS} ${EXTRA_LIBS} -pthread
done

echo "Built:"
echo "  ${BUILD_DIR}/libtransport.a"
for test_name in "${TESTS[@]}"; do
  echo "  ${BUILD_DIR}/${test_name}"
done
