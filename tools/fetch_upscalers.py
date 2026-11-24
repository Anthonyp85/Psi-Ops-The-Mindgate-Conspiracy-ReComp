#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch and verify official vendor-signed x64 temporal upscaler plugins for Psi-Ops.

Downloads the three official vendor-signed retail Windows x64 temporal upscaler
packages and their vendor license notices into the git-ignored ``plugins/``
directory:

  1. NVIDIA DLSS 4 (retail ``/rel/`` build only): ``nvngx_dlss.dll``
  2. AMD FidelityFX SDK 2.4 (FSR 3.1 / 4 DX12):  ``amd_fidelityfx_loader_dx12.dll``,
                                                  ``amd_fidelityfx_upscaler_dx12.dll``
  3. Intel XeSS 2 (SDK v2.1.1):                   ``libxess.dll``

Every DLL is verified against:
  - Pinned SHA-256 digest
  - PE32+ (AMD64 ``0x8664``, optional header magic ``0x020B``)
  - Offline Windows Authenticode (``WinVerifyTrust`` + signer subject match)
  - Zero forbidden dev-watermark markers (e.g. ``DO NOT DISTRIBUTE``)
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = REPO_ROOT / "plugins"
USER_AGENT = "PsiOps-Recomp-UpscalerFetcher/1.0"

FORBIDDEN_WATERMARKS: Tuple[bytes, ...] = (
    b"do not distribute",
    b"confidential - do not distribute",
    b"development build only",
)


@dataclass(frozen=True)
class PluginArtifact:
    name: str
    vendor: str
    version: str
    url: str
    sha256: str
    is_dll: bool
    expected_signer_substr: str = ""
    required_exports: Tuple[str, ...] = ()


# Pinned official vendor release URLs and verified SHA-256 digests.
ARTIFACTS: Tuple[PluginArtifact, ...] = (
    PluginArtifact(
        name="nvngx_dlss.dll",
        vendor="NVIDIA DLSS 4 (retail /rel/)",
        version="v310.9.16",
        url="https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll",
        sha256="d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        is_dll=True,
        expected_signer_substr="NVIDIA",
        required_exports=("NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_EvaluateFeature"),
    ),
    PluginArtifact(
        name="LICENSE.NVIDIA-DLSS.txt",
        vendor="NVIDIA DLSS 4 License",
        version="v310.9.16",
        url="https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/LICENSE.txt",
        sha256="d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
        is_dll=False,
    ),
    PluginArtifact(
        name="amd_fidelityfx_loader_dx12.dll",
        vendor="AMD FidelityFX SDK (FSR 3.1 / 4 DX12 Loader)",
        version="v2.4.0",
        url="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll",
        sha256="e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb",
        is_dll=True,
        expected_signer_substr="Advanced Micro Devices",
        required_exports=("ffxCreateContext", "ffxDispatch", "ffxDestroyContext"),
    ),
    PluginArtifact(
        name="amd_fidelityfx_upscaler_dx12.dll",
        vendor="AMD FidelityFX SDK (FSR 3.1 / 4 DX12 Upscaler)",
        version="v2.4.0",
        url="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll",
        sha256="aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace",
        is_dll=True,
        expected_signer_substr="Advanced Micro Devices",
        required_exports=(),
    ),
    PluginArtifact(
        name="LICENSE.AMD-FidelityFX.md",
        vendor="AMD FidelityFX SDK License",
        version="v2.4.0",
        url="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/docs/license.md",
        sha256="410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe",
        is_dll=False,
    ),
    PluginArtifact(
        name="libxess.dll",
        vendor="Intel XeSS 2",
        version="v2.1.1",
        url="https://raw.githubusercontent.com/intel/xess/v2.1.1/bin/libxess.dll",
        sha256="251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7",
        is_dll=True,
        expected_signer_substr="Intel",
        required_exports=("xessD3D12CreateContext", "xessD3D12Init", "xessD3D12Execute"),
    ),
    PluginArtifact(
        name="LICENSE.Intel-XeSS.txt",
        vendor="Intel XeSS 2 License",
        version="v2.1.1",
        url="https://raw.githubusercontent.com/intel/xess/v2.1.1/LICENSE.txt",
        sha256="784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df",
        is_dll=False,
    ),
)


def compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().lower()


