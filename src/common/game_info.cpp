// SPDX-License-Identifier: GPL-2.0-or-later
// Psi-Ops: The Mindgate Conspiracy (Original Xbox, Title ID 0x4D570016)

#include "app/app.h"
#include "common/input_recorder.h"
#include "common/launcher_ui.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "common/ui_common.h"
#include "common/update_checker.h"
#include "common/version.h"
#include "core/input.h"
#include "core/perf.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "upscale/upscaler_manager.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

static_assert(PSIOPS_VERSION_MAJOR == 1 && PSIOPS_VERSION_MINOR == 1 && PSIOPS_VERSION_PATCH == 0 &&
                  std::string_view(PSIOPS_VERSION_STR) == "1.1.0",
              "Keep game_info.cpp, version.h, and CMakeLists.txt synchronized (v1.1.0)");

uint32_t PsiOpsLiveCameraVa();
uint64_t PsiOpsConsumeMkbActionsFired();
uint32_t PsiOpsGetMkbReloadCount();

namespace {

// Bounds check for guest pointer chains read by telemetry/camera callbacks (same contract as hooks.cpp ValidGuest):
// rejects null / first-64 KiB (null + field offset) pointers, 32-bit wrap-around, and spans whose first or last
// byte is not committed. These run on host threads while the guest mutates the objects, so check the full span.
bool ValidGuest(uint32_t va, uint32_t bytes) {
    if (kernel::g_base == nullptr || bytes == 0u || va < mem::kUserLow) return false;
    if (va > 0xFFFFFFFFu - (bytes - 1u)) return false;
    if (!mem::IsCommitted(va)) return false;
    return bytes == 1u || mem::IsCommitted(va + (bytes - 1u));
}

float ReadGuestF32(uint32_t va) {
    uint32_t raw = kernel::R32(va);
    float f = 0.0f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

void WriteGuestF32(uint32_t va, float v) {
    uint32_t raw = 0;
    std::memcpy(&raw, &v, sizeof(raw));
    kernel::W32(va, raw);
}

// Binary-verified Psi-Ops (XDK 5849, default.xbe) native 16:9 square-pixel aspect override:
// Cancels the NTSC 10/11 CRT non-square PAR multiplier (0x002F6C76..0x002F6C89) so
// BuildProj (0x002F5840) computes exact 16:9 (1.777778) and MWSFD FMVs (0x0026D0D0) stay 4:3 pillarboxed.
void ApplyPsiOpsAspectFix() {
    if (!ValidGuest(0x005DEFC8u, 4u) || !ValidGuest(0x005CCBD0u, 1u) || !ValidGuest(0x005CCBCCu, 4u)) {
        return;
    }
    uint32_t renderer = kernel::R32(0x005DEFC8u);
    if (!ValidGuest(renderer + 0x10u, 4u)) return;
    if (app::Widescreen() && kernel::G<uint8_t>(0x005CCBD0u) != 0u) {
        const float target_par = app::Get().aspect_correct_render ? (4.0f / 3.0f) : ((4.0f / 3.0f) * (10.0f / 11.0f));
        float cur_r = ReadGuestF32(renderer + 0x10u);
        float cur_g = ReadGuestF32(0x005CCBCCu);
        if (std::isfinite(cur_r) && cur_r > 0.5f && std::fabs(cur_r - target_par) > 1e-4f) {
            WriteGuestF32(renderer + 0x10u, target_par);
        }
        if (std::isfinite(cur_g) && cur_g > 0.5f && std::fabs(cur_g - target_par) > 1e-4f) {
            WriteGuestF32(0x005CCBCCu, target_par);
        }
    }
}

struct PsiOpsRawPhase {
    perf::PhaseInfo pi{};
    uint32_t app = 0;
    uint32_t state = 0;
    uint32_t world = 0;
    uint32_t player = 0;
    uint32_t hud = 0;
    bool status_ready = false;
    bool in_shell = true;
    bool fmv_active = false;
    bool pause_open = false;
    bool non_pointer_modal = false;
    bool modal_ui_open = false;
    double sim_s = 0.0;
};

const char* PhaseToString(perf::Phase p) {
    switch (p) {
        case perf::Phase::Fmv: return "fmv";
        case perf::Phase::TitleMenu: return "title_menu";
        case perf::Phase::LoadingTransition: return "loading_transition";
        case perf::Phase::GameplayActive: return "gameplay_active";
        case perf::Phase::Paused: return "paused";
        default: return "title_menu";
    }
}

// Binary-verified Psi-Ops (XDK 5849, default.xbe) GameApp, HUD modal UI & timing telemetry VAs.
PsiOpsRawPhase ReadPsiOpsPhaseRaw() {
    PsiOpsRawPhase out{};
    if (!ValidGuest(0x00586A04u, 4u)) return out;

    // When menus are confirmed via PC mouse (CallSendUiEvent 0x3F2) without a physical controller button held,
    // Psi-Ops's BindActivePort (sub_00294500) scans ports 3..0 with strict '>' against -1.0f, so Port 3 (disconnected)
    // wins the 0.0f tie and swaps into active_player_port[0] (0x0055673C), muting all subsequent FMV skip & gameplay
    // input on Port 0. Restore Port 0 whenever M&K owns Port 0 or Player 0 is bound to an invalid/disconnected port.
    if (ValidGuest(0x0055673Cu, 16u)) {
        uint32_t p0 = kernel::R32(0x0055673Cu);
        bool p0_disconnected =
            (p0 >= 4u) ||
            (ValidGuest(0x005DED18u + p0 * 0x94u + 0x14u, 1u) && kernel::G<uint8_t>(0x005DED18u + p0 * 0x94u + 0x14u) == 0u);
        bool mkb_owns = (mkb::GetActiveDevice() == mkb::ActiveDevice::Mkb &&
                         mkb::GetConfig().input_mode != mkb::InputMode::Controller);
        if (p0 != 0u && (p0_disconnected || mkb_owns)) {
            for (uint32_t slot = 1u; slot < 4u; ++slot) {
                if (kernel::R32(0x0055673Cu + slot * 4u) == 0u) {
                    kernel::W32(0x0055673Cu + slot * 4u, p0 < 4u ? p0 : slot);
                }
            }
            kernel::W32(0x0055673Cu, 0u);
        }
    }

    uint32_t app = kernel::R32(0x00586A04u);
    if (!ValidGuest(app, 0xD8u)) return out;  // fields read below span [app+0x38, app+0xD8)
    out.app = app;

    float dt_s = ReadGuestF32(app + 0x38u);
    if (std::isfinite(dt_s) && dt_s >= 0.0f && dt_s < 5.0f) out.pi.g_dt_ms = dt_s * 1000.0f;

    out.state = kernel::R32(app + 0x70u);
    out.player = kernel::R32(app + 0xD0u);
    out.world = kernel::R32(app + 0xD4u);

    double real_s = double(kernel::R64(app + 0x40u)) + double(ReadGuestF32(app + 0x48u));
    double sim_s = double(kernel::R64(app + 0x50u)) + double(ReadGuestF32(app + 0x58u));
    out.sim_s = std::isfinite(sim_s) ? sim_s : 0.0;

    out.fmv_active = ValidGuest(0x005C4198u, 4u) && ValidGuest(0x005C4250u, 4u) &&
                     kernel::R32(0x005C4198u) != 0u && kernel::R32(0x005C4250u) == 0u;
    out.status_ready = ValidGuest(0x005ED2B8u, 1u) && ((kernel::G<uint8_t>(0x005ED2B8u) & 1u) != 0u);
    out.in_shell = !out.status_ready || !ValidGuest(0x005ECEA7u, 1u) || (kernel::G<uint8_t>(0x005ECEA7u) != 0u);

    bool hud_modal_lock_on = false;
    bool objectives_open_on = false;
    if (ValidGuest(0x005CCB90u, 4u)) {
        uint32_t hud = kernel::R32(0x005CCB90u);
        if (ValidGuest(hud + 0x324u, 0x98u)) {  // [hud+0x324] .. [hud+0x3BB]
            out.hud = hud;
            uint8_t hud_modal_lock = kernel::G<uint8_t>(hud + 0x324u);
            uint8_t pause_open = kernel::G<uint8_t>(hud + 0x3B4u);
            uint8_t objectives_open = kernel::G<uint8_t>(hud + 0x3B5u);
            out.pause_open = (pause_open != 0u);
            hud_modal_lock_on = (hud_modal_lock != 0u);
            objectives_open_on = (objectives_open != 0u);
        }
    }

    if (ValidGuest(0x005DEFC8u, 4u)) {
        uint32_t swap_obj = kernel::R32(0x005DEFC8u);
        if (ValidGuest(swap_obj + 0x1Cu, 4u)) out.pi.g_frame = kernel::R32(swap_obj + 0x1Cu);
    }

    if (out.fmv_active) {
        out.pi.phase = perf::Phase::Fmv;
        out.pi.g_elapsed_s = std::isfinite(real_s) ? float(real_s) : 0.0f;
    } else if (out.in_shell) {
        out.pi.phase = perf::Phase::TitleMenu;
        out.pi.g_elapsed_s = std::isfinite(real_s) ? float(real_s) : 0.0f;
    } else if (out.world == 0u || out.state == 8u) {
        out.pi.phase = perf::Phase::LoadingTransition;
        out.pi.g_elapsed_s = std::isfinite(real_s) ? float(real_s) : 0.0f;
    } else if (out.state == 0u) {
        out.pi.phase = perf::Phase::GameplayActive;
        out.pi.g_elapsed_s = float(out.sim_s);
    } else {
        out.pi.phase = perf::Phase::Paused;
        out.pi.g_elapsed_s = float(out.sim_s);
    }
    // app->state == 4 is Psi-Ops's in-game Tutorial Tip / Mission Objectives overlay (screen='Tip', 0 spatial
    // buttons, dismissed by A / LMB). Keep sim_s accounting in perf::Phase::Paused while treating it as a
    // non-pointer modal so the OS cursor NEVER unlocks or pops up mid-gameplay.
    const bool in_tip_modal = !out.fmv_active && !out.in_shell && (out.world != 0u) && (out.state == 4u) &&
                              !out.pause_open;
    out.non_pointer_modal = in_tip_modal || hud_modal_lock_on || objectives_open_on;
    // Only unlock/show the OS menu cursor when an actual point-and-click menu is open
    // (TitleMenu, Pause Menu state==6 / SavePrompt, or hud+0x3B4 pause_open).
    out.modal_ui_open = (out.pi.phase == perf::Phase::TitleMenu) || out.pause_open ||
                        (out.pi.phase == perf::Phase::Paused && !in_tip_modal);
    return out;
}

perf::PhaseInfo QueryPsiOpsPhase() { return ReadPsiOpsPhaseRaw().pi; }

// Binary-verified Psi-Ops 3D Camera extraction for temporal upscalers (DLSS 4, FSR 3.1, XeSS 2):
//   renderer        = R32(0x005DEFC8)
//   [renderer+0x24] = View (4x4 row-major row-vector RH, looking down -Z)
//   [renderer+0xA4] = Proj (4x4 row-major unjittered OpenGL RH, Z in [-1,+1])
//   g_active_camera = R32(0x005CC6E8) (fovY_rad = F32(cam+0x80), cam_world = R32(cam+0x24))
gpu::CameraFrameData QueryPsiOpsCamera() {
    ApplyPsiOpsAspectFix();
    PsiOpsRawPhase raw = ReadPsiOpsPhaseRaw();
    gpu::CameraFrameData out{};
    out.guest_frame = raw.pi.g_frame;
    out.dt_ms = (raw.pi.g_dt_ms > 0.5f && raw.pi.g_dt_ms < 200.0f) ? raw.pi.g_dt_ms : 16.667f;
    out.is_3d_gameplay = ((raw.pi.phase == perf::Phase::GameplayActive) || (raw.state == 4u)) &&
                         (raw.world != 0u) && !raw.in_shell && !raw.fmv_active && !raw.pause_open;
    if (!out.is_3d_gameplay || !ValidGuest(0x005DEFC8u, 4u)) return out;

    uint32_t renderer = kernel::R32(0x005DEFC8u);
    if (!ValidGuest(renderer + 0x24u, 0xC0u)) return out;  // View [+0x24] and Proj [+0xA4] matrices

    float v_guest[16];
    float p_guest[16];
    for (int i = 0; i < 16; ++i) {
        v_guest[i] = ReadGuestF32(renderer + 0x24u + uint32_t(i * 4));
        p_guest[i] = ReadGuestF32(renderer + 0xA4u + uint32_t(i * 4));
        if (!std::isfinite(v_guest[i]) || !std::isfinite(p_guest[i])) return out;
    }
    // Sanity-check perspective projection (m00 > 0, m11 > 0, m23 ~ -1)
    if (p_guest[0] <= 0.05f || p_guest[5] <= 0.05f || std::fabs(p_guest[11] + 1.0f) > 0.25f) return out;

    // Transpose guest row-vector matrices into column-vector convention (clip = proj_rev * view * world)
    float p_col[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.view[r * 4 + c] = v_guest[c * 4 + r];
            p_col[r * 4 + c] = p_guest[c * 4 + r];
        }
    }
    // Remap OpenGL Z_gl in [-1,+1] -> XboxXBERecomp host D3D12 reversed-Z d_host = 0.5 - 0.5 * Z_gl (1 near, 0 far)
    std::memcpy(out.proj_rev, p_col, sizeof(p_col));
    for (int c = 0; c < 4; ++c) {
        out.proj_rev[2 * 4 + c] = 0.5f * p_col[3 * 4 + c] - 0.5f * p_col[2 * 4 + c];
    }

    // Extract camera world forward (-Z axis of view matrix) and world position
    out.cam_fwd[0] = -v_guest[2];
    out.cam_fwd[1] = -v_guest[6];
    out.cam_fwd[2] = -v_guest[10];

    bool got_pos = false;
    uint32_t active_cam = ValidGuest(0x005CC6E8u, 4u) ? kernel::R32(0x005CC6E8u) : 0u;
    out.session = active_cam;
    if (ValidGuest(active_cam + 0x24u, 0x60u)) {  // [cam+0x24] world .. [cam+0x80] fovY
        float fov = ReadGuestF32(active_cam + 0x80u);
        if (std::isfinite(fov) && fov > 0.1f && fov < 2.8f) out.fov_y_rad = fov;
        else out.fov_y_rad = 2.0f * std::atan(1.0f / p_guest[5]);

        uint32_t cam_world = kernel::R32(active_cam + 0x24u);
        if (ValidGuest(cam_world + 0x30u, 0xCu)) {
            float cx = ReadGuestF32(cam_world + 0x30u);
            float cy = ReadGuestF32(cam_world + 0x34u);
            float cz = ReadGuestF32(cam_world + 0x38u);
            if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cz)) {
                out.cam_pos[0] = cx;
                out.cam_pos[1] = cy;
                out.cam_pos[2] = cz;
                got_pos = true;
            }
        }
    } else {
        out.fov_y_rad = 2.0f * std::atan(1.0f / p_guest[5]);
    }
    if (!got_pos) {
        // Derive world position from orthogonal View matrix: pos = -R^T * t
        float tx = v_guest[12], ty = v_guest[13], tz = v_guest[14];
        out.cam_pos[0] = -(v_guest[0] * tx + v_guest[1] * ty + v_guest[2] * tz);
        out.cam_pos[1] = -(v_guest[4] * tx + v_guest[5] * ty + v_guest[6] * tz);
        out.cam_pos[2] = -(v_guest[8] * tx + v_guest[9] * ty + v_guest[10] * tz);
    }

    out.near_plane = 0.1f;
    out.valid = true;
    return out;
}

