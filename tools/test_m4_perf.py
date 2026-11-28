#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 4 cross-hardware performance, 60/30 FPS fidelity, multi-GPU & pacing suite.

Supports:
  --mode pre|post|full|compare
  --platform win64|linux|both
  --out-dir .local/m4_perf

Measures and validates:
  1. All 11 mandatory performance metrics across BOTH gameplay_active and fmv/cutscene
     phases (cold-cache and warm-cache runs using identical deterministic workloads):
       (1) avg_fps, (2) p50_ms, (3) fps_1pct_low, (4) p99_ms, (5) p99_9_ms & max_ms,
       (6) CPU/GPU workload, (7) render/lock stalls & mid_frame_flushes == 0,
       (8) shader/PSO compile stalls, boot_prewarm_ms & sync_present_cache_writes == 0,
       (9) audio sync/underruns == 0 & glitches == 0,
       (10) XISO / Disc::BlockCache hit rate, OS disc reads & I/O latencies,
       (11) working set / commit peaks, 32 MiB GPU texture slabs & upload ring cap.
  2. 60 FPS vs 30 FPS 1.00x simulation fidelity (g_elapsed_to_wall_ratio in [0.97, 1.03]
     and matching world position / physics state at equal sim_elapsed_s).
  3. Multi-GPU & UMA scaling: PE exports (NvOptimusEnablement &
     AmdPowerXpressRequestHighPerformance via pefile), --list-gpus, live Win64 runs on
     GPU 0 (RTX 3090, 64 MiB ring) and GPU 1 (AMD Radeon UMA, 16/32 MiB ring), per-VID/DID
     PSO & metadata cache files on D3D12 & Linux Vulkan 1.2, and 32 MiB slab stats.
  4. VRR / VSync pacing matrix across 60, 90, 120, 144, 165 Hz (Auto, VSync, VRR, Mailbox)
     and RAM-bounded PrepareGameCode (~768 MiB/slot across 4/8/16/64 GB + --compile-jobs).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

try:
    import pefile  # type: ignore
except ImportError:
    pefile = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_m2_gameplay as m2  # noqa: E402

REFRESH_MATRIX_HZ = (60, 90, 120, 144, 165)
PRESENT_MODES = ("auto", "vsync", "vrr", "mailbox")
RAM_BUDGETS_MB = (4096, 8192, 16384, 65536)

PREWARM_LOG_RE = re.compile(r"startup prewarm done in\s+([0-9]+(?:\.[0-9]+)?)\s*ms", re.IGNORECASE)
UPLOAD_LOG_RE = re.compile(r"upload=(\d+)\s*MiB", re.IGNORECASE)
ADAPTER_LOG_RE = re.compile(r"(?:D3D12|Vulkan) adapter:\s*(.+?)\s*\(VID=0x([0-9A-Fa-f]{4,8})\s+DID=0x([0-9A-Fa-f]{4,8})", re.IGNORECASE)


@dataclass
class ElevenMetrics:
    # 1..5: Frame rate & frame-time percentiles
    frames: int = 0
    avg_fps: float = 0.0
    p50_ms: float = 0.0
    fps_1pct_low: float = 0.0
    p99_ms: float = 0.0
    p99_9_ms: float = 0.0
    max_ms: float = 0.0
    # 6: CPU / GPU workload
    guest_cpu_ms: float = 0.0
    render_cpu_ms: float = 0.0
    gpu_ms: float = 0.0
    process_cpu_pct: float = 0.0
    # 7: Render / lock stalls
    lock_stalls: int = 0
    mid_frame_flushes: int = 0
    # 8: Shader / PSO compile stalls & async cache
    compile_count: int = 0
    compile_ms_total: float = 0.0
    pso_create_count: int = 0
    pso_create_ms_total: float = 0.0
    boot_prewarm_ms: float = 0.0
    sync_present_cache_writes: int = 0
    # 9: Audio sync / underruns
    audio_underruns: int = 0
    audio_glitches: int = 0
    audio_blocks: int = 0
    av_sync_ratio: float = 0.0
    # 10: XISO / Disc::BlockCache I/O
    disc_cache_hit_pct: float = 0.0
    disc_reads: int = 0
    disc_os_reads: int = 0
    io_avg_us: float = 0.0
    io_p99_us: float = 0.0
    io_max_us: float = 0.0
    # 11: Memory & GPU allocation spikes
    working_set_peak_mb: float = 0.0
    commit_peak_mb: float = 0.0
    gpu_slab_count: int = 0
    gpu_slab_mb: float = 0.0
    upload_ring_mb: float = 0.0
    upload_overflows: int = 0
    # Simulation speed ratio
    g_elapsed_to_wall_ratio: float = 0.0
    # Zero-gated counters absent from perf.json (a missing key must fail, never read as a passing 0)
    missing_keys: list[str] = field(default_factory=list)


@dataclass
class BenchmarkRunResult:
    name: str
    platform: str
    cache_state: str
    gpu_index: int | None = None
    fps_cap: int = 60
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    passed: bool = False
    adapter_name: str = ""
    adapter_vid: str = ""
    adapter_did: str = ""
    log_path: str = ""
    perf_json_path: str = ""
    state_log_path: str = ""
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    metrics_overall: dict[str, Any] = field(default_factory=dict)
    metrics_gameplay: dict[str, Any] = field(default_factory=dict)
    metrics_fmv: dict[str, Any] = field(default_factory=dict)


def _required_counter(section: dict[str, Any], section_name: str, keys: tuple[str, ...], missing: list[str]) -> int:
    """Read a zero-gated counter; record it as missing instead of silently defaulting to 0."""
    for key in keys:
        if key in section:
            return int(section[key])
    missing.append(f"{section_name}.{keys[0]}")
    return 0


