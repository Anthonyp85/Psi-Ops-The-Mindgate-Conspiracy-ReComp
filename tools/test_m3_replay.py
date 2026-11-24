#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 3 automated controller .rec recording & deterministic replay suite.

Aligned with runtime Stage 1B interface:
  - CLI: --record-input <file.rec>, --replay-input <file.rec>,
         --replay-mode hybrid|frame|time, --replay-exit,
         --state-log <file.jsonl>, --perf-log <file.json>
  - Env: XBE_AUTOTEST=1, XBE_AUTOTEST_SCRIPT=<script>, XBE_AUTOTEST_OUT_DIR=<dir>,
         XBE_MKB_AUTOTEST=<noise_script> (for replay M&K bypass isolation)

Records a controller-driven session covering:
  Title Screen -> Main Menu -> New Game -> Level 1 Active 3D Gameplay
  (movement, camera, firing/rumble) -> PAUSED -> RESUME GAME -> PAUSED ->
  RESTART CHECKPOINT -> Active 3D Gameplay -> PAUSED -> GO TO MAIN MENU.

Replays across Windows Release (2x hybrid, frame, time, cold cache, M&K noise
isolation), Windows Debug, and WSL2 Ubuntu 22.04 Linux Release (Vulkan 1.2),
verifying --state-log schema, lifecycle transitions, rumble events, multi-run
determinism, zero M&K interference during replay, process hygiene, and
.xiso.iso mtime_ns immutability.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_m2_gameplay as m2  # noqa: E402

REQUIRED_STATE_LOG_KEYS = (
    "frame_id",
    "timestamp_us",
    "eff_us",
    "mode",
    "phase",
    "state",
    "in_shell",
    "fmv_active",
    "sim_elapsed_s",
    "rumble_left",
    "rumble_right",
)

# Extended Milestone 3 controller recording script (ms from first Present()).
# Pause menu order (verified in Milestone 2):
#   0: RESUME GAME | 1: CONTROL LAYOUT | 2: OPTIONS | 3: SAVE CHECKPOINT
#   4: LOAD GAME | 5: RESTART CHECKPOINT | 6: SHOW QUICK TIP | 7: GO TO MAIN MENU
DEFAULT_M3_RECORD_SCRIPT = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops Milestone 3 controller session script for --record-input
# Covers: Title -> Main Menu -> New Game -> Level 1 Gameplay (move/look/fire/rumble)
#         -> Pause -> Resume -> Pause -> Restart Checkpoint -> Gameplay
#         -> Pause -> Go To Main Menu (quit to title) -> clean exit.

# 1. Boot FMVs (~20.5s) -> Title Screen (~25s) -> START -> Main Menu
pad_ms 26400 26800 START

# 2. Main Menu: START NEW GAME (A) -> Confirm Difficulty Normal (A)
pad_ms 29000 29400 A
pad_ms 30800 31200 A

# 3. Skip Intro FMVs into Level 1 Solitary Confinement
pad_ms 35000 35350 START
pad_ms 36400 36750 START
pad_ms 37800 38150 START
pad_ms 45500 45850 START

# 4. Dismiss Mission Objectives popup ("Accept (A)")
pad_ms 50000 50350 A
pad_ms 51200 51550 A
pad_ms 52400 52750 A

# 5. Active 3D Gameplay Segment 1: walk, strafe, look, fire (RT -> rumble), psi
pad_ms 53800 55500 none 0 26000 6000 0 0 0
pad_ms 55700 57800 RT 18000 16000 20000 -1500 0 255
pad_ms 58000 59500 A -16000 20000 18000 1000 0 0
pad_ms 59700 61500 LT,X 0 14000 -18000 1000 255 0

# 6. PAUSED -> RESUME GAME
pad_ms 62500 62850 START
pad_ms 64200 64550 A

# 7. Active 3D Gameplay Segment 2 (post-resume motion + shot)
pad_ms 65500 67200 RT 0 22000 14000 0 0 255

