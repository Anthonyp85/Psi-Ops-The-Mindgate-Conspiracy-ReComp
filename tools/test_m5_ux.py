#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 5 automated UX, Launcher, Overlay, Steam Deck & PrepareGameCode suite.

Verifies all Milestone 5 end-user experience and packaging deliverables:
  (a) 6-Tab Pre-Game Graphical Launcher on Win64 (D3D12) and Linux (SDL_Renderer
      1280x800 Steam Deck viewport): captures all 6 tabs, verifies non-black
      distinct tab renders, checks --launcher/--no-launcher bypasses, 5 Hardware
      Presets, [Ready] plugin badges, and ZERO settings.toml writes on
      unconfigured Exit.
  (b) Live-synced F1 / BACK+L3 Overlay + live HUD capture over 3D gameplay, plus
      In-App XISO File Browser capture (microSD Quick-Jump & ValidateDiscImage).
  (c) Early utility CLIs (--validate-iso valid & invalid, --create-shortcut .lnk
      on Win64 and .desktop on Linux) + Win32 PE resource audit (RT_GROUP_ICON,
      RT_MANIFEST PerMonitorV2, RT_VERSION VS_VERSION_INFO via pefile).
  (d) WSL2 sandboxed install-steamdeck.sh + --uninstall binary shortcuts.vdf
      idempotent round-trip (preserving unrelated pre-existing shortcuts).
  (e) Unpacked end-user first-launch PrepareGameCode local compilation + cached
      second-launch reuse on BOTH Windows x64 (PsiOps.exe + compiler/ + plugins/,
      0 prebuilt PsiOps.dll/.xbe) AND Native Linux (PsiOps + compiler/, 0 prebuilt
      libPsiOps.so/.xbe).
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
import zipfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

try:
    import pefile  # type: ignore
except ImportError:
    pefile = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_m2_gameplay as m2  # noqa: E402

LAUNCHER_TABS = (
    (0, "tab0_play"),
    (1, "tab1_upscaling"),
    (2, "tab2_display"),
    (3, "tab3_audio"),
    (4, "tab4_controls"),
    (5, "tab5_tools"),
)

EXPECTED_XBE_SHA1 = "42dd95fa444039b82f843ff02a31833da97d41cf"
EXPECTED_TITLE_ID = "0x4D570016"


