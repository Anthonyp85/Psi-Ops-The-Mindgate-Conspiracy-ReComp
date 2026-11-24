#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops: The Mindgate Conspiracy (Original Xbox) - Native Linux x86_64 / WSL2 build script

set -euo pipefail

PRESET="linux-release"
ALLOW_NO_XBE="OFF"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            PRESET="$2"
            shift 2
            ;;
        --headers-only)
            ALLOW_NO_XBE="ON"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--preset linux-release|linux-debug] [--headers-only]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if pgrep -x PsiOps >/dev/null 2>&1; then
    echo "ERROR: PsiOps process is currently running. Stop it before building." >&2
    exit 1
fi

for tool in cmake ninja clang clang++ ld.lld pkg-config python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Required tool '$tool' not found in PATH." >&2
        exit 1
    fi
done

echo "[1/4] Toolchain:"
echo "      $(clang++ --version | head -n 1)"
echo "      $(cmake --version | head -n 1)"
echo "      $(ldd --version | head -n 1)"

XBE_PATH=""
if [[ "$ALLOW_NO_XBE" == "OFF" ]]; then
    if [[ -f ".local/xbe/default.xbe" ]]; then
        XBE_PATH=".local/xbe/default.xbe"
    elif [[ -f "assets/default.xbe" ]]; then
        XBE_PATH="assets/default.xbe"
    else
        echo "[2/4] Extracting private .local/xbe/default.xbe from XISO..."
        python3 tools/extract_xbe.py
        XBE_PATH=".local/xbe/default.xbe"
    fi
fi

OUT_DIR="${SCRIPT_DIR}/out/build/${PRESET}"
echo "[3/4] Configuring CMake preset '${PRESET}' (PSIOPS_ALLOW_NO_XBE=${ALLOW_NO_XBE})..."
cmake --preset "${PRESET}" -DPSIOPS_ALLOW_NO_XBE="${ALLOW_NO_XBE}"

if [[ "$ALLOW_NO_XBE" == "OFF" && ! -f "config/game.toml" ]]; then
    echo "      Generating config/game.toml via xbe_symbols..."
    cmake --build "${OUT_DIR}" --target xbe_symbols
    SDK_DIR="$(tr -d '\r\n' < "${OUT_DIR}/sdk_dir.txt")"
    mkdir -p config
    "${OUT_DIR}/xbe_symbols" "${XBE_PATH}" "${SDK_DIR}/xdk" --library PsiOps.dll --out config/game.toml
fi

echo "[4/4] Building PsiOps (${PRESET})..."
cmake --build "${OUT_DIR}"

if [[ -f "${OUT_DIR}/PsiOps" ]] && command -v objdump >/dev/null 2>&1; then
    MAX_GLIBC="$(objdump -T "${OUT_DIR}/PsiOps" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n 1 || true)"
    echo "      ELF64 max GLIBC symbol version: ${MAX_GLIBC:-none}"
fi

echo "Build complete: ${OUT_DIR}/PsiOps"
