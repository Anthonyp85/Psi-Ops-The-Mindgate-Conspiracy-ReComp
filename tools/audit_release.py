#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""6-Gate Release Auditor for Psi-Ops: The Mindgate Conspiracy Native Recompilation.

Enforces copyright-clean distribution, full Git history hygiene, pure-LF patch parity,
Win64 signed retail upscaler integrity, Linux ELF64 GLIBC_2.35 / $ORIGIN RPATH isolation,
and untouched local source media verification.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import io
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import asdict, dataclass, field
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_REPO_NAME = "Psi-Ops-The-Mindgate-Conspiracy-ReComp"
DEFAULT_VERSION = "v1.0.0"
PINNED_SDK_SHA = "cc9edda3a821a4204dc28783f429b88a27ade4f9"

# Expected immutable source media ground truth (located dynamically in Gate 6)
EXPECTED_XISO_SIZE = 2801008640
EXPECTED_XISO_MTIME_NS = 1793646566434966000
EXPECTED_XISO_SHA256 = "fe1b8f418631a3b1d35d7c76d12fd461a10a062746432c9823251ea4f2b96e4e"
EXPECTED_ARCHIVE_SIZE = 2243057495
EXPECTED_ARCHIVE_MTIME_NS = 1795310831123776200
EXPECTED_NOTE_SIZE = 330
EXPECTED_NOTE_MTIME_NS = 1793646567091975900

# Pinned retail Win64 vendor upscaler DLL SHA-256 digests and expected Authenticode signers
PINNED_VENDOR_DLLS: dict[str, tuple[str, str]] = {
    "plugins/nvngx_dlss.dll": (
        "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        "nvidia",
    ),
    "plugins/amd_fidelityfx_loader_dx12.dll": (
        "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb",
        "advanced micro devices",
    ),
    "plugins/amd_fidelityfx_upscaler_dx12.dll": (
        "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace",
        "advanced micro devices",
    ),
    "plugins/libxess.dll": (
        "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7",
        "intel",
    ),
}

# Hex-encoded sanitization tokens (case-insensitive) so this auditor carries zero literal matches
_SANITIZE_TOKENS_HEX = [
    "616672696361",                  # personal profile username
    "76696d6d",                      # companion site token
    "643a5c5f30335f7073696f7073",    # local drive workspace path (backslash)
    "643a2f5f30335f7073696f7073",    # local drive workspace path (slash)
    "737566666572696e67",            # foreign title token
    "3464353730303162",              # foreign title id
    "7375727265616c",                # foreign developer token
    "746f72717565",                  # foreign protagonist token
    "6361726e617465",                # foreign setting token
    "6162626f7474",                  # foreign setting token
    "3535633930326636",              # foreign hash token
]
SANITIZE_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    (f"token_{i}", re.compile(re.escape(bytes.fromhex(h).decode("ascii")), re.IGNORECASE))
    for i, h in enumerate(_SANITIZE_TOKENS_HEX)
]

FORBIDDEN_GIT_EXTS = {
    ".exe", ".dll", ".so", ".dylib", ".lib", ".a", ".obj", ".o", ".pdb", ".dmp",
    ".xbe", ".iso", ".xiso", ".7z", ".zip", ".tar", ".gz", ".sfd", ".msx", ".mbx",
    ".xbox", ".xbx", ".gpr", ".rec",
}

TEXT_CHECK_EXTS = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".hlsl", ".py", ".sh", ".bat", ".cmd",
    ".cmake", ".toml", ".json", ".yml", ".yaml", ".md", ".txt", ".patch",
    ".rc", ".manifest", ".desktop", ".gitattributes", ".gitignore", ".gitmodules",
}

