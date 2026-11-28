#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops: The Mindgate Conspiracy - Copyright-clean Native Linux x86_64 / Steam Deck packager.

set -euo pipefail

REPO_NAME="Psi-Ops-The-Mindgate-Conspiracy-ReComp"
VERSION="v1.2.3"
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            VERSION="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--version v1.2.3] [--skip-build]"
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

DIST_DIR="${SCRIPT_DIR}/dist"
STAGE_DIR="${DIST_DIR}/stage-linux"
BUILD_DIR="${SCRIPT_DIR}/out/build/headers-only-linux"
TAR_PATH="${DIST_DIR}/${REPO_NAME}-${VERSION}-linux-x86_64.tar.gz"

echo ""
echo "  ${REPO_NAME} ${VERSION} - Native Linux x86_64 / Steam Deck Release Packaging"
echo "  =============================================================================="

if pgrep -x PsiOps >/dev/null 2>&1; then
    echo "  ERROR: PsiOps process is currently running. Stop it before packaging." >&2
    exit 1
fi

for tool in cmake ninja clang clang++ ld.lld patchelf readelf ldd python3 tar realpath; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  ERROR: Required tool '${tool}' not found in PATH." >&2
        exit 1
    fi
done

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "[1/5] Building copyright-clean Linux --headers-only target..."
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
        -DPSIOPS_ALLOW_NO_XBE=ON \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" \
        "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS=-march=x86-64-v2 \
        -DCMAKE_CXX_FLAGS=-march=x86-64-v2
    cmake --build "${BUILD_DIR}" --target PsiOps
else
    echo "[1/5] Skipping build (--skip-build); using existing ${BUILD_DIR}..."
fi

if [[ ! -f "${BUILD_DIR}/PsiOps" ]]; then
    echo "  ERROR: Missing ${BUILD_DIR}/PsiOps." >&2
    exit 1
fi

echo "[2/5] Checking ELF64 GLIBC symbol ceiling..."
MAX_GLIBC="$(readelf --version-info "${BUILD_DIR}/PsiOps" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n 1 || true)"
echo "      PsiOps max GLIBC symbol: ${MAX_GLIBC:-none}"
python3 - "${MAX_GLIBC:-GLIBC_2.0}" <<'PY'
import sys
ver = sys.argv[1].replace("GLIBC_", "")
parts = tuple(int(x) for x in ver.split(".")[:2])
if parts > (2, 35):
    print(f"ERROR: GLIBC symbol {sys.argv[1]} exceeds SteamOS 3.x ceiling GLIBC_2.35", file=sys.stderr)
    sys.exit(1)
PY

echo "[3/5] Staging copyright-clean Linux bundle & RPATH-isolated freestanding compiler/..."
rm -rf "${STAGE_DIR}"
mkdir -p "${DIST_DIR}" "${STAGE_DIR}/config" "${STAGE_DIR}/compiler/lib"

cp -L "$(realpath "${BUILD_DIR}/PsiOps")" "${STAGE_DIR}/PsiOps"
chmod 0755 "${STAGE_DIR}/PsiOps"

install -m 0755 "${SCRIPT_DIR}/packaging/steamdeck/install-steamdeck.sh" "${STAGE_DIR}/install-steamdeck.sh"
install -m 0644 "${SCRIPT_DIR}/packaging/steamdeck/psi-ops-recomp.desktop" "${STAGE_DIR}/psi-ops-recomp.desktop"
install -m 0644 "${SCRIPT_DIR}/resources/psiops.png" "${STAGE_DIR}/psiops.png"
install -m 0644 "${SCRIPT_DIR}/config/game.toml" "${STAGE_DIR}/config/game.toml"

for doc in LICENSE THIRD_PARTY_NOTICES.md README.md; do
    if [[ ! -f "${SCRIPT_DIR}/${doc}" ]]; then
        echo "  ERROR: Missing required release document '${doc}'." >&2
        exit 1
    fi
    install -m 0644 "${SCRIPT_DIR}/${doc}" "${STAGE_DIR}/${doc}"
done

# Stage dereferenced compiler binaries & headers (0 symlinks, 0 compiler/work)
for bin in xbe_recomp clang ld.lld; do
    SRC_BIN="${BUILD_DIR}/compiler/${bin}"
    if [[ ! -e "${SRC_BIN}" ]]; then
        echo "  ERROR: Missing compiler binary ${SRC_BIN}." >&2
        exit 1
    fi
    cp -L "$(realpath "${SRC_BIN}")" "${STAGE_DIR}/compiler/${bin}"
    chmod 0755 "${STAGE_DIR}/compiler/${bin}"
done