def inspect_pe64(path: Path) -> Tuple[bool, str, List[str]]:
    """Verify PE32+ (AMD64 0x8664, magic 0x020B) and parse export symbol names."""
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return False, "missing MZ header", []
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        return False, "missing PE signature", []
    machine, num_sections, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    if machine != 0x8664:
        return False, f"unexpected PE machine 0x{machine:04X} (expected 0x8664 AMD64)", []
    opt_off = pe_off + 24
    if opt_off + 2 > len(data):
        return False, "truncated optional header", []
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    if opt_magic != 0x020B:
        return False, f"unexpected PE optional magic 0x{opt_magic:04X} (expected 0x020B PE32+)", []

    # Parse sections so we can map Export Directory RVA -> file offset.
    sec_off = opt_off + opt_size
    sections: List[Tuple[int, int, int, int]] = []
    for i in range(num_sections):
        so = sec_off + i * 40
        if so + 40 > len(data):
            break
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, so + 8)
        sections.append((va, max(vsize, rsize), rptr, rsize))

    def rva_to_off(rva: int) -> Optional[int]:
        for va, span, rptr, rsize in sections:
            if va <= rva < va + span:
                off = rptr + (rva - va)
                return off if off < len(data) else None
        return None

    exports: List[str] = []
    if opt_size >= 112 + 8:
        exp_rva, exp_size = struct.unpack_from("<II", data, opt_off + 112)
        if exp_rva and exp_size:
            exp_off = rva_to_off(exp_rva)
            if exp_off is not None and exp_off + 40 <= len(data):
                num_names = struct.unpack_from("<I", data, exp_off + 24)[0]
                names_rva = struct.unpack_from("<I", data, exp_off + 32)[0]
                names_off = rva_to_off(names_rva)
                if names_off is not None:
                    for i in range(min(num_names, 4096)):
                        if names_off + (i + 1) * 4 > len(data):
                            break
                        nrva = struct.unpack_from("<I", data, names_off + i * 4)[0]
                        noff = rva_to_off(nrva)
                        if noff is not None:
                            end = data.find(b"\x00", noff, min(len(data), noff + 256))
                            if end != -1:
                                exports.append(data[noff:end].decode("ascii", errors="replace"))

    return True, f"PE32+ AMD64 (0x8664), {num_sections} sections, {len(exports)} exports", exports


def scan_watermarks(path: Path) -> List[str]:
    """Scan binary for forbidden development/watermark markers (ASCII & UTF-16LE)."""
    hits: List[str] = []
    data = path.read_bytes()
    lower_ascii = data.lower()
    for marker in FORBIDDEN_WATERMARKS:
        if marker in lower_ascii:
            hits.append(f"ASCII:{marker.decode('ascii')}")
        utf16_lo = marker.decode("ascii").encode("utf-16le")
        utf16_hi = marker.decode("ascii").upper().encode("utf-16le")
        if utf16_lo in data or utf16_hi in data:
            hits.append(f"UTF16LE:{marker.decode('ascii')}")
    return hits


