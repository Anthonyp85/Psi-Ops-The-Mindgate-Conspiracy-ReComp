// SPDX-License-Identifier: GPL-2.0-or-later
// Cross-platform Raw Mouse & Keyboard host layer, Smart OS Cursor Capture,
// portable OS-independent [controls] / [controls.bindings] persistence,
// 4-column Click-to-Rebind ImGui helper, and in-process XBE_MKB_AUTOTEST driver.

#include "common/mkb_input.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/ui_aspect.h"
#include "core/log.h"

namespace mkb {

namespace {

constexpr float kPi = 3.14159265358979323846f;
// Baseline 0.00080 rad/count => ~24.94 cm/360 @ 800 DPI at sensitivity 1.0 (1px = 0.046 deg, 400px flick = 18.3 deg).
constexpr float kBaseRadPerCount = 0.00080f;

struct KeyMeta {
    KeyCode code;
    const char* token;
};

constexpr KeyMeta kKeyTable[] = {
    {KeyCode::None, ""},
    {KeyCode::A, "A"}, {KeyCode::B, "B"}, {KeyCode::C, "C"}, {KeyCode::D, "D"}, {KeyCode::E, "E"},
    {KeyCode::F, "F"}, {KeyCode::G, "G"}, {KeyCode::H, "H"}, {KeyCode::I, "I"}, {KeyCode::J, "J"},
    {KeyCode::K, "K"}, {KeyCode::L, "L"}, {KeyCode::M, "M"}, {KeyCode::N, "N"}, {KeyCode::O, "O"},
    {KeyCode::P, "P"}, {KeyCode::Q, "Q"}, {KeyCode::R, "R"}, {KeyCode::S, "S"}, {KeyCode::T, "T"},
    {KeyCode::U, "U"}, {KeyCode::V, "V"}, {KeyCode::W, "W"}, {KeyCode::X, "X"}, {KeyCode::Y, "Y"},
    {KeyCode::Z, "Z"},
    {KeyCode::Num0, "0"}, {KeyCode::Num1, "1"}, {KeyCode::Num2, "2"}, {KeyCode::Num3, "3"}, {KeyCode::Num4, "4"},
    {KeyCode::Num5, "5"}, {KeyCode::Num6, "6"}, {KeyCode::Num7, "7"}, {KeyCode::Num8, "8"}, {KeyCode::Num9, "9"},
    {KeyCode::Space, "Space"}, {KeyCode::Enter, "Enter"}, {KeyCode::Escape, "Escape"},
    {KeyCode::Tab, "Tab"}, {KeyCode::Backspace, "Backspace"},
    {KeyCode::LShift, "LShift"}, {KeyCode::RShift, "RShift"},
    {KeyCode::LCtrl, "LCtrl"}, {KeyCode::RCtrl, "RCtrl"},
    {KeyCode::LAlt, "LAlt"}, {KeyCode::RAlt, "RAlt"},
    {KeyCode::Up, "Up"}, {KeyCode::Down, "Down"}, {KeyCode::Left, "Left"}, {KeyCode::Right, "Right"},
    {KeyCode::F1, "F1"}, {KeyCode::F2, "F2"}, {KeyCode::F3, "F3"}, {KeyCode::F4, "F4"},
    {KeyCode::F5, "F5"}, {KeyCode::F6, "F6"}, {KeyCode::F7, "F7"}, {KeyCode::F8, "F8"},
    {KeyCode::F9, "F9"}, {KeyCode::F10, "F10"}, {KeyCode::F11, "F11"}, {KeyCode::F12, "F12"},
    {KeyCode::Minus, "Minus"}, {KeyCode::Equals, "Equals"},
    {KeyCode::LBracket, "LBracket"}, {KeyCode::RBracket, "RBracket"}, {KeyCode::Backslash, "Backslash"},
    {KeyCode::Semicolon, "Semicolon"}, {KeyCode::Apostrophe, "Apostrophe"},
    {KeyCode::Comma, "Comma"}, {KeyCode::Period, "Period"}, {KeyCode::Slash, "Slash"}, {KeyCode::Grave, "Grave"},
    {KeyCode::MouseLeft, "LMB"}, {KeyCode::MouseRight, "RMB"}, {KeyCode::MouseMiddle, "MMB"},
    {KeyCode::Mouse4, "Mouse4"}, {KeyCode::Mouse5, "Mouse5"},
    {KeyCode::WheelUp, "WheelUp"}, {KeyCode::WheelDown, "WheelDown"},
};

struct ActionMeta {
    Action action;
    const char* key;
    const char* label;
    KeyCode def_primary;
    KeyCode def_secondary;
};

constexpr ActionMeta kActionTable[] = {
    {Action::MoveForward, "move_forward", "Move Forward", KeyCode::W, KeyCode::Up},
    {Action::MoveBackward, "move_backward", "Move Backward", KeyCode::S, KeyCode::Down},
    {Action::MoveLeft, "move_left", "Strafe Left", KeyCode::A, KeyCode::Left},
    {Action::MoveRight, "move_right", "Strafe Right", KeyCode::D, KeyCode::Right},
    {Action::Fire, "fire", "Fire Weapon", KeyCode::MouseLeft, KeyCode::F},
    {Action::FineAim, "fine_aim", "Sniper Scope / Lock-On", KeyCode::MouseRight, KeyCode::V},
    {Action::Reload, "reload", "Reload / MC Secondary", KeyCode::R, KeyCode::MouseMiddle},
    {Action::Jump, "jump", "Jump / Mantle", KeyCode::Space, KeyCode::None},
    {Action::Crouch, "crouch", "Crouch / Sneak", KeyCode::LCtrl, KeyCode::C},
    {Action::WallMove, "wall_move", "Wall Cover / Peek", KeyCode::LAlt, KeyCode::None},
    {Action::Interact, "interact", "Use / Interact / Pickup", KeyCode::E, KeyCode::Enter},
    {Action::Melee, "melee", "Melee Attack", KeyCode::Q, KeyCode::Mouse4},
    {Action::PsiUse, "psi_use", "Use Active Psi Power", KeyCode::LShift, KeyCode::Mouse5},
    {Action::WeaponNext, "weapon_next", "Next Weapon / Zoom In", KeyCode::WheelUp, KeyCode::None},
    {Action::WeaponPrev, "weapon_prev", "Prev Weapon / Zoom Out", KeyCode::WheelDown, KeyCode::None},
    {Action::PsiTk, "psi_tk", "Psi: Telekinesis (TK)", KeyCode::Num1, KeyCode::None},
    {Action::PsiPk, "psi_pk", "Psi: Pyrokinesis (PK)", KeyCode::Num2, KeyCode::None},
    {Action::PsiMd, "psi_md", "Psi: Mind Drain (MD)", KeyCode::Num3, KeyCode::None},
    {Action::PsiMc, "psi_mc", "Psi: Mind Control (MC)", KeyCode::Num4, KeyCode::None},
    {Action::PsiRv, "psi_rv", "Psi: Remote View (RV)", KeyCode::Num5, KeyCode::None},
    {Action::PsiAv, "psi_av", "Psi: Aura View (AV)", KeyCode::Num6, KeyCode::None},
    {Action::TkCamControl, "tk_cam_control", "TK Camera Control", KeyCode::G, KeyCode::None},
    {Action::McExplode, "mc_explode", "MC Head Pop (Hold)", KeyCode::X, KeyCode::None},
    {Action::Pause, "pause", "Pause / Back", KeyCode::Escape, KeyCode::None},
    {Action::Objectives, "objectives", "Mission Objectives", KeyCode::Tab, KeyCode::None},
};
static_assert(sizeof(kActionTable) / sizeof(kActionTable[0]) == size_t(Action::Count), "ActionTable mismatch");

std::string UpperAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string LowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void ResetDefaultBindings(Config& cfg) {
    for (const auto& m : kActionTable) {
        cfg.bindings[size_t(m.action)][0] = m.def_primary;
        cfg.bindings[size_t(m.action)][1] = m.def_secondary;
    }
}

struct MkbKeyEvent {
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    std::vector<KeyCode> keys;
};

struct MkbMouseEvent {
    uint64_t target_ms = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    bool triggered = false;
};

struct MkbWheelEvent {
    uint64_t target_ms = 0;
    int32_t delta = 0;
    bool triggered = false;
};

struct MkbPadEvent {
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool has_sticks = false;
};

struct MkbSetEvent {
    uint64_t target_ms = 0;
    std::string key;
    std::string value;
    bool triggered = false;
};

struct MkbCapEvent {
    uint64_t target_ms = 0;
    std::filesystem::path path;
    bool triggered = false;
};

void ApplyPadToken(const std::string& tok, MkbPadEvent& ev) {
    std::string u = UpperAscii(tok);
    if (u.empty() || u == "NONE" || u == "0") return;
    if (u == "UP" || u == "DPAD_UP") ev.buttons |= 1u << 0;
    else if (u == "DOWN" || u == "DPAD_DOWN") ev.buttons |= 1u << 1;
    else if (u == "LEFT" || u == "DPAD_LEFT") ev.buttons |= 1u << 2;
    else if (u == "RIGHT" || u == "DPAD_RIGHT") ev.buttons |= 1u << 3;
    else if (u == "START") ev.buttons |= 1u << 4;
    else if (u == "BACK") ev.buttons |= 1u << 5;
    else if (u == "LTHUMB" || u == "LS" || u == "L3") ev.buttons |= 1u << 6;
    else if (u == "RTHUMB" || u == "RS" || u == "R3") ev.buttons |= 1u << 7;
    else if (u == "A") ev.analog[0] = 0xFF;
    else if (u == "B") ev.analog[1] = 0xFF;
    else if (u == "X") ev.analog[2] = 0xFF;
    else if (u == "Y") ev.analog[3] = 0xFF;
    else if (u == "BLACK" || u == "RB") ev.analog[4] = 0xFF;
    else if (u == "WHITE" || u == "LB") ev.analog[5] = 0xFF;
    else if (u == "LT" || u == "LTRIGGER") ev.analog[6] = 0xFF;
    else if (u == "RT" || u == "RTRIGGER") ev.analog[7] = 0xFF;
}

#ifdef _WIN32
constexpr uint32_t kWinMsgSyncCursor = WM_APP + 3;
#else
constexpr int32_t kSdlEvSyncCursor = 0x58424503;
#endif

struct MkbCursorEvent {
    uint64_t target_ms = 0;
    float ui_x = 320.0f;
    float ui_y = 240.0f;
    float client_x = 0.0f;
    float client_y = 0.0f;
    float client_w = 0.0f;
    float client_h = 0.0f;
    bool is_client_coords = false;
    bool click_lmb = false;
    bool click_rmb = false;
    bool has_pos = true;
    bool triggered = false;
};

struct MkbState {
    std::once_flag defaults_once;
    std::mutex lock;
    Config cfg;

    void* window_handle = nullptr;
    std::atomic<bool> window_focused{true};
    std::atomic<bool> in_size_move{false};
    std::atomic<bool> overlay_open{false};
    std::atomic<bool> cursor_captured{false};
    std::atomic<bool> cursor_should_hide{false};
    std::atomic<bool> os_cursor_locked{false};
    std::atomic<bool> os_cursor_hidden{false};
    std::atomic<bool> os_cursor_visible{true};
    std::atomic<bool> cursor_tick_queued{false};
    std::atomic<bool> last_raw_was_absolute{false};
    std::atomic<int> imgui_overlay_synced{-1};
    bool cursor_sync_initialized = false;
    bool last_requested_capture = false;
    bool last_requested_hide = false;
    std::atomic<ActiveDevice> active_device{ActiveDevice::Mkb};
    std::atomic<perf::Phase> cur_phase{perf::Phase::TitleMenu};
    std::atomic<bool> cur_modal_ui{false};
    std::atomic<bool> cur_non_pointer_modal{false};
    bool non_pointer_lmb_pending = false;
    std::atomic<bool> non_pointer_ui_confirm_pending{false};

    // Real OS key/button state
    std::atomic<bool> key_down[size_t(KeyCode::Count)] = {};
    // Latched mouse buttons (bits 0..4 for MouseLeft..Mouse5) pressed outside active 3D gameplay
    // or held across a menu/modal/overlay -> 3D gameplay transition until physical button-up.
    std::atomic<uint8_t> suppressed_mouse_mask{0};
    bool prev_gameplay_mouse_allowed = false;

    // Physical controller previous state for edge-triggered Auto hot-switch immunity
    uint16_t prev_phys_buttons = 0;
    uint8_t prev_phys_analog[8] = {};

    // Raw mouse count accumulators (written by WM_INPUT / SDL_EVENT_MOUSE_MOTION / XBE_MKB_AUTOTEST)
    std::atomic<int64_t> raw_dx{0};
    std::atomic<int64_t> raw_dy{0};
    bool prev_abs_valid = false;
    int32_t prev_abs_x = 0;
    int32_t prev_abs_y = 0;
    // Optional 1-frame smoothing carry (only non-zero if mouse_smoothing == true)
    float smooth_dx = 0.0f;
    float smooth_dy = 0.0f;

    // Menu cursor state in Psi-Ops 512x465 (y_off=+10) virtual UI space
    float ui_x = 256.0f;
    float ui_y = 222.5f;
    bool ui_pos_valid = false;
    bool ui_moved_pending = false;
    bool ui_lmb_down = false;
    bool ui_lmb_click_pending = false;
    bool ui_rmb_click_pending = false;
    int32_t ui_wheel_pending = 0;
    // Latched pad pulses produced by mouse menu interactions
    int menu_confirm_hold = 0;
    int menu_cancel_hold = 0;
    int menu_start_hold = 0;
    int menu_nav_y_dir = 0;
    int menu_nav_y_hold = 0;
    int menu_nav_x_dir = 0;
    int menu_nav_x_hold = 0;
    // Menu UI telemetry for --state-log
    std::string ui_screen_name;
    int ui_hovered_idx = -1;
    int ui_item_count = 0;

    // Queued wheel pulses (+1 / -1) and context-aware latched Esc edge
    std::deque<int32_t> wheel_queue;
    std::deque<int32_t> gameplay_wheel_queue;
    int32_t prev_gameplay_wheel_dir = 0;
    int32_t active_wheel_dir = 0;
    int wheel_hold_frames = 0;
    bool prev_esc_down = false;
    bool esc_edge_pending = false;
    int esc_hold_frames = 0;
    perf::Phase prev_present_phase = perf::Phase::TitleMenu;
    bool prev_psi_down[6] = {};
    std::atomic<int> pending_direct_psi{-1};
    std::atomic<int> selected_psi_idx{0};

    // Short sliding window of consumed raw mouse deltas for TK Throw flick detection (0x0D)
    struct MouseFlickSample {
        std::chrono::steady_clock::time_point tp{};
        int32_t dx = 0;
        int32_t dy = 0;
    };
    std::deque<MouseFlickSample> mouse_flick_history;

    // Click-to-rebind UI state
    int rebind_action = -1;
    int rebind_slot = -1;
    uint64_t rebind_arm_frame = 0;
    int rebind_arm_ui_frame = 0;      // ImGui frame the capture was armed on (launcher has no present frames)
    bool rebind_committed = false;    // set when the window-message path stores a binding; panel marks dirty

    // Per-frame telemetry latched for --state-log
    int32_t frame_mouse_dx = 0;
    int32_t frame_mouse_dy = 0;
    float frame_d_yaw = 0.0f;
    float frame_d_pitch = 0.0f;
    bool frame_mouse_consumed_by_hook = false;
    float frame_wasd_x = 0.0f;
    float frame_wasd_y = 0.0f;
    float frame_wasd_mag = 0.0f;
    int32_t frame_wheel_pulse = 0;
    bool frame_esc_pulse = false;
    bool frame_mkb_bypassed = false;

    std::atomic<ActionMapCallback> action_cb{nullptr};

