#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 5 live GPU hardware verification suite for all Psi-Ops upscalers.

Verifies the complete Milestone 5 upscaling pipeline across BOTH platforms:

  1. Plugin Binary Integrity Gate (via tools/fetch_upscalers.py):
     Pinned SHA-256 + PE32+ (0x8664) + offline WinVerifyTrust Authenticode +
     0 forbidden dev-watermark hits for all 4 retail DLLs + 3 vendor licenses.

  2. Windows x64 D3D12 (RTX 3090 / GPU 0) — ALL 9 Upscalers + Presets + Fallback:
       - 6 built-in spatial upscalers:
           fsr1 (default), nis, cas, bicubic (upscale), bicubic_ssaa (150% SSAA),
           integer, bilinear
       - 3 official vendor-signed retail temporal plugins (must activate live
         vendor backend — silent spatial fallback is a hard FAIL):
           dlss (Presets auto, E, J, K + DLAA 100%), fsr3 (FSR 3.1 / 4), xess (XeSS 2)
       - NGX driver-cache automatic fallback test:
           Temporarily moves plugins/nvngx_dlss.dll aside in a guaranteed
           try/finally restore block, clears cache/ngx/nvngx_dlss.dll, launches
           with --upscaler dlss, and verifies Authenticode-verified auto-copy
           from %ProgramData%\\NVIDIA\\NGX\\models\\dlss\\ -> cache/ngx/nvngx_dlss.dll
           plus live DLSS 4 activation.

  3. Native Linux x86_64 Vulkan 1.2 (WSL2 Ubuntu 22.04 + xvfb-run 1280x800):
       - All 6 built-in spatial upscalers (fsr1, nis, cas, bicubic, bicubic_ssaa,
         integer, bilinear) with 0 Vulkan/glslang errors.
       - Graceful temporal -> fsr1 fallback for dlss, fsr3, and xess.
"""

from __future__ import annotations

import argparse
import hashlib
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
import fetch_upscalers as fu  # noqa: E402
import test_m2_gameplay as m2  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parent.parent

# Deterministic scripted controller timeline reaching active 3D gameplay
# (Solitary Confinement) with ~3s of live camera pan + movement so temporal
# history, Halton (2,3) jitter, and RG16F camera motion vectors stabilize
# before capturing gameplay.bmp and exiting cleanly.
DEFAULT_UPSCALER_SCRIPT = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops Milestone 5 live upscaler 3D gameplay capture timeline
# 1. Boot FMVs (~20.5s) -> Title Screen -> START -> Main Menu -> New Game -> Normal
pad_ms 26400 26800 START
pad_ms 29000 29400 A
pad_ms 30800 31200 A

# 2. Skip intro FMVs into Level 1 Solitary Confinement
pad_ms 35000 35350 START
pad_ms 36400 36750 START
pad_ms 37800 38150 START
pad_ms 45500 45850 START

# 3. Dismiss Mission Objectives popup ("Accept (A)")
pad_ms 50000 50350 A
pad_ms 51200 51550 A
pad_ms 52400 52750 A

# 4. Active 3D gameplay motion (~180 frames @ 60 FPS) to exercise Halton jitter + MVs
pad_ms 53200 54600 none 0 22000 12000 -1000 0 0
pad_ms 54800 55800 none 12000 10000 -14000 500 0 0

# 5. Capture active 3D gameplay + HUD frame and exit cleanly
capture_ms 56200 gameplay.bmp
exit_ms 57200
"""

# Structured log fallback regexes (used if perf.json lacks an "upscaler" object)
UPSCALER_LOG_RE = re.compile(
    r"upscaler:\s*requested=(?P<req>[a-z0-9_+-]+)\s+active=(?P<act>[a-z0-9_+-]+)",
    re.IGNORECASE,
)
UPSCALER_RES_RE = re.compile(
    r"render=(?P<rw>\d+)x(?P<rh>\d+)\s*(?:->|to)\s*(?:display|output|bb)=(?P<dw>\d+)x(?P<dh>\d+)",
    re.IGNORECASE,
)
UPSCALER_PRESET_RE = re.compile(r"dlss_preset=(?P<preset>[A-Za-z0-9_]+)", re.IGNORECASE)