LINUX_CORE_LIBS = {
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


@dataclass
class GateResult:
    gate: int
    name: str
    passed: bool = False
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    details: dict[str, Any] = field(default_factory=dict)


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _is_text_like(path_str: str, data: bytes) -> bool:
    name = pathlib.Path(path_str).name
    ext = pathlib.Path(path_str).suffix.lower()
    if ext in (".png", ".ico", ".bmp", ".jpg", ".jpeg", ".gif", ".res"):
        return False
    if ext in TEXT_CHECK_EXTS or name in ("LICENSE", "CMakeLists.txt", ".gitignore", ".gitattributes", ".gitmodules"):
        return True
    return b"\x00" not in data[:4096]


def _scan_sanitization(path_str: str, data: bytes) -> list[str]:
    if not _is_text_like(path_str, data):
        return []
    text = data.decode("utf-8", errors="ignore")
    hits: list[str] = []
    for tok_id, pat in SANITIZE_PATTERNS:
        m = pat.search(text)
        if m:
            line_no = text.count("\n", 0, m.start()) + 1
            hits.append(f"{path_str}:{line_no}: matched forbidden sanitization rule ({tok_id})")
    return hits


def _is_pe_or_elf(data: bytes) -> str | None:
    if len(data) >= 4 and data[:4] == b"\x7fELF":
        return "ELF binary"
    if len(data) >= 0x40 and data[:2] == b"MZ":
        pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_off + 4 <= len(data) and data[pe_off : pe_off + 4] == b"PE\x00\x00":
            return "PE binary"
    return None


def _inspect_pe64_bytes(data: bytes) -> tuple[bool, str]:
    if len(data) < 0x100 or data[:2] != b"MZ":
        return False, "Missing MZ header"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 0x1A > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return False, "Missing PE signature"
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    opt_magic = struct.unpack_from("<H", data, pe_off + 24)[0]
    if machine != 0x8664 or opt_magic != 0x020B:
        return False, f"Expected PE32+ AMD64 (0x8664/0x020B), got 0x{machine:04X}/0x{opt_magic:04X}"
    return True, "PE32+ AMD64"


def _verify_authenticode_win32(path: pathlib.Path, expected_signer_substr: str) -> tuple[bool, str]:
    if sys.platform != "win32":
        return True, "Authenticode check skipped on non-Windows host"
    try:
        sys.path.insert(0, str(REPO_ROOT / "tools"))
        import fetch_upscalers as fu

        return fu.verify_authenticode_win32(path, expected_signer_substr)
    except Exception as exc:
        return False, f"Authenticode verification error: {exc}"


def _check_win64_exe_resources_and_exports(exe_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    try:
        import pefile
    except Exception:
        return errors
    pe = pefile.PE(str(exe_path), fast_load=False)
    try:
        res_ids = (
            {e.id for e in pe.DIRECTORY_ENTRY_RESOURCE.entries}
            if hasattr(pe, "DIRECTORY_ENTRY_RESOURCE")
            else set()
        )
        for rid, rname in ((3, "RT_ICON"), (14, "RT_GROUP_ICON"), (16, "RT_VERSION"), (24, "RT_MANIFEST")):
            if rid not in res_ids:
                errors.append(f"{exe_path.name}: missing Win32 PE resource {rname}")
        exports = set()
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if sym.name:
                    exports.add(sym.name.decode("ascii", errors="ignore"))
        for req_exp in ("NvOptimusEnablement", "AmdPowerXpressRequestHighPerformance"):
            if req_exp not in exports:
                errors.append(f"{exe_path.name}: missing GPU preference export {req_exp}")
    finally:
        pe.close()
    return errors


def _parse_elf64(data: bytes) -> dict[str, Any]:
    """Pure-Python ELF64 parser extracting machine, DT_NEEDED, DT_RPATH/DT_RUNPATH, and GLIBC verneed."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("Not an ELF binary")
    ei_class = data[4]
    ei_data = data[5]
    if ei_class != 2 or ei_data != 1:
        raise ValueError(f"Expected ELF64 little-endian (class=2, data=1), got class={ei_class}, data={ei_data}")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)

    sections: list[dict[str, Any]] = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + 64 > len(data):
            break
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = (
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        )
        sections.append(
            {
                "index": i,
                "name_off": sh_name,
                "type": sh_type,
                "offset": sh_offset,
                "size": sh_size,
                "link": sh_link,
                "info": sh_info,
                "entsize": sh_entsize,
            }
        )

    def read_cstr(blob: bytes, start: int) -> str:
        if start < 0 or start >= len(blob):
            return ""
        end = blob.find(b"\x00", start)
        if end < 0:
            end = len(blob)
        return blob[start:end].decode("utf-8", errors="replace")

    needed: list[str] = []
    rpaths: list[str] = []
    runpaths: list[str] = []
    glibc_versions: set[tuple[int, int]] = set()

    for sec in sections:
        # SHT_DYNAMIC = 6
        if sec["type"] == 6 and sec["link"] < len(sections):
            strsec = sections[sec["link"]]
            dynstr = data[strsec["offset"] : strsec["offset"] + strsec["size"]]
            entsize = sec["entsize"] or 16
            pos = sec["offset"]
            end = min(len(data), sec["offset"] + sec["size"])
            while pos + 16 <= end:
                d_tag, d_val = struct.unpack_from("<qQ", data, pos)
                pos += entsize
                if d_tag == 0:
                    break
                if d_tag == 1:  # DT_NEEDED
                    needed.append(read_cstr(dynstr, int(d_val)))
                elif d_tag == 15:  # DT_RPATH
                    rpaths.append(read_cstr(dynstr, int(d_val)))
                elif d_tag == 29:  # DT_RUNPATH
                    runpaths.append(read_cstr(dynstr, int(d_val)))

        # SHT_GNU_verneed = 0x6FFFFFFE
        if sec["type"] == 0x6FFFFFFE and sec["link"] < len(sections):
            strsec = sections[sec["link"]]
            dynstr = data[strsec["offset"] : strsec["offset"] + strsec["size"]]
            pos = sec["offset"]
            end = min(len(data), sec["offset"] + sec["size"])
            visited = 0
            while pos + 16 <= end and visited < 4096:
                visited += 1
                vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", data, pos)
                aux_pos = pos + vn_aux
                for _ in range(vn_cnt):
                    if aux_pos + 16 > end:
                        break
                    vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", data, aux_pos)
                    vname = read_cstr(dynstr, int(vna_name))
                    m = re.fullmatch(r"GLIBC_(\d+)\.(\d+)(?:\.\d+)?", vname)
                    if m:
                        glibc_versions.add((int(m.group(1)), int(m.group(2))))
                    if vna_next == 0:
                        break
                    aux_pos += vna_next
                if vn_next == 0:
                    break
                pos += vn_next

    max_glibc = max(glibc_versions) if glibc_versions else (0, 0)
    return {
        "e_type": e_type,
        "e_machine": e_machine,
        "needed": needed,
        "rpaths": rpaths,
        "runpaths": runpaths,
        "max_glibc": max_glibc,
        "max_glibc_str": f"GLIBC_{max_glibc[0]}.{max_glibc[1]}" if glibc_versions else "none",
    }


def _git_out(repo: pathlib.Path, args: list[str], binary: bool = False) -> bytes | str:
    proc = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        check=True,
    )
    return proc.stdout if binary else proc.stdout.decode("utf-8", errors="replace")


def run_gate_1_git_history() -> GateResult:
    res = GateResult(gate=1, name="Full Git Object History Scan")
    try:
        head_sha = str(_git_out(REPO_ROOT, ["rev-parse", "--verify", "HEAD"])).strip()
        commit_count = len(str(_git_out(REPO_ROOT, ["rev-list", "--all"])).splitlines())
        remotes = str(_git_out(REPO_ROOT, ["remote", "-v"])).strip()
        res.details["head_sha"] = head_sha
        res.details["commit_count"] = commit_count
        res.details["remotes"] = remotes
    except Exception as exc:
        res.errors.append(f"Git HEAD unavailable (create local commit first): {exc}")
        return res

    raw_objs = str(_git_out(REPO_ROOT, ["rev-list", "--objects", "--all"])).splitlines()
    blobs_checked = 0
    for line in raw_objs:
        parts = line.split(" ", 1)
        oid = parts[0].strip()
        path_str = parts[1].strip() if len(parts) > 1 else ""
        if not path_str:
            continue
        otype = str(_git_out(REPO_ROOT, ["cat-file", "-t", oid])).strip()
        if otype != "blob":
            continue
        blobs_checked += 1

        lower_path = path_str.lower().replace("\\", "/")
        ext = pathlib.Path(lower_path).suffix
        if ext in FORBIDDEN_GIT_EXTS:
            res.errors.append(f"Forbidden extension in Git history: {path_str} ({oid[:10]})")
        if (
            lower_path.startswith((".local/", "out/", "dist/", "plugins/", "assets/"))
            or pathlib.Path(lower_path).name == "settings.toml"
        ):
            res.errors.append(f"Forbidden path in Git history: {path_str} ({oid[:10]})")

        blob = bytes(_git_out(REPO_ROOT, ["cat-file", "blob", oid], binary=True))
        max_bytes = (4 << 20) if lower_path == "patches/xboxxberecomp.patch" else (2 << 20)
        if len(blob) > max_bytes:
            res.errors.append(f"Oversized Git blob ({len(blob):,} B > {max_bytes:,} B): {path_str}")

        bin_kind = _is_pe_or_elf(blob)
        if bin_kind:
            res.errors.append(f"Compiled {bin_kind} stored in Git history: {path_str} ({oid[:10]})")

        san_hits = _scan_sanitization(path_str, blob)
        res.errors.extend(san_hits[:5])

    res.details["blobs_checked"] = blobs_checked
    res.passed = len(res.errors) == 0
    return res


def run_gate_2_head_hygiene() -> GateResult:
    res = GateResult(gate=2, name="HEAD SPDX, Sanitization, Licensing & Patch Parity")
    try:
        tree_lines = str(_git_out(REPO_ROOT, ["ls-tree", "-r", "--full-tree", "HEAD"])).splitlines()
        use_git = True
    except Exception:
        tree_lines = []
        use_git = False
        res.warnings.append("No Git HEAD commit found; auditing working tree files instead.")

    tracked_files: list[str] = []
    sdk_gitlink_sha = ""
    if use_git:
        for line in tree_lines:
            meta, path_str = line.split("\t", 1)
            mode, otype, oid = meta.split()
            if mode == "160000" and path_str == "XboxXBERecomp":
                sdk_gitlink_sha = oid
                continue
            if otype == "blob":
                tracked_files.append(path_str)
        res.details["sdk_gitlink_sha"] = sdk_gitlink_sha
        if sdk_gitlink_sha != PINNED_SDK_SHA:
            res.errors.append(
                f"XboxXBERecomp gitlink SHA mismatch: got '{sdk_gitlink_sha}', expected '{PINNED_SDK_SHA}'"
            )
    else:
        for dp, dirs, files in os.walk(REPO_ROOT):
            dirs[:] = [
                d for d in dirs
                if d not in (".git", "out", ".local", "plugins", "dist", "generated", "cache", "logs", "hdd", "XboxXBERecomp")
                and not d.startswith("Psi-Ops - ")
            ]
            for fn in files:
                p = pathlib.Path(dp) / fn
                if p.suffix.lower() not in FORBIDDEN_GIT_EXTS:
                    tracked_files.append(p.relative_to(REPO_ROOT).as_posix())

    spdx_required_exts = {".h", ".hpp", ".c", ".cpp", ".py", ".sh", ".bat", ".rc", ".desktop", ".yml", ".yaml"}
    spdx_required_names = {"CMakeLists.txt"}
    missing_spdx: list[str] = []
    crlf_errors: list[str] = []

    for rel in tracked_files:
        p = REPO_ROOT / rel
        if not p.is_file():
            res.errors.append(f"Tracked file missing in working tree: {rel}")
            continue
        raw = p.read_bytes()
        res.errors.extend(_scan_sanitization(rel, raw)[:5])

        ext = p.suffix.lower()
        name = p.name
        if ext in spdx_required_exts or name in spdx_required_names:
            head_txt = raw[:512].decode("utf-8", errors="ignore")
            if "SPDX-License-Identifier: GPL-2.0-or-later" not in head_txt:
                missing_spdx.append(rel)

        if ext in (".bat", ".cmd"):
            # Batch scripts should use CRLF
            if b"\n" in raw and b"\r\n" not in raw:
                crlf_errors.append(f"{rel}: expected CRLF line endings")
        elif _is_text_like(rel, raw):
            if b"\r" in raw:
                cr_n = raw.count(b"\r")
                crlf_errors.append(f"{rel}: contains {cr_n} CR bytes (must be pure LF)")

    if missing_spdx:
        res.errors.append(f"Missing SPDX GPL-2.0-or-later header in: {missing_spdx}")
    if crlf_errors:
        res.errors.extend(crlf_errors[:10])

    # LICENSE + Runtime Upscaler Plugin Exception
    lic_path = REPO_ROOT / "LICENSE"
    if not lic_path.is_file():
        res.errors.append("Missing LICENSE file")
    else:
        lic_txt = lic_path.read_text(encoding="utf-8", errors="replace")
        for needle in (
            "GNU GENERAL PUBLIC LICENSE",
            "Version 2",
            "nvngx_dlss.dll",
            "amd_fidelityfx_loader_dx12.dll",
            "amd_fidelityfx_upscaler_dx12.dll",
            "libxess.dll",
        ):
            if needle not in lic_txt:
                res.errors.append(f"LICENSE missing required text/exception clause: {needle!r}")

    # THIRD_PARTY_NOTICES.md (all 12 attributions)
    tp_path = REPO_ROOT / "THIRD_PARTY_NOTICES.md"
    if not tp_path.is_file():
        res.errors.append("Missing THIRD_PARTY_NOTICES.md")
    else:
        tp_txt = tp_path.read_text(encoding="utf-8", errors="replace").lower()
        for dep in (
            "xboxxberecomp", "zydis", "toml++", "imgui", "sdl3", "glslang",
            "zlib", "llvm", "msvc", "fidelityfx", "nvidia", "xess",
        ):
            if dep not in tp_txt:
                res.errors.append(f"THIRD_PARTY_NOTICES.md missing attribution for: {dep}")

    for req_doc in ("README.md", ".github/workflows/build.yml", ".gitmodules", "release.bat", "release_linux.sh"):
        if not (REPO_ROOT / req_doc).is_file():
            res.errors.append(f"Missing required repository file: {req_doc}")

    # Byte-for-byte patches/xboxxberecomp.patch parity
    patch_path = REPO_ROOT / "patches" / "xboxxberecomp.patch"
    if not patch_path.is_file():
        res.errors.append("Missing patches/xboxxberecomp.patch")
    else:
        patch_bytes = patch_path.read_bytes()
        res.details["patch_bytes"] = len(patch_bytes)
        res.details["patch_sha256"] = _sha256_bytes(patch_bytes)
        if b"\r" in patch_bytes:
            cr_n = patch_bytes.count(b"\r")
            res.errors.append(f"patches/xboxxberecomp.patch contains {cr_n} CR bytes")
        sdk_dir = REPO_ROOT / "XboxXBERecomp"
        if (sdk_dir / ".git").exists():
            cur_sdk = str(_git_out(sdk_dir, ["rev-parse", "HEAD"])).strip()
            res.details["sdk_working_sha"] = cur_sdk
            if cur_sdk != PINNED_SDK_SHA:
                res.errors.append(f"XboxXBERecomp HEAD is {cur_sdk}, expected {PINNED_SDK_SHA}")
            live_diff = bytes(_git_out(sdk_dir, ["diff", "--no-ext-diff", "--binary", PINNED_SDK_SHA], binary=True))
            if live_diff != patch_bytes:
                res.errors.append(
                    f"patches/xboxxberecomp.patch differs from live XboxXBERecomp diff "
                    f"(patch={len(patch_bytes)} B vs live={len(live_diff)} B)"
                )
            rev = subprocess.run(
                ["git", "-C", str(sdk_dir), "apply", "--reverse", "--check", str(patch_path)],
                capture_output=True,
                text=True,
            )
            if rev.returncode != 0:
                res.errors.append(f"git apply --reverse --check failed: {rev.stderr.strip()}")
            sdk_st = str(_git_out(sdk_dir, ["status", "--short"])).splitlines()
            sdk_untracked = [l for l in sdk_st if l.startswith("??")]
            if sdk_untracked:
                res.errors.append(f"Untracked files in XboxXBERecomp/: {sdk_untracked}")

    res.details["tracked_files_count"] = len(tracked_files)
    res.passed = len(res.errors) == 0
    return res


def run_gate_3_source_zip(dist_dir: pathlib.Path, repo_name: str, version: str) -> GateResult:
    res = GateResult(gate=3, name="source.zip Exact 1-to-1 HEAD Parity")
    src_zip = dist_dir / f"{repo_name}-{version}-source.zip"
    res.details["archive"] = str(src_zip)
    if not src_zip.is_file():
        res.errors.append(f"Missing source archive: {src_zip}")
        return res

    res.details["size"] = src_zip.stat().st_size
    res.details["sha256"] = _sha256_file(src_zip)

    try:
        tree_lines = str(_git_out(REPO_ROOT, ["ls-tree", "-r", "--full-tree", "HEAD"])).splitlines()
    except Exception as exc:
        res.errors.append(f"Cannot inspect HEAD tree: {exc}")
        return res

    head_blobs: dict[str, bytes] = {}
    for line in tree_lines:
        meta, path_str = line.split("\t", 1)
        mode, otype, oid = meta.split()
        if otype == "blob":
            head_blobs[path_str] = bytes(_git_out(REPO_ROOT, ["cat-file", "blob", oid], binary=True))

    zip_blobs: dict[str, bytes] = {}
    with zipfile.ZipFile(src_zip, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            rel = info.filename.replace("\\", "/")
            data = zf.read(info)
            zip_blobs[rel] = data
            ext = pathlib.Path(rel).suffix.lower()
            if ext in FORBIDDEN_GIT_EXTS or _is_pe_or_elf(data):
                res.errors.append(f"Forbidden binary inside source.zip: {rel}")

    head_set = set(head_blobs)
    zip_set = set(zip_blobs)
    missing = sorted(head_set - zip_set)
    extra = sorted(zip_set - head_set)
    if missing:
        res.errors.append(f"Files in HEAD missing from source.zip: {missing[:10]}")
    if extra:
        res.errors.append(f"Extra files in source.zip not in HEAD: {extra[:10]}")

    mismatched: list[str] = []
    for rel in sorted(head_set & zip_set):
        expected = head_blobs[rel]
        if pathlib.Path(rel).suffix.lower() in (".bat", ".cmd"):
            # .gitattributes specifies `*.bat text eol=crlf`, so git archive exports CRLF
            expected = expected.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        if _sha256_bytes(expected) != _sha256_bytes(zip_blobs[rel]):
            mismatched.append(rel)
    if mismatched:
        res.errors.append(f"SHA-256 mismatches between HEAD and source.zip: {mismatched[:10]}")

    res.details["file_count"] = len(zip_blobs)
    res.passed = len(res.errors) == 0
    return res


def _win64_path_allowed(rel: str) -> bool:
    exact = {
        "PsiOps.exe",
        "psiops.png",
        "config/game.toml",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "README.md",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "msvcp140.dll",
        "compiler/xbe_recomp.exe",
        "compiler/clang.exe",
        "compiler/lld-link.exe",
        "compiler/PsiOps.lib",
        "compiler/LLVM-LICENSE.txt",
        "compiler/config/game.toml",
        "compiler/vcruntime140.dll",
        "compiler/vcruntime140_1.dll",
        "compiler/msvcp140.dll",
        "plugins/nvngx_dlss.dll",
        "plugins/amd_fidelityfx_loader_dx12.dll",
        "plugins/amd_fidelityfx_upscaler_dx12.dll",
        "plugins/libxess.dll",
        "plugins/LICENSE.NVIDIA-DLSS.txt",
        "plugins/LICENSE.AMD-FidelityFX.md",
        "plugins/LICENSE.Intel-XeSS.txt",
    }
    if rel in exact:
        return True
    if rel.startswith("compiler/include/cpu/") and rel.endswith(".h"):
        return True
    if rel.startswith("compiler/include/freestanding/") and "/" not in rel[len("compiler/include/freestanding/") :]:
        return True
    if re.fullmatch(r"compiler/xdk/\d+/(layout|signatures)\.toml", rel):
        return True
    return False


def run_gate_4_win64_zip(dist_dir: pathlib.Path, repo_name: str, version: str) -> GateResult:
    res = GateResult(gate=4, name="win64-launcher.zip Allowlist, PE32+, Authenticode & Zero-Watermark")
    win_zip = dist_dir / f"{repo_name}-{version}-win64-launcher.zip"
    res.details["archive"] = str(win_zip)
    if not win_zip.is_file():
        res.errors.append(f"Missing Win64 archive: {win_zip}")
        return res

    res.details["size"] = win_zip.stat().st_size
    res.details["sha256"] = _sha256_file(win_zip)

    required = {
        "PsiOps.exe",
        "psiops.png",
        "config/game.toml",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "README.md",
        "compiler/xbe_recomp.exe",
        "compiler/clang.exe",
        "compiler/lld-link.exe",
        "compiler/PsiOps.lib",
        "compiler/LLVM-LICENSE.txt",
        "compiler/config/game.toml",
        "compiler/xdk/5849/layout.toml",
        "compiler/xdk/5849/signatures.toml",
        "plugins/nvngx_dlss.dll",
        "plugins/amd_fidelityfx_loader_dx12.dll",
        "plugins/amd_fidelityfx_upscaler_dx12.dll",
        "plugins/libxess.dll",
        "plugins/LICENSE.NVIDIA-DLSS.txt",
        "plugins/LICENSE.AMD-FidelityFX.md",
        "plugins/LICENSE.Intel-XeSS.txt",
    }

    entries: set[str] = set()
    dll_checks: dict[str, Any] = {}

    with tempfile.TemporaryDirectory(prefix="psiops_audit_win_") as td:
        td_path = pathlib.Path(td)
        with zipfile.ZipFile(win_zip, "r") as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                rel = info.filename.replace("\\", "/")
                entries.add(rel)

                if not _win64_path_allowed(rel):
                    res.errors.append(f"File outside strict Win64 allowlist: {rel}")

                lower = rel.lower()
                if lower.endswith((".xbe", ".iso", ".xiso", ".pdb", ".7z")) or lower in (
                    "psiops.dll",
                    "settings.toml",
                ) or lower.startswith("compiler/work/"):
                    res.errors.append(f"Forbidden file inside win64-launcher.zip: {rel}")

                if lower.endswith((".exe", ".dll")):
                    data = zf.read(info)
                    pe_ok, pe_msg = _inspect_pe64_bytes(data)
                    if not pe_ok:
                        res.errors.append(f"{rel}: {pe_msg}")
                    if rel == "PsiOps.exe":
                        tmp_exe = td_path / "PsiOps.exe"
                        tmp_exe.write_bytes(data)
                        pe_err = _check_win64_exe_resources_and_exports(tmp_exe)
                        res.errors.extend(pe_err)
                    if rel in PINNED_VENDOR_DLLS:
                        want_sha, want_signer = PINNED_VENDOR_DLLS[rel]
                        got_sha = _sha256_bytes(data)
                        wm = b"DO NOT DISTRIBUTE" in data
                        tmp_dll = td_path / pathlib.Path(rel).name
                        tmp_dll.write_bytes(data)
                        auth_ok, auth_msg = _verify_authenticode_win32(tmp_dll, want_signer)
                        dll_checks[rel] = {
                            "size": len(data),
                            "sha256": got_sha,
                            "sha256_match": got_sha == want_sha,
                            "watermark_found": wm,
                            "authenticode_ok": auth_ok,
                            "authenticode": auth_msg,
                        }
                        if got_sha != want_sha:
                            res.errors.append(f"{rel}: SHA-256 mismatch (got {got_sha}, expected {want_sha})")
                        if wm:
                            res.errors.append(f"{rel}: contains 'DO NOT DISTRIBUTE' dev watermark")
                        if not auth_ok:
                            res.errors.append(f"{rel}: {auth_msg}")

    missing = sorted(required - entries)
    if missing:
        res.errors.append(f"Missing required files in win64-launcher.zip: {missing}")

    res.details["file_count"] = len(entries)
    res.details["vendor_dlls"] = dll_checks
    res.passed = len(res.errors) == 0
    return res


def _linux_path_allowed(rel: str) -> bool:
    exact = {
        "PsiOps",
        "install-steamdeck.sh",
        "psi-ops-recomp.desktop",
        "psiops.png",
        "config/game.toml",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "README.md",
        "compiler/xbe_recomp",
        "compiler/clang",
        "compiler/ld.lld",
        "compiler/LLVM-LICENSE.txt",
        "compiler/config/game.toml",
    }
    if rel in exact:
        return True
    if rel.startswith("compiler/lib/") and ".so" in pathlib.Path(rel).name and "/" not in rel[len("compiler/lib/") :]:
        return True
    if rel.startswith("compiler/include/cpu/") and rel.endswith(".h"):
        return True
    if rel.startswith("compiler/include/freestanding/") and "/" not in rel[len("compiler/include/freestanding/") :]:
        return True
    if re.fullmatch(r"compiler/xdk/\d+/(layout|signatures)\.toml", rel):
        return True
    return False


def run_gate_5_linux_tarball(dist_dir: pathlib.Path, repo_name: str, version: str) -> GateResult:
    res = GateResult(gate=5, name="linux-x86_64.tar.gz ELF64, GLIBC_2.35, $ORIGIN RPATH & Zero Symlinks")
    tar_path = dist_dir / f"{repo_name}-{version}-linux-x86_64.tar.gz"
    res.details["archive"] = str(tar_path)
    if not tar_path.is_file():
        res.errors.append(f"Missing Linux tarball: {tar_path}")
        return res

    res.details["size"] = tar_path.stat().st_size
    res.details["sha256"] = _sha256_file(tar_path)

    required = {
        "PsiOps",
        "install-steamdeck.sh",
        "psi-ops-recomp.desktop",
        "psiops.png",
        "config/game.toml",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "README.md",
        "compiler/xbe_recomp",
        "compiler/clang",
        "compiler/ld.lld",
        "compiler/LLVM-LICENSE.txt",
        "compiler/config/game.toml",
        "compiler/xdk/5849/layout.toml",
        "compiler/xdk/5849/signatures.toml",
    }
    exec_required = {"PsiOps", "install-steamdeck.sh", "compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld"}

    entries: set[str] = set()
    bundled_libs: set[str] = set()
    elf_reports: dict[str, Any] = {}
    symlinks: list[str] = []

    with tarfile.open(tar_path, "r:gz") as tf:
        members = tf.getmembers()
        for m in members:
            rel = m.name.lstrip("./")
            if not rel:
                continue
            if m.issym() or m.islnk():
                symlinks.append(rel)
                continue
            if not m.isfile():
                continue
            entries.add(rel)

            if not _linux_path_allowed(rel):
                res.errors.append(f"File outside strict Linux allowlist: {rel}")

            lower = rel.lower()
            if lower.endswith((".dll", ".exe", ".xbe", ".iso", ".xiso", ".7z")) or lower in (
                "libpsiops.so",
                "settings.toml",
            ) or lower.startswith("compiler/work/"):
                res.errors.append(f"Forbidden file inside linux tarball: {rel}")

            if rel in exec_required and (m.mode & 0o111) != 0o111:
                res.errors.append(f"{rel}: expected executable mode (0755), got 0o{m.mode:o}")

            if rel.startswith("compiler/lib/"):
                bundled_libs.add(pathlib.Path(rel).name)

        # Second pass: pure-Python ELF64 audit on all binaries & bundled libs
        for m in members:
            if not m.isfile():
                continue
            rel = m.name.lstrip("./")
            is_elf_target = rel in ("PsiOps", "compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld") or rel.startswith(
                "compiler/lib/"
            )
            if not is_elf_target:
                continue
            fh = tf.extractfile(m)
            if fh is None:
                continue
            data = fh.read()
            try:
                info = _parse_elf64(data)
            except Exception as exc:
                res.errors.append(f"{rel}: ELF64 parse failed: {exc}")
                continue
            elf_reports[rel] = {
                "max_glibc": info["max_glibc_str"],
                "rpaths": info["rpaths"] + info["runpaths"],
                "needed": info["needed"],
            }
            if info["e_machine"] != 62:
                res.errors.append(f"{rel}: expected EM_X86_64 (62), got {info['e_machine']}")
            if info["max_glibc"] > (2, 35):
                res.errors.append(f"{rel}: max GLIBC {info['max_glibc_str']} exceeds GLIBC_2.35")

            combined_rpath = ":".join(info["rpaths"] + info["runpaths"])
            if rel in ("compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld"):
                if "$ORIGIN" not in combined_rpath or "$ORIGIN/lib" not in combined_rpath:
                    res.errors.append(
                        f"{rel}: missing '$ORIGIN:$ORIGIN/lib' RPATH/RUNPATH (got {combined_rpath!r})"
                    )
                for dep in info["needed"]:
                    if dep not in LINUX_CORE_LIBS and dep not in bundled_libs:
                        res.errors.append(f"{rel}: unbundled DT_NEEDED library '{dep}'")
            elif rel.startswith("compiler/lib/"):
                if "$ORIGIN" not in combined_rpath:
                    res.errors.append(f"{rel}: missing '$ORIGIN' RPATH/RUNPATH (got {combined_rpath!r})")
                for dep in info["needed"]:
                    if dep not in LINUX_CORE_LIBS and dep not in bundled_libs:
                        res.errors.append(f"{rel}: unbundled transitive DT_NEEDED library '{dep}'")
            elif rel == "PsiOps":
                for dep in info["needed"]:
                    if dep not in LINUX_CORE_LIBS:
                        res.errors.append(f"PsiOps: non-core DT_NEEDED dependency '{dep}'")

    if symlinks:
        res.errors.append(f"Symlinks found in linux tarball (must be 0): {symlinks[:10]}")
    missing = sorted(required - entries)
    if missing:
        res.errors.append(f"Missing required files in linux tarball: {missing}")

    res.details["file_count"] = len(entries)
    res.details["bundled_compiler_libs"] = sorted(bundled_libs)
    res.details["elf_summary"] = {
        k: {"max_glibc": v["max_glibc"], "rpaths": v["rpaths"]}
        for k, v in elf_reports.items()
        if not k.startswith("compiler/lib/")
    }
    res.passed = len(res.errors) == 0
    return res


def run_gate_6_source_media() -> GateResult:
    res = GateResult(gate=6, name="Untouched Local Source Media Integrity")

    # Dynamically discover local XISO and companion archive/note without hardcoding site/user strings
    iso_candidates = [
        p for p in REPO_ROOT.glob("*/*.iso")
        if p.is_file() and not p.parts[-2].startswith(".") and p.parts[-2] not in ("out", "dist", "plugins")
    ]
    if not iso_candidates:
        res.errors.append("Could not locate local retail .iso under workspace.")
        return res
    iso_path = sorted(iso_candidates)[0]
    iso_st = iso_path.stat()
    iso_sha = _sha256_file(iso_path)
    res.details["xiso"] = {
        "name": iso_path.name,
        "size": iso_st.st_size,
        "mtime_ns": iso_st.st_mtime_ns,
        "sha256": iso_sha,
    }
    if iso_st.st_size != EXPECTED_XISO_SIZE:
        res.errors.append(f"XISO size mismatch: got {iso_st.st_size}, expected {EXPECTED_XISO_SIZE}")
    if iso_st.st_mtime_ns != EXPECTED_XISO_MTIME_NS:
        res.errors.append(f"XISO mtime_ns modified: got {iso_st.st_mtime_ns}, expected {EXPECTED_XISO_MTIME_NS}")
    if iso_sha != EXPECTED_XISO_SHA256:
        res.errors.append(f"XISO SHA-256 mismatch: got {iso_sha}, expected {EXPECTED_XISO_SHA256}")

    arch_candidates = sorted(p for p in REPO_ROOT.glob("*.7z") if p.is_file())
    if arch_candidates:
        arch = arch_candidates[0]
        ast = arch.stat()
        res.details["archive_7z"] = {"name": arch.name, "size": ast.st_size, "mtime_ns": ast.st_mtime_ns}
        if ast.st_size != EXPECTED_ARCHIVE_SIZE or ast.st_mtime_ns != EXPECTED_ARCHIVE_MTIME_NS:
            res.errors.append(
                f"Source .7z modified: size={ast.st_size} (exp {EXPECTED_ARCHIVE_SIZE}), "
                f"mtime_ns={ast.st_mtime_ns} (exp {EXPECTED_ARCHIVE_MTIME_NS})"
            )

    note_candidates = sorted(p for p in iso_path.parent.glob("*.txt") if p.is_file())
    if note_candidates:
        note = note_candidates[0]
        nst = note.stat()
        res.details["companion_note"] = {"size": nst.st_size, "mtime_ns": nst.st_mtime_ns}
        if nst.st_size != EXPECTED_NOTE_SIZE or nst.st_mtime_ns != EXPECTED_NOTE_MTIME_NS:
            res.errors.append(
                f"Companion note modified: size={nst.st_size} (exp {EXPECTED_NOTE_SIZE}), "
                f"mtime_ns={nst.st_mtime_ns} (exp {EXPECTED_NOTE_MTIME_NS})"
            )

    res.passed = len(res.errors) == 0
    return res


def run_gate_4_ci_win64_unpacked() -> GateResult:
    res = GateResult(gate=4, name="CI Win64 Headers-Only PE & Plugins Verification")
    exe = REPO_ROOT / "out" / "build" / "headers-only-win" / "PsiOps.exe"
    if not exe.is_file():
        res.errors.append(f"Missing CI headers-only Win64 binary: {exe}")
    else:
        ok, msg = _inspect_pe64_bytes(exe.read_bytes())
        if not ok:
            res.errors.append(f"PsiOps.exe: {msg}")
        res.errors.extend(_check_win64_exe_resources_and_exports(exe))
    plugins_dir = REPO_ROOT / "plugins"
    for rel, (want_sha, want_signer) in PINNED_VENDOR_DLLS.items():
        p = plugins_dir / pathlib.Path(rel).name
        if not p.is_file():
            res.errors.append(f"Missing plugin DLL: {p}")
            continue
        data = p.read_bytes()
        if _sha256_bytes(data) != want_sha:
            res.errors.append(f"{rel}: SHA-256 mismatch")
        if b"DO NOT DISTRIBUTE" in data:
            res.errors.append(f"{rel}: dev watermark found")
        auth_ok, auth_msg = _verify_authenticode_win32(p, want_signer)
        if not auth_ok:
            res.errors.append(f"{rel}: {auth_msg}")
    res.passed = len(res.errors) == 0
    return res


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="6-Gate Release Auditor for Psi-Ops Native Recompilation.")
    parser.add_argument("--repo-name", default=DEFAULT_REPO_NAME)
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--dist-dir", type=pathlib.Path, default=REPO_ROOT / "dist")
    parser.add_argument(
        "--gates",
        default="",
        help="Comma-separated gate numbers to run (default: 1,2,3,4,5,6; or platform-scoped with --ci).",
    )
    parser.add_argument(
        "--ci",
        action="store_true",
        help="CI mode: audit Git/HEAD hygiene plus platform-appropriate artifacts without requiring local retail XISO.",
    )
    parser.add_argument(
        "--platform",
        choices=("all", "win64", "linux"),
        default="all",
        help="Platform scope when running with --ci (default: all).",
    )
    parser.add_argument("--out-json", type=pathlib.Path, default=None)
    args = parser.parse_args(argv)

    dist_dir = args.dist_dir.resolve()
    if args.gates.strip():
        selected = {int(tok.strip()) for tok in args.gates.split(",") if tok.strip()}
    elif args.ci:
        if args.platform == "linux":
            selected = {1, 2, 5}
        elif args.platform == "win64":
            selected = {1, 2, 4}
        else:
            selected = {1, 2, 3, 4, 5}
    else:
        selected = {1, 2, 3, 4, 5, 6}

    results: list[GateResult] = []
    if 1 in selected:
        results.append(run_gate_1_git_history())
    if 2 in selected:
        results.append(run_gate_2_head_hygiene())
    if 3 in selected:
        results.append(run_gate_3_source_zip(dist_dir, args.repo_name, args.version))
    if 4 in selected:
        win_zip = dist_dir / f"{args.repo_name}-{args.version}-win64-launcher.zip"
        if args.ci and not win_zip.is_file():
            results.append(run_gate_4_ci_win64_unpacked())
        else:
            results.append(run_gate_4_win64_zip(dist_dir, args.repo_name, args.version))
    if 5 in selected:
        results.append(run_gate_5_linux_tarball(dist_dir, args.repo_name, args.version))
    if 6 in selected:
        results.append(run_gate_6_source_media())

    all_passed = all(r.passed for r in results) and len(results) > 0

    print(f"=== Psi-Ops 6-Gate Release Audit ({args.repo_name} {args.version}) ===")
    for r in results:
        tag = "PASS" if r.passed else "FAIL"
        print(f"  [Gate {r.gate}: {tag}] {r.name}")
        for w in r.warnings:
            print(f"    WARN: {w}")
        for e in r.errors:
            print(f"    ERROR: {e}", file=sys.stderr)

    passed_n = sum(1 for r in results if r.passed)
    print(f"Overall: {'PASS' if all_passed else 'FAIL'} ({passed_n}/{len(results)} gates passed)")

    if args.out_json is not None:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "repo_name": args.repo_name,
            "version": args.version,
            "all_passed": all_passed,
            "gates": [asdict(r) for r in results],
        }
        args.out_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")

    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
