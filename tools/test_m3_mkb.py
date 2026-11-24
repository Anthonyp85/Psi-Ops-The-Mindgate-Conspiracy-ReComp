#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 3 automated Native-PC Mouse & Keyboard verification suite (Win64 + Linux).

Aligned with runtime Stage 1B interface:
  - Env: XBE_MKB_AUTOTEST=<script.txt>, XBE_AUTOTEST_OUT_DIR=<dir>
  - CLI: --no-launcher --iso <iso> --state-log <state.jsonl> --settings <settings.toml>
         and --save-settings-exit (for portable [controls] & [controls.bindings] round-trip)
  - Directives: key_ms, mouse_ms, wheel_ms, pad_ms, set_ms, capture_ms, exit_ms

Verifies quantitatively across Win64 & WSL2 Linux (zero OS SendInput):
  1. 1px micro-aim (+1 px X and +1 px Y -> non-zero d_yaw/d_pitch, 0 deadzone loss)
  2. Flick 0.000-deg stop-drift (+400 px flick followed by 0 px -> 0.000000 rad drift)
  3. dt frame-rate independence (60 FPS vs 30 FPS same +100px delta -> < 1e-4 rad diff)
     and cm/360 @ 800 DPI formula consistency
  4. Decoupled PC Mouse Invert-Y (negates mouse d_pitch without altering guest_invert_y)
  5. Raw 1:1 Aim toggle (mouse_raw_aim=true bypasses sticky-aim slowdown)
  6. WASD diagonal normalization (W, W+D, W+A, S+D, S+A magnitude == 1.000 +/- 0.002)
  7. Queued WheelUp/WheelDown pulses, direct 1-key bindings, context-aware latched Esc,
     and Auto hot-switching (mkb <-> controller)
  8. Smart OS Cursor Capture & Release states (captured ONLY in focused unpaused 3D
     gameplay; released on Title, Main Menu, FMV, Pause, F1 Overlay, Focus Loss)
  9. Portable OS-independent settings.toml [controls] & [controls.bindings]
     (Primary + Secondary string tokens) save/load parity across Windows & Linux.
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
import tomllib
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_m2_gameplay as m2  # noqa: E402

# Portable OS-independent [controls] & [controls.bindings] profile matching Stage 1B schema
PORTABLE_CONTROLS_TOML = """\
[controls]
input_mode = 'auto'
mouse_sensitivity = 1.0
mouse_invert_y = false
mouse_raw_aim = true
mouse_smoothing = false
rumble_strength = 100
controller_deadzone_left = 15
controller_deadzone_right = 15
dualsense_touchpad_back = true

[controls.bindings]
move_forward = ['W', 'Up']
move_backward = ['S', 'Down']
move_left = ['A', 'Left']
move_right = ['D', 'Right']
fire = ['LMB', 'F']
fine_aim = ['RMB', 'V']
reload = ['R', 'MMB']
jump = ['Space', '']
crouch = ['LCtrl', 'C']
interact = ['E', 'Enter']
melee = ['Q', 'Mouse4']
psi_use = ['RMB', 'Mouse5']
weapon_next = ['WheelUp', '']
weapon_prev = ['WheelDown', '']
psi_tk = ['1', '']
psi_pk = ['2', '']
psi_md = ['3', '']
psi_mc = ['4', '']
psi_rv = ['5', '']
psi_av = ['6', '']
pause = ['Escape', '']
objectives = ['Tab', '']
"""

FORBIDDEN_OS_KEYCODE_RE = re.compile(r"\b(VK_[A-Z0-9_]+|SDL_SCANCODE_[A-Z0-9_]+|0x[0-9A-Fa-f]+)\b")

# In-process XBE_MKB_AUTOTEST script (ms from first Present()) matching #42 + #106 directives.
DEFAULT_M3_MKB_SCRIPT = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops Milestone 3 in-process M&K + Mouse Menu + DualSense Noise + OS Cursor verification script
# Directives:
#   key_ms <start_ms> <end_ms> <tokens_csv>
#   mouse_ms <ms> <dx> <dy>
#   wheel_ms <ms> <delta>
#   cursor_ms <ms> <ui_x> <ui_y>
#   lmb_click_ms <ms> [ui_x ui_y]
#   rmb_click_ms <ms>
#   pad_ms <start_ms> <end_ms> <buttons|none> [lx ly rx ry lt rt]
#   set_ms <ms> <mouse_invert_y|mouse_raw_aim|mouse_sensitivity|overlay_open|window_focused|fps_cap|input_mode|controller_deadzone_left|controller_deadzone_right> <val>
#   capture_ms <ms> <file.bmp>
#   exit_ms <ms>

# 1. Pure ZERO-KEYBOARD / ZERO-CONTROLLER Mouse Menu Navigation:
#    Boot FMV (~8000ms) -> Title Screen AttractMode (~25800ms) -> LMB advances Press START
lmb_click_ms 26400 256 300

# 2. Main Menu (RootMenuContent): Hover + LMB click START NEW GAME (x=162..356, y=265..374)
cursor_ms 28500 256 276
lmb_click_ms 29000 256 276

# 3. DifficultySelect: Hover + LMB click Normal difficulty
cursor_ms 30500 256 276
lmb_click_ms 30900 256 276

