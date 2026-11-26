// SPDX-License-Identifier: GPL-2.0-or-later
// Cross-platform Raw Mouse & Keyboard host layer, Smart OS Cursor Capture,
// portable OS-independent [controls] / [controls.bindings] persistence,
// 4-column Click-to-Rebind ImGui helper, and in-process XBE_MKB_AUTOTEST driver.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "common/input_recorder.h"
#include "core/input.h"
#include "core/perf.h"

namespace mkb {

enum class InputMode : uint8_t {
    Auto = 0,
    Mkb = 1,
    Controller = 2,
};

enum class ActiveDevice : uint8_t {
    Mkb = 0,
    Controller = 1,
};

// Portable OS-independent key/mouse token enum (never serialized as VK_* or SDL_SCANCODE_*).
enum class KeyCode : uint16_t {
    None = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space, Enter, Escape, Tab, Backspace,
    LShift, RShift, LCtrl, RCtrl, LAlt, RAlt,
    Up, Down, Left, Right,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Minus, Equals, LBracket, RBracket, Backslash,
    Semicolon, Apostrophe, Comma, Period, Slash, Grave,
    MouseLeft, MouseRight, MouseMiddle, Mouse4, Mouse5,
    WheelUp, WheelDown,
    Count
};

enum class Action : uint8_t {
    MoveForward = 0,
    MoveBackward,
    MoveLeft,
    MoveRight,
    Fire,
    FineAim,
    Reload,
    Jump,
    Crouch,
    WallMove,
    Interact,
    Melee,
    PsiUse,
    WeaponNext,
    WeaponPrev,
    PsiTk,
    PsiPk,
    PsiMd,
    PsiMc,
    PsiRv,
    PsiAv,
    TkCamControl,
    McExplode,
    Pause,
    Objectives,
    Count
};

struct Config {
    InputMode input_mode = InputMode::Auto;
    float mouse_sensitivity = 1.0f;
    bool mouse_invert_y = false;
    bool mouse_raw_aim = true;
    bool mouse_smoothing = false;
    uint32_t rumble_strength = 100;
    uint32_t controller_deadzone_left = 15;
    uint32_t controller_deadzone_right = 15;
    bool dualsense_touchpad_back = true;
    KeyCode bindings[size_t(Action::Count)][2] = {};
};

// Portable token conversion ("W", "LMB", "RMB", "WheelUp", "Escape", etc.).
const char* KeyCodeToToken(KeyCode k);
KeyCode TokenToKeyCode(std::string_view token);
const char* ActionToKey(Action a);
const char* ActionLabel(Action a);

// Sensitivity math (algebraic 1:1 rad/count <-> cm/360 @ 800 DPI).
float RadPerCount(float sensitivity);
float CmPer360At800Dpi(float sensitivity);

// Lifecycle & app::kGame hook wiring.
void Init();
void Shutdown();
void LoadSettings(const void* toml_table);
void SaveSettings(void* toml_table);
void OnWindowCreated(void* window_handle);
bool OnWindowMessage(void* window_handle, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void OnPadPoll(int port, input::Pad& pad);
void OnPresent(uint64_t present_frame, perf::Phase phase, bool modal_ui_open, std::filesystem::path& out_capture_bmp,
               bool& out_should_exit, bool non_pointer_modal = false);
void DrawOverlayUi(double fps);

// Query state for Psi-Ops binary-verified camera/input hooks.
const Config& GetConfig();
ActiveDevice GetActiveDevice();
bool IsMkbActiveForGameplay();
bool IsRawAimEnabled();
bool IsMouseInvertY();
bool IsCursorCaptured();
bool IsOsCursorVisible();
bool IsOverlayOpen();
void SetOverlayOpen(bool open);
bool IsAutotestActive();

// Menu pointer state in Psi-Ops 512x465 (y_off=+10) virtual UI space (updated on WM_MOUSEMOVE / SDL / autotest).
struct MenuMouseState {
    float x = 256.0f;
    float y = 222.5f;
    bool valid = false;
    bool moved = false;
    bool lmb_down = false;
    bool lmb_clicked = false;
    bool rmb_clicked = false;
    int32_t wheel_delta = 0;
};
// Consumed on the guest main thread inside Hook_HudUpdate; returns true if menu input should be processed.
bool ConsumeMenuMouseEvent(MenuMouseState& out);
// Consumed once by Hook_HudUpdate when LMB is clicked during an in-game non-pointer modal (Tips / Objectives).
bool ConsumeNonPointerModalConfirm();
// Pulses Xbox pad A / B / Start for screens driven by XInput (e.g. AttractMode / FMV skip).
void PulseMenuPad(bool confirm_a, bool cancel_b, bool start);
// Reports active UI screen / hovered widget telemetry for --state-log JSONL verification.
void ReportMenuUiTelemetry(const char* screen_name, int hovered_idx, int item_count);
MenuMouseState GetMenuMouseState();

// Consumes raw pixel delta once for the current gameplay frame (zero deadzone, dt-independent, 0.000 stop-drift).
// Returns true if non-zero mouse delta was consumed.
bool ConsumeCameraMouseDelta(float& out_d_yaw_rad, float& out_d_pitch_rad, int32_t* out_dx = nullptr,
                             int32_t* out_dy = nullptr);

// Query direct 1-key Psi power selection request (-1 if none, else 0..5 for TK/PK/MD/MC/RV/AV).
int ConsumeDirectPsiSelect();

// Active Psi power slot (0=TK, 1=PK, 2=MD, 3=MC, 4=RV, 5=AV) used by Action::PsiUse (LShift/Mouse5).
int GetSelectedPsiIndex();
void SetSelectedPsiIndex(int idx);

// Per-guest-frame decoupled M&K snapshot queried inside InputManager::Update (0x00294130).
struct GameplayInputSnapshot {
    bool mkb_active = false;
    bool overlay_open = false;
    bool held[size_t(Action::Count)] = {};
    int32_t wheel_delta = 0;       // +1 WheelUp / -1 WheelDown / 0 none (1 guest tick per notch)
    bool any_key_or_mouse = false; // for idle timer reset (0x86)
    float wasd_x = 0.0f;           // diagonal-normalized [-1..+1]
    float wasd_y = 0.0f;
    float wasd_mag = 0.0f;
    float recent_flick_peak = 0.0f; // peak single-frame raw mouse speed over last 140ms (TK Throw 0x0D)
    float recent_flick_disp = 0.0f; // net raw mouse displacement over last 140ms (TK Throw 0x0D)
};
void PollGameplayInput(GameplayInputSnapshot& out);
bool IsActionHeld(Action a);

// Snapshot M&K telemetry for unified --state-log JSONL output.
input_rec::MkbStateSnapshot SnapshotTelemetry();

// Action-to-Xbox-pad callback so Psi-Ops's exact binary-verified CONTROL LAYOUT maps cleanly.
struct ActionPadState {
    bool held[size_t(Action::Count)] = {};
    bool wheel_up_pulse = false;
    bool wheel_down_pulse = false;
    bool esc_pulse = false;
    float wasd_x = 0.0f;
    float wasd_y = 0.0f;
    float wasd_mag = 0.0f;
    perf::Phase phase = perf::Phase::TitleMenu;
};
using ActionMapCallback = void (*)(const ActionPadState& act, input::Pad& pad);
void SetActionMapCallback(ActionMapCallback cb);

// Controls UI helper (4-column Click-to-Rebind table + sliders), embeddable in Launcher and F1 Overlay.
void DrawControlsSettingsPanel(bool auto_save = true, bool* out_dirty = nullptr);

// Click-to-rebind lifecycle: a pending "Press key..." capture is cancelled whenever the Controls panel stops
// being shown (tab switch, overlay close, launcher exit) so it can never swallow a later gameplay key.
// Must not be called from inside DrawControlsSettingsPanel.
bool IsRebinding();
void CancelRebind();

}  // namespace mkb
