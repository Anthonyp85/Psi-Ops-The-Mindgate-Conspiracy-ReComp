#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Idempotent XboxXBERecomp SDK bootstrap & patch script for Psi-Ops Native Recomp (Linux / WSL2).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$REPO_ROOT/XboxXBERecomp"
PATCH_FILE="$REPO_ROOT/patches/xboxxberecomp.patch"
SDK_URL="https://github.com/TGP482/XboxXBERecomp.git"
SDK_SHA="cc9edda3a821a4204dc28783f429b88a27ade4f9"

if ! command -v git >/dev/null 2>&1; then
    echo "[setup_sdk] ERROR: git not found in PATH." >&2
    exit 1
fi

if [[ ! -f "$PATCH_FILE" ]]; then
    echo "[setup_sdk] ERROR: missing patch file $PATCH_FILE." >&2
    exit 1
fi

if [[ ! -e "$SDK_DIR/.git" ]]; then
    echo "[setup_sdk] Cloning XboxXBERecomp into $SDK_DIR..."
    if [[ -f "$REPO_ROOT/.gitmodules" ]]; then
        git -C "$REPO_ROOT" submodule update --init XboxXBERecomp || true
    fi
    if [[ ! -e "$SDK_DIR/.git" ]]; then
        git clone "$SDK_URL" "$SDK_DIR"
    fi
fi

CUR_SHA="$(git -C "$SDK_DIR" rev-parse HEAD 2>/dev/null || true)"
if [[ "$CUR_SHA" != "$SDK_SHA" ]]; then
    echo "[setup_sdk] Checking out pinned XboxXBERecomp commit $SDK_SHA..."
    if ! git -C "$SDK_DIR" cat-file -e "${SDK_SHA}^{commit}" 2>/dev/null; then
        git -C "$SDK_DIR" fetch origin
    fi
    git -C "$SDK_DIR" checkout "$SDK_SHA"
fi

mark_new_files() {
    while IFS= read -r f; do
        [[ -n "$f" ]] && git -C "$SDK_DIR" add -N -- "$f" >/dev/null 2>&1 || true
    done < <(git -C "$SDK_DIR" ls-files --others --exclude-standard)
}

if git -C "$SDK_DIR" apply --reverse --check --whitespace=nowarn "$PATCH_FILE" >/dev/null 2>&1; then
    echo "[setup_sdk] XboxXBERecomp (${SDK_SHA:0:8}) is already patched."
    mark_new_files
    exit 0
fi

if ! git -C "$SDK_DIR" apply --check --whitespace=nowarn "$PATCH_FILE" >/dev/null 2>&1; then
    echo "[setup_sdk] ERROR: patches/xboxxberecomp.patch does not apply cleanly to $SDK_DIR." >&2
    echo "[setup_sdk] Inspect 'git -C XboxXBERecomp status' and retry on a clean $SDK_SHA tree." >&2
    exit 1
fi

echo "[setup_sdk] Applying patches/xboxxberecomp.patch..."
git -C "$SDK_DIR" apply --whitespace=nowarn "$PATCH_FILE"
mark_new_files
echo "[setup_sdk] XboxXBERecomp (${SDK_SHA:0:8}) patched successfully."