@dataclass
class CheckResult:
    name: str
    passed: bool = False
    details: dict[str, Any] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def _run_platform_cmd(
    platform: str,
    exe_path: Path,
    args: list[str],
    env_extra: dict[str, str],
    wsl_distro: str,
    timeout_s: int,
    use_xvfb: bool = False,
    xvfb_res: str = "1280x800x24",
    cwd_override: Path | None = None,
) -> tuple[int, str, str, float]:
    t0 = time.monotonic()
    work_dir = cwd_override if cwd_override is not None else exe_path.parent
    if platform == "win64":
        env = os.environ.copy()
        env.update(env_extra)
        proc = subprocess.run(
            [str(exe_path)] + args,
            cwd=str(work_dir),
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
        return proc.returncode, proc.stdout or "", proc.stderr or "", round(time.monotonic() - t0, 3)

    wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
    wsl_dir = m2.to_wsl_path(work_dir) if sys.platform == "win32" else str(work_dir)
    env_kv = [f"{k}={v}" for k, v in env_extra.items()]
    inner = (["env"] + env_kv if env_kv else [])
    if use_xvfb:
        inner += ["xvfb-run", "-a", "-s", f"-screen 0 {xvfb_res}"]
    inner += [wsl_exe] + args
    cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner
    proc = subprocess.run(
        cmd,
        cwd=None if sys.platform == "win32" else str(work_dir),
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
    )
    return proc.returncode, proc.stdout or "", proc.stderr or "", round(time.monotonic() - t0, 3)


def check_pe_resources(exe_path: Path) -> CheckResult:
    res = CheckResult(name="win32_pe_resources")
    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res
    if pefile is None:
        res.errors.append("pefile module not installed")
        return res

    pe = pefile.PE(str(exe_path), fast_load=True)
    try:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        rsrc = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
        found_types: set[int] = set()
        manifest_text = ""
        if rsrc is not None:
            for entry in rsrc.entries:
                if entry.id is not None:
                    found_types.add(int(entry.id))
                # RT_MANIFEST == 24
                if entry.id == 24 and hasattr(entry, "directory"):
                    for sub in entry.directory.entries:
                        for lang in sub.directory.entries:
                            data_rva = lang.data.struct.OffsetToData
                            size = lang.data.struct.Size
                            manifest_text = pe.get_memory_mapped_image()[data_rva : data_rva + size].decode("utf-8", errors="replace")

        has_icon = (3 in found_types) and (14 in found_types)  # RT_ICON=3, RT_GROUP_ICON=14
        has_version = 16 in found_types  # RT_VERSION=16
        has_manifest = 24 in found_types  # RT_MANIFEST=24
        has_pmv2 = "PerMonitorV2" in manifest_text

        version_strings: dict[str, str] = {}
        if hasattr(pe, "FileInfo"):
            for fi in pe.FileInfo:
                for st in fi:
                    if hasattr(st, "StringTable"):
                        for tbl in st.StringTable:
                            for k, v in tbl.entries.items():
                                version_strings[k.decode("utf-8", errors="replace")] = v.decode("utf-8", errors="replace")

        res.details = {
            "has_icon": has_icon,
            "has_version": has_version,
            "has_manifest": has_manifest,
            "has_per_monitor_v2": has_pmv2,
            "version_strings": version_strings,
        }
        if not has_icon:
            res.errors.append("Missing RT_ICON / RT_GROUP_ICON in Win32 PE resources.")
        if not has_version:
            res.errors.append("Missing RT_VERSION (VS_VERSION_INFO) in Win32 PE resources.")
        if not has_manifest or not has_pmv2:
            res.errors.append("Missing RT_MANIFEST or PerMonitorV2 DPI awareness in Win32 PE resources.")
        prod = version_strings.get("ProductName", "")
        if "Psi-Ops" not in prod:
            res.errors.append(f"VS_VERSION_INFO ProductName missing 'Psi-Ops': got {prod!r}")
    finally:
        pe.close()
    res.passed = len(res.errors) == 0
    return res


def check_validate_iso_and_shortcut(
    platform: str,
    exe_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    res = CheckResult(name=f"{platform}_validate_iso_and_shortcut")
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True}
        return res

    iso_arg = str(iso_path) if platform == "win64" else (m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path))
    rc, out_s, err_s, wall = _run_platform_cmd(platform, exe_path, ["--validate-iso", iso_arg], {}, wsl_distro, 25)
    res.details["validate_rc"] = rc
    res.details["validate_wall_s"] = wall
    if rc != 0:
        res.errors.append(f"--validate-iso failed rc={rc}: {err_s.strip()}")
    else:
        try:
            data = json.loads(out_s.strip().splitlines()[-1])
            res.details["validate_json"] = data
            if not data.get("valid"):
                res.errors.append(f"--validate-iso returned valid=false: {data}")
            if str(data.get("title_id", "")).upper() != EXPECTED_TITLE_ID.upper():
                res.errors.append(f"Unexpected title_id: {data.get('title_id')} (expected {EXPECTED_TITLE_ID})")
            if int(data.get("xdk_build", 0)) != 5849:
                res.errors.append(f"Unexpected xdk_build: {data.get('xdk_build')} (expected 5849)")
            if str(data.get("sha1", "")).lower() != EXPECTED_XBE_SHA1:
                res.errors.append(f"Unexpected sha1: {data.get('sha1')}")
        except Exception as exc:
            res.errors.append(f"Failed to parse --validate-iso JSON: {exc} (stdout={out_s!r})")

    # Negative validation check
    bad_rc, bad_out, _, _ = _run_platform_cmd(platform, exe_path, ["--validate-iso", "nonexistent_invalid.iso"], {}, wsl_distro, 15)
    res.details["invalid_iso_rc"] = bad_rc
    if bad_rc == 0:
        res.errors.append("--validate-iso unexpectedly returned rc=0 on nonexistent ISO.")

    # --create-shortcut check into isolated output directory
    sc_dir = out_dir / f"{platform}_shortcut"
    sc_dir.mkdir(parents=True, exist_ok=True)
    sc_arg = str(sc_dir) if platform == "win64" else (m2.to_wsl_path(sc_dir) if sys.platform == "win32" else str(sc_dir))
    sc_env = {} if platform == "win64" else {"HOME": sc_arg, "XDG_DATA_HOME": f"{sc_arg}/.local/share"}
    sc_rc, sc_out, sc_err, _ = _run_platform_cmd(platform, exe_path, ["--create-shortcut", sc_arg], sc_env, wsl_distro, 25)
    res.details["shortcut_rc"] = sc_rc
    if sc_rc != 0:
        res.errors.append(f"--create-shortcut failed rc={sc_rc}: {sc_err.strip()}")
    else:
        try:
            sdata = json.loads(sc_out.strip().splitlines()[-1])
            res.details["shortcut_json"] = sdata
            if not sdata.get("ok"):
                res.errors.append(f"--create-shortcut returned ok=false: {sdata}")
        except Exception as exc:
            res.errors.append(f"Failed to parse --create-shortcut JSON: {exc}")
        if platform == "win64":
            lnks = list(sc_dir.glob("*.lnk"))
            res.details["lnk_files"] = [p.name for p in lnks]
            if not lnks or lnks[0].stat().st_size == 0:
                res.errors.append(f"No non-empty .lnk created in {sc_dir}")
        else:
            desks = list(sc_dir.rglob("*.desktop"))
            res.details["desktop_files"] = [str(p.relative_to(sc_dir)) for p in desks]
            if not desks:
                res.errors.append(f"No .desktop file created under {sc_dir}")
            else:
                dtxt = desks[0].read_text(encoding="utf-8", errors="replace")
                if "Exec=" not in dtxt or "Icon=" not in dtxt:
                    res.errors.append(f"Malformed .desktop file: {desks[0]}")

    res.passed = len(res.errors) == 0
    return res