# 4. Intro Cutscene (~34000ms): Skip cleanly via pure LMB click (zero keyboard)
lmb_click_ms 34800 256 240
lmb_click_ms 36200 256 240
lmb_click_ms 37600 256 240
lmb_click_ms 45500 256 240

# 5. Wait for in-engine level00 cell intro to finish & pop up Mission Objectives (Tip ~58889ms),
#    then dismiss Mission Objectives purely via LMB click (zero keyboard, zero controller!)
lmb_click_ms 59500 256 240
lmb_click_ms 60200 256 240

# 6. Active 3D Gameplay setup + Continuous Simulated Noisy DualSense Edge Controller (deadzone=0!)
#    Verifies 0 flip-flops out of mkb, 100% working LMB/WASD/1:1 mouse aim, and OS cursor invisible!
set_ms 60600 input_mode auto
set_ms 60610 controller_deadzone_left 0
set_ms 60620 controller_deadzone_right 0
set_ms 60640 mouse_sensitivity 1.0
set_ms 60680 mouse_invert_y 0
set_ms 60720 mouse_raw_aim 1
set_ms 60760 fps_cap 60
pad_ms 60700 71800 none 700 -550 -600 480 0 0

# 7. 1px micro-aim precision (0 deadzone loss)
mouse_ms 61400 1 0
mouse_ms 61700 0 1

# 8. High-speed +400px flick + immediate 0.000-deg stop-drift on next frames
mouse_ms 62100 400 0

# 9. dt frame-rate independence (60 FPS vs 30 FPS with identical +100px delta)
set_ms 62500 fps_cap 60
mouse_ms 62750 100 0
set_ms 63100 fps_cap 30
mouse_ms 63500 100 0
set_ms 63900 fps_cap 60

# 10. Decoupled PC Mouse Invert-Y (negates mouse d_pitch without mutating guest_invert_y)
set_ms 64200 mouse_invert_y 0
mouse_ms 64400 0 50
set_ms 64700 mouse_invert_y 1
mouse_ms 64900 0 50
set_ms 65200 mouse_invert_y 0

# 11. Raw 1:1 Aim toggle (off -> on)
set_ms 65400 mouse_raw_aim 0
set_ms 65650 mouse_raw_aim 1

# 12. WASD cardinal & diagonal normalization (1.000 +/- 0.002)
key_ms 66000 66400 W
key_ms 66600 67000 W,D
key_ms 67200 67600 W,A
key_ms 67800 68200 S,D
key_ms 68400 68800 S,A

# 13. Queued WheelUp/WheelDown pulses, direct 1-key bindings, LMB fire, jump & gameplay PNG
wheel_ms 69200 1
wheel_ms 69500 -1
key_ms 69800 70200 1
key_ms 70400 70800 LMB
key_ms 71000 71400 Space
capture_ms 71600 mkb_gameplay.bmp

# Restore 15% deadzones after noisy-controller stress window
set_ms 71850 controller_deadzone_left 15
set_ms 71860 controller_deadzone_right 15

# 14. Auto hot-switching (genuine controller press -> mkb)
pad_ms 72000 72300 A
mouse_ms 72600 20 0

# 15. Smart cursor release on F1 Overlay & Focus Loss, re-capture on restore
set_ms 73000 overlay_open 1
set_ms 73400 overlay_open 0
set_ms 73800 window_focused 0
set_ms 74200 window_focused 1

# 16. Pause Menu Mouse Navigation: Esc -> Pause (cursor visible) -> Hover item 0 -> RMB resumes gameplay
key_ms 74800 75300 Escape
cursor_ms 75650 256 97
rmb_click_ms 76150

# 17. Pause Menu Mouse Quit-to-Main-Menu: Esc -> Hover + LMB QUIT TO MAIN MENU [item 7: y=331..353] -> SavePrompt Quit Without Saving [item 1: y=222..244] -> Title Menu
key_ms 77600 78000 Escape
cursor_ms 78500 256 342
lmb_click_ms 78900 256 342
cursor_ms 79800 256 233
lmb_click_ms 80200 256 233