    // In-process XBE_MKB_AUTOTEST state (zero OS SendInput)
    std::atomic<bool> autotest_active{false};
    bool autotest_clock_started = false;
    std::chrono::steady_clock::time_point autotest_t0{};
    std::atomic<uint64_t> autotest_elapsed_ms{0};
    std::vector<MkbKeyEvent> at_keys;
    std::vector<MkbMouseEvent> at_mouse;
    std::vector<MkbWheelEvent> at_wheel;
    std::vector<MkbPadEvent> at_pads;
    std::vector<MkbSetEvent> at_sets;
    std::vector<MkbCapEvent> at_caps;
    std::vector<MkbCursorEvent> at_cursors;
    uint64_t at_exit_ms = 0;
    bool at_exit_posted = false;
    bool at_key_down[size_t(KeyCode::Count)] = {};
};

MkbState& St() {
    static MkbState s;
    std::call_once(s.defaults_once, [&] { ResetDefaultBindings(s.cfg); });
    return s;
}

// Maps OS window client coordinates (cx, cy) into Psi-Ops Layer 0x17 SetOrtho2D UI space
// (binary-verified 0x002F5970: width=512.0, height=465.0, offset_x=0.0, offset_y=+10.0),
// accounting first for the centered 16:9 / 4:3 presentation viewport inside (win_w, win_h)
// (letterboxed on 16:10, pillarboxed on 21:9) and then for 4:3 UI pillarboxing (0.75 * vp_w_169)
// inside that presentation viewport whenever Layer 0x17 is 4:3 pillarboxed.
bool MapClientToUiSpace(float cx, float cy, float win_w, float win_h, float& out_x, float& out_y) {
    if (win_w < 1.0f || win_h < 1.0f) return false;
    const bool wide = (win_w * 2.0f >= win_h * 3.0f);
    const float pres_asp = wide ? (16.0f / 9.0f) : (4.0f / 3.0f);
    float vp_w = std::min(win_w, win_h * pres_asp);
    const float vp_h = vp_w / pres_asp;
    float vp_x = 0.5f * (win_w - vp_w);
    const float vp_y = 0.5f * (win_h - vp_h);
    if (wide && psiops::ui_aspect::IsCurrentMenuPillarboxed43()) {
        const float box_w = vp_w * 0.75f;  // Centered 4:3 sub-box (vp_h * 4/3) inside the 16:9 viewport
        vp_x += 0.5f * (vp_w - box_w);
        vp_w = box_w;
    }
    if (vp_w < 1.0f || vp_h < 1.0f) return false;
    const float u = (cx - vp_x) / vp_w;
    const float v = (cy - vp_y) / vp_h;
    out_x = u * 512.0f;
    out_y = v * 465.0f - 10.0f;
    return true;
}

void SyncImGuiMouseFlags(bool overlay_open, bool force = false) {
    // Lock ordering rule: NEVER call while holding MkbState::lock (UiLock -> MkbState::lock).
    MkbState& st = St();
    const int want = overlay_open ? 1 : 0;
    if (!force && st.imgui_overlay_synced.load(std::memory_order_acquire) == want) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ui_lk(app::UiLock());
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = false;
        if (overlay_open) {
            io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse);
        } else {
            const bool was_enabled = (io.ConfigFlags & ImGuiConfigFlags_NoMouse) == 0;
            io.ConfigFlags |= (ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse);
            if (was_enabled) {
                io.ClearInputMouse();
            }
        }
        st.imgui_overlay_synced.store(want, std::memory_order_release);
    }
}

bool IsInteractiveMenuPhase(perf::Phase phase, bool modal_ui_open, bool non_pointer_modal) {
    if (phase == perf::Phase::TitleMenu) return true;
    if (modal_ui_open) return true;
    if (phase == perf::Phase::Paused && !non_pointer_modal) return true;
    return false;
}

bool ComputeWantHideCursor(const MkbState& st) {
    if (input_rec::ShouldBypassMkb()) return false;
    if (!st.window_focused.load(std::memory_order_relaxed)) return false;
    if (st.in_size_move.load(std::memory_order_relaxed)) return false;
    if (st.overlay_open.load(std::memory_order_relaxed)) return false;
    if (IsInteractiveMenuPhase(st.cur_phase.load(std::memory_order_relaxed),
                               st.cur_modal_ui.load(std::memory_order_relaxed),
                               st.cur_non_pointer_modal.load(std::memory_order_relaxed))) {
        return false;
    }
    return true;
}

bool ComputeWantCursorCapture(const MkbState& st) {
    if (!ComputeWantHideCursor(st)) return false;
    if (st.cfg.input_mode == InputMode::Controller) return false;
    return true;
}

constexpr uint8_t MouseButtonMaskBit(KeyCode kc) {
    switch (kc) {
        case KeyCode::MouseLeft: return uint8_t(1u << 0);
        case KeyCode::MouseRight: return uint8_t(1u << 1);
        case KeyCode::MouseMiddle: return uint8_t(1u << 2);
        case KeyCode::Mouse4: return uint8_t(1u << 3);
        case KeyCode::Mouse5: return uint8_t(1u << 4);
        default: return 0u;
    }
}

bool IsGameplayMouseButtonsAllowed(const MkbState& st) {
    if (st.overlay_open.load(std::memory_order_relaxed)) return false;
    if (st.cur_modal_ui.load(std::memory_order_relaxed)) return false;
    if (st.cur_non_pointer_modal.load(std::memory_order_relaxed)) return false;
    return st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive;
}

void NoteMouseButtonEdge(MkbState& st, KeyCode kc, bool down) {
    const uint8_t bit = MouseButtonMaskBit(kc);
    if (bit == 0u) return;
    if (down) {
        if (!IsGameplayMouseButtonsAllowed(st)) {
            st.suppressed_mouse_mask.fetch_or(bit, std::memory_order_acq_rel);
        }
    } else {
        st.suppressed_mouse_mask.fetch_and(uint8_t(~bit), std::memory_order_acq_rel);
    }
}

void LatchHeldMouseButtonsForTransition(MkbState& st) {
    constexpr KeyCode kMouseBtns[5] = {
        KeyCode::MouseLeft, KeyCode::MouseRight, KeyCode::MouseMiddle, KeyCode::Mouse4, KeyCode::Mouse5,
    };
    uint8_t mask = 0u;
    for (size_t i = 0; i < 5; ++i) {
        if (st.key_down[size_t(kMouseBtns[i])].load(std::memory_order_relaxed)) {
            mask |= uint8_t(1u << i);
        }
    }
    if (mask != 0u) {
        st.suppressed_mouse_mask.fetch_or(mask, std::memory_order_acq_rel);
    }
    st.raw_dx.store(0, std::memory_order_relaxed);
    st.raw_dy.store(0, std::memory_order_relaxed);
    st.smooth_dx = 0.0f;
    st.smooth_dy = 0.0f;
}

#ifdef _WIN32
// Persistent 32x32 100%-transparent BGRA + 1bpp AND-mask HCURSOR.
// Avoids Windows 11 black cursor sprites and IddCx / SudoMaker Virtual Display Adapter
// fallback black blocks triggered by SetCursor(nullptr).
HCURSOR CreateTransparentBlankCursor() {
    constexpr int kW = 32;
    constexpr int kH = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = kW;
    bi.bV5Height = -kH;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000u;
    bi.bV5GreenMask = 0x0000FF00u;
    bi.bV5BlueMask = 0x000000FFu;
    bi.bV5AlphaMask = 0xFF000000u;

    void* color_bits = nullptr;
    HDC screen_dc = GetDC(nullptr);
    HBITMAP hbm_color =
        CreateDIBSection(screen_dc, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, &color_bits, nullptr, 0);
    if (screen_dc) ReleaseDC(nullptr, screen_dc);

    HCURSOR hcur = nullptr;
    if (hbm_color && color_bits) {
        std::memset(color_bits, 0, size_t(kW) * size_t(kH) * 4u);
        uint8_t and_mask[128];
        std::memset(and_mask, 0xFF, sizeof(and_mask));
        HBITMAP hbm_mask = CreateBitmap(kW, kH, 1, 1, and_mask);
        if (hbm_mask) {
            ICONINFO ii{};
            ii.fIcon = FALSE;
            ii.xHotspot = kW / 2;
            ii.yHotspot = kH / 2;
            ii.hbmMask = hbm_mask;
            ii.hbmColor = hbm_color;
            hcur = CreateIconIndirect(&ii);
            DeleteObject(hbm_mask);
        }
        DeleteObject(hbm_color);
    } else if (hbm_color) {
        DeleteObject(hbm_color);
    }

    if (!hcur) {
        uint8_t and_bits[128];
        uint8_t xor_bits[128];
        std::memset(and_bits, 0xFF, sizeof(and_bits));
        std::memset(xor_bits, 0x00, sizeof(xor_bits));
        hcur = CreateCursor(GetModuleHandleW(nullptr), kW / 2, kH / 2, kW, kH, and_bits, xor_bits);
    }
    return hcur;
}

HCURSOR GetBlankCursorWin32() {
    static HCURSOR h_blank = CreateTransparentBlankCursor();
    return h_blank;
}

HCURSOR GetArrowCursorWin32() {
    static HCURSOR h_arrow = LoadCursorW(nullptr, IDC_ARROW);
    return h_arrow;
}

bool ComputeClientLockRectWin32(HWND hwnd, RECT& out_rect) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) return false;
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
    RECT clip{tl.x, tl.y, br.x, br.y};
    if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            clip.left = std::max(clip.left, mi.rcMonitor.left);
            clip.top = std::max(clip.top, mi.rcMonitor.top);
            clip.right = std::min(clip.right, mi.rcMonitor.right);
            clip.bottom = std::min(clip.bottom, mi.rcMonitor.bottom);
        }
    }
    if ((clip.right - clip.left) > 8 && (clip.bottom - clip.top) > 8) {
        clip.left += 2;
        clip.top += 2;
        clip.right -= 2;
        clip.bottom -= 2;
    }
    if (clip.right <= clip.left || clip.bottom <= clip.top) return false;
    out_rect = clip;
    return true;
}

void ReassertWin32ClipAndWarpOnGuiThread(MkbState& st, HWND hwnd, bool was_locked) {
    if (st.autotest_active.load(std::memory_order_relaxed)) return;
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    RECT lock_rect{};
    if (!ComputeClientLockRectWin32(hwnd, lock_rect)) return;
    RECT cur_clip{};
    const bool clip_matches =
        GetClipCursor(&cur_clip) && cur_clip.left == lock_rect.left && cur_clip.top == lock_rect.top &&
        cur_clip.right == lock_rect.right && cur_clip.bottom == lock_rect.bottom;
    if (!clip_matches) {
        ClipCursor(&lock_rect);
    }
    if (!st.last_raw_was_absolute.load(std::memory_order_relaxed)) {
        const LONG cx = (lock_rect.left + lock_rect.right) / 2;
        const LONG cy = (lock_rect.top + lock_rect.bottom) / 2;
        const LONG half_w = std::max<LONG>(1, (lock_rect.right - lock_rect.left) / 2);
        const LONG half_h = std::max<LONG>(1, (lock_rect.bottom - lock_rect.top) / 2);
        POINT pt{};
        if (!was_locked || !GetCursorPos(&pt) || std::abs(pt.x - cx) * 4 > half_w ||
            std::abs(pt.y - cy) * 4 > half_h) {
            SetCursorPos(cx, cy);
        }
    }
}

bool QueryOsCursorVisibleWin32(const MkbState& st) {
    HWND hwnd = static_cast<HWND>(st.window_handle);
    HCURSOR h_blank = GetBlankCursorWin32();
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        if (hwnd && IsWindow(hwnd)) {
            HWND over = WindowFromPoint(ci.ptScreenPos);
            HWND root = over ? GetAncestor(over, GA_ROOT) : nullptr;
            if (root == hwnd) {
                if ((ci.flags & CURSOR_SHOWING) == 0) return false;
                if (ci.hCursor == nullptr || (h_blank && ci.hCursor == h_blank)) return false;
                return true;
            }
        }
    }
    return st.os_cursor_visible.load(std::memory_order_acquire);
}
#endif

// Must ONLY run on the OS GUI / window message-loop thread!
void ApplyOsCursorCaptureOnGuiThread(MkbState& st, void* win_ptr, bool want_capture, bool want_hide, bool force) {
    if (!win_ptr) return;
    want_hide = want_hide || want_capture;
    const bool is_autotest = st.autotest_active.load(std::memory_order_relaxed);
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(win_ptr);
    if (!IsWindow(hwnd)) return;
    const bool was_locked = st.os_cursor_locked.load(std::memory_order_relaxed);
    const bool was_hidden = st.os_cursor_hidden.load(std::memory_order_relaxed);
    HCURSOR h_blank = GetBlankCursorWin32();
    HCURSOR h_arrow = GetArrowCursorWin32();

    // 1. Multi-monitor ClipCursor lock + center-warp (only when want_capture is true)
    if (want_capture) {
        if (!is_autotest) {
            ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, was_locked);
        }
        st.os_cursor_locked.store(true, std::memory_order_release);
    } else {
        if (!is_autotest && (was_locked || force)) {
            ClipCursor(nullptr);
        }
        st.os_cursor_locked.store(false, std::memory_order_release);
    }

    // 2. Class cursor override + 32x32 transparent HCURSOR + ShowCursor display count
    if (want_hide) {
        const LONG_PTR want_cls = reinterpret_cast<LONG_PTR>(h_blank);
        if (GetClassLongPtrW(hwnd, GCLP_HCURSOR) != want_cls) {
            SetClassLongPtrW(hwnd, GCLP_HCURSOR, want_cls);
        }
        if (!was_hidden || force) {
            while (ShowCursor(FALSE) >= 0) {
            }
            SetCursor(h_blank);
            st.os_cursor_hidden.store(true, std::memory_order_release);

            (void)ShowCursor(FALSE);
            int disp_cnt = ShowCursor(TRUE);
            HCURSOR cur_h = GetCursor();
            bool sprite_vis = (cur_h != nullptr) && (!h_blank || cur_h != h_blank);
            bool vis = (disp_cnt >= 0) && sprite_vis;
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci)) {
                HWND over = WindowFromPoint(ci.ptScreenPos);
                HWND root = over ? GetAncestor(over, GA_ROOT) : nullptr;
                if (root == hwnd) {
                    vis = ((ci.flags & CURSOR_SHOWING) != 0) && (ci.hCursor != nullptr) &&
                          (!h_blank || ci.hCursor != h_blank);
                }
            }
            st.os_cursor_visible.store(vis, std::memory_order_release);
        } else {
            if (GetCursor() != h_blank) {
                SetCursor(h_blank);
            }
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            bool vis = false;
            if (GetCursorInfo(&ci)) {
                HWND over = WindowFromPoint(ci.ptScreenPos);
                HWND root = over ? GetAncestor(over, GA_ROOT) : nullptr;
                if (root == hwnd && ((ci.flags & CURSOR_SHOWING) != 0) && (ci.hCursor != nullptr) &&
                    (!h_blank || ci.hCursor != h_blank)) {
                    while (ShowCursor(FALSE) >= 0) {
                    }
                    SetCursor(h_blank);
                    if (GetCursorInfo(&ci)) {
                        vis = ((ci.flags & CURSOR_SHOWING) != 0) && (ci.hCursor != nullptr) &&
                              (!h_blank || ci.hCursor != h_blank);
                    }
                }
            }
            st.os_cursor_visible.store(vis, std::memory_order_release);
        }
    } else {
        const LONG_PTR want_cls = reinterpret_cast<LONG_PTR>(h_arrow);
        if (GetClassLongPtrW(hwnd, GCLP_HCURSOR) != want_cls) {
            SetClassLongPtrW(hwnd, GCLP_HCURSOR, want_cls);
        }
        if (was_hidden || force || !st.os_cursor_visible.load(std::memory_order_relaxed)) {
            while (ShowCursor(TRUE) < 0) {
            }
            SetCursor(h_arrow);
            st.os_cursor_hidden.store(false, std::memory_order_release);

            (void)ShowCursor(TRUE);
            int disp_cnt = ShowCursor(FALSE);
            HCURSOR cur_h = GetCursor();
            bool sprite_vis = (cur_h != nullptr) && (!h_blank || cur_h != h_blank);
            bool vis = (disp_cnt >= 0) && sprite_vis;
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci)) {
                HWND over = WindowFromPoint(ci.ptScreenPos);
                HWND root = over ? GetAncestor(over, GA_ROOT) : nullptr;
                if (root == hwnd) {
                    vis = ((ci.flags & CURSOR_SHOWING) != 0) && (ci.hCursor != nullptr) &&
                          (!h_blank || ci.hCursor != h_blank);
                }
            }
            st.os_cursor_visible.store(vis, std::memory_order_release);
        }
    }
