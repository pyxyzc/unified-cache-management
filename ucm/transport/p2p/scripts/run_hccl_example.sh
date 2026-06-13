#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-hccl-example}"

RANK_COUNT="${RANK_COUNT:-2}"
ROOT_INFO_RANK="${ROOT_INFO_RANK:-0}"
ROOT_RANK="${ROOT_RANK:-0}"
ROOT_IP="${ROOT_IP:-127.0.0.1}"
LISTEN_IP="${LISTEN_IP:-0.0.0.0}"
PORT="${PORT:-8085}"
BYTES="${BYTES:-4096}"
DEVICE_BASE="${DEVICE_BASE:-0}"
BOOTSTRAP_SLEEP="${BOOTSTRAP_SLEEP:-1}"
HCCL_HOST_SOCKET_PORT_RANGE="${HCCL_HOST_SOCKET_PORT_RANGE:-56000-56100}"
HCCL_NPU_SOCKET_PORT_RANGE="${HCCL_NPU_SOCKET_PORT_RANGE:-56200-56300}"
HCCL_OP_RETRY_ENABLE="${HCCL_OP_RETRY_ENABLE:-L0:0,L1:0,L2:0}"

export HCCL_HOST_SOCKET_PORT_RANGE
export HCCL_NPU_SOCKET_PORT_RANGE
export HCCL_OP_RETRY_ENABLE

usage() {
    cat <<EOF
Usage:
  bash ${BASH_SOURCE[0]}
  RANK=0 bash ${BASH_SOURCE[0]}

Compile first:
  bash ${SCRIPT_DIR}/compile_hccl_example.sh

Common env:
  BUILD_DIR=${BUILD_DIR}
  RANK_COUNT=${RANK_COUNT}
  ROOT_INFO_RANK=${ROOT_INFO_RANK}
  ROOT_RANK=${ROOT_RANK}
  ROOT_IP=${ROOT_IP}
  LISTEN_IP=${LISTEN_IP}
  PORT=${PORT}
  BYTES=${BYTES}
  DEVICE_BASE=${DEVICE_BASE}
  DEVICE=<single-rank device id>
  HCCL_HOST_SOCKET_PORT_RANGE=${HCCL_HOST_SOCKET_PORT_RANGE}
  HCCL_NPU_SOCKET_PORT_RANGE=${HCCL_NPU_SOCKET_PORT_RANGE}
  HCCL_SOCKET_IFNAME=<optional host NIC name>

Examples:
  # Local two-rank run on one host. This does not compile.
  bash ucm/transport/p2p/scripts/run_hccl_example.sh

  # Two machines:
  # root-info host
  RANK=0 RANK_COUNT=2 ROOT_INFO_RANK=0 ROOT_RANK=0 LISTEN_IP=0.0.0.0 PORT=8085 DEVICE=0 \\
    bash ucm/transport/p2p/scripts/run_hccl_example.sh

  # client host
  RANK=1 RANK_COUNT=2 ROOT_INFO_RANK=0 ROOT_RANK=0 ROOT_IP=<root-host-ip> PORT=8085 DEVICE=0 \\
    bash ucm/transport/p2p/scripts/run_hccl_example.sh
EOF
}

find_example_bin() {
    local bin="${BUILD_DIR}/ucm/transport/p2p/ucm_transport_hccl_example"
    if [[ -x "${bin}" ]]; then
        echo "${bin}"
        return 0
    fi
    bin="${BUILD_DIR}/ucm/transport/p2p/Debug/ucm_transport_hccl_example"
    if [[ -x "${bin}" ]]; then
        echo "${bin}"
        return 0
    fi
    echo "cannot find ucm_transport_hccl_example under ${BUILD_DIR}; run compile script first" >&2
    return 1
}

run_rank() {
    local bin="$1"
    local rank="$2"
    local endpoint_ip="${ROOT_IP}"
    local device="$((DEVICE_BASE + rank))"

    if [[ "${rank}" == "${ROOT_INFO_RANK}" ]]; then
        endpoint_ip="${LISTEN_IP}"
    fi
    if [[ -n "${RANK:-}" && -n "${DEVICE:-}" ]]; then
        device="${DEVICE}"
    fi

    echo "[rank ${rank}] endpoint=${endpoint_ip}:${PORT} device=${device} " \
        "hcclHostPortRange=${HCCL_HOST_SOCKET_PORT_RANGE} hcclNpuPortRange=${HCCL_NPU_SOCKET_PORT_RANGE}"
    "${bin}" \
        --rank "${rank}" \
        --rank-count "${RANK_COUNT}" \
        --root-info-rank "${ROOT_INFO_RANK}" \
        --root-rank "${ROOT_RANK}" \
        --endpoint-ip "${endpoint_ip}" \
        --endpoint-port "${PORT}" \
        --device "${device}" \
        --bytes "${BYTES}"
}

run_local_example() {
    local bin="$1"
    local pids=()

    run_rank "${bin}" "${ROOT_INFO_RANK}" &
    pids+=("$!")
    sleep "${BOOTSTRAP_SLEEP}"

    for ((rank = 0; rank < RANK_COUNT; ++rank)); do
        if [[ "${rank}" == "${ROOT_INFO_RANK}" ]]; then
            continue
        fi
        run_rank "${bin}" "${rank}" &
        pids+=("$!")
    done

    local failed=0
    for pid in "${pids[@]}"; do
        if ! wait "${pid}"; then
            failed=1
        fi
    done
    return "${failed}"
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

BIN="$(find_example_bin)"
if [[ -n "${RANK:-}" ]]; then
    run_rank "${BIN}" "${RANK}"
else
    run_local_example "${BIN}"
fi
