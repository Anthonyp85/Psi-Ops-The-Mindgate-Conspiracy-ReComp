#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""6-Gate Release Auditor for Psi-Ops: The Mindgate Conspiracy Native Recompilation.

Enforces copyright-clean distribution, full Git history hygiene, pure-LF patch parity,
Win64 signed retail upscaler integrity, Linux ELF64 GLIBC_2.35 / $ORIGIN RPATH isolation,
and untouched local source media verification. Gate 2b scans project-built PE/ELF/.lib binaries
(out/build/* or --build-dir / --scan-binary) and the packaged project binaries for leaked host paths
(user profiles, home dirs, drive/WSL workspace roots, CMake build/_deps trees, absolute PDB paths).
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
DEFAULT_VERSION = "v1.2.3"
PINNED_SDK_SHA = "cc9edda3a821a4204dc28783f429b88a27ade4f9"
ALLOWED_OWNER_NAME = "anthonyp85"
ALLOWED_OWNER_EMAIL = "214066322+anthonyp85@users.noreply.github.com"

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
    "616672696361",                              # personal profile username
    "616e74686f6e79707269746368657474",          # private email local-part
    "676d61696c2e636f6d",                        # private webmail domain
    "76696d6d",                                  # companion site token
    "5f30335f7073696f7073",                      # local workspace directory token
    "643a5c5f30335f7073696f7073",                # local drive workspace path (backslash)
    "643a2f5f30335f7073696f7073",                # local drive workspace path (slash)
    "5f30325f746865737566666572696e67",          # foreign local workspace token
    "737566666572696e677562756e7475",            # local WSL distro token
    "633a5c75736572735c",                        # Windows user profile path (backslash)
    "633a2f75736572732f",                        # Windows user profile path (slash)
    "2f6d6e742f642f",                            # WSL host drive mount path
    "737566666572696e67",                        # foreign title token
    "3464353730303162",                          # foreign title id
    "7375727265616c",                            # foreign developer token
    "746f72717565",                              # foreign protagonist token
    "6361726e617465",                            # foreign setting token
    "6162626f7474",                              # foreign setting token
    "3535633930326636",                          # foreign hash token
]
SANITIZE_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    (f"token_{i}", re.compile(re.escape(bytes.fromhex(h).decode("ascii")), re.IGNORECASE))
    for i, h in enumerate(_SANITIZE_TOKENS_HEX)
]

# Hex-encoded PII tokens scanned in both ASCII & UTF-16LE across EVERY packaged archive member (including binaries)
_BINARY_PII_STRICT_HEX = [
    ("email_local", "616e74686f6e79707269746368657474"),
    ("email_domain", "676d61696c2e636f6d"),
    ("ws_psiops", "5f30335f7073696f7073"),
    ("ws_foreign", "5f30325f746865737566666572696e67"),
    ("wsl_distro", "737566666572696e677562756e7475"),
    ("win_users_bs", "633a5c75736572735c"),
    ("win_users_fs", "633a2f75736572732f"),
    ("wsl_mnt_d", "2f6d6e742f642f"),
    ("profile_bs", "75736572735c616672696361"),
    ("profile_fs", "75736572732f616672696361"),
    ("profile_home", "686f6d652f616672696361"),
    ("profile_dir_bs", "5c6166726963615c"),
    ("profile_dir_fs", "2f6166726963612f"),
]
_BINARY_PII_STRICT: list[tuple[str, bytes, bytes]] = [
    (
        name,
        bytes.fromhex(h),
        bytes.fromhex(h).decode("ascii").encode("utf-16le"),
    )
    for name, h in _BINARY_PII_STRICT_HEX
]
# Bare tokens strictly forbidden in all project-owned binaries and all text/doc/source files
# (upstream third-party compiler/ICU/vendor libraries legitimately embed Unicode/CLDR/locale
# geographic tables and LLVM instruction mnemonics).
_PROJECT_ONLY_BARE_TOKENS: list[tuple[str, bytes, bytes]] = [
    (
        "profile_bare",
        bytes.fromhex("616672696361"),
        bytes.fromhex("616672696361").decode("ascii").encode("utf-16le"),
    ),
    (
        "companion_site",
        bytes.fromhex("76696d6d"),
        bytes.fromhex("76696d6d").decode("ascii").encode("utf-16le"),
    ),
]
_UPSTREAM_THIRD_PARTY_BINARIES = {
    "compiler/clang.exe",
    "compiler/lld-link.exe",
    "compiler/clang",
    "compiler/ld.lld",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "compiler/vcruntime140.dll",
    "compiler/vcruntime140_1.dll",
    "compiler/msvcp140.dll",
    "plugins/nvngx_dlss.dll",
    "plugins/amd_fidelityfx_loader_dx12.dll",
    "plugins/amd_fidelityfx_upscaler_dx12.dll",
    "plugins/libxess.dll",
}

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
    gate: int | str
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


def _is_upstream_third_party_binary(member_name: str) -> bool:
    norm = member_name.replace("\\", "/").lstrip("./")
    if norm in _UPSTREAM_THIRD_PARTY_BINARIES:
        return True
    return norm.startswith("compiler/lib/") and ".so" in pathlib.Path(norm).name


def _scan_binary_and_text_pii(member_name: str, data: bytes) -> list[str]:
    """Scan raw member bytes (both ASCII and UTF-16LE, case-insensitive) for personal/host PII."""
    low = data.lower()
    hits: list[str] = []
    for rule_name, ascii_needle, utf16_needle in _BINARY_PII_STRICT:
        if ascii_needle in low:
            hits.append(f"{member_name}: matched forbidden PII rule ({rule_name}, ASCII)")
        if utf16_needle in low:
            hits.append(f"{member_name}: matched forbidden PII rule ({rule_name}, UTF-16LE)")

    if not _is_upstream_third_party_binary(member_name):
        for rule_name, ascii_needle, utf16_needle in _PROJECT_ONLY_BARE_TOKENS:
            if ascii_needle in low:
                hits.append(f"{member_name}: matched forbidden PII rule ({rule_name}, ASCII)")
            if utf16_needle in low:
                hits.append(f"{member_name}: matched forbidden PII rule ({rule_name}, UTF-16LE)")

    return hits


# --- Gate 2b: generic host-path leak scanner for project-built PE/ELF/COFF-archive binaries -----------------------
# Complements the hex-token PII rules above with structural patterns, so any builder's machine (other user names,
# drives, WSL mounts, CI runners) is caught too. Anchored to avoid false positives on legitimate strings such as
# https:// URLs, C:\Windows\..., C:\ProgramData, ~/.local/share or /usr/include/... .
_BINARY_ASCII_STR_RE = re.compile(rb"[\x20-\x7e]{5,}")
_BINARY_UTF16LE_STR_RE = re.compile(rb"(?:[\x20-\x7e]\x00){5,}")
_URL_STRIP_RE = re.compile(r"https?://[^\s\"'<>]+", re.IGNORECASE)
# Prebuilt Microsoft CRT/STL static objects carry Microsoft's own build-agent paths (<drive>:\a\_work\N\s\src\vctools\...)
_MSVC_CRT_INTERNAL_RE = re.compile(
    r"[A-Za-z]:[/\\]a[/\\]_work[/\\]\d+[/\\]s[/\\](?:src[/\\]vctools[/\\]crt|binaries)[/\\][^\s\"'<>]+",
    re.IGNORECASE,
)
_POSIX_ANCHOR = r"(?<![A-Za-z0-9_.:/-])"
BINARY_PATH_LEAK_REGEXES: list[tuple[str, re.Pattern[str]]] = [
    ("windows_users_path", re.compile(r"[A-Za-z]:[/\\]Users[/\\][A-Za-z0-9_. -]+", re.IGNORECASE)),
    ("posix_home_or_users_path", re.compile(_POSIX_ANCHOR + r"/(?:home|Users)/[A-Za-z0-9_.-]+[/\\]")),
    ("wsl_mnt_path", re.compile(_POSIX_ANCHOR + r"/mnt/[A-Za-z]/[A-Za-z0-9_.-]+")),
    ("worktree_or_agent_dir", re.compile(r"(?:[/\\]\.gemini[/\\]|[/\\]worktrees[/\\][0-9a-f-]{8,})", re.IGNORECASE)),
    ("cmake_build_dir", re.compile(r"(?:^|[/\\])out[/\\]build(?:[/\\]|$)", re.IGNORECASE)),
    ("fetchcontent_deps_dir", re.compile(r"(?:^|[/\\])_deps[/\\][A-Za-z0-9_.-]+-(?:src|build)(?:[/\\]|$)", re.IGNORECASE)),
    ("abs_pdb_path", re.compile(r"(?:[A-Za-z]:[/\\]|" + _POSIX_ANCHOR + r"/)[A-Za-z0-9_./\\: -]{1,240}\.pdb\b", re.IGNORECASE)),
    ("abs_win_underscore_root", re.compile(r"[A-Za-z]:[/\\]_[A-Za-z0-9_.-]{2,}")),
    (
        "abs_win_source_path",
        re.compile(r"[A-Za-z]:[/\\][A-Za-z0-9_./\\+ -]{1,240}\.(?:c|cc|cpp|cxx|h|hpp|inl|obj|o)\b", re.IGNORECASE),
    ),
    (
        "abs_posix_build_source_path",
        re.compile(
            _POSIX_ANCHOR
            + r"/(?:root|tmp|var/folders|private/var|opt|work|workspace|runner|srv)/[A-Za-z0-9_./+-]{1,240}"
            r"\.(?:c|cc|cpp|cxx|h|hpp|inl|o)\b"
        ),
    ),
]
# Generic directory / account names that must not become dynamic leak patterns
_GENERIC_PATH_NAMES = {
    "user", "users", "public", "default", "root", "admin", "administrator", "runner", "ubuntu", "deck",
    "app", "src", "source", "bin", "lib", "opt", "cpu", "gpu", "xdk", "sdk", "build", "work", "workspace",
    "repo", "recomp", "psiops", "psi-ops", "github", "home",
}

# Project-built binaries scanned inside the release archives (vendor/toolchain binaries are pinned elsewhere)
PROJECT_WIN64_ARCHIVE_BINARIES = {"PsiOps.exe", "compiler/xbe_recomp.exe", "compiler/PsiOps.lib"}
PROJECT_LINUX_ARCHIVE_BINARIES = {"PsiOps", "compiler/xbe_recomp"}
# Project-built binaries looked up (relative) inside each scanned CMake build directory
PROJECT_BUILD_DIR_BINARIES = (
    "PsiOps.exe",
    "PsiOps.dll",
    "xbe_symbols.exe",
    "compiler/xbe_recomp.exe",
    "compiler/PsiOps.lib",
    "PsiOps",
    "libPsiOps.so",
    "xbe_symbols",
    "compiler/xbe_recomp",
)
DEFAULT_BINARY_BUILD_DIRS = (
    "release",
    "relwithdebinfo",
    "linux-release",
    "linux-cross-release",
    "headers-only-win",
    "headers-only-linux",
)


def _pe_codeview_pdb_paths(data: bytes) -> list[str]:
    """Pure-Python PE/COFF CodeView (RSDS/NB10) debug-directory PDB path extractor (PE32 and PE32+)."""
    out: list[str] = []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return out
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 24 > len(data) or data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        return out
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    if opt + size_opt > len(data) or size_opt < 96:
        return out
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B and size_opt >= 112 + 7 * 8:
        num_dd, dd_base = struct.unpack_from("<I", data, opt + 108)[0], opt + 112
    elif magic == 0x10B and size_opt >= 96 + 7 * 8:
        num_dd, dd_base = struct.unpack_from("<I", data, opt + 92)[0], opt + 96
    else:
        return out
    if num_dd <= 6:
        return out
    dbg_rva, dbg_size = struct.unpack_from("<II", data, dd_base + 6 * 8)
    if dbg_rva == 0 or dbg_size < 28:
        return out
    sections: list[tuple[int, int, int, int]] = []
    sec_base = opt + size_opt
    for i in range(num_sections):
        so = sec_base + i * 40
        if so + 40 > len(data):
            break
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, so + 8)
        sections.append((va, max(vsize, raw_size), raw_ptr, raw_size))

    def rva_to_off(rva: int) -> int | None:
        for va, span, raw_ptr, raw_size in sections:
            if va <= rva < va + span:
                delta = rva - va
                return raw_ptr + delta if delta < raw_size and raw_ptr + delta < len(data) else None
        return None

    dir_off = rva_to_off(dbg_rva)
    if dir_off is None:
        return out
    for i in range(dbg_size // 28):
        eo = dir_off + i * 28
        if eo + 28 > len(data):
            break
        dbg_type, size_of_data, _addr, ptr_raw = struct.unpack_from("<IIII", data, eo + 12)
        if dbg_type != 2 or ptr_raw == 0 or size_of_data < 16 or ptr_raw + size_of_data > len(data):
            continue
        cv = data[ptr_raw : ptr_raw + size_of_data]
        raw: bytes | None = None
        if cv[:4] == b"RSDS" and len(cv) >= 24:
            raw = cv[24:]
        elif cv[:4] == b"NB10" and len(cv) >= 16:
            raw = cv[16:]
        if raw is not None:
            nul = raw.find(b"\x00")
            out.append((raw[:nul] if nul >= 0 else raw).decode("utf-8", errors="replace"))
    return out


def _dynamic_leak_regexes(repo_root: pathlib.Path = REPO_ROOT) -> list[tuple[str, re.Pattern[str]]]:
    """Leak patterns derived at runtime from this checkout's directory name and the auditing account name."""
    extra: list[tuple[str, re.Pattern[str]]] = []
    repo_dir = repo_root.name.strip()
    if len(repo_dir) >= 4 and repo_dir.lower() not in _GENERIC_PATH_NAMES | {DEFAULT_REPO_NAME.lower()}:
        extra.append(("repo_checkout_dir", re.compile(rf"[/\\]{re.escape(repo_dir)}(?:[/\\]|$)", re.IGNORECASE)))
    for env_key in ("USERNAME", "USER", "LOGNAME"):
        uname = os.environ.get(env_key, "").strip()
        if len(uname) >= 3 and uname.lower() not in _GENERIC_PATH_NAMES:
            extra.append(("host_username_dir", re.compile(rf"[/\\]{re.escape(uname)}(?:[/\\]|$)", re.IGNORECASE)))
            break
    return extra


def scan_binary_paths(data: bytes, repo_root: pathlib.Path = REPO_ROOT) -> list[str]:
    """Scan compiled binary bytes (ASCII/UTF-8, UTF-16LE strings and PE CodeView records) for host path leaks."""
    findings: list[str] = []
    seen: set[str] = set()

    def record(tag: str, sample: str) -> None:
        key = f"{tag}:{sample.strip()[:160]}"
        if key not in seen:
            seen.add(key)
            findings.append(key)

    for pdb in _pe_codeview_pdb_paths(data):
        if "/" in pdb or "\\" in pdb or ":" in pdb:
            record("pe_codeview_abs_pdb", pdb)

    checks = BINARY_PATH_LEAK_REGEXES + _dynamic_leak_regexes(repo_root)

    def check(strings: Any, enc: str) -> None:
        for s in strings:
            cleaned = _MSVC_CRT_INTERNAL_RE.sub("", _URL_STRIP_RE.sub("", s))
            if not cleaned:
                continue
            for label, rx in checks:
                m = rx.search(cleaned)
                if m:
                    record(f"{enc}:{label}", m.group(0))

    check((m.group(0).decode("ascii", errors="ignore") for m in _BINARY_ASCII_STR_RE.finditer(data)), "ascii")
    check((m.group(0).decode("utf-16le", errors="ignore") for m in _BINARY_UTF16LE_STR_RE.finditer(data)), "utf16le")
    return findings


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


def _check_win64_exe_resources_and_exports(exe_path: pathlib.Path, version: str = DEFAULT_VERSION) -> list[str]:
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

        # Verify RT_VERSION FileVersion / ProductVersion match expected release version
        want_ver = version.lstrip("v")
        ver_strings: dict[str, str] = {}
        if hasattr(pe, "FileInfo"):
            for fi in pe.FileInfo:
                for st in fi:
                    if hasattr(st, "StringTable"):
                        for tbl in st.StringTable:
                            for k, v in tbl.entries.items():
                                ver_strings[k.decode("utf-8", errors="replace")] = v.decode("utf-8", errors="replace")
        for key in ("FileVersion", "ProductVersion"):
            got = ver_strings.get(key, "")
            if got != want_ver and not got.startswith(want_ver + "."):
                errors.append(f"{exe_path.name}: RT_VERSION {key}={got!r} does not match {want_ver!r}")

        # Verify PE RSDS debug record does not leak absolute build-machine PDB paths
        if hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
            for dbg in pe.DIRECTORY_ENTRY_DEBUG:
                entry = getattr(dbg, "entry", None)
                pdb_raw = getattr(entry, "PdbFileName", None)
                if pdb_raw:
                    pdb_str = pdb_raw.rstrip(b"\x00").decode("utf-8", errors="replace")
                    if ":" in pdb_str or "\\" in pdb_str or "/" in pdb_str:
                        errors.append(f"{exe_path.name}: RSDS PDB path leaks host directory ({pdb_str!r})")

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
    res = GateResult(gate=1, name="Full Git Object History & Commit Metadata PII Scan")
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

    # Audit every commit's Author, Committer, and full Commit Message across HEAD/all refs
    raw_log = str(_git_out(REPO_ROOT, ["log", "--all", "--format=%H%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%B%x1e"]))
    commits_audited = 0
    for rec in raw_log.split("\x1e"):
        rec = rec.strip("\r\n")
        if not rec:
            continue
        parts = rec.split("\x1f", 5)
        if len(parts) < 6:
            continue
        sha, an, ae, cn, ce, body = parts
        commits_audited += 1
        meta_blob = f"{an} <{ae}>\n{cn} <{ce}>\n{body}".encode("utf-8", errors="ignore")
        res.errors.extend(_scan_sanitization(f"commit:{sha[:10]}", meta_blob)[:5])
        for role, name, email in (("author", an, ae), ("committer", cn, ce)):
            if name.strip().lower() == ALLOWED_OWNER_NAME and email.strip().lower() != ALLOWED_OWNER_EMAIL:
                res.errors.append(
                    f"commit {sha[:10]}: {role} email {email!r} must use noreply identity {ALLOWED_OWNER_EMAIL!r}"
                )
    res.details["commits_audited"] = commits_audited

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


def run_gate_2b_binary_hygiene(
    build_dirs: list[pathlib.Path], explicit_binaries: list[pathlib.Path], require_any: bool = False
) -> GateResult:
    res = GateResult(gate="2b", name="Built Binary Path Hygiene (zero host/user/workspace/build/PDB paths)")
    targets: list[pathlib.Path] = []
    seen: set[pathlib.Path] = set()

    def add(p: pathlib.Path) -> None:
        rp = p.resolve()
        if rp not in seen:
            seen.add(rp)
            targets.append(p)

    for b in explicit_binaries:
        if b.is_file():
            add(b)
        else:
            res.errors.append(f"Requested --scan-binary file does not exist: {b}")
    dirs = build_dirs or [REPO_ROOT / "out" / "build" / d for d in DEFAULT_BINARY_BUILD_DIRS]
    for d in dirs:
        if not d.is_dir():
            if build_dirs:
                res.errors.append(f"Requested --build-dir directory does not exist: {d}")
            continue
        for rel in PROJECT_BUILD_DIR_BINARIES:
            cand = d / rel
            if cand.is_file():
                add(cand)

    scanned: list[dict[str, Any]] = []
    for t in targets:
        data = t.read_bytes()
        leaks = scan_binary_paths(data)
        try:
            label = t.resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            label = t.name
        scanned.append({"path": label, "bytes": len(data), "clean": not leaks, "leaks": leaks[:20]})
        if leaks:
            res.errors.append(f"{label}: binary host-path leak {leaks[:10]}")

    if not targets:
        msg = "No built project binaries found to scan (pass --build-dir / --scan-binary)."
        (res.errors if require_any else res.warnings).append(msg)
    res.details["binaries_scanned"] = len(scanned)
    res.details["binaries"] = scanned
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
            res.errors.extend(_scan_binary_and_text_pii(rel, data)[:5])
            res.errors.extend(_scan_sanitization(rel, data)[:5])

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

                data = zf.read(info)
                res.errors.extend(_scan_binary_and_text_pii(rel, data)[:5])
                if rel in PROJECT_WIN64_ARCHIVE_BINARIES:
                    leaks = scan_binary_paths(data)
                    if leaks:
                        res.errors.append(f"{rel}: binary host-path leak {leaks[:10]}")

                if lower.endswith((".exe", ".dll")):
                    pe_ok, pe_msg = _inspect_pe64_bytes(data)
                    if not pe_ok:
                        res.errors.append(f"{rel}: {pe_msg}")
                    if rel == "PsiOps.exe":
                        tmp_exe = td_path / "PsiOps.exe"
                        tmp_exe.write_bytes(data)
                        pe_err = _check_win64_exe_resources_and_exports(tmp_exe, version)
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

        # Second pass: pure-Python ELF64 & ASCII/UTF-16LE PII audit on all members
        for m in members:
            if not m.isfile():
                continue
            rel = m.name.lstrip("./")
            fh = tf.extractfile(m)
            if fh is None:
                continue
            data = fh.read()
            res.errors.extend(_scan_binary_and_text_pii(rel, data)[:5])
            if rel in PROJECT_LINUX_ARCHIVE_BINARIES:
                leaks = scan_binary_paths(data)
                if leaks:
                    res.errors.append(f"{rel}: binary host-path leak {leaks[:10]}")
            is_elf_target = rel in ("PsiOps", "compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld") or rel.startswith(
                "compiler/lib/"
            )
            if not is_elf_target:
                continue
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


def run_gate_4_ci_win64_unpacked(version: str = DEFAULT_VERSION) -> GateResult:
    res = GateResult(gate=4, name="CI Win64 Headers-Only PE, PII & Plugins Verification")
    exe = REPO_ROOT / "out" / "build" / "headers-only-win" / "PsiOps.exe"
    if not exe.is_file():
        res.errors.append(f"Missing CI headers-only Win64 binary: {exe}")
    else:
        exe_data = exe.read_bytes()
        ok, msg = _inspect_pe64_bytes(exe_data)
        if not ok:
            res.errors.append(f"PsiOps.exe: {msg}")
        res.errors.extend(_check_win64_exe_resources_and_exports(exe, version))
        res.errors.extend(_scan_binary_and_text_pii("PsiOps.exe", exe_data)[:5])
    recomp = REPO_ROOT / "out" / "build" / "headers-only-win" / "compiler" / "xbe_recomp.exe"
    if recomp.is_file():
        res.errors.extend(_scan_binary_and_text_pii("compiler/xbe_recomp.exe", recomp.read_bytes())[:5])
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
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        action="append",
        default=[],
        help="CMake build directory whose project binaries Gate 2b scans for host-path leaks (repeatable; "
        "default: existing out/build/{" + ",".join(DEFAULT_BINARY_BUILD_DIRS) + "}).",
    )
    parser.add_argument(
        "--scan-binary",
        type=pathlib.Path,
        action="append",
        default=[],
        help="Explicit PE/ELF/.lib file for Gate 2b to scan (repeatable).",
    )
    parser.add_argument("--skip-build-binaries", action="store_true", help="Skip Gate 2b (built binary path hygiene).")
    parser.add_argument(
        "--require-archives",
        action="store_true",
        help="With --ci: fail Gate 4 when the packaged win64-launcher.zip is missing instead of falling back to the "
        "unpacked build-tree check (use in CI right after release.bat).",
    )
    args = parser.parse_args(argv)

    dist_dir = args.dist_dir.resolve()
    if args.gates.strip():
        selected = {tok.strip().lower() for tok in args.gates.split(",") if tok.strip()}
    elif args.ci:
        if args.platform == "linux":
            selected = {"1", "2", "2b", "5"}
        elif args.platform == "win64":
            selected = {"1", "2", "2b", "4"}
        else:
            selected = {"1", "2", "2b", "3", "4", "5"}
    else:
        selected = {"1", "2", "2b", "3", "4", "5", "6"}
    if args.skip_build_binaries and not (args.build_dir or args.scan_binary):
        selected.discard("2b")

    results: list[GateResult] = []
    if "1" in selected:
        results.append(run_gate_1_git_history())
    if "2" in selected:
        results.append(run_gate_2_head_hygiene())
    if "2b" in selected or args.build_dir or args.scan_binary:
        results.append(
            run_gate_2b_binary_hygiene(
                [d.resolve() for d in args.build_dir],
                [b.resolve() for b in args.scan_binary],
                require_any=args.ci or bool(args.build_dir) or bool(args.scan_binary),
            )
        )
    if "3" in selected:
        results.append(run_gate_3_source_zip(dist_dir, args.repo_name, args.version))
    if "4" in selected:
        win_zip = dist_dir / f"{args.repo_name}-{args.version}-win64-launcher.zip"
        if args.ci and not win_zip.is_file() and not args.require_archives:
            results.append(run_gate_4_ci_win64_unpacked(args.version))
        else:
            results.append(run_gate_4_win64_zip(dist_dir, args.repo_name, args.version))
    if "5" in selected:
        results.append(run_gate_5_linux_tarball(dist_dir, args.repo_name, args.version))
    if "6" in selected:
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
