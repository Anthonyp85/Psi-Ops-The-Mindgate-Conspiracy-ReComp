#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 2 automated gameplay & dual-platform verification suite for Psi-Ops.

Drives repeatable headless/scripted verification runs of Psi-Ops: The Mindgate
Conspiracy (Original Xbox static recompilation) on:
  - Windows x64 (out/build/release/PsiOps.exe, D3D12 + XAudio2)
  - Native Linux x86_64 via WSL2 Ubuntu 22.04 (out/build/linux-release/PsiOps,
    Vulkan 1.2 + SDL3 under xvfb-run -a / lavapipe)

All controller input is injected strictly inside the host runtime via
XBE_AUTOTEST_SCRIPT (never OS-level SendInput). Verifies process hygiene before
and after every run, hard timeouts (<= 120s), multi-checkpoint PNG screenshots
(non-black, valid dimensions, distinct frame hashes & pixel motion across 3D
gameplay), clean logs (zero crashes/exceptions/D3D12/Vulkan/glslang errors),
audio & Disc::BlockCache telemetry, 1.00x simulation timing, and source .xiso.iso
mtime_ns immutability.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

try:
    from PIL import Image  # type: ignore
except ImportError:
    Image = None

# Expected immutable source media constants (Phase 0 verified ground truth)
EXPECTED_ISO_REL = Path("Psi-Ops - The Mindgate Conspiracy (USA)") / "Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso"
EXPECTED_7Z_REL = Path("Psi-Ops - The Mindgate Conspiracy (USA).7z")

# Default wall-clock-indexed XBE_AUTOTEST_SCRIPT driving:
#   Boot FMVs -> Title Screen -> START -> Main Menu -> New Game + Difficulty ->
#   Intro Cutscene (verify playback & skip) -> Active 3D Gameplay (12s / 600+
#   frames at 60 FPS: walk/strafe + 360-deg camera look + jump/fire/psi) ->
#   Pause -> Resume -> clean exit.
# Uses ms elapsed since the first Present() so both 60 FPS Win64 D3D12 and
# software Vulkan (llvmpipe) stay locked to Psi-Ops's 1.00x real-time timeline.
DEFAULT_M2_SCRIPT = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops Milestone 2 scripted controller & multi-checkpoint capture timeline
# Syntax:
#   pad_ms <start_ms> <end_ms> <buttons_csv|none> [lx ly rx ry lt rt]
#   capture_ms <ms> <filename.bmp>
#   exit_ms <ms>

# 1. Boot FMVs complete ~20.5s; Shell/Title Screen settles by ~25s
capture_ms 26000 title.bmp

# 2. Press START at Title Screen -> Main Menu
pad_ms 26400 26800 START
capture_ms 28500 main_menu.bmp

# 3. Select New Game (A) -> Select Difficulty -> Confirm Normal (A) -> Intro Cutscene
pad_ms 29000 29400 A
pad_ms 30800 31200 A

# 4. Verify Intro Cutscene playback, then skip intro FMVs into Level 1
capture_ms 34200 cutscene.bmp
pad_ms 35000 35350 START
pad_ms 36400 36750 START
pad_ms 37800 38150 START
# Skip 8s Cell Intro FMV #4 cleanly into Level 1 Solitary Confinement
pad_ms 45500 45850 START

# 5. Dismiss Mission Objectives popup ("Accept (A)") once Solitary Confinement loads
pad_ms 50000 50350 A
pad_ms 51200 51550 A
pad_ms 52400 52750 A

# 6. Sustained Active 3D Gameplay checkpoint A (initial 3D cell doorway + Nick Scryer + HUD)
capture_ms 53500 gameplay_a.bmp

# 7. 15 seconds (900 frames @ 60 FPS; 650+ frames even on Linux llvmpipe) of
#    Active 3D Gameplay: walk out into lit corridor, strafe, 360-deg camera
#    look, jump, fire silenced pistol, psi powers, and frame corridor vista
pad_ms 53800 55500 none 0 26000 4000 0 0 0
pad_ms 55700 58100 RT 20000 16000 22000 -2000 0 255
pad_ms 58300 59700 A -20000 22000 24000 1500 0 0
pad_ms 59900 62300 LT,X 0 18000 -24000 1500 255 0
pad_ms 62500 64900 Y -14000 12000 -10000 0 0 0
pad_ms 65100 67700 none -12000 10000 -8000 0 0 0

# 8. Active 3D Gameplay checkpoint B (distinct hallway position, camera angle & ammo)
capture_ms 68500 gameplay_b.bmp