def extract_eleven_metrics(perf: dict[str, Any], phase_key: str, log_text: str = "") -> ElevenMetrics:
    meta = perf.get("meta", {})
    phase = perf.get("phases", {}).get(phase_key, {})
    workload = perf.get("workload", {})
    stalls = perf.get("stalls", {})
    shaders = perf.get("shaders", {})
    audio = perf.get("audio", {})
    io_s = perf.get("io", {})
    mem = perf.get("memory", {})

    m = ElevenMetrics()
    m.frames = int(phase.get("frames", 0))
    m.avg_fps = float(phase.get("avg_fps", 0.0))
    m.p50_ms = float(phase.get("p50_ms", 0.0))
    m.fps_1pct_low = float(phase.get("fps_1pct_low_mean", 0.0))
    m.p99_ms = float(phase.get("p99_ms", 0.0))
    m.p99_9_ms = float(phase.get("p99_9_ms", 0.0))
    m.max_ms = float(phase.get("max_ms", 0.0))

    m.guest_cpu_ms = float(phase.get("guest_work_ms_avg", 0.0))
    m.render_cpu_ms = float(phase.get("render_host_ms_avg", 0.0))
    m.gpu_ms = float(phase.get("gpu_ms_avg", 0.0))
    m.process_cpu_pct = float(workload.get("process_cpu_pct_avg", 0.0))

    missing = m.missing_keys
    m.lock_stalls = _required_counter(stalls, "stalls", ("lock_stalls", "render_waits_total"), missing)
    m.mid_frame_flushes = _required_counter(stalls, "stalls", ("mid_frame_flushes",), missing)

    m.compile_count = _required_counter(shaders, "shaders", ("compile_count",), missing)
    m.compile_ms_total = float(shaders.get("compile_ms_total", 0.0))
    m.pso_create_count = _required_counter(shaders, "shaders", ("pso_create_count",), missing)
    m.pso_create_ms_total = float(shaders.get("pso_create_ms_total", 0.0))
    prewarm_ms = float(shaders.get("boot_prewarm_ms", 0.0))
    if prewarm_ms <= 0.0 and log_text:
        pm = PREWARM_LOG_RE.search(log_text)
        if pm:
            prewarm_ms = float(pm.group(1))
    m.boot_prewarm_ms = round(prewarm_ms, 3)
    m.sync_present_cache_writes = _required_counter(shaders, "shaders", ("sync_present_cache_writes",), missing)

    m.audio_underruns = _required_counter(audio, "audio", ("underruns", "xaudio2_underruns"), missing) + int(
        audio.get("stream_underruns", 0)
    )
    m.audio_glitches = _required_counter(audio, "audio", ("glitches", "xaudio2_glitches"), missing)
    m.audio_blocks = int(audio.get("blocks_submitted", 0))
    wall_s = float(meta.get("wall_duration_s", 0.0))
    if "av_sync_ratio" in audio:
        m.av_sync_ratio = float(audio["av_sync_ratio"])
    elif wall_s > 0.0 and m.audio_blocks > 0:
        m.av_sync_ratio = round((m.audio_blocks * 0.01) / wall_s, 4)

    m.disc_reads = int(io_s.get("disc_reads", 0))
    m.disc_os_reads = int(io_s.get("disc_os_reads", m.disc_reads))
    m.disc_cache_hit_pct = float(io_s.get("disc_cache_hit_pct", 0.0))
    if "io_avg_us" in io_s:
        m.io_avg_us = float(io_s["io_avg_us"])
    elif m.disc_reads > 0:
        m.io_avg_us = round((float(io_s.get("disc_read_ms_total", 0.0)) * 1000.0) / m.disc_reads, 2)
    m.io_p99_us = float(io_s.get("io_p99_us", 0.0))
    if "io_max_us" in io_s:
        m.io_max_us = float(io_s["io_max_us"])
    else:
        m.io_max_us = round(float(io_s.get("disc_read_ms_max", 0.0)) * 1000.0, 2)

    m.working_set_peak_mb = float(mem.get("working_set_peak_mb", mem.get("host_ws_peak_mb", 0.0)))
    m.commit_peak_mb = float(mem.get("commit_peak_mb", mem.get("host_private_peak_mb", 0.0)))
    m.gpu_slab_count = int(mem.get("gpu_slab_count", 0))
    m.gpu_slab_mb = float(mem.get("gpu_slab_mb", 0.0))
    ring_mb = float(mem.get("upload_ring_mb", mem.get("upload_ring_cap_mb", mem.get("d3d12_upload_ring_cap_mb", 0.0))))
    if ring_mb <= 0.0 and log_text:
        um = UPLOAD_LOG_RE.search(log_text)
        if um:
            ring_mb = float(um.group(1))
    m.upload_ring_mb = ring_mb
    m.upload_overflows = _required_counter(mem, "memory", ("upload_overflows", "d3d12_upload_overflows"), missing)
    m.g_elapsed_to_wall_ratio = float(phase.get("g_elapsed_to_wall_ratio", 0.0))
    return m


def check_pe_gpu_exports(exe_path: Path) -> tuple[bool, dict[str, Any]]:
    info: dict[str, Any] = {"exe": str(exe_path), "NvOptimusEnablement": False, "AmdPowerXpressRequestHighPerformance": False}
    if not exe_path.is_file():
        info["error"] = f"PE file not found: {exe_path}"
        return False, info
    if pefile is None:
        info["error"] = "pefile module not installed"
        return False, info

    pe = pefile.PE(str(exe_path), fast_load=True)
    try:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        exp_dir = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        names: set[str] = set()
        if exp_dir is not None:
            for sym in exp_dir.symbols:
                if sym.name:
                    names.add(sym.name.decode("utf-8", errors="replace"))
        info["NvOptimusEnablement"] = "NvOptimusEnablement" in names
        info["AmdPowerXpressRequestHighPerformance"] = "AmdPowerXpressRequestHighPerformance" in names
        info["export_count"] = len(names)
    finally:
        pe.close()

    ok = info["NvOptimusEnablement"] and info["AmdPowerXpressRequestHighPerformance"]
    return ok, info