bool IsSniperScopeActive() {
    uint32_t cam = PsiOpsLiveCameraVa();
    if (!ValidGuest(cam + 0x37Cu, 4u)) return false;
    float sniper_timer = ReadGuestF32(cam + 0x37Cu);
    return std::isfinite(sniper_timer) && sniper_timer > 0.8f;
}

// Binary-verified Psi-Ops CONTROL LAYOUT (Config 1 default, adapts dynamically to Config 2/3 via 0x005ECEB4).
void PsiOpsActionMap(const mkb::ActionPadState& act, input::Pad& pad) {
    using mkb::Action;
    if (act.phase == perf::Phase::Fmv) {
        if (act.esc_pulse || act.held[size_t(Action::Interact)] || act.held[size_t(Action::Jump)] ||
            act.held[size_t(Action::Fire)]) {
            pad.buttons |= 1u << 4;  // START skips FMV/cutscene
        }
        return;
    }

    if (act.phase == perf::Phase::TitleMenu || act.phase == perf::Phase::Paused) {
        if (act.held[size_t(Action::MoveForward)]) pad.buttons |= 1u << 0;   // DPAD_UP
        if (act.held[size_t(Action::MoveBackward)]) pad.buttons |= 1u << 1;  // DPAD_DOWN
        if (act.held[size_t(Action::MoveLeft)]) pad.buttons |= 1u << 2;      // DPAD_LEFT
        if (act.held[size_t(Action::MoveRight)]) pad.buttons |= 1u << 3;     // DPAD_RIGHT
        if (act.held[size_t(Action::Interact)] || act.held[size_t(Action::Jump)]) {
            pad.analog[0] = 0xFF;  // A confirms menu item / dismisses Tip
            if (act.phase == perf::Phase::TitleMenu) pad.buttons |= 1u << 4;  // START advances Title Screen
        }
        if (act.held[size_t(Action::Objectives)]) {
            pad.buttons |= 1u << 5;  // BACK closes Mission Objectives (0x22) in UI mode 0x16
        }
        if (act.esc_pulse) {
            if (act.phase == perf::Phase::Paused) pad.buttons |= 1u << 4;  // START resumes gameplay
            else pad.analog[1] = 0xFF;  // B goes back in frontend menus
        }
        return;
    }

    // Active 3D gameplay pad fallback (InputManager::GetActionValue @ 0x294160 decouples every gameplay action
    // directly by Action ID in modes 2/3/4; keep pad bits clean & accurate for any passthrough / mode transition).
    uint32_t cfg = 2u;
    if (ValidGuest(0x005ECEB4u, 4u)) {
        uint32_t v = kernel::R32(0x005ECEB4u);
        if (v >= 2u && v <= 4u) cfg = v;
    }

    if (act.wasd_mag > 0.0f) {
        pad.lx = int16_t(std::clamp(std::lround(act.wasd_x * 32767.0f), -32768L, 32767L));
        pad.ly = int16_t(std::clamp(std::lround(act.wasd_y * 32767.0f), -32768L, 32767L));
    }

    // Fire (RT)
    if (act.held[size_t(Action::Fire)]) pad.analog[7] = 0xFF;

    // Face & Stick buttons (config-aware, strictly fixing Jump != X and Reload != DPad_Down)
    if (act.held[size_t(Action::Interact)]) {
        pad.analog[0] = 0xFF;  // A = Interact (0x1F) in all configs
    }
    if (act.held[size_t(Action::Jump)]) {
        if (cfg == 3u) pad.analog[1] = 0xFF;       // B in Config 2
        else if (cfg == 4u) pad.analog[3] = 0xFF;  // Y in Config 3
        else pad.analog[0] = 0xFF;                 // A in Config 1 (NEVER X!)
    }
    if (act.held[size_t(Action::Melee)]) {
        pad.analog[2] = 0xFF;  // X = Melee (0x20) ONLY
    }
    if (act.held[size_t(Action::Crouch)]) {
        if (cfg == 3u) pad.buttons |= 1u << 6;  // LTHUMB in Config 2
        else pad.analog[1] = 0xFF;              // B in Config 1/3
    }
    if (act.held[size_t(Action::WallMove)]) {
        if (cfg == 4u) pad.analog[1] = 0xFF;    // B in Config 3
        else pad.buttons |= 1u << 6;            // LTHUMB in Config 1/2
    }
    if (act.held[size_t(Action::FineAim)]) {
        if (cfg == 4u) pad.buttons |= 1u << 6;  // LTHUMB in Config 3
        else pad.analog[3] = 0xFF;              // Y in Config 1/2
    }

    // Weapon Cycle / Sniper Scope Zoom (Reload R is handled directly by Weapon::Reload, NEVER DPAD_DOWN!)
    if (act.wheel_up_pulse || act.wheel_down_pulse) {
        if (IsSniperScopeActive()) {
            if (act.wheel_up_pulse) pad.buttons |= 1u << 0;    // DPAD_UP = Sniper Zoom In (0x34)
            if (act.wheel_down_pulse) pad.buttons |= 1u << 1;  // DPAD_DOWN = Sniper Zoom Out (0x35)
        } else {
            if (cfg == 2u) pad.buttons |= 1u << 1;  // DPAD_DOWN = Cycle Weapon (0x18)
            else pad.analog[4] = 0xFF;              // BLACK
        }
    }

    // Active Psi power (LShift/Mouse5 -> selected Psi 0..5) + direct 1..6 hotkeys
    const int sel_psi = mkb::GetSelectedPsiIndex();
    auto press_psi = [&](int idx) {
        switch (idx) {
            case 0: pad.analog[6] = 0xFF; break;                                                // LT = TK
            case 1: if (cfg == 2u) pad.analog[5] = 0xFF; else pad.buttons |= 1u << 0; break;    // PK
            case 2: if (cfg == 2u) pad.analog[4] = 0xFF; else pad.analog[5] = 0xFF; break;      // MD
            case 3: if (cfg == 2u) pad.buttons |= 1u << 0; else pad.buttons |= 1u << 1; break;  // MC
            case 4: pad.buttons |= 1u << 2; break;                                              // DPAD_LEFT = RV
            case 5: pad.buttons |= 1u << 3; break;                                              // DPAD_RIGHT = AV
            default: break;
        }
    };
    if (act.held[size_t(Action::PsiUse)]) press_psi(sel_psi);
    for (int i = 0; i < 6; ++i) {
        if (act.held[size_t(Action::PsiTk) + size_t(i)]) press_psi(i);
    }

    // Tab -> BACK (kept continuous across Gameplay -> Objectives mode 0x16 so holding Tab never bounces closed)
    if (act.held[size_t(Action::Objectives)]) pad.buttons |= 1u << 5;
}