def _get_authenticode_signer_win32(path: Path) -> str:
    """Extract the Authenticode certificate subject Common Name via crypt32.dll."""
    if sys.platform != "win32":
        return ""
    try:
        crypt32 = ctypes.windll.crypt32
        CERT_QUERY_OBJECT_FILE = 1
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED = 1 << 10
        CERT_QUERY_FORMAT_FLAG_BINARY = 1 << 1
        CMSG_SIGNER_INFO_PARAM = 6
        CERT_NAME_SIMPLE_DISPLAY_TYPE = 4

        class CRYPT_INTEGER_BLOB(ctypes.Structure):
            _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.c_void_p)]

        class CERT_INFO_PARTIAL(ctypes.Structure):
            _fields_ = [
                ("dwVersion", wintypes.DWORD),
                ("SerialNumber", CRYPT_INTEGER_BLOB),
                ("SignatureAlgorithm_pszObjId", ctypes.c_void_p),
                ("SignatureAlgorithm_Parameters", CRYPT_INTEGER_BLOB),
                ("Issuer", CRYPT_INTEGER_BLOB),
            ]

        class CMSG_SIGNER_INFO_HEADER(ctypes.Structure):
            _fields_ = [
                ("dwVersion", wintypes.DWORD),
                ("Issuer", CRYPT_INTEGER_BLOB),
                ("SerialNumber", CRYPT_INTEGER_BLOB),
            ]

        class CERT_FIND_INFO(ctypes.Structure):
            _fields_ = [
                ("dwVersion", wintypes.DWORD),
                ("SerialNumber", CRYPT_INTEGER_BLOB),
                ("SignatureAlgorithm_pszObjId", ctypes.c_void_p),
                ("SignatureAlgorithm_Parameters", CRYPT_INTEGER_BLOB),
                ("Issuer", CRYPT_INTEGER_BLOB),
            ]

        encoding = wintypes.DWORD(0)
        content_type = wintypes.DWORD(0)
        format_type = wintypes.DWORD(0)
        h_store = ctypes.c_void_p()
        h_msg = ctypes.c_void_p()

        crypt32.CryptQueryObject.argtypes = [
            wintypes.DWORD,
            ctypes.c_wchar_p,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_void_p,
        ]
        crypt32.CryptQueryObject.restype = wintypes.BOOL

        ok = crypt32.CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            str(path),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,
            ctypes.byref(encoding),
            ctypes.byref(content_type),
            ctypes.byref(format_type),
            ctypes.byref(h_store),
            ctypes.byref(h_msg),
            None,
        )
        if not ok or not h_store or not h_msg:
            return ""

        try:
            crypt32.CryptMsgGetParam.argtypes = [
                ctypes.c_void_p,
                wintypes.DWORD,
                wintypes.DWORD,
                ctypes.c_void_p,
                ctypes.POINTER(wintypes.DWORD),
            ]
            crypt32.CryptMsgGetParam.restype = wintypes.BOOL

            cb_signer = wintypes.DWORD(0)
            if not crypt32.CryptMsgGetParam(h_msg, CMSG_SIGNER_INFO_PARAM, 0, None, ctypes.byref(cb_signer)):
                return ""
            buf = ctypes.create_string_buffer(cb_signer.value)
            if not crypt32.CryptMsgGetParam(h_msg, CMSG_SIGNER_INFO_PARAM, 0, buf, ctypes.byref(cb_signer)):
                return ""
            signer = ctypes.cast(buf, ctypes.POINTER(CMSG_SIGNER_INFO_HEADER)).contents

            # Enumerate certificates in embedded store and match Issuer+SerialNumber.
            crypt32.CertFindCertificateInStore.argtypes = [
                ctypes.c_void_p,
                wintypes.DWORD,
                wintypes.DWORD,
                wintypes.DWORD,
                ctypes.c_void_p,
                ctypes.c_void_p,
            ]
            crypt32.CertFindCertificateInStore.restype = ctypes.c_void_p
            CERT_FIND_SUBJECT_CERT = 11 << 16
            X509_ASN_ENCODING = 0x00000001
            PKCS_7_ASN_ENCODING = 0x00010000

            find_info = CERT_FIND_INFO()
            find_info.Issuer = signer.Issuer
            find_info.SerialNumber = signer.SerialNumber

            p_cert = crypt32.CertFindCertificateInStore(
                h_store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SUBJECT_CERT,
                ctypes.byref(find_info),
                None,
            )
            if not p_cert:
                return ""
            try:
                crypt32.CertGetNameStringW.argtypes = [
                    ctypes.c_void_p,
                    wintypes.DWORD,
                    wintypes.DWORD,
                    ctypes.c_void_p,
                    ctypes.c_wchar_p,
                    wintypes.DWORD,
                ]
                crypt32.CertGetNameStringW.restype = wintypes.DWORD
                name_buf = ctypes.create_unicode_buffer(512)
                nchars = crypt32.CertGetNameStringW(
                    p_cert,
                    CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0,
                    None,
                    name_buf,
                    len(name_buf),
                )
                return name_buf.value if nchars > 1 else ""
            finally:
                crypt32.CertFreeCertificateContext.argtypes = [ctypes.c_void_p]
                crypt32.CertFreeCertificateContext(p_cert)
        finally:
            crypt32.CertCloseStore.argtypes = [ctypes.c_void_p, wintypes.DWORD]
            crypt32.CertCloseStore(h_store, 0)
            crypt32.CryptMsgClose.argtypes = [ctypes.c_void_p]
            crypt32.CryptMsgClose(h_msg)
    except Exception:
        return ""