# 8. PAUSED -> RESTART CHECKPOINT (index 5: DOWN x5, A, confirm dialog defaults to NO -> UP -> YES, A)
pad_ms 68000 68350 START
pad_ms 69200 69400 DOWN
pad_ms 69650 69850 DOWN
pad_ms 70100 70300 DOWN
pad_ms 70550 70750 DOWN
pad_ms 71000 71200 DOWN
pad_ms 71700 72000 A
pad_ms 72600 72850 UP
pad_ms 73300 73600 A

# 9. Dismiss post-checkpoint Mission Objectives popup (~4.1s after respawn -> ~77.5s) & move in active 3D gameplay
pad_ms 78500 78850 A
pad_ms 79500 79850 A
pad_ms 80400 81800 none 0 22000 10000 0 0 0

# 10. PAUSED -> GO TO MAIN MENU (index 7: DOWN x7, A -> "Save Before Quitting?" [Yes/No/Don't Quit] -> UP, DOWN -> No, A)
pad_ms 82600 82950 START
pad_ms 83800 84000 DOWN
pad_ms 84250 84450 DOWN
pad_ms 84700 84900 DOWN
pad_ms 85150 85350 DOWN
pad_ms 85600 85800 DOWN
pad_ms 86050 86250 DOWN
pad_ms 86500 86700 DOWN
pad_ms 87200 87500 A
pad_ms 88100 88350 UP
pad_ms 88650 88900 DOWN
pad_ms 89300 89600 A

# 11. Settle back in Title/Main Menu shell and exit cleanly
exit_ms 95500
"""

# Aggressive M&K noise script (XBE_MKB_AUTOTEST=<path>) injected simultaneously
# during --replay-input. Must be 100% bypassed via IsReplayActive()!
MKB_REPLAY_ISOLATION_NOISE_SCRIPT = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic M&K noise injected during --replay-input to prove M&K bypass isolation.
key_ms 54000 59000 W,D,Space,LMB,RMB
mouse_ms 54500 400 -250
mouse_ms 56000 -500 300
wheel_ms 56500 1
wheel_ms 57500 -1
key_ms 60000 60300 Escape
key_ms 66000 66300 Escape
mouse_ms 79500 600 200
"""


@dataclass
class StateLogSummary:
    record_count: int = 0
    schema_errors: list[str] = field(default_factory=list)
    phase_sequence: list[str] = field(default_factory=list)
    macro_phase_sequence: list[str] = field(default_factory=list)
    max_frame_id: int = 0
    max_timestamp_us: int = 0
    max_eff_us: int = 0
    rumble_nonzero_events: int = 0
    mkb_bypassed_records: int = 0
    gameplay_segments: int = 0
    pause_segments: int = 0
    sim_time_resets: int = 0
    returned_to_shell: bool = False
    gameplay_samples: list[dict[str, Any]] = field(default_factory=list)
    final_record: dict[str, Any] = field(default_factory=dict)


@dataclass
class ReplayRunResult:
    name: str
    platform: str
    build_kind: str
    replay_mode: str
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    passed: bool = False
    log_path: str = ""
    state_log_path: str = ""
    perf_json_path: str = ""
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    summary: dict[str, Any] = field(default_factory=dict)


def _extract_pos(rec: dict[str, Any]) -> tuple[float, float, float] | None:
    if "player_x" in rec and "player_y" in rec and "player_z" in rec:
        return (float(rec["player_x"]), float(rec["player_y"]), float(rec["player_z"]))
    pos = rec.get("player_pos")
    if isinstance(pos, list) and len(pos) == 3:
        return (float(pos[0]), float(pos[1]), float(pos[2]))
    return None


def _extract_ammo(rec: dict[str, Any]) -> tuple[int, int] | None:
    if "ammo_clip" in rec:
        return (int(rec["ammo_clip"]), int(rec.get("ammo_reserve", 0)))
    if "ammo" in rec:
        return (int(rec["ammo"]), 0)
    return None