UPSCALER_ERROR_PATTERNS = (
    re.compile(r"\bNGX.*failed\b", re.IGNORECASE),
    re.compile(r"\bFFX.*failed\b", re.IGNORECASE),
    re.compile(r"\bXeSS.*failed\b", re.IGNORECASE),
    re.compile(r"upscaler:.*\b(fatal|crash|corrupt|device\s+removed)\b", re.IGNORECASE),
)

CANONICAL_BACKEND_MAP = {
    "fsr1": "fsr1",
    "fsr_1": "fsr1",
    "fsr1.0": "fsr1",
    "nis": "nis",
    "cas": "cas",
    "bicubic": "bicubic",
    "bicubic5": "bicubic",
    "bicubic_5tap": "bicubic",
    "integer": "integer",
    "nearest": "integer",
    "bilinear": "bilinear",
    "dlss": "dlss",
    "dlss4": "dlss",
    "dlaa": "dlss",
    "fsr3": "fsr3",
    "fsr3.1": "fsr3",
    "fsr31": "fsr3",
    "fsr4": "fsr3",
    "xess": "xess",
    "xess2": "xess",
}


@dataclass(frozen=True)
class UpscalerCase:
    case_id: str
    platform: str
    requested: str
    expected_active: str
    quality: str = "quality"
    dlss_preset: str = ""
    sharpness: float | None = None
    render_scale_pct: int | None = None
    expect_fallback: bool = False
    scale_relation: str = "upscale"  # "upscale" (<), "native" (==), "ssaa" (>), "any"
    extra_args: tuple[str, ...] = ()
    ngx_cache_fallback_test: bool = False