def verify_authenticode_win32(path: Path, expected_signer_substr: str = "") -> Tuple[bool, str]:
    """Run offline WinVerifyTrust Authenticode check on Windows."""
    if sys.platform != "win32":
        return True, "non-Windows host (WinVerifyTrust skipped)"

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", ctypes.c_ubyte * 8),
        ]

    class WINTRUST_FILE_INFO(ctypes.Structure):
        _fields_ = [
            ("cbStruct", wintypes.DWORD),
            ("pcwszFilePath", wintypes.LPCWSTR),
            ("hFile", wintypes.HANDLE),
            ("pgKnownSubject", ctypes.POINTER(GUID)),
        ]

    class WINTRUST_DATA(ctypes.Structure):
        _fields_ = [
            ("cbStruct", wintypes.DWORD),
            ("pPolicyCallbackData", ctypes.c_void_p),
            ("pSIPClientData", ctypes.c_void_p),
            ("dwUIChoice", wintypes.DWORD),
            ("fdwRevocationChecks", wintypes.DWORD),
            ("dwUnionChoice", wintypes.DWORD),
            ("pFile", ctypes.POINTER(WINTRUST_FILE_INFO)),
            ("dwStateAction", wintypes.DWORD),
            ("hWVTStateData", wintypes.HANDLE),
            ("pwszURLReference", wintypes.LPCWSTR),
            ("dwProvFlags", wintypes.DWORD),
            ("dwUIContext", wintypes.DWORD),
            ("pSignatureSettings", ctypes.c_void_p),
        ]

    # WINTRUST_ACTION_GENERIC_VERIFY_V2 {00AAC56B-CD44-11d0-8CC2-00C04FC295EE}
    action_guid = GUID(
        0x00AAC56B,
        0xCD44,
        0x11D0,
        (ctypes.c_ubyte * 8)(0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE),
    )

    WTD_UI_NONE = 2
    WTD_REVOKE_NONE = 0
    WTD_CHOICE_FILE = 1
    WTD_STATEACTION_VERIFY = 1
    WTD_STATEACTION_CLOSE = 2
    WTD_REVOCATION_CHECK_NONE = 0x00000010
    WTD_CACHE_ONLY_URL_RETRIEVAL = 0x00001000

    file_info = WINTRUST_FILE_INFO()
    file_info.cbStruct = ctypes.sizeof(WINTRUST_FILE_INFO)
    file_info.pcwszFilePath = str(path.resolve())
    file_info.hFile = None
    file_info.pgKnownSubject = None

    trust_data = WINTRUST_DATA()
    trust_data.cbStruct = ctypes.sizeof(WINTRUST_DATA)
    trust_data.dwUIChoice = WTD_UI_NONE
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE
    trust_data.dwUnionChoice = WTD_CHOICE_FILE
    trust_data.pFile = ctypes.pointer(file_info)
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY
    trust_data.hWVTStateData = None
    trust_data.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL
    trust_data.dwUIContext = 0

    wintrust = ctypes.windll.wintrust
    wintrust.WinVerifyTrust.argtypes = [
        wintypes.HWND,
        ctypes.POINTER(GUID),
        ctypes.POINTER(WINTRUST_DATA),
    ]
    wintrust.WinVerifyTrust.restype = ctypes.c_long

    status = wintrust.WinVerifyTrust(None, ctypes.byref(action_guid), ctypes.byref(trust_data))

    # Release WVT state data cleanly.
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE
    wintrust.WinVerifyTrust(None, ctypes.byref(action_guid), ctypes.byref(trust_data))

    signer = _get_authenticode_signer_win32(path)
    if status != 0:
        return False, f"WinVerifyTrust failed (0x{status & 0xFFFFFFFF:08X}), signer='{signer}'"
    if expected_signer_substr and expected_signer_substr.lower() not in signer.lower():
        return False, f"signer mismatch: got '{signer}', expected substring '{expected_signer_substr}'"
    return True, f"WinVerifyTrust OK (0x0), signer='{signer}'"