def parse_state_log(path: Path) -> StateLogSummary:
    summary = StateLogSummary()
    if not path.is_file():
        summary.schema_errors.append(f"Missing state log JSONL: {path}")
        return summary

    lines = [ln.strip() for ln in path.read_text(encoding="utf-8", errors="replace").splitlines() if ln.strip()]
    if not lines:
        summary.schema_errors.append(f"State log JSONL is empty: {path}")
        return summary

    prev_frame = -1
    prev_ts = -1
    prev_sim = -1.0
    last_phase = ""
    saw_gameplay = False
    records: list[dict[str, Any]] = []

    for idx, line in enumerate(lines, start=1):
        try:
            rec = json.loads(line)
        except Exception as exc:
            summary.schema_errors.append(f"Line {idx}: invalid JSON ({exc})")
            continue
        if not isinstance(rec, dict):
            summary.schema_errors.append(f"Line {idx}: expected JSON object")
            continue

        missing = [k for k in REQUIRED_STATE_LOG_KEYS if k not in rec]
        if missing:
            summary.schema_errors.append(f"Line {idx}: missing required keys {missing}")
            continue

        frame_id = int(rec["frame_id"])
        ts_us = int(rec["timestamp_us"])
        eff_us = int(rec["eff_us"])
        phase = str(rec["phase"])
        sim_s = float(rec["sim_elapsed_s"])
        r_left = int(rec["rumble_left"])
        r_right = int(rec["rumble_right"])

        if frame_id < prev_frame:
            summary.schema_errors.append(f"Line {idx}: non-monotonic frame_id ({frame_id} < {prev_frame})")
        if ts_us < prev_ts:
            summary.schema_errors.append(f"Line {idx}: non-monotonic timestamp_us ({ts_us} < {prev_ts})")
        if eff_us < 0:
            summary.schema_errors.append(f"Line {idx}: negative eff_us ({eff_us})")

        prev_frame = frame_id
        prev_ts = ts_us
        summary.max_frame_id = max(summary.max_frame_id, frame_id)
        summary.max_timestamp_us = max(summary.max_timestamp_us, ts_us)
        summary.max_eff_us = max(summary.max_eff_us, eff_us)

        if r_left > 0 or r_right > 0:
            summary.rumble_nonzero_events += 1
        if rec.get("mkb_hook_bypassed") is True:
            summary.mkb_bypassed_records += 1

        if phase != last_phase:
            summary.phase_sequence.append(phase)
            if phase == "gameplay_active":
                summary.gameplay_segments += 1
                saw_gameplay = True
            elif phase == "paused":
                summary.pause_segments += 1
            elif saw_gameplay and phase == "title_menu":
                summary.returned_to_shell = True
            last_phase = phase

        if phase == "gameplay_active":
            if prev_sim > 1.0 and sim_s + 0.5 < prev_sim:
                summary.sim_time_resets += 1
            prev_sim = sim_s
            summary.gameplay_samples.append(rec)

        records.append(rec)

    summary.record_count = len(records)
    if records:
        summary.final_record = records[-1]
        # Build debounced macro_phase_sequence (ignores <200ms single-frame inter-FMV/spawn blips)
        runs: list[tuple[str, int, int]] = []
        for rec in records:
            ph = str(rec["phase"])
            eu = int(rec["eff_us"])
            if runs and runs[-1][0] == ph:
                runs[-1] = (ph, runs[-1][1], eu)
            else:
                runs.append((ph, eu, eu))
        # Build debounced macro_phase_sequence:
        # - skips loading_transition
        # - ignores <200ms single-frame blips AND <1000ms black gaps between consecutive FMVs
        for idx_r, (ph, t0, t1) in enumerate(runs):
            if ph == "loading_transition":
                continue
            dur_us = t1 - t0
            is_last = idx_r == len(runs) - 1
            if not is_last and dur_us < 200_000:
                continue
            if ph == "title_menu" and not is_last and dur_us < 1_000_000:
                continue
            if not summary.macro_phase_sequence or summary.macro_phase_sequence[-1] != ph:
                summary.macro_phase_sequence.append(ph)
    return summary