input_rec::GuestStateSnapshot QueryPsiOpsGuestState() {
    PsiOpsRawPhase raw = ReadPsiOpsPhaseRaw();
    input_rec::GuestStateSnapshot s{};
    s.phase = PhaseToString(raw.pi.phase);
    s.state = raw.state;
    s.in_shell = raw.in_shell ? 1 : 0;
    s.fmv_active = raw.fmv_active ? 1 : 0;
    s.sim_elapsed_s = raw.sim_s;
    s.g_frame = raw.pi.g_frame;

    // Guest Invert-Y option (GlobalOption[8] @ 0x005ECD5C: 0=Xbox default inverted, 1=normal)
    if (raw.status_ready && ValidGuest(0x005ECD5Cu, 4u)) {
        s.has_guest_invert_y = true;
        s.guest_invert_y = (kernel::R32(0x005ECD5Cu) == 0u);
    }

    uint32_t psi_fn = 0u;
    uint32_t player = raw.player;
    if (!player && ValidGuest(raw.hud + 0x3B8u, 4u)) player = kernel::R32(raw.hud + 0x3B8u);
    if (ValidGuest(player + 0x70u, 0x71Cu)) {  // [player+0x70] .. [player+0x78B]
        // Player world position (Transform @ [player+0x70] -> vec3 at +0x04/+0x08/+0x0C)
        uint32_t tr = kernel::R32(player + 0x70u);
        if (ValidGuest(tr + 0x04u, 0xCu)) {
            float px = ReadGuestF32(tr + 0x04u);
            float py = ReadGuestF32(tr + 0x08u);
            float pz = ReadGuestF32(tr + 0x0Cu);
            if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz) && std::fabs(px) < 1e6f &&
                std::fabs(py) < 1e6f && std::fabs(pz) < 1e6f) {
                s.has_player_pos = true;
                s.player_x = px;
                s.player_y = py;
                s.player_z = pz;
            }
        }

        // Player health / psi ([player+0x770], [player+0x778])
        float hp = ReadGuestF32(player + 0x770u);
        float psi = ReadGuestF32(player + 0x778u);
        if (std::isfinite(hp) && std::isfinite(psi) && hp >= -100.0f && hp <= 100000.0f && psi >= -100.0f &&
            psi <= 100000.0f) {
            s.has_vitals = true;
            s.health = hp;
            s.psi = psi;
        }

        // Active Weapon clip, clip_size, reserve ammo & weapon_type
        uint32_t wpn = kernel::R32(player + 0x6FCu);
        if (!wpn) {
            uint32_t holder = kernel::R32(player + 0x788u);
            if (ValidGuest(holder + 0x4u, 4u)) wpn = kernel::R32(holder + 0x4u);
        }
        if (ValidGuest(wpn + 0x10u, 0x44u)) {  // [wpn+0x10] type .. [wpn+0x50]
            int32_t clip = int32_t(kernel::R32(wpn + 0x48u));
            int32_t clip_size = int32_t(kernel::R32(wpn + 0x4Cu));
            int32_t wtype = int32_t(kernel::R32(wpn + 0x10u));
            int32_t reserve = 0;
            uint32_t reserve_ptr = kernel::R32(wpn + 0x44u);
            if (ValidGuest(reserve_ptr, 4u)) reserve = int32_t(kernel::R32(reserve_ptr));
            if (clip >= 0 && clip < 10000 && reserve >= 0 && reserve < 100000) {
                s.has_vitals = true;
                s.ammo_clip = clip;
                s.ammo_reserve = reserve;
                if (clip_size > 0 && clip_size < 1000) s.ammo_clip_size = clip_size;
                if (wtype >= 0 && wtype < 64) s.weapon_type = wtype;
            }
        }

        // Active Psi power state fn + AnimStateMachine state + Melee timer + Lock-On state
        uint32_t psi_mgr = kernel::R32(player + 0x6F4u);
        if (ValidGuest(psi_mgr + 0x4u, 4u)) psi_fn = kernel::R32(psi_mgr + 0x4u);

        s.has_player_actions = true;
        s.psi_fn = psi_fn;
        s.psi_selected = mkb::GetSelectedPsiIndex();
        s.sniper_scoped = IsSniperScopeActive();
        s.mkb_actions_fired = PsiOpsConsumeMkbActionsFired();
        s.mkb_reload_count = PsiOpsGetMkbReloadCount();
        {
            uint32_t asm_ptr = kernel::R32(player + 0x438u);
            if (ValidGuest(asm_ptr + 0x44u, 4u)) {
                s.anim_state = kernel::R32(asm_ptr + 0x44u);
            }
        }
        if (ValidGuest(player + 0x85Cu, 4u)) {
            float mt = ReadGuestF32(player + 0x85Cu);
            if (std::isfinite(mt) && mt >= 0.0f && mt < 1e6f) s.melee_timer = mt;
        }
        {
            s.lock_on_active = (kernel::R32(player + 0x740u) != 0u) || (kernel::G<uint8_t>(player + 0x710u) != 0u);
        }
    }

    // Camera yaw/pitch/mode from exact live Camera* captured in Camera::ApplyManualAim (edi)
    uint32_t cam = PsiOpsLiveCameraVa();
    if (ValidGuest(cam + 0x2D8u, 0xACu)) {  // [cam+0x2D8] yaw .. [cam+0x383]
        float cyaw = ReadGuestF32(cam + 0x2D8u);
        float cpitch = ReadGuestF32(cam + 0x2DCu);
        if (std::isfinite(cyaw) && std::isfinite(cpitch) && std::fabs(cyaw) < 1e4f && std::fabs(cpitch) < 1e4f) {
            s.has_camera = true;
            s.cam_yaw = cyaw;
            s.cam_pitch = cpitch;
            float sniper_timer = ReadGuestF32(cam + 0x37Cu);
            if (psi_fn == 0x00115C80u || psi_fn == 0x00116630u) s.cam_mode = 2u;  // Remote View
            else if (psi_fn == 0x00114D80u || psi_fn == 0x00115160u || psi_fn == 0x001153E0u)
                s.cam_mode = 3u;  // Mind Control
            else if (psi_fn == 0x00116BE0u || psi_fn == 0x00117380u || psi_fn == 0x001174F0u)
                s.cam_mode = 4u;  // Telekinesis
            else if (std::isfinite(sniper_timer) && sniper_timer > 0.8f)
                s.cam_mode = 1u;  // Sniper Scope
            else s.cam_mode = 0u;  // Standard 3rd-person
        }
    }

    return s;
}