# 9. Open Pause Menu -> capture -> Resume -> capture -> clean exit
pad_ms 69100 69450 START
capture_ms 70500 pause.bmp
pad_ms 71100 71450 START
capture_ms 72500 resume.bmp
exit_ms 73500
"""

REQUIRED_CHECKPOINTS = ("title", "main_menu", "gameplay_a", "gameplay_b", "pause")
OPTIONAL_CHECKPOINTS = ("cutscene", "resume")

CRASH_PATTERNS = (
    re.compile(r"\bexception\s+[0-9A-Fa-f]{8}\b", re.IGNORECASE),
    re.compile(r"unhandled\s+exception", re.IGNORECASE),
    re.compile(r"\b(SIGSEGV|SIGABRT|SIGBUS|SIGILL|SIGFPE)\b"),
    re.compile(r"\bKeBugCheck\b"),
    re.compile(r"\bAssertion\s+failed\b", re.IGNORECASE),
    re.compile(r"\bFATAL\b"),
    re.compile(r"crash\s+dump\s+written", re.IGNORECASE),
)

SHADER_GPU_ERROR_PATTERNS = (
    re.compile(r"d3d12:.*\b(failed|error|device\s+removed)\b", re.IGNORECASE),
    re.compile(r"vulkan:.*\b(failed|error|VK_ERROR_DEVICE_LOST)\b", re.IGNORECASE),
    re.compile(r"glslang.*(error|failed)", re.IGNORECASE),
    re.compile(r"SPIR-?V.*(error|failed)", re.IGNORECASE),
    re.compile(r"CreateGraphicsPipelineState\s+failed", re.IGNORECASE),
    re.compile(r"vkCreateGraphicsPipelines\s+failed", re.IGNORECASE),
    re.compile(r"DXGI_ERROR_DEVICE_REMOVED|VK_ERROR_DEVICE_LOST", re.IGNORECASE),
    re.compile(r"shader\s+compile\s+failed", re.IGNORECASE),
)


@dataclass
class CheckpointResult:
    name: str
    path: str
    exists: bool = False
    width: int = 0
    height: int = 0
    mean_luma: float = 0.0
    non_black_pct: float = 0.0
    sha256: str = ""
    passed: bool = False
    reason: str = ""


@dataclass
class RunResult:
    platform: str
    run_index: int
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    log_path: str = ""
    perf_json_path: str = ""
    passed: bool = False
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    checkpoints: dict[str, dict[str, Any]] = field(default_factory=dict)
    diffs: dict[str, dict[str, Any]] = field(default_factory=dict)
    telemetry: dict[str, Any] = field(default_factory=dict)


def to_wsl_path(win_path: Path) -> str:
    resolved = win_path.resolve()
    drive = resolved.drive.rstrip(":").lower()
    if not drive:
        return resolved.as_posix()
    rel = resolved.as_posix().split(":", 1)[1].lstrip("/")
    return f"/mnt/{drive}/{rel}"


def snapshot_media_stats(repo_root: Path, iso_path: Path) -> dict[str, tuple[int, int]]:
    snap: dict[str, tuple[int, int]] = {}
    targets: list[Path] = [iso_path, repo_root / EXPECTED_7Z_REL]
    if iso_path.parent.is_dir():
        targets.extend(sorted(iso_path.parent.glob("*.txt")))
    for p in targets:
        if p.is_file():
            st = p.stat()
            snap[str(p)] = (st.st_size, st.st_mtime_ns)
    return snap


def assert_media_unchanged(before: dict[str, tuple[int, int]]) -> list[str]:
    errs: list[str] = []
    for path_s, (exp_sz, exp_mt) in before.items():
        p = Path(path_s)
        if not p.is_file():
            errs.append(f"Protected media missing after run: {path_s}")
            continue
        st = p.stat()
        if st.st_size != exp_sz or st.st_mtime_ns != exp_mt:
            errs.append(
                f"Protected media modified: {path_s} "
                f"(size {exp_sz}->{st.st_size}, mtime_ns {exp_mt}->{st.st_mtime_ns})"
            )
    return errs


def discover_wsl_ubuntu2204_distro(explicit: str | None) -> str:
    if explicit:
        return explicit
    env_distro = os.environ.get("PSIOPS_WSL_DISTRO", "").strip()
    if env_distro:
        return env_distro

    proc = subprocess.run(
        ["wsl", "-l", "-q"],
        capture_output=True,
        timeout=15,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"Failed to list WSL distros (rc={proc.returncode}). Pass --wsl-distro explicitly.")
    # wsl -l -q emits UTF-16LE on Windows
    raw = proc.stdout
    try:
        text = raw.decode("utf-16le") if b"\x00" in raw else raw.decode("utf-8", errors="replace")
    except UnicodeDecodeError:
        text = raw.decode("utf-8", errors="replace")

    distros = [line.strip().lstrip("\ufeff") for line in text.splitlines() if line.strip()]
    candidates = [d for d in distros if not d.lower().startswith("docker-desktop")]
    # Prefer a distro whose name mentions ubuntu / 2204, then verify /etc/os-release VERSION_ID=22.04
    candidates.sort(key=lambda d: (0 if ("2204" in d.lower() or "22.04" in d.lower()) else 1, 0 if "ubuntu" in d.lower() else 1, d.lower()))
    for d in candidates:
        probe = subprocess.run(
            ["wsl", "-d", d, "--", "cat", "/etc/os-release"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        if probe.returncode == 0 and 'VERSION_ID="22.04"' in probe.stdout:
            return d
    raise RuntimeError(
        f"No Ubuntu 22.04 WSL2 distro found among {distros!r}. Pass --wsl-distro <name> or set PSIOPS_WSL_DISTRO."
    )


def get_running_win_psiops() -> list[int]:
    if sys.platform != "win32":
        return []
    try:
        import psutil  # type: ignore

        pids: list[int] = []
        for p in psutil.process_iter(["pid", "name"]):
            try:
                if (p.info.get("name") or "").lower() == "psiops.exe":
                    pids.append(int(p.info["pid"]))
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return pids
    except Exception:
        pass
    proc = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq PsiOps.exe", "/FO", "CSV", "/NH"],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    pids = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("INFO:"):
            continue
        parts = [p.strip('"') for p in line.split(",")]
        if len(parts) >= 2 and parts[0].lower() == "psiops.exe":
            try:
                pids.append(int(parts[1]))
            except ValueError:
                pass
    return pids


def kill_win_psiops() -> None:
    if sys.platform != "win32":
        return
    subprocess.run(
        ["taskkill", "/F", "/T", "/IM", "PsiOps.exe"],
        capture_output=True,
        timeout=10,
        check=False,
    )


def get_running_wsl_psiops(distro: str) -> list[int]:
    cmd = ["pgrep", "-x", "PsiOps"] if sys.platform != "win32" else ["wsl", "-d", distro, "--", "pgrep", "-x", "PsiOps"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15, check=False)
    if proc.returncode != 0:
        return []
    pids: list[int] = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.isdigit():
            pids.append(int(line))
    return pids


def kill_wsl_psiops(distro: str) -> None:
    cmd = ["pkill", "-9", "-x", "PsiOps"] if sys.platform != "win32" else ["wsl", "-d", distro, "--", "pkill", "-9", "-x", "PsiOps"]
    subprocess.run(cmd, capture_output=True, timeout=15, check=False)


def _read_bmp_rgb_stdlib(bmp_path: Path) -> tuple[int, int, bytes]:
    data = bmp_path.read_bytes()
    if len(data) < 54 or data[0:2] != b"BM":
        raise ValueError(f"Not a valid BMP file: {bmp_path}")
    pixel_off = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if compression != 0 or bpp not in (24, 32):
        raise ValueError(f"Unsupported BMP format (bpp={bpp}, comp={compression}) in {bmp_path}")
    top_down = height < 0
    w, h = abs(width), abs(height)
    stride = ((w * (bpp // 8) + 3) // 4) * 4
    out = bytearray(w * h * 3)
    bytes_pp = bpp // 8
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        row_start = pixel_off + src_y * stride
        dst_row = y * w * 3
        for x in range(w):
            p = row_start + x * bytes_pp
            b, g, r = data[p], data[p + 1], data[p + 2]
            q = dst_row + x * 3
            out[q] = r
            out[q + 1] = g
            out[q + 2] = b
    return w, h, bytes(out)


def load_image_rgb(path: Path) -> tuple[int, int, bytes]:
    if Image is not None:
        with Image.open(path) as img:
            rgb = img.convert("RGB")
            return rgb.width, rgb.height, rgb.tobytes()
    if path.suffix.lower() == ".bmp":
        return _read_bmp_rgb_stdlib(path)
    raise RuntimeError("Pillow (PIL) is required to read PNG images.")


def ensure_png(capture_dir: Path, name: str) -> Path | None:
    png_path = capture_dir / f"{name}.png"
    bmp_path = capture_dir / f"{name}.bmp"
    if bmp_path.is_file() and Image is not None:
        with Image.open(bmp_path) as img:
            img.save(png_path, format="PNG")
        return png_path
    if png_path.is_file():
        return png_path
    if bmp_path.is_file():
        return bmp_path
    return None


def analyze_checkpoint(capture_dir: Path, name: str) -> tuple[CheckpointResult, tuple[int, int, bytes] | None]:
    img_path = ensure_png(capture_dir, name)
    res = CheckpointResult(name=name, path=str(img_path) if img_path else str(capture_dir / f"{name}.png"))
    if img_path is None or not img_path.is_file():
        res.reason = "missing capture file"
        return res, None

    res.exists = True
    res.sha256 = hashlib.sha256(img_path.read_bytes()).hexdigest()
    try:
        w, h, rgb = load_image_rgb(img_path)
    except Exception as exc:
        res.reason = f"failed to decode image: {exc}"
        return res, None

    res.width = w
    res.height = h
    if w < 320 or h < 240:
        res.reason = f"invalid dimensions {w}x{h} (< 320x240)"
        return res, (w, h, rgb)

    # Sample strided pixels (fast even on 2560x1440 without requiring numpy)
    total_px = w * h
    step = max(1, total_px // 200_000)
    luma_sum = 0.0
    non_black = 0
    sampled = 0
    for idx in range(0, total_px, step):
        off = idx * 3
        r, g, b = rgb[off], rgb[off + 1], rgb[off + 2]
        luma = 0.299 * r + 0.587 * g + 0.114 * b
        luma_sum += luma
        if luma > 12.0:
            non_black += 1
        sampled += 1

    res.mean_luma = round(luma_sum / max(1, sampled), 3)
    res.non_black_pct = round(100.0 * non_black / max(1, sampled), 3)

    if res.mean_luma < 2.0 or res.non_black_pct < 1.0:
        res.reason = f"frame appears black/blank (mean_luma={res.mean_luma}, non_black_pct={res.non_black_pct}%)"
        return res, (w, h, rgb)

    res.passed = True
    res.reason = "ok"
    return res, (w, h, rgb)


def compare_frames(
    img_a: tuple[int, int, bytes],
    img_b: tuple[int, int, bytes],
) -> dict[str, Any]:
    wa, ha, rgba = img_a
    wb, hb, rgbb = img_b
    if wa != wb or ha != hb:
        return {"same_size": False, "mean_abs_diff": 255.0, "changed_pct": 100.0}
    total_px = wa * ha
    step = max(1, total_px // 200_000)
    diff_sum = 0
    changed = 0
    sampled = 0
    for idx in range(0, total_px, step):
        off = idx * 3
        d = (
            abs(rgba[off] - rgbb[off])
            + abs(rgba[off + 1] - rgbb[off + 1])
            + abs(rgba[off + 2] - rgbb[off + 2])
        ) / 3.0
        diff_sum += d
        if d > 8.0:
            changed += 1
        sampled += 1
    return {
        "same_size": True,
        "mean_abs_diff": round(diff_sum / max(1, sampled), 3),
        "changed_pct": round(100.0 * changed / max(1, sampled), 3),
    }


def parse_log_and_perf(
    log_text: str,
    perf_json_path: Path,
    require_speed_ratio: bool,
    speed_min: float,
    speed_max: float,
) -> tuple[list[str], list[str], dict[str, Any]]:
    errors: list[str] = []
    warnings: list[str] = []
    telemetry: dict[str, Any] = {}

    for pat in CRASH_PATTERNS:
        m = pat.search(log_text)
        if m:
            errors.append(f"Crash/exception detected in log: {m.group(0)!r}")

    for pat in SHADER_GPU_ERROR_PATTERNS:
        m = pat.search(log_text)
        if m:
            errors.append(f"GPU/shader error detected in log: {m.group(0)!r}")

    if "Exit (0)" not in log_text:
        errors.append("Log did not record clean 'Exit (0)'.")

    if "Verified Retail" not in log_text:
        warnings.append("Log did not contain '(Verified Retail)' disc confirmation line.")

    has_audio_init = ("audio:" in log_text) or ("dsound:" in log_text)
    telemetry["log_audio_init"] = has_audio_init

    if perf_json_path.is_file():
        try:
            perf = json.loads(perf_json_path.read_text(encoding="utf-8"))
        except Exception as exc:
            errors.append(f"Failed to parse perf JSON ({perf_json_path}): {exc}")
            return errors, warnings, telemetry

        meta = perf.get("meta", {})
        phases = perf.get("phases", {})
        overall = phases.get("overall", {})
        gameplay = phases.get("gameplay_active", {})
        audio = perf.get("audio", {})
        io_stats = perf.get("io", {})
        shaders = perf.get("shaders", {})

        total_presents = int(meta.get("total_presents", 0))
        disc_reads = int(io_stats.get("disc_reads", 0))
        disc_bytes = int(io_stats.get("disc_bytes", 0))
        audio_blocks = int(audio.get("blocks_submitted", 0))
        audio_voices = int(audio.get("active_voices_peak", 0))
        audio_streams = int(audio.get("stream_packets_completed", 0))
        gp_frames = int(gameplay.get("frames", 0))
        gp_ratio = float(gameplay.get("g_elapsed_to_wall_ratio", 0.0))
        overall_ratio = float(overall.get("g_elapsed_to_wall_ratio", 0.0))
        active_ratio = gp_ratio if gp_ratio > 0.0 else overall_ratio

        telemetry.update(
            {
                "total_presents": total_presents,
                "wall_duration_s": meta.get("wall_duration_s", 0.0),
                "overall_avg_fps": overall.get("avg_fps", 0.0),
                "gameplay_frames": gp_frames,
                "gameplay_avg_fps": gameplay.get("avg_fps", 0.0),
                "gameplay_g_dt_ms_p50": gameplay.get("g_dt_ms_p50", 0.0),
                "gameplay_elapsed_to_wall_ratio": gp_ratio,
                "overall_elapsed_to_wall_ratio": overall_ratio,
                "disc_reads": disc_reads,
                "disc_bytes": disc_bytes,
                "audio_blocks_submitted": audio_blocks,
                "audio_active_voices_peak": audio_voices,
                "audio_stream_packets_completed": audio_streams,
                "shader_compile_count": shaders.get("compile_count", 0),
                "pso_create_count": shaders.get("pso_create_count", 0),
            }
        )

        if total_presents <= 0:
            errors.append("perf.json reports 0 presented frames.")
        if disc_reads <= 0 or disc_bytes <= 0:
            errors.append(f"No Disc::BlockCache / XISO read activity recorded (reads={disc_reads}, bytes={disc_bytes}).")
        if audio_blocks <= 0 and audio_voices <= 0 and audio_streams <= 0 and not has_audio_init:
            errors.append("No audio progression recorded in perf.json or logs.")

        if active_ratio > 0.0:
            if not (speed_min <= active_ratio <= speed_max):
                errors.append(
                    f"1.00x timing ratio out of bounds: g_elapsed_to_wall_ratio={active_ratio:.4f} "
                    f"(expected [{speed_min:.2f}, {speed_max:.2f}])."
                )
        elif require_speed_ratio:
            errors.append(
                "Timing telemetry missing: g_elapsed_to_wall_ratio is 0.0 "
                "(wire perf::SetPhaseCallback with guest dt/elapsed telemetry)."
            )
        else:
            warnings.append("g_elapsed_to_wall_ratio is 0.0 (phase callback not yet reporting guest elapsed time).")
    else:
        errors.append(f"Missing perf telemetry JSON: {perf_json_path}")

    return errors, warnings, telemetry


def run_win64(
    repo_root: Path,
    exe_path: Path,
    iso_path: Path,
    script_text: str,
    run_dir: Path,
    timeout_s: int,
    run_index: int,
    require_speed_ratio: bool,
    speed_min: float,
    speed_max: float,
    dry_run: bool,
) -> RunResult:
    res = RunResult(platform="win64", run_index=run_index)
    run_dir.mkdir(parents=True, exist_ok=True)
    script_path = run_dir / "autotest_script.txt"
    script_path.write_text(script_text, encoding="utf-8", newline="\n")
    perf_json = run_dir / "perf.json"
    res.perf_json_path = str(perf_json)
    log_file = exe_path.parent / "logs" / "psiops.log"
    saved_log = run_dir / "psiops.log"
    res.log_path = str(saved_log)

    if dry_run:
        res.exit_code = 0
        res.passed = True
        res.telemetry = {"dry_run": True, "exe": str(exe_path), "script": str(script_path)}
        return res

    if not exe_path.is_file():
        res.errors.append(f"Win64 executable not found: {exe_path}")
        return res

    pre_pids = get_running_win_psiops()
    if pre_pids:
        res.errors.append(f"Refusing to launch: PsiOps.exe already running (PIDs={pre_pids}).")
        return res

    media_before = snapshot_media_stats(repo_root, iso_path)
    env = os.environ.copy()
    env["XBE_AUTOTEST"] = "1"
    env["PSIOPS_AUTOTEST"] = "1"
    env["XBE_AUTOTEST_SCRIPT"] = str(script_path)
    env["XBE_AUTOTEST_OUT_DIR"] = str(run_dir)
    env["XBE_PERF_LOG"] = str(perf_json)
    env["XBE_ISO_PATH"] = str(iso_path)

    cmd = [
        str(exe_path),
        "--no-launcher",
        "--iso",
        str(iso_path),
        "--perf-log",
        str(perf_json),
    ]

    t0 = time.monotonic()
    stdout_s = ""
    stderr_s = ""
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(exe_path.parent),
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
        res.exit_code = proc.returncode
        stdout_s = proc.stdout or ""
        stderr_s = proc.stderr or ""
    except subprocess.TimeoutExpired as exc:
        res.timed_out = True
        res.errors.append(f"Win64 run exceeded hard timeout ({timeout_s}s).")
        if exc.stdout:
            stdout_s = exc.stdout if isinstance(exc.stdout, str) else exc.stdout.decode("utf-8", errors="replace")
        if exc.stderr:
            stderr_s = exc.stderr if isinstance(exc.stderr, str) else exc.stderr.decode("utf-8", errors="replace")
        kill_win_psiops()
    finally:
        res.wall_s = round(time.monotonic() - t0, 3)
        post_pids = get_running_win_psiops()
        if post_pids:
            kill_win_psiops()
            time.sleep(0.5)
            still = get_running_win_psiops()
            res.errors.append(f"Leftover PsiOps.exe process(es) detected after exit (initial={post_pids}, remaining={still}).")

    if log_file.is_file():
        shutil.copy2(log_file, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"Non-zero Win64 exit code: {res.exit_code}")

    _finalize_run_checks(res, run_dir, log_text, perf_json, media_before, require_speed_ratio, speed_min, speed_max)
    return res


def run_linux_wsl(
    repo_root: Path,
    exe_path: Path,
    iso_path: Path,
    distro: str,
    script_text: str,
    run_dir: Path,
    timeout_s: int,
    run_index: int,
    require_speed_ratio: bool,
    speed_min: float,
    speed_max: float,
    dry_run: bool,
) -> RunResult:
    res = RunResult(platform="linux", run_index=run_index)
    run_dir.mkdir(parents=True, exist_ok=True)
    script_path = run_dir / "autotest_script.txt"
    script_path.write_text(script_text, encoding="utf-8", newline="\n")
    perf_json = run_dir / "perf.json"
    res.perf_json_path = str(perf_json)
    log_file = exe_path.parent / "logs" / "psiops.log"
    saved_log = run_dir / "psiops.log"
    res.log_path = str(saved_log)

    if dry_run:
        res.exit_code = 0
        res.passed = True
        res.telemetry = {"dry_run": True, "wsl_distro": distro, "exe": str(exe_path), "script": str(script_path)}
        return res

    if not exe_path.is_file():
        res.errors.append(f"Linux executable not found: {exe_path}")
        return res

    pre_pids = get_running_wsl_psiops(distro)
    if pre_pids:
        res.errors.append(f"Refusing to launch: Linux PsiOps already running in {distro} (PIDs={pre_pids}).")
        return res

    media_before = snapshot_media_stats(repo_root, iso_path)
    wsl_exe = to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
    wsl_dir = to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
    wsl_iso = to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path)
    wsl_script = to_wsl_path(script_path) if sys.platform == "win32" else str(script_path)
    wsl_out = to_wsl_path(run_dir) if sys.platform == "win32" else str(run_dir)
    wsl_perf = to_wsl_path(perf_json) if sys.platform == "win32" else str(perf_json)

    inner_cmd = [
        "env",
        "XBE_AUTOTEST=1",
        "PSIOPS_AUTOTEST=1",
        f"XBE_AUTOTEST_SCRIPT={wsl_script}",
        f"XBE_AUTOTEST_OUT_DIR={wsl_out}",
        f"XBE_PERF_LOG={wsl_perf}",
        f"XBE_ISO_PATH={wsl_iso}",
        "SDL_AUDIODRIVER=dummy",
        "xvfb-run",
        "-a",
        "-s",
        "-screen 0 640x480x24",
        wsl_exe,
        "--no-launcher",
        "--iso",
        wsl_iso,
        "--perf-log",
        wsl_perf,
    ]
    if sys.platform == "win32":
        cmd = ["wsl", "-d", distro, "--cd", wsl_dir, "--"] + inner_cmd
    else:
        cmd = inner_cmd

    t0 = time.monotonic()
    stdout_s = ""
    stderr_s = ""
    try:
        proc = subprocess.run(
            cmd,
            cwd=None if sys.platform == "win32" else str(exe_path.parent),
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
        res.exit_code = proc.returncode
        stdout_s = proc.stdout or ""
        stderr_s = proc.stderr or ""
    except subprocess.TimeoutExpired as exc:
        res.timed_out = True
        res.errors.append(f"Linux Vulkan run exceeded hard timeout ({timeout_s}s).")
        if exc.stdout:
            stdout_s = exc.stdout if isinstance(exc.stdout, str) else exc.stdout.decode("utf-8", errors="replace")
        if exc.stderr:
            stderr_s = exc.stderr if isinstance(exc.stderr, str) else exc.stderr.decode("utf-8", errors="replace")
        kill_wsl_psiops(distro)
    finally:
        res.wall_s = round(time.monotonic() - t0, 3)
        post_pids = get_running_wsl_psiops(distro)
        if post_pids:
            kill_wsl_psiops(distro)
            time.sleep(0.5)
            still = get_running_wsl_psiops(distro)
            res.errors.append(f"Leftover Linux PsiOps process(es) detected after exit (initial={post_pids}, remaining={still}).")

    if log_file.is_file():
        shutil.copy2(log_file, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"Non-zero Linux exit code: {res.exit_code}")

    _finalize_run_checks(res, run_dir, log_text, perf_json, media_before, require_speed_ratio, speed_min, speed_max)
    return res


def _finalize_run_checks(
    res: RunResult,
    run_dir: Path,
    log_text: str,
    perf_json: Path,
    media_before: dict[str, tuple[int, int]],
    require_speed_ratio: bool,
    speed_min: float,
    speed_max: float,
) -> None:
    # 1. Media immutability
    res.errors.extend(assert_media_unchanged(media_before))

    # 2. Log + perf telemetry checks
    log_errs, log_warns, telemetry = parse_log_and_perf(
        log_text, perf_json, require_speed_ratio, speed_min, speed_max
    )
    res.errors.extend(log_errs)
    res.warnings.extend(log_warns)
    res.telemetry.update(telemetry)

    # 3. Multi-checkpoint PNG verification
    decoded: dict[str, tuple[int, int, bytes]] = {}
    for cp in REQUIRED_CHECKPOINTS + OPTIONAL_CHECKPOINTS:
        cp_res, img_data = analyze_checkpoint(run_dir, cp)
        res.checkpoints[cp] = asdict(cp_res)
        if img_data is not None:
            decoded[cp] = img_data
        if cp in REQUIRED_CHECKPOINTS and not cp_res.passed:
            res.errors.append(f"Required checkpoint '{cp}' failed: {cp_res.reason}")

    # 4. Cross-checkpoint distinctness & live 3D motion proof
    if "gameplay_a" in decoded and "gameplay_b" in decoded:
        diff_gp = compare_frames(decoded["gameplay_a"], decoded["gameplay_b"])
        res.diffs["gameplay_a_vs_b"] = diff_gp
        if res.checkpoints["gameplay_a"]["sha256"] == res.checkpoints["gameplay_b"]["sha256"]:
            res.errors.append("gameplay_a and gameplay_b have identical hashes (no 3D camera/player motion detected).")
        elif diff_gp["mean_abs_diff"] < 1.0 or diff_gp["changed_pct"] < 0.5:
            res.errors.append(
                f"gameplay_a vs gameplay_b shows insufficient 3D motion "
                f"(mean_abs_diff={diff_gp['mean_abs_diff']}, changed_pct={diff_gp['changed_pct']}%)."
            )

    if "main_menu" in decoded and "gameplay_a" in decoded:
        diff_mg = compare_frames(decoded["main_menu"], decoded["gameplay_a"])
        res.diffs["main_menu_vs_gameplay_a"] = diff_mg
        if res.checkpoints["main_menu"]["sha256"] == res.checkpoints["gameplay_a"]["sha256"]:
            res.errors.append("main_menu and gameplay_a are identical (game did not transition into 3D gameplay).")

    if "gameplay_b" in decoded and "pause" in decoded:
        diff_gp_pause = compare_frames(decoded["gameplay_b"], decoded["pause"])
        res.diffs["gameplay_b_vs_pause"] = diff_gp_pause
        if res.checkpoints["gameplay_b"]["sha256"] == res.checkpoints["pause"]["sha256"]:
            res.errors.append("gameplay_b and pause are identical (Pause menu did not open).")

    res.passed = len(res.errors) == 0


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Milestone 2 automated gameplay verification suite (Win64 D3D12 + Linux Vulkan 1.2)."
    )
    parser.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    parser.add_argument("--runs", type=int, default=1, help="Number of consecutive runs per platform (e.g. 2 for repeatability).")
    parser.add_argument("--timeout", type=int, default=120, help="Hard timeout in seconds per run (<= 120 recommended).")
    parser.add_argument("--iso", type=Path, default=repo_root / EXPECTED_ISO_REL, help="Read-only Psi-Ops .xiso.iso path.")
    parser.add_argument("--win-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    parser.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    parser.add_argument("--wsl-distro", type=str, default=None, help="WSL2 Ubuntu 22.04 distro name (auto-discovered if omitted).")
    parser.add_argument("--script", type=Path, default=None, help="Optional custom XBE_AUTOTEST_SCRIPT file.")
    parser.add_argument("--out-dir", type=Path, default=repo_root / ".local" / "m2_verify")
    parser.add_argument("--require-speed-ratio", action="store_true", help="Fail if g_elapsed_to_wall_ratio is 0.0.")
    parser.add_argument("--speed-min", type=float, default=0.85, help="Minimum acceptable g_elapsed_to_wall_ratio.")
    parser.add_argument("--speed-max", type=float, default=1.15, help="Maximum acceptable g_elapsed_to_wall_ratio.")
    parser.add_argument("--dry-run", action="store_true", help="Validate config, paths, and script generation without launching PsiOps.")
    args = parser.parse_args()

    if args.runs < 1:
        print("ERROR: --runs must be >= 1", file=sys.stderr)
        return 2
    if args.timeout <= 0 or args.timeout > 120:
        print(f"WARNING: --timeout {args.timeout}s is outside recommended (1..120s) range.", file=sys.stderr)
    if not args.iso.is_file():
        print(f"ERROR: XISO not found: {args.iso}", file=sys.stderr)
        return 2

    script_text = args.script.read_text(encoding="utf-8") if args.script else DEFAULT_M2_SCRIPT
    platforms = ["win64", "linux"] if args.platform == "both" else [args.platform]

    wsl_distro = ""
    if "linux" in platforms and sys.platform == "win32":
        try:
            wsl_distro = discover_wsl_ubuntu2204_distro(args.wsl_distro)
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    results: list[RunResult] = []
    all_passed = True

    for plat in platforms:
        for run_idx in range(1, args.runs + 1):
            run_dir = args.out_dir / f"{plat}_run{run_idx}"
            print(f"[test_m2_gameplay] Starting {plat} run {run_idx}/{args.runs} (timeout={args.timeout}s)...")
            if plat == "win64":
                r = run_win64(
                    repo_root,
                    args.win_exe,
                    args.iso,
                    script_text,
                    run_dir,
                    args.timeout,
                    run_idx,
                    args.require_speed_ratio,
                    args.speed_min,
                    args.speed_max,
                    args.dry_run,
                )
            else:
                r = run_linux_wsl(
                    repo_root,
                    args.linux_exe,
                    args.iso,
                    wsl_distro,
                    script_text,
                    run_dir,
                    args.timeout,
                    run_idx,
                    args.require_speed_ratio,
                    args.speed_min,
                    args.speed_max,
                    args.dry_run,
                )
            results.append(r)
            status = "PASS" if r.passed else "FAIL"
            print(f"[test_m2_gameplay] {plat} run {run_idx}: {status} (exit={r.exit_code}, wall={r.wall_s}s)")
            for w in r.warnings:
                print(f"  WARN: {w}")
            for e in r.errors:
                print(f"  FAIL: {e}")
            if not r.passed:
                all_passed = False

    report = {
        "iso": str(args.iso),
        "platforms": platforms,
        "runs_per_platform": args.runs,
        "all_passed": all_passed,
        "results": [asdict(r) for r in results],
    }
    report_path = args.out_dir / "m2_verify_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"[test_m2_gameplay] Report written to: {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