def validate_lifecycle(summary: StateLogSummary, strict_full_lifecycle: bool) -> tuple[list[str], list[str]]:
    errs: list[str] = []
    warns: list[str] = []
    errs.extend(summary.schema_errors)

    if summary.record_count < 10:
        errs.append(f"Too few state-log records ({summary.record_count} < 10).")

    if "gameplay_active" not in summary.phase_sequence:
        errs.append(f"Never entered gameplay_active (phases={summary.phase_sequence}).")
    if "paused" not in summary.phase_sequence:
        errs.append(f"Never entered paused state (phases={summary.phase_sequence}).")
    if summary.gameplay_segments < 2:
        msg = f"Expected >= 2 gameplay_active segments (pause/resume + checkpoint restart), got {summary.gameplay_segments}."
        if strict_full_lifecycle:
            errs.append(msg)
        else:
            warns.append(msg)
    if summary.pause_segments < 2:
        msg = f"Expected >= 2 pause segments (resume, restart checkpoint, quit to title), got {summary.pause_segments}."
        if strict_full_lifecycle:
            errs.append(msg)
        else:
            warns.append(msg)
    if not summary.returned_to_shell:
        msg = f"Did not observe return to title_menu after gameplay (phases={summary.phase_sequence})."
        if strict_full_lifecycle:
            errs.append(msg)
        else:
            warns.append(msg)
    if summary.rumble_nonzero_events == 0:
        warns.append("No non-zero rumble events recorded during session (verify fire/hit rumble telemetry).")
    return errs, warns


def compare_state_logs(
    ref: StateLogSummary,
    cand: StateLogSummary,
    label: str,
    strict_isolation: bool = False,
) -> list[str]:
    errs: list[str] = []
    if ref.macro_phase_sequence != cand.macro_phase_sequence:
        errs.append(
            f"[{label}] Macro phase sequence diverged: expected {ref.macro_phase_sequence}, got {cand.macro_phase_sequence}."
        )

    for key in ("phase", "state", "in_shell", "fmv_active"):
        if ref.final_record.get(key) != cand.final_record.get(key):
            errs.append(
                f"[{label}] Final '{key}' mismatch: ref={ref.final_record.get(key)!r}, cand={cand.final_record.get(key)!r}."
            )

    if strict_isolation and cand.mkb_bypassed_records == 0:
        errs.append(f"[{label}] Expected mkb_hook_bypassed=true records during replay M&K isolation test.")

    if ref.gameplay_samples and cand.gameplay_samples:
        r_last = ref.gameplay_samples[-1]
        c_last = cand.gameplay_samples[-1]

        r_ammo = _extract_ammo(r_last)
        c_ammo = _extract_ammo(c_last)
        if r_ammo is not None and c_ammo is not None and r_ammo != c_ammo:
            errs.append(f"[{label}] Final gameplay ammo mismatch: ref={r_ammo}, cand={c_ammo}.")

        rp = _extract_pos(r_last)
        cp = _extract_pos(c_last)
        if rp is not None and cp is not None:
            dist = math.sqrt(sum((a - b) ** 2 for a, b in zip(rp, cp)))
            tol = 0.25 if strict_isolation else 1.5
            if dist > tol:
                errs.append(
                    f"[{label}] Final gameplay player_pos diverged by {dist:.3f} (> {tol}): ref={rp}, cand={cp}."
                )

        if "cam_yaw" in r_last and "cam_yaw" in c_last:
            dyaw = abs(float(r_last["cam_yaw"]) - float(c_last["cam_yaw"]))
            if dyaw > math.pi:
                dyaw = abs(2.0 * math.pi - dyaw)
            tol_rad = 0.02 if strict_isolation else 0.15
            if dyaw > tol_rad:
                errs.append(
                    f"[{label}] Final gameplay cam_yaw diverged by {dyaw:.4f} rad (> {tol_rad})."
                )

        if strict_isolation:
            if (ref.rumble_nonzero_events > 0) != (cand.rumble_nonzero_events > 0):
                errs.append(
                    f"[{label}] Rumble activity diverged under M&K noise: ref={ref.rumble_nonzero_events}, cand={cand.rumble_nonzero_events}."
                )
    return errs


def _check_log_clean(log_text: str) -> list[str]:
    errs: list[str] = []
    for pat in m2.CRASH_PATTERNS:
        m = pat.search(log_text)
        if m:
            errs.append(f"Crash/exception in log: {m.group(0)!r}")
    for pat in m2.SHADER_GPU_ERROR_PATTERNS:
        m = pat.search(log_text)
        if m:
            errs.append(f"GPU/shader error in log: {m.group(0)!r}")
    if "Exit (0)" not in log_text:
        errs.append("Log missing clean 'Exit (0)'.")
    return errs


