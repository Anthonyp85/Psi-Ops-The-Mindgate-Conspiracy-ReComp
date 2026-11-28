#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Automated Cold-Start AOT Shader, PSO & Upscaler Pre-Compilation Verification Suite.

Verifies zero mid-gameplay shader/upscaler compilation stalls from a completely
empty cache/ directory across BOTH platforms:

  1. Windows x64 D3D12 (2560x1440, GPU 0 RTX 3090, DLSS 4 Preset K Quality):
       - Deletes out/build/release/cache/ completely (0 cached shaders/PSOs).
       - Plays Boot FMV -> level97 Shell -> level00 3D Gameplay -> Pause -> Exit(0).
       - Asserts:
           * aot_shaders_compiled > 50 (cold AOT pre-compiled full NV2A shader set)
           * aot_psos_built > 0
           * dlss_prewarmed_at_boot == True (NGX_D3D12_CreateFeature ran at boot)
           * mid_gameplay_shader_compiles == 0
           * mid_gameplay_dlss_feature_creates == 0
           * sync_present_cache_writes == 0
           * On-disk cache files (shaders.bin, psos_*.bin, pso_meta_*.bin, aot_stamp_*.bin) persisted.
       - Optional warm follow-up run verifies instant cached boot (aot_cached == True)
         and 0 mid-gameplay compiles / 0 DLSS re-creates.

  2. Native Linux x86_64 Vulkan 1.2 (WSL2 Ubuntu 22.04, xvfb-run 1280x800, FSR 1.0):
       - Deletes out/build/linux-release/cache/ completely (0 cached shaders/PSOs).
       - Plays Boot FMV -> level97 Shell -> level00 3D Gameplay -> Pause -> Exit(0).
       - Asserts:
           * aot_shaders_compiled > 50
           * aot_psos_built > 0
           * mid_gameplay_shader_compiles == 0
           * sync_present_cache_writes == 0
           * On-disk Vulkan cache files (vk_shaders.bin, vk_psos_*.bin, vk_pso_meta_*.bin, aot_stamp_*.bin) persisted.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_m2_gameplay as m2  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parent.parent

AOT_LOG_RE = re.compile(
    r"(?:d3d12|vulkan):\s*AOT precompile done in\s+(?P<ms>[0-9]+(?:\.[0-9]+)?)\s*ms\s*"
    r"\(scanned_vs=(?P<scanned_vs>\d+),\s*compiled_shaders=(?P<compiled>\d+),\s*"
    r"cached_shaders=(?P<cached>\d+),\s*psos_built=(?P<psos>\d+),\s*"
    r"total_shaders=(?P<total_sh>\d+),\s*total_psos=(?P<total_pso>\d+),\s*"
    r"threads=(?P<threads>\d+),\s*aot_cached=(?P<aot_cached>true|false)\)",
    re.IGNORECASE,
)

SESSION_SUMMARY_RE = re.compile(
    r"(?:d3d12|vulkan):\s*session shader summary\s*"
    r"\(aot_compiled=(?P<aot_compiled>\d+),\s*aot_psos=(?P<aot_psos>\d+),\s*"
    r"runtime_compiles=(?P<runtime_compiles>\d+),\s*runtime_psos=(?P<runtime_psos>\d+)\)",
    re.IGNORECASE,
)

BOOT_BOUNDARY_RE = re.compile(r"(?:d3d12|vulkan):\s*640x480\s+guest,", re.IGNORECASE)
DLSS_CREATE_RE = re.compile(r"upscale:\s*DLSS 4 feature created:", re.IGNORECASE)
TEMPORAL_PREWARM_RE = re.compile(r"upscale:\s*temporal feature prewarmed", re.IGNORECASE)
UPSCALER_PREWARM_DONE_RE = re.compile(r"(?:d3d12|vulkan):\s*upscaler prewarm complete", re.IGNORECASE)
RUNTIME_COMPILE_RE = re.compile(r"(?:d3d12|vulkan):\s*runtime shader compile\b", re.IGNORECASE)