#else
    (void)force;
    SDL_Window* win = static_cast<SDL_Window*>(win_ptr);
    if (want_capture) {
        if (!is_autotest) {
            int lw = 0, lh = 0;
            SDL_GetWindowSize(win, &lw, &lh);
            SDL_Rect inner_rect{0, 0, lw, lh};
            if (lw > 8 && lh > 8) {
                inner_rect.x = 2;
                inner_rect.y = 2;
                inner_rect.w = lw - 4;
                inner_rect.h = lh - 4;
            }
            if (inner_rect.w > 0 && inner_rect.h > 0) {
                SDL_SetWindowMouseRect(win, &inner_rect);
            }
            SDL_SetWindowMouseGrab(win, true);
            SDL_SetWindowRelativeMouseMode(win, true);
        }
        SDL_HideCursor();
        st.os_cursor_locked.store(true, std::memory_order_release);
        st.os_cursor_hidden.store(true, std::memory_order_release);
    } else if (want_hide) {
        if (!is_autotest) {
            SDL_SetWindowRelativeMouseMode(win, false);
            SDL_SetWindowMouseGrab(win, false);
            SDL_SetWindowMouseRect(win, nullptr);
        }
        SDL_HideCursor();
        st.os_cursor_locked.store(false, std::memory_order_release);
        st.os_cursor_hidden.store(true, std::memory_order_release);
    } else {
        if (!is_autotest) {
            SDL_SetWindowRelativeMouseMode(win, false);
            SDL_SetWindowMouseGrab(win, false);
            SDL_SetWindowMouseRect(win, nullptr);
        }
        SDL_ShowCursor();
        st.os_cursor_locked.store(false, std::memory_order_release);
        st.os_cursor_hidden.store(false, std::memory_order_release);
    }
    st.os_cursor_visible.store(SDL_CursorVisible(), std::memory_order_release);
#endif
}

// Safe to call from ANY thread (D3D12/Vulkan render thread, guest thread, or GUI thread).
// Marshals ShowCursor/SetCursor/ClipCursor/SDL_SetWindowRelativeMouseMode strictly onto the GUI thread.
void ApplyOsCursorCaptureLocked(MkbState& st, bool want_capture, bool want_hide, bool force = false,
                                bool tick = false) {
    want_hide = want_hide || want_capture;
    st.cursor_captured.store(want_capture, std::memory_order_release);
    st.cursor_should_hide.store(want_hide, std::memory_order_release);
    if (!st.window_handle) return;
    const bool changed = !st.cursor_sync_initialized || st.last_requested_capture != want_capture ||
                         st.last_requested_hide != want_hide;
    if (!force && !changed && !tick) {
        return;
    }
    st.cursor_sync_initialized = true;
    st.last_requested_capture = want_capture;
    st.last_requested_hide = want_hide;
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(st.window_handle);
    if (IsWindow(hwnd)) {
        if (GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, nullptr)) {
            ApplyOsCursorCaptureOnGuiThread(st, hwnd, want_capture, want_hide, force || changed);
        } else {
            const WPARAM flags = (want_capture ? 1u : 0u) | (want_hide ? 2u : 0u) | ((force || changed) ? 4u : 0u);
            if (force || changed) {
                st.cursor_tick_queued.store(true, std::memory_order_release);
                PostMessageW(hwnd, kWinMsgSyncCursor, flags, 0);
            } else if (!st.cursor_tick_queued.exchange(true, std::memory_order_acq_rel)) {
                PostMessageW(hwnd, kWinMsgSyncCursor, flags, 0);
            }
        }
    }
#else
    if (force || changed) {
        st.cursor_tick_queued.store(true, std::memory_order_release);
        SDL_Event cev{};
        cev.type = SDL_EVENT_USER;
        cev.user.code = kSdlEvSyncCursor;
        cev.user.data1 = reinterpret_cast<void*>(
            uintptr_t((want_capture ? 1u : 0u) | (want_hide ? 2u : 0u) | 4u));
        SDL_PushEvent(&cev);
    } else if (tick && !st.cursor_tick_queued.exchange(true, std::memory_order_acq_rel)) {
        SDL_Event cev{};
        cev.type = SDL_EVENT_USER;
        cev.user.code = kSdlEvSyncCursor;
        cev.user.data1 = reinterpret_cast<void*>(uintptr_t((want_capture ? 1u : 0u) | (want_hide ? 2u : 0u)));
        SDL_PushEvent(&cev);
    }
#endif
}

void MarkMkbActivity(MkbState& st) {
    if (st.cfg.input_mode == InputMode::Auto) {
        st.active_device.store(ActiveDevice::Mkb, std::memory_order_relaxed);
    }
}

bool PadHasRealActivity(MkbState& st, const input::Pad& p) {
    if (!p.connected) {
        st.prev_phys_buttons = 0;
        std::memset(st.prev_phys_analog, 0, sizeof(st.prev_phys_analog));
        return false;
    }
    // Require a NEW digital button press edge so held/noisy bits cannot continuously override Auto mode
    const uint16_t btn_edge = uint16_t(p.buttons & ~st.prev_phys_buttons);
    st.prev_phys_buttons = p.buttons;

    bool analog_active = false;
    for (int i = 0; i < 6; ++i) {
        if (p.analog[i] > 45u && st.prev_phys_analog[i] <= 45u) analog_active = true;
        st.prev_phys_analog[i] = p.analog[i];
    }
    for (int i = 6; i < 8; ++i) {
        if (p.analog[i] > 45u) analog_active = true;
        st.prev_phys_analog[i] = p.analog[i];
    }
    if (btn_edge != 0 || analog_active) return true;

    // Enforce minimum radial stick actuation > 0.25 (~8192/32767) regardless of controller_deadzone slider
    constexpr float kMinStickRadial = 0.25f * 32767.0f;
    const float lmag = std::hypot(float(p.lx), float(p.ly));
    const float rmag = std::hypot(float(p.rx), float(p.ry));
    return (lmag > kMinStickRadial) || (rmag > kMinStickRadial);
}

#ifdef _WIN32
KeyCode Win32VkToKeyCode(WPARAM vk, LPARAM lp) {
    UINT sc = (UINT(lp) >> 16) & 0xFFu;
    bool ext = (lp & (1LL << 24)) != 0;
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;

    if (vk >= 'A' && vk <= 'Z') return KeyCode(uint16_t(KeyCode::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return KeyCode(uint16_t(KeyCode::Num0) + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return KeyCode(uint16_t(KeyCode::F1) + (vk - VK_F1));
    switch (vk) {
        case VK_SPACE: return KeyCode::Space;
        case VK_RETURN: return KeyCode::Enter;
        case VK_ESCAPE: return KeyCode::Escape;
        case VK_TAB: return KeyCode::Tab;
        case VK_BACK: return KeyCode::Backspace;
        case VK_LSHIFT: return KeyCode::LShift;
        case VK_RSHIFT: return KeyCode::RShift;
        case VK_LCONTROL: return KeyCode::LCtrl;
        case VK_RCONTROL: return KeyCode::RCtrl;
        case VK_LMENU: return KeyCode::LAlt;
        case VK_RMENU: return KeyCode::RAlt;
        case VK_UP: return KeyCode::Up;
        case VK_DOWN: return KeyCode::Down;
        case VK_LEFT: return KeyCode::Left;
        case VK_RIGHT: return KeyCode::Right;
        case VK_OEM_MINUS: return KeyCode::Minus;
        case VK_OEM_PLUS: return KeyCode::Equals;
        case VK_OEM_4: return KeyCode::LBracket;
        case VK_OEM_6: return KeyCode::RBracket;
        case VK_OEM_5: return KeyCode::Backslash;
        case VK_OEM_1: return KeyCode::Semicolon;
        case VK_OEM_7: return KeyCode::Apostrophe;
        case VK_OEM_COMMA: return KeyCode::Comma;
        case VK_OEM_PERIOD: return KeyCode::Period;
        case VK_OEM_2: return KeyCode::Slash;
        case VK_OEM_3: return KeyCode::Grave;
        default: return KeyCode::None;
    }
}
#else
KeyCode SdlScancodeToKeyCode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return KeyCode(uint16_t(KeyCode::A) + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return KeyCode(uint16_t(KeyCode::Num1) + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return KeyCode::Num0;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return KeyCode(uint16_t(KeyCode::F1) + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_SPACE: return KeyCode::Space;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return KeyCode::Enter;
        case SDL_SCANCODE_ESCAPE: return KeyCode::Escape;
        case SDL_SCANCODE_TAB: return KeyCode::Tab;
        case SDL_SCANCODE_BACKSPACE: return KeyCode::Backspace;
        case SDL_SCANCODE_LSHIFT: return KeyCode::LShift;
        case SDL_SCANCODE_RSHIFT: return KeyCode::RShift;
        case SDL_SCANCODE_LCTRL: return KeyCode::LCtrl;
        case SDL_SCANCODE_RCTRL: return KeyCode::RCtrl;
        case SDL_SCANCODE_LALT: return KeyCode::LAlt;
        case SDL_SCANCODE_RALT: return KeyCode::RAlt;
        case SDL_SCANCODE_UP: return KeyCode::Up;
        case SDL_SCANCODE_DOWN: return KeyCode::Down;
        case SDL_SCANCODE_LEFT: return KeyCode::Left;
        case SDL_SCANCODE_RIGHT: return KeyCode::Right;
        case SDL_SCANCODE_MINUS: return KeyCode::Minus;
        case SDL_SCANCODE_EQUALS: return KeyCode::Equals;
        case SDL_SCANCODE_LEFTBRACKET: return KeyCode::LBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return KeyCode::RBracket;
        case SDL_SCANCODE_BACKSLASH: return KeyCode::Backslash;
        case SDL_SCANCODE_SEMICOLON: return KeyCode::Semicolon;
        case SDL_SCANCODE_APOSTROPHE: return KeyCode::Apostrophe;
        case SDL_SCANCODE_COMMA: return KeyCode::Comma;
        case SDL_SCANCODE_PERIOD: return KeyCode::Period;
        case SDL_SCANCODE_SLASH: return KeyCode::Slash;
        case SDL_SCANCODE_GRAVE: return KeyCode::Grave;
        default: return KeyCode::None;
    }
}
#endif

void LoadAutotestScriptLocked(MkbState& st) {
    const char* env = std::getenv("XBE_MKB_AUTOTEST");
    if (!env || !*env || std::strcmp(env, "0") == 0) return;
    std::string script_path_s = env;
    if (script_path_s == "1") {
        if (const char* p2 = std::getenv("XBE_MKB_AUTOTEST_SCRIPT"); p2 && *p2) script_path_s = p2;
        else return;
    }
    std::filesystem::path sp(script_path_s);
    std::ifstream f(sp);
    if (!f) {
        LOGE("mkb: failed to open XBE_MKB_AUTOTEST=%s", sp.string().c_str());
        return;
    }
    const char* out_env = std::getenv("XBE_AUTOTEST_OUT_DIR");
    std::filesystem::path out_dir = (out_env && *out_env) ? std::filesystem::path(out_env) : app::ExeDir();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream iss(line);
        std::string cmd;
        if (!(iss >> cmd)) continue;
        std::string ucmd = UpperAscii(cmd);
        if (ucmd == "KEY_MS" || ucmd == "MOUSE_BTN_MS") {
            MkbKeyEvent ev{};
            std::string csv;
            if (!(iss >> ev.start_ms >> ev.end_ms >> csv)) continue;
            std::stringstream ss(csv);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                KeyCode k = TokenToKeyCode(tok);
                if (k != KeyCode::None) ev.keys.push_back(k);
            }
            if (!ev.keys.empty()) st.at_keys.push_back(std::move(ev));
        } else if (ucmd == "MOUSE_MS" || ucmd == "MOUSE_MOVE_MS") {
            MkbMouseEvent ev{};
            if (iss >> ev.target_ms >> ev.dx >> ev.dy) st.at_mouse.push_back(ev);
        } else if (ucmd == "WHEEL_MS" || ucmd == "MOUSE_WHEEL_MS") {
            MkbWheelEvent ev{};
            if (iss >> ev.target_ms >> ev.delta) st.at_wheel.push_back(ev);
        } else if (ucmd == "PAD_MS" || ucmd == "PAD_PULSE_MS") {
            MkbPadEvent ev{};
            std::string btns;
            if (!(iss >> ev.start_ms >> ev.end_ms >> btns)) continue;
            std::stringstream bss(btns);
            std::string tok;
            while (std::getline(bss, tok, ',')) ApplyPadToken(tok, ev);
            int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
            if (iss >> lx >> ly >> rx >> ry) {
                ev.lx = int16_t(std::clamp(lx, -32768, 32767));
                ev.ly = int16_t(std::clamp(ly, -32768, 32767));
                ev.rx = int16_t(std::clamp(rx, -32768, 32767));
                ev.ry = int16_t(std::clamp(ry, -32768, 32767));
                ev.has_sticks = true;
                if (iss >> lt) ev.analog[6] = std::max(ev.analog[6], uint8_t(std::clamp(lt, 0, 255)));
                if (iss >> rt) ev.analog[7] = std::max(ev.analog[7], uint8_t(std::clamp(rt, 0, 255)));
            }
            st.at_pads.push_back(ev);
        } else if (ucmd == "SET_MS" || ucmd == "SET_OPT") {
            MkbSetEvent ev{};
            if (iss >> ev.target_ms >> ev.key >> ev.value) st.at_sets.push_back(std::move(ev));
        } else if (ucmd == "CURSOR_MS" || ucmd == "UI_CURSOR_MS") {
            MkbCursorEvent ev{};
            if (iss >> ev.target_ms >> ev.ui_x >> ev.ui_y) {
                ev.has_pos = true;
                st.at_cursors.push_back(ev);
            }
        } else if (ucmd == "LMB_CLICK_MS" || ucmd == "UI_CLICK_MS") {
            MkbCursorEvent ev{};
            if (iss >> ev.target_ms) {
                ev.click_lmb = true;
                if (iss >> ev.ui_x >> ev.ui_y) ev.has_pos = true;
                else ev.has_pos = false;
                st.at_cursors.push_back(ev);
            }
        } else if (ucmd == "CLIENT_CURSOR_MS" || ucmd == "CLIENT_CLICK_MS") {
            MkbCursorEvent ev{};
            if (iss >> ev.target_ms >> ev.client_x >> ev.client_y) {
                if (!(iss >> ev.client_w >> ev.client_h)) {
                    const app::Mode out_m = app::Output();
                    ev.client_w = float(out_m.width ? out_m.width : 1920u);
                    ev.client_h = float(out_m.height ? out_m.height : 1080u);
                }
                ev.has_pos = true;
                ev.is_client_coords = true;
                ev.click_lmb = (ucmd == "CLIENT_CLICK_MS");
                st.at_cursors.push_back(ev);
            }
        } else if (ucmd == "RMB_CLICK_MS" || ucmd == "UI_BACK_MS") {
            MkbCursorEvent ev{};
            if (iss >> ev.target_ms) {
                ev.click_rmb = true;
                ev.has_pos = false;
                st.at_cursors.push_back(ev);
            }
        } else if (ucmd == "CAPTURE_MS") {
            uint64_t ms = 0;
            std::string fname;
            if (!(iss >> ms >> fname)) continue;
            std::filesystem::path p(fname);
            if (p.is_relative()) p = out_dir / p;
            st.at_caps.push_back({ms, std::move(p), false});
        } else if (ucmd == "EXIT_MS") {
            uint64_t ms = 0;
            if (iss >> ms) st.at_exit_ms = std::max(st.at_exit_ms, ms);
        }
    }
    st.autotest_active.store(true, std::memory_order_release);
    LOGR("mkb: loaded XBE_MKB_AUTOTEST=%s (keys=%zu mouse=%zu wheel=%zu pads=%zu sets=%zu caps=%zu exit_ms=%llu)",
         sp.string().c_str(), st.at_keys.size(), st.at_mouse.size(), st.at_wheel.size(), st.at_pads.size(),
         st.at_sets.size(), st.at_caps.size(), (unsigned long long)st.at_exit_ms);
}

void ApplyRuntimeOptionLocked(MkbState& st, const std::string& key_raw, const std::string& val_raw) {
    std::string k = LowerAscii(key_raw);
    std::string v = LowerAscii(val_raw);
    bool bval = (v == "1" || v == "true" || v == "on");
    if (k == "mouse_invert_y" || k == "invert_y") {
        st.cfg.mouse_invert_y = bval;
    } else if (k == "mouse_raw_aim" || k == "raw_aim") {
        st.cfg.mouse_raw_aim = bval;
    } else if (k == "mouse_sensitivity" || k == "sensitivity") {
        st.cfg.mouse_sensitivity = std::clamp(std::strtof(val_raw.c_str(), nullptr), 0.05f, 20.0f);
    } else if (k == "mouse_smoothing" || k == "smoothing") {
        st.cfg.mouse_smoothing = bval;
    } else if (k == "overlay_open") {
        st.overlay_open.store(bval, std::memory_order_release);
        if (!bval) {
            st.rebind_action = -1;
            st.rebind_slot = -1;
        }
    } else if (k == "window_focused") {
        st.window_focused.store(bval, std::memory_order_release);
    } else if (k == "fps_cap") {
        app::Get().fps_cap = uint32_t(std::max(0, std::atoi(val_raw.c_str())));
    } else if (k == "controller_deadzone_left") {
        st.cfg.controller_deadzone_left = uint32_t(std::clamp(std::atoi(val_raw.c_str()), 0, 40));
        input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
    } else if (k == "controller_deadzone_right") {
        st.cfg.controller_deadzone_right = uint32_t(std::clamp(std::atoi(val_raw.c_str()), 0, 40));
        input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
    } else if (k == "input_mode") {
        if (v == "mkb") {
            st.cfg.input_mode = InputMode::Mkb;
            st.active_device.store(ActiveDevice::Mkb, std::memory_order_relaxed);
        } else if (v == "controller") {
            st.cfg.input_mode = InputMode::Controller;
            st.active_device.store(ActiveDevice::Controller, std::memory_order_relaxed);
        } else {
            st.cfg.input_mode = InputMode::Auto;
        }
    } else if (k == "ui_aspect" || k == "ui_aspect_mode") {
        if (v == "menus_and_crosshair" || v == "menus_crosshair" || v == "1") {
            psiops::ui_aspect::SetMode(psiops::ui_aspect::Mode::MenusAndCrosshair);
        } else if (v == "stretched" || v == "stretched_16x9" || v == "2") {
            psiops::ui_aspect::SetMode(psiops::ui_aspect::Mode::Stretched16x9);
        } else {
            psiops::ui_aspect::SetMode(psiops::ui_aspect::Mode::AspectCorrect);
        }
    }
}

bool IsKeyHeldLocked(const MkbState& st, KeyCode k) {
    if (k == KeyCode::None || size_t(k) >= size_t(KeyCode::Count)) return false;
    if (st.autotest_active.load(std::memory_order_relaxed)) {
        return st.at_key_down[size_t(k)];
    }
    const uint8_t mbit = MouseButtonMaskBit(k);
    if (mbit != 0u && (st.suppressed_mouse_mask.load(std::memory_order_acquire) & mbit) != 0u) {
        return false;
    }
    return st.key_down[size_t(k)].load(std::memory_order_relaxed);
}

bool IsActionHeldLocked(const MkbState& st, Action a) {
    const auto& slots = st.cfg.bindings[size_t(a)];
    return IsKeyHeldLocked(st, slots[0]) || IsKeyHeldLocked(st, slots[1]);
}

void DefaultActionMap(const ActionPadState& act, input::Pad& pad) {
    const bool in_menu = (act.phase == perf::Phase::TitleMenu || act.phase == perf::Phase::Paused);
    const bool in_fmv = (act.phase == perf::Phase::Fmv);

    if (in_fmv) {
        if (act.esc_pulse || act.held[size_t(Action::Interact)] || act.held[size_t(Action::Jump)]) {
            pad.buttons |= 1u << 4;  // START to skip FMV
        }
        return;
    }

    if (in_menu) {
        if (act.held[size_t(Action::MoveForward)]) pad.buttons |= 1u << 0;   // DPAD_UP
        if (act.held[size_t(Action::MoveBackward)]) pad.buttons |= 1u << 1;  // DPAD_DOWN
        if (act.held[size_t(Action::MoveLeft)]) pad.buttons |= 1u << 2;      // DPAD_LEFT
        if (act.held[size_t(Action::MoveRight)]) pad.buttons |= 1u << 3;     // DPAD_RIGHT
        if (act.held[size_t(Action::Interact)] || act.held[size_t(Action::Jump)]) {
            pad.analog[0] = 0xFF;  // A
            if (act.phase == perf::Phase::TitleMenu) pad.buttons |= 1u << 4;  // START on Title Screen
        }
        if (act.esc_pulse) {
            if (act.phase == perf::Phase::Paused) pad.buttons |= 1u << 4;  // START resumes
            else pad.analog[1] = 0xFF;  // B goes back in frontend menus
        }
        return;
    }

    // Active 3D gameplay fallback mapping (overridden by Psi-Ops binary-verified ActionMapCallback)
    if (act.wasd_mag > 0.0f) {
        pad.lx = int16_t(std::clamp(std::lround(act.wasd_x * 32767.0f), -32768L, 32767L));
        pad.ly = int16_t(std::clamp(std::lround(act.wasd_y * 32767.0f), -32768L, 32767L));
    }
    if (act.held[size_t(Action::Fire)]) pad.analog[7] = 0xFF;       // RT
    if (act.held[size_t(Action::PsiUse)]) pad.analog[6] = 0xFF;     // LT
    if (act.held[size_t(Action::Jump)]) pad.analog[0] = 0xFF;       // A
    if (act.held[size_t(Action::Interact)]) pad.analog[2] = 0xFF;   // X
    if (act.held[size_t(Action::Reload)]) pad.analog[3] = 0xFF;     // Y
    if (act.held[size_t(Action::Crouch)]) pad.buttons |= 1u << 6;   // LTHUMB
    if (act.held[size_t(Action::FineAim)]) pad.buttons |= 1u << 7;  // RTHUMB
    if (act.held[size_t(Action::Melee)]) pad.analog[1] = 0xFF;      // B
    if (act.wheel_up_pulse) pad.analog[5] = 0xFF;                   // WHITE
    if (act.wheel_down_pulse) pad.analog[4] = 0xFF;                 // BLACK
    if (act.esc_pulse) pad.buttons |= 1u << 4;                      // START -> Pause
    if (act.held[size_t(Action::Objectives)]) pad.buttons |= 1u << 5;  // BACK
}

}  // namespace

const char* KeyCodeToToken(KeyCode k) {
    for (const auto& m : kKeyTable) {
        if (m.code == k) return m.token;
    }
    return "";
}

KeyCode TokenToKeyCode(std::string_view token) {
    std::string u = UpperAscii(token);
    if (u.empty() || u == "NONE") return KeyCode::None;
    if (u == "ESC") return KeyCode::Escape;
    if (u == "RETURN") return KeyCode::Enter;
    if (u == "CTRL") return KeyCode::LCtrl;
    if (u == "SHIFT") return KeyCode::LShift;
    if (u == "ALT") return KeyCode::LAlt;
    for (const auto& m : kKeyTable) {
        if (UpperAscii(m.token) == u) return m.code;
    }
    return KeyCode::None;
}

const char* ActionToKey(Action a) {
    if (size_t(a) < size_t(Action::Count)) return kActionTable[size_t(a)].key;
    return "";
}

const char* ActionLabel(Action a) {
    if (size_t(a) < size_t(Action::Count)) return kActionTable[size_t(a)].label;
    return "";
}

float RadPerCount(float sensitivity) { return kBaseRadPerCount * std::clamp(sensitivity, 0.05f, 20.0f); }

float CmPer360At800Dpi(float sensitivity) {
    float rpc = RadPerCount(sensitivity);
    return ((2.0f * kPi) / rpc) / 800.0f * 2.54f;
}

void Init() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    st.suppressed_mouse_mask.store(0u, std::memory_order_release);
    st.prev_gameplay_mouse_allowed = false;
    LoadAutotestScriptLocked(st);
    input::SetRumbleStrength(st.cfg.rumble_strength);
    input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
    input::SetTouchpadAsBack(st.cfg.dualsense_touchpad_back);
}

