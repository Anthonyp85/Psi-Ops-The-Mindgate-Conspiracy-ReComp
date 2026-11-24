// SPDX-License-Identifier: GPL-2.0-or-later
// Host-boundary controller .rec recording, deterministic replay, and --state-log JSONL emitter.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "core/input.h"
#include "core/perf.h"

namespace input_rec {

enum class ReplayMode : uint8_t {
    Hybrid = 0,
    Frame = 1,
    Time = 2,
};

struct GuestStateSnapshot {
    const char* phase = "title_menu";
    uint32_t state = 0;
    int in_shell = 1;
    int fmv_active = 0;
    double sim_elapsed_s = 0.0;
    uint32_t g_frame = 0;

    bool has_player_pos = false;
    float player_x = 0.0f;
    float player_y = 0.0f;
    float player_z = 0.0f;

    bool has_camera = false;
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    uint32_t cam_mode = 0;

    bool has_vitals = false;
    float health = 0.0f;
    float psi = 0.0f;
    int32_t ammo_clip = -1;
    int32_t ammo_reserve = -1;

    bool has_guest_invert_y = false;
    bool guest_invert_y = false;
};

struct MkbStateSnapshot {
    const char* input_device = "mkb";
    bool cursor_captured = false;
    bool os_cursor_visible = true;
    bool overlay_open = false;
    bool window_focused = true;
    int32_t mouse_dx = 0;
    int32_t mouse_dy = 0;
    float d_yaw = 0.0f;
    float d_pitch = 0.0f;
    float wasd_x = 0.0f;
    float wasd_y = 0.0f;
    float wasd_mag = 0.0f;
    bool mouse_invert_y = false;
    bool mouse_raw_aim = true;
    float mouse_sens = 1.0f;
    float cm_per_360 = 0.0f;
    int32_t wheel_pulse = 0;
    bool esc_pulse = false;
    bool mkb_hook_bypassed = false;
    std::string ui_screen;
    int ui_hovered_idx = -1;
    int ui_item_count = 0;
};

using GuestStateCallback = GuestStateSnapshot (*)();
using MkbStateCallback = MkbStateSnapshot (*)();

// Parse CLI flags (--record-input, --replay-input, --replay-mode, --replay-exit, --state-log)
// and environment fallbacks; call once during app::kGame.init().
void Init();

// Flush .rec and --state-log buffers cleanly on shutdown.
void Shutdown();

// Register optional game-specific guest & M&K telemetry callbacks for --state-log checkpoints.
void SetGuestStateCallback(GuestStateCallback cb);
void SetMkbStateCallback(MkbStateCallback cb);

// Zero-overhead mode queries.
bool IsRecordActive();
bool IsReplayActive();
bool IsStateLogActive();
bool ShouldBypassMkb();
ReplayMode GetReplayMode();

// Called from app::kGame.pad(port, pad):
// - In replay mode: overwrites pad with deterministic replayed sample and returns true.
// - In record mode: observes pad state for the current frame and returns false.
bool OnPadPoll(int port, input::Pad& pad);

// Called from app::kGame.rumble(port, left, right): records latest guest rumble motor state.
void OnRumble(int port, uint16_t left, uint16_t right);

// Called once per Present() via input::SetPresentCallback.
void OnPresent(uint64_t present_frame, std::filesystem::path& out_capture_bmp, bool& out_should_exit);

// Monotonic present timing helpers shared with M&K telemetry.
uint64_t CurrentPresentFrame();
uint64_t ElapsedWallUs();
uint64_t ElapsedEffUs();

}  // namespace input_rec