def verify_artifact(
    artifact: PluginArtifact,
    out_dir: Path,
) -> Tuple[bool, Dict[str, object]]:
    dest = out_dir / artifact.name
    info: Dict[str, object] = {
        "name": artifact.name,
        "vendor": artifact.vendor,
        "version": artifact.version,
        "path": str(dest),
        "ok": False,
        "errors": [],
    }
    errors: List[str] = info["errors"]  # type: ignore[assignment]

    if not dest.is_file():
        errors.append("file missing")
        return False, info

    size = dest.stat().st_size
    sha = compute_sha256(dest)
    info["size"] = size
    info["sha256"] = sha

    if size == 0:
        errors.append("empty file")
    if sha != artifact.sha256.lower():
        errors.append(f"SHA-256 mismatch: got {sha}, expected {artifact.sha256.lower()}")

    if artifact.is_dll:
        pe_ok, pe_msg, exports = inspect_pe64(dest)
        info["pe64"] = pe_msg
        info["export_count"] = len(exports)
        if not pe_ok:
            errors.append(f"PE64 check failed: {pe_msg}")
        else:
            export_set = set(exports)
            missing_exports = [e for e in artifact.required_exports if e not in export_set]
            if missing_exports:
                errors.append(f"missing required exports: {missing_exports}")

        auth_ok, auth_msg = verify_authenticode_win32(dest, artifact.expected_signer_substr)
        info["authenticode"] = auth_msg
        if not auth_ok:
            errors.append(f"Authenticode check failed: {auth_msg}")

        wm_hits = scan_watermarks(dest)
        info["watermark_hits"] = wm_hits
        if wm_hits:
            errors.append(f"forbidden watermark marker(s) found: {wm_hits}")

    info["ok"] = len(errors) == 0
    return bool(info["ok"]), info


def download_file(url: str, dest: Path) -> int:
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    fd, tmp_name = tempfile.mkstemp(prefix=f".{dest.name}.", suffix=".tmp", dir=str(dest.parent))
    os.close(fd)
    tmp_path = Path(tmp_name)
    total = 0
    try:
        with urllib.request.urlopen(req, timeout=120) as resp, tmp_path.open("wb") as out_f:
            while True:
                chunk = resp.read(1024 * 1024)
                if not chunk:
                    break
                out_f.write(chunk)
                total += len(chunk)
        shutil.move(str(tmp_path), str(dest))
        return total
    except Exception:
        if tmp_path.exists():
            tmp_path.unlink(missing_ok=True)
        raise


def verify_all_plugins(
    out_dir: Path = DEFAULT_OUT_DIR,
) -> Tuple[bool, List[Dict[str, object]]]:
    results: List[Dict[str, object]] = []
    all_ok = True
    for art in ARTIFACTS:
        ok, info = verify_artifact(art, out_dir)
        results.append(info)
        if not ok:
            all_ok = False
    return all_ok, results


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fetch and verify official vendor-signed x64 upscaler plugins for Psi-Ops."
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help=f"Destination directory (default: {DEFAULT_OUT_DIR})",
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="Verify existing files in --out-dir without downloading.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Re-download all artifacts even if already present and verified.",
    )
    args = parser.parse_args(argv)

    out_dir: Path = args.out_dir.resolve()
    if not args.verify_only:
        out_dir.mkdir(parents=True, exist_ok=True)
        for art in ARTIFACTS:
            dest = out_dir / art.name
            if not args.force and dest.is_file():
                ok, _ = verify_artifact(art, out_dir)
                if ok:
                    print(f"[SKIP] {art.name} already verified ({dest.stat().st_size:,} B)")
                    continue
            print(f"[FETCH] {art.name} ({art.vendor} {art.version}) <- {art.url}")
            try:
                nbytes = download_file(art.url, dest)
                print(f"        downloaded {nbytes:,} B")
            except urllib.error.URLError as exc:
                print(f"[FAIL] download error for {art.name}: {exc}", file=sys.stderr)
                return 2

    all_ok, results = verify_all_plugins(out_dir)
    print(f"\n=== Plugin Verification Summary ({out_dir}) ===")
    for info in results:
        status = "PASS" if info["ok"] else "FAIL"
        size = info.get("size", 0)
        sha = info.get("sha256", "")
        print(f"  [{status}] {info['name']} ({info['version']}, {size:,} B)")
        if sha:
            print(f"         SHA-256: {sha}")
        if "pe64" in info:
            print(f"         PE64:    {info['pe64']}")
        if "authenticode" in info:
            print(f"         Sign:    {info['authenticode']}")
        if "watermark_hits" in info:
            print(f"         WM hits: {len(info['watermark_hits'])}")
        for err in info["errors"]:
            print(f"         ERROR:   {err}", file=sys.stderr)

    if not all_ok:
        print("\n[FAIL] One or more upscaler plugins failed verification.", file=sys.stderr)
        return 1
    print("\n[PASS] All 4 vendor-signed retail x64 DLLs and 3 license notices verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
