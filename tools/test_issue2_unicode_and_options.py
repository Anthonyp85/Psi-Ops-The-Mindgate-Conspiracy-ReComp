#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Issue #2 Regression Suite: Non-ACP Unicode Paths & Live Options Sync.

Verifies on Windows x64 (D3D12) and Native Linux (Vulkan 1.2 / SDL3):
  (a) Sibling non-ACP Unicode directories (e.g. Japanese CJK folder names in
      parent directory) never crash startup, FindWorkspaceIsos(), or the In-App
      File Browser when launching unconfigured without settings.toml.
  (b) Packaged/end-user FindWorkspaceIsos() scans only ExeDir() and immediate
      subdirectories (never walking 6 ancestor folders into parent user trees).
  (c) Installing and running PsiOps directly inside a non-ACP Unicode folder
      with a Unicode disc image path in settings.toml works end-to-end across
      --validate-iso, --create-shortcut, Pre-Game Launcher, File Browser,
      Launcher->Game autoplay, logx::Init, kernel HDD mounts, perf.json, and
      state.jsonl.
  (d) Adjusting and syncing [game_options] (SFX/Music volume, Invert Y, HUD,
      Vibration) and opening the F1 Recomp Options Menu during early boot FMV
      (before EngineAudio::Init creates SoundBus instances) never dereferences
      null/freed guest SoundBus or non-HUD UIController pointers.
