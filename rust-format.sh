#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash rust-format.sh [--check|--fix|ci] [PATH...]

Rust formatting helper for Unified Cache Management.
If rustfmt is missing and rustup is available, this script installs the
repository Rust toolchain declared in rust-toolchain.toml.

Modes:
  --fix       Format Rust sources in place. This is the default.
  --check     Check Rust formatting without modifying files.
  ci          Alias for --check.

PATH arguments may point to files or directories. When omitted, the repository
root is scanned. Cargo manifests are checked with cargo fmt, and every discovered
.rs file is formatted with rustfmt so newly added files are covered immediately.
EOF
}

ensure_rust_toolchain() {
    if command -v rustfmt &> /dev/null && command -v cargo &> /dev/null; then
        return
    fi

    if ! command -v rustup &> /dev/null; then
        echo "Rust formatting tools are missing, and rustup is unavailable."
        echo "Please install Rust via rustup first:"
        echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
        echo "Then rerun:"
        echo "  rustup toolchain install"
        exit 1
    fi

    echo "Rust formatting tools are missing. Installing the repository Rust toolchain..."
    rustup toolchain install

    if ! command -v rustfmt &> /dev/null || ! command -v cargo &> /dev/null; then
        echo "Rust formatting tools are still unavailable after rustup toolchain install."
        echo "Please check your Rust installation and PATH."
        exit 1
    fi
}

repo_root() {
    local root
    root="$(git rev-parse --show-toplevel 2> /dev/null || pwd)"
    cd "$root" && pwd
}

to_unix_path() {
    local path="$1"
    if command -v cygpath &> /dev/null; then
        cygpath -u "$path"
    else
        printf '%s\n' "$path"
    fi
}

collect_rust_files() {
    local root="$1"
    shift
    local args=("$@")

    if ((${#args[@]} == 0)); then
        find "$root" \
            \( -path '*/.git' -o -path '*/target' -o -path '*/build' -o -path '*/dist' \) -prune \
            -o -type f -name '*.rs' -print
        return
    fi

    local item
    for item in "${args[@]}"; do
        if [[ -d "$item" ]]; then
            find "$item" \
                \( -path '*/.git' -o -path '*/target' -o -path '*/build' -o -path '*/dist' \) -prune \
                -o -type f -name '*.rs' -print
        elif [[ -f "$item" && "$item" == *.rs ]]; then
            printf '%s\n' "$item"
        fi
    done
}

find_nearest_manifest() {
    local file="$1"
    local root="$2"
    local dir
    dir="$(cd "$(dirname "$file")" && pwd)"

    while [[ "$dir" == "$root"* ]]; do
        if [[ -f "$dir/Cargo.toml" ]]; then
            printf '%s\n' "$dir/Cargo.toml"
            return
        fi

        if [[ "$dir" == "$root" ]]; then
            break
        fi
        dir="$(dirname "$dir")"
    done
}

collect_manifests() {
    local root="$1"
    shift
    local args=("$@")

    if ((${#args[@]} == 0)); then
        find "$root" \
            \( -path '*/.git' -o -path '*/target' -o -path '*/build' -o -path '*/dist' \) -prune \
            -o -type f -name 'Cargo.toml' -print
        return
    fi

    local item
    for item in "${args[@]}"; do
        if [[ -d "$item" ]]; then
            find "$item" \
                \( -path '*/.git' -o -path '*/target' -o -path '*/build' -o -path '*/dist' \) -prune \
                -o -type f -name 'Cargo.toml' -print
        elif [[ -f "$item" && "$(basename "$item")" == 'Cargo.toml' ]]; then
            printf '%s\n' "$item"
        elif [[ -f "$item" && "$item" == *.rs ]]; then
            find_nearest_manifest "$item" "$root"
        fi
    done
}

main() {
    local mode="fix"
    local paths=()

    while (($# > 0)); do
        case "$1" in
            --fix)
                mode="fix"
                ;;
            --check|ci)
                mode="check"
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            --)
                shift
                paths+=("$@")
                break
                ;;
            -*)
                echo "Unknown option: $1"
                usage
                exit 2
                ;;
            *)
                paths+=("$1")
                ;;
        esac
        shift
    done

    local root
    root="$(repo_root)"
    cd "$root"

    local manifests=()
    local rust_files=()
    mapfile -t manifests < <(collect_manifests "$root" "${paths[@]}" | sort -u)
    mapfile -t rust_files < <(collect_rust_files "$root" "${paths[@]}" | sort -u)

    if ((${#manifests[@]} == 0 && ${#rust_files[@]} == 0)); then
        echo "No Rust sources found. rust-format: passed."
        exit 0
    fi

    ensure_rust_toolchain

    local rustfmt_args=()
    if [[ "$mode" == "check" ]]; then
        rustfmt_args+=(--check)
        echo "Checking Rust formatting..."
    else
        echo "Formatting Rust sources..."
    fi

    if ((${#manifests[@]} > 0)); then
        local manifest
        for manifest in "${manifests[@]}"; do
            echo "cargo fmt: $(to_unix_path "$manifest")"
            if [[ "$mode" == "check" ]]; then
                cargo fmt --manifest-path "$manifest" --all -- --check
            else
                cargo fmt --manifest-path "$manifest" --all
            fi
        done
    fi

    if ((${#rust_files[@]} > 0)); then
        echo "rustfmt: ${#rust_files[@]} file(s)"
        rustfmt --edition 2021 "${rustfmt_args[@]}" "${rust_files[@]}"
    fi

    echo "rust-format: passed."
}

main "$@"