def parse_list_gpus_output(stdout_text: str) -> list[dict[str, Any]]:
    text = stdout_text.strip()
    if not text:
        return []
    # Prefer JSON output if --list-gpus emits JSON array or JSONL
    try:
        parsed = json.loads(text)
        if isinstance(parsed, list):
            return [x for x in parsed if isinstance(x, dict)]
        if isinstance(parsed, dict) and isinstance(parsed.get("gpus"), list):
            return parsed["gpus"]
    except Exception:
        pass

    gpus: list[dict[str, Any]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        m_line = re.search(r"\[(\d+)\]\s*(.+?)\s*\(VID=0x([0-9A-Fa-f]{4})\s+DID=0x([0-9A-Fa-f]{4})", line)
        if m_line:
            gpus.append(
                {
                    "index": int(m_line.group(1)),
                    "name": m_line.group(2).strip(),
                    "vid": m_line.group(3).upper(),
                    "did": m_line.group(4).upper(),
                    "uma": "uma=true" in line.lower(),
                }
            )
    return gpus


def run_benchmark_case(
    name: str,
    platform: str,
    cache_state: str,
    exe_path: Path,
    iso_path: Path,
    repo_root: Path,
    run_dir: Path,
    workload_kind: str,
    workload_path: Path,
    fps_cap: int,
    gpu_index: int | None,
    present_mode: str | None,
    extra_cli: list[str],
    extra_env: dict[str, str],
    timeout_s: int,
    wsl_distro: str,
    strict_post: bool,
    dry_run: bool,
    resume: bool = False,
) -> BenchmarkRunResult:
    res = BenchmarkRunResult(
        name=name,
        platform=platform,
        cache_state=cache_state,
        gpu_index=gpu_index,
        fps_cap=fps_cap,
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    perf_json = run_dir / "perf.json"
    state_log = run_dir / "state_log.jsonl"
    saved_log = run_dir / "psiops.log"
    res.perf_json_path = str(perf_json)
    res.state_log_path = str(state_log)
    res.log_path = str(saved_log)

    if dry_run:
        res.exit_code = 0
        res.passed = True
        return res

    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res

    media_before = m2.snapshot_media_stats(repo_root, iso_path)
    logs_dir = exe_path.parent / "logs"
    log_file = logs_dir / "psiops.log"
    logs_before: dict[Path, int] = (
        {p: p.stat().st_mtime_ns for p in logs_dir.glob("psiops*.log") if p.is_file()}
        if logs_dir.is_dir()
        else {}
    )

    # When --resume is passed, reuse an existing clean run newer than the target binary.
    if resume and perf_json.is_file() and saved_log.is_file() and state_log.is_file():
        exe_mtime = exe_path.stat().st_mtime
        if perf_json.stat().st_mtime > exe_mtime and saved_log.stat().st_mtime > exe_mtime:
            prev_log = saved_log.read_text(encoding="utf-8", errors="replace")
            if "Exit (0)" in prev_log:
                res.exit_code = 0
                perf_meta = json.loads(perf_json.read_text(encoding="utf-8")).get("meta", {})
                res.wall_s = round(float(perf_meta.get("wall_duration_s", 0.0)), 3)
                log_text = prev_log
                am = ADAPTER_LOG_RE.search(log_text)
                if am:
                    res.adapter_name = am.group(1).strip()
                    res.adapter_vid = am.group(2).upper()
                    res.adapter_did = am.group(3).upper()
                for pat in m2.CRASH_PATTERNS:
                    hit = pat.search(log_text)
                    if hit:
                        res.errors.append(f"Crash/exception in log: {hit.group(0)!r}")
                for pat in m2.SHADER_GPU_ERROR_PATTERNS:
                    hit = pat.search(log_text)
                    if hit:
                        res.errors.append(f"GPU/shader error in log: {hit.group(0)!r}")
                perf = json.loads(perf_json.read_text(encoding="utf-8"))
                m_ov = extract_eleven_metrics(perf, "overall", log_text)
                m_gp = extract_eleven_metrics(perf, "gameplay_active", log_text)
                m_fmv = extract_eleven_metrics(perf, "fmv", log_text)
                res.metrics_overall = asdict(m_ov)
                res.metrics_gameplay = asdict(m_gp)
                res.metrics_fmv = asdict(m_fmv)
                if m_ov.frames <= 0:
                    res.errors.append("perf.json reports 0 overall frames.")
                if strict_post:
                    if m_ov.missing_keys:
                        res.errors.append(f"perf.json is missing zero-gated metrics: {', '.join(m_ov.missing_keys)}.")
                    if m_ov.audio_underruns != 0 or m_ov.audio_glitches != 0:
                        res.errors.append(f"Audio underruns/glitches non-zero (underruns={m_ov.audio_underruns}, glitches={m_ov.audio_glitches}).")
                    if m_ov.mid_frame_flushes != 0:
                        res.errors.append(f"Mid-frame Submit(true) flushes non-zero ({m_ov.mid_frame_flushes}).")
                    if m_ov.sync_present_cache_writes != 0:
                        res.errors.append(f"Sync disk cache writes in Present() non-zero ({m_ov.sync_present_cache_writes}).")
                    if m_ov.upload_overflows != 0:
                        res.errors.append(f"Upload ring overflows non-zero ({m_ov.upload_overflows}).")
                res.passed = len(res.errors) == 0
                return res

    cli: list[str] = ["--no-launcher", "--fps-cap", str(fps_cap)]
    if gpu_index is not None:
        cli.extend(["--gpu", str(gpu_index)])
    if present_mode:
        cli.extend(["--present-mode", present_mode])
    cli.extend(extra_cli)

    if platform == "win64":
        pre_pids = m2.get_running_win_psiops()
        if pre_pids:
            res.errors.append(f"Refusing to launch: PsiOps.exe already running (PIDs={pre_pids}).")
            return res

        env = os.environ.copy()
        env["XBE_AUTOTEST"] = "1"
        env["PSIOPS_AUTOTEST"] = "1"
        env["XBE_ISO_PATH"] = str(iso_path)
        env["XBE_PERF_LOG"] = str(perf_json)
        env["XBE_AUTOTEST_OUT_DIR"] = str(run_dir)
        if cache_state == "cold":
            env["XBE_NO_SHADER_CACHE"] = "1"
        if workload_kind == "rec":
            cli.extend(["--replay-input", str(workload_path), "--replay-mode", "hybrid", "--replay-exit"])
        else:
            env["XBE_AUTOTEST_SCRIPT"] = str(workload_path)
        env.update(extra_env)

        cmd = [str(exe_path), "--iso", str(iso_path), "--perf-log", str(perf_json), "--state-log", str(state_log)] + cli
        t0 = time.monotonic()
        stdout_s, stderr_s = "", ""
        try:
            proc = subprocess.run(cmd, cwd=str(exe_path.parent), env=env, capture_output=True, text=True, timeout=timeout_s, check=False)
            res.exit_code = proc.returncode
            stdout_s, stderr_s = proc.stdout or "", proc.stderr or ""
        except subprocess.TimeoutExpired:
            res.timed_out = True
            res.errors.append(f"{name}: exceeded hard timeout ({timeout_s}s).")
            m2.kill_win_psiops()
        finally:
            res.wall_s = round(time.monotonic() - t0, 3)
            post_pids = m2.get_running_win_psiops()
            if post_pids:
                m2.kill_win_psiops()
                res.errors.append(f"{name}: leftover PsiOps.exe processes: {post_pids}.")
    else:
        pre_pids = m2.get_running_wsl_psiops(wsl_distro)
        if pre_pids:
            res.errors.append(f"Refusing to launch: Linux PsiOps already running in {wsl_distro} (PIDs={pre_pids}).")
            return res

        wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
        wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
        wsl_iso = m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path)
        wsl_perf = m2.to_wsl_path(perf_json) if sys.platform == "win32" else str(perf_json)
        wsl_state = m2.to_wsl_path(state_log) if sys.platform == "win32" else str(state_log)
        wsl_out = m2.to_wsl_path(run_dir) if sys.platform == "win32" else str(run_dir)
        wsl_work = m2.to_wsl_path(workload_path) if sys.platform == "win32" else str(workload_path)

        env_kv = [
            "XBE_AUTOTEST=1",
            "PSIOPS_AUTOTEST=1",
            f"XBE_ISO_PATH={wsl_iso}",
            f"XBE_PERF_LOG={wsl_perf}",
            f"XBE_AUTOTEST_OUT_DIR={wsl_out}",
            "SDL_AUDIODRIVER=dummy",
        ]
        if cache_state == "cold":
            env_kv.append("XBE_NO_SHADER_CACHE=1")
        if workload_kind == "rec":
            cli.extend(["--replay-input", wsl_work, "--replay-mode", "hybrid", "--replay-exit"])
        else:
            env_kv.append(f"XBE_AUTOTEST_SCRIPT={wsl_work}")
        for k, v in extra_env.items():
            env_kv.append(f"{k}={v}")

        inner = (
            ["env"]
            + env_kv
            + [
                "xvfb-run",
                "-a",
                "-s",
                "-screen 0 640x480x24",
                wsl_exe,
                "--iso",
                wsl_iso,
                "--perf-log",
                wsl_perf,
                "--state-log",
                wsl_state,
            ]
            + cli
        )
        cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner

        t0 = time.monotonic()
        stdout_s, stderr_s = "", ""
        try:
            proc = subprocess.run(cmd, cwd=None if sys.platform == "win32" else str(exe_path.parent), capture_output=True, text=True, timeout=timeout_s, check=False)
            res.exit_code = proc.returncode
            stdout_s, stderr_s = proc.stdout or "", proc.stderr or ""
        except subprocess.TimeoutExpired:
            res.timed_out = True
            res.errors.append(f"{name}: exceeded hard timeout ({timeout_s}s).")
            m2.kill_wsl_psiops(wsl_distro)
        finally:
            res.wall_s = round(time.monotonic() - t0, 3)
            post_pids = m2.get_running_wsl_psiops(wsl_distro)
            if post_pids:
                m2.kill_wsl_psiops(wsl_distro)
                res.errors.append(f"{name}: leftover Linux PsiOps processes: {post_pids}.")

    newest_log: Path | None = None
    if logs_dir.is_dir():
        changed = [
            p for p in logs_dir.glob("psiops*.log")
            if p.is_file() and (p not in logs_before or p.stat().st_mtime_ns != logs_before[p])
        ]
        if changed:
            newest_log = max(changed, key=lambda p: p.stat().st_mtime_ns)
    if newest_log is None and log_file.is_file():
        newest_log = log_file

    if newest_log is not None:
        shutil.copy2(newest_log, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    am = ADAPTER_LOG_RE.search(log_text)
    if am:
        res.adapter_name = am.group(1).strip()
        res.adapter_vid = am.group(2).upper()
        res.adapter_did = am.group(3).upper()

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"{name}: non-zero exit code {res.exit_code}.")

    for pat in m2.CRASH_PATTERNS:
        hit = pat.search(log_text)
        if hit:
            res.errors.append(f"Crash/exception in log: {hit.group(0)!r}")
    for pat in m2.SHADER_GPU_ERROR_PATTERNS:
        hit = pat.search(log_text)
        if hit:
            res.errors.append(f"GPU/shader error in log: {hit.group(0)!r}")
    if "Exit (0)" not in log_text:
        res.errors.append("Log missing clean 'Exit (0)'.")

    res.errors.extend(m2.assert_media_unchanged(media_before))

    if not perf_json.is_file():
        res.errors.append(f"Missing perf telemetry JSON: {perf_json}")
        return res

    perf = json.loads(perf_json.read_text(encoding="utf-8"))
    m_ov = extract_eleven_metrics(perf, "overall", log_text)
    m_gp = extract_eleven_metrics(perf, "gameplay_active", log_text)
    m_fmv = extract_eleven_metrics(perf, "fmv", log_text)
    res.metrics_overall = asdict(m_ov)
    res.metrics_gameplay = asdict(m_gp)
    res.metrics_fmv = asdict(m_fmv)

    if m_ov.frames <= 0:
        res.errors.append("perf.json reports 0 overall frames.")
    if m_gp.frames <= 0:
        res.warnings.append("perf.json reports 0 gameplay_active frames.")
    if m_fmv.frames <= 0:
        res.warnings.append("perf.json reports 0 fmv frames.")

    if strict_post:
        if m_ov.missing_keys:
            res.errors.append(f"perf.json is missing zero-gated metrics: {', '.join(m_ov.missing_keys)}.")
        if m_ov.audio_underruns != 0 or m_ov.audio_glitches != 0:
            res.errors.append(f"Audio underruns/glitches non-zero (underruns={m_ov.audio_underruns}, glitches={m_ov.audio_glitches}).")
        if m_ov.mid_frame_flushes != 0:
            res.errors.append(f"Mid-frame Submit(true) flushes non-zero ({m_ov.mid_frame_flushes}).")
        if m_ov.sync_present_cache_writes != 0:
            res.errors.append(f"Sync disk cache writes in Present() non-zero ({m_ov.sync_present_cache_writes}).")
        if m_ov.upload_overflows != 0:
            res.errors.append(f"Upload ring overflows non-zero ({m_ov.upload_overflows}).")

    res.passed = len(res.errors) == 0
    return res


def verify_60_vs_30_fidelity(run60: BenchmarkRunResult, run30: BenchmarkRunResult) -> dict[str, Any]:
    report: dict[str, Any] = {"passed": False, "errors": []}
    r60 = float(run60.metrics_gameplay.get("g_elapsed_to_wall_ratio", 0.0))
    r30 = float(run30.metrics_gameplay.get("g_elapsed_to_wall_ratio", 0.0))
    report["ratio_60fps"] = r60
    report["ratio_30fps"] = r30
    if not (0.97 <= r60 <= 1.03):
        report["errors"].append(f"60 FPS speed ratio out of [0.97, 1.03]: {r60:.4f}")
    if not (0.97 <= r30 <= 1.03):
        report["errors"].append(f"30 FPS speed ratio out of [0.97, 1.03]: {r30:.4f}")

    # Compare world position at the highest common sim_elapsed_s in --state-log
    p60 = Path(run60.state_log_path)
    p30 = Path(run30.state_log_path)
    if p60.is_file() and p30.is_file():
        def load_first_gp_segment(path: Path) -> list[dict[str, Any]]:
            out = []
            last_sim = -1.0
            for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
                ln = ln.strip()
                if not ln:
                    continue
                rec = json.loads(ln)
                if rec.get("phase") == "gameplay_active" and "player_x" in rec:
                    sim = float(rec.get("sim_elapsed_s", 0.0))
                    # Stop at checkpoint restart (sim_elapsed_s resets back near 0) so we compare matching segments.
                    if out and sim + 1.0 < last_sim:
                        break
                    out.append(rec)
                    last_sim = sim
            return out

        g60, g30 = load_first_gp_segment(p60), load_first_gp_segment(p30)
        if g60 and g30:
            target_sim = min(float(g60[-1].get("sim_elapsed_s", 0.0)), float(g30[-1].get("sim_elapsed_s", 0.0)))
            dists: list[float] = []
            yaw_diffs: list[float] = []
            for r30 in g30:
                sim_t = float(r30.get("sim_elapsed_s", 0.0))
                if sim_t > target_sim + 0.05:
                    break
                r60 = min(g60, key=lambda r: abs(float(r.get("sim_elapsed_s", 0.0)) - sim_t))
                dists.append(
                    math.sqrt(
                        (float(r60["player_x"]) - float(r30["player_x"])) ** 2
                        + (float(r60["player_y"]) - float(r30["player_y"])) ** 2
                        + (float(r60["player_z"]) - float(r30["player_z"])) ** 2
                    )
                )
                if "cam_yaw" in r60 and "cam_yaw" in r30:
                    yaw_diffs.append(abs(float(r60["cam_yaw"]) - float(r30["cam_yaw"])))

            dists_sorted = sorted(dists)
            med_dist = dists_sorted[len(dists_sorted) // 2] if dists_sorted else 0.0
            max_yaw = max(yaw_diffs) if yaw_diffs else 0.0
            s60 = min(g60, key=lambda r: abs(float(r.get("sim_elapsed_s", 0.0)) - target_sim))
            s30 = min(g30, key=lambda r: abs(float(r.get("sim_elapsed_s", 0.0)) - target_sim))
            end_dist = math.sqrt(
                (float(s60["player_x"]) - float(s30["player_x"])) ** 2
                + (float(s60["player_y"]) - float(s30["player_y"])) ** 2
                + (float(s60["player_z"]) - float(s30["player_z"])) ** 2
            )
            report["compared_sim_s"] = round(target_sim, 3)
            report["matched_samples"] = len(dists)
            report["pos_60fps"] = [s60["player_x"], s60["player_y"], s60["player_z"]]
            report["pos_30fps"] = [s30["player_x"], s30["player_y"], s30["player_z"]]
            report["median_world_distance_delta"] = round(med_dist, 4)
            report["end_world_distance_delta"] = round(end_dist, 4)
            report["max_yaw_delta_rad"] = round(max_yaw, 5)
            report["vitals_match"] = (
                s60.get("health") == s30.get("health")
                and s60.get("psi") == s30.get("psi")
                and s60.get("ammo_clip") == s30.get("ammo_clip")
            )
            if med_dist > 1.0:
                report["errors"].append(f"60 vs 30 FPS median world position diverged by {med_dist:.4f} (> 1.0).")
            if max_yaw > 0.05:
                report["errors"].append(f"60 vs 30 FPS max camera yaw diverged by {max_yaw:.5f} rad (> 0.05).")
            if not report["vitals_match"]:
                report["errors"].append(f"60 vs 30 FPS end vitals mismatch (60={s60.get('health')}/{s60.get('psi')}/{s60.get('ammo_clip')}, 30={s30.get('health')}/{s30.get('psi')}/{s30.get('ammo_clip')}).")
    report["passed"] = len(report["errors"]) == 0
    return report


def run_pacing_matrix(exe_path: Path, platform: str, wsl_distro: str, dry_run: bool) -> dict[str, Any]:
    out: dict[str, Any] = {"platform": platform, "passed": True, "cases": [], "errors": []}
    for hz in REFRESH_MATRIX_HZ:
        for mode in PRESENT_MODES:
            is_60_mult = (hz % 60 == 0)
            if dry_run:
                sync_interval = (hz // 60) if (mode in ("auto", "vsync") and is_60_mult) else 0
                vsync_paces = (sync_interval > 0)
                case = {
                    "hz": hz,
                    "mode": mode,
                    "sync_interval": sync_interval,
                    "vsync_paces": vsync_paces,
                    "double_vblank_wait": False,
                    "dry_run": True,
                }
                out["cases"].append(case)
                continue

            if platform == "win64":
                cmd = [str(exe_path), "--query-pacing", "--simulate-refresh-hz", str(hz), "--present-mode", mode, "--fps-cap", "60"]
                proc = subprocess.run(cmd, cwd=str(exe_path.parent), capture_output=True, text=True, timeout=15, check=False)
            else:
                wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
                wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
                inner = [wsl_exe, "--query-pacing", "--simulate-refresh-hz", str(hz), "--present-mode", mode, "--fps-cap", "60"]
                cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15, check=False)

            if proc.returncode != 0:
                out["errors"].append(f"pacing query {platform} {hz}Hz/{mode} failed rc={proc.returncode}")
                continue
            try:
                data = json.loads(proc.stdout.strip().splitlines()[-1])
            except Exception as exc:
                out["errors"].append(f"pacing query {platform} {hz}Hz/{mode} invalid JSON: {exc}")
                continue
            out["cases"].append(data)

            # Validate de-conflicted VRR/VSync contract (matches --query-pacing JSON schema)
            si = int(data.get("sync_interval", -1))
            vp = bool(data.get("vsync_pacing_active", data.get("vsync_paces", False)))
            if bool(data.get("double_vblank_wait", False)):
                out["errors"].append(f"{platform} {hz}Hz/{mode} reported double_vblank_wait=true: {data}")
            if mode == "auto":
                if is_60_mult:
                    if si != hz // 60 or not vp:
                        out["errors"].append(f"{platform} {hz}Hz/auto expected sync_interval={hz//60} & vsync_pacing_active=true, got {data}")
                else:
                    if si != 0 or vp:
                        out["errors"].append(f"{platform} {hz}Hz/auto (non-60Hz multiple) must avoid VBlank double-wait (expected sync_interval=0, vsync_pacing_active=false), got {data}")
            elif mode == "vsync":
                if is_60_mult:
                    if si != hz // 60 or not vp or bool(data.get("allow_tearing", False)):
                        out["errors"].append(f"{platform} {hz}Hz/vsync expected sync_interval={hz//60}, vsync_pacing_active=true & allow_tearing=false, got {data}")
                else:
                    if si != 0 or vp or bool(data.get("allow_tearing", False)):
                        out["errors"].append(f"{platform} {hz}Hz/vsync (non-60Hz multiple) must use tear-free mailbox pacing (expected sync_interval=0, vsync_pacing_active=false, allow_tearing=false), got {data}")
            elif mode == "vrr":
                if si != 0 or vp:
                    out["errors"].append(f"{platform} {hz}Hz/vrr must use sync_interval=0 & vsync_pacing_active=false, got {data}")
            elif mode == "mailbox":
                if si != 0 or vp:
                    out["errors"].append(f"{platform} {hz}Hz/mailbox must use sync_interval=0 & vsync_pacing_active=false, got {data}")

    out["passed"] = len(out["errors"]) == 0
    return out


def run_compile_jobs_matrix(exe_path: Path, platform: str, wsl_distro: str, dry_run: bool) -> dict[str, Any]:
    out: dict[str, Any] = {"platform": platform, "passed": True, "cases": [], "errors": []}
    for ram_mb in RAM_BUDGETS_MB:
        if dry_run:
            est_jobs = max(1, min(16, ram_mb // 768))
            out["cases"].append({"avail_ram_mb": ram_mb, "jobs": est_jobs, "dry_run": True})
            continue

        if platform == "win64":
            cmd = [str(exe_path), "--query-compile-jobs", "--simulate-ram-mb", str(ram_mb)]
            proc = subprocess.run(cmd, cwd=str(exe_path.parent), capture_output=True, text=True, timeout=15, check=False)
        else:
            wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
            wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
            inner = [wsl_exe, "--query-compile-jobs", "--simulate-ram-mb", str(ram_mb)]
            cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15, check=False)

        if proc.returncode != 0:
            out["errors"].append(f"compile-jobs query {platform} {ram_mb}MB failed rc={proc.returncode}")
            continue
        try:
            data = json.loads(proc.stdout.strip().splitlines()[-1])
        except Exception as exc:
            out["errors"].append(f"compile-jobs query {platform} {ram_mb}MB invalid JSON: {exc}")
            continue
        jobs = int(data.get("slots", data.get("jobs", 0)))
        out["cases"].append(data)
        max_allowed = max(1, ram_mb // 768)
        if jobs < 1 or jobs > max_allowed:
            out["errors"].append(f"{platform} {ram_mb}MB returned slots={jobs} (exceeds ~768 MiB/slot cap {max_allowed}).")

    # Verify explicit --compile-jobs 3 override
    if not dry_run:
        if platform == "win64":
            cmd = [str(exe_path), "--query-compile-jobs", "--compile-jobs", "3"]
            proc = subprocess.run(cmd, cwd=str(exe_path.parent), capture_output=True, text=True, timeout=15, check=False)
        else:
            wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
            wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
            inner = [wsl_exe, "--query-compile-jobs", "--compile-jobs", "3"]
            cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15, check=False)
        if proc.returncode == 0:
            data = json.loads(proc.stdout.strip().splitlines()[-1])
            out["override_check"] = data
            if int(data.get("slots", data.get("jobs", 0))) != 3:
                out["errors"].append(f"{platform} --compile-jobs 3 override failed: got {data}")
        else:
            out["errors"].append(f"{platform} --compile-jobs 3 override failed rc={proc.returncode}")

    out["passed"] = len(out["errors"]) == 0
    return out


def verify_cache_files(exe_dir: Path, platform: str, vid: str, did: str) -> dict[str, Any]:
    cache_dir = exe_dir / "cache"
    if platform == "win64":
        expected = [f"psos_{vid}_{did}.bin", f"pso_meta_{vid}_{did}.bin"]
    else:
        expected = ["vk_shaders.bin", f"vk_psos_{vid}_{did}.bin", f"vk_pso_meta_{vid}_{did}.bin"]
    found: dict[str, int] = {}
    missing: list[str] = []
    for fn in expected:
        p = cache_dir / fn
        if p.is_file() and p.stat().st_size > 0:
            found[fn] = p.stat().st_size
        else:
            missing.append(fn)
    return {"cache_dir": str(cache_dir), "found": found, "missing": missing, "passed": len(missing) == 0}


def build_pre_vs_post_markdown(pre_report: dict[str, Any], post_report: dict[str, Any]) -> str:
    pre_by_name = {r["name"]: r for r in pre_report.get("benchmarks", [])}
    post_by_name = {r["name"]: r for r in post_report.get("benchmarks", [])}
    lines = ["# Milestone 4 Pre- vs Post-Optimization 11-Metric Comparison\n"]
    metric_keys = (
        ("avg_fps", "Avg FPS"),
        ("p50_ms", "p50 (ms)"),
        ("fps_1pct_low", "1% Low FPS"),
        ("p99_ms", "p99 (ms)"),
        ("p99_9_ms", "p99.9 (ms)"),
        ("max_ms", "Max (ms)"),
        ("guest_cpu_ms", "Guest CPU (ms)"),
        ("render_cpu_ms", "Render CPU (ms)"),
        ("gpu_ms", "GPU (ms)"),
        ("lock_stalls", "Lock Stalls"),
        ("mid_frame_flushes", "Mid-Frame Flushes"),
        ("compile_count", "Shader Compiles"),
        ("pso_create_count", "PSO Creates"),
        ("boot_prewarm_ms", "Boot Prewarm (ms)"),
        ("sync_present_cache_writes", "Sync Present Cache Writes"),
        ("audio_underruns", "Audio Underruns"),
        ("audio_glitches", "Audio Glitches"),
        ("av_sync_ratio", "A/V Sync Ratio"),
        ("disc_cache_hit_pct", "Disc Cache Hit %"),
        ("disc_os_reads", "OS Disc Reads"),
        ("io_avg_us", "I/O Avg (us)"),
        ("io_p99_us", "I/O p99 (us)"),
        ("io_max_us", "I/O Max (us)"),
        ("working_set_peak_mb", "Working Set Peak (MB)"),
        ("commit_peak_mb", "Commit Peak (MB)"),
        ("gpu_slab_count", "GPU Slabs (32 MiB)"),
        ("upload_ring_mb", "Upload Ring (MiB)"),
        ("g_elapsed_to_wall_ratio", "Sim Speed Ratio"),
    )

    common = sorted(set(pre_by_name.keys()) & set(post_by_name.keys()))
    for name in common:
        for phase_field, phase_label in (("metrics_gameplay", "Gameplay Active"), ("metrics_fmv", "FMV / Cutscene")):
            pm = pre_by_name[name].get(phase_field, {})
            qm = post_by_name[name].get(phase_field, {})
            lines.append(f"## {name} — {phase_label}\n")
            lines.append("| Metric | Pre | Post | Delta |")
            lines.append("|---|---:|---:|---:|")
            for k, label in metric_keys:
                v0 = pm.get(k, 0)
                v1 = qm.get(k, 0)
                delta = round(float(v1) - float(v0), 4) if isinstance(v0, (int, float)) and isinstance(v1, (int, float)) else "n/a"
                lines.append(f"| {label} | {v0} | {v1} | {delta} |")
            lines.append("")
    return "\n".join(lines) + "\n"


def _self_test_m4_logic(repo_root: Path) -> None:
    """In-memory verification during --dry-run using Milestone 1/2/3 perf.json if present."""
    sample_perf = {
        "meta": {"wall_duration_s": 30.0, "total_presents": 1800},
        "phases": {
            "overall": {"frames": 1799, "avg_fps": 60.0, "p50_ms": 16.67, "fps_1pct_low_mean": 58.5, "p99_ms": 17.1, "p99_9_ms": 18.2, "max_ms": 19.0, "guest_work_ms_avg": 2.1, "render_host_ms_avg": 0.4, "gpu_ms_avg": 0.5, "g_elapsed_to_wall_ratio": 1.0},
            "gameplay_active": {"frames": 1200, "avg_fps": 60.0, "p50_ms": 16.67, "fps_1pct_low_mean": 59.0, "p99_ms": 16.9, "p99_9_ms": 17.4, "max_ms": 18.0, "guest_work_ms_avg": 2.4, "render_host_ms_avg": 0.5, "gpu_ms_avg": 0.6, "g_elapsed_to_wall_ratio": 0.998},
            "fmv": {"frames": 599, "avg_fps": 60.0, "p50_ms": 16.67, "fps_1pct_low_mean": 59.5, "p99_ms": 16.8, "p99_9_ms": 17.0, "max_ms": 17.2, "guest_work_ms_avg": 1.2, "render_host_ms_avg": 0.2, "gpu_ms_avg": 0.1, "g_elapsed_to_wall_ratio": 1.0},
        },
        "workload": {"process_cpu_pct_avg": 6.5},
        "stalls": {"render_waits_total": 0, "lock_stalls": 0, "mid_frame_flushes": 0},
        "shaders": {"compile_count": 0, "pso_create_count": 0, "boot_prewarm_ms": 3.2, "sync_present_cache_writes": 0},
        "audio": {"xaudio2_underruns": 0, "xaudio2_glitches": 0, "stream_underruns": 0, "blocks_submitted": 3000, "av_sync_ratio": 1.0},
        "io": {"disc_reads": 320, "disc_os_reads": 40, "disc_cache_hit_pct": 87.5, "io_avg_us": 42.0, "io_p99_us": 180.0, "io_max_us": 310.0},
        "memory": {"host_ws_peak_mb": 420.0, "host_private_peak_mb": 510.0, "gpu_slab_count": 2, "gpu_slab_mb": 64.0, "d3d12_upload_ring_cap_mb": 64.0, "d3d12_upload_overflows": 0},
    }
    m_gp = extract_eleven_metrics(sample_perf, "gameplay_active")
    assert m_gp.avg_fps == 60.0 and m_gp.mid_frame_flushes == 0 and m_gp.sync_present_cache_writes == 0
    assert m_gp.disc_cache_hit_pct == 87.5 and m_gp.gpu_slab_count == 2 and m_gp.upload_ring_mb == 64.0
    assert m_gp.missing_keys == [], m_gp.missing_keys
    m_empty = extract_eleven_metrics({"phases": {"overall": {"frames": 10}}}, "overall")
    assert set(m_empty.missing_keys) == {
        "stalls.lock_stalls", "stalls.mid_frame_flushes", "shaders.compile_count", "shaders.pso_create_count",
        "shaders.sync_present_cache_writes", "audio.underruns", "audio.glitches", "memory.upload_overflows",
    }, m_empty.missing_keys
    md = build_pre_vs_post_markdown(
        {"benchmarks": [{"name": "win64_warm", "metrics_gameplay": asdict(m_gp), "metrics_fmv": asdict(m_gp)}]},
        {"benchmarks": [{"name": "win64_warm", "metrics_gameplay": asdict(m_gp), "metrics_fmv": asdict(m_gp)}]},
    )
    assert "| Avg FPS |" in md
    assert parse_list_gpus_output("[0] NVIDIA GeForce RTX 3090 (VID=0x10DE DID=0x2204 UMA=false)\n[1] AMD Radeon(TM) Graphics (VID=0x1002 DID=0x13C0 UMA=true)")


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Milestone 4 performance, 60/30 FPS fidelity, multi-GPU & pacing suite.")
    parser.add_argument("--mode", choices=("pre", "post", "full", "compare"), default="post")
    parser.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    parser.add_argument("--out-dir", type=Path, default=repo_root / ".local" / "m4_perf")
    parser.add_argument("--iso", type=Path, default=repo_root / m2.EXPECTED_ISO_REL)
    parser.add_argument("--win-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    parser.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    parser.add_argument("--rec", type=Path, default=repo_root / ".local" / "m3_replay" / "m3_session.rec")
    parser.add_argument("--wsl-distro", type=str, default=None)
    parser.add_argument("--timeout", type=int, default=160)
    parser.add_argument("--skip-igpu", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.iso.is_file():
        print(f"ERROR: XISO not found: {args.iso}", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.dry_run:
        _self_test_m4_logic(repo_root)

    if args.mode == "compare":
        pre_p = args.out_dir / "pre_report.json"
        post_p = args.out_dir / "post_report.json"
        if not pre_p.is_file() or not post_p.is_file():
            print(f"ERROR: Need both {pre_p} and {post_p} for --mode compare.", file=sys.stderr)
            return 2
        md = build_pre_vs_post_markdown(json.loads(pre_p.read_text(encoding="utf-8")), json.loads(post_p.read_text(encoding="utf-8")))
        out_md = args.out_dir / "pre_vs_post_comparison.md"
        out_md.write_text(md, encoding="utf-8", newline="\n")
        print(f"[test_m4_perf] Wrote comparison table: {out_md}")
        return 0

    platforms = ["win64", "linux"] if args.platform == "both" else [args.platform]
    wsl_distro = ""
    if "linux" in platforms and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)

    # Select deterministic workload: prefer recorded Milestone 3 .rec, else Milestone 2 script
    if args.rec.is_file() and args.rec.stat().st_size > 0:
        workload_kind, workload_path = "rec", args.rec
    else:
        script_path = args.out_dir / "m4_workload_script.txt"
        script_path.write_text(m2.DEFAULT_M2_SCRIPT, encoding="utf-8", newline="\n")
        workload_kind, workload_path = "script", script_path

    modes_to_run = ["pre", "post"] if args.mode == "full" else [args.mode]
    overall_ok = True

    for run_mode in modes_to_run:
        strict_post = (run_mode == "post")
        mode_dir = args.out_dir / ("pre_baseline" if run_mode == "pre" else "post_optimized")
        mode_dir.mkdir(parents=True, exist_ok=True)
        benchmarks: list[BenchmarkRunResult] = []
        extras: dict[str, Any] = {}
        mode_ok = True

        # 1. Cold & Warm 11-metric benchmarks on Win64 & Linux
        for plat in platforms:
            exe = args.win_exe if plat == "win64" else args.linux_exe
            for cache_state in ("cold", "warm"):
                bname = f"{plat}_{cache_state}_60fps"
                print(f"[test_m4_perf:{run_mode}] Running {bname}...")
                r = run_benchmark_case(
                    name=bname,
                    platform=plat,
                    cache_state=cache_state,
                    exe_path=exe,
                    iso_path=args.iso,
                    repo_root=repo_root,
                    run_dir=mode_dir / bname,
                    workload_kind=workload_kind,
                    workload_path=workload_path,
                    fps_cap=60,
                    gpu_index=0 if (plat == "win64" and strict_post) else None,
                    present_mode="auto" if strict_post else None,
                    extra_cli=[],
                    extra_env={},
                    timeout_s=args.timeout,
                    wsl_distro=wsl_distro,
                    strict_post=strict_post,
                    dry_run=args.dry_run,
                    resume=args.resume,
                )
                benchmarks.append(r)
                print(f"[test_m4_perf:{run_mode}] {bname}: {'PASS' if r.passed else 'FAIL'} (exit={r.exit_code}, wall={r.wall_s}s)")
                if not r.passed:
                    mode_ok = False
                    for e in r.errors:
                        print(f"  FAIL: {e}")

        # 2. Post-mode Milestone 4 gates: 60 vs 30 FPS fidelity, Multi-GPU/UMA, Pacing Matrix, Compile Jobs
        if strict_post:
            if "win64" in platforms:
                # 2a. PE high-performance GPU exports
                if not args.dry_run:
                    pe_ok, pe_info = check_pe_gpu_exports(args.win_exe)
                    extras["pe_gpu_exports"] = pe_info
                    if not pe_ok:
                        mode_ok = False
                        print(f"  FAIL: PE GPU exports check failed: {pe_info}")

                # 2b. --list-gpus discovery
                if not args.dry_run:
                    lg = subprocess.run([str(args.win_exe), "--list-gpus"], cwd=str(args.win_exe.parent), capture_output=True, text=True, timeout=15, check=False)
                    gpus = parse_list_gpus_output(lg.stdout)
                    extras["list_gpus"] = {"rc": lg.returncode, "gpus": gpus}
                    if lg.returncode != 0 or not gpus:
                        mode_ok = False
                        print(f"  FAIL: --list-gpus failed (rc={lg.returncode}, gpus={gpus})")

                # 2c. 60 FPS vs 30 FPS 1.00x fidelity
                print("[test_m4_perf:post] Running win64_warm_30fps for 60 vs 30 FPS fidelity...")
                r30 = run_benchmark_case(
                    name="win64_warm_30fps",
                    platform="win64",
                    cache_state="warm",
                    exe_path=args.win_exe,
                    iso_path=args.iso,
                    repo_root=repo_root,
                    run_dir=mode_dir / "win64_warm_30fps",
                    workload_kind=workload_kind,
                    workload_path=workload_path,
                    fps_cap=30,
                    gpu_index=0,
                    present_mode="auto",
                    extra_cli=[],
                    extra_env={},
                    timeout_s=args.timeout,
                    wsl_distro=wsl_distro,
                    strict_post=True,
                    dry_run=args.dry_run,
                    resume=args.resume,
                )
                benchmarks.append(r30)
                if not r30.passed:
                    mode_ok = False
                r60 = next((b for b in benchmarks if b.name == "win64_warm_60fps"), None)
                if r60 and not args.dry_run:
                    fid = verify_60_vs_30_fidelity(r60, r30)
                    extras["fidelity_60_vs_30"] = fid
                    if not fid["passed"]:
                        mode_ok = False
                        print(f"  FAIL: 60 vs 30 FPS fidelity: {fid['errors']}")

                # 2d. GPU 0 (RTX 3090 -> 64 MiB ring) & GPU 1 (AMD Radeon UMA -> 16/32 MiB ring) + per-VID/DID caches
                if r60 and not args.dry_run:
                    vid0, did0 = r60.adapter_vid or "10DE", r60.adapter_did or "2204"
                    c0 = verify_cache_files(args.win_exe.parent, "win64", vid0, did0)
                    extras["gpu0_cache"] = c0
                    ring0 = float(r60.metrics_overall.get("upload_ring_mb", 0.0))
                    if ring0 != 64.0:
                        mode_ok = False
                        print(f"  FAIL: GPU 0 expected 64 MiB upload ring, got {ring0} MiB")
                    if not c0["passed"]:
                        mode_ok = False
                        print(f"  FAIL: GPU 0 per-VID/DID cache missing: {c0['missing']}")

                if not args.skip_igpu:
                    print("[test_m4_perf:post] Running win64_gpu1_uma_60fps (AMD Radeon UMA iGPU)...")
                    r_igpu = run_benchmark_case(
                        name="win64_gpu1_uma_60fps",
                        platform="win64",
                        cache_state="warm",
                        exe_path=args.win_exe,
                        iso_path=args.iso,
                        repo_root=repo_root,
                        run_dir=mode_dir / "win64_gpu1_uma_60fps",
                        workload_kind=workload_kind,
                        workload_path=workload_path,
                        fps_cap=60,
                        gpu_index=1,
                        present_mode="auto",
                        extra_cli=[],
                        extra_env={},
                        timeout_s=args.timeout,
                        wsl_distro=wsl_distro,
                        strict_post=True,
                        dry_run=args.dry_run,
                    )
                    benchmarks.append(r_igpu)
                    if not r_igpu.passed:
                        mode_ok = False
                    if not args.dry_run:
                        vid1, did1 = r_igpu.adapter_vid or "1002", r_igpu.adapter_did or "13C0"
                        c1 = verify_cache_files(args.win_exe.parent, "win64", vid1, did1)
                        extras["gpu1_cache"] = c1
                        ring1 = float(r_igpu.metrics_overall.get("upload_ring_mb", 0.0))
                        if ring1 not in (16.0, 32.0):
                            mode_ok = False
                            print(f"  FAIL: GPU 1 UMA expected 16 or 32 MiB upload ring, got {ring1} MiB")
                        if not c1["passed"]:
                            mode_ok = False
                            print(f"  FAIL: GPU 1 per-VID/DID cache missing: {c1['missing']}")

                # 2e. VRR / VSync pacing matrix & RAM-bounded PrepareGameCode (Win64)
                pm_w = run_pacing_matrix(args.win_exe, "win64", wsl_distro, args.dry_run)
                cj_w = run_compile_jobs_matrix(args.win_exe, "win64", wsl_distro, args.dry_run)
                extras["pacing_win64"] = pm_w
                extras["compile_jobs_win64"] = cj_w
                if not pm_w["passed"] or not cj_w["passed"]:
                    mode_ok = False

            if "linux" in platforms:
                r_lin = next((b for b in benchmarks if b.name == "linux_warm_60fps"), None)
                if r_lin and not args.dry_run:
                    vid_l, did_l = r_lin.adapter_vid or "10005", r_lin.adapter_did or "0000"
                    c_l = verify_cache_files(args.linux_exe.parent, "linux", vid_l, did_l)
                    extras["linux_vk_cache"] = c_l
                    if not c_l["passed"]:
                        mode_ok = False
                        print(f"  FAIL: Linux Vulkan cache missing: {c_l['missing']}")
                pm_l = run_pacing_matrix(args.linux_exe, "linux", wsl_distro, args.dry_run)
                cj_l = run_compile_jobs_matrix(args.linux_exe, "linux", wsl_distro, args.dry_run)
                extras["pacing_linux"] = pm_l
                extras["compile_jobs_linux"] = cj_l
                if not pm_l["passed"] or not cj_l["passed"]:
                    mode_ok = False

        report = {
            "mode": run_mode,
            "iso": str(args.iso),
            "workload_kind": workload_kind,
            "workload_path": str(workload_path),
            "passed": mode_ok,
            "benchmarks": [asdict(b) for b in benchmarks],
            "extras": extras,
        }
        out_json = args.out_dir / f"{run_mode}_report.json"
        out_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"[test_m4_perf:{run_mode}] Report written: {out_json} (passed={mode_ok})")
        if not mode_ok:
            overall_ok = False

    if args.mode == "full":
        pre_p = args.out_dir / "pre_report.json"
        post_p = args.out_dir / "post_report.json"
        if pre_p.is_file() and post_p.is_file():
            md = build_pre_vs_post_markdown(json.loads(pre_p.read_text(encoding="utf-8")), json.loads(post_p.read_text(encoding="utf-8")))
            out_md = args.out_dir / "pre_vs_post_comparison.md"
            out_md.write_text(md, encoding="utf-8", newline="\n")
            print(f"[test_m4_perf] Wrote comparison table: {out_md}")

    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