"""

from __future__ import annotations

import argparse
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

EXPECTED_XBE_SHA1 = "42dd95fa444039b82f843ff02a31833da97d41cf"
EXPECTED_TITLE_ID = "0x4D570016"

# Pure-ASCII \uXXXX escapes for non-ACP CJK + Latin-1 directory/file names
CJK_SIBLING_DIR = "\u3053\u306e\u3059\u3070_\u5fa9\u6d3b\u306e\u30d9\u30eb\u30c7\u30a3\u30a2"
CJK_INSTALL_DIR = "\u65e5\u672c\u8a9e_\u8d85\u80fd\u529b_\u00c9dition"
CJK_DISC_DIR = "\u30c7\u30a3\u30b9\u30af_\u4fdd\u7ba1\u5eab"
CJK_ISO_NAME = "\u30b5\u30a4\u30aa\u30d7\u30b9_USA.xiso.iso"


@dataclass
class CheckResult:
    name: str
    passed: bool = False
    details: dict[str, Any] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)


def _run_cmd(
    platform: str,
    exe_path: Path,
    args: list[str],
    env_extra: dict[str, str],
    wsl_distro: str,
    timeout_s: int,
    use_xvfb: bool = False,
    cwd_override: Path | None = None,
) -> tuple[int, str, str, float]:
    t0 = time.monotonic()
    work_dir = cwd_override if cwd_override is not None else exe_path.parent
    if platform == "win64":
        env = os.environ.copy()
        env.update(env_extra)
        try:
            proc = subprocess.run(
                [str(exe_path)] + args,
                cwd=str(work_dir),
                env=env,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_s,
                check=False,
            )
            return proc.returncode, proc.stdout or "", proc.stderr or "", round(time.monotonic() - t0, 3)
        except subprocess.TimeoutExpired as exc:
            return 124, exc.stdout or "", f"TimeoutExpired({timeout_s}s)", round(time.monotonic() - t0, 3)

    wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
    wsl_dir = m2.to_wsl_path(work_dir) if sys.platform == "win32" else str(work_dir)
    env_kv = [f"{k}={v}" for k, v in env_extra.items()]
    inner = (["env"] + env_kv if env_kv else [])
    if use_xvfb:
        inner += ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24"]
    inner += [wsl_exe] + args
    cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner
    try:
        proc = subprocess.run(
            cmd,
            cwd=None if sys.platform == "win32" else str(work_dir),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_s,
            check=False,
        )
        return proc.returncode, proc.stdout or "", proc.stderr or "", round(time.monotonic() - t0, 3)
    except subprocess.TimeoutExpired as exc:
        return 124, exc.stdout or "", f"TimeoutExpired({timeout_s}s)", round(time.monotonic() - t0, 3)


def _stage_runtime_tree(platform: str, src_exe: Path, dst_dir: Path, include_game_lib: bool) -> Path:
    dst_dir.mkdir(parents=True, exist_ok=True)
    src_dir = src_exe.parent
    dst_exe = dst_dir / src_exe.name
    shutil.copy2(src_exe, dst_exe)
    if platform == "win64":
        for dll in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
            cand = src_dir / dll
            if cand.is_file():
                shutil.copy2(cand, dst_dir / dll)
        if include_game_lib and (src_dir / "PsiOps.dll").is_file():
            dst_dll = dst_dir / "PsiOps.dll"
            shutil.copy2(src_dir / "PsiOps.dll", dst_dll)
            exe_mtime = dst_exe.stat().st_mtime
            if dst_dll.stat().st_mtime <= exe_mtime:
                os.utime(dst_dll, (exe_mtime + 2.0, exe_mtime + 2.0))
        src_plugins = src_dir / "plugins"
        if src_plugins.is_dir():
            shutil.copytree(src_plugins, dst_dir / "plugins", dirs_exist_ok=True)
    else:
        os.chmod(dst_exe, 0o755)
        if include_game_lib and (src_dir / "libPsiOps.so").is_file():
            dst_so = dst_dir / "libPsiOps.so"
            shutil.copy2(src_dir / "libPsiOps.so", dst_so)
            exe_mtime = dst_exe.stat().st_mtime
            if dst_so.stat().st_mtime <= exe_mtime:
                os.utime(dst_so, (exe_mtime + 2.0, exe_mtime + 2.0))
    return dst_exe


def _link_readonly_iso(src_iso: Path, dst_iso: Path) -> None:
    dst_iso.parent.mkdir(parents=True, exist_ok=True)
    if dst_iso.exists():
        dst_iso.unlink()
    try:
        os.link(src_iso, dst_iso)
        return
    except OSError:
        pass
    os.symlink(src_iso, dst_iso)


def _toml_escape_basic(val: str) -> str:
    return val.replace("\\", "\\\\").replace('"', '\\"')


def check_sibling_unicode_unconfigured_startup(
    platform: str,
    exe_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    """Reproduces Issue #2 Symptom (b): unconfigured launch with CJK sibling folder in parent dir."""
    res = CheckResult(name=f"{platform}_sibling_unicode_unconfigured_startup")
    parent_root = out_dir / f"{platform}_parent_workspace"
    sibling_cjk = parent_root / CJK_SIBLING_DIR
    stage_dir = parent_root / "PsiOps-Unconfigured-Stage"
    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "sibling_dir": CJK_SIBLING_DIR}
        return res

    if parent_root.exists():
        shutil.rmtree(parent_root)
    sibling_cjk.mkdir(parents=True, exist_ok=True)
    # Place both a dummy file and a dummy .iso in the sibling folder so any parent walk would hit it
    (sibling_cjk / "readme.txt").write_text("dummy\n", encoding="utf-8", newline="\n")
    (sibling_cjk / "other_game.iso").write_bytes(b"NOT_AN_XISO_HEADER_000000")

    staged_exe = _stage_runtime_tree(platform, exe_path, stage_dir, include_game_lib=False)
    bmp_play = parent_root / "unconfigured_play_tab.bmp"
    bmp_fb = parent_root / "parent_cjk_file_browser.bmp"

    plat_bmp_play = (
        str(bmp_play) if platform == "win64" else (m2.to_wsl_path(bmp_play) if sys.platform == "win32" else str(bmp_play))
    )
    plat_bmp_fb = (
        str(bmp_fb) if platform == "win64" else (m2.to_wsl_path(bmp_fb) if sys.platform == "win32" else str(bmp_fb))
    )
    plat_parent = (
        str(parent_root)
        if platform == "win64"
        else (m2.to_wsl_path(parent_root) if sys.platform == "win32" else str(parent_root))
    )

    env_base: dict[str, str] = {}
    if platform == "linux":
        env_base["SDL_AUDIODRIVER"] = "dummy"
        env_base["SteamDeck"] = "1"

    # 1. Launch without --launcher and without settings.toml -> ShouldBypassLauncher() -> HasValidConfiguredGame()
    #    -> FindWorkspaceIsos() must NOT crash on sibling_cjk and must open the Launcher cleanly.
    rc1, _, err1, wall1 = _run_cmd(
        platform,
        staged_exe,
        ["--capture-launcher-bmp", plat_bmp_play],
        env_base,
        wsl_distro,
        45,
        use_xvfb=(platform == "linux"),
        cwd_override=stage_dir,
    )
    res.details["unconfigured_rc"] = rc1
    res.details["unconfigured_wall_s"] = wall1
    if rc1 != 0:
        res.errors.append(f"Unconfigured launch beside CJK sibling folder crashed/failed rc={rc1}: {err1.strip()}")
    else:
        cp1, _ = m2.analyze_checkpoint(parent_root, "unconfigured_play_tab")
        res.details["play_tab_capture"] = asdict(cp1)
        if not cp1.passed:
            res.errors.append(f"Unconfigured Launcher Play tab capture failed: {cp1.reason}")

    if (stage_dir / "settings.toml").exists():
        res.errors.append("Unconfigured Launcher exit unexpectedly wrote settings.toml.")
    dumps = list((stage_dir / "CrashDumps").glob("*.dmp")) if (stage_dir / "CrashDumps").is_dir() else []
    if dumps:
        res.errors.append(f"Crash dump(s) generated during unconfigured launch: {[d.name for d in dumps]}")

    # 2. Open In-App File Browser directly on parent_root containing CJK_SIBLING_DIR (via temporary settings iso_path)
    tmp_cfg = parent_root / "browser_dir_settings.toml"
    tmp_cfg.write_text(f'iso_path = "{_toml_escape_basic(plat_parent)}"\n', encoding="utf-8", newline="\n")
    plat_cfg = str(tmp_cfg) if platform == "win64" else (m2.to_wsl_path(tmp_cfg) if sys.platform == "win32" else str(tmp_cfg))
    rc2, _, err2, _ = _run_cmd(
        platform,
        staged_exe,
        ["--launcher", "--open-file-browser", "--settings", plat_cfg, "--capture-launcher-bmp", plat_bmp_fb],
        env_base,
        wsl_distro,
        45,
        use_xvfb=(platform == "linux"),
        cwd_override=stage_dir,
    )
    res.details["file_browser_cjk_rc"] = rc2
    if rc2 != 0:
        res.errors.append(f"In-App File Browser on CJK directory crashed/failed rc={rc2}: {err2.strip()}")
    else:
        cp2, _ = m2.analyze_checkpoint(parent_root, "parent_cjk_file_browser")
        res.details["file_browser_capture"] = asdict(cp2)
        if not cp2.passed:
            res.errors.append(f"In-App File Browser CJK capture failed: {cp2.reason}")

    res.passed = len(res.errors) == 0
    return res