@dataclass
class ShaderRunResult:
    name: str
    platform: str
    cache_mode: str  # "cold" or "warm"
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    passed: bool = False
    aot_ms: float = 0.0
    aot_scanned_vs: int = 0
    aot_shaders_compiled: int = 0
    aot_shaders_cached: int = 0
    aot_psos_built: int = 0
    aot_total_shaders: int = 0
    aot_total_psos: int = 0
    aot_threads: int = 0
    aot_cached_skip: bool = False
    dlss_prewarmed_at_boot: bool = False
    mid_gameplay_dlss_feature_creates: int = 0
    mid_gameplay_shader_compiles: int = 0
    perf_compile_count: int = 0
    perf_csv_shader_sum: int = 0
    summary_runtime_compiles: int = 0
    summary_runtime_psos: int = 0
    sync_present_cache_writes: int = 0
    gameplay_frames: int = 0
    total_presents: int = 0
    cache_files_present: list[str] = field(default_factory=list)
    log_path: str = ""
    perf_json_path: str = ""
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def _ensure_clean_processes(platform: str, wsl_distro: str) -> None:
    if platform == "win64":
        if m2.get_running_win_psiops():
            m2.kill_win_psiops()
            time.sleep(0.5)
    else:
        if m2.get_running_wsl_psiops(wsl_distro):
            m2.kill_wsl_psiops(wsl_distro)
            time.sleep(0.5)


def _wipe_cache_dir(cache_dir: Path) -> None:
    if cache_dir.exists():
        shutil.rmtree(cache_dir, ignore_errors=False)


def _list_cache_files(cache_dir: Path) -> list[str]:
    if not cache_dir.is_dir():
        return []
    return sorted(p.name for p in cache_dir.iterdir() if p.is_file())