void Shutdown() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    st.suppressed_mouse_mask.store(0u, std::memory_order_release);
    st.prev_gameplay_mouse_allowed = false;
    ApplyOsCursorCaptureLocked(st, false, false, true);
#ifdef _WIN32
    ClipCursor(nullptr);
    if (HWND hwnd = static_cast<HWND>(st.window_handle); hwnd && IsWindow(hwnd)) {
        SetClassLongPtrW(hwnd, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(GetArrowCursorWin32()));
    }
#endif
}

void LoadSettings(const void* toml_table_ptr) {
    if (!toml_table_ptr) return;
    const auto& root = *static_cast<const toml::table*>(toml_table_ptr);
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    const toml::table* ctrls = root["controls"].as_table();
    if (!ctrls) return;

    if (auto v = (*ctrls)["input_mode"].value<std::string>()) {
        std::string lv = LowerAscii(*v);
        if (lv == "mkb") {
            st.cfg.input_mode = InputMode::Mkb;
            st.active_device.store(ActiveDevice::Mkb, std::memory_order_relaxed);
        } else if (lv == "controller") {
            st.cfg.input_mode = InputMode::Controller;
            st.active_device.store(ActiveDevice::Controller, std::memory_order_relaxed);
        } else {
            st.cfg.input_mode = InputMode::Auto;
        }
    }
    if (auto v = (*ctrls)["mouse_sensitivity"].value<double>()) {
        st.cfg.mouse_sensitivity = std::clamp(float(*v), 0.05f, 20.0f);
    }
    if (auto v = (*ctrls)["mouse_invert_y"].value<bool>()) st.cfg.mouse_invert_y = *v;
    if (auto v = (*ctrls)["mouse_raw_aim"].value<bool>()) st.cfg.mouse_raw_aim = *v;
    else if (auto v2 = (*ctrls)["raw_aim"].value<bool>()) st.cfg.mouse_raw_aim = *v2;
    if (auto v = (*ctrls)["mouse_smoothing"].value<bool>()) st.cfg.mouse_smoothing = *v;
    else if (auto vf = (*ctrls)["mouse_smoothing"].value<double>()) st.cfg.mouse_smoothing = (*vf > 0.001);
    if (auto v = (*ctrls)["rumble_strength"].value<int64_t>()) {
        st.cfg.rumble_strength = uint32_t(std::clamp<int64_t>(*v, 0, 100));
    }
    if (auto v = (*ctrls)["controller_deadzone_left"].value<int64_t>()) {
        st.cfg.controller_deadzone_left = uint32_t(std::clamp<int64_t>(*v, 0, 40));
    }
    if (auto v = (*ctrls)["controller_deadzone_right"].value<int64_t>()) {
        st.cfg.controller_deadzone_right = uint32_t(std::clamp<int64_t>(*v, 0, 40));
    }
    if (auto v = (*ctrls)["dualsense_touchpad_back"].value<bool>()) st.cfg.dualsense_touchpad_back = *v;
    else if (auto v2 = (*ctrls)["touchpad_as_back"].value<bool>()) st.cfg.dualsense_touchpad_back = *v2;

    if (const toml::table* binds = (*ctrls)["bindings"].as_table()) {
        for (const auto& m : kActionTable) {
            const toml::array* arr = (*binds)[m.key].as_array();
            if (!arr) continue;
            KeyCode s0 = KeyCode::None, s1 = KeyCode::None;
            if (arr->size() >= 1) {
                if (auto t0 = (*arr)[0].value<std::string>()) s0 = TokenToKeyCode(*t0);
            }
            if (arr->size() >= 2) {
                if (auto t1 = (*arr)[1].value<std::string>()) s1 = TokenToKeyCode(*t1);
            }
            st.cfg.bindings[size_t(m.action)][0] = s0;
            st.cfg.bindings[size_t(m.action)][1] = s1;
        }
    }

    input::SetRumbleStrength(st.cfg.rumble_strength);
    input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
    input::SetTouchpadAsBack(st.cfg.dualsense_touchpad_back);
}

void SaveSettings(void* toml_table_ptr) {
    if (!toml_table_ptr) return;
    auto& root = *static_cast<toml::table*>(toml_table_ptr);
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);

    const char* mode_str = (st.cfg.input_mode == InputMode::Mkb)
                               ? "mkb"
                               : (st.cfg.input_mode == InputMode::Controller ? "controller" : "auto");
    toml::table ctrls;
    ctrls.insert("input_mode", mode_str);
    ctrls.insert("mouse_sensitivity", double(st.cfg.mouse_sensitivity));
    ctrls.insert("mouse_invert_y", st.cfg.mouse_invert_y);
    ctrls.insert("mouse_raw_aim", st.cfg.mouse_raw_aim);
    ctrls.insert("mouse_smoothing", st.cfg.mouse_smoothing);
    ctrls.insert("rumble_strength", int64_t(st.cfg.rumble_strength));
    ctrls.insert("controller_deadzone_left", int64_t(st.cfg.controller_deadzone_left));
    ctrls.insert("controller_deadzone_right", int64_t(st.cfg.controller_deadzone_right));
    ctrls.insert("dualsense_touchpad_back", st.cfg.dualsense_touchpad_back);

    toml::table binds;
    for (const auto& m : kActionTable) {
        toml::array arr;
        arr.push_back(std::string(KeyCodeToToken(st.cfg.bindings[size_t(m.action)][0])));
        arr.push_back(std::string(KeyCodeToToken(st.cfg.bindings[size_t(m.action)][1])));
        binds.insert(m.key, std::move(arr));
    }
    ctrls.insert("bindings", std::move(binds));
    root.insert_or_assign("controls", std::move(ctrls));
}

void OnWindowCreated(void* window_handle) {
    MkbState& st = St();
    SyncImGuiMouseFlags(st.overlay_open.load(std::memory_order_acquire), true);
    std::lock_guard<std::mutex> lk(st.lock);
    const bool first_bind = (st.window_handle != window_handle);
    st.window_handle = window_handle;
    st.prev_abs_valid = false;
#ifdef _WIN32
    if (HWND hwnd = static_cast<HWND>(window_handle)) {
        // Initialize blank & arrow HCURSOR handles on the GUI thread before render/telemetry threads query them.
        (void)GetBlankCursorWin32();
        (void)GetArrowCursorWin32();
        if (!st.autotest_active.load(std::memory_order_relaxed)) {
            if (IsWindowVisible(hwnd)) {
                const bool focused = !IsIconic(hwnd) && (GetForegroundWindow() == hwnd || GetActiveWindow() == hwnd ||
                                                         GetFocus() == hwnd);
                st.window_focused.store(focused, std::memory_order_release);
            } else {
                st.window_focused.store(true, std::memory_order_release);
            }
        }
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
        rid.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
        rid.dwFlags = 0;         // only while focused (never captures background desktop)
        rid.hwndTarget = hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            LOGW("mkb: RegisterRawInputDevices(mouse) failed (err=%lu)", GetLastError());
        } else if (first_bind) {
            LOGR("mkb: Win32 WM_INPUT Raw Input mouse registered (hwnd=%p)", window_handle);
        }
        ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
    }
#else
    if (window_handle) {
        if (first_bind) {
            LOGR("mkb: Linux SDL3 relative mouse + scancode host layer initialized");
        }
        if (!st.autotest_active.load(std::memory_order_relaxed)) {
            SDL_Window* win = static_cast<SDL_Window*>(window_handle);
            SDL_WindowFlags wf = SDL_GetWindowFlags(win);
            if ((wf & SDL_WINDOW_HIDDEN) == 0) {
                const bool focused = ((wf & SDL_WINDOW_MINIMIZED) == 0) && ((wf & SDL_WINDOW_INPUT_FOCUS) != 0);
                st.window_focused.store(focused, std::memory_order_release);
            } else {
                st.window_focused.store(true, std::memory_order_release);
            }
        }
        ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
    }