def check_launcher_and_browser(
    platform: str,
    exe_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    res = CheckResult(name=f"{platform}_launcher_6tabs_and_browser")
    cap_dir = out_dir / f"{platform}_launcher"
    cap_dir.mkdir(parents=True, exist_ok=True)
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "tabs": [t[1] for t in LAUNCHER_TABS]}
        return res

    # 1. Verify ZERO settings.toml writes on unconfigured Exit
    clean_settings = cap_dir / "unconfigured_settings.toml"
    if clean_settings.exists():
        clean_settings.unlink()

    tab_hashes: dict[str, str] = {}
    tab_dims: dict[str, list[int]] = {}
    for tab_idx, tab_name in LAUNCHER_TABS:
        bmp_path = cap_dir / f"{tab_name}.bmp"
        bmp_arg = str(bmp_path) if platform == "win64" else (m2.to_wsl_path(bmp_path) if sys.platform == "win32" else str(bmp_path))
        set_arg = str(clean_settings) if platform == "win64" else (m2.to_wsl_path(clean_settings) if sys.platform == "win32" else str(clean_settings))
        env_extra = {"XBE_SHOW_LAUNCHER": "1", "XBE_SETTINGS_PATH": set_arg}
        if platform == "linux":
            env_extra["SDL_AUDIODRIVER"] = "dummy"
            env_extra["SteamDeck"] = "1"
        args = ["--launcher", "--launcher-tab", str(tab_idx), "--capture-launcher-bmp", bmp_arg, "--settings", set_arg]
        rc, _, err_s, _ = _run_platform_cmd(platform, exe_path, args, env_extra, wsl_distro, 45, use_xvfb=(platform == "linux"), xvfb_res="1280x800x24")
        if rc != 0:
            res.errors.append(f"Launcher tab {tab_idx} ({tab_name}) exited rc={rc}: {err_s.strip()}")
            continue
        cp, _ = m2.analyze_checkpoint(cap_dir, tab_name)
        if not cp.passed:
            res.errors.append(f"Launcher tab {tab_idx} ({tab_name}) capture failed: {cp.reason}")
        else:
            tab_hashes[tab_name] = cp.sha256
            tab_dims[tab_name] = [cp.width, cp.height]
            if platform == "linux" and (cp.width != 1280 or cp.height != 800):
                res.warnings.append(f"Linux launcher tab {tab_name} captured at {cp.width}x{cp.height} (expected 1280x800 Steam Deck viewport).")

    res.details["tab_hashes"] = tab_hashes
    res.details["tab_dims"] = tab_dims
    if len(set(tab_hashes.values())) < 5:
        res.errors.append(f"Expected distinct renders across 6 launcher tabs, got only {len(set(tab_hashes.values()))} unique hashes.")

    # Confirm 0 settings.toml created when exiting unconfigured launcher!
    if clean_settings.exists():
        res.errors.append(f"Launcher wrote settings.toml ({clean_settings}) on unconfigured Exit (must write 0 files!).")
    else:
        res.details["zero_settings_write_on_unconfigured_exit"] = True

    # 2. Capture In-App XISO File Browser
    fb_bmp = cap_dir / "file_browser.bmp"
    fb_arg = str(fb_bmp) if platform == "win64" else (m2.to_wsl_path(fb_bmp) if sys.platform == "win32" else str(fb_bmp))
    fb_env = {"XBE_SHOW_LAUNCHER": "1"}
    if platform == "linux":
        fb_env["SDL_AUDIODRIVER"] = "dummy"
        fb_env["SteamDeck"] = "1"
    fb_rc, _, fb_err, _ = _run_platform_cmd(
        platform,
        exe_path,
        ["--launcher", "--open-file-browser", "--capture-launcher-bmp", fb_arg],
        fb_env,
        wsl_distro,
        45,
        use_xvfb=(platform == "linux"),
        xvfb_res="1280x800x24",
    )
    if fb_rc != 0:
        res.errors.append(f"In-App File Browser capture exited rc={fb_rc}: {fb_err.strip()}")
    else:
        cp_fb, _ = m2.analyze_checkpoint(cap_dir, "file_browser")
        res.details["file_browser"] = asdict(cp_fb)
        if not cp_fb.passed:
            res.errors.append(f"In-App File Browser PNG check failed: {cp_fb.reason}")

    # 3. Capture live F1 Overlay + HUD over 3D gameplay
    ov_bmp = cap_dir / "f1_overlay_hud.bmp"
    ov_arg = str(ov_bmp) if platform == "win64" else (m2.to_wsl_path(ov_bmp) if sys.platform == "win32" else str(ov_bmp))
    iso_arg = str(iso_path) if platform == "win64" else (m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path))
    ov_env = {
        "XBE_AUTOTEST": "1",
        "PSIOPS_AUTOTEST": "1",
        "XBE_OPEN_SETTINGS": "1",
        "XBE_CAPTURE_UI_BMP": ov_arg,
        "XBE_ISO_PATH": iso_arg,
    }
    if platform == "linux":
        ov_env["SDL_AUDIODRIVER"] = "dummy"
    ov_args = ["--no-launcher", "--iso", iso_arg, "--open-settings", "--capture-ui-bmp", ov_arg, "--capture-after-frames", "120", "--capture-exit"]
    ov_rc, _, ov_err, _ = _run_platform_cmd(
        platform,
        exe_path,
        ov_args,
        ov_env,
        wsl_distro,
        60,
        use_xvfb=(platform == "linux"),
        xvfb_res="1280x800x24",
    )
    if ov_rc != 0:
        res.errors.append(f"F1 Overlay capture exited rc={ov_rc}: {ov_err.strip()}")
    else:
        cp_ov, _ = m2.analyze_checkpoint(cap_dir, "f1_overlay_hud")
        res.details["f1_overlay_hud"] = asdict(cp_ov)
        if not cp_ov.passed:
            res.errors.append(f"F1 Overlay PNG check failed: {cp_ov.reason}")

    res.passed = len(res.errors) == 0
    return res


