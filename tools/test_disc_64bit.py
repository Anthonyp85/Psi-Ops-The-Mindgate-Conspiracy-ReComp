#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only 64-bit (>2 GiB / INT32_MAX) XDVDFS offset verification test.

Traverses the Psi-Ops XISO strictly read-only, identifies every file whose byte
range crosses or exceeds 0x7FFFFFFF (2 GiB), verifies the 2 GiB boundary straddle,
required boot FMVs (Legal.sfd, LOGO.sfd, Psi_logo.sfd), and the highest-offset
archive entry (0xA4EC7800..0xA6F3D800) by XDVDFS offset/index, asserting valid
MPEG-PS pack headers (00 00 01 BA) and readable tail sectors.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR_SIZE = 2048
INT32_MAX = 0x7FFF_FFFF
TWO_GIB = 0x8000_0000
MPEG_PS_PACK_START = b"\x00\x00\x01\xBA"
CANDIDATE_BASES = (0x0, 0x18300000)


@dataclass(frozen=True)
class DiscEntry:
    path: str
    sector: int
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


def find_default_iso(repo_root: Path) -> Path:
    default_iso = (
        repo_root
        / "Psi-Ops - The Mindgate Conspiracy (USA)"
        / "Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso"
    )
    if default_iso.is_file():
        return default_iso
    raise FileNotFoundError(f"Default XISO not found at {default_iso}; pass --iso <path>.")


def detect_xdvdfs_base(f) -> tuple[int, int, int]:
    for base in CANDIDATE_BASES:
        f.seek(base + 32 * SECTOR_SIZE)
        hdr = f.read(SECTOR_SIZE)
        if len(hdr) == SECTOR_SIZE and hdr[0:20] == XDVDFS_MAGIC and hdr[0x7EC:0x800] == XDVDFS_MAGIC:
            root_sector, root_size = struct.unpack_from("<II", hdr, 20)
            return base, root_sector, root_size
    raise ValueError("Invalid XDVDFS header.")


def walk_xdvdfs(f, base: int, dir_sector: int, dir_size: int, prefix: str = "") -> list[DiscEntry]:
    entries: list[DiscEntry] = []
    if dir_size == 0:
        return entries
    f.seek(base + dir_sector * SECTOR_SIZE)
    table = f.read(dir_size)
    if len(table) != dir_size:
        raise ValueError(f"Truncated directory table at sector {dir_sector}")

    stack = [0]
    visited: set[int] = set()
    while stack:
        off = stack.pop()
        if off in visited or off + 14 > len(table):
            continue
        visited.add(off)
        left, right, sector, size, attrs, name_len = struct.unpack_from("<HHIIBB", table, off)
        name = table[off + 14 : off + 14 + name_len].decode("ascii", errors="replace")
        rel_path = f"{prefix}/{name}" if prefix else name
        if attrs & 0x10:
            entries.extend(walk_xdvdfs(f, base, sector, size, rel_path))
        else:
            entries.append(DiscEntry(rel_path, sector, base + sector * SECTOR_SIZE, size))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return entries


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Verify 64-bit (>2 GiB) read-only XDVDFS access.")
    parser.add_argument("--iso", type=Path, default=None, help="Path to Psi-Ops .xiso.iso (opened strictly read-only).")
    args = parser.parse_args()

    iso_path = args.iso if args.iso is not None else find_default_iso(repo_root)
    iso_stat_before = iso_path.stat()

    with iso_path.open("rb") as f:
        base, root_sector, root_size = detect_xdvdfs_base(f)
        all_entries = walk_xdvdfs(f, base, root_sector, root_size)
        over_2g = sorted((e for e in all_entries if e.end > INT32_MAX), key=lambda e: e.offset)

        print(f"[test_disc_64bit] ISO: {iso_path} (size={iso_stat_before.st_size:,} B, xdvdfs_base=0x{base:X})")
        print(f"[test_disc_64bit] Total files={len(all_entries)}, files exceeding 0x7FFFFFFF (2 GiB)={len(over_2g)}")

        if len(over_2g) != 21:
            print(f"ERROR: Expected 21 files >2 GiB, found {len(over_2g)}", file=sys.stderr)
            return 1

        # Verify 2 GiB boundary straddle entry (0x7E155800..0x80310000)
        straddlers = [e for e in over_2g if e.offset < TWO_GIB < e.end]
        if len(straddlers) != 1 or straddlers[0].offset != 0x7E155800 or straddlers[0].end != 0x80310000:
            print(f"ERROR: Unexpected 2 GiB straddle entry: {straddlers}", file=sys.stderr)
            return 1

        # Verify required boot FMVs are present above 2 GiB
        by_lower = {e.path.lower(): e for e in over_2g}
        for boot_sfd in ("data/movies/legal.sfd", "data/movies/logo.sfd", "data/movies/psi_logo.sfd"):
            if boot_sfd not in by_lower:
                print(f"ERROR: Expected boot FMV above 2 GiB missing: {boot_sfd}", file=sys.stderr)
                return 1

        # Verify highest-offset entry by offset (0xA4EC7800..0xA6F3D800)
        highest = over_2g[-1]
        if highest.offset != 0xA4EC7800 or highest.end != 0xA6F3D800:
            print(
                f"ERROR: Unexpected highest-offset entry range: 0x{highest.offset:X}..0x{highest.end:X}",
                file=sys.stderr,
            )
            return 1

        # Read-verify MPEG-PS pack header and tail sector on all 21 >2 GiB .sfd entries
        for idx, entry in enumerate(over_2g, start=1):
            if not entry.path.lower().endswith(".sfd"):
                print(f"ERROR: Non-.sfd entry above 2 GiB: #{idx} offset=0x{entry.offset:X}", file=sys.stderr)
                return 1
            f.seek(entry.offset)
            head = f.read(16)
            if len(head) != 16 or head[:4] != MPEG_PS_PACK_START:
                print(
                    f"ERROR: Entry #{idx} (0x{entry.offset:X}) bad MPEG-PS magic: {head[:4].hex()}",
                    file=sys.stderr,
                )
                return 1

            # For the boundary straddle file, also read across 0x7FFFF800..0x80000800
            if entry.offset < TWO_GIB < entry.end:
                f.seek(TWO_GIB - 2048)
                bridge = f.read(4096)
                if len(bridge) != 4096:
                    print("ERROR: Failed reading 4 KiB window across 2 GiB boundary.", file=sys.stderr)
                    return 1

            tail_off = entry.end - min(SECTOR_SIZE, entry.size)
            f.seek(tail_off)
            tail = f.read(min(SECTOR_SIZE, entry.size))
            if len(tail) != min(SECTOR_SIZE, entry.size):
                print(f"ERROR: Short tail read on entry #{idx} at 0x{tail_off:X}", file=sys.stderr)
                return 1

        print(
            f"[test_disc_64bit] PASS: All {len(over_2g)} >2 GiB .sfd entries verified "
            f"(straddle=0x{straddlers[0].offset:X}..0x{straddlers[0].end:X}, "
            f"boot FMVs Legal/LOGO/Psi_logo OK, max=0x{highest.offset:X}..0x{highest.end:X}, "
            f"all MPEG-PS 00 00 01 BA + tail reads OK)."
        )

    iso_stat_after = iso_path.stat()
    if iso_stat_before.st_mtime_ns != iso_stat_after.st_mtime_ns or iso_stat_before.st_size != iso_stat_after.st_size:
        print("ERROR: ISO stat changed during read-only test!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