#endif
}

bool OnWindowMessage(void* window_handle, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    MkbState& st = St();
    auto handle_rebind_key = [&](KeyCode k) -> bool {
        if (k == KeyCode::None) return false;
        std::lock_guard<std::mutex> lk(st.lock);
        if (st.rebind_action < 0) return false;
        if (!st.overlay_open.load(std::memory_order_acquire)) {
            // The Controls panel isn't visible: drop a stale capture instead of eating a gameplay key.
            st.rebind_action = -1;
            st.rebind_slot = -1;
            return false;
        }
        if (k == KeyCode::MouseLeft && input_rec::CurrentPresentFrame() <= st.rebind_arm_frame) return false;
        if (k != KeyCode::Escape) {
            st.cfg.bindings[st.rebind_action][st.rebind_slot] = k;
            st.rebind_committed = true;
        }
        st.rebind_action = -1;
        st.rebind_slot = -1;
        return true;
    };

#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(window_handle ? window_handle : st.window_handle);
    auto recover_focus_on_input = [&]() {
        if (st.autotest_active.load(std::memory_order_relaxed)) return;
        if (!st.window_focused.load(std::memory_order_relaxed)) {
            st.window_focused.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
        }
    };
    auto update_client_ui_pos = [&](int cx, int cy) {
        if (!hwnd) return;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return;
        float ux = 256.0f, uy = 222.5f;
        if (MapClientToUiSpace(float(cx), float(cy), float(rc.right - rc.left), float(rc.bottom - rc.top), ux, uy)) {
            std::lock_guard<std::mutex> lk(st.lock);
            if (!st.ui_pos_valid || std::fabs(ux - st.ui_x) > 0.25f || std::fabs(uy - st.ui_y) > 0.25f) {
                st.ui_x = ux;
                st.ui_y = uy;
                st.ui_pos_valid = true;
                st.ui_moved_pending = true;
            }
        }
    };

    switch (msg) {
        case kWinMsgSyncCursor: {
            st.cursor_tick_queued.store(false, std::memory_order_release);
            SyncImGuiMouseFlags(st.overlay_open.load(std::memory_order_acquire));
            if (!st.autotest_active.load(std::memory_order_relaxed) && hwnd && IsWindow(hwnd) &&
                IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                HWND fg = GetForegroundWindow();
                const bool os_focused = (fg == hwnd) || (GetActiveWindow() == hwnd) || (GetFocus() == hwnd);
                if (os_focused && !st.window_focused.load(std::memory_order_relaxed)) {
                    st.window_focused.store(true, std::memory_order_release);
                } else if (!os_focused && fg != nullptr && st.window_focused.load(std::memory_order_relaxed)) {
                    st.window_focused.store(false, std::memory_order_release);
                }
            }
            bool want_cap = false;
            bool want_hide = false;
            {
                std::lock_guard<std::mutex> lk(st.lock);
                want_cap = ComputeWantCursorCapture(st);
                want_hide = ComputeWantHideCursor(st);
                st.cursor_captured.store(want_cap, std::memory_order_release);
                st.cursor_should_hide.store(want_hide || want_cap, std::memory_order_release);
                st.cursor_sync_initialized = true;
                st.last_requested_capture = want_cap;
                st.last_requested_hide = want_hide || want_cap;
            }
            ApplyOsCursorCaptureOnGuiThread(st, hwnd, want_cap, want_hide, (wp & 4u) != 0);
            if (result) *result = 0;
            return true;
        }
        case WM_SETCURSOR: {
            if (st.cursor_should_hide.load(std::memory_order_acquire)) {
                SetCursor(GetBlankCursorWin32());
                if (result) *result = TRUE;
                return true;
            }
            if (LOWORD(lp) == HTCLIENT && !st.overlay_open.load(std::memory_order_acquire)) {
                SetCursor(GetArrowCursorWin32());
                if (result) *result = TRUE;
                return true;
            }
            return false;
        }
        case WM_MOUSEMOVE: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            if (st.cursor_captured.load(std::memory_order_acquire)) {
                ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
            } else if (!st.overlay_open.load(std::memory_order_acquire) &&
                       !st.cursor_should_hide.load(std::memory_order_acquire)) {
                int cx = int(short(LOWORD(lp)));
                int cy = int(short(HIWORD(lp)));
                MarkMkbActivity(st);
                update_client_ui_pos(cx, cy);
            }
            return false;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            bool down = (msg != WM_LBUTTONUP);
            NoteMouseButtonEdge(st, KeyCode::MouseLeft, down);
            if (down) {
                recover_focus_on_input();
                MarkMkbActivity(st);
                if (handle_rebind_key(KeyCode::MouseLeft)) return true;
                if (!st.overlay_open.load(std::memory_order_acquire)) {
                    if (!st.cursor_captured.load(std::memory_order_acquire) &&
                        !st.cursor_should_hide.load(std::memory_order_acquire)) {
                        update_client_ui_pos(int(short(LOWORD(lp))), int(short(HIWORD(lp))));
                        std::lock_guard<std::mutex> lk(st.lock);
                        st.ui_lmb_down = true;
                        st.ui_lmb_click_pending = true;
                    } else if (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::Fmv ||
                               st.cur_non_pointer_modal.load(std::memory_order_acquire)) {
                        std::lock_guard<std::mutex> lk(st.lock);
                        st.non_pointer_lmb_pending = true;
                    }
                }
            } else {
                {
                    std::lock_guard<std::mutex> lk(st.lock);
                    st.ui_lmb_down = false;
                }
                if (st.cursor_captured.load(std::memory_order_acquire)) {
                    ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
                }
            }
            st.key_down[size_t(KeyCode::MouseLeft)].store(down, std::memory_order_relaxed);
            return false;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_RBUTTONUP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            bool down = (msg != WM_RBUTTONUP);
            NoteMouseButtonEdge(st, KeyCode::MouseRight, down);
            if (down) {
                recover_focus_on_input();
                MarkMkbActivity(st);
                if (handle_rebind_key(KeyCode::MouseRight)) return true;
                if (!st.cursor_captured.load(std::memory_order_acquire) &&
                    !st.cursor_should_hide.load(std::memory_order_acquire) &&
                    !st.overlay_open.load(std::memory_order_acquire)) {
                    update_client_ui_pos(int(short(LOWORD(lp))), int(short(HIWORD(lp))));
                    std::lock_guard<std::mutex> lk(st.lock);
                    st.ui_rmb_click_pending = true;
                }
            } else if (st.cursor_captured.load(std::memory_order_acquire)) {
                ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
            }
            st.key_down[size_t(KeyCode::MouseRight)].store(down, std::memory_order_relaxed);
            return false;
        }
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_MBUTTONUP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            bool down = (msg != WM_MBUTTONUP);
            NoteMouseButtonEdge(st, KeyCode::MouseMiddle, down);
            if (down) {
                recover_focus_on_input();
                MarkMkbActivity(st);
                if (handle_rebind_key(KeyCode::MouseMiddle)) return true;
            } else if (st.cursor_captured.load(std::memory_order_acquire)) {
                ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
            }
            st.key_down[size_t(KeyCode::MouseMiddle)].store(down, std::memory_order_relaxed);
            return false;
        }
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_XBUTTONUP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            WORD xbtn = HIWORD(wp);
            KeyCode kc = (xbtn == XBUTTON1) ? KeyCode::Mouse4 : ((xbtn == XBUTTON2) ? KeyCode::Mouse5 : KeyCode::None);
            if (kc != KeyCode::None) {
                bool down = (msg != WM_XBUTTONUP);
                NoteMouseButtonEdge(st, kc, down);
                if (down) {
                    recover_focus_on_input();
                    MarkMkbActivity(st);
                    if (handle_rebind_key(kc)) return true;
                } else if (st.cursor_captured.load(std::memory_order_acquire)) {
                    ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
                }
                st.key_down[size_t(kc)].store(down, std::memory_order_relaxed);
            }
            return false;
        }
        case WM_MOUSEWHEEL: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            SHORT wd = static_cast<SHORT>(HIWORD(wp));
            int notches = (wd > 0) ? 1 : (wd < 0 ? -1 : 0);
            if (notches != 0) {
                MarkMkbActivity(st);
                KeyCode wk = (notches > 0) ? KeyCode::WheelUp : KeyCode::WheelDown;
                if (!handle_rebind_key(wk)) {
                    std::lock_guard<std::mutex> lk(st.lock);
                    if (st.cursor_captured.load(std::memory_order_acquire)) {
                        if (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive) {
                            if (st.wheel_queue.size() < 8) st.wheel_queue.push_back(notches);
                            if (st.gameplay_wheel_queue.size() < 8) st.gameplay_wheel_queue.push_back(notches);
                        }
                    } else if (!st.cursor_should_hide.load(std::memory_order_acquire) &&
                               !st.overlay_open.load(std::memory_order_acquire)) {
                        st.ui_wheel_pending += notches;
                    }
                }
            }
            return false;
        }
        case WM_INPUT: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            UINT sz = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
            if (sz > 0 && sz <= 256) {
                alignas(8) uint8_t buf[256];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == sz) {
                    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
                    if (ri->header.dwType == RIM_TYPEMOUSE) {
                        const RAWMOUSE& rm = ri->data.mouse;
                        const bool is_abs = (rm.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
                        st.last_raw_was_absolute.store(is_abs, std::memory_order_relaxed);
                        int32_t dx = 0, dy = 0;
                        if (!is_abs) {
                            dx = int32_t(rm.lLastX);
                            dy = int32_t(rm.lLastY);
                        } else {
                            // Virtual display / absolute pointing device fallback (0..65535 -> screen pixels)
                            const bool virt = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                            const int sw = std::max(1, GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN));
                            const int sh = std::max(1, GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));
                            int32_t px = int32_t((int64_t(rm.lLastX) * sw) / 65535);
                            int32_t py = int32_t((int64_t(rm.lLastY) * sh) / 65535);
                            if (st.prev_abs_valid) {
                                dx = px - st.prev_abs_x;
                                dy = py - st.prev_abs_y;
                            }
                            st.prev_abs_x = px;
                            st.prev_abs_y = py;
                            st.prev_abs_valid = true;
                        }
                        if (dx != 0 || dy != 0) {
                            MarkMkbActivity(st);
                            bool cap = st.cursor_captured.load(std::memory_order_acquire);
                            if (!cap && ComputeWantCursorCapture(st)) {
                                std::lock_guard<std::mutex> lk(st.lock);
                                ApplyOsCursorCaptureLocked(st, true, true, true);
                                cap = true;
                            }
                            if (cap) {
                                if (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive) {
                                    st.raw_dx.fetch_add(dx, std::memory_order_relaxed);
                                    st.raw_dy.fetch_add(dy, std::memory_order_relaxed);
                                }
                                ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
                            }
                        }
                        auto set_btn = [&](USHORT down_flag, USHORT up_flag, KeyCode kc) {
                            if (rm.usButtonFlags & down_flag) {
                                MarkMkbActivity(st);
                                NoteMouseButtonEdge(st, kc, true);
                                if (!handle_rebind_key(kc)) st.key_down[size_t(kc)].store(true, std::memory_order_relaxed);
                            }
                            if (rm.usButtonFlags & up_flag) {
                                NoteMouseButtonEdge(st, kc, false);
                                st.key_down[size_t(kc)].store(false, std::memory_order_relaxed);
                                if (st.cursor_captured.load(std::memory_order_acquire)) {
                                    ReassertWin32ClipAndWarpOnGuiThread(st, hwnd, true);
                                }
                            }
                        };
                        set_btn(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, KeyCode::MouseLeft);
                        set_btn(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, KeyCode::MouseRight);
                        set_btn(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, KeyCode::MouseMiddle);
                        set_btn(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, KeyCode::Mouse4);
                        set_btn(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, KeyCode::Mouse5);
                    }
                }
            }
            return false;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            recover_focus_on_input();
            KeyCode kc = Win32VkToKeyCode(wp, lp);
            if (kc == KeyCode::F1 && ((lp & (1LL << 30)) == 0)) {
                bool next = !st.overlay_open.load(std::memory_order_relaxed);
                st.overlay_open.store(next, std::memory_order_release);
                SyncImGuiMouseFlags(next, true);
                std::lock_guard<std::mutex> lk(st.lock);
                if (!next) {
                    st.rebind_action = -1;
                    st.rebind_slot = -1;
                    LatchHeldMouseButtonsForTransition(st);
                }
                ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
                return true;
            }
            if (kc != KeyCode::None) {
                MarkMkbActivity(st);
                // Ignore auto-repeat so holding the key that armed the capture (Enter/Space) can't bind itself.
                if (((lp & (1LL << 30)) == 0) && handle_rebind_key(kc)) return true;
                st.key_down[size_t(kc)].store(true, std::memory_order_relaxed);
            }
            return false;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            KeyCode kc = Win32VkToKeyCode(wp, lp);
            if (kc != KeyCode::None) st.key_down[size_t(kc)].store(false, std::memory_order_relaxed);
            return false;
        }
        case WM_ACTIVATE: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            bool focused = (LOWORD(wp) != WA_INACTIVE);
            st.window_focused.store(focused, std::memory_order_release);
            if (!focused) {
                for (size_t i = 0; i < size_t(KeyCode::Count); ++i)
                    st.key_down[i].store(false, std::memory_order_relaxed);
                st.suppressed_mouse_mask.store(0u, std::memory_order_release);
            }
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
            return false;
        }
        case WM_ACTIVATEAPP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            bool focused = (wp != FALSE);
            st.window_focused.store(focused, std::memory_order_release);
            if (!focused) {
                for (size_t i = 0; i < size_t(KeyCode::Count); ++i)
                    st.key_down[i].store(false, std::memory_order_relaxed);
                st.suppressed_mouse_mask.store(0u, std::memory_order_release);
            }
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
            return false;
        }
        case WM_KILLFOCUS: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            st.window_focused.store(false, std::memory_order_release);
            for (size_t i = 0; i < size_t(KeyCode::Count); ++i)
                st.key_down[i].store(false, std::memory_order_relaxed);
            st.suppressed_mouse_mask.store(0u, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, false, false, true);
            return false;
        }
        case WM_SETFOCUS: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            st.window_focused.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
            return false;
        }
        case WM_ENTERSIZEMOVE: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            st.in_size_move.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, false, false, true);
            return false;
        }
        case WM_EXITSIZEMOVE:
        case WM_DISPLAYCHANGE:
        case WM_SIZE:
        case WM_MOVE: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            if (msg == WM_EXITSIZEMOVE) st.in_size_move.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            bool want_cap = ComputeWantCursorCapture(st);
            bool want_hide = ComputeWantHideCursor(st);
            if (want_cap || want_hide || msg == WM_EXITSIZEMOVE || msg == WM_DISPLAYCHANGE) {
                ApplyOsCursorCaptureLocked(st, want_cap, want_hide, true);
            }
            return false;
        }
        default:
            return false;
    }