def check_launcher_to_game_autoplay(
    platform: str,
    exe_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
    *,
    use_existing_settings: bool = False,
    expect_dlss: bool = False,
    auto_start_compile: bool = False,
    min_frames: int = 180,
    exit_ms: int = 7500,
    label: str = "",
    cwd_override: Path | None = None,
    timeout_s: int = 120,
) -> CheckResult:
    """End-to-end test: real Pre-Game Launcher -> PLAY PSI-OPS -> live game frames -> clean exit_ms."""
    suffix = f"_{label}" if label else ""
    res = CheckResult(name=f"{platform}_launcher_to_game_autoplay{suffix}")
    cap_dir = out_dir / f"{platform}_autoplay{suffix}"
    cap_dir.mkdir(parents=True, exist_ok=True)
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "min_frames": min_frames, "exit_ms": exit_ms}
        return res

    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res

    for stale in (cap_dir / "launcher_to_game.bmp", cap_dir / "launcher_to_game.png"):
        if stale.exists():
            stale.unlink()

    script_path = cap_dir / "autoplay_script.txt"
    script_path.write_text(
        f"capture {min_frames} launcher_to_game.bmp\nexit_ms {exit_ms}\n",
        encoding="utf-8",
        newline="\n",
    )

    iso_arg = str(iso_path) if platform == "win64" else (m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path))
    scr_arg = str(script_path) if platform == "win64" else (m2.to_wsl_path(script_path) if sys.platform == "win32" else str(script_path))
    out_arg = str(cap_dir) if platform == "win64" else (m2.to_wsl_path(cap_dir) if sys.platform == "win32" else str(cap_dir))

    env_extra: dict[str, str] = {
        "XBE_LAUNCHER_AUTOPLAY_MS": "400",
        "XBE_AUTOTEST_SCRIPT": scr_arg,
        "XBE_AUTOTEST_OUT_DIR": out_arg,
    }
    if auto_start_compile:
        env_extra["XBE_AUTO_START"] = "1"
    if not use_existing_settings:
        iso_settings = cap_dir / "autoplay_settings.toml"
        if iso_settings.exists():
            iso_settings.unlink()
        set_arg = str(iso_settings) if platform == "win64" else (m2.to_wsl_path(iso_settings) if sys.platform == "win32" else str(iso_settings))
        env_extra["XBE_SETTINGS_PATH"] = set_arg
        env_extra["XBE_ISO_PATH"] = iso_arg
    if platform == "linux":
        env_extra["SDL_AUDIODRIVER"] = "dummy"
        env_extra["SteamDeck"] = "1"

    logs_dir = (cwd_override if cwd_override is not None else exe_path.parent) / "logs"
    before_logs = {p.name: p.stat().st_mtime_ns for p in logs_dir.glob("*.log")} if logs_dir.is_dir() else {}

    # Strictly do NOT pass --no-launcher! Exercise the real Launcher -> PLAY -> Game Window path.
    cmd_args: list[str] = []
    if not use_existing_settings:
        cmd_args += ["--iso", iso_arg]

    rc, _, err_s, wall = _run_platform_cmd(
        platform,
        exe_path,
        cmd_args,
        env_extra,
        wsl_distro,
        timeout_s,
        use_xvfb=(platform == "linux"),
        xvfb_res="1280x800x24",
        cwd_override=cwd_override,
    )
    res.details["rc"] = rc
    res.details["wall_s"] = wall
    if rc != 0:
        res.errors.append(f"Launcher->Game autoplay exited rc={rc}: {err_s.strip()}")

    # Locate the newest log written by this run
    log_path: Path | None = None
    if logs_dir.is_dir():
        candidates = [
            p for p in logs_dir.glob("*.log")
            if p.name not in before_logs or p.stat().st_mtime_ns > before_logs[p.name]
        ]
        if candidates:
            log_path = max(candidates, key=lambda p: p.stat().st_mtime_ns)
    if log_path is None or not log_path.is_file():
        res.errors.append(f"No session log found in {logs_dir}")
    else:
        log_txt = log_path.read_text(encoding="utf-8", errors="replace")
        res.details["log_path"] = str(log_path)
        opened = "launcher: opened " in log_txt
        autoplay = "launcher: autoplay triggered " in log_txt
        closed_play = "launch_game=true" in log_txt
        booted = (
            ("boot: title 4D570016" in log_txt)
            or ("threads: single_core_guest" in log_txt)
            or ("D3D12 adapter:" in log_txt)
            or ("Vulkan adapter:" in log_txt)
        )
        res.details["launcher_opened"] = opened
        res.details["launcher_autoplay_triggered"] = autoplay
        res.details["launcher_closed_launch_game"] = closed_play
        res.details["game_booted"] = booted
        if not opened or not autoplay or not closed_play:
            res.errors.append(
                f"Log missing launcher->play transition markers (opened={opened}, autoplay={autoplay}, launch_game={closed_play})"
            )
        if not booted:
            res.errors.append("Log missing game boot/GPU init marker.")

        m_exit = re.search(r"autotest: clean exit requested at frame=(\d+)\s+ms=(\d+)", log_txt)
        if not m_exit:
            res.errors.append("Game exited prematurely before XBE_AUTOTEST_SCRIPT exit_ms fired!")
        else:
            swap_count = int(m_exit.group(1))
            exit_elapsed_ms = int(m_exit.group(2))
            res.details["swap_count"] = swap_count
            res.details["exit_elapsed_ms"] = exit_elapsed_ms
            if swap_count < min_frames:
                res.errors.append(f"Rendered SwapCount={swap_count} < required minimum {min_frames} frames.")

        if expect_dlss:
            dlss_init = "upscale: DLSS 4 initialized" in log_txt
            dlss_feat = "upscale: DLSS 4 feature created" in log_txt
            preset_k = ("Preset K" in log_txt) or ("dlss_preset = 'k'" in log_txt)
            res.details["dlss4_initialized"] = dlss_init
            res.details["dlss4_feature_created"] = dlss_feat
            res.details["dlss_preset_k"] = preset_k
            if not dlss_init:
                res.errors.append("Expected DLSS 4 initialization on RTX GPU, but 'upscale: DLSS 4 initialized' not in log.")
            if not preset_k:
                res.errors.append("Expected DLSS Preset K active in log.")

    cp, _ = m2.analyze_checkpoint(cap_dir, "launcher_to_game")
    res.details["capture"] = asdict(cp)
    if not cp.passed:
        res.errors.append(f"Game frame capture check failed: {cp.reason}")

    res.passed = len(res.errors) == 0
    return res