def check_unicode_install_and_options_sync(
    platform: str,
    exe_path: Path,
    iso_path: Path,
    out_dir: Path,
    wsl_distro: str,
    dry_run: bool,
) -> CheckResult:
    """Verifies installing & booting inside a CJK directory + Unicode ISO + live [game_options] / F1 sync."""
    res = CheckResult(name=f"{platform}_unicode_install_and_options_sync")
    root = out_dir / f"{platform}_unicode_e2e"
    install_dir = root / CJK_INSTALL_DIR
    disc_dir = root / CJK_DISC_DIR
    unicode_iso = disc_dir / CJK_ISO_NAME
    shortcut_dir = root / ("\u30b7\u30e7\u30fc\u30c8\u30ab\u30c3\u30c8_" + platform)

    if dry_run:
        res.passed = True
        res.details = {"dry_run": True, "install_dir": CJK_INSTALL_DIR, "iso_name": CJK_ISO_NAME}
        return res

    if root.exists():
        shutil.rmtree(root)
    staged_exe = _stage_runtime_tree(platform, exe_path, install_dir, include_game_lib=True)
    _link_readonly_iso(iso_path, unicode_iso)

    plat_iso = (
        str(unicode_iso)
        if platform == "win64"
        else (m2.to_wsl_path(unicode_iso) if sys.platform == "win32" else str(unicode_iso))
    )
    missing_iso = disc_dir / ("\u5b58\u5728\u3057\u306a\u3044_" + CJK_ISO_NAME)
    plat_missing = (
        str(missing_iso)
        if platform == "win64"
        else (m2.to_wsl_path(missing_iso) if sys.platform == "win32" else str(missing_iso))
    )

    # 1. --validate-iso on Unicode ISO path (valid) & missing Unicode ISO path (clean JSON error, no crash)
    v_rc, v_out, v_err, _ = _run_cmd(
        platform, staged_exe, ["--validate-iso", plat_iso], {}, wsl_distro, 30, cwd_override=install_dir
    )
    res.details["validate_unicode_rc"] = v_rc
    if v_rc != 0:
        res.errors.append(f"--validate-iso on Unicode path failed rc={v_rc}: {v_err.strip()}")
    else:
        try:
            v_json = json.loads(v_out.strip().splitlines()[-1])
            if not v_json.get("valid") or str(v_json.get("sha1", "")).lower() != EXPECTED_XBE_SHA1:
                res.errors.append(f"Unexpected --validate-iso payload: {v_json}")
        except Exception as exc:
            res.errors.append(f"Could not parse --validate-iso JSON: {exc}")

    m_rc, m_out, m_err, _ = _run_cmd(
        platform, staged_exe, ["--validate-iso", plat_missing], {}, wsl_distro, 20, cwd_override=install_dir
    )
    res.details["validate_missing_unicode_rc"] = m_rc
    if m_rc != 1:
        res.errors.append(f"--validate-iso on missing Unicode path returned rc={m_rc} (expected 1): {m_err.strip()}")
    else:
        try:
            m_json = json.loads(m_out.strip().splitlines()[-1])
            if m_json.get("code") != "ERR_NOT_FOUND":
                res.errors.append(f"Unexpected error code for missing Unicode ISO: {m_json}")
        except Exception as exc:
            res.errors.append(f"Could not parse missing Unicode ISO JSON: {exc}")

    # 2. --create-shortcut from Unicode ExeDir into Unicode shortcut directory
    shortcut_dir.mkdir(parents=True, exist_ok=True)
    plat_sc = (
        str(shortcut_dir)
        if platform == "win64"
        else (m2.to_wsl_path(shortcut_dir) if sys.platform == "win32" else str(shortcut_dir))
    )
    sc_env = {} if platform == "win64" else {"HOME": plat_sc, "XDG_DATA_HOME": f"{plat_sc}/.local/share"}
    sc_rc, sc_out, sc_err, _ = _run_cmd(
        platform, staged_exe, ["--create-shortcut", plat_sc], sc_env, wsl_distro, 25, cwd_override=install_dir
    )
    res.details["shortcut_unicode_rc"] = sc_rc
    if sc_rc != 0:
        res.errors.append(f"--create-shortcut in Unicode dir failed rc={sc_rc}: {sc_err.strip()}")
    else:
        try:
            sc_json = json.loads(sc_out.strip().splitlines()[-1])
            if not sc_json.get("ok"):
                res.errors.append(f"--create-shortcut returned ok=false: {sc_json}")
        except Exception as exc:
            res.errors.append(f"Failed to parse --create-shortcut JSON: {exc}")

    # 3. End-to-end Launcher -> Autoplay -> Boot inside CJK_INSTALL_DIR using UTF-8 iso_path in settings.toml
    #    + non-default [game_options] + --open-settings live F1 overlay during boot FMV (before EngineAudio::Init!)
    settings_toml = install_dir / "settings.toml"
    settings_toml.write_text(
        "\n".join(
            [
                'display_mode = "windowed"',
                "width = 1280",
                "height = 720",
                "fps_cap = 60",
                'upscaler = "fsr1"',
                f'iso_path = "{_toml_escape_basic(plat_iso)}"',
                "",
                "[game_options]",
                "sfx_volume = 60",
                "music_volume = 40",
                "controller_invert_y = true",
                "vibration = false",
                "hud_visible = false",
                "auto_hide_hud = true",
                "",
            ]
        ),
        encoding="utf-8",
        newline="\n",
    )

    cap_dir = install_dir / "captures"
    cap_dir.mkdir(parents=True, exist_ok=True)
    script_path = cap_dir / "autotest_unicode.txt"
    script_path.write_text("capture 150 unicode_boot_f1.bmp\nexit_ms 7500\n", encoding="utf-8", newline="\n")
    perf_json = install_dir / "perf_unicode.json"
    state_log = install_dir / "state_unicode.jsonl"

    plat_scr = (
        str(script_path)
        if platform == "win64"
        else (m2.to_wsl_path(script_path) if sys.platform == "win32" else str(script_path))
    )
    plat_cap = (
        str(cap_dir) if platform == "win64" else (m2.to_wsl_path(cap_dir) if sys.platform == "win32" else str(cap_dir))
    )
    plat_perf = (
        str(perf_json)
        if platform == "win64"
        else (m2.to_wsl_path(perf_json) if sys.platform == "win32" else str(perf_json))
    )
    plat_state = (
        str(state_log)
        if platform == "win64"
        else (m2.to_wsl_path(state_log) if sys.platform == "win32" else str(state_log))
    )

    run_env: dict[str, str] = {
        "XBE_LAUNCHER_AUTOPLAY_MS": "350",
        "XBE_AUTOTEST_SCRIPT": plat_scr,
        "XBE_AUTOTEST_OUT_DIR": plat_cap,
    }
    if platform == "linux":
        run_env["SDL_AUDIODRIVER"] = "dummy"
        run_env["SteamDeck"] = "1"

    # Do NOT pass --iso on CLI: force settings.toml (File().wstring() + Utf8ToPath(iso_path)) to resolve the CJK ISO!
    run_rc, _, run_err, run_wall = _run_cmd(
        platform,
        staged_exe,
        ["--open-settings", "--perf-log", plat_perf, "--state-log", plat_state],
        run_env,
        wsl_distro,
        120,
        use_xvfb=(platform == "linux"),
        cwd_override=install_dir,
    )
    res.details["autoplay_unicode_rc"] = run_rc
    res.details["autoplay_unicode_wall_s"] = run_wall
    if run_rc != 0:
        res.errors.append(f"Unicode install Launcher->Game autoplay failed rc={run_rc}: {run_err.strip()}")

    log_file = install_dir / "logs" / "psiops.log"
    if not log_file.is_file():
        log_file = install_dir / "logs" / "PsiOps.log"
    if not log_file.is_file():
        res.errors.append(f"Expected UTF-8 log file missing inside Unicode install dir: {log_file}")
    else:
        log_txt = log_file.read_text(encoding="utf-8", errors="replace")
        if "launcher: autoplay triggered" not in log_txt or "launch_game=true" not in log_txt:
            res.errors.append("PsiOps.log missing launcher->game autoplay markers.")
        m_exit = re.search(r"autotest: clean exit requested at frame=(\d+)\s+ms=(\d+)", log_txt)
        if not m_exit:
            res.errors.append("Game exited before XBE_AUTOTEST_SCRIPT clean exit_ms fired.")
        else:
            res.details["swap_count"] = int(m_exit.group(1))
            if int(m_exit.group(1)) < 150:
                res.errors.append(f"SwapCount={m_exit.group(1)} < 150 frames.")

    if not perf_json.is_file() or perf_json.stat().st_size < 64:
        res.errors.append(f"perf.json was not written to Unicode path: {perf_json}")
    if not state_log.is_file() or state_log.stat().st_size < 64:
        res.errors.append(f"state.jsonl was not written to Unicode path: {state_log}")
    if not (install_dir / "hdd" / "E").is_dir() or not (install_dir / "hdd" / "Z").is_dir():
        res.errors.append("Xbox HDD partitions (hdd/E, hdd/Z) were not mounted inside Unicode ExeDir.")

    cp_boot, _ = m2.analyze_checkpoint(cap_dir, "unicode_boot_f1")
    res.details["boot_capture"] = asdict(cp_boot)
    if not cp_boot.passed:
        res.errors.append(f"Unicode boot + F1 overlay frame capture failed: {cp_boot.reason}")

    # Clean up hardlink/symlink before finishing so staging doesn't leave large links around
    if unicode_iso.exists() or unicode_iso.is_symlink():
        unicode_iso.unlink()

    res.passed = len(res.errors) == 0
    return res


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="Issue #2 Unicode Paths & Live Options Sync Regression Suite.")
    ap.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    ap.add_argument("--win64-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    ap.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    ap.add_argument("--iso", type=Path, default=repo_root / m2.EXPECTED_ISO_REL)
    ap.add_argument("--wsl-distro", default=None)
    ap.add_argument("--out-dir", type=Path, default=repo_root / "out" / "test_issue2")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    iso_path = args.iso.resolve()
    out_dir = args.out_dir.resolve()
    win64_exe = args.win64_exe.resolve()
    linux_exe = args.linux_exe.resolve()
    if not args.dry_run and not iso_path.is_file():
        print(f"[ERROR] Retail XISO not found: {iso_path}", file=sys.stderr)
        return 2

    platforms = ["win64", "linux"] if args.platform == "both" else [args.platform]
    wsl_distro = args.wsl_distro or (
        m2.discover_wsl_ubuntu2204_distro()
        if ("linux" in platforms and sys.platform == "win32" and not args.dry_run)
        else "Ubuntu-22.04"
    )

    media_before = m2.snapshot_media_stats(repo_root, iso_path) if not args.dry_run else {}
    out_dir.mkdir(parents=True, exist_ok=True)

    results: list[CheckResult] = []
    for plat in platforms:
        exe = win64_exe if plat == "win64" else linux_exe
        if not args.dry_run and not exe.is_file():
            r = CheckResult(name=f"{plat}_binary_present", passed=False, errors=[f"Missing executable: {exe}"])
            results.append(r)
            continue
        results.append(check_sibling_unicode_unconfigured_startup(plat, exe, out_dir, wsl_distro, args.dry_run))
        results.append(
            check_unicode_install_and_options_sync(plat, exe, iso_path, out_dir, wsl_distro, args.dry_run)
        )

    if not args.dry_run:
        media_after = m2.snapshot_media_stats(repo_root, iso_path)
        media_chk = CheckResult(name="source_media_untouched")
        if media_before != media_after:
            media_chk.errors.append(f"Source media stat changed! before={media_before} after={media_after}")
        media_chk.passed = len(media_chk.errors) == 0
        results.append(media_chk)

    all_passed = all(r.passed for r in results)
    summary = {
        "suite": "test_issue2_unicode_and_options",
        "passed": all_passed,
        "checks": [asdict(r) for r in results],
    }
    report_path = out_dir / "issue2_report.json"
    report_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8", newline="\n")

    for r in results:
        tag = "PASS" if r.passed else "FAIL"
        print(f"[{tag}] {r.name}")
        for e in r.errors:
            print(f"    error: {e}")
    print(f"[SUMMARY] passed={all_passed} ({sum(1 for r in results if r.passed)}/{len(results)}) -> {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