#else
    const SDL_Event* ev = reinterpret_cast<const SDL_Event*>(lp);
    if (!ev) return false;
    SDL_Window* win = static_cast<SDL_Window*>(window_handle ? window_handle : st.window_handle);
    auto recover_sdl_focus = [&]() {
        if (st.autotest_active.load(std::memory_order_relaxed)) return;
        if (!st.window_focused.load(std::memory_order_relaxed)) {
            st.window_focused.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
        }
    };
    auto update_sdl_ui_pos = [&](float mx, float my) {
        if (!win) return;
        int lw = 0, lh = 0;
        SDL_GetWindowSize(win, &lw, &lh);
        float ux = 256.0f, uy = 222.5f;
        if (MapClientToUiSpace(mx, my, float(lw), float(lh), ux, uy)) {
            std::lock_guard<std::mutex> lk(st.lock);
            if (!st.ui_pos_valid || std::fabs(ux - st.ui_x) > 0.25f || std::fabs(uy - st.ui_y) > 0.25f) {
                st.ui_x = ux;
                st.ui_y = uy;
                st.ui_pos_valid = true;
                st.ui_moved_pending = true;
            }
        }
    };

    switch (msg) {
        case SDL_EVENT_USER: {
            if (ev->user.code == kSdlEvSyncCursor) {
                st.cursor_tick_queued.store(false, std::memory_order_release);
                SyncImGuiMouseFlags(st.overlay_open.load(std::memory_order_acquire));
                if (!st.autotest_active.load(std::memory_order_relaxed) && win) {
                    SDL_WindowFlags wf = SDL_GetWindowFlags(win);
                    if ((wf & SDL_WINDOW_HIDDEN) == 0 && (wf & SDL_WINDOW_MINIMIZED) == 0) {
                        const bool os_focused = (wf & SDL_WINDOW_INPUT_FOCUS) != 0;
                        st.window_focused.store(os_focused, std::memory_order_release);
                    }
                }
                bool want_cap = false;
                bool want_hide = false;
                {
                    std::lock_guard<std::mutex> lk(st.lock);
                    want_cap = ComputeWantCursorCapture(st);
                    want_hide = ComputeWantHideCursor(st);
                    st.cursor_captured.store(want_cap, std::memory_order_release);
                    st.cursor_should_hide.store(want_hide || want_cap, std::memory_order_release);
                    st.cursor_sync_initialized = true;
                    st.last_requested_capture = want_cap;
                    st.last_requested_hide = want_hide || want_cap;
                }
                const uintptr_t bits = reinterpret_cast<uintptr_t>(ev->user.data1);
                ApplyOsCursorCaptureOnGuiThread(st, win, want_cap, want_hide, (bits & 4u) != 0);
                if (result) *result = 0;
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            int32_t dx = int32_t(std::lround(ev->motion.xrel));
            int32_t dy = int32_t(std::lround(ev->motion.yrel));
            if (dx != 0 || dy != 0) {
                MarkMkbActivity(st);
                bool cap = st.cursor_captured.load(std::memory_order_acquire);
                if (!cap && ComputeWantCursorCapture(st)) {
                    std::lock_guard<std::mutex> lk(st.lock);
                    ApplyOsCursorCaptureLocked(st, true, true, true);
                    cap = true;
                }
                if (cap) {
                    if (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive) {
                        st.raw_dx.fetch_add(dx, std::memory_order_relaxed);
                        st.raw_dy.fetch_add(dy, std::memory_order_relaxed);
                    }
                } else if (!st.cursor_should_hide.load(std::memory_order_acquire) &&
                           !st.overlay_open.load(std::memory_order_acquire)) {
                    update_sdl_ui_pos(ev->motion.x, ev->motion.y);
                }
            }
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            KeyCode kc = KeyCode::None;
            switch (ev->button.button) {
                case SDL_BUTTON_LEFT: kc = KeyCode::MouseLeft; break;
                case SDL_BUTTON_RIGHT: kc = KeyCode::MouseRight; break;
                case SDL_BUTTON_MIDDLE: kc = KeyCode::MouseMiddle; break;
                case SDL_BUTTON_X1: kc = KeyCode::Mouse4; break;
                case SDL_BUTTON_X2: kc = KeyCode::Mouse5; break;
                default: break;
            }
            if (kc != KeyCode::None) {
                bool down = (msg == SDL_EVENT_MOUSE_BUTTON_DOWN);
                NoteMouseButtonEdge(st, kc, down);
                if (down) {
                    recover_sdl_focus();
                    MarkMkbActivity(st);
                    if (handle_rebind_key(kc)) return true;
                    if (!st.overlay_open.load(std::memory_order_acquire)) {
                        if (!st.cursor_captured.load(std::memory_order_acquire) &&
                            !st.cursor_should_hide.load(std::memory_order_acquire)) {
                            update_sdl_ui_pos(ev->button.x, ev->button.y);
                            std::lock_guard<std::mutex> lk(st.lock);
                            if (kc == KeyCode::MouseLeft) {
                                st.ui_lmb_down = true;
                                st.ui_lmb_click_pending = true;
                            } else if (kc == KeyCode::MouseRight) {
                                st.ui_rmb_click_pending = true;
                            }
                        } else if (kc == KeyCode::MouseLeft &&
                                   (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::Fmv ||
                                    st.cur_non_pointer_modal.load(std::memory_order_acquire))) {
                            std::lock_guard<std::mutex> lk(st.lock);
                            st.non_pointer_lmb_pending = true;
                        }
                    }
                } else {
                    if (kc == KeyCode::MouseLeft) {
                        std::lock_guard<std::mutex> lk(st.lock);
                        st.ui_lmb_down = false;
                    }
                    if (st.cursor_captured.load(std::memory_order_acquire)) {
                        ApplyOsCursorCaptureOnGuiThread(st, win, true, true, false);
                    }
                }
                st.key_down[size_t(kc)].store(down, std::memory_order_relaxed);
            }
            return false;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            int notches = (ev->wheel.y > 0.0f) ? 1 : (ev->wheel.y < 0.0f ? -1 : 0);
            if (notches != 0) {
                MarkMkbActivity(st);
                KeyCode wk = (notches > 0) ? KeyCode::WheelUp : KeyCode::WheelDown;
                if (!handle_rebind_key(wk)) {
                    std::lock_guard<std::mutex> lk(st.lock);
                    if (st.cursor_captured.load(std::memory_order_acquire)) {
                        if (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive) {
                            if (st.wheel_queue.size() < 8) st.wheel_queue.push_back(notches);
                            if (st.gameplay_wheel_queue.size() < 8) st.gameplay_wheel_queue.push_back(notches);
                        }
                    } else if (!st.cursor_should_hide.load(std::memory_order_acquire) &&
                               !st.overlay_open.load(std::memory_order_acquire)) {
                        st.ui_wheel_pending += notches;
                    }
                }
            }
            return false;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            KeyCode kc = SdlScancodeToKeyCode(ev->key.scancode);
            bool down = (msg == SDL_EVENT_KEY_DOWN);
            if (down) recover_sdl_focus();
            if (down && kc == KeyCode::F1 && !ev->key.repeat) {
                bool next = !st.overlay_open.load(std::memory_order_relaxed);
                st.overlay_open.store(next, std::memory_order_release);
                SyncImGuiMouseFlags(next, true);
                std::lock_guard<std::mutex> lk(st.lock);
                if (!next) {
                    st.rebind_action = -1;
                    st.rebind_slot = -1;
                    LatchHeldMouseButtonsForTransition(st);
                }
                ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
                return true;
            }
            if (kc != KeyCode::None) {
                if (down) {
                    MarkMkbActivity(st);
                    if (!ev->key.repeat && handle_rebind_key(kc)) return true;
                }
                st.key_down[size_t(kc)].store(down, std::memory_order_relaxed);
            }
            return false;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            st.window_focused.store(false, std::memory_order_release);
            for (size_t i = 0; i < size_t(KeyCode::Count); ++i)
                st.key_down[i].store(false, std::memory_order_relaxed);
            st.suppressed_mouse_mask.store(0u, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, false, false, true);
            return false;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_RESTORED: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            st.window_focused.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(st.lock);
            ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
            return false;
        }
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
            if (st.autotest_active.load(std::memory_order_relaxed)) return false;
            std::lock_guard<std::mutex> lk(st.lock);
            bool want_cap = ComputeWantCursorCapture(st);
            bool want_hide = ComputeWantHideCursor(st);
            if (want_cap || want_hide) {
                ApplyOsCursorCaptureLocked(st, want_cap, want_hide, true);
            }
            return false;
        }
        default:
            return false;
    }
#endif
}

void OnPadPoll(int port, input::Pad& pad) {
    if (port != 0) return;
    MkbState& st = St();
    if (input_rec::ShouldBypassMkb()) {
        std::lock_guard<std::mutex> lk(st.lock);
        st.frame_mkb_bypassed = true;
        return;
    }

    std::lock_guard<std::mutex> lk(st.lock);
    st.frame_mkb_bypassed = false;

    // Synthesize autotest controller pulse (used to verify Auto hot-switching mkb <-> controller)
    if (st.autotest_active.load(std::memory_order_relaxed)) {
        uint64_t ms = st.autotest_elapsed_ms.load(std::memory_order_acquire);
        for (const auto& pe : st.at_pads) {
            if (ms < pe.start_ms || ms > pe.end_ms) continue;
            pad.connected = true;
            pad.buttons |= pe.buttons;
            for (int i = 0; i < 8; ++i) pad.analog[i] = std::max(pad.analog[i], pe.analog[i]);
            if (pe.has_sticks) {
                pad.lx = pe.lx;
                pad.ly = pe.ly;
                pad.rx = pe.rx;
                pad.ry = pe.ry;
            }
        }
    }

    // Auto hot-switching: only genuine physical actuation (>0.25 radial stick, >45 trigger, or new button press edge)
    if (PadHasRealActivity(st, pad)) {
        if (st.cfg.input_mode == InputMode::Auto) {
            st.active_device.store(ActiveDevice::Controller, std::memory_order_relaxed);
        }
    }

    // Keep virtual Port 0 connected in Auto/Mkb mode (or autotest) so switching Controller -> idle -> Mkb
    // never triggers Xbox "Please reconnect controller" pause popups on controller-less PCs.
    if (st.cfg.input_mode != InputMode::Controller || st.autotest_active.load(std::memory_order_relaxed)) {
        pad.connected = true;
    }

    if (st.cfg.input_mode == InputMode::Controller) return;
    if (st.cfg.input_mode == InputMode::Auto && st.active_device.load(std::memory_order_relaxed) != ActiveDevice::Mkb) {
        return;
    }

    // CRITICAL: When M&K owns Port 0, clear physical controller idle/noise fields completely BEFORE
    // mapping M&K actions so resting DualSense Edge ADC jitter (e.g. deadzone=0) NEVER leaks into
    // XInputGetState or conflicts with LMB/WASD/mouse aim!
    pad = input::Pad{};
    pad.connected = true;
    if (st.overlay_open.load(std::memory_order_relaxed)) return;

    ActionPadState act{};
    act.phase = st.cur_phase.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size_t(Action::Count); ++i) {
        act.held[i] = IsActionHeldLocked(st, Action(i));
    }

    // Diagonal-normalized WASD (magnitude == 1.000 for cardinal & diagonals)
    float rx = (act.held[size_t(Action::MoveRight)] ? 1.0f : 0.0f) - (act.held[size_t(Action::MoveLeft)] ? 1.0f : 0.0f);
    float ry =
        (act.held[size_t(Action::MoveForward)] ? 1.0f : 0.0f) - (act.held[size_t(Action::MoveBackward)] ? 1.0f : 0.0f);
    float mag = std::hypot(rx, ry);
    if (mag > 0.0f) {
        act.wasd_x = rx / mag;
        act.wasd_y = ry / mag;
        act.wasd_mag = std::hypot(act.wasd_x, act.wasd_y);
    }
    st.frame_wasd_x = act.wasd_x;
    st.frame_wasd_y = act.wasd_y;
    st.frame_wasd_mag = act.wasd_mag;

    // Context-aware latched Escape edge (fires once per press, held cleanly for ~3 frames)
    bool esc_now = act.held[size_t(Action::Pause)];
    if (esc_now && !st.prev_esc_down) {
        st.esc_edge_pending = true;
        st.esc_hold_frames = 3;
    }
    st.prev_esc_down = esc_now;
    if (st.esc_hold_frames > 0) {
        act.esc_pulse = true;
        st.frame_esc_pulse = true;
    }

    // Queued WheelUp / WheelDown pulses
    if (st.active_wheel_dir > 0) act.wheel_up_pulse = true;
    else if (st.active_wheel_dir < 0) act.wheel_down_pulse = true;

    // Direct 1-key Psi power selection edges (1..6 -> TK/PK/MD/MC/RV/AV)
    constexpr Action kPsiActs[6] = {Action::PsiTk, Action::PsiPk, Action::PsiMd,
                                    Action::PsiMc, Action::PsiRv, Action::PsiAv};
    for (int i = 0; i < 6; ++i) {
        bool down = act.held[size_t(kPsiActs[i])];
        if (down && !st.prev_psi_down[i]) {
            st.pending_direct_psi.store(i, std::memory_order_release);
            st.selected_psi_idx.store(i, std::memory_order_release);
        }
        st.prev_psi_down[i] = down;
    }

    if (ActionMapCallback cb = st.action_cb.load(std::memory_order_acquire)) {
        cb(act, pad);
    } else {
        DefaultActionMap(act, pad);
    }

    // Apply latched mouse-menu pulses (LMB Confirm / Start, RMB Back, Wheel Up/Down, Slider Left/Right).
    // During FMV and in-game non-pointer modals (Tips / Mission Objectives), LMB dismissal pulses A / START
    // without ever unlocking or showing the OS cursor.
    const bool captured_now = st.cursor_captured.load(std::memory_order_acquire);
    const bool allow_confirm_pulses = !captured_now || (act.phase == perf::Phase::Fmv) ||
                                      st.cur_non_pointer_modal.load(std::memory_order_acquire);
    if (allow_confirm_pulses) {
        if (st.menu_confirm_hold > 0) pad.analog[0] = 0xFF;  // A
        if (st.menu_cancel_hold > 0) pad.analog[1] = 0xFF;   // B
        if (st.menu_start_hold > 0) pad.buttons |= 1u << 4;  // START
        if (!captured_now) {
            if (st.menu_nav_y_hold > 0) {
                if (st.menu_nav_y_dir < 0) pad.buttons |= 1u << 0;       // DPAD_UP
                else if (st.menu_nav_y_dir > 0) pad.buttons |= 1u << 1;  // DPAD_DOWN
            }
            if (st.menu_nav_x_hold > 0) {
                if (st.menu_nav_x_dir < 0) pad.buttons |= 1u << 2;       // DPAD_LEFT
                else if (st.menu_nav_x_dir > 0) pad.buttons |= 1u << 3;  // DPAD_RIGHT
            }
        }
    }
}