install -m 0644 "${BUILD_DIR}/compiler/LLVM-LICENSE.txt" "${STAGE_DIR}/compiler/LLVM-LICENSE.txt"
cp -RL "${BUILD_DIR}/compiler/config" "${STAGE_DIR}/compiler/config"
cp -RL "${BUILD_DIR}/compiler/include" "${STAGE_DIR}/compiler/include"
cp -RL "${BUILD_DIR}/compiler/xdk" "${STAGE_DIR}/compiler/xdk"
if [[ -f "${SCRIPT_DIR}/XboxXBERecomp/xdk/5849/signatures.toml" ]]; then
    mkdir -p "${STAGE_DIR}/compiler/xdk/5849"
    install -m 0644 "${SCRIPT_DIR}/XboxXBERecomp/xdk/5849/signatures.toml" "${STAGE_DIR}/compiler/xdk/5849/signatures.toml"
fi
rm -rf "${STAGE_DIR}/compiler/work"
# Drop any residual debug sections (DWARF comp dirs / source paths) from project-built ELF binaries
if command -v strip >/dev/null 2>&1; then
    strip --strip-debug "${STAGE_DIR}/PsiOps" "${STAGE_DIR}/compiler/xbe_recomp"
fi
find "${STAGE_DIR}/compiler/config" "${STAGE_DIR}/compiler/include" "${STAGE_DIR}/compiler/xdk" -type f -exec chmod 0644 {} +
find "${STAGE_DIR}/compiler" -type d -exec chmod 0755 {} +

echo "[4/5] Collecting non-core shared libraries into compiler/lib/ and patching \$ORIGIN RPATHs..."
python3 - "${STAGE_DIR}/compiler" <<'PY'
import os
import pathlib
import re
import shutil
import subprocess
import sys

comp_dir = pathlib.Path(sys.argv[1]).resolve()
lib_dir = comp_dir / "lib"
lib_dir.mkdir(parents=True, exist_ok=True)

# Universal ABI-guaranteed system libraries provided by SteamOS 3.x & all x86_64 glibc >= 2.35 distros
CORE_LIBS = {
    "libc.so.6",
    "libm.so.6",
    "libdl.so.2",
    "libpthread.so.0",
    "librt.so.1",
    "libgcc_s.so.1",
    "libstdc++.so.6",
    "ld-linux-x86-64.so.2",
    "linux-vdso.so.1",
}

def ldd_deps(elf_path: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    out = subprocess.check_output(["ldd", str(elf_path)], text=True, stderr=subprocess.STDOUT)
    found = []
    for line in out.splitlines():
        m = re.match(r"\s*(\S+)\s*=>\s*(\S+)\s*\(", line)
        if not m:
            continue
        soname, resolved = m.group(1), m.group(2)
        if soname in CORE_LIBS or resolved == "not":
            continue
        p = pathlib.Path(resolved)
        if p.exists():
            found.append((soname, p.resolve()))
    return found

queue = [comp_dir / "xbe_recomp", comp_dir / "clang", comp_dir / "ld.lld"]
copied: dict[str, pathlib.Path] = {}
seen_bins: set[pathlib.Path] = set()

while queue:
    cur = queue.pop(0)
    if cur in seen_bins:
        continue
    seen_bins.add(cur)
    for soname, real_src in ldd_deps(cur):
        if soname in CORE_LIBS:
            continue
        dst = lib_dir / soname
        if soname not in copied:
            shutil.copy2(real_src, dst, follow_symlinks=True)
            os.chmod(dst, 0o755)
            copied[soname] = dst
            queue.append(dst)

# Patch RPATH on compiler binaries ($ORIGIN:$ORIGIN/lib) and bundled libs ($ORIGIN)
for bname in ("xbe_recomp", "clang", "ld.lld"):
    bpath = comp_dir / bname
    subprocess.run(
        ["patchelf", "--force-rpath", "--set-rpath", "$ORIGIN:$ORIGIN/lib", str(bpath)],
        check=True,
    )

for soname, dst in sorted(copied.items()):
    subprocess.run(
        ["patchelf", "--force-rpath", "--set-rpath", "$ORIGIN", str(dst)],
        check=True,
    )

print(f"      Bundled {len(copied)} isolated shared libraries into compiler/lib/: {', '.join(sorted(copied))}")
PY

# Verify zero symlinks and zero forbidden artifacts in staging
SYMLINKS="$(find "${STAGE_DIR}" -type l)"
if [[ -n "${SYMLINKS}" ]]; then
    echo "  ERROR: Symlinks remain in Linux staging tree:" >&2
    echo "${SYMLINKS}" >&2
    exit 1
fi
for bad in libPsiOps.so default.xbe settings.toml; do
    if [[ -e "${STAGE_DIR}/${bad}" ]]; then
        echo "  ERROR: Forbidden artifact '${bad}' detected in staging!" >&2
        exit 1
    fi
done

echo "[5/5] Packing ${TAR_PATH}..."
rm -f "${TAR_PATH}"
tar --sort=name --owner=0 --group=0 --numeric-owner -C "${STAGE_DIR}" -czf "${TAR_PATH}" .
rm -rf "${STAGE_DIR}"

sha256sum "${TAR_PATH}"
ls -lh "${TAR_PATH}"
echo ""
echo "  Linux release packaging complete."