def execute_session(
    name: str,
    platform: str,
    build_kind: str,
    exe_path: Path,
    iso_path: Path,
    repo_root: Path,
    run_dir: Path,
    cli_args: list[str],
    extra_env: dict[str, str],
    replay_mode: str,
    timeout_s: int,
    wsl_distro: str,
    dry_run: bool,
) -> tuple[ReplayRunResult, StateLogSummary | None]:
    res = ReplayRunResult(
        name=name,
        platform=platform,
        build_kind=build_kind,
        replay_mode=replay_mode,
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    state_log = run_dir / "state_log.jsonl"
    perf_json = run_dir / "perf.json"
    saved_log = run_dir / "psiops.log"
    res.state_log_path = str(state_log)
    res.perf_json_path = str(perf_json)
    res.log_path = str(saved_log)

    if dry_run:
        res.exit_code = 0
        res.passed = True
        res.summary = {"dry_run": True, "exe": str(exe_path), "cli": cli_args}
        return res, None

    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res, None

    media_before = m2.snapshot_media_stats(repo_root, iso_path)
    log_file = exe_path.parent / "logs" / "psiops.log"

    if platform == "win64":
        pre_pids = m2.get_running_win_psiops()
        if pre_pids:
            res.errors.append(f"Refusing to launch: PsiOps.exe already running (PIDs={pre_pids}).")
            return res, None

        env = os.environ.copy()
        env["XBE_AUTOTEST"] = "1"
        env["PSIOPS_AUTOTEST"] = "1"
        env["XBE_ISO_PATH"] = str(iso_path)
        env["XBE_PERF_LOG"] = str(perf_json)
        env.update(extra_env)

        cmd = [
            str(exe_path),
            "--no-launcher",
            "--iso",
            str(iso_path),
            "--state-log",
            str(state_log),
            "--perf-log",
            str(perf_json),
        ] + cli_args

        t0 = time.monotonic()
        stdout_s, stderr_s = "", ""
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
                res.errors.append(f"{name}: leftover PsiOps.exe processes after exit: {post_pids}.")
    else:
        pre_pids = m2.get_running_wsl_psiops(wsl_distro)
        if pre_pids:
            res.errors.append(f"Refusing to launch: Linux PsiOps already running in {wsl_distro} (PIDs={pre_pids}).")
            return res, None

        wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
        wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
        wsl_iso = m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path)
        wsl_state = m2.to_wsl_path(state_log) if sys.platform == "win32" else str(state_log)
        wsl_perf = m2.to_wsl_path(perf_json) if sys.platform == "win32" else str(perf_json)

        env_kv = [
            "XBE_AUTOTEST=1",
            "PSIOPS_AUTOTEST=1",
            f"XBE_ISO_PATH={wsl_iso}",
            f"XBE_PERF_LOG={wsl_perf}",
            "SDL_AUDIODRIVER=dummy",
        ] + [f"{k}={v}" for k, v in extra_env.items()]

        inner = (
            ["env"]
            + env_kv
            + [
                "xvfb-run",
                "-a",
                "-s",
                "-screen 0 640x480x24",
                wsl_exe,
                "--no-launcher",
                "--iso",
                wsl_iso,
                "--state-log",
                wsl_state,
                "--perf-log",
                wsl_perf,
            ]
            + cli_args
        )
        cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + inner if sys.platform == "win32" else inner

        t0 = time.monotonic()
        stdout_s, stderr_s = "", ""
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
                res.errors.append(f"{name}: leftover Linux PsiOps processes after exit: {post_pids}.")

    if log_file.is_file():
        shutil.copy2(log_file, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"{name}: non-zero exit code {res.exit_code}.")

    res.errors.extend(m2.assert_media_unchanged(media_before))
    res.errors.extend(_check_log_clean(log_text))

    summary = parse_state_log(state_log)
    lc_errs, lc_warns = validate_lifecycle(summary, strict_full_lifecycle=True)
    res.errors.extend(lc_errs)
    res.warnings.extend(lc_warns)
    res.summary = {
        "record_count": summary.record_count,
        "phase_sequence": summary.phase_sequence,
        "macro_phase_sequence": summary.macro_phase_sequence,
        "max_frame_id": summary.max_frame_id,
        "max_timestamp_us": summary.max_timestamp_us,
        "max_eff_us": summary.max_eff_us,
        "rumble_nonzero_events": summary.rumble_nonzero_events,
        "mkb_bypassed_records": summary.mkb_bypassed_records,
        "gameplay_segments": summary.gameplay_segments,
        "pause_segments": summary.pause_segments,
        "sim_time_resets": summary.sim_time_resets,
        "returned_to_shell": summary.returned_to_shell,
        "final_record": summary.final_record,
    }
    res.passed = len(res.errors) == 0
    return res, summary