def build_win64_cases(include_ngx_fallback: bool = True) -> list[UpscalerCase]:
    cases = [
        # 6 built-in spatial upscalers (including >100% SSAA bicubic)
        UpscalerCase("win64_fsr1", "win64", "fsr1", "fsr1", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("win64_nis", "win64", "nis", "nis", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("win64_cas", "win64", "cas", "cas", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("win64_bicubic", "win64", "bicubic", "bicubic", quality="quality", scale_relation="upscale"),
        UpscalerCase("win64_bicubic_ssaa_150", "win64", "bicubic", "bicubic", quality="custom", render_scale_pct=150, scale_relation="ssaa"),
        UpscalerCase("win64_integer", "win64", "integer", "integer", quality="quality", scale_relation="upscale"),
        UpscalerCase("win64_bilinear", "win64", "bilinear", "bilinear", quality="quality", scale_relation="upscale"),
        # NVIDIA DLSS 4 — Presets Auto, E, J, K + DLAA 100%
        UpscalerCase("win64_dlss_auto", "win64", "dlss", "dlss", quality="quality", dlss_preset="auto", scale_relation="upscale"),
        UpscalerCase("win64_dlss_preset_e", "win64", "dlss", "dlss", quality="balanced", dlss_preset="E", scale_relation="upscale"),
        UpscalerCase("win64_dlss_preset_j", "win64", "dlss", "dlss", quality="performance", dlss_preset="J", scale_relation="upscale"),
        UpscalerCase("win64_dlss_preset_k", "win64", "dlss", "dlss", quality="quality", dlss_preset="K", scale_relation="upscale"),
        UpscalerCase("win64_dlss_dlaa_100", "win64", "dlss", "dlss", quality="dlaa", dlss_preset="K", render_scale_pct=100, scale_relation="native"),
        # AMD FSR 3.1 / 4 & Intel XeSS 2
        UpscalerCase("win64_fsr3", "win64", "fsr3", "fsr3", quality="quality", scale_relation="upscale"),
        UpscalerCase("win64_xess", "win64", "xess", "xess", quality="quality", scale_relation="upscale"),
    ]
    if include_ngx_fallback:
        cases.append(
            UpscalerCase(
                "win64_dlss_ngx_cache_fallback",
                "win64",
                "dlss",
                "dlss",
                quality="quality",
                dlss_preset="auto",
                scale_relation="upscale",
                ngx_cache_fallback_test=True,
            )
        )
    return cases


def build_linux_cases() -> list[UpscalerCase]:
    return [
        # All 6 built-in spatial upscalers on Linux Vulkan 1.2 (Steam Deck 1280x800)
        UpscalerCase("linux_fsr1", "linux", "fsr1", "fsr1", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("linux_nis", "linux", "nis", "nis", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("linux_cas", "linux", "cas", "cas", quality="quality", sharpness=0.5, scale_relation="upscale"),
        UpscalerCase("linux_bicubic", "linux", "bicubic", "bicubic", quality="quality", scale_relation="upscale"),
        UpscalerCase("linux_bicubic_ssaa_150", "linux", "bicubic", "bicubic", quality="custom", render_scale_pct=150, scale_relation="ssaa"),
        UpscalerCase("linux_integer", "linux", "integer", "integer", quality="quality", scale_relation="upscale"),
        UpscalerCase("linux_bilinear", "linux", "bilinear", "bilinear", quality="quality", scale_relation="upscale"),
        # Graceful temporal -> FSR 1.0 fallback on Linux Vulkan 1.2
        UpscalerCase("linux_dlss_fallback_fsr1", "linux", "dlss", "fsr1", quality="quality", expect_fallback=True, scale_relation="upscale"),
        UpscalerCase("linux_fsr3_fallback_fsr1", "linux", "fsr3", "fsr1", quality="quality", expect_fallback=True, scale_relation="upscale"),
        UpscalerCase("linux_xess_fallback_fsr1", "linux", "xess", "fsr1", quality="quality", expect_fallback=True, scale_relation="upscale"),
    ]


@dataclass
class CaseRunResult:
    case_id: str
    platform: str
    requested: str
    expected_active: str
    actual_active: str = ""
    fallback_triggered: bool = False
    dlss_preset_requested: str = ""
    dlss_preset_actual: str = ""
    render_w: int = 0
    render_h: int = 0
    display_w: int = 0
    display_h: int = 0
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    screenshot_png: str = ""
    screenshot_sha256: str = ""
    screenshot_luma: float = 0.0
    screenshot_non_black_pct: float = 0.0
    ngx_cache_verified: bool | None = None
    log_path: str = ""
    perf_json_path: str = ""
    passed: bool = False
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def _canonical_backend(name: str) -> str:
    key = re.sub(r"[^a-z0-9._+-]", "", name.strip().lower())
    return CANONICAL_BACKEND_MAP.get(key, key)


def _parse_resolution(spec: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+)x(\d+)", spec.strip().lower())
    if not m:
        raise ValueError(f"Invalid resolution '{spec}', expected WxH (e.g. 1920x1080)")
    return int(m.group(1)), int(m.group(2))


def _analyze_capture(bmp_path: Path, png_path: Path) -> tuple[bool, dict[str, Any], str]:
    if not bmp_path.is_file() or bmp_path.stat().st_size < 54:
        return False, {}, f"Capture missing or empty: {bmp_path}"
    try:
        w, h, rgb = m2._read_bmp_rgb_stdlib(bmp_path)
    except Exception as exc:
        return False, {}, f"Failed to decode BMP {bmp_path}: {exc}"

    total_px = max(1, w * h)
    luma_sum = 0.0
    non_black = 0
    # Sample every 4th pixel for speed on high-res captures.
    step = 4
    sampled = 0
    for idx in range(0, len(rgb), 3 * step):
        r, g, b = rgb[idx], rgb[idx + 1], rgb[idx + 2]
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        luma_sum += y
        if r > 8 or g > 8 or b > 8:
            non_black += 1
        sampled += 1
    mean_luma = luma_sum / max(1, sampled)
    non_black_pct = (100.0 * non_black) / max(1, sampled)

    # Convert BMP -> PNG if PIL is installed; otherwise retain BMP path.
    out_img = bmp_path
    if m2.Image is not None:
        try:
            with m2.Image.open(bmp_path) as im:
                im.convert("RGB").save(png_path, format="PNG")
            out_img = png_path
        except Exception:
            out_img = bmp_path

    sha = hashlib.sha256(out_img.read_bytes()).hexdigest()
    info = {
        "path": str(out_img),
        "width": w,
        "height": h,
        "mean_luma": round(mean_luma, 2),
        "non_black_pct": round(non_black_pct, 2),
        "sha256": sha,
    }
    if w < 320 or h < 200:
        return False, info, f"Invalid capture dimensions {w}x{h}"
    if mean_luma < 3.0 or non_black_pct < 5.0:
        return False, info, f"Capture appears black/blank (mean_luma={mean_luma:.2f}, non_black={non_black_pct:.2f}%)"
    return True, info, ""


def _extract_upscaler_telemetry(perf: dict[str, Any], log_text: str) -> dict[str, Any]:
    out: dict[str, Any] = {
        "requested": "",
        "active": "",
        "fallback": False,
        "dlss_preset": "",
        "render_w": 0,
        "render_h": 0,
        "display_w": 0,
        "display_h": 0,
    }
    up = perf.get("upscaler")
    if isinstance(up, dict):
        out["requested"] = _canonical_backend(str(up.get("requested", "")))
        out["active"] = _canonical_backend(str(up.get("active", up.get("backend", ""))))
        out["fallback"] = bool(up.get("fallback", False))
        out["dlss_preset"] = str(up.get("dlss_preset", up.get("preset", "")))
        out["render_w"] = int(up.get("render_w", up.get("render_width", 0)) or 0)
        out["render_h"] = int(up.get("render_h", up.get("render_height", 0)) or 0)
        out["display_w"] = int(up.get("display_w", up.get("display_width", up.get("output_w", 0))) or 0)
        out["display_h"] = int(up.get("display_h", up.get("display_height", up.get("output_h", 0))) or 0)

    if not out["active"] and log_text:
        m = UPSCALER_LOG_RE.search(log_text)
        if m:
            out["requested"] = _canonical_backend(m.group("req"))
            out["active"] = _canonical_backend(m.group("act"))
    if (out["render_w"] == 0 or out["display_w"] == 0) and log_text:
        mr = UPSCALER_RES_RE.search(log_text)
        if mr:
            out["render_w"] = int(mr.group("rw"))
            out["render_h"] = int(mr.group("rh"))
            out["display_w"] = int(mr.group("dw"))
            out["display_h"] = int(mr.group("dh"))
    if not out["dlss_preset"] and log_text:
        mp = UPSCALER_PRESET_RE.search(log_text)
        if mp:
            out["dlss_preset"] = mp.group("preset")
    if out["requested"] and out["active"] and out["requested"] != out["active"]:
        out["fallback"] = True
    return out


def _ensure_clean_processes(platform: str, wsl_distro: str) -> None:
    if platform == "win64":
        if m2.get_running_win_psiops():
            m2.kill_win_psiops()
            time.sleep(0.5)
    else:
        if m2.get_running_wsl_psiops(wsl_distro):
            m2.kill_wsl_psiops(wsl_distro)
            time.sleep(0.5)


def _build_case_cli_args(
    case: UpscalerCase,
    iso_arg: str,
    settings_arg: str,
    perf_arg: str,
    width: int,
    height: int,
    rec_arg: str | None,
) -> list[str]:
    args: list[str] = [
        "--no-launcher",
        "--iso",
        iso_arg,
        "--settings",
        settings_arg,
        "--perf-log",
        perf_arg,
        "--width",
        str(width),
        "--height",
        str(height),
        "--upscaler",
        case.requested,
        "--upscale-quality",
        case.quality,
    ]
    if case.platform == "win64":
        args += ["--gpu", "0"]
    if case.dlss_preset:
        args += ["--dlss-preset", case.dlss_preset]
    if case.sharpness is not None:
        args += ["--upscale-sharpness", f"{case.sharpness:.2f}"]
    if case.render_scale_pct is not None:
        args += ["--render-scale", str(case.render_scale_pct)]
    if rec_arg:
        args += ["--replay", rec_arg, "--replay-exit"]
    args.extend(case.extra_args)
    return args


def _run_single_case(
    case: UpscalerCase,
    exe_path: Path,
    iso_path: Path,
    case_dir: Path,
    width: int,
    height: int,
    wsl_distro: str,
    timeout_s: int,
    rec_path: Path | None,
    dry_run: bool,
) -> CaseRunResult:
    res = CaseRunResult(
        case_id=case.case_id,
        platform=case.platform,
        requested=case.requested,
        expected_active=case.expected_active,
        dlss_preset_requested=case.dlss_preset,
    )
    case_dir.mkdir(parents=True, exist_ok=True)
    settings_path = case_dir / "settings.toml"
    perf_path = case_dir / "perf.json"
    log_path = case_dir / "run.log"
    script_path = case_dir / "autotest.txt"
    capture_dir = case_dir / "captures"
    capture_dir.mkdir(parents=True, exist_ok=True)
    res.log_path = str(log_path)
    res.perf_json_path = str(perf_path)

    if rec_path is None:
        script_path.write_text(DEFAULT_UPSCALER_SCRIPT, encoding="utf-8", newline="\n")

    def plat_path(p: Path) -> str:
        if case.platform == "win64" or sys.platform != "win32":
            return str(p)
        return m2.to_wsl_path(p)

    cli_args = _build_case_cli_args(
        case=case,
        iso_arg=plat_path(iso_path),
        settings_arg=plat_path(settings_path),
        perf_arg=plat_path(perf_path),
        width=width,
        height=height,
        rec_arg=plat_path(rec_path) if rec_path is not None else None,
    )

    env_extra: dict[str, str] = {
        "XBE_AUTOTEST": "1",
        "XBE_AUTOTEST_OUT_DIR": plat_path(capture_dir),
        "XBE_CAPTURE_DIR": plat_path(capture_dir),
    }
    if rec_path is None:
        env_extra["XBE_AUTOTEST_SCRIPT"] = plat_path(script_path)

    if dry_run:
        res.passed = True
        res.actual_active = case.expected_active
        res.warnings.append(f"dry-run: {' '.join([str(exe_path)] + cli_args)}")
        return res

    # If testing Win64 NGX driver-cache fallback, temporarily move ALL local
    # plugins/nvngx_dlss.dll copies aside inside a guaranteed try/finally block.
    moved_backups: list[tuple[Path, Path]] = []
    cache_candidates = [
        exe_path.parent / "cache" / "ngx" / "nvngx_dlss.dll",
        REPO_ROOT / "cache" / "ngx" / "nvngx_dlss.dll",
    ]
    plugin_candidates = list(
        dict.fromkeys(
            [
                (REPO_ROOT / "plugins" / "nvngx_dlss.dll").resolve(),
                (exe_path.parent / "plugins" / "nvngx_dlss.dll").resolve(),
                (exe_path.parent / "nvngx_dlss.dll").resolve(),
            ]
        )
    )

    _ensure_clean_processes(case.platform, wsl_distro)
    try:
        if case.ngx_cache_fallback_test and case.platform == "win64":
            for p_dll in plugin_candidates:
                if p_dll.is_file():
                    bak = p_dll.with_name(p_dll.name + ".m5_ngx_test_bak")
                    shutil.move(str(p_dll), str(bak))
                    moved_backups.append((bak, p_dll))
            for c_dll in cache_candidates:
                if c_dll.is_file():
                    c_dll.unlink()

        t0 = time.monotonic()
        if case.platform == "win64":
            env = os.environ.copy()
            env.update(env_extra)
            try:
                proc = subprocess.run(
                    [str(exe_path)] + cli_args,
                    cwd=str(exe_path.parent),
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=timeout_s,
                    check=False,
                )
                res.exit_code = proc.returncode
                combined = (proc.stdout or "") + "\n" + (proc.stderr or "")
            except subprocess.TimeoutExpired as exc:
                res.timed_out = True
                res.exit_code = -1
                combined = ((exc.stdout or "") if isinstance(exc.stdout, str) else "") + "\n" + (
                    (exc.stderr or "") if isinstance(exc.stderr, str) else ""
                )
                m2.kill_win_psiops()
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
                combined = (proc.stdout or "") + "\n" + (proc.stderr or "")
            except subprocess.TimeoutExpired as exc:
                res.timed_out = True
                res.exit_code = -1
                combined = ((exc.stdout or "") if isinstance(exc.stdout, str) else "") + "\n" + (
                    (exc.stderr or "") if isinstance(exc.stderr, str) else ""
                )
                m2.kill_wsl_psiops(wsl_distro)
        res.wall_s = round(time.monotonic() - t0, 3)
        log_path.write_text(combined, encoding="utf-8", errors="replace", newline="\n")

        # Validate NGX driver-cache copy BEFORE restoring plugins/nvngx_dlss.dll.
        if case.ngx_cache_fallback_test and case.platform == "win64":
            found_cache = next((c for c in cache_candidates if c.is_file()), None)
            if found_cache is None:
                res.ngx_cache_verified = False
                res.errors.append(
                    "NGX driver-cache fallback did not populate cache/ngx/nvngx_dlss.dll"
                )
            else:
                pe_ok, pe_msg, _ = fu.inspect_pe64(found_cache)
                auth_ok, auth_msg = fu.verify_authenticode_win32(found_cache, "NVIDIA")
                wm_hits = fu.scan_watermarks(found_cache)
                res.ngx_cache_verified = pe_ok and auth_ok and len(wm_hits) == 0
                if not pe_ok:
                    res.errors.append(f"NGX cached DLL PE64 failed: {pe_msg}")
                if not auth_ok:
                    res.errors.append(f"NGX cached DLL Authenticode failed: {auth_msg}")
                if wm_hits:
                    res.errors.append(f"NGX cached DLL contains dev watermark: {wm_hits}")
    finally:
        # Always restore moved plugin backups and enforce zero lingering PsiOps processes.
        for bak, orig in reversed(moved_backups):
            if bak.is_file():
                if orig.exists():
                    orig.unlink()
                shutil.move(str(bak), str(orig))
        _ensure_clean_processes(case.platform, wsl_distro)

    # Evaluate exit status and log cleanliness.
    if res.timed_out:
        res.errors.append(f"Process timed out after {timeout_s}s")
    if res.exit_code != 0:
        res.errors.append(f"Non-zero exit code: {res.exit_code}")

    for pat in m2.CRASH_PATTERNS + m2.SHADER_GPU_ERROR_PATTERNS + UPSCALER_ERROR_PATTERNS:
        m_err = pat.search(combined)
        if m_err:
            res.errors.append(f"Log error matched /{pat.pattern}/: {m_err.group(0)!r}")

    # Parse perf.json + log telemetry.
    perf_data: dict[str, Any] = {}
    if perf_path.is_file():
        try:
            perf_data = json.loads(perf_path.read_text(encoding="utf-8", errors="replace"))
        except Exception as exc:
            res.errors.append(f"Failed to parse perf.json: {exc}")
    else:
        res.errors.append(f"Missing perf.json: {perf_path}")

    tele = _extract_upscaler_telemetry(perf_data, combined)
    res.actual_active = tele["active"]
    res.fallback_triggered = bool(tele["fallback"])
    res.dlss_preset_actual = tele["dlss_preset"]
    res.render_w = tele["render_w"]
    res.render_h = tele["render_h"]
    res.display_w = tele["display_w"]
    res.display_h = tele["display_h"]

    if not res.actual_active:
        res.errors.append("Active upscaler backend not reported in perf.json or stdout log")
    elif res.actual_active != case.expected_active:
        res.errors.append(
            f"Backend mismatch: requested={case.requested}, expected_active={case.expected_active}, "
            f"got actual_active={res.actual_active}"
        )
    if case.expect_fallback and not res.fallback_triggered:
        res.errors.append(f"Expected graceful fallback to {case.expected_active}, but fallback flag was false")
    if not case.expect_fallback and res.fallback_triggered:
        res.errors.append(
            f"Unexpected silent fallback occurred (requested={case.requested}, active={res.actual_active})"
        )

    if res.render_w <= 0 or res.render_h <= 0 or res.display_w <= 0 or res.display_h <= 0:
        res.errors.append(
            f"Missing decoupled resolution telemetry (render={res.render_w}x{res.render_h}, "
            f"display={res.display_w}x{res.display_h})"
        )
    else:
        if case.scale_relation == "upscale" and not (res.render_w < res.display_w and res.render_h < res.display_h):
            res.errors.append(
                f"Expected decoupled upscaling render < display, got {res.render_w}x{res.render_h} -> {res.display_w}x{res.display_h}"
            )
        elif case.scale_relation == "native" and not (res.render_w == res.display_w and res.render_h == res.display_h):
            res.errors.append(
                f"Expected 100% native/DLAA render == display, got {res.render_w}x{res.render_h} -> {res.display_w}x{res.display_h}"
            )
        elif case.scale_relation == "ssaa" and not (res.render_w > res.display_w and res.render_h > res.display_h):
            res.errors.append(
                f"Expected >100% SSAA render > display, got {res.render_w}x{res.render_h} -> {res.display_w}x{res.display_h}"
            )

    if case.dlss_preset and res.dlss_preset_actual:
        if case.dlss_preset.lower() != "auto" and res.dlss_preset_actual.lower() != case.dlss_preset.lower():
            res.errors.append(
                f"DLSS preset mismatch: requested={case.dlss_preset}, actual={res.dlss_preset_actual}"
            )

    # Validate active 3D gameplay screenshot capture (when autotest capture is active).
    bmp_capture = capture_dir / "gameplay.bmp"
    if not bmp_capture.is_file():
        bmps = sorted(capture_dir.glob("*.bmp"))
        if bmps:
            bmp_capture = bmps[-1]
    if bmp_capture.is_file():
        cap_ok, cap_info, cap_err = _analyze_capture(bmp_capture, case_dir / f"{case.case_id}.png")
        res.screenshot_png = str(cap_info.get("path", ""))
        res.screenshot_sha256 = str(cap_info.get("sha256", ""))
        res.screenshot_luma = float(cap_info.get("mean_luma", 0.0))
        res.screenshot_non_black_pct = float(cap_info.get("non_black_pct", 0.0))
        if not cap_ok:
            res.errors.append(cap_err)
    elif rec_path is None:
        res.errors.append(f"Expected gameplay screenshot capture missing in {capture_dir}")

    res.passed = len(res.errors) == 0
    return res


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Milestone 5 live GPU hardware verification suite for all Psi-Ops upscalers."
    )
    parser.add_argument(
        "--platform",
        choices=("win64", "linux", "both"),
        default="both",
        help="Target platform(s) to test (default: both).",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=REPO_ROOT / ".local" / "m5_upscalers",
        help="Output directory for logs, perf JSONs, captures, and summary report.",
    )
    parser.add_argument(
        "--plugins-dir",
        type=Path,
        default=REPO_ROOT / "plugins",
        help="Directory holding vendor-signed retail x64 DLL plugins.",
    )
    parser.add_argument(
        "--win-exe",
        type=Path,
        default=REPO_ROOT / "out" / "build" / "release" / "PsiOps.exe",
        help="Path to Windows x64 PsiOps.exe.",
    )
    parser.add_argument(
        "--linux-exe",
        type=Path,
        default=REPO_ROOT / "out" / "build" / "linux-release" / "PsiOps",
        help="Path to Native Linux x86_64 PsiOps ELF.",
    )
    parser.add_argument(
        "--iso",
        type=Path,
        default=REPO_ROOT / m2.EXPECTED_ISO_REL,
        help="Path to read-only Psi-Ops .xiso.iso.",
    )
    parser.add_argument(
        "--rec",
        type=Path,
        default=None,
        help="Optional deterministic .rec replay file instead of built-in XBE_AUTOTEST_SCRIPT.",
    )
    parser.add_argument(
        "--resolution",
        default="1920x1080",
        help="Win64 display resolution WxH (default: 1920x1080).",
    )
    parser.add_argument(
        "--linux-resolution",
        default="1280x800",
        help="Linux xvfb/Steam Deck display resolution WxH (default: 1280x800).",
    )
    parser.add_argument(
        "--wsl-distro",
        default=None,
        help="Explicit WSL2 Ubuntu 22.04 distro name (auto-discovered by default).",
    )
    parser.add_argument(
        "--cases",
        default="",
        help="Comma-separated substring filter of case_ids to run (default: all).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Hard per-case timeout in seconds (default: 120).",
    )
    parser.add_argument(
        "--skip-plugin-verify",
        action="store_true",
        help="Skip tools/fetch_upscalers.py plugin binary gate.",
    )
    parser.add_argument(
        "--skip-ngx-fallback",
        action="store_true",
        help="Skip Win64 NGX driver-cache fallback test.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print planned verification matrix and validate plugin binaries without launching PsiOps.",
    )
    args = parser.parse_args(argv)

    win_w, win_h = _parse_resolution(args.resolution)
    lin_w, lin_h = _parse_resolution(args.linux_resolution)
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    iso_path = args.iso.resolve()
    rec_path = args.rec.resolve() if args.rec is not None else None

    report: dict[str, Any] = {
        "platform": args.platform,
        "dry_run": args.dry_run,
        "win_resolution": f"{win_w}x{win_h}",
        "linux_resolution": f"{lin_w}x{lin_h}",
        "plugin_gate": {"skipped": args.skip_plugin_verify, "passed": True, "artifacts": []},
        "cases": [],
        "media_errors": [],
        "passed": False,
    }

    overall_ok = True

    # 1. Plugin Binary Integrity Gate (when Win64 is included or not skipped).
    if not args.skip_plugin_verify and args.platform in ("win64", "both"):
        plug_ok, plug_results = fu.verify_all_plugins(args.plugins_dir.resolve())
        report["plugin_gate"] = {
            "skipped": False,
            "passed": plug_ok,
            "plugins_dir": str(args.plugins_dir.resolve()),
            "artifacts": plug_results,
        }
        status = "PASS" if plug_ok else "FAIL"
        print(f"[GATE] Plugin Binary Integrity ({args.plugins_dir.resolve()}): {status}")
        if not plug_ok:
            overall_ok = False
            for info in plug_results:
                if not info["ok"]:
                    print(f"  [FAIL] {info['name']}: {info['errors']}", file=sys.stderr)

    # 2. Build filtered case matrix.
    cases: list[UpscalerCase] = []
    if args.platform in ("win64", "both"):
        cases.extend(build_win64_cases(include_ngx_fallback=not args.skip_ngx_fallback))
    if args.platform in ("linux", "both"):
        cases.extend(build_linux_cases())

    if args.cases.strip():
        tokens = [t.strip().lower() for t in args.cases.split(",") if t.strip()]
        cases = [c for c in cases if any(tok in c.case_id.lower() for tok in tokens)]

    if not cases:
        print("[FAIL] No test cases selected.", file=sys.stderr)
        return 2

    wsl_distro = ""
    if any(c.platform == "linux" for c in cases) and not args.dry_run and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)
        print(f"[INFO] Discovered WSL2 Ubuntu 22.04 distro: {wsl_distro}")

    media_before = m2.snapshot_media_stats(REPO_ROOT, iso_path)

    # 3. Execute matrix.
    for case in cases:
        exe = args.win_exe.resolve() if case.platform == "win64" else args.linux_exe.resolve()
        w, h = (win_w, win_h) if case.platform == "win64" else (lin_w, lin_h)
        if not args.dry_run and not exe.is_file():
            rr = CaseRunResult(
                case_id=case.case_id,
                platform=case.platform,
                requested=case.requested,
                expected_active=case.expected_active,
                errors=[f"Executable not found: {exe}"],
            )
        else:
            rr = _run_single_case(
                case=case,
                exe_path=exe,
                iso_path=iso_path,
                case_dir=out_dir / case.case_id,
                width=w,
                height=h,
                wsl_distro=wsl_distro,
                timeout_s=args.timeout,
                rec_path=rec_path,
                dry_run=args.dry_run,
            )
        report["cases"].append(asdict(rr))
        tag = "PASS" if rr.passed else "FAIL"
        res_str = f"{rr.render_w}x{rr.render_h}->{rr.display_w}x{rr.display_h}" if rr.render_w else "dry-run"
        print(
            f"  [{tag}] {rr.case_id:<30} req={rr.requested:<8} active={rr.actual_active or '?':<8} "
            f"{res_str:<19} wall={rr.wall_s:.1f}s"
        )
        if not rr.passed:
            overall_ok = False
            for err in rr.errors:
                print(f"         ERROR: {err}", file=sys.stderr)

    # 4. Verify protected source media immutability.
    media_errs = m2.assert_media_unchanged(media_before)
    report["media_errors"] = media_errs
    if media_errs:
        overall_ok = False
        for err in media_errs:
            print(f"[FAIL] {err}", file=sys.stderr)

    report["passed"] = overall_ok
    report_path = out_dir / "m5_upscalers_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"\nReport written to: {report_path}")
    print(f"Overall result: {'PASS' if overall_ok else 'FAIL'} ({sum(1 for c in report['cases'] if c['passed'])}/{len(report['cases'])} cases passed)")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