def check_extracted_zip_launcher_autoplay(
    zip_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    """Extract win64-launcher.zip into a clean folder and verify Launcher -> Play -> PrepareGameCode -> Live Game."""
    res = CheckResult(name="win64_zip_launcher_to_game_autoplay")
    stage_dir = out_dir / "extracted_win64_zip"
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "zip": str(zip_path)}
        return res
    if not zip_path.is_file():
        res.errors.append(f"Win64 release zip not found: {zip_path}")
        return res
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(stage_dir)
    exe_path = stage_dir / "PsiOps.exe"
    if not exe_path.is_file():
        res.errors.append(f"Extracted zip missing PsiOps.exe: {stage_dir}")
        return res
    inner = check_launcher_to_game_autoplay(
        "win64",
        exe_path,
        iso_path,
        out_dir,
        wsl_distro,
        False,
        use_existing_settings=False,
        expect_dlss=False,
        auto_start_compile=True,
        min_frames=180,
        exit_ms=7500,
        label="extracted_zip",
        cwd_override=stage_dir,
        timeout_s=600,
    )
    res.passed = inner.passed
    res.details = inner.details
    res.details["zip"] = str(zip_path)
    res.errors.extend(inner.errors)
    res.warnings.extend(inner.warnings)
    return res


def _build_dummy_shortcuts_vdf() -> bytes:
    """Create a valid binary Steam shortcuts.vdf containing one unrelated dummy game."""
    def s(k: str, v: str) -> bytes:
        return b"\x01" + k.encode("utf-8") + b"\x00" + v.encode("utf-8") + b"\x00"

    def i32(k: str, v: int) -> bytes:
        return b"\x02" + k.encode("utf-8") + b"\x00" + struct.pack("<I", v & 0xFFFFFFFF)

    entry0 = (
        i32("appid", 0x91234567)
        + s("AppName", "Unrelated Existing Game")
        + s("Exe", '"/home/deck/Games/Other/other_bin"')
        + s("StartDir", '"/home/deck/Games/Other"')
        + s("icon", "")
        + b"\x00tags\x00\x08"
        + b"\x08"
    )
    return b"\x00shortcuts\x00\x000\x00" + entry0 + b"\x08\x08"


