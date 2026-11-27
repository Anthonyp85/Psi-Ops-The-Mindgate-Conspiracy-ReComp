#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only XDVDFS extractor for Psi-Ops: The Mindgate Conspiracy.

Reads default.xbe strictly read-only from the local .xiso.iso (supporting both
raw XISO and full-DVD game partitions). The default mode verifies the USA
executable for a local build. --pal extracts a known PAL executable to a
separate location for porting work.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import tempfile
from pathlib import Path

XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR_SIZE = 2048
CANDIDATE_BASES = (0x0, 0x18300000)

EXPECTED_XBE_SIZE = 5_672_960
EXPECTED_XBE_SHA256 = "2241d4c515479c525752800556322ce1055464c78198b76e364c27dca30205be"
EXPECTED_XBE_SHA1 = "42dd95fa444039b82f843ff02a31833da97d41cf"
# Retail XBE checksums published by MobCat's Xbox Title ID Database.
PAL_REVISIONS = {
    "457278d333f3d2e3c262f575085107d9": "Europe (MW02202E)",
    "d4763c9dfaeea8ab09704075b85a653c": "Germany (MW02203E)",
}


def sha256_sha1_file(path: Path) -> tuple[int, str, str]:
    h256 = hashlib.sha256()
    h1 = hashlib.sha1()
    total = 0
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h256.update(chunk)
            h1.update(chunk)
            total += len(chunk)
    return total, h256.hexdigest(), h1.hexdigest()


def find_default_iso(repo_root: Path) -> Path:
    candidates = [
        repo_root / "Psi-Ops - The Mindgate Conspiracy (USA)" / "Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso",
        repo_root / "assets" / "Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso",
    ]
    for p in candidates:
        if p.is_file():
            return p
    for sub in sorted(repo_root.iterdir()):
        if sub.is_dir() and sub.name not in {".git", ".local", "out", "build", "XboxXBERecomp"}:
            for item in sorted(sub.iterdir()):
                if item.is_file() and item.name.lower().endswith((".xiso.iso", ".iso")):
                    return item
    raise FileNotFoundError(
        "Could not locate Psi-Ops .xiso.iso. Pass --iso <path> explicitly."
    )


def detect_xdvdfs_base(f) -> tuple[int, int, int]:
    for base in CANDIDATE_BASES:
        f.seek(base + 32 * SECTOR_SIZE)
        hdr = f.read(SECTOR_SIZE)
        if len(hdr) == SECTOR_SIZE and hdr[0:20] == XDVDFS_MAGIC and hdr[0x7EC:0x800] == XDVDFS_MAGIC:
            root_sector, root_size = struct.unpack_from("<II", hdr, 20)
            return base, root_sector, root_size
    raise ValueError("Input file is not a valid XDVDFS image (MICROSOFT*XBOX*MEDIA magic not found).")


def find_root_entry(f, base: int, root_sector: int, root_size: int, target_name: str) -> tuple[int, int, int]:
    f.seek(base + root_sector * SECTOR_SIZE)
    table = f.read(root_size)
    if len(table) != root_size:
        raise ValueError("Truncated XDVDFS root directory table.")

    target_lower = target_name.lower()
    stack = [0]
    visited: set[int] = set()
    while stack:
        off = stack.pop()
        if off in visited or off + 14 > len(table):
            continue
        visited.add(off)
        left, right, sector, size, attrs, name_len = struct.unpack_from("<HHIIBB", table, off)
        name_bytes = table[off + 14 : off + 14 + name_len]
        name = name_bytes.decode("ascii", errors="replace")
        if name.lower() == target_lower and not (attrs & 0x10):
            return sector, size, base + sector * SECTOR_SIZE
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    raise FileNotFoundError(f"'{target_name}' not found in XDVDFS root directory.")


