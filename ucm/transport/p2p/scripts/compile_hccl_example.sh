#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-hccl-example}"
DOWNLOAD_DEPENDENCE="${DOWNLOAD_DEPENDENCE:-OFF}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"

usage() {
    cat <<EOF
Usage:
  bash ${BASH_SOURCE[0]}

Env:
  BUILD_DIR=${BUILD_DIR}
  DOWNLOAD_DEPENDENCE=${DOWNLOAD_DEPENDENCE}
  JOBS=${JOBS}

Examples:
  bash ucm/transport/p2p/scripts/compile_hccl_example.sh
  DOWNLOAD_DEPENDENCE=ON bash ucm/transport/p2p/scripts/compile_hccl_example.sh
EOF
}

case "${1:-}" in
    -h|--help|help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage
        exit 2
        ;;
esac

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DRUNTIME_ENVIRONMENT=ascend \
    -DBUILD_UCM_STORE=OFF \
    -DBUILD_UCM_SPARSE=OFF \
    -DBUILD_UCM_ASU=OFF \
    -DBUILD_UCM_MINDIE=OFF \
    -DBUILD_UNIT_TESTS=OFF \
    -DBUILD_UCM_P2P_HCCL_EXAMPLE=ON \
    -DDOWNLOAD_DEPENDENCE="${DOWNLOAD_DEPENDENCE}"

cmake --build "${BUILD_DIR}" --target ucm_transport_hccl_example -j "${JOBS}"

BIN="${BUILD_DIR}/ucm/transport/p2p/ucm_transport_hccl_example"
if [[ ! -x "${BIN}" ]]; then
    BIN="${BUILD_DIR}/ucm/transport/p2p/Debug/ucm_transport_hccl_example"
fi
echo "${BIN}"