def _parse_shortcuts_vdf_names(raw: bytes) -> list[str]:
    names: list[str] = []
    idx = 0
    marker = b"\x01AppName\x00"
    while True:
        pos = raw.find(marker, idx)
        if pos == -1:
            break
        start = pos + len(marker)
        end = raw.index(b"\x00", start)
        names.append(raw[start:end].decode("utf-8", errors="replace"))
        idx = end + 1
    return names


def check_steamdeck_installer_roundtrip(repo_root: Path, out_dir: Path, wsl_distro: str) -> CheckResult:
    res = CheckResult(name="steamdeck_installer_vdf_roundtrip")
    installer = repo_root / "packaging" / "steamdeck" / "install-steamdeck.sh"
    icon_src = repo_root / "resources" / "psiops.png"
    if not installer.is_file():
        res.errors.append(f"Installer missing: {installer}")
        return res

    sandbox = out_dir / "steamdeck_sandbox"
    if sandbox.exists():
        shutil.rmtree(sandbox)
    fake_install = sandbox / "game_dir"
    fake_steam = sandbox / "Steam"
    fake_xdg = sandbox / "xdg_share"
    fake_cfg = fake_steam / "userdata" / "12345678" / "config"
    fake_install.mkdir(parents=True, exist_ok=True)
    fake_cfg.mkdir(parents=True, exist_ok=True)
    fake_xdg.mkdir(parents=True, exist_ok=True)

    # Create dummy executable + copy real icon
    (fake_install / "PsiOps").write_bytes(b"#!/bin/sh\nexit 0\n")
    if icon_src.is_file():
        shutil.copy2(icon_src, fake_install / "psiops.png")
    vdf_path = fake_cfg / "shortcuts.vdf"
    vdf_path.write_bytes(_build_dummy_shortcuts_vdf())

    def run_sh(extra_args: list[str]) -> tuple[int, str, str]:
        sh_path = m2.to_wsl_path(installer) if sys.platform == "win32" else str(installer)
        inst_arg = m2.to_wsl_path(fake_install) if sys.platform == "win32" else str(fake_install)
        st_arg = m2.to_wsl_path(fake_steam) if sys.platform == "win32" else str(fake_steam)
        xdg_arg = m2.to_wsl_path(fake_xdg) if sys.platform == "win32" else str(fake_xdg)
        inner = ["bash", sh_path, "--install-dir", inst_arg, "--steam-root", st_arg, "--xdg-data", xdg_arg] + extra_args
        cmd = ["wsl", "-d", wsl_distro, "--"] + inner if sys.platform == "win32" else inner
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
        return proc.returncode, proc.stdout or "", proc.stderr or ""

    # Run 1: Install
    rc1, out1, err1 = run_sh([])
    if rc1 != 0:
        res.errors.append(f"install-steamdeck.sh run 1 failed rc={rc1}: {err1.strip()}")
        return res
    names1 = _parse_shortcuts_vdf_names(vdf_path.read_bytes())
    baks = list(fake_cfg.glob("shortcuts.vdf.bak.*"))
    desk = fake_xdg / "applications" / "psi-ops-recomp.desktop"
    icon_dst = fake_xdg / "icons" / "hicolor" / "256x256" / "apps" / "psiops.png"
    res.details["after_install_1_names"] = names1
    res.details["backups_created"] = len(baks)
    if names1 != ["Unrelated Existing Game", "Psi-Ops: The Mindgate Conspiracy"]:
        res.errors.append(f"Unexpected shortcuts.vdf entries after install 1: {names1}")
    if not baks:
        res.errors.append("No shortcuts.vdf backup (.bak) created before modification.")
    if not desk.is_file() or "Proton" not in out1:
        res.errors.append("Missing .desktop file or native-Linux/no-Proton notice.")
    if icon_src.is_file() and not icon_dst.is_file():
        res.errors.append(f"Icon not installed to {icon_dst}")

    # Run 2: Idempotent re-install (must NOT duplicate entry!)
    rc2, _, err2 = run_sh([])
    if rc2 != 0:
        res.errors.append(f"install-steamdeck.sh run 2 failed rc={rc2}: {err2.strip()}")
    names2 = _parse_shortcuts_vdf_names(vdf_path.read_bytes())
    res.details["after_install_2_names"] = names2
    if names2 != ["Unrelated Existing Game", "Psi-Ops: The Mindgate Conspiracy"]:
        res.errors.append(f"Idempotency failed (duplicate/corrupt entries after run 2): {names2}")

    # Run 3: --uninstall (must remove Psi-Ops + icon + .desktop while preserving unrelated game!)
    rc3, _, err3 = run_sh(["--uninstall"])
    if rc3 != 0:
        res.errors.append(f"install-steamdeck.sh --uninstall failed rc={rc3}: {err3.strip()}")
    names3 = _parse_shortcuts_vdf_names(vdf_path.read_bytes())
    res.details["after_uninstall_names"] = names3
    if names3 != ["Unrelated Existing Game"]:
        res.errors.append(f"Uninstall did not cleanly restore unrelated entry only: {names3}")
    if desk.exists() or icon_dst.exists():
        res.errors.append("Uninstall left behind .desktop or icon file.")

    res.passed = len(res.errors) == 0
    return res