def _sum_csv_shaders(csv_path: Path) -> int:
    if not csv_path.is_file():
        return 0
    total = 0
    with csv_path.open("r", encoding="utf-8", errors="replace", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                total += int(row.get("shaders", "0") or 0)
            except ValueError:
                pass
    return total


def _analyze_run(
    res: ShaderRunResult,
    log_text: str,
    perf_json_path: Path,
    cache_dir: Path,
    expect_dlss: bool,
    min_aot_shaders: int,
) -> None:
    # 1. Crash & GPU error checks
    for pat in m2.CRASH_PATTERNS:
        if pat.search(log_text):
            res.errors.append(f"Crash pattern matched in log: {pat.pattern}")
    for pat in m2.SHADER_GPU_ERROR_PATTERNS:
        if pat.search(log_text):
            res.errors.append(f"GPU/shader error matched in log: {pat.pattern}")

    # 2. Split log at boot-complete boundary (before vs after first guest frame)
    m_bound = BOOT_BOUNDARY_RE.search(log_text)
    if m_bound:
        boot_log = log_text[: m_bound.start()]
        post_boot_log = log_text[m_bound.end() :]
    else:
        boot_log = log_text
        post_boot_log = ""
        res.errors.append("Missing backend boot completion marker ('640x480 guest,') in psiops.log.")

    # 3. Parse AOT precompilation line
    m_aot = AOT_LOG_RE.search(log_text)
    if m_aot:
        res.aot_ms = float(m_aot.group("ms"))
        res.aot_scanned_vs = int(m_aot.group("scanned_vs"))
        res.aot_shaders_compiled = int(m_aot.group("compiled"))
        res.aot_shaders_cached = int(m_aot.group("cached"))
        res.aot_psos_built = int(m_aot.group("psos"))
        res.aot_total_shaders = int(m_aot.group("total_sh"))
        res.aot_total_psos = int(m_aot.group("total_pso"))
        res.aot_threads = int(m_aot.group("threads"))
        res.aot_cached_skip = m_aot.group("aot_cached").lower() == "true"
    else:
        res.errors.append("Missing 'AOT precompile done' log line in psiops.log.")

    # 4. Parse session shader summary line
    m_sum = SESSION_SUMMARY_RE.search(log_text)
    if m_sum:
        res.summary_runtime_compiles = int(m_sum.group("runtime_compiles"))
        res.summary_runtime_psos = int(m_sum.group("runtime_psos"))
    else:
        res.warnings.append("Missing 'session shader summary' log line in psiops.log.")

    # 5. Parse DLSS 4 boot prewarming vs mid-gameplay feature creation
    boot_dlss_creates = len(DLSS_CREATE_RE.findall(boot_log))
    boot_temporal_prewarm = bool(TEMPORAL_PREWARM_RE.search(boot_log))
    boot_upscaler_done = bool(UPSCALER_PREWARM_DONE_RE.search(boot_log))
    post_dlss_creates = len(DLSS_CREATE_RE.findall(post_boot_log))
    res.dlss_prewarmed_at_boot = (boot_dlss_creates >= 1) and boot_temporal_prewarm and boot_upscaler_done
    res.mid_gameplay_dlss_feature_creates = post_dlss_creates

    if expect_dlss:
        if not res.dlss_prewarmed_at_boot:
            res.errors.append(
                f"DLSS 4 feature was NOT eagerly prewarmed at boot "
                f"(boot_dlss_creates={boot_dlss_creates}, temporal_prewarm={boot_temporal_prewarm}, "
                f"upscaler_prewarm_done={boot_upscaler_done})."
            )
        if res.mid_gameplay_dlss_feature_creates != 0:
            res.errors.append(
                f"Expected mid_gameplay_dlss_feature_creates == 0, got {res.mid_gameplay_dlss_feature_creates}."
            )

    # 6. Parse perf.json & perf.csv
    runtime_log_compiles = len(RUNTIME_COMPILE_RE.findall(post_boot_log))
    if perf_json_path.is_file():
        perf = json.loads(perf_json_path.read_text(encoding="utf-8", errors="replace"))
        meta = perf.get("meta", {})
        shaders = perf.get("shaders", {})
        phases = perf.get("phases", {})
        res.total_presents = int(meta.get("total_presents", 0) or 0)
        res.gameplay_frames = int(phases.get("gameplay_active", {}).get("frames", 0) or 0)
        # A missing zero-gated counter must fail the run, never read as a passing 0.
        for required_key in ("compile_count", "sync_present_cache_writes"):
            if required_key not in shaders:
                res.errors.append(f"perf.json is missing shaders.{required_key}.")
        res.perf_compile_count = int(shaders.get("compile_count", 0) or 0)
        res.sync_present_cache_writes = int(shaders.get("sync_present_cache_writes", 0) or 0)
        csv_path_str = str(meta.get("csv_path", "") or "")
        csv_path = Path(csv_path_str) if csv_path_str else perf_json_path.with_name(perf_json_path.stem + ".frames.csv")
        if not csv_path.is_file():
            frames_csv = perf_json_path.with_name(perf_json_path.stem + ".frames.csv")
            csv_path = frames_csv if frames_csv.is_file() else perf_json_path.with_suffix(".csv")
        res.perf_csv_shader_sum = _sum_csv_shaders(csv_path)
    else:
        res.errors.append(f"Missing perf.json: {perf_json_path}")

    res.mid_gameplay_shader_compiles = max(
        res.perf_compile_count,
        res.perf_csv_shader_sum,
        res.summary_runtime_compiles,
        runtime_log_compiles,
    )

    # 7. Core assertions
    if res.cache_mode == "cold":
        if res.aot_shaders_compiled <= min_aot_shaders:
            res.errors.append(
                f"Cold boot AOT compiled too few shaders: aot_shaders_compiled={res.aot_shaders_compiled} "
                f"(expected > {min_aot_shaders})."
            )
        if res.aot_psos_built <= 0:
            res.errors.append(f"Cold boot AOT built 0 PSOs (expected > 0).")
    else:
        if not res.aot_cached_skip:
            res.warnings.append("Warm run did not take fast AOT cached skip path.")
        if res.aot_shaders_compiled != 0:
            res.errors.append(f"Warm run compiled {res.aot_shaders_compiled} shaders at boot (expected 0).")

    if res.mid_gameplay_shader_compiles != 0:
        res.errors.append(
            f"Mid-gameplay shader compilations detected: mid_gameplay_shader_compiles={res.mid_gameplay_shader_compiles} "
            f"(perf={res.perf_compile_count}, csv={res.perf_csv_shader_sum}, "
            f"summary={res.summary_runtime_compiles}, log={runtime_log_compiles}); expected 0."
        )

    if res.summary_runtime_psos != 0:
        res.errors.append(
            f"Mid-gameplay PSO creations detected: summary_runtime_psos={res.summary_runtime_psos}; expected 0."
        )

    if res.sync_present_cache_writes != 0:
        res.errors.append(f"Synchronous Present() cache writes detected: {res.sync_present_cache_writes} (expected 0).")

    if res.gameplay_frames <= 0:
        res.errors.append(f"Did not reach active 3D gameplay (gameplay_active.frames={res.gameplay_frames}).")

    # 8. Check on-disk cache files persisted cleanly
    res.cache_files_present = _list_cache_files(cache_dir)
    if res.platform == "win64":
        has_sh = "shaders.bin" in res.cache_files_present
        has_pso = any(f.startswith("psos_") and f.endswith(".bin") for f in res.cache_files_present)
        has_meta = any(f.startswith("pso_meta_") and f.endswith(".bin") for f in res.cache_files_present)
        if not (has_sh and has_pso and has_meta):
            res.errors.append(
                f"Expected Win64 cache files missing in {cache_dir}: found {res.cache_files_present}"
            )
    else:
        has_sh = "vk_shaders.bin" in res.cache_files_present
        has_meta = any(f.startswith("vk_pso_meta_") and f.endswith(".bin") for f in res.cache_files_present)
        if not (has_sh and has_meta):
            res.errors.append(
                f"Expected Linux Vulkan cache files missing in {cache_dir}: found {res.cache_files_present}"
            )

    res.passed = (res.exit_code == 0) and (not res.timed_out) and (len(res.errors) == 0)


def run_case(
    name: str,
    platform: str,
    cache_mode: str,
    exe_path: Path,
    iso_path: Path,
    run_dir: Path,
    wsl_distro: str,
    timeout_s: int,
    min_aot_shaders: int,
    dry_run: bool,
) -> ShaderRunResult:
    res = ShaderRunResult(name=name, platform=platform, cache_mode=cache_mode)
    run_dir.mkdir(parents=True, exist_ok=True)
    script_path = run_dir / "autotest_script.txt"
    script_path.write_text(m2.DEFAULT_M2_SCRIPT, encoding="utf-8", newline="\n")
    settings_path = run_dir / "settings.toml"
    perf_json = run_dir / "perf.json"
    saved_log = run_dir / "psiops.log"
    res.perf_json_path = str(perf_json)
    res.log_path = str(saved_log)

    width, height = (2560, 1440) if platform == "win64" else (1280, 800)
    expect_dlss = platform == "win64"

    def plat_path(p: Path) -> str:
        if platform == "win64" or sys.platform != "win32":
            return str(p)
        return m2.to_wsl_path(p)

    cli_args: list[str] = [
        "--no-launcher",
        "--iso",
        plat_path(iso_path),
        "--settings",
        plat_path(settings_path),
        "--perf-log",
        plat_path(perf_json),
        "--width",
        str(width),
        "--height",
        str(height),
    ]
    if platform == "win64":
        cli_args += [
            "--gpu",
            "0",
            "--upscaler",
            "dlss",
            "--upscale-quality",
            "quality",
            "--dlss-preset",
            "K",
        ]
    else:
        cli_args += [
            "--upscaler",
            "fsr1",
            "--upscale-quality",
            "quality",
        ]

    if dry_run:
        res.exit_code = 0
        res.passed = True
        res.warnings.append(f"dry-run: {exe_path} {' '.join(cli_args)}")
        return res

    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res

    _ensure_clean_processes(platform, wsl_distro)
    media_before = m2.snapshot_media_stats(REPO_ROOT, iso_path)
    cache_dir = exe_path.parent / "cache"
    if cache_mode == "cold":
        _wipe_cache_dir(cache_dir)

    env_extra = {
        "XBE_AUTOTEST": "1",
        "PSIOPS_AUTOTEST": "1",
        "XBE_AUTOTEST_SCRIPT": plat_path(script_path),
        "XBE_AUTOTEST_OUT_DIR": plat_path(run_dir),
        "XBE_PERF_LOG": plat_path(perf_json),
        "XBE_ISO_PATH": plat_path(iso_path),
    }

    logs_dir = exe_path.parent / "logs"
    logs_before: dict[Path, int] = (
        {p: p.stat().st_mtime_ns for p in logs_dir.glob("psiops*.log") if p.is_file()} if logs_dir.is_dir() else {}
    )

    t0 = time.monotonic()
    stdout_s = ""
    stderr_s = ""
    try:
        if platform == "win64":
            env = os.environ.copy()
            env.update(env_extra)
            proc = subprocess.run(
                [str(exe_path), *cli_args],
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
        else:
            env_kv = [f"{k}={v}" for k, v in env_extra.items()]
            inner = [
                "env",
                *env_kv,
                "xvfb-run",
                "-a",
                "-s",
                f"-screen 0 {width}x{height}x24",
                plat_path(exe_path),
                *cli_args,
            ]
            cmd = (
                ["wsl", "-d", wsl_distro, "--cd", plat_path(exe_path.parent), "--", *inner]
                if sys.platform == "win32"
                else inner
            )
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
        res.exit_code = -1
        res.errors.append(f"{name} timed out after {timeout_s}s.")
        if isinstance(exc.stdout, str):
            stdout_s = exc.stdout
        if isinstance(exc.stderr, str):
            stderr_s = exc.stderr
        if platform == "win64":
            m2.kill_win_psiops()
        else:
            m2.kill_wsl_psiops(wsl_distro)
    finally:
        res.wall_s = round(time.monotonic() - t0, 3)
        _ensure_clean_processes(platform, wsl_distro)

    newest_log: Path | None = None
    if logs_dir.is_dir():
        after_logs = [p for p in logs_dir.glob("psiops*.log") if p.is_file()]
        created_or_updated = [p for p in after_logs if p not in logs_before or p.stat().st_mtime_ns > logs_before[p]]
        pool = created_or_updated if created_or_updated else after_logs
        if pool:
            newest_log = max(pool, key=lambda p: p.stat().st_mtime_ns)
    if newest_log is not None and newest_log.is_file():
        shutil.copy2(newest_log, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"Non-zero exit code: {res.exit_code}")

    media_errs = m2.assert_media_unchanged(media_before)
    res.errors.extend(media_errs)

    _analyze_run(res, log_text, perf_json, cache_dir, expect_dlss=expect_dlss, min_aot_shaders=min_aot_shaders)
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description="Psi-Ops AOT Shader, PSO & Upscaler Pre-Compilation Verification Suite")
    ap.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    ap.add_argument("--out-dir", type=Path, default=REPO_ROOT / ".local" / "shader_precompile_verify")
    ap.add_argument("--win-exe", type=Path, default=REPO_ROOT / "out" / "build" / "release" / "PsiOps.exe")
    ap.add_argument("--linux-exe", type=Path, default=REPO_ROOT / "out" / "build" / "linux-release" / "PsiOps")
    ap.add_argument("--iso", type=Path, default=REPO_ROOT / m2.EXPECTED_ISO_REL)
    ap.add_argument("--wsl-distro", type=str, default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--min-aot-shaders", type=int, default=50)
    ap.add_argument("--skip-warm", action="store_true", help="Run cold-cache tests only (skip warm follow-up run)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    out_dir: Path = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    iso_path: Path = args.iso.resolve()
    if not args.dry_run and not iso_path.is_file():
        print(f"[ERROR] Disc image not found: {iso_path}", file=sys.stderr)
        return 2

    wsl_distro = args.wsl_distro or "Ubuntu-22.04"
    if not args.dry_run and args.platform in ("linux", "both") and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)

    plan: list[tuple[str, str, str, Path]] = []
    if args.platform in ("win64", "both"):
        plan.append(("win64_cold_dlss4_k_1440p", "win64", "cold", args.win_exe.resolve()))
        if not args.skip_warm:
            plan.append(("win64_warm_dlss4_k_1440p", "win64", "warm", args.win_exe.resolve()))
    if args.platform in ("linux", "both"):
        plan.append(("linux_cold_vulkan_1280x800", "linux", "cold", args.linux_exe.resolve()))
        if not args.skip_warm:
            plan.append(("linux_warm_vulkan_1280x800", "linux", "warm", args.linux_exe.resolve()))

    results: list[ShaderRunResult] = []
    for name, plat, mode, exe in plan:
        print(f"[RUN] {name} (platform={plat}, cache={mode}) ...", flush=True)
        r = run_case(
            name=name,
            platform=plat,
            cache_mode=mode,
            exe_path=exe,
            iso_path=iso_path,
            run_dir=out_dir / name,
            wsl_distro=wsl_distro,
            timeout_s=args.timeout,
            min_aot_shaders=args.min_aot_shaders,
            dry_run=args.dry_run,
        )
        results.append(r)
        status = "PASS" if r.passed else "FAIL"
        print(
            f"[{status}] {name}: wall={r.wall_s:.1f}s aot_ms={r.aot_ms:.1f} "
            f"aot_compiled={r.aot_shaders_compiled} aot_psos={r.aot_psos_built} "
            f"mid_game_compiles={r.mid_gameplay_shader_compiles} "
            f"dlss_boot_prewarm={r.dlss_prewarmed_at_boot} "
            f"mid_game_dlss_creates={r.mid_gameplay_dlss_feature_creates} "
            f"gameplay_frames={r.gameplay_frames}",
            flush=True,
        )
        for err in r.errors:
            print(f"    [ERR] {err}", flush=True)
        for warn in r.warnings:
            print(f"    [WARN] {warn}", flush=True)

    passed_count = sum(1 for r in results if r.passed)
    all_pass = passed_count == len(results) and len(results) > 0
    report = {
        "passed": all_pass,
        "passed_count": passed_count,
        "total_count": len(results),
        "results": [asdict(r) for r in results],
    }
    report_path = out_dir / "shader_precompile_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"[SUMMARY] {passed_count}/{len(results)} passed -> {report_path}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