void OnPresent(uint64_t /*present_frame*/, perf::Phase phase, bool modal_ui_open,
               std::filesystem::path& out_capture_bmp, bool& out_should_exit, bool non_pointer_modal) {
    MkbState& st = St();
    const bool overlay_before = st.overlay_open.load(std::memory_order_acquire);
    SyncImGuiMouseFlags(overlay_before);
    bool overlay_after = overlay_before;
    {
        std::lock_guard<std::mutex> lk(st.lock);
        st.cur_phase.store(phase, std::memory_order_relaxed);
        st.cur_modal_ui.store(modal_ui_open, std::memory_order_relaxed);
        st.cur_non_pointer_modal.store(non_pointer_modal, std::memory_order_relaxed);
        if (phase != st.prev_present_phase) {
            // Clear latched Esc pulse across phase boundaries so Pause <-> Gameplay never double-toggles.
            st.esc_hold_frames = 0;
            if (phase != perf::Phase::GameplayActive) {
                st.gameplay_wheel_queue.clear();
                st.prev_gameplay_wheel_dir = 0;
            }
            st.prev_present_phase = phase;
        }

        // Click-through transition guard: when transitioning from menus, modals, FMVs, or the ImGui overlay
        // into active 3D gameplay, latch all currently held mouse buttons until physically released and discard
        // stale mouse delta so clicking "Resume Game" or dismissing a modal never fires a stray shot.
        const bool allow_gameplay_mouse = IsGameplayMouseButtonsAllowed(st);
        if (allow_gameplay_mouse && !st.prev_gameplay_mouse_allowed) {
            LatchHeldMouseButtonsForTransition(st);
        } else if (phase != perf::Phase::GameplayActive) {
            st.raw_dx.store(0, std::memory_order_relaxed);
            st.raw_dy.store(0, std::memory_order_relaxed);
            st.smooth_dx = 0.0f;
            st.smooth_dy = 0.0f;
        }
        st.prev_gameplay_mouse_allowed = allow_gameplay_mouse;

        // Advance in-process XBE_MKB_AUTOTEST schedule on shared stall-clamped eff_us clock
        if (st.autotest_active.load(std::memory_order_relaxed)) {
            uint64_t ms = input_rec::ElapsedEffUs() / 1000ull;
            st.autotest_elapsed_ms.store(ms, std::memory_order_release);

            // Apply scheduled set_ms toggles (skipped when replay bypasses M&K)
            const bool bypassed = input_rec::ShouldBypassMkb();
            if (!bypassed) {
                for (auto& se : st.at_sets) {
                    if (!se.triggered && ms >= se.target_ms) {
                        se.triggered = true;
                        ApplyRuntimeOptionLocked(st, se.key, se.value);
                    }
                }
            }

            // Update synthetic held keys for the upcoming frame
            std::memset(st.at_key_down, 0, sizeof(st.at_key_down));
            bool any_at_key = false;
            for (const auto& ke : st.at_keys) {
                if (ms >= ke.start_ms && ms <= ke.end_ms) {
                    for (KeyCode k : ke.keys) {
                        if (size_t(k) < size_t(KeyCode::Count)) {
                            st.at_key_down[size_t(k)] = true;
                            any_at_key = true;
                        }
                    }
                }
            }
            if (any_at_key && !bypassed) MarkMkbActivity(st);

            // Inject one-shot raw mouse count events (for next guest gameplay frame)
            for (auto& me : st.at_mouse) {
                if (!me.triggered && ms >= me.target_ms) {
                    me.triggered = true;
                    if (!bypassed) {
                        MarkMkbActivity(st);
                        st.raw_dx.fetch_add(me.dx, std::memory_order_relaxed);
                        st.raw_dy.fetch_add(me.dy, std::memory_order_relaxed);
                    }
                }
            }

            // Inject one-shot mouse wheel events
            for (auto& we : st.at_wheel) {
                if (!we.triggered && ms >= we.target_ms) {
                    we.triggered = true;
                    if (!bypassed) {
                        MarkMkbActivity(st);
                        if (st.cursor_captured.load(std::memory_order_acquire)) {
                            if (st.wheel_queue.size() < 8) st.wheel_queue.push_back(we.delta);
                            if (st.gameplay_wheel_queue.size() < 8) st.gameplay_wheel_queue.push_back(we.delta);
                        } else if (!st.overlay_open.load(std::memory_order_acquire)) {
                            st.ui_wheel_pending += we.delta;
                        }
                    }
                }
            }

            // Inject scheduled menu cursor hover & LMB/RMB click events (for pure mouse-menu autotest)
            for (auto& ce : st.at_cursors) {
                if (!ce.triggered && ms >= ce.target_ms) {
                    ce.triggered = true;
                    if (!bypassed) {
                        MarkMkbActivity(st);
                        if (ce.has_pos) {
                            float mapped_x = ce.ui_x;
                            float mapped_y = ce.ui_y;
                            if (!ce.is_client_coords ||
                                MapClientToUiSpace(ce.client_x, ce.client_y, ce.client_w, ce.client_h, mapped_x,
                                                   mapped_y)) {
                                st.ui_x = mapped_x;
                                st.ui_y = mapped_y;
                                st.ui_pos_valid = true;
                                st.ui_moved_pending = true;
                            }
                        }
                        if (ce.click_lmb) {
                            if (phase == perf::Phase::Fmv || non_pointer_modal) {
                                st.non_pointer_lmb_pending = true;
                            } else {
                                st.ui_lmb_click_pending = true;
                            }
                        }
                        if (ce.click_rmb) st.ui_rmb_click_pending = true;
                    }
                }
            }

            // Trigger scheduled captures & clean exit (only when replay is not owning exit)
            if (!bypassed) {
                bool any_pending_cap = false;
                for (auto& ce : st.at_caps) {
                    if (!ce.triggered && ms >= ce.target_ms) {
                        if (out_capture_bmp.empty()) {
                            ce.triggered = true;
                            out_capture_bmp = ce.path;
                            LOGR("mkb: capture triggered ms=%llu -> %s", (unsigned long long)ms,
                                 ce.path.string().c_str());
                        } else {
                            any_pending_cap = true;
                        }
                    } else if (!ce.triggered && st.at_exit_ms > 0 && ce.target_ms <= st.at_exit_ms) {
                        any_pending_cap = true;
                    }
                }
                if (!any_pending_cap && out_capture_bmp.empty() && st.at_exit_ms > 0 && !st.at_exit_posted &&
                    ms >= st.at_exit_ms) {
                    st.at_exit_posted = true;
                    out_should_exit = true;
                    LOGR("mkb: autotest clean exit requested at ms=%llu", (unsigned long long)ms);
                }
            }
        }

        // Advance latched Esc / wheel / menu-mouse pulse hold counters once per present
        if (st.esc_hold_frames > 0) --st.esc_hold_frames;
        if (st.menu_confirm_hold > 0) --st.menu_confirm_hold;
        if (st.menu_cancel_hold > 0) --st.menu_cancel_hold;
        if (st.menu_start_hold > 0) --st.menu_start_hold;
        if (st.menu_nav_y_hold > 0) {
            --st.menu_nav_y_hold;
            if (st.menu_nav_y_hold == 0) st.menu_nav_y_dir = 0;
        }
        if (st.menu_nav_x_hold > 0) {
            --st.menu_nav_x_hold;
            if (st.menu_nav_x_hold == 0) st.menu_nav_x_dir = 0;
        }
        if (st.wheel_hold_frames > 0) {
            --st.wheel_hold_frames;
            if (st.wheel_hold_frames == 0) st.active_wheel_dir = 0;
        }
        if (st.active_wheel_dir == 0 && !st.wheel_queue.empty()) {
            st.active_wheel_dir = st.wheel_queue.front();
            st.wheel_queue.pop_front();
            st.wheel_hold_frames = 3;
            st.frame_wheel_pulse = st.active_wheel_dir;
        }

        // Re-evaluate Smart OS Cursor Capture & Hide state every present.
        // On real (non-autotest) runs, coalesce a lightweight per-present GUI-thread check so
        // GetClipCursor, blank sprite, and post-splash focus handover self-heal within 1 frame.
        const bool want_capture = ComputeWantCursorCapture(st);
        const bool want_hide = ComputeWantHideCursor(st);
        const bool want_tick = !st.autotest_active.load(std::memory_order_relaxed) &&
                               (want_capture || want_hide || !st.window_focused.load(std::memory_order_relaxed));
        ApplyOsCursorCaptureLocked(st, want_capture, want_hide, false, want_tick);

        // Allow LMB click dismissal during FMV (skip cutscene) and in-game non-pointer modals
        // (Tutorial Tips / Mission Objectives) without ever showing or unlocking the OS cursor.
        if (st.non_pointer_lmb_pending) {
            st.non_pointer_lmb_pending = false;
            if (!st.overlay_open.load(std::memory_order_acquire) && !input_rec::ShouldBypassMkb()) {
                if (phase == perf::Phase::Fmv) {
                    st.menu_start_hold = std::max(st.menu_start_hold, 4);
                    st.menu_confirm_hold = std::max(st.menu_confirm_hold, 4);
                } else if (non_pointer_modal) {
                    st.menu_confirm_hold = std::max(st.menu_confirm_hold, 4);
                    st.non_pointer_ui_confirm_pending.store(true, std::memory_order_release);
                }
            }
        }

        if (want_capture || want_hide || st.overlay_open.load(std::memory_order_acquire) ||
            input_rec::ShouldBypassMkb()) {
            st.ui_moved_pending = false;
            st.ui_lmb_click_pending = false;
            st.ui_rmb_click_pending = false;
            st.ui_wheel_pending = 0;
            if (want_capture || want_hide) {
                st.ui_screen_name.clear();
                st.ui_hovered_idx = -1;
                st.ui_item_count = 0;
            }
        }
        overlay_after = st.overlay_open.load(std::memory_order_acquire);
    }
    if (overlay_after != overlay_before) {
        SyncImGuiMouseFlags(overlay_after, true);
    }
}

bool ConsumeCameraMouseDelta(float& out_d_yaw_rad, float& out_d_pitch_rad, int32_t* out_dx, int32_t* out_dy) {
    out_d_yaw_rad = 0.0f;
    out_d_pitch_rad = 0.0f;
    if (out_dx) *out_dx = 0;
    if (out_dy) *out_dy = 0;

    MkbState& st = St();
    int64_t dx64 = st.raw_dx.exchange(0, std::memory_order_acq_rel);
    int64_t dy64 = st.raw_dy.exchange(0, std::memory_order_acq_rel);

    std::lock_guard<std::mutex> lk(st.lock);
    const bool mkb_owns = (st.cfg.input_mode == InputMode::Mkb) ||
                          (st.cfg.input_mode == InputMode::Auto &&
                           st.active_device.load(std::memory_order_relaxed) == ActiveDevice::Mkb);
    if (input_rec::ShouldBypassMkb() || !st.cursor_captured.load(std::memory_order_relaxed) || !mkb_owns ||
        st.cur_phase.load(std::memory_order_relaxed) != perf::Phase::GameplayActive) {
        st.smooth_dx = 0.0f;
        st.smooth_dy = 0.0f;
        st.frame_mouse_dx = 0;
        st.frame_mouse_dy = 0;
        st.frame_d_yaw = 0.0f;
        st.frame_d_pitch = 0.0f;
        st.frame_mouse_consumed_by_hook = true;
        return false;
    }

    float fdx = float(dx64);
    float fdy = float(dy64);
    if (st.cfg.mouse_smoothing) {
        st.smooth_dx = 0.5f * st.smooth_dx + 0.5f * fdx;
        st.smooth_dy = 0.5f * st.smooth_dy + 0.5f * fdy;
        if (std::fabs(st.smooth_dx) < 1e-4f) st.smooth_dx = 0.0f;
        if (std::fabs(st.smooth_dy) < 1e-4f) st.smooth_dy = 0.0f;
        fdx = st.smooth_dx;
        fdy = st.smooth_dy;
    } else {
        st.smooth_dx = 0.0f;
        st.smooth_dy = 0.0f;
    }

    float rpc = RadPerCount(st.cfg.mouse_sensitivity);
    // Proven Psi-Ops right-handed Y-up signs: +dx (Right) -> negative manualAngle; +dy (Down) -> positive manualPitch
    float dyaw = -fdx * rpc;
    float dpitch = fdy * rpc * (st.cfg.mouse_invert_y ? -1.0f : 1.0f);

    st.frame_mouse_dx += int32_t(dx64);
    st.frame_mouse_dy += int32_t(dy64);
    st.frame_d_yaw += dyaw;
    st.frame_d_pitch += dpitch;
    st.frame_mouse_consumed_by_hook = true;

    if (dx64 != 0 || dy64 != 0) {
        const auto now = std::chrono::steady_clock::now();
        st.mouse_flick_history.push_back({now, int32_t(dx64), int32_t(dy64)});
        while (st.mouse_flick_history.size() > 32) {
            st.mouse_flick_history.pop_front();
        }
        while (!st.mouse_flick_history.empty() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(now - st.mouse_flick_history.front().tp).count() >
                   150) {
            st.mouse_flick_history.pop_front();
        }
    }

    out_d_yaw_rad = dyaw;
    out_d_pitch_rad = dpitch;
    if (out_dx) *out_dx = int32_t(dx64);
    if (out_dy) *out_dy = int32_t(dy64);
    return (dx64 != 0 || dy64 != 0 || dyaw != 0.0f || dpitch != 0.0f);
}

int ConsumeDirectPsiSelect() { return St().pending_direct_psi.exchange(-1, std::memory_order_acq_rel); }

int GetSelectedPsiIndex() { return std::clamp(St().selected_psi_idx.load(std::memory_order_acquire), 0, 5); }

void SetSelectedPsiIndex(int idx) { St().selected_psi_idx.store(std::clamp(idx, 0, 5), std::memory_order_release); }

bool IsActionHeld(Action a) {
    if (size_t(a) >= size_t(Action::Count)) return false;
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    if (input_rec::ShouldBypassMkb() || st.overlay_open.load(std::memory_order_relaxed)) return false;
    const bool mkb_owns = (st.cfg.input_mode == InputMode::Mkb) ||
                          (st.cfg.input_mode == InputMode::Auto &&
                           st.active_device.load(std::memory_order_relaxed) == ActiveDevice::Mkb);
    if (!mkb_owns) return false;
    return IsActionHeldLocked(st, a);
}

void PollGameplayInput(GameplayInputSnapshot& out) {
    out = GameplayInputSnapshot{};
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    out.overlay_open = st.overlay_open.load(std::memory_order_relaxed);
    if (input_rec::ShouldBypassMkb() || out.overlay_open) {
        st.gameplay_wheel_queue.clear();
        st.prev_gameplay_wheel_dir = 0;
        return;
    }
    const bool mkb_owns = (st.cfg.input_mode == InputMode::Mkb) ||
                          (st.cfg.input_mode == InputMode::Auto &&
                           st.active_device.load(std::memory_order_relaxed) == ActiveDevice::Mkb);
    if (!mkb_owns) {
        st.gameplay_wheel_queue.clear();
        st.prev_gameplay_wheel_dir = 0;
        return;
    }
    out.mkb_active = (st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive);

    // Always snapshot physical held state while M&K owns input so edge tracking stays synced across Paused -> Gameplay.
    bool any_held = false;
    for (size_t i = 0; i < size_t(Action::Count); ++i) {
        bool h = IsActionHeldLocked(st, Action(i));
        out.held[i] = h;
        any_held = any_held || h;
    }

    if (out.mkb_active) {
        // Consume 1 queued wheel notch per guest tick, inserting a 1-tick 0 gap between identical consecutive notches
        // so rapid multi-notch scrolling produces distinct rising edges.
        if (!st.gameplay_wheel_queue.empty()) {
            int32_t front_dir = st.gameplay_wheel_queue.front() > 0 ? 1 : -1;
            if (st.prev_gameplay_wheel_dir == front_dir) {
                st.prev_gameplay_wheel_dir = 0;
            } else {
                st.gameplay_wheel_queue.pop_front();
                out.wheel_delta = front_dir;
                st.prev_gameplay_wheel_dir = front_dir;
            }
        } else {
            st.prev_gameplay_wheel_dir = 0;
        }
        if (out.wheel_delta != 0) {
            KeyCode wk = (out.wheel_delta > 0) ? KeyCode::WheelUp : KeyCode::WheelDown;
            for (size_t i = 0; i < size_t(Action::Count); ++i) {
                if (st.cfg.bindings[i][0] == wk || st.cfg.bindings[i][1] == wk) {
                    out.held[i] = true;
                    any_held = true;
                }
            }
        }
    }

    // Diagonal-normalized WASD movement
    float rx = (out.held[size_t(Action::MoveRight)] ? 1.0f : 0.0f) - (out.held[size_t(Action::MoveLeft)] ? 1.0f : 0.0f);
    float ry =
        (out.held[size_t(Action::MoveForward)] ? 1.0f : 0.0f) - (out.held[size_t(Action::MoveBackward)] ? 1.0f : 0.0f);
    float mag = std::hypot(rx, ry);
    if (mag > 0.0f) {
        out.wasd_x = rx / mag;
        out.wasd_y = ry / mag;
        out.wasd_mag = std::hypot(out.wasd_x, out.wasd_y);
    }

    // Recent mouse flick velocity over last 140ms for TK Throw (0x0D)
    const auto now = std::chrono::steady_clock::now();
    while (!st.mouse_flick_history.empty() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(now - st.mouse_flick_history.front().tp).count() >
               140) {
        st.mouse_flick_history.pop_front();
    }
    float peak_spd = 0.0f;
    float sum_dx = 0.0f;
    float sum_dy = 0.0f;
    for (const auto& ms : st.mouse_flick_history) {
        float s = std::hypot(float(ms.dx), float(ms.dy));
        if (s > peak_spd) peak_spd = s;
        sum_dx += float(ms.dx);
        sum_dy += float(ms.dy);
    }
    out.recent_flick_peak = peak_spd;
    out.recent_flick_disp = std::hypot(sum_dx, sum_dy);
    out.any_key_or_mouse = any_held || (out.wheel_delta != 0) || (peak_spd > 0.0f);
}