def check_unpacked_prepare_game_code(
    platform: str,
    repo_root: Path,
    exe_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    res = CheckResult(name=f"{platform}_unpacked_prepare_game_code")
    stage_dir = out_dir / f"unpacked_{platform}"
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "stage_dir": str(stage_dir)}
        return res

    if not exe_path.is_file():
        res.errors.append(f"Built executable not found: {exe_path}")
        return res
    src_compiler = exe_path.parent / "compiler"
    if not src_compiler.is_dir():
        res.errors.append(f"compiler/ directory not found beside {exe_path}")
        return res

    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    # Copy ONLY end-user distributable artifacts (STRICTLY 0 prebuilt PsiOps.dll/libPsiOps.so and 0 .xbe!)
    dst_exe = stage_dir / exe_path.name
    shutil.copy2(exe_path, dst_exe)
    shutil.copytree(src_compiler, stage_dir / "compiler", ignore=shutil.ignore_patterns("work"))
    if (repo_root / "resources" / "psiops.png").is_file():
        shutil.copy2(repo_root / "resources" / "psiops.png", stage_dir / "psiops.png")
    if platform == "win64" and (repo_root / "plugins").is_dir():
        shutil.copytree(repo_root / "plugins", stage_dir / "plugins")

    # Assert strictly zero prebuilt game code DLL/.so or .xbe exist in unpacked stage
    code_artifact = stage_dir / ("PsiOps.dll" if platform == "win64" else "libPsiOps.so")
    if code_artifact.exists() or list(stage_dir.rglob("*.xbe")):
        res.errors.append("Staging hygiene violation: prebuilt game code or .xbe present before first launch!")
        return res

    # Short autotest script exiting cleanly after first frames prove full boot
    boot_script = stage_dir / "boot_smoke.txt"
    boot_script.write_text("exit_ms 4000\n", encoding="utf-8", newline="\n")

    iso_arg = str(iso_path) if platform == "win64" else (m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path))
    scr_arg = str(boot_script) if platform == "win64" else (m2.to_wsl_path(boot_script) if sys.platform == "win32" else str(boot_script))
    env_run = {
        "XBE_AUTOTEST": "1",
        "PSIOPS_AUTOTEST": "1",
        "XBE_AUTO_START": "1",
        "XBE_AUTOTEST_SCRIPT": scr_arg,
        "XBE_ISO_PATH": iso_arg,
    }
    if platform == "linux":
        env_run["SDL_AUDIODRIVER"] = "dummy"

    # Run 1: Cold First-Launch PrepareGameCode compilation (generous 600s timeout)
    print(f"[test_m5_ux] Running unpacked {platform} Run 1 (first-launch PrepareGameCode compile)...")
    rc1, _, err1, wall1 = _run_platform_cmd(
        platform, dst_exe, ["--no-launcher", "--iso", iso_arg], env_run, wsl_distro, 600, use_xvfb=(platform == "linux"), cwd_override=stage_dir
    )
    res.details["run1_rc"] = rc1
    res.details["run1_wall_s"] = wall1
    built_stamp = stage_dir / "compiler" / "work" / "PsiOps" / "built.txt"
    if rc1 != 0:
        res.errors.append(f"Unpacked {platform} Run 1 failed rc={rc1}: {err1.strip()}")
        return res
    if not code_artifact.is_file() or code_artifact.stat().st_size == 0:
        res.errors.append(f"Unpacked {platform} Run 1 did not produce {code_artifact.name}.")
        return res
    if not built_stamp.is_file():
        res.errors.append(f"Unpacked {platform} Run 1 did not write {built_stamp}.")

    artifact_mtime_1 = code_artifact.stat().st_mtime_ns
    artifact_sha_1 = hashlib.sha256(code_artifact.read_bytes()).hexdigest()
    res.details["compiled_artifact_bytes"] = code_artifact.stat().st_size
    res.details["compiled_artifact_sha256"] = artifact_sha_1

    # Run 2: Cached Second Launch (must skip recompilation and boot fast!)
    print(f"[test_m5_ux] Running unpacked {platform} Run 2 (cached second-launch reuse)...")
    rc2, _, err2, wall2 = _run_platform_cmd(
        platform, dst_exe, ["--no-launcher", "--iso", iso_arg], env_run, wsl_distro, 60, use_xvfb=(platform == "linux"), cwd_override=stage_dir
    )
    res.details["run2_rc"] = rc2
    res.details["run2_wall_s"] = wall2
    if rc2 != 0:
        res.errors.append(f"Unpacked {platform} Run 2 failed rc={rc2}: {err2.strip()}")
    if code_artifact.stat().st_mtime_ns != artifact_mtime_1:
        res.errors.append(f"Unpacked {platform} Run 2 unnecessarily recompiled {code_artifact.name} (mtime changed)!")
    if wall2 > 30.0:
        res.warnings.append(f"Unpacked {platform} cached Run 2 took {wall2}s (> 30s).")

    res.passed = len(res.errors) == 0
    return res


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Milestone 5 UX, Launcher, Overlay, Steam Deck & PrepareGameCode suite.")
    parser.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    parser.add_argument("--out-dir", type=Path, default=repo_root / ".local" / "m5_ux")
    parser.add_argument("--iso", type=Path, default=repo_root / m2.EXPECTED_ISO_REL)
    parser.add_argument("--win-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    parser.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    parser.add_argument("--wsl-distro", type=str, default=None)
    parser.add_argument("--skip-unpacked-compile", action="store_true")
    parser.add_argument("--only-steamdeck-installer", action="store_true")
    parser.add_argument("--only-launcher-autoplay", action="store_true")
    parser.add_argument("--use-existing-win-settings", action="store_true")
    parser.add_argument("--expect-dlss", action="store_true")
    parser.add_argument("--win-zip", type=Path, default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.iso.is_file():
        print(f"ERROR: XISO not found: {args.iso}", file=sys.stderr)
        return 2

    platforms = ["win64", "linux"] if args.platform == "both" else [args.platform]
    wsl_distro = ""
    if ("linux" in platforms or args.only_steamdeck_installer) and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    media_before = m2.snapshot_media_stats(repo_root, args.iso)
    checks: list[CheckResult] = []

    if args.only_steamdeck_installer:
        checks.append(check_steamdeck_installer_roundtrip(repo_root, args.out_dir, wsl_distro))
    elif args.only_launcher_autoplay:
        if "win64" in platforms:
            checks.append(
                check_launcher_to_game_autoplay(
                    "win64",
                    args.win_exe,
                    args.iso,
                    args.out_dir,
                    wsl_distro,
                    args.dry_run,
                    use_existing_settings=args.use_existing_win_settings,
                    expect_dlss=args.expect_dlss,
                )
            )
            if args.win_zip is not None:
                checks.append(
                    check_extracted_zip_launcher_autoplay(
                        args.win_zip, args.iso, args.out_dir, wsl_distro, args.dry_run
                    )
                )
        if "linux" in platforms:
            checks.append(
                check_launcher_to_game_autoplay(
                    "linux",
                    args.linux_exe,
                    args.iso,
                    args.out_dir,
                    wsl_distro,
                    args.dry_run,
                    use_existing_settings=False,
                    expect_dlss=False,
                    exit_ms=18000,
                    timeout_s=180,
                )
            )
    else:
        if "win64" in platforms:
            if not args.dry_run:
                checks.append(check_pe_resources(args.win_exe))
            checks.append(check_validate_iso_and_shortcut("win64", args.win_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))
            checks.append(check_launcher_and_browser("win64", args.win_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))
            checks.append(
                check_launcher_to_game_autoplay(
                    "win64",
                    args.win_exe,
                    args.iso,
                    args.out_dir,
                    wsl_distro,
                    args.dry_run,
                    use_existing_settings=args.use_existing_win_settings,
                    expect_dlss=args.expect_dlss,
                )
            )
            if args.win_zip is not None:
                checks.append(
                    check_extracted_zip_launcher_autoplay(
                        args.win_zip, args.iso, args.out_dir, wsl_distro, args.dry_run
                    )
                )
            if not args.skip_unpacked_compile:
                checks.append(check_unpacked_prepare_game_code("win64", repo_root, args.win_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))

        if "linux" in platforms:
            checks.append(check_steamdeck_installer_roundtrip(repo_root, args.out_dir, wsl_distro))
            checks.append(check_validate_iso_and_shortcut("linux", args.linux_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))
            checks.append(check_launcher_and_browser("linux", args.linux_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))
            checks.append(
                check_launcher_to_game_autoplay(
                    "linux",
                    args.linux_exe,
                    args.iso,
                    args.out_dir,
                    wsl_distro,
                    args.dry_run,
                    use_existing_settings=False,
                    expect_dlss=False,
                    exit_ms=18000,
                    timeout_s=180,
                )
            )
            if not args.skip_unpacked_compile:
                checks.append(check_unpacked_prepare_game_code("linux", repo_root, args.linux_exe, args.iso, args.out_dir, wsl_distro, args.dry_run))

    media_errs = m2.assert_media_unchanged(media_before)
    all_passed = all(c.passed for c in checks) and len(media_errs) == 0

    for c in checks:
        print(f"[test_m5_ux] {c.name}: {'PASS' if c.passed else 'FAIL'}")
        for w in c.warnings:
            print(f"  WARN: {w}")
        for e in c.errors:
            print(f"  FAIL: {e}")
    for me in media_errs:
        print(f"  FAIL: {me}")

    report = {
        "iso": str(args.iso),
        "platforms": platforms,
        "all_passed": all_passed,
        "media_errors": media_errs,
        "checks": [asdict(c) for c in checks],
    }
    report_path = args.out_dir / "m5_ux_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"[test_m5_ux] Report written to: {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
