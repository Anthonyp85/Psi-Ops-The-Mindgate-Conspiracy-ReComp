#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Idempotent Steam Deck / SteamOS & Linux desktop installer for Psi-Ops: The Mindgate Conspiracy.
#
# Installs the native Linux icon and .desktop launcher, backs up Steam's binary
# shortcuts.vdf, and registers/updates (or removes with --uninstall) the Native
# Linux Non-Steam Game entry.
#
# IMPORTANT: PsiOps is a NATIVE Linux x86_64 ELF binary. Do NOT enable Steam Play
# / Proton compatibility on this shortcut!

set -euo pipefail

UNINSTALL=0
INSTALL_DIR=""
STEAM_ROOT=""
XDG_DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
APP_NAME="Psi-Ops: The Mindgate Conspiracy"
SLUG="psiops"
DESKTOP_NAME="psi-ops-recomp.desktop"

usage() {
    cat <<EOF
Usage: $0 [options]
  --install-dir <dir>   Directory containing the native Linux PsiOps binary (default: script dir or parent)
  --steam-root <dir>    Override Steam root directory (default: auto-detect ~/.local/share/Steam or ~/.steam/steam)
  --xdg-data <dir>      Override XDG data directory (default: \$XDG_DATA_HOME or ~/.local/share)
  --uninstall           Remove icon, .desktop entry, and Steam shortcuts.vdf entry
  -h, --help            Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --steam-root)
            STEAM_ROOT="$2"
            shift 2
            ;;
        --xdg-data)
            XDG_DATA_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$INSTALL_DIR" ]]; then
    if [[ -x "${SCRIPT_DIR}/PsiOps" ]]; then
        INSTALL_DIR="${SCRIPT_DIR}"
    elif [[ -x "${SCRIPT_DIR}/../../out/build/linux-release/PsiOps" ]]; then
        INSTALL_DIR="$(cd "${SCRIPT_DIR}/../../out/build/linux-release" && pwd)"
    else
        INSTALL_DIR="${SCRIPT_DIR}"
    fi
fi

EXE_PATH="${INSTALL_DIR}/PsiOps"
ICON_SRC=""
for cand in \
    "${INSTALL_DIR}/${SLUG}.png" \
    "${SCRIPT_DIR}/${SLUG}.png" \
    "${SCRIPT_DIR}/../../resources/${SLUG}.png"; do
    if [[ -f "$cand" ]]; then
        ICON_SRC="$cand"
        break
    fi
done

ICON_DST="${XDG_DATA_DIR}/icons/hicolor/256x256/apps/${SLUG}.png"
DESKTOP_DST="${XDG_DATA_DIR}/applications/${DESKTOP_NAME}"

if [[ -z "$STEAM_ROOT" ]]; then
    for cand in "${HOME}/.local/share/Steam" "${HOME}/.steam/steam" "${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [[ -d "${cand}/userdata" ]]; then
            STEAM_ROOT="$cand"
            break
        fi
    done
fi