def extract_xbe(iso_path: Path, out_path: Path, *, pal: bool, force: bool) -> None:
    with iso_path.open("rb") as f:
        base, root_sector, root_size = detect_xdvdfs_base(f)
        sector, size, byte_off = find_root_entry(f, base, root_sector, root_size, "default.xbe")
        if not pal and size != EXPECTED_XBE_SIZE:
            raise ValueError(f"Unexpected default.xbe size: got {size}, expected {EXPECTED_XBE_SIZE} for USA retail.")
        f.seek(0, os.SEEK_END)
        if byte_off + size > f.tell():
            raise ValueError("Truncated default.xbe in XDVDFS image.")
        f.seek(byte_off)
        data = f.read(size)
        if len(data) != size:
            raise ValueError(f"Short read extracting default.xbe: got {len(data)}, expected {size}")

    sha256 = hashlib.sha256(data).hexdigest()
    sha1 = hashlib.sha1(data).hexdigest()
    if pal:
        md5 = hashlib.md5(data, usedforsecurity=False).hexdigest()
        revision = PAL_REVISIONS.get(md5)
        if revision is None:
            raise ValueError(f"Unrecognized PAL default.xbe (MD5 {md5}, SHA-1 {sha1}).")
    else:
        if sha256 != EXPECTED_XBE_SHA256 or sha1 != EXPECTED_XBE_SHA1:
            raise ValueError(f"Unrecognized USA retail default.xbe (SHA-256 {sha256}, SHA-1 {sha1}).")
        revision = "USA (MW02201A)"

    if out_path.resolve() == iso_path.resolve():
        raise ValueError("The output must be separate from the input disc image.")
    if not force and out_path.is_file() and sha256_sha1_file(out_path) == (size, sha256, sha1):
        print(f"[extract_xbe] Up to date: {out_path}")
    else:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_f = tempfile.NamedTemporaryFile(dir=out_path.parent, delete=False)
        tmp_path = Path(out_f.name)
        try:
            with out_f:
                out_f.write(data)
            os.replace(tmp_path, out_path)
        finally:
            tmp_path.unlink(missing_ok=True)
        print(f"[extract_xbe] Extracted default.xbe read-only from: {iso_path}")
        print(f"              xdvdfs_base=0x{base:X} sector={sector} offset=0x{byte_off:X} size={size}")
        print(f"              out={out_path}")
    print(f"              revision={revision}")
    print(f"              sha256={sha256}")
    print(f"              sha1={sha1}")


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Read-only idempotent XDVDFS default.xbe extractor.")
    parser.add_argument("--iso", type=Path, help="Path to a Psi-Ops XISO or ISO (opened strictly read-only).")
    parser.add_argument("--pal", action="store_true", help="Extract a PAL executable for porting. Requires --iso.")
    parser.add_argument(
        "--out",
        type=Path,
        help="Output path (defaults to .local/xbe/default.xbe, or .local/xbe/pal/default.xbe with --pal).",
    )
    parser.add_argument("--force", action="store_true", help="Re-extract even if output already matches SHA-256.")
    args = parser.parse_args()

    if args.pal and args.iso is None:
        parser.error("--pal requires --iso")
    usa_out = repo_root / ".local" / "xbe" / "default.xbe"
    out_path = args.out or (usa_out.parent / "pal" / "default.xbe" if args.pal else usa_out)
    if args.pal and out_path.resolve() in {usa_out.resolve(), (repo_root / "assets" / "default.xbe").resolve()}:
        parser.error("--pal requires a separate output path from the USA build inputs")

    if args.iso is None and out_path.is_file() and not args.force:
        size, sha256, sha1 = sha256_sha1_file(out_path)
        if size == EXPECTED_XBE_SIZE and sha256 == EXPECTED_XBE_SHA256 and sha1 == EXPECTED_XBE_SHA1:
            print(f"[extract_xbe] Up to date (idempotent no-op): {out_path}")
            print(f"              size={size} sha256={sha256} sha1={sha1}")
            return 0

    try:
        iso_path = args.iso if args.iso is not None else find_default_iso(repo_root)
        extract_xbe(iso_path, out_path, pal=args.pal, force=args.force)
    except (OSError, ValueError) as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