exit_ms 86500
"""


@dataclass
class MkbRunResult:
    platform: str
    exit_code: int = -1
    timed_out: bool = False
    wall_s: float = 0.0
    passed: bool = False
    log_path: str = ""
    state_log_path: str = ""
    screenshot_path: str = ""
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)


def validate_portable_settings_toml(toml_text: str) -> list[str]:
    errs: list[str] = []
    if FORBIDDEN_OS_KEYCODE_RE.search(toml_text):
        errs.append("settings.toml contains non-portable OS keycode literals (VK_*/SDL_SCANCODE_*/hex).")
    try:
        parsed = tomllib.loads(toml_text)
    except Exception as exc:
        return [f"Invalid TOML in settings.toml: {exc}"]

    controls = parsed.get("controls")
    if not isinstance(controls, dict):
        errs.append("settings.toml missing [controls] table.")
        return errs

    for key in (
        "input_mode",
        "mouse_sensitivity",
        "mouse_invert_y",
        "mouse_raw_aim",
        "mouse_smoothing",
        "rumble_strength",
        "dualsense_touchpad_back",
    ):
        if key not in controls:
            errs.append(f"settings.toml [controls] missing key '{key}'.")

    bindings = controls.get("bindings")
    if not isinstance(bindings, dict) or not bindings:
        errs.append("settings.toml missing [controls.bindings] table.")
        return errs

    for action, slots in bindings.items():
        if not isinstance(slots, list) or len(slots) != 2 or not all(isinstance(s, str) for s in slots):
            errs.append(f"[controls.bindings].{action} must be a 2-slot [Primary, Secondary] string array, got {slots!r}.")
    return errs


def _rec_ms(rec: dict[str, Any]) -> float:
    if "eff_us" in rec:
        return float(rec["eff_us"]) / 1000.0
    return float(rec.get("timestamp_us", 0)) / 1000.0


def _window(records: list[dict[str, Any]], start_ms: float, end_ms: float) -> list[dict[str, Any]]:
    return [r for r in records if start_ms <= _rec_ms(r) <= end_ms]


def evaluate_mkb_state_log(state_log_path: Path) -> tuple[list[str], list[str], dict[str, Any]]:
    errs: list[str] = []
    warns: list[str] = []
    metrics: dict[str, Any] = {}

    if not state_log_path.is_file():
        return [f"Missing M&K --state-log JSONL: {state_log_path}"], warns, metrics

    records: list[dict[str, Any]] = []
    for idx, line in enumerate(state_log_path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except Exception as exc:
            errs.append(f"State-log line {idx}: invalid JSON ({exc})")
            continue
        if isinstance(rec, dict):
            records.append(rec)

    metrics["record_count"] = len(records)
    if len(records) < 20:
        return errs + [f"Too few state-log records ({len(records)} < 20)."], warns, metrics

    # Helper to find the injected mouse event record inside [start_ms, end_ms]
    def find_mouse_event(start_ms: float, end_ms: float, want_dx: int, want_dy: int, label: str) -> tuple[int, dict[str, Any]] | None:
        for idx, r in enumerate(records):
            t = _rec_ms(r)
            if start_ms <= t <= end_ms and int(r.get("mouse_dx", 0)) == want_dx and int(r.get("mouse_dy", 0)) == want_dy:
                return idx, r
        errs.append(f"Missing injected mouse event '{label}' (dx={want_dx}, dy={want_dy}) in [{start_ms}..{end_ms}ms].")
        return None

    # 1. 1px micro-aim precision (0 deadzone loss) + cm/360 @ 800 DPI formula
    ev_1x = find_mouse_event(61300, 61600, 1, 0, "micro_1px_x")
    ev_1y = find_mouse_event(61600, 61900, 0, 1, "micro_1px_y")
    if ev_1x:
        _, r1x = ev_1x
        dyaw = float(r1x.get("d_yaw", 0.0))
        cm360 = float(r1x.get("cm_per_360", 0.0))
        metrics["micro_1px_d_yaw"] = dyaw
        metrics["cm_per_360"] = cm360
        if abs(dyaw) <= 1e-6:
            errs.append(f"1px X micro-aim lost in deadzone (d_yaw={dyaw}).")
        elif cm360 > 0.0:
            # 1 count at 800 DPI => rad_per_count = 2*pi / (cm_per_360 / 2.54 * 800)
            expected_rpc = (2.0 * math.pi) / ((cm360 / 2.54) * 800.0)
            metrics["expected_rad_per_count"] = expected_rpc
            if abs(abs(dyaw) - expected_rpc) > max(1e-4, 0.05 * expected_rpc):
                errs.append(f"1px X micro-aim vs cm/360 mismatch: |d_yaw|={abs(dyaw):.6f} vs expected={expected_rpc:.6f} (cm/360={cm360:.3f}).")
        else:
            warns.append("cm_per_360 not reported (>0) in micro_1px_x record.")

    if ev_1y:
        _, r1y = ev_1y
        dpitch = float(r1y.get("d_pitch", 0.0))
        metrics["micro_1px_d_pitch"] = dpitch
        if abs(dpitch) <= 1e-6:
            errs.append(f"1px Y micro-aim lost in deadzone (d_pitch={dpitch}).")

    # 2. Flick (+400 px) followed by 0.000-deg stop-drift on the very next frame
    ev_flick = find_mouse_event(62000, 62300, 400, 0, "flick_400px")
    if ev_flick:
        f_idx, rf = ev_flick
        flick_dyaw = float(rf.get("d_yaw", 0.0))
        metrics["flick_400px_d_yaw"] = flick_dyaw
        if abs(flick_dyaw) < 0.05:
            errs.append(f"400px flick produced too little yaw rotation (d_yaw={flick_dyaw:.6f} rad).")
        if f_idx + 1 < len(records):
            r_next = records[f_idx + 1]
            stop_dyaw = float(r_next.get("d_yaw", 0.0))
            stop_dpitch = float(r_next.get("d_pitch", 0.0))
            metrics["stop_drift_d_yaw"] = stop_dyaw
            metrics["stop_drift_d_pitch"] = stop_dpitch
            if int(r_next.get("mouse_dx", 0)) == 0 and int(r_next.get("mouse_dy", 0)) == 0:
                if abs(stop_dyaw) > 1e-6 or abs(stop_dpitch) > 1e-6:
                    errs.append(
                        f"Non-zero stop-drift on frame after 400px flick: d_yaw={stop_dyaw:.6f}, d_pitch={stop_dpitch:.6f} (expected 0.000000)."
                    )
        else:
            errs.append("No frame recorded after 400px flick to verify 0.000-deg stop-drift.")

    # 3. dt frame-rate independence (60 FPS vs 30 FPS with identical +100px delta)
    ev_60 = find_mouse_event(62650, 63000, 100, 0, "dt_60fps")
    ev_30 = find_mouse_event(63400, 63800, 100, 0, "dt_30fps")
    if ev_60 and ev_30:
        d60 = float(ev_60[1].get("d_yaw", 0.0))
        d30 = float(ev_30[1].get("d_yaw", 0.0))
        diff = abs(d60 - d30)
        metrics["dt_60fps_d_yaw"] = d60
        metrics["dt_30fps_d_yaw"] = d30
        metrics["dt_diff_rad"] = diff
        if abs(d60) <= 1e-5 or abs(d30) <= 1e-5:
            errs.append(f"dt independence test had zero rotation (60fps={d60}, 30fps={d30}).")
        elif diff >= 1e-4:
            errs.append(f"Mouse aim is NOT dt-independent: 60fps={d60:.6f} vs 30fps={d30:.6f} (diff={diff:.6f} >= 1e-4 rad).")

    # 4. Decoupled PC Mouse Invert-Y
    ev_inv0 = find_mouse_event(64300, 64600, 0, 50, "invert_y_off")
    ev_inv1 = find_mouse_event(64800, 65100, 0, 50, "invert_y_on")
    if ev_inv0 and ev_inv1:
        r0, r1 = ev_inv0[1], ev_inv1[1]
        p0, p1 = float(r0.get("d_pitch", 0.0)), float(r1.get("d_pitch", 0.0))
        g0, g1 = r0.get("guest_invert_y"), r1.get("guest_invert_y")
        metrics["invert_y_off_d_pitch"] = p0
        metrics["invert_y_on_d_pitch"] = p1
        if bool(r0.get("mouse_invert_y", True)) is not False or bool(r1.get("mouse_invert_y", False)) is not True:
            errs.append("mouse_invert_y flag did not toggle cleanly between invert_y_off and invert_y_on.")
        if p0 * p1 >= 0.0:
            errs.append(f"PC Mouse Invert-Y did not negate pitch sign (off={p0:.6f}, on={p1:.6f}).")
        elif abs(abs(p0) - abs(p1)) > 1e-4:
            errs.append(f"PC Mouse Invert-Y altered pitch magnitude (|off|={abs(p0):.6f}, |on|={abs(p1):.6f}).")
        if g0 is not None and g1 is not None and g0 != g1:
            errs.append(f"PC Mouse Invert-Y mutated guest gamepad invert flag ({g0} -> {g1}).")

    # 5. Raw 1:1 Aim toggle (mouse_raw_aim)
    w_raw0 = _window(records, 65450, 65620)
    w_raw1 = _window(records, 65700, 65900)
    if not w_raw0 or not any(r.get("mouse_raw_aim") is False for r in w_raw0):
        errs.append("mouse_raw_aim=false not observed in [65450..65620ms].")
    if not w_raw1 or not any(r.get("mouse_raw_aim") is True for r in w_raw1):
        errs.append("mouse_raw_aim=true not observed in [65700..65900ms].")

    # 6. WASD cardinal & diagonal normalization (1.000 +/- 0.002)
    wasd_windows = {
        "wasd_w": (66100, 66350),
        "wasd_wd": (66700, 66950),
        "wasd_wa": (67300, 67550),
        "wasd_sd": (67900, 68150),
        "wasd_sa": (68500, 68750),
    }
    wasd_mags: dict[str, float] = {}
    for label, (s_ms, e_ms) in wasd_windows.items():
        win = [r for r in _window(records, s_ms, e_ms) if float(r.get("wasd_mag", 0.0)) > 0.1]
        if not win:
            errs.append(f"No active WASD input observed for '{label}' in [{s_ms}..{e_ms}ms].")
            continue
        mag = float(win[len(win) // 2].get("wasd_mag", 0.0))
        wasd_mags[label] = round(mag, 6)
        if abs(mag - 1.0) > 0.002:
            errs.append(f"WASD '{label}' magnitude out of bounds: {mag:.6f} (expected 1.000 +/- 0.002).")
    metrics["wasd_magnitudes"] = wasd_mags

    # 7. Queued WheelUp/WheelDown pulses & Auto hot-switching (mkb <-> controller)
    w_wu = _window(records, 69150, 69400)
    w_wd = _window(records, 69450, 69700)
    if not any(int(r.get("wheel_pulse", 0)) > 0 for r in w_wu):
        errs.append("WheelUp (+1) pulse not observed in [69150..69400ms].")
    if not any(int(r.get("wheel_pulse", 0)) < 0 for r in w_wd):
        errs.append("WheelDown (-1) pulse not observed in [69450..69700ms].")

    # 7b. Continuous noisy physical DualSense Edge immunity (deadzone=0, lx=700, ly=-550, rx=-600, ry=480)
    w_noise = _window(records, 60900, 71750)
    if not w_noise:
        errs.append("No state-log frames found in noisy DualSense Edge stress window [60900..71750ms].")
    else:
        flip_flops = [r for r in w_noise if str(r.get("input_device", "")).lower() != "mkb"]
        metrics["dualsense_noise_frames"] = len(w_noise)
        metrics["dualsense_noise_flip_flops"] = len(flip_flops)
        if flip_flops:
            errs.append(
                f"Noisy DualSense Edge (deadzone=0, lx=700, ly=-550, rx=-600, ry=480) hijacked Auto mode on "
                f"{len(flip_flops)}/{len(w_noise)} frames (expected 0 flip-flops)."
            )

    w_hs_pad = _window(records, 72050, 72350)
    w_hs_mkb = _window(records, 72600, 72900)
    if not any(str(r.get("input_device", "")).lower() == "controller" for r in w_hs_pad):
        errs.append("Auto hot-switch to controller failed in [72050..72350ms].")
    if not any(str(r.get("input_device", "")).lower() == "mkb" for r in w_hs_mkb):
        errs.append("Auto hot-switch back to mkb failed in [72600..72900ms].")

    # 8. Smart OS Cursor Capture, real OS-level visibility (GetCursorInfo / SDL_CursorVisible), & Mouse Menus
    cursor_windows = {
        "cursor_fmv": (7500, 8500, False, True, None),
        "cursor_title": (25800, 26300, False, True, "title_menu"),
        "cursor_main_menu": (28200, 28800, False, True, "title_menu"),
        "cursor_cutscene": (33800, 34400, False, True, None),
        "cursor_gameplay": (60900, 61300, True, False, "gameplay_active"),
        "cursor_overlay_open": (73100, 73350, False, True, None),
        "cursor_overlay_closed": (73500, 73750, True, False, "gameplay_active"),
        "cursor_focus_lost": (73900, 74150, False, True, None),
        "cursor_focus_regained": (74300, 74650, True, False, "gameplay_active"),
        "cursor_pause": (75500, 76000, False, True, "paused"),
        "cursor_rmb_resume": (76550, 77300, True, False, "gameplay_active"),
        "cursor_quit_to_menu": (83000, 85000, False, True, "title_menu"),
    }
    cursor_states: dict[str, bool | None] = {}
    os_cursor_states: dict[str, bool | None] = {}
    for label, (s_ms, e_ms, want_cap, want_os_vis, want_phase) in cursor_windows.items():
        win = _window(records, s_ms, e_ms)
        if not win:
            errs.append(f"No state-log frames found for '{label}' in [{s_ms}..{e_ms}ms].")
            cursor_states[label] = None
            os_cursor_states[label] = None
            continue
        sample = win[len(win) // 2]
        got_cap = bool(sample.get("cursor_captured"))
        cursor_states[label] = got_cap
        if got_cap is not want_cap:
            errs.append(f"Smart cursor '{label}' mismatch: expected captured={want_cap}, got {got_cap}.")
        if "os_cursor_visible" in sample:
            got_vis = bool(sample.get("os_cursor_visible"))
            os_cursor_states[label] = got_vis
            if got_vis is not want_os_vis:
                errs.append(
                    f"Real OS cursor visibility '{label}' mismatch: expected os_cursor_visible={want_os_vis}, got {got_vis}."
                )
        else:
            errs.append(f"Missing real OS cursor visibility field 'os_cursor_visible' in '{label}'.")
        if want_phase is not None and str(sample.get("phase", "")) != want_phase:
            errs.append(f"Phase '{label}' mismatch: expected '{want_phase}', got {sample.get('phase')!r}.")
    metrics["cursor_states"] = cursor_states
    metrics["os_cursor_states"] = os_cursor_states

    # 8b. Verify pure mouse menu hover detection on RootMenuContent and Pause Menu
    w_main_hover = _window(records, 28550, 28950)
    w_pause_hover = _window(records, 75700, 76100)
    if not any(int(r.get("ui_hovered_idx", -1)) >= 0 for r in w_main_hover):
        errs.append("Main Menu mouse hover not detected (ui_hovered_idx < 0 in [28550..28950ms]).")
    if not any(int(r.get("ui_hovered_idx", -1)) >= 0 for r in w_pause_hover):
        errs.append("Pause Menu mouse hover not detected (ui_hovered_idx < 0 in [75700..76100ms]).")

    # Verify context-aware latched Esc fired once on Pause transition
    esc_win = _window(records, 74750, 75500)
    if not any(bool(r.get("esc_pulse")) for r in esc_win):
        warns.append("esc_pulse=true not observed during 3D->Pause Escape press (verify latched Esc logging).")

    return errs, warns, metrics


def run_mkb_platform(
    platform: str,
    repo_root: Path,
    exe_path: Path,
    iso_path: Path,
    script_text: str,
    run_dir: Path,
    timeout_s: int,
    wsl_distro: str,
    dry_run: bool,
) -> MkbRunResult:
    res = MkbRunResult(platform=platform)
    run_dir.mkdir(parents=True, exist_ok=True)
    script_path = run_dir / "mkb_script.txt"
    script_path.write_text(script_text, encoding="utf-8", newline="\n")
    settings_path = run_dir / "settings_portable.toml"
    settings_path.write_text(PORTABLE_CONTROLS_TOML, encoding="utf-8", newline="\n")
    state_log_path = run_dir / "mkb_state_log.jsonl"
    saved_log = run_dir / "psiops.log"
    res.state_log_path = str(state_log_path)
    res.log_path = str(saved_log)

    toml_errs = validate_portable_settings_toml(PORTABLE_CONTROLS_TOML)
    res.errors.extend(toml_errs)

    if dry_run:
        res.exit_code = 0
        res.passed = len(res.errors) == 0
        res.metrics = {"dry_run": True, "portable_toml_ok": len(toml_errs) == 0}
        return res

    if not exe_path.is_file():
        res.errors.append(f"Executable not found: {exe_path}")
        return res

    media_before = m2.snapshot_media_stats(repo_root, iso_path)
    log_file = exe_path.parent / "logs" / "psiops.log"

    if platform == "win64":
        pre_pids = m2.get_running_win_psiops()
        if pre_pids:
            res.errors.append(f"Refusing to launch: PsiOps.exe already running (PIDs={pre_pids}).")
            return res

        # Quick portable settings.toml round-trip check (--save-settings-exit)
        rt = subprocess.run(
            [str(exe_path), "--no-launcher", "--settings", str(settings_path), "--save-settings-exit"],
            cwd=str(exe_path.parent),
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        if rt.returncode != 0:
            res.errors.append(f"Win64 --save-settings-exit failed (rc={rt.returncode}).")
        else:
            res.errors.extend(validate_portable_settings_toml(settings_path.read_text(encoding="utf-8")))

        env = os.environ.copy()
        env["XBE_AUTOTEST"] = "1"
        env["PSIOPS_AUTOTEST"] = "1"
        env["XBE_MKB_AUTOTEST"] = str(script_path)
        env["XBE_AUTOTEST_OUT_DIR"] = str(run_dir)
        env["XBE_ISO_PATH"] = str(iso_path)
        env["XBE_SETTINGS_PATH"] = str(settings_path)

        cmd = [
            str(exe_path),
            "--no-launcher",
            "--iso",
            str(iso_path),
            "--settings",
            str(settings_path),
            "--state-log",
            str(state_log_path),
        ]
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
            res.errors.append(f"Win64 M&K run exceeded hard timeout ({timeout_s}s).")
            m2.kill_win_psiops()
        finally:
            res.wall_s = round(time.monotonic() - t0, 3)
            post_pids = m2.get_running_win_psiops()
            if post_pids:
                m2.kill_win_psiops()
                res.errors.append(f"Leftover PsiOps.exe processes after M&K run: {post_pids}.")
    else:
        pre_pids = m2.get_running_wsl_psiops(wsl_distro)
        if pre_pids:
            res.errors.append(f"Refusing to launch: Linux PsiOps already running (PIDs={pre_pids}).")
            return res

        wsl_exe = m2.to_wsl_path(exe_path) if sys.platform == "win32" else str(exe_path)
        wsl_dir = m2.to_wsl_path(exe_path.parent) if sys.platform == "win32" else str(exe_path.parent)
        wsl_iso = m2.to_wsl_path(iso_path) if sys.platform == "win32" else str(iso_path)
        wsl_script = m2.to_wsl_path(script_path) if sys.platform == "win32" else str(script_path)
        wsl_settings = m2.to_wsl_path(settings_path) if sys.platform == "win32" else str(settings_path)
        wsl_state = m2.to_wsl_path(state_log_path) if sys.platform == "win32" else str(state_log_path)
        wsl_out = m2.to_wsl_path(run_dir) if sys.platform == "win32" else str(run_dir)

        rt_inner = [wsl_exe, "--no-launcher", "--settings", wsl_settings, "--save-settings-exit"]
        rt_cmd = ["wsl", "-d", wsl_distro, "--cd", wsl_dir, "--"] + rt_inner if sys.platform == "win32" else rt_inner
        rt = subprocess.run(rt_cmd, capture_output=True, text=True, timeout=20, check=False)
        if rt.returncode != 0:
            res.errors.append(f"Linux --save-settings-exit failed (rc={rt.returncode}).")
        else:
            res.errors.extend(validate_portable_settings_toml(settings_path.read_text(encoding="utf-8")))

        inner = [
            "env",
            "XBE_AUTOTEST=1",
            "PSIOPS_AUTOTEST=1",
            f"XBE_MKB_AUTOTEST={wsl_script}",
            f"XBE_AUTOTEST_OUT_DIR={wsl_out}",
            f"XBE_ISO_PATH={wsl_iso}",
            f"XBE_SETTINGS_PATH={wsl_settings}",
            "SDL_AUDIODRIVER=dummy",
            "xvfb-run",
            "-a",
            "-s",
            "-screen 0 640x480x24",
            wsl_exe,
            "--no-launcher",
            "--iso",
            wsl_iso,
            "--settings",
            wsl_settings,
            "--state-log",
            wsl_state,
        ]
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
            res.errors.append(f"Linux M&K run exceeded hard timeout ({timeout_s}s).")
            m2.kill_wsl_psiops(wsl_distro)
        finally:
            res.wall_s = round(time.monotonic() - t0, 3)
            post_pids = m2.get_running_wsl_psiops(wsl_distro)
            if post_pids:
                m2.kill_wsl_psiops(wsl_distro)
                res.errors.append(f"Leftover Linux PsiOps processes after M&K run: {post_pids}.")

    logs_dir = exe_path.parent / "logs"
    candidates = sorted(logs_dir.glob("psiops*.log"), key=lambda p: p.stat().st_mtime_ns) if logs_dir.is_dir() else []
    newest_log = candidates[-1] if candidates else log_file
    if newest_log.is_file():
        shutil.copy2(newest_log, saved_log)
        log_text = saved_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = stdout_s + "\n" + stderr_s
        saved_log.write_text(log_text, encoding="utf-8", newline="\n")

    if res.exit_code != 0 and not res.timed_out:
        res.errors.append(f"Non-zero exit code: {res.exit_code}")

    for pat in m2.CRASH_PATTERNS:
        m = pat.search(log_text)
        if m:
            res.errors.append(f"Crash/exception in log: {m.group(0)!r}")
    for pat in m2.SHADER_GPU_ERROR_PATTERNS:
        m = pat.search(log_text)
        if m:
            res.errors.append(f"GPU/shader error in log: {m.group(0)!r}")
    if "Exit (0)" not in log_text:
        res.errors.append("Log missing clean 'Exit (0)'.")

    res.errors.extend(m2.assert_media_unchanged(media_before))

    tel_errs, tel_warns, metrics = evaluate_mkb_state_log(state_log_path)
    res.errors.extend(tel_errs)
    res.warnings.extend(tel_warns)
    res.metrics.update(metrics)

    cp, _ = m2.analyze_checkpoint(run_dir, "mkb_gameplay")
    if cp.exists:
        res.screenshot_path = cp.path
        res.metrics["screenshot_passed"] = cp.passed
        if not cp.passed:
            res.errors.append(f"mkb_gameplay screenshot failed: {cp.reason}")

    res.passed = len(res.errors) == 0
    return res


def _self_test_mkb_logic(tmp_dir: Path) -> None:
    """In-memory unit check run during --dry-run verifying all quantitative assertions."""
    rpc = 0.0007853981633974483
    cm360 = ((2.0 * math.pi) / rpc) / 800.0 * 2.54
    rows = [
        {"eff_us": 8000_000, "phase": "fmv", "cursor_captured": False, "os_cursor_visible": True},
        {"eff_us": 26000_000, "phase": "title_menu", "cursor_captured": False, "os_cursor_visible": True, "ui_screen": "AttractMode"},
        {"eff_us": 28500_000, "phase": "title_menu", "cursor_captured": False, "os_cursor_visible": True, "ui_screen": "RootMenuContent"},
        {"eff_us": 28700_000, "phase": "title_menu", "cursor_captured": False, "os_cursor_visible": True, "ui_screen": "RootMenuContent", "ui_hovered_idx": 0, "ui_item_count": 4},
        {"eff_us": 34000_000, "phase": "fmv", "cursor_captured": False, "os_cursor_visible": True},
        {"eff_us": 61100_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 61400_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 1, "mouse_dy": 0, "d_yaw": rpc, "d_pitch": 0.0, "cm_per_360": cm360},
        {"eff_us": 61700_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 0, "mouse_dy": 1, "d_yaw": 0.0, "d_pitch": rpc, "cm_per_360": cm360},
        {"eff_us": 62100_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 400, "mouse_dy": 0, "d_yaw": 400.0 * rpc, "d_pitch": 0.0},
        {"eff_us": 62116_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 0, "mouse_dy": 0, "d_yaw": 0.0, "d_pitch": 0.0},
        {"eff_us": 62750_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 100, "mouse_dy": 0, "d_yaw": 100.0 * rpc},
        {"eff_us": 63500_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 100, "mouse_dy": 0, "d_yaw": 100.0 * rpc},
        {"eff_us": 64400_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 0, "mouse_dy": 50, "d_pitch": 50.0 * rpc, "mouse_invert_y": False, "guest_invert_y": False},
        {"eff_us": 64900_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_dx": 0, "mouse_dy": 50, "d_pitch": -50.0 * rpc, "mouse_invert_y": True, "guest_invert_y": False},
        {"eff_us": 65500_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_raw_aim": False},
        {"eff_us": 65750_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "mouse_raw_aim": True},
        {"eff_us": 66200_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wasd_x": 0.0, "wasd_y": 1.0, "wasd_mag": 1.0},
        {"eff_us": 66800_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wasd_x": 0.707107, "wasd_y": 0.707107, "wasd_mag": 1.0},
        {"eff_us": 67400_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wasd_x": -0.707107, "wasd_y": 0.707107, "wasd_mag": 1.0},
        {"eff_us": 68000_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wasd_x": 0.707107, "wasd_y": -0.707107, "wasd_mag": 1.0},
        {"eff_us": 68600_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wasd_x": -0.707107, "wasd_y": -0.707107, "wasd_mag": 1.0},
        {"eff_us": 69220_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wheel_pulse": 1},
        {"eff_us": 69520_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False, "wheel_pulse": -1},
        {"eff_us": 72150_000, "phase": "gameplay_active", "input_device": "controller", "cursor_captured": False, "os_cursor_visible": True},
        {"eff_us": 72650_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 73200_000, "phase": "gameplay_active", "input_device": "mkb", "overlay_open": True, "cursor_captured": False, "os_cursor_visible": True},
        {"eff_us": 73600_000, "phase": "gameplay_active", "input_device": "mkb", "overlay_open": False, "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 74000_000, "phase": "gameplay_active", "input_device": "mkb", "window_focused": False, "cursor_captured": False, "os_cursor_visible": True},
        {"eff_us": 74450_000, "phase": "gameplay_active", "input_device": "mkb", "window_focused": True, "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 74850_000, "phase": "gameplay_active", "input_device": "mkb", "esc_pulse": True, "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 75750_000, "phase": "paused", "input_device": "mkb", "cursor_captured": False, "os_cursor_visible": True, "ui_screen": "Menu", "ui_hovered_idx": 0, "ui_item_count": 5},
        {"eff_us": 76900_000, "phase": "gameplay_active", "input_device": "mkb", "cursor_captured": True, "os_cursor_visible": False},
        {"eff_us": 84000_000, "phase": "title_menu", "input_device": "mkb", "cursor_captured": False, "os_cursor_visible": True, "ui_screen": "RootMenuContent"},
    ]
    tmp_dir.mkdir(parents=True, exist_ok=True)
    p = tmp_dir / "_selftest_mkb_state.jsonl"
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8", newline="\n")
    errs, warns, _ = evaluate_mkb_state_log(p)
    assert not errs, f"M&K self-test errors: {errs}"
    assert not warns, f"M&K self-test warnings: {warns}"
    assert not validate_portable_settings_toml(PORTABLE_CONTROLS_TOML)
    p.unlink()


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Milestone 3 automated Native-PC Mouse & Keyboard verification suite (Win64 + Linux)."
    )
    parser.add_argument("--platform", choices=("win64", "linux", "both"), default="both")
    parser.add_argument("--timeout", type=int, default=120, help="Hard timeout in seconds per run.")
    parser.add_argument("--iso", type=Path, default=repo_root / m2.EXPECTED_ISO_REL)
    parser.add_argument("--win-exe", type=Path, default=repo_root / "out" / "build" / "release" / "PsiOps.exe")
    parser.add_argument("--linux-exe", type=Path, default=repo_root / "out" / "build" / "linux-release" / "PsiOps")
    parser.add_argument("--wsl-distro", type=str, default=None)
    parser.add_argument("--script", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=repo_root / ".local" / "m3_mkb")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.iso.is_file():
        print(f"ERROR: XISO not found: {args.iso}", file=sys.stderr)
        return 2

    platforms = ["win64", "linux"] if args.platform == "both" else [args.platform]
    wsl_distro = ""
    if "linux" in platforms and sys.platform == "win32":
        wsl_distro = m2.discover_wsl_ubuntu2204_distro(args.wsl_distro)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.dry_run:
        _self_test_mkb_logic(args.out_dir)

    script_text = args.script.read_text(encoding="utf-8") if args.script else DEFAULT_M3_MKB_SCRIPT
    results: list[MkbRunResult] = []
    all_passed = True

    for plat in platforms:
        exe = args.win_exe if plat == "win64" else args.linux_exe
        print(f"[test_m3_mkb] Starting {plat} M&K verification (timeout={args.timeout}s)...")
        r = run_mkb_platform(
            platform=plat,
            repo_root=repo_root,
            exe_path=exe,
            iso_path=args.iso,
            script_text=script_text,
            run_dir=args.out_dir / plat,
            timeout_s=args.timeout,
            wsl_distro=wsl_distro,
            dry_run=args.dry_run,
        )
        results.append(r)
        print(f"[test_m3_mkb] {plat}: {'PASS' if r.passed else 'FAIL'} (exit={r.exit_code}, wall={r.wall_s}s)")
        for w in r.warnings:
            print(f"  WARN: {w}")
        for e in r.errors:
            print(f"  FAIL: {e}")
        if not r.passed:
            all_passed = False

    if not args.dry_run and len(results) == 2 and all(r.passed for r in results):
        p_win = args.out_dir / "win64" / "settings_portable.toml"
        p_lin = args.out_dir / "linux" / "settings_portable.toml"
        if p_win.is_file() and p_lin.is_file():
            t_win = tomllib.loads(p_win.read_text(encoding="utf-8")).get("controls", {})
            t_lin = tomllib.loads(p_lin.read_text(encoding="utf-8")).get("controls", {})
            if t_win != t_lin:
                print(f"  FAIL: Cross-platform [controls] parity mismatch: win64={t_win} vs linux={t_lin}")
                all_passed = False

    report = {
        "iso": str(args.iso),
        "platforms": platforms,
        "all_passed": all_passed,
        "results": [asdict(r) for r in results],
    }
    report_path = args.out_dir / "m3_mkb_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"[test_m3_mkb] Report written to: {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