update_shortcuts_vdf() {
    local mode="$1"
    if [[ -z "$STEAM_ROOT" || ! -d "${STEAM_ROOT}/userdata" ]]; then
        echo "[install-steamdeck] Steam userdata directory not found; skipping shortcuts.vdf update."
        return 0
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "[install-steamdeck] python3 not found; skipping binary shortcuts.vdf update." >&2
        return 1
    fi

    python3 - "$mode" "$STEAM_ROOT" "$APP_NAME" "$EXE_PATH" "$INSTALL_DIR" "$ICON_DST" <<'PYEOF'
import os
import shutil
import struct
import sys
import time
import zlib
from pathlib import Path

mode, steam_root, app_name, exe_path, start_dir, icon_path = sys.argv[1:7]


def read_cstr(buf: bytes, pos: int) -> tuple[str, int]:
    end = buf.index(b"\x00", pos)
    return buf[pos:end].decode("utf-8", errors="replace"), end + 1


def parse_vdf_map(buf: bytes, pos: int) -> tuple[dict, int]:
    out = {}
    while pos < len(buf):
        t = buf[pos]
        pos += 1
        if t == 0x08:
            break
        key, pos = read_cstr(buf, pos)
        if t == 0x00:
            val, pos = parse_vdf_map(buf, pos)
            out[key] = val
        elif t == 0x01:
            val, pos = read_cstr(buf, pos)
            out[key] = val
        elif t == 0x02:
            val = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
            out[key] = val
        else:
            raise ValueError(f"Unsupported binary VDF type byte 0x{t:02X} at offset {pos - 1}")
    return out, pos


def write_vdf_map(d: dict) -> bytes:
    parts = bytearray()
    for k, v in d.items():
        kb = str(k).encode("utf-8") + b"\x00"
        if isinstance(v, dict):
            parts.append(0x00)
            parts.extend(kb)
            parts.extend(write_vdf_map(v))
        elif isinstance(v, int):
            parts.append(0x02)
            parts.extend(kb)
            parts.extend(struct.pack("<I", v & 0xFFFFFFFF))
        else:
            parts.append(0x01)
            parts.extend(kb)
            parts.extend(str(v).encode("utf-8") + b"\x00")
    parts.append(0x08)
    return bytes(parts)


def steam_appid(exe_quoted: str, name: str) -> int:
    crc = zlib.crc32((exe_quoted + name).encode("utf-8")) & 0xFFFFFFFF
    return crc | 0x80000000


userdata = Path(steam_root) / "userdata"
updated = 0
for user_dir in sorted(userdata.iterdir()):
    if not user_dir.is_dir() or not user_dir.name.isdigit():
        continue
    cfg_dir = user_dir / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    vdf_path = cfg_dir / "shortcuts.vdf"

    if vdf_path.is_file() and vdf_path.stat().st_size > 0:
        raw = vdf_path.read_bytes()
        root, _ = parse_vdf_map(raw, 0)
        shortcuts = root.get("shortcuts", {})
        if not isinstance(shortcuts, dict):
            shortcuts = {}
        # Backup existing shortcuts.vdf once per run
        bak = cfg_dir / f"shortcuts.vdf.bak.{int(time.time())}"
        shutil.copy2(vdf_path, bak)
    else:
        shortcuts = {}

    # Preserve existing unrelated shortcuts in numeric order
    ordered = []
    for k, v in sorted(shortcuts.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 999999):
        if not isinstance(v, dict):
            continue
        existing_name = str(v.get("AppName", v.get("appname", "")))
        existing_exe = str(v.get("Exe", v.get("exe", ""))).strip('"')
        if existing_name == app_name or Path(existing_exe).name == "PsiOps":
            continue
        ordered.append(v)

    if mode == "install":
        exe_quoted = f'"{exe_path}"'
        entry = {
            "appid": steam_appid(exe_quoted, app_name),
            "AppName": app_name,
            "Exe": exe_quoted,
            "StartDir": f'"{start_dir}"',
            "icon": icon_path,
            "ShortcutPath": "",
            "LaunchOptions": "",
            "IsHidden": 0,
            "AllowDesktopConfig": 1,
            "AllowOverlay": 1,
            "OpenVR": 0,
            "Devkit": 0,
            "DevkitGameID": "",
            "DevkitOverrideAppID": 0,
            "LastPlayTime": 0,
            "FlatpakAppID": "",
            "tags": {"0": "Native Linux"},
        }
        ordered.append(entry)

    reindexed = {str(i): item for i, item in enumerate(ordered)}
    out_bytes = write_vdf_map({"shortcuts": reindexed})
    tmp = vdf_path.with_suffix(".vdf.tmp")
    tmp.write_bytes(out_bytes)
    os.replace(tmp, vdf_path)
    updated += 1
    print(f"[install-steamdeck] {mode}: updated {vdf_path} (total_entries={len(ordered)})")

if updated == 0:
    print("[install-steamdeck] No numeric Steam userdata profiles found under userdata/.")
PYEOF
}

if [[ "$UNINSTALL" -eq 1 ]]; then
    rm -f "$ICON_DST" "$DESKTOP_DST"
    update_shortcuts_vdf "uninstall"
    echo "[install-steamdeck] Uninstalled icon, desktop entry, and Steam Non-Steam shortcut."
    exit 0
fi

mkdir -p "$(dirname "$ICON_DST")" "$(dirname "$DESKTOP_DST")"

if [[ -n "$ICON_SRC" && -f "$ICON_SRC" ]]; then
    cp -f "$ICON_SRC" "$ICON_DST"
    echo "[install-steamdeck] Installed icon: ${ICON_DST}"
else
    echo "[install-steamdeck] WARNING: ${SLUG}.png icon not found; desktop entry will reference theme icon '${SLUG}'."
fi

cat > "$DESKTOP_DST" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
GenericName=Action-Adventure Game (Native Linux Static Recompilation)
Comment=Native Linux x86_64 Vulkan 1.2 static recompilation of Psi-Ops: The Mindgate Conspiracy (do NOT run under Proton)
Exec="${EXE_PATH}"
Path=${INSTALL_DIR}
Icon=${ICON_DST}
Terminal=false
Categories=Game;ActionGame;
StartupNotify=true
StartupWMClass=PsiOps
EOF
chmod 755 "$DESKTOP_DST"
echo "[install-steamdeck] Installed desktop entry: ${DESKTOP_DST}"

update_shortcuts_vdf "install"

echo ""
echo "================================================================================"
echo "  Psi-Ops: The Mindgate Conspiracy (Native Linux x86_64) installed!"
echo "  Executable : ${EXE_PATH}"
echo "  IMPORTANT  : Restart Steam to see the Non-Steam Game shortcut."
echo "               DO NOT enable 'Force the use of a specific Steam Play"
echo "               compatibility tool' (Proton) — this is a NATIVE Linux binary."
echo "================================================================================"