def _self_test_state_log_logic(tmp_dir: Path) -> None:
    """In-memory unit check run during --dry-run to validate JSONL parser & comparator."""
    sample = [
        {"frame_id": 100, "timestamp_us": 26000000, "eff_us": 26000000, "mode": "replay", "phase": "title_menu", "state": 1, "in_shell": 1, "fmv_active": 0, "sim_elapsed_s": 0.0, "rumble_left": 0, "rumble_right": 0, "mkb_hook_bypassed": True},
        {"frame_id": 3200, "timestamp_us": 53500000, "eff_us": 53500000, "mode": "replay", "phase": "gameplay_active", "state": 0, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 1.0, "rumble_left": 0, "rumble_right": 0, "player_x": 1.0, "player_y": 0.0, "player_z": 2.0, "cam_yaw": 0.5, "ammo_clip": 12, "ammo_reserve": 12, "mkb_hook_bypassed": True},
        {"frame_id": 3400, "timestamp_us": 56800000, "eff_us": 56800000, "mode": "replay", "phase": "gameplay_active", "state": 0, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 4.3, "rumble_left": 32000, "rumble_right": 16000, "player_x": 2.0, "player_y": 0.0, "player_z": 4.0, "cam_yaw": 0.9, "ammo_clip": 11, "ammo_reserve": 12, "mkb_hook_bypassed": True},
        {"frame_id": 3750, "timestamp_us": 62600000, "eff_us": 62600000, "mode": "replay", "phase": "paused", "state": 1, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 4.3, "rumble_left": 0, "rumble_right": 0, "mkb_hook_bypassed": True},
        {"frame_id": 3950, "timestamp_us": 66000000, "eff_us": 66000000, "mode": "replay", "phase": "gameplay_active", "state": 0, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 6.0, "rumble_left": 20000, "rumble_right": 0, "player_x": 3.0, "player_y": 0.0, "player_z": 5.0, "cam_yaw": 1.1, "ammo_clip": 10, "ammo_reserve": 12, "mkb_hook_bypassed": True},
        {"frame_id": 4100, "timestamp_us": 68500000, "eff_us": 68500000, "mode": "replay", "phase": "paused", "state": 1, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 6.0, "rumble_left": 0, "rumble_right": 0, "mkb_hook_bypassed": True},
        {"frame_id": 4650, "timestamp_us": 78000000, "eff_us": 78000000, "mode": "replay", "phase": "gameplay_active", "state": 0, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 0.5, "rumble_left": 0, "rumble_right": 0, "player_x": 1.0, "player_y": 0.0, "player_z": 2.0, "cam_yaw": 0.5, "ammo_clip": 12, "ammo_reserve": 12, "mkb_hook_bypassed": True},
        {"frame_id": 4800, "timestamp_us": 80500000, "eff_us": 80500000, "mode": "replay", "phase": "gameplay_active", "state": 0, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 3.0, "rumble_left": 0, "rumble_right": 0, "player_x": 1.5, "player_y": 0.0, "player_z": 3.0, "cam_yaw": 0.7, "ammo_clip": 12, "ammo_reserve": 12, "mkb_hook_bypassed": True},
        {"frame_id": 4950, "timestamp_us": 83000000, "eff_us": 83000000, "mode": "replay", "phase": "paused", "state": 1, "in_shell": 0, "fmv_active": 0, "sim_elapsed_s": 3.0, "rumble_left": 0, "rumble_right": 0, "mkb_hook_bypassed": True},
        {"frame_id": 5300, "timestamp_us": 89000000, "eff_us": 89000000, "mode": "replay", "phase": "title_menu", "state": 1, "in_shell": 1, "fmv_active": 0, "sim_elapsed_s": 0.0, "rumble_left": 0, "rumble_right": 0, "mkb_hook_bypassed": True},
    ]
    tmp_dir.mkdir(parents=True, exist_ok=True)
    p = tmp_dir / "_selftest_state.jsonl"
    p.write_text("\n".join(json.dumps(r) for r in sample) + "\n", encoding="utf-8", newline="\n")
    s1 = parse_state_log(p)
    errs, _ = validate_lifecycle(s1, strict_full_lifecycle=True)
    assert not errs, f"Self-test lifecycle errors: {errs}"
    assert s1.sim_time_resets == 1
    assert s1.rumble_nonzero_events == 2
    assert not compare_state_logs(s1, s1, "self", strict_isolation=True)
    p.unlink()


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Milestone 3 controller .rec recording & deterministic multi-run replay verification suite."
    )
    parser.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    parser.add_argument("--modes", type=str, default="hybrid,frame,time", help="Comma-separated --replay-mode values.")
    parser.add_argument("--timeout", type=int, default=150, help="Hard timeout in seconds per run.")
    parser.add_argument("--iso", type=Path, default=repo_root / m2.EXPECTED_ISO_REL)
    parser.add_argument("--win-release-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    parser.add_argument("--win-debug-exe", type=Path, default=repo_root / "out" / "build" / "debug" / "PsiOps.exe")
    parser.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    parser.add_argument("--wsl-distro", type=str, default=None)
    parser.add_argument("--record-script", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=repo_root / ".local" / "m3_replay")
    parser.add_argument("--skip-debug", action="store_true")
    parser.add_argument("--skip-cold-cache", action="store_true")
    parser.add_argument("--skip-mkb-isolation", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.iso.is_file():
        print(f"ERROR: XISO not found: {args.iso}", file=sys.stderr)
        return 2

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for mode in modes:
        if mode not in ("hybrid", "frame", "time"):
            print(f"ERROR: Invalid replay mode {mode!r} (expected hybrid|frame|time)", file=sys.stderr)
            return 2

    wsl_distro = ""
    if args.platform in ("linux", "both") and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.dry_run:
        _self_test_state_log_logic(args.out_dir)

    rec_script_text = args.record_script.read_text(encoding="utf-8") if args.record_script else DEFAULT_M3_RECORD_SCRIPT
    rec_script_path = args.out_dir / "record_script.txt"
    rec_script_path.write_text(rec_script_text, encoding="utf-8", newline="\n")

    noise_script_path = args.out_dir / "mkb_noise_script.txt"
    noise_script_path.write_text(MKB_REPLAY_ISOLATION_NOISE_SCRIPT, encoding="utf-8", newline="\n")

    rec_file = args.out_dir / "m3_session.rec"
    results: list[ReplayRunResult] = []
    all_passed = True

    rec_platform = "win64" if args.platform in ("win64", "both") else "linux"
    rec_exe = args.win_release_exe if rec_platform == "win64" else args.linux_exe
    rec_file_arg = str(rec_file) if rec_platform == "win64" else (m2.to_wsl_path(rec_file) if sys.platform == "win32" else str(rec_file))
    rec_script_env = str(rec_script_path) if rec_platform == "win64" else (m2.to_wsl_path(rec_script_path) if sys.platform == "win32" else str(rec_script_path))
    rec_out_env = str(args.out_dir / "record") if rec_platform == "win64" else (m2.to_wsl_path(args.out_dir / "record") if sys.platform == "win32" else str(args.out_dir / "record"))

    print(f"[test_m3_replay] Step 1: Recording {rec_file.name} on {rec_platform} Release...")
    rec_res, _ = execute_session(
        name="record_reference",
        platform=rec_platform,
        build_kind="release",
        exe_path=rec_exe,
        iso_path=args.iso,
        repo_root=repo_root,
        run_dir=args.out_dir / "record",
        cli_args=["--record-input", rec_file_arg],
        extra_env={"XBE_AUTOTEST_SCRIPT": rec_script_env, "XBE_AUTOTEST_OUT_DIR": rec_out_env},
        replay_mode="record",
        timeout_s=args.timeout,
        wsl_distro=wsl_distro,
        dry_run=args.dry_run,
    )
    if not args.dry_run and (not rec_file.is_file() or rec_file.stat().st_size == 0):
        rec_res.errors.append(f"Recorded .rec file missing or empty: {rec_file}")
        rec_res.passed = False
    results.append(rec_res)
    print(f"[test_m3_replay] record_reference: {'PASS' if rec_res.passed else 'FAIL'} (exit={rec_res.exit_code}, wall={rec_res.wall_s}s)")
    if not rec_res.passed:
        all_passed = False

    ref_hybrid_summary: StateLogSummary | None = None

    def _run_replay(
        run_name: str,
        plat: str,
        build_kind: str,
        exe: Path,
        mode: str,
        extra_env: dict[str, str],
        compare_against_ref: bool,
        strict_isolation: bool = False,
    ) -> StateLogSummary | None:
        nonlocal all_passed
        rec_arg = str(rec_file) if plat == "win64" else (m2.to_wsl_path(rec_file) if sys.platform == "win32" else str(rec_file))
        print(f"[test_m3_replay] Replaying {run_name} ({plat}/{build_kind}, mode={mode})...")
        r, summ = execute_session(
            name=run_name,
            platform=plat,
            build_kind=build_kind,
            exe_path=exe,
            iso_path=args.iso,
            repo_root=repo_root,
            run_dir=args.out_dir / run_name,
            cli_args=["--replay-input", rec_arg, "--replay-mode", mode, "--replay-exit"],
            extra_env=extra_env,
            replay_mode=mode,
            timeout_s=args.timeout,
            wsl_distro=wsl_distro,
            dry_run=args.dry_run,
        )
        if not args.dry_run and compare_against_ref and ref_hybrid_summary is not None and summ is not None:
            r.errors.extend(compare_state_logs(ref_hybrid_summary, summ, run_name, strict_isolation=strict_isolation))
            r.passed = len(r.errors) == 0
        results.append(r)
        print(f"[test_m3_replay] {run_name}: {'PASS' if r.passed else 'FAIL'} (exit={r.exit_code}, wall={r.wall_s}s)")
        for e in r.errors:
            print(f"  FAIL: {e}")
        if not r.passed:
            all_passed = False
        return summ

    if args.platform in ("win64", "both"):
        if "hybrid" in modes:
            ref_hybrid_summary = _run_replay("win64_release_hybrid_run1", "win64", "release", args.win_release_exe, "hybrid", {}, False)
            _run_replay("win64_release_hybrid_run2", "win64", "release", args.win_release_exe, "hybrid", {}, True)
        for mode in modes:
            if mode == "hybrid":
                continue
            _run_replay(f"win64_release_{mode}", "win64", "release", args.win_release_exe, mode, {}, ref_hybrid_summary is not None)
        if not args.skip_cold_cache:
            _run_replay(
                "win64_release_cold_cache",
                "win64",
                "release",
                args.win_release_exe,
                "hybrid",
                {"XBE_NO_SHADER_CACHE": "1"},
                ref_hybrid_summary is not None,
            )
        if not args.skip_mkb_isolation:
            _run_replay(
                "win64_release_mkb_isolation",
                "win64",
                "release",
                args.win_release_exe,
                "hybrid",
                {"XBE_MKB_AUTOTEST": str(noise_script_path)},
                ref_hybrid_summary is not None,
                strict_isolation=True,
            )
        if not args.skip_debug:
            _run_replay(
                "win64_debug_hybrid",
                "win64",
                "debug",
                args.win_debug_exe,
                "hybrid",
                {},
                ref_hybrid_summary is not None,
            )

    if args.platform in ("linux", "both"):
        _run_replay(
            "linux_release_hybrid",
            "linux",
            "release",
            args.linux_exe,
            "hybrid",
            {},
            ref_hybrid_summary is not None,
        )

    report = {
        "iso": str(args.iso),
        "rec_file": str(rec_file),
        "platform": args.platform,
        "modes": modes,
        "all_passed": all_passed,
        "results": [asdict(r) for r in results],
    }
    report_path = args.out_dir / "m3_replay_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"[test_m3_replay] Report written to: {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