input_rec::MkbStateSnapshot SnapshotTelemetry() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    input_rec::MkbStateSnapshot s{};
    ActiveDevice dev = st.active_device.load(std::memory_order_relaxed);
    if (st.cfg.input_mode == InputMode::Mkb) dev = ActiveDevice::Mkb;
    else if (st.cfg.input_mode == InputMode::Controller) dev = ActiveDevice::Controller;
    s.input_device = (dev == ActiveDevice::Controller) ? "controller" : "mkb";
    s.cursor_captured = st.cursor_captured.load(std::memory_order_acquire);
#ifdef _WIN32
    s.os_cursor_visible = QueryOsCursorVisibleWin32(st);
#else
    s.os_cursor_visible = st.os_cursor_visible.load(std::memory_order_acquire);
#endif
    s.overlay_open = st.overlay_open.load(std::memory_order_relaxed);
    s.window_focused = st.window_focused.load(std::memory_order_relaxed);
    s.mouse_dx = st.frame_mouse_dx;
    s.mouse_dy = st.frame_mouse_dy;
    s.d_yaw = st.frame_d_yaw;
    s.d_pitch = st.frame_d_pitch;
    s.wasd_x = st.frame_wasd_x;
    s.wasd_y = st.frame_wasd_y;
    s.wasd_mag = st.frame_wasd_mag;
    s.mouse_invert_y = st.cfg.mouse_invert_y;
    s.mouse_raw_aim = st.cfg.mouse_raw_aim;
    s.mouse_sens = st.cfg.mouse_sensitivity;
    s.cm_per_360 = CmPer360At800Dpi(st.cfg.mouse_sensitivity);
    s.wheel_pulse = st.frame_wheel_pulse;
    s.esc_pulse = st.frame_esc_pulse;
    s.mkb_hook_bypassed = st.frame_mkb_bypassed || input_rec::ShouldBypassMkb();
    s.ui_screen = st.ui_screen_name;
    s.ui_hovered_idx = st.ui_hovered_idx;
    s.ui_item_count = st.ui_item_count;

    // Clear per-frame one-shot latches so the following frame reports exact 0 when idle
    st.frame_mouse_dx = 0;
    st.frame_mouse_dy = 0;
    st.frame_d_yaw = 0.0f;
    st.frame_d_pitch = 0.0f;
    st.frame_mouse_consumed_by_hook = false;
    st.frame_wheel_pulse = 0;
    st.frame_esc_pulse = false;
    return s;
}

const Config& GetConfig() { return St().cfg; }
ActiveDevice GetActiveDevice() { return St().active_device.load(std::memory_order_relaxed); }
bool IsMkbActiveForGameplay() {
    const MkbState& st = St();
    if (input_rec::ShouldBypassMkb()) return false;
    if (st.cfg.input_mode == InputMode::Controller) return false;
    if (st.cfg.input_mode == InputMode::Auto && st.active_device.load(std::memory_order_relaxed) != ActiveDevice::Mkb)
        return false;
    return st.cur_phase.load(std::memory_order_relaxed) == perf::Phase::GameplayActive;
}
bool IsRawAimEnabled() { return St().cfg.mouse_raw_aim; }
bool IsMouseInvertY() { return St().cfg.mouse_invert_y; }
bool IsCursorCaptured() { return St().cursor_captured.load(std::memory_order_acquire); }
bool IsOsCursorVisible() {
#ifdef _WIN32
    return QueryOsCursorVisibleWin32(St());
#else
    return St().os_cursor_visible.load(std::memory_order_acquire);
#endif
}
bool IsOverlayOpen() { return St().overlay_open.load(std::memory_order_relaxed); }
void SetOverlayOpen(bool open) {
    MkbState& st = St();
    st.overlay_open.store(open, std::memory_order_release);
    SyncImGuiMouseFlags(open, true);
    std::lock_guard<std::mutex> lk(st.lock);
    if (!open) {
        st.rebind_action = -1;
        st.rebind_slot = -1;
        LatchHeldMouseButtonsForTransition(st);
    }
    st.prev_gameplay_mouse_allowed = IsGameplayMouseButtonsAllowed(st);
    ApplyOsCursorCaptureLocked(st, ComputeWantCursorCapture(st), ComputeWantHideCursor(st), true);
}
bool IsRebinding() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    return st.rebind_action >= 0;
}
void CancelRebind() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    st.rebind_action = -1;
    st.rebind_slot = -1;
}
bool IsAutotestActive() { return St().autotest_active.load(std::memory_order_relaxed); }
void SetActionMapCallback(ActionMapCallback cb) { St().action_cb.store(cb, std::memory_order_release); }

bool ConsumeNonPointerModalConfirm() {
    return St().non_pointer_ui_confirm_pending.exchange(false, std::memory_order_acq_rel);
}

bool ConsumeMenuMouseEvent(MenuMouseState& out) {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    if (input_rec::ShouldBypassMkb() || st.cursor_captured.load(std::memory_order_acquire) ||
        st.overlay_open.load(std::memory_order_acquire)) {
        return false;
    }
    if (st.cfg.input_mode == InputMode::Controller) return false;
    if (st.cfg.input_mode == InputMode::Auto && st.active_device.load(std::memory_order_relaxed) != ActiveDevice::Mkb) {
        return false;
    }
    out.x = st.ui_x;
    out.y = st.ui_y;
    out.valid = st.ui_pos_valid;
    out.moved = st.ui_moved_pending;
    out.lmb_down = st.ui_lmb_down;
    out.lmb_clicked = st.ui_lmb_click_pending;
    out.rmb_clicked = st.ui_rmb_click_pending;
    out.wheel_delta = st.ui_wheel_pending;
    st.ui_moved_pending = false;
    st.ui_lmb_click_pending = false;
    st.ui_rmb_click_pending = false;
    st.ui_wheel_pending = 0;
    return out.moved || out.lmb_clicked || out.rmb_clicked || out.wheel_delta != 0 || out.lmb_down;
}

void PulseMenuPad(bool confirm_a, bool cancel_b, bool start) {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    if (confirm_a) st.menu_confirm_hold = std::max(st.menu_confirm_hold, 4);
    if (cancel_b) st.menu_cancel_hold = std::max(st.menu_cancel_hold, 4);
    if (start) st.menu_start_hold = std::max(st.menu_start_hold, 4);
}

void ReportMenuUiTelemetry(const char* screen_name, int hovered_idx, int item_count) {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    std::string next_name = screen_name ? screen_name : "";
    if (!next_name.empty() && !st.ui_screen_name.empty() && next_name != st.ui_screen_name) {
        // Screen transitioned: cancel any latched menu pad pulse so clicks never bleed into the next screen.
        st.menu_confirm_hold = 0;
        st.menu_cancel_hold = 0;
        st.menu_start_hold = 0;
    }
    st.ui_screen_name = std::move(next_name);
    st.ui_hovered_idx = hovered_idx;
    st.ui_item_count = item_count;
}

MenuMouseState GetMenuMouseState() {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    MenuMouseState m{};
    m.x = st.ui_x;
    m.y = st.ui_y;
    m.valid = st.ui_pos_valid;
    m.lmb_down = st.ui_lmb_down;
    return m;
}

// ImGui-side rebind capture. The pre-game launcher windows don't route messages through OnWindowMessage, so
// without this a "Press key..." armed in the launcher could never complete and would leak into gameplay.
// In-game the window-message path normally wins (it consumes the key before ImGui sees it).
static KeyCode PollImGuiRebindInput() {
    struct KeyMap {
        ImGuiKey key;
        KeyCode code;
    };
    static constexpr KeyMap kKeys[] = {
        {ImGuiKey_Space, KeyCode::Space},          {ImGuiKey_Enter, KeyCode::Enter},
        {ImGuiKey_KeypadEnter, KeyCode::Enter},    {ImGuiKey_Tab, KeyCode::Tab},
        {ImGuiKey_Backspace, KeyCode::Backspace},  {ImGuiKey_LeftShift, KeyCode::LShift},
        {ImGuiKey_RightShift, KeyCode::RShift},    {ImGuiKey_LeftCtrl, KeyCode::LCtrl},
        {ImGuiKey_RightCtrl, KeyCode::RCtrl},      {ImGuiKey_LeftAlt, KeyCode::LAlt},
        {ImGuiKey_RightAlt, KeyCode::RAlt},        {ImGuiKey_UpArrow, KeyCode::Up},
        {ImGuiKey_DownArrow, KeyCode::Down},       {ImGuiKey_LeftArrow, KeyCode::Left},
        {ImGuiKey_RightArrow, KeyCode::Right},     {ImGuiKey_Minus, KeyCode::Minus},
        {ImGuiKey_Equal, KeyCode::Equals},         {ImGuiKey_LeftBracket, KeyCode::LBracket},
        {ImGuiKey_RightBracket, KeyCode::RBracket}, {ImGuiKey_Backslash, KeyCode::Backslash},
        {ImGuiKey_Semicolon, KeyCode::Semicolon},  {ImGuiKey_Apostrophe, KeyCode::Apostrophe},
        {ImGuiKey_Comma, KeyCode::Comma},          {ImGuiKey_Period, KeyCode::Period},
        {ImGuiKey_Slash, KeyCode::Slash},          {ImGuiKey_GraveAccent, KeyCode::Grave},
    };
    for (int i = 0; i < 26; ++i) {
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_A + i), false)) return KeyCode(uint16_t(KeyCode::A) + i);
    }
    for (int i = 0; i < 10; ++i) {
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + i), false)) return KeyCode(uint16_t(KeyCode::Num0) + i);
    }
    // F1 is reserved for the overlay toggle (in-game it never reaches the action map).
    for (int i = 1; i < 12; ++i) {
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_F1 + i), false)) return KeyCode(uint16_t(KeyCode::F1) + i);
    }
    for (const KeyMap& m : kKeys) {
        if (ImGui::IsKeyPressed(m.key, false)) return m.code;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return KeyCode::MouseLeft;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return KeyCode::MouseRight;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) return KeyCode::MouseMiddle;
    if (ImGui::IsMouseClicked(3)) return KeyCode::Mouse4;
    if (ImGui::IsMouseClicked(4)) return KeyCode::Mouse5;
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel > 0.0f) return KeyCode::WheelUp;
    if (wheel < 0.0f) return KeyCode::WheelDown;
    return KeyCode::None;
}

void DrawControlsSettingsPanel(bool auto_save, bool* out_dirty) {
    MkbState& st = St();
    std::lock_guard<std::mutex> lk(st.lock);
    bool dirty = false;

    // A binding stored by the window-message capture path still has to be saved.
    if (st.rebind_committed) {
        st.rebind_committed = false;
        dirty = true;
    }
    if (st.rebind_action >= 0 && ImGui::GetFrameCount() > st.rebind_arm_ui_frame) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            st.rebind_action = -1;  // Esc / gamepad B cancels without changing the binding
            st.rebind_slot = -1;
        } else if (KeyCode cap = PollImGuiRebindInput(); cap != KeyCode::None) {
            st.cfg.bindings[st.rebind_action][st.rebind_slot] = cap;
            st.rebind_action = -1;
            st.rebind_slot = -1;
            dirty = true;
        }
    }

    const char* modes[] = {"Auto (Instant Hot-Switch)", "Mouse & Keyboard", "Controller"};
    int mode_idx = std::clamp(int(st.cfg.input_mode), 0, 2);
    if (ImGui::Combo("Input Mode", &mode_idx, modes, 3)) {
        st.cfg.input_mode = InputMode(mode_idx);
        if (st.cfg.input_mode == InputMode::Mkb) st.active_device.store(ActiveDevice::Mkb, std::memory_order_relaxed);
        else if (st.cfg.input_mode == InputMode::Controller) {
            st.active_device.store(ActiveDevice::Controller, std::memory_order_relaxed);
            st.rebind_action = -1;
            st.rebind_slot = -1;
        }
        dirty = true;
    }

    if (ImGui::SliderFloat("Mouse Sensitivity", &st.cfg.mouse_sensitivity, 0.10f, 5.00f, "%.2f")) dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f cm/360 @ 800 DPI)", CmPer360At800Dpi(st.cfg.mouse_sensitivity));

    if (ImGui::Checkbox("PC Mouse Invert Y (Decoupled from Gamepad)", &st.cfg.mouse_invert_y)) dirty = true;
    if (ImGui::Checkbox("Raw 1:1 Aim (Bypass Sticky-Aim Slowdown)", &st.cfg.mouse_raw_aim)) dirty = true;
    if (ImGui::Checkbox("Mouse Smoothing", &st.cfg.mouse_smoothing)) dirty = true;

    int rumble = int(st.cfg.rumble_strength);
    if (ImGui::SliderInt("Rumble Strength (%)", &rumble, 0, 100)) {
        st.cfg.rumble_strength = uint32_t(std::clamp(rumble, 0, 100));
        input::SetRumbleStrength(st.cfg.rumble_strength);
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Test Rumble")) {
        input::TestRumblePulse(0, 0xA000u, 0x8000u, 250u);
    }

    int dz_l = int(st.cfg.controller_deadzone_left);
    int dz_r = int(st.cfg.controller_deadzone_right);
    if (ImGui::SliderInt("Left Stick Deadzone (%)", &dz_l, 0, 40)) {
        st.cfg.controller_deadzone_left = uint32_t(std::clamp(dz_l, 0, 40));
        input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
        dirty = true;
    }
    if (ImGui::SliderInt("Right Stick Deadzone (%)", &dz_r, 0, 40)) {
        st.cfg.controller_deadzone_right = uint32_t(std::clamp(dz_r, 0, 40));
        input::SetDeadzones(st.cfg.controller_deadzone_left, st.cfg.controller_deadzone_right);
        dirty = true;
    }
    if (ImGui::Checkbox("DualSense Touchpad -> Back", &st.cfg.dualsense_touchpad_back)) {
        input::SetTouchpadAsBack(st.cfg.dualsense_touchpad_back);
        dirty = true;
    }

    ImGui::Separator();
    if (ImGui::Button("Reset Default Bindings")) {
        ResetDefaultBindings(st.cfg);
        st.rebind_action = -1;
        st.rebind_slot = -1;
        dirty = true;
    }

    if (ImGui::BeginTable("ControlsBindings", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();

        // While a capture is pending the table is modal: keyboard-nav Space/Enter or a click must bind the
        // pending slot, not re-arm/clear another button on the same frame.
        const bool capturing = (st.rebind_action >= 0);
        ImGui::BeginDisabled(capturing);
        for (int i = 0; i < int(Action::Count); ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kActionTable[i].label);

            for (int slot = 0; slot < 2; ++slot) {
                ImGui::TableSetColumnIndex(1 + slot);
                bool waiting = (st.rebind_action == i && st.rebind_slot == slot);
                const char* tok = KeyCodeToToken(st.cfg.bindings[i][slot]);
                std::string btn_label = waiting ? "Press key... (Esc / B cancels)" : (tok[0] ? tok : "---");
                btn_label += slot == 0 ? "##p" : "##s";
                if (ImGui::Button(btn_label.c_str(), ImVec2(-1.0f, 0.0f)) && !capturing) {
                    st.rebind_action = i;
                    st.rebind_slot = slot;
                    st.rebind_arm_frame = input_rec::CurrentPresentFrame();
                    st.rebind_arm_ui_frame = ImGui::GetFrameCount();
                }
            }
            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("Clear##c", ImVec2(-1.0f, 0.0f)) && !capturing) {
                st.cfg.bindings[i][0] = KeyCode::None;
                st.cfg.bindings[i][1] = KeyCode::None;
                if (st.rebind_action == i) {
                    st.rebind_action = -1;
                    st.rebind_slot = -1;
                }
                dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    if (dirty) {
        if (out_dirty) *out_dirty = true;
        if (auto_save) app::Save();
    }
}

void DrawOverlayUi(double fps) {
    MkbState& st = St();
    if (!st.overlay_open.load(std::memory_order_relaxed)) return;
    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Psi-Ops Controls (F1)", &open)) {
        ImGui::Text("FPS: %.1f | Active Input: %s", fps,
                    GetActiveDevice() == ActiveDevice::Controller ? "Controller" : "Mouse & Keyboard");
        ImGui::Separator();
        DrawControlsSettingsPanel();
    }
    ImGui::End();
    if (!open) {
        SetOverlayOpen(false);
    }
}

}  // namespace mkb