void OnPsiOpsPresent(uint64_t present_frame, std::filesystem::path& out_capture_bmp, bool& out_should_exit) {
    ApplyPsiOpsAspectFix();
    // 1. Advance shared eff_us clock, advance deterministic replay, and log frame telemetry first
    //    (captures consumed mouse_dx/dy & d_yaw/d_pitch from the frame just rendered).
    input_rec::OnPresent(present_frame, out_capture_bmp, out_should_exit);
    // 2. Advance M&K autotest schedule and update Smart OS Cursor Capture for the upcoming frame.
    PsiOpsRawPhase raw = ReadPsiOpsPhaseRaw();
    mkb::OnPresent(present_frame, raw.pi.phase, raw.modal_ui_open, out_capture_bmp, out_should_exit,
                   raw.non_pointer_modal);
}

void OnPsiOpsPad(int port, input::Pad& pad) {
    settings_ui::OnPadPoll(port, pad);
    if (input_rec::IsReplayActive()) {
        input_rec::OnPadPoll(port, pad);
        return;
    }
    mkb::OnPadPoll(port, pad);
    input_rec::OnPadPoll(port, pad);
}

void OnPsiOpsRumble(int port, uint16_t& left, uint16_t& right) {
    input_rec::OnRumble(port, left, right);
}

void OnPsiOpsLoadSettings(const void* toml_table) {
    mkb::LoadSettings(toml_table);
    ui_common::LoadUpdatePrefs(toml_table);
}

void OnPsiOpsSaveSettings(void* toml_table) {
    mkb::SaveSettings(toml_table);
    ui_common::SaveUpdatePrefs(toml_table);
}

void OnPsiOpsShutdown() {
    psiops::ShutdownUpdateChecker();
    mkb::Shutdown();
    input_rec::Shutdown();
}

void OnPsiOpsInit() {
    perf::SetPhaseCallback(QueryPsiOpsPhase);
    upscale::RegisterUpscaler();
    gpu::SetCameraCallback(QueryPsiOpsCamera);
    input_rec::Init();
    mkb::Init();
    mkb::SetActionMapCallback(PsiOpsActionMap);
    settings_ui::Init();
    input_rec::SetGuestStateCallback(QueryPsiOpsGuestState);
    input_rec::SetMkbStateCallback(mkb::SnapshotTelemetry);
    input::SetPresentCallback(OnPsiOpsPresent);
    input::SetShutdownCallback(OnPsiOpsShutdown);
}

}  // namespace

const app::GameInfo app::kGame = {
    .title = L"Psi-Ops: The Mindgate Conspiracy",
    .short_title = L"Psi-Ops",
    .name = L"PsiOps",
    .log = "psiops",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Provide Psi-Ops: The Mindgate Conspiracy (USA) as a read-only Xbox disc image (.xiso.iso / .iso) via --iso, settings.toml (iso_path), or inside the assets/ or game folder.",
    .images = {{"default.xbe", "PsiOps"}},
    .options = {},
    .load = OnPsiOpsLoadSettings,
    .save = OnPsiOpsSaveSettings,
    .init = OnPsiOpsInit,
    .loaded = nullptr,
    .window = mkb::OnWindowCreated,
    .message = mkb::OnWindowMessage,
    .ui = settings_ui::Draw,
    .pad = OnPsiOpsPad,
    .rumble = OnPsiOpsRumble,
    .releases = {},
    .expected_title_id = 0x4D570016u,
    .cs_aliases = {},
    .single_core_guest = true,
    .launcher = launcher_ui::RunLauncher,
};
