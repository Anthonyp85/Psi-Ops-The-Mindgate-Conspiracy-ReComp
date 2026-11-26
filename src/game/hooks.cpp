// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific function hooks for Psi-Ops: The Mindgate Conspiracy (default.xbe, Title ID 0x4D570016).
// All addresses and ABIs are binary-verified against retail USA default.xbe (XDK 5849).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "common/input_recorder.h"
#include "common/mkb_input.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr float kPitchClampRad = 1.2217305f;  // +-70.0 deg (0x3F9C61AA @ 0x0051F974)

float ReadF32(const uint8_t* base, uint32_t va) {
    float v = 0.0f;
    std::memcpy(&v, base + va, sizeof(v));
    return v;
}

void WriteF32(uint8_t* base, uint32_t va, float v) { std::memcpy(base + va, &v, sizeof(v)); }

uint32_t ReadU32(const uint8_t* base, uint32_t va) {
    uint32_t v = 0;
    std::memcpy(&v, base + va, sizeof(v));
    return v;
}

void WriteU32(uint8_t* base, uint32_t va, uint32_t v) { std::memcpy(base + va, &v, sizeof(v)); }

uint8_t ReadU8(const uint8_t* base, uint32_t va) { return base[va]; }

// Bounds check for every guest pointer a hook dereferences: rejects null / first-64 KiB (null + field offset)
// pointers, 32-bit wrap-around, and any [va, va + bytes) whose first or last byte is not committed guest memory.
// Callers pass the full span they touch (base + highest field + size), not a single probe byte.
bool ValidGuest(uint32_t va, uint32_t bytes = 4u) {
    if (kernel::g_base == nullptr || bytes == 0u || va < mem::kUserLow) return false;
    if (va > 0xFFFFFFFFu - (bytes - 1u)) return false;
    if (!mem::IsCommitted(va)) return false;
    return bytes == 1u || mem::IsCommitted(va + (bytes - 1u));
}

// Re-entering recompiled guest code from inside a hook.
// XboxXBERecomp models the guest x87 FPU purely in software inside X86Context (TOP, FCW, FSW, ST0-7, with MMX in
// mm[8]); it is never mirrored into the host FPU / MXCSR (no fldcw/fesetround in the runtime), so host C++ float
// math inside a hook cannot disturb it. What CAN corrupt the x87 stack, rounding/precision control or registers that
// the interrupted guest function is still using is calling back into guest code from the hook: the callee may leave
// values pushed, move TOP, reload FCW, or clobber GPRs/flags/XMM at a mid-function hook site.
// GuestCallFrame snapshots the whole context on entry (a superset of a SavedX87State: GPRs, flags, x87
// TOP/FCW/FSW/ST0-7, MMX, XMM, MXCSR) and restores it when the scope exits normally. If the guest call unwinds via a
// host exception (x86_icall throws X86Resume for guest C++ catch continuations), the restore is skipped: the frame
// that catches it owns esp/ctx from then on, and rolling ctx back underneath it would desync the guest stack.
// The scratch window below esp that the guest callee will use is validated before anything is written.
class GuestCallFrame {
public:
    static constexpr uint32_t kScratchBytes = 0x400u;  // pushed args + callee frame headroom below guest esp
    static constexpr uint32_t kDummyReturn = 0xFFFFFFF0u;

    GuestCallFrame(X86Context& ctx, uint8_t* base)
        : ctx_(ctx),
          base_(base),
          saved_(ctx),
          exceptions_(std::uncaught_exceptions()),
          ok_(ValidGuest(ctx.esp - kScratchBytes, kScratchBytes)) {}
    ~GuestCallFrame() {
        if (std::uncaught_exceptions() == exceptions_) ctx_ = saved_;
    }
    GuestCallFrame(const GuestCallFrame&) = delete;
    GuestCallFrame& operator=(const GuestCallFrame&) = delete;

    bool ok() const { return ok_; }
    void Push(uint32_t v) {
        if (!ok_) return;
        if (saved_.esp - ctx_.esp >= kScratchBytes / 2u) {
            ok_ = false;  // never run a guest call with a half-built argument list
            return;
        }
        ctx_.esp -= 4u;
        WriteU32(base_, ctx_.esp, v);
    }
    // Pushes a dummy return address (recompiled `ret` returns to the host) and runs the guest function.
    void Call(uint32_t fn) {
        if (!ok_) return;
        Push(kDummyReturn);
        x86_icall(ctx_, base_, fn);
    }

private:
    X86Context& ctx_;
    uint8_t* base_;
    X86Context saved_;
    int exceptions_;
    bool ok_;
};

// Saved guest LookSpring (GameStatus+0x240 @ 0x005ECEB8) so controller mode restores user's preference.
std::atomic<bool> g_look_spring_overridden{false};
std::atomic<uint32_t> g_saved_look_spring{1u};
// Last pitch delta consumed by Camera::ApplyManualAim on this frame, shared with Player::UpdateTKPitch during TK.
std::atomic<float> g_pending_tk_pitch{0.0f};
// Exact live Camera* VA observed in Camera::ApplyManualAim (shared with game_info.cpp telemetry).
std::atomic<uint32_t> g_live_camera_va{0u};
// True while Nick holds the Sniper Rifle ([wpn+0x10]==3) or is actively sniper-scoped (shared with AutoSteer bypass).
std::atomic<bool> g_im_sniper_weapon_or_scoped{false};
// Telemetry accumulated from Hook_InputManagerGetActionValue / Hook_InputManagerUpdate for --state-log.
std::atomic<uint64_t> g_mkb_actions_fired_mask{0u};
std::atomic<uint32_t> g_mkb_reload_count{0u};

void SyncLookSpring(uint8_t* base, bool mkb_active) {
    constexpr uint32_t kLookSpringVa = 0x005ECEB8u;
    if (!ValidGuest(kLookSpringVa, 4u)) return;
    if (mkb_active) {
        if (!g_look_spring_overridden.exchange(true, std::memory_order_acq_rel)) {
            g_saved_look_spring.store(ReadU32(base, kLookSpringVa), std::memory_order_relaxed);
        }
        uint32_t zero = 0u;
        std::memcpy(base + kLookSpringVa, &zero, sizeof(zero));
    } else if (g_look_spring_overridden.exchange(false, std::memory_order_acq_rel)) {
        uint32_t orig = g_saved_look_spring.load(std::memory_order_relaxed);
        std::memcpy(base + kLookSpringVa, &orig, sizeof(orig));
    }
}

}  // namespace

// CRI ADX (ADXXB Ver.1.49) priority-ceiling spin-blocker thread at 0x003FFCF0..0x003FFD12.
// ADXM_Unlock (0x003FFC20) calls SuspendThread on this thread, but XboxXBERecomp uses cooperative
// SuspendPoint() checks inside kernel calls; because 0x003FFD00..0x003FFD12 makes zero kernel calls,
// checking kernel::SuspendPoint() here lets SuspendThread park it cleanly on both Windows and Linux.
void Hook_AdxSpinYield(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    kernel::SuspendPoint();
    std::this_thread::yield();
}

// Camera::ApplyManualAim @ 0x0004EED0
// Covers 3rd-person, Sniper Scope / 1st-person zoom, Mind Control (MC), and TK camera look.
// ABI at entry: edi = Camera*, [esp+4] = float yaw_in, [esp+8] = float pitch_in, [esp+0xC] = float dt (ret 0xC).
// Inside 0x0004EED0, guest computes manualAngle += yaw_in * yaw_spd * dt and manualPitch += pitch_in * pitch_spd * dt.
// Dividing raw mouse rad deltas by (spd * dt) cancels dt and guest stick speed algebraically -> exact dt-independent
// 1:1 mouse aim with 0 deadzone, 0.000 stop-drift, decoupled PC Invert-Y, and preserved +-70 deg clamp & quaternion.
uint32_t PsiOpsLiveCameraVa() { return g_live_camera_va.load(std::memory_order_acquire); }

void Hook_CameraApplyManualAim(X86Context& ctx, uint8_t* base) {
    // Camera fields used here and by game_info.cpp telemetry span [cam+0x2D8, cam+0x384).
    constexpr uint32_t kCameraSpan = 0x384u;
    if (ValidGuest(ctx.edi, kCameraSpan)) {
        g_live_camera_va.store(ctx.edi, std::memory_order_release);
    }
    if (input_rec::ShouldBypassMkb()) {
        SyncLookSpring(base, false);
        return;
    }
    const bool mkb_active = mkb::IsMkbActiveForGameplay();
    SyncLookSpring(base, mkb_active);
    if (!mkb_active) return;

    if (!ValidGuest(ctx.esp + 0x4u, 0xCu)) return;  // yaw_in, pitch_in, dt
    float dt = ReadF32(base, ctx.esp + 0xCu);
    if (!std::isfinite(dt) || dt <= 1e-6f || dt > 1.0f) return;

    float dyaw = 0.0f;
    float dpitch = 0.0f;
    if (!mkb::ConsumeCameraMouseDelta(dyaw, dpitch)) {
        g_pending_tk_pitch.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Optional Sniper Scope FOV sensitivity scaling (20 deg -> 4 deg zoom)
    float fov_scale = 1.0f;
    uint32_t cam = ctx.edi;
    if (ValidGuest(cam + 0x37Cu, 8u)) {
        float sniper_timer = ReadF32(base, cam + 0x37Cu);
        if (std::isfinite(sniper_timer) && sniper_timer > 0.8f) {
            float zoom = std::clamp(ReadF32(base, cam + 0x380u), 0.0f, 1.0f);
            float fov_deg = 20.0f - 16.0f * zoom;
            fov_scale = fov_deg / 60.0f;
        }
    }
    dyaw *= fov_scale;
    dpitch *= fov_scale;
    g_pending_tk_pitch.store(dpitch, std::memory_order_relaxed);

    // Query live guest yaw_spd (0x000DCDE0) and pitch_spd (0x000DCE00) so we cancel them cleanly
    float yaw_spd = 1.10330f;
    float pitch_spd = 1.08460f;
    if (ValidGuest(0x005ECD70u, 8u) && ValidGuest(0x005CD288u, 8u)) {
        float y_div = ReadF32(base, 0x005CD288u);
        float p_div = ReadF32(base, 0x005CD28Cu);
        int32_t opt13 = int32_t(ReadU32(base, 0x005ECD70u));
        int32_t opt14 = int32_t(ReadU32(base, 0x005ECD74u));
        if (std::isfinite(y_div) && std::fabs(y_div) > 1e-4f) yaw_spd = float(opt13 + 15) / y_div;
        if (std::isfinite(p_div) && std::fabs(p_div) > 1e-4f) pitch_spd = float(opt14 + 15) / p_div;
    }
    if (std::fabs(yaw_spd) < 1e-4f) yaw_spd = 1.10330f;
    if (std::fabs(pitch_spd) < 1e-4f) pitch_spd = 1.08460f;

    float yaw_in = ReadF32(base, ctx.esp + 0x4u);
    float pitch_in = ReadF32(base, ctx.esp + 0x8u);
    if (!std::isfinite(yaw_in)) yaw_in = 0.0f;
    if (!std::isfinite(pitch_in)) pitch_in = 0.0f;

    WriteF32(base, ctx.esp + 0x4u, yaw_in + dyaw / (yaw_spd * dt));
    WriteF32(base, ctx.esp + 0x8u, pitch_in + dpitch / (pitch_spd * dt));
}

// PsiPower::RemoteViewUpdate -> call QuatFromEuler @ 0x00115E3C
// Stack at call site: [esp+0] = float d_pitch_rad, [esp+4] = float d_yaw_rad, [esp+8] = float roll.
void Hook_RemoteViewAim(X86Context& ctx, uint8_t* base) {
    if (input_rec::ShouldBypassMkb() || !mkb::IsMkbActiveForGameplay()) return;
    if (!ValidGuest(ctx.esp, 8u)) return;  // d_pitch, d_yaw

    float dyaw = 0.0f;
    float dpitch = 0.0f;
    if (!mkb::ConsumeCameraMouseDelta(dyaw, dpitch)) return;

    float p = ReadF32(base, ctx.esp + 0x0u);
    float y = ReadF32(base, ctx.esp + 0x4u);
    if (!std::isfinite(p)) p = 0.0f;
    if (!std::isfinite(y)) y = 0.0f;
    WriteF32(base, ctx.esp + 0x0u, p + dpitch);
    WriteF32(base, ctx.esp + 0x4u, y + dyaw);
}

// Player::UpdateTKPitch @ 0x000FC680
// ABI at entry: edi = Player*, [esp+4] = float dt (ret 4).
// Adds mouse pitch delta directly into TK held-object elevation [player+0x744] clamped to +-70 deg when TK is active.
void Hook_UpdateTKPitch(X86Context& ctx, uint8_t* base) {
    if (input_rec::ShouldBypassMkb() || !mkb::IsMkbActiveForGameplay()) return;
    uint32_t player = ctx.edi;
    if (!ValidGuest(player + 0x6F4u, 0x54u)) return;  // [player+0x6F4] psi_mgr .. [player+0x744] elevation

    // Only apply when Telekinesis psi state is active ({0x116BE0, 0x117380, 0x1174F0})
    uint32_t psi_mgr = ReadU32(base, player + 0x6F4u);
    if (!ValidGuest(psi_mgr, 8u)) return;
    uint32_t psi_fn = ReadU32(base, psi_mgr + 0x4u);
    if (psi_fn != 0x00116BE0u && psi_fn != 0x00117380u && psi_fn != 0x001174F0u) return;

    float dpitch = g_pending_tk_pitch.exchange(0.0f, std::memory_order_acq_rel);
    if (dpitch == 0.0f) return;

    float cur = ReadF32(base, player + 0x744u);
    if (!std::isfinite(cur)) cur = 0.0f;
    WriteF32(base, player + 0x744u, std::clamp(cur - dpitch, -kPitchClampRad, kPitchClampRad));
}

// Camera::AutoSteerToPoint @ 0x0004F4F0 (with on_true = "ret" -> ret 0x1C).
// ABI at entry: ecx = Camera*, [esp+4] = speed, [esp+8..0x14] = vec4 tgt,
//               [esp+0x18] = float* out_yaw, [esp+0x1C] = float* out_pitch.
// When M&K is active with Raw 1:1 Aim enabled, zeroes *out_yaw/*out_pitch and returns early to bypass auto-steer
// (except while the player explicitly holds RMB/V Target Lock-On on a non-sniper weapon).
bool Hook_CameraAutoSteerBypass(X86Context& ctx, uint8_t* base) {
    if (input_rec::ShouldBypassMkb() || !mkb::IsMkbActiveForGameplay() || !mkb::IsRawAimEnabled()) {
        return false;
    }
    if (mkb::IsActionHeld(mkb::Action::FineAim) && !g_im_sniper_weapon_or_scoped.load(std::memory_order_relaxed)) {
        return false;
    }
    if (ValidGuest(ctx.esp + 0x18u, 8u)) {
        uint32_t out_yaw_va = ReadU32(base, ctx.esp + 0x18u);
        uint32_t out_pitch_va = ReadU32(base, ctx.esp + 0x1Cu);
        if (ValidGuest(out_yaw_va, 4u)) WriteF32(base, out_yaw_va, 0.0f);
        if (ValidGuest(out_pitch_va, 4u)) WriteF32(base, out_pitch_va, 0.0f);
    }
    return true;
}

namespace {

constexpr uint32_t kFourccObj = 0x4F424A20u;   // 'OBJ '
constexpr uint32_t kFourccGrop = 0x47524F50u;  // 'GROP'
constexpr uint32_t kFourccPoly = 0x504F4C59u;  // 'POLY'
constexpr uint32_t kFourccText = 0x54455854u;  // 'TEXT'

constexpr uint32_t kVtblList = 0x004FED10u;
constexpr uint32_t kVtblSldp = 0x004FECB8u;
constexpr uint32_t kVtblTransformNode = 0x004FEF84u;

constexpr uint32_t kUiEvtGainFocus = 0x3ECu;
constexpr uint32_t kUiEvtLoseFocus = 0x3EDu;
constexpr uint32_t kUiEvtUp = 0x3EEu;
constexpr uint32_t kUiEvtDown = 0x3EFu;
constexpr uint32_t kUiEvtLeft = 0x3F0u;
constexpr uint32_t kUiEvtRight = 0x3F1u;
constexpr uint32_t kUiEvtConfirm = 0x3F2u;
constexpr uint32_t kUiEvtBack = 0x3F3u;
constexpr uint32_t kUiEvtStart = 0x81Au;

bool IsUiBusy(const uint8_t* base, uint32_t ui_mgr) {
    // Mirrors UIActionQueue::IsBusy @ 0x00250DE0(ui_mgr + 0x22C).
    if (!ValidGuest(ui_mgr + 0x234u, 4u)) return false;
    uint32_t node = ReadU32(base, ui_mgr + 0x234u);
    for (int i = 0; i < 256 && ValidGuest(node, 0x3Cu); ++i) {
        if (ReadU32(base, node + 0x4u) != 0u && ReadU32(base, node + 0xCu) != 0u) return true;
        node = ReadU32(base, node + 0x38u);
    }
    return false;
}

void CallSetFocusedChild(X86Context& ctx, uint8_t* base, uint32_t active_parent, uint32_t child, uint32_t ui_mgr) {
    GuestCallFrame frame(ctx, base);
    if (!frame.ok()) return;
    ctx.ebx = active_parent;
    ctx.edi = child;
    ctx.esi = 0u;
    frame.Push(1u);      // [esp+8] = send_events
    frame.Push(ui_mgr);  // [esp+4] = ui_mgr
    frame.Call(0x0024CAD0u);
}

void EnsurePlayerPortBound(uint8_t* base) {
    if (!ValidGuest(0x0055673Cu, 16u)) return;
    uint32_t p0 = ReadU32(base, 0x0055673Cu);
    if (p0 == 0u) return;
    bool p0_disconnected =
        (p0 >= 4u) || (ValidGuest(0x005DED18u + p0 * 0x94u + 0x14u, 1u) && ReadU8(base, 0x005DED18u + p0 * 0x94u + 0x14u) == 0u);
    bool mkb_owns =
        (mkb::GetActiveDevice() == mkb::ActiveDevice::Mkb && mkb::GetConfig().input_mode != mkb::InputMode::Controller);
    if (p0_disconnected || mkb_owns) {
        for (uint32_t slot = 1u; slot < 4u; ++slot) {
            if (ReadU32(base, 0x0055673Cu + slot * 4u) == 0u) {
                WriteU32(base, 0x0055673Cu + slot * 4u, p0 < 4u ? p0 : slot);
            }
        }
        WriteU32(base, 0x0055673Cu, 0u);
    }
}

void CallSendUiEvent(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t event_id) {
    {
        GuestCallFrame frame(ctx, base);
        if (!frame.ok()) return;
        ctx.esi = ui_mgr;
        ctx.eax = 0u;
        ctx.edx = 0u;
        frame.Push(event_id);  // [esp+4] = event_id
        frame.Call(0x0024DDC0u);
    }
    EnsurePlayerPortBound(base);
}

void CallSldpStep(X86Context& ctx, uint8_t* base, uint32_t sldp, int32_t steps) {
    if (steps == 0 || !ValidGuest(sldp, 4u)) return;
    uint32_t fn = (steps > 0) ? 0x0012D3E0u : 0x0012D490u;
    uint32_t count = uint32_t(steps > 0 ? steps : -steps);
    GuestCallFrame frame(ctx, base);
    if (!frame.ok()) return;
    ctx.ecx = sldp;
    frame.Push(count);
    frame.Call(fn);
}

void CallListStep(X86Context& ctx, uint8_t* base, uint32_t list_obj, int32_t steps) {
    if (steps == 0 || !ValidGuest(list_obj, 4u)) return;
    uint32_t fn = (steps > 0) ? 0x0012C270u : 0x0012C3F0u;
    int32_t n = std::clamp(steps > 0 ? steps : -steps, 1, 8);
    for (int32_t i = 0; i < n; ++i) {
        GuestCallFrame frame(ctx, base);
        if (!frame.ok()) return;
        ctx.ecx = list_obj;
        frame.Push(1u);
        frame.Call(fn);
    }
}

void MulMat4(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
                               a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void ComputeWorldMat(const uint8_t* base, uint32_t xform, float out[16]) {
    static constexpr float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(out, kIdent, sizeof(kIdent));
    // Unwrap CUIObject's [obj+0x48] transform wrapper -> TransformNode* ([wrapper+0x04], vtable 0x004FEF84),
    // matching UIChild::UpdateTransform @ 0x001358F7 -> TransformNode::GetWorldMatrix @ 0x001319F0.
    uint32_t cur = xform;
    if (ValidGuest(cur, 0x0Cu) && ReadU32(base, cur) != kVtblTransformNode) {
        uint32_t inner = ReadU32(base, cur + 0x4u);
        if (ValidGuest(inner, 0x0Cu) && ReadU32(base, inner) == kVtblTransformNode) {
            cur = inner;
        }
    }
    for (int depth = 0; depth < 32 && ValidGuest(cur, 0x10u); ++depth) {
        uint32_t m_va = ReadU32(base, cur + 0x4u);
        if (ValidGuest(m_va, 64u)) {
            float m[16];
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                m[i] = ReadF32(base, m_va + uint32_t(i) * 4u);
                if (!std::isfinite(m[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                float tmp[16];
                MulMat4(out, m, tmp);
                std::memcpy(out, tmp, sizeof(tmp));
            }
        }
        cur = ReadU32(base, cur + 0x8u);
    }
}

void TransformUiPt(const float m[16], float x, float y, float& ox, float& oy) {
    ox = x * m[0] + y * m[4] + m[12];
    oy = x * m[1] + y * m[5] + m[13];
}

bool ObjectHasEvent(const uint8_t* base, uint32_t data, uint32_t want_evt) {
    if (!ValidGuest(data, 0x1Cu)) return false;
    uint32_t evt_arr = ReadU32(base, data + 0x10u);
    if (!ValidGuest(evt_arr, 4u)) return false;
    uint32_t count = ReadU32(base, evt_arr);
    if (count == 0u || count > 128u || !ValidGuest(evt_arr + 4u, count * 4u)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, evt_arr + 4u + i * 4u);
        if (ValidGuest(entry, 4u) && ReadU32(base, entry) == want_evt) return true;
    }
    return false;
}

struct UiBounds {
    float x0 = 1e9f;
    float y0 = 1e9f;
    float x1 = -1e9f;
    float y1 = -1e9f;
    bool valid = false;

    void Include(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        if (x < -512.0f || x > 1024.0f || y < -512.0f || y > 1024.0f) return;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
        valid = true;
    }

    bool Contains(float x, float y) const { return valid && x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float Area() const { return valid ? std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0) : 1e18f; }
};

void AccumObjectBounds(const uint8_t* base, uint32_t obj, uint32_t inherited_xform, UiBounds& out, int depth) {
    if (depth > 8 || !ValidGuest(obj, 0x4Cu)) return;
    uint32_t data = ReadU32(base, obj + 0x1Cu);
    if (!ValidGuest(data, 0x1Cu)) return;
    uint32_t children_arr = ReadU32(base, data + 0x18u);
    if (!ValidGuest(children_arr, 4u)) return;
    uint32_t count = ReadU32(base, children_arr);
    if (count == 0u || count > 256u || !ValidGuest(children_arr + 4u, count * 4u)) return;

    // Mirror CUIObject::Render @ 0x0024C660: inherit ancestor TransformNode when [obj+0x48] is NULL.
    uint32_t eff_xform = inherited_xform;
    uint32_t own_xform = ReadU32(base, obj + 0x48u);
    if (ValidGuest(own_xform, 0x10u)) eff_xform = own_xform;

    float wm[16];
    ComputeWorldMat(base, eff_xform, wm);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, children_arr + 4u + i * 4u);
        if (!ValidGuest(entry, 0x30u)) continue;
        uint32_t fourcc = ReadU32(base, entry);
        uint32_t child_ptr = ReadU32(base, entry + 0x8u);
        if (fourcc == kFourccPoly) {
            UiBounds pb{};
            bool got_vb = false;
            if (ValidGuest(child_ptr, 0x20u)) {
                uint32_t vb = ReadU32(base, child_ptr + 0x1Cu);
                float ox = ReadF32(base, child_ptr + 0x10u);
                float oy = ReadF32(base, child_ptr + 0x14u);
                if (!std::isfinite(ox)) ox = 0.0f;
                if (!std::isfinite(oy)) oy = 0.0f;
                if (ValidGuest(vb, 0x28u + 4u * 0x28u)) {
                    for (uint32_t k = 0; k < 4u; ++k) {
                        float lx = ReadF32(base, vb + 0x28u + k * 0x28u) + ox;
                        float ly = ReadF32(base, vb + 0x28u + k * 0x28u + 4u) + oy;
                        float wx, wy;
                        TransformUiPt(wm, lx, ly, wx, wy);
                        pb.Include(wx, wy);
                    }
                    got_vb = pb.valid;
                }
            }
            if (!got_vb && ValidGuest(entry, 0x10u + 4u * 12u)) {
                for (uint32_t k = 0; k < 4u; ++k) {
                    float lx = ReadF32(base, entry + 0x10u + k * 12u);
                    float ly = ReadF32(base, entry + 0x10u + k * 12u + 4u);
                    float wx, wy;
                    TransformUiPt(wm, lx, ly, wx, wy);
                    pb.Include(wx, wy);
                }
            }
            // Ignore full-screen backdrop quads when computing individual widget bounds.
            if (pb.valid && !((pb.x1 - pb.x0) > 480.0f && (pb.y1 - pb.y0) > 380.0f)) {
                out.Include(pb.x0, pb.y0);
                out.Include(pb.x1, pb.y1);
            }
        } else if (fourcc == kFourccText) {
            float tx = ReadF32(base, entry + 0x18u);
            float ty = ReadF32(base, entry + 0x1Cu);
            float tw = ReadF32(base, entry + 0x24u);
            float th = ReadF32(base, entry + 0x28u);
            if (!std::isfinite(tx)) tx = 0.0f;
            if (!std::isfinite(ty)) ty = 0.0f;
            if (!std::isfinite(tw) || tw < 4.0f) tw = 140.0f;
            if (!std::isfinite(th) || th < 16.0f) th = 18.0f;
            if (ValidGuest(child_ptr, 0x18u)) {
                float dx = ReadF32(base, child_ptr + 0x10u);
                float dy = ReadF32(base, child_ptr + 0x14u);
                if (std::isfinite(dx) && std::isfinite(dy) && std::fabs(dx) < 600.0f && std::fabs(dy) < 600.0f) {
                    if (tx == 0.0f && ty == 0.0f && (dx != 0.0f || dy != 0.0f)) {
                        tx = dx;
                        ty = dy;
                    }
                }
            }
            float wx0, wy0, wx1, wy1;
            TransformUiPt(wm, tx, ty, wx0, wy0);
            TransformUiPt(wm, tx + tw, ty + th, wx1, wy1);
            out.Include(wx0, wy0);
            out.Include(wx1, wy1);
        } else if ((fourcc == kFourccObj || fourcc == kFourccGrop) && ValidGuest(child_ptr, 0x4Cu)) {
            AccumObjectBounds(base, child_ptr, eff_xform, out, depth + 1);
        }
    }
}

struct UiCandidate {
    uint32_t obj = 0u;
    uint32_t vtable = 0u;
    UiBounds rect{};
    bool is_slider = false;
    bool is_toggle_row = false;
    float track_x0 = 273.0f;
    float track_x1 = 420.0f;
};

void CollectUiCandidates(const uint8_t* base, uint32_t node, uint32_t active_parent, uint32_t inherited_xform,
                         bool ancestors_live, std::vector<UiCandidate>& out, int depth) {
    if (depth > 12 || !ValidGuest(node, 0x4Cu)) return;
    uint32_t data = ReadU32(base, node + 0x1Cu);
    if (!ValidGuest(data, 0x1Cu)) return;

    uint32_t fourcc = ReadU32(base, data);
    uint32_t flags = ReadU32(base, data + 0xCu);
    uint32_t eff_xform = inherited_xform;
    uint32_t own_xform = ReadU32(base, node + 0x48u);
    if (ValidGuest(own_xform, 0x10u)) eff_xform = own_xform;

    // Containers (active_parent / GROP) only need visible (bit 0); selectable leaf buttons require visible+enabled (0x3).
    const bool container_live = (node == active_parent) ? true : (ancestors_live && ((flags & 0x1u) != 0u));
    const bool leaf_live = ancestors_live && ((flags & 0x3u) == 0x3u);
    if (!container_live) return;

    if (node != active_parent && fourcc == kFourccObj && ReadU32(base, node + 0x24u) == active_parent) {
        if (!leaf_live) return;
        uint32_t vtbl = ReadU32(base, node);
        const bool is_sldp = (vtbl == kVtblSldp);
        const bool is_list = (vtbl == kVtblList);
        const bool focusable =
            ObjectHasEvent(base, data, kUiEvtGainFocus) || ObjectHasEvent(base, data, kUiEvtLoseFocus) || is_sldp ||
            is_list;
        if (focusable) {
            UiCandidate cand{};
            cand.obj = node;
            cand.vtable = vtbl;
            cand.is_slider = is_sldp;
            AccumObjectBounds(base, node, eff_xform, cand.rect, 0);
            if (!cand.rect.valid && is_sldp && ValidGuest(node, 0xA4u)) {
                float wm[16];
                ComputeWorldMat(base, eff_xform, wm);
                float sx, sy, ex, ey;
                TransformUiPt(wm, ReadF32(base, node + 0x94u), ReadF32(base, node + 0x98u), sx, sy);
                TransformUiPt(wm, ReadF32(base, node + 0x9Cu), ReadF32(base, node + 0xA0u), ex, ey);
                cand.rect.Include(sx, sy - 8.0f);
                cand.rect.Include(ex, ey + 12.0f);
            }
            if (cand.rect.valid) {
                if (is_sldp && ValidGuest(node, 0xA4u)) {
                    float wm[16];
                    ComputeWorldMat(base, eff_xform, wm);
                    float sx, sy, ex, ey;
                    TransformUiPt(wm, ReadF32(base, node + 0x94u), ReadF32(base, node + 0x98u), sx, sy);
                    TransformUiPt(wm, ReadF32(base, node + 0x9Cu), ReadF32(base, node + 0xA0u), ex, ey);
                    if (std::isfinite(sx) && std::isfinite(ex) && std::fabs(ex - sx) > 8.0f) {
                        cand.track_x0 = std::min(sx, ex);
                        cand.track_x1 = std::max(sx, ex);
                    } else {
                        cand.track_x0 = std::max(cand.rect.x0, 273.0f);
                        cand.track_x1 = std::min(cand.rect.x1, 420.0f);
                        if (cand.track_x1 - cand.track_x0 < 16.0f) {
                            cand.track_x0 = 273.0f;
                            cand.track_x1 = 420.0f;
                        }
                    }
                    cand.rect.x0 = std::min(cand.rect.x0, 20.0f);
                    cand.rect.x1 = std::max(cand.rect.x1, 465.0f);
                } else if (ObjectHasEvent(base, data, kUiEvtLeft) || ObjectHasEvent(base, data, kUiEvtRight)) {
                    // Options toggle rows: extend hit rect across right-column ON/OFF pills (x=20..488).
                    cand.is_toggle_row = true;
                    cand.rect.x0 = std::min(cand.rect.x0, 20.0f);
                    cand.rect.x1 = std::max(cand.rect.x1, 488.0f);
                }
                if (cand.rect.y1 - cand.rect.y0 < 18.0f) {
                    float mid = 0.5f * (cand.rect.y0 + cand.rect.y1);
                    cand.rect.y0 = mid - 9.0f;
                    cand.rect.y1 = mid + 9.0f;
                }
                out.push_back(cand);
            }
            return;
        }
    }

    uint32_t children_arr = ReadU32(base, data + 0x18u);
    if (!ValidGuest(children_arr, 4u)) return;
    uint32_t count = ReadU32(base, children_arr);
    if (count == 0u || count > 256u || !ValidGuest(children_arr + 4u, count * 4u)) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, children_arr + 4u + i * 4u);
        if (!ValidGuest(entry, 0xCu)) continue;
        uint32_t c_fourcc = ReadU32(base, entry);
        uint32_t c_ptr = ReadU32(base, entry + 0x8u);
        if ((c_fourcc == kFourccObj || c_fourcc == kFourccGrop) && ValidGuest(c_ptr, 0x4Cu)) {
            CollectUiCandidates(base, c_ptr, active_parent, eff_xform, container_live, out, depth + 1);
        }
    }
}

bool TryHandleFocusedList(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t list_obj,
                          uint32_t fallback_xform, const mkb::MenuMouseState& mms, int& out_hovered, int& out_items) {
    if (!ValidGuest(list_obj, 0xD0u) || ReadU32(base, list_obj) != kVtblList) return false;
    int32_t sel = int32_t(ReadU32(base, list_obj + 0x64u));
    int32_t scroll_top = int32_t(ReadU32(base, list_obj + 0x68u));
    uint32_t slots_ptr = ReadU32(base, list_obj + 0x78u);
    uint32_t mode = ReadU32(base, list_obj + 0x94u);
    uint32_t item_begin = ReadU32(base, list_obj + 0x9Cu);
    uint32_t item_end = ReadU32(base, list_obj + 0xA0u);
    int32_t slot_count = int32_t(ReadU32(base, list_obj + 0xC8u));
    bool wrap = (ReadU8(base, list_obj + 0xCCu) != 0u);
    int32_t item_count = (item_end > item_begin) ? int32_t((item_end - item_begin) / 28u) : 0;
    if (item_count <= 0 || slot_count <= 0 || slot_count > 32 || !ValidGuest(slots_ptr, uint32_t(slot_count) * 4u)) {
        return false;
    }
    out_items = item_count;
    float wm[16];
    uint32_t lx = ReadU32(base, list_obj + 0x48u);
    ComputeWorldMat(base, ValidGuest(lx, 0x10u) ? lx : fallback_xform, wm);

    int32_t hit_item = -1;
    if (mms.valid) {
        for (int32_t b = 0; b < slot_count; ++b) {
            uint32_t uitext = ReadU32(base, slots_ptr + uint32_t(b) * 4u);
            if (!ValidGuest(uitext, 0x20u)) continue;
            int32_t item_idx = -1;
            if (mode == 0u) {
                item_idx = sel + b - 1;
                if (wrap && item_count > 0) item_idx = ((item_idx % item_count) + item_count) % item_count;
            } else {
                item_idx = scroll_top + b;
                if (wrap && item_count > 0) item_idx %= item_count;
            }
            if (item_idx < 0 || item_idx >= item_count) continue;
            float tx = ReadF32(base, uitext + 0x10u);
            float ty = ReadF32(base, uitext + 0x14u);
            float tw = 260.0f;
            float th = 22.0f;
            uint32_t td = ReadU32(base, uitext + 0x1Cu);
            if (ValidGuest(td, 0x2Cu)) {
                float w0 = ReadF32(base, td + 0x24u);
                float h0 = ReadF32(base, td + 0x28u);
                if (std::isfinite(w0) && w0 > 8.0f && w0 < 512.0f) tw = w0;
                if (std::isfinite(h0) && h0 > 8.0f && h0 < 120.0f) th = std::max(18.0f, h0);
            }
            float wx0, wy0, wx1, wy1;
            TransformUiPt(wm, tx, ty, wx0, wy0);
            TransformUiPt(wm, tx + tw, ty + th, wx1, wy1);
            UiBounds rb{};
            rb.Include(wx0, wy0);
            rb.Include(wx1, wy1);
            if (rb.Contains(mms.x, mms.y)) {
                hit_item = item_idx;
                break;
            }
        }
    }
    out_hovered = hit_item;
    if (mms.wheel_delta != 0) {
        CallListStep(ctx, base, list_obj, mms.wheel_delta > 0 ? -1 : 1);
        return true;
    }
    if (hit_item >= 0) {
        if (hit_item != sel) {
            CallListStep(ctx, base, list_obj, hit_item - sel);
        }
        if (mms.lmb_clicked) {
            CallSendUiEvent(ctx, base, ui_mgr, kUiEvtConfirm);
        }
        return true;
    }
    return false;
}

std::string g_last_logged_ui_screen;
size_t g_last_logged_ui_count = 0;
int g_last_logged_ui_y0 = -99999;

}  // namespace

// HUD::Update @ 0x000C1040 (ecx = HUD*, runs every frame in both Frontend Shell and In-Game Pause/Modals).
// Implements full PC Mouse point-and-click support across all Psi-Ops UI screens using the single embedded
// UIManager (hud + 0xB0) in binary-verified 512x465 (y_off=+10) Layer 0x17 ortho space.
void Hook_HudUpdate(X86Context& ctx, uint8_t* base) {
    EnsurePlayerPortBound(base);
    if (input_rec::ShouldBypassMkb() || mkb::IsOverlayOpen()) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    if (mkb::IsCursorCaptured()) {
        if (mkb::ConsumeNonPointerModalConfirm()) {
            uint32_t hud = ctx.ecx;
            if (!ValidGuest(hud, 0x260u) && ValidGuest(0x005CCB90u, 4u)) {
                hud = ReadU32(base, 0x005CCB90u);
            }
            if (ValidGuest(hud, 0x300u)) {
                uint32_t ui_mgr = hud + 0xB0u;
                if (ValidGuest(ui_mgr, 0x240u) && ReadU32(base, ui_mgr + 0x10u) == 0u &&
                    ReadU32(base, ui_mgr + 0x14u) != 0u && !IsUiBusy(base, ui_mgr)) {
                    CallSendUiEvent(ctx, base, ui_mgr, kUiEvtConfirm);
                }
            }
            mkb::PulseMenuPad(true, false, false);
        }
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    const mkb::Config& cfg = mkb::GetConfig();
    if (cfg.input_mode == mkb::InputMode::Controller) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }

    uint32_t hud = ctx.ecx;
    if (!ValidGuest(hud, 0x260u) && ValidGuest(0x005CCB90u, 4u)) {
        hud = ReadU32(base, 0x005CCB90u);
    }
    if (!ValidGuest(hud, 0x300u)) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    uint32_t ui_mgr = hud + 0xB0u;
    if (!ValidGuest(ui_mgr, 0x240u)) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    if (ReadU32(base, ui_mgr + 0x10u) != 0u || ReadU32(base, ui_mgr + 0x14u) == 0u) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    int32_t stack_top = int32_t(ReadU32(base, ui_mgr + 0x1A4u));
    if (stack_top < 0 || stack_top >= 16) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    uint32_t top_screen = ReadU32(base, ui_mgr + 0x1A8u + uint32_t(stack_top) * 4u);
    if (!ValidGuest(top_screen, 0x68u)) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }

    char screen_name[64] = {};
    for (int i = 0; i < 63; ++i) {
        uint8_t c = ReadU8(base, top_screen + 0x8u + uint32_t(i));
        if (c == 0u || c < 0x20u || c >= 0x7Fu) break;
        screen_name[i] = char(c);
    }

    uint32_t s54 = ReadU32(base, top_screen + 0x54u);
    uint32_t s0c = ValidGuest(s54, 0x10u) ? ReadU32(base, s54 + 0xCu) : 0u;
    uint32_t root_obj = ValidGuest(s0c, 0xCu) ? ReadU32(base, s0c + 0x8u) : 0u;

    uint32_t active_parent = root_obj;
    uint32_t active_parent_xform = 0u;
    uint32_t focused_leaf = 0u;
    for (int depth = 0; depth < 16 && ValidGuest(active_parent, 0x4Cu); ++depth) {
        uint32_t ax = ReadU32(base, active_parent + 0x48u);
        if (ValidGuest(ax, 0x10u)) active_parent_xform = ax;
        uint32_t next = ReadU32(base, active_parent + 0x28u);
        if (!ValidGuest(next, 0x4Cu)) {
            focused_leaf = 0u;
            break;
        }
        uint32_t next2 = ReadU32(base, next + 0x28u);
        if (!ValidGuest(next2, 0x4Cu)) {
            focused_leaf = next;
            break;
        }
        active_parent = next;
    }

    std::vector<UiCandidate> cands;
    if (ValidGuest(active_parent, 0x4Cu)) {
        CollectUiCandidates(base, active_parent, active_parent, active_parent_xform, true, cands, 0);
    }

    // Evaluate current pointer hover index for live telemetry even when idle.
    mkb::MenuMouseState cur_ptr = mkb::GetMenuMouseState();
    int hovered_idx = -1;
    if (cur_ptr.valid) {
        float best_area = 1e18f;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].rect.Contains(cur_ptr.x, cur_ptr.y)) {
                float a = cands[i].rect.Area();
                if (a < best_area) {
                    best_area = a;
                    hovered_idx = int(i);
                }
            }
        }
    }
    mkb::ReportMenuUiTelemetry(screen_name, hovered_idx, int(cands.size()));

    int cur_y0_bucket = cands.empty() ? -99999 : int(std::lround(cands[0].rect.y0 / 4.0f));
    if (mkb::IsAutotestActive() &&
        (g_last_logged_ui_screen != screen_name || g_last_logged_ui_count != cands.size() ||
         g_last_logged_ui_y0 != cur_y0_bucket)) {
        g_last_logged_ui_screen = screen_name;
        g_last_logged_ui_count = cands.size();
        g_last_logged_ui_y0 = cur_y0_bucket;
        std::string summary;
        for (size_t i = 0; i < cands.size(); ++i) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " [%zu:(%.1f,%.1f)-(%.1f,%.1f)%s%s]", i, cands[i].rect.x0, cands[i].rect.y0,
                          cands[i].rect.x1, cands[i].rect.y1, cands[i].is_slider ? " SLDP" : "",
                          cands[i].is_toggle_row ? " TOG" : "");
            summary += buf;
        }
        LOGR("ui_menu: screen='%s' items=%zu active_parent=0x%08X focus=0x%08X%s", screen_name, cands.size(),
             active_parent, focused_leaf, summary.c_str());
    }

    // Wait for any in-flight UI transition queue to settle before dispatching focus/action events.
    if (IsUiBusy(base, ui_mgr)) return;

    mkb::MenuMouseState mms{};
    if (!mkb::ConsumeMenuMouseEvent(mms)) return;

    // 1. AttractMode (Title "Press START"): pulse Port 0 START so Psi-Ops binds active_player_port=0
    //    natively before transitioning to RootMenuContent (ReportMenuUiTelemetry clears the pulse on screen change).
    if (std::strcmp(screen_name, "AttractMode") == 0) {
        if (mms.lmb_clicked) {
            mkb::PulseMenuPad(false, false, true);
        }
        return;
    }

    // 2. RMB Back / Cancel on any menu, submenu, pause menu, or modal.
    if (mms.rmb_clicked) {
        CallSendUiEvent(ctx, base, ui_mgr, kUiEvtBack);
        return;
    }

    // 3. Focused LIST control (SaveSystem, PlayMode, LevelSelect, sub-lists).
    if (ValidGuest(focused_leaf, 0xD0u) && ReadU32(base, focused_leaf) == kVtblList) {
        int list_hov = -1, list_cnt = 0;
        if (TryHandleFocusedList(ctx, base, ui_mgr, focused_leaf, active_parent_xform, mms, list_hov, list_cnt)) {
            mkb::ReportMenuUiTelemetry(screen_name, list_hov, list_cnt);
            return;
        }
    }

    // 4. MouseWheel navigates menu selection Up/Down cleanly.
    if (mms.wheel_delta != 0) {
        CallSendUiEvent(ctx, base, ui_mgr, mms.wheel_delta > 0 ? kUiEvtUp : kUiEvtDown);
    }

    // 5. Recompute hover hit against candidate buttons/rows/sliders at event coordinates.
    int hit_idx = -1;
    if (mms.valid) {
        float best_area = 1e18f;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].rect.Contains(mms.x, mms.y)) {
                float a = cands[i].rect.Area();
                if (a < best_area) {
                    best_area = a;
                    hit_idx = int(i);
                }
            }
        }
    }
    mkb::ReportMenuUiTelemetry(screen_name, hit_idx, int(cands.size()));

    if (hit_idx >= 0) {
        const UiCandidate& hit = cands[size_t(hit_idx)];
        uint32_t cur_focus = ValidGuest(active_parent, 0x2Cu) ? ReadU32(base, active_parent + 0x28u) : 0u;
        // Only invoke SetFocusedChild on actual focus change -> zero repeat hover SFX spam.
        if (hit.obj != cur_focus) {
            CallSetFocusedChild(ctx, base, active_parent, hit.obj, ui_mgr);
        }

        if (hit.is_slider) {
            if ((mms.lmb_clicked || mms.lmb_down) && ValidGuest(hit.obj, 0xB0u) &&
                ReadU32(base, hit.obj + 0xACu) != 0u) {
                int32_t min_v = int32_t(ReadU32(base, hit.obj + 0x84u));
                int32_t cur_v = int32_t(ReadU32(base, hit.obj + 0x88u));
                int32_t max_v = int32_t(ReadU32(base, hit.obj + 0x8Cu));
                int32_t step = std::max<int32_t>(1, int32_t(ReadU32(base, hit.obj + 0x90u)));
                float span = std::max(1.0f, hit.track_x1 - hit.track_x0);
                if (max_v > min_v && mms.x >= hit.track_x0 - 20.0f && mms.x <= hit.track_x1 + 20.0f) {
                    float t = std::clamp((mms.x - hit.track_x0) / span, 0.0f, 1.0f);
                    int32_t target = min_v + int32_t(std::lround(t * float(max_v - min_v)));
                    int32_t delta_steps = (target - cur_v) / step;
                    if (delta_steps != 0) {
                        CallSldpStep(ctx, base, hit.obj, delta_steps);
                    }
                }
            }
        } else if (hit.is_toggle_row) {
            if (mms.lmb_clicked) {
                uint32_t dir_evt = (mms.x >= 240.0f && mms.x < 355.0f) ? kUiEvtLeft : kUiEvtRight;
                CallSendUiEvent(ctx, base, ui_mgr, dir_evt);
            }
        } else if (mms.lmb_clicked) {
            CallSendUiEvent(ctx, base, ui_mgr, kUiEvtConfirm);
        }
    } else if (mms.lmb_clicked && cands.empty()) {
        // Modals without focusable child buttons (e.g., Mission Objectives / Tip / ControlLayout).
        CallSendUiEvent(ctx, base, ui_mgr, kUiEvtConfirm);
        mkb::PulseMenuPad(true, false, false);
    }
}

namespace {

constexpr uint32_t kInputMgrSingletonVa = 0x005EC518u;
constexpr uint32_t kVtblShotgun = 0x004F9650u;
constexpr uint32_t kPsiFnTkHold = 0x00116BE0u;
constexpr uint32_t kPsiFnRvFly = 0x00115C80u;

struct GuestMkbInputFrame {
    bool valid = false;
    bool held[size_t(mkb::Action::Count)] = {};
    bool prev_held[size_t(mkb::Action::Count)] = {};
    bool edge[size_t(mkb::Action::Count)] = {};
    int32_t wheel_delta = 0;
    bool any_input = false;
    float wasd_x = 0.0f;
    float wasd_y = 0.0f;
    float recent_flick_peak = 0.0f;
    float recent_flick_disp = 0.0f;

    int sniper_zoom_in_frames = 0;
    int sniper_zoom_out_frames = 0;
    int tk_push_frames = 0;
    int tk_pull_frames = 0;

    std::chrono::steady_clock::time_point mc_explode_start{};
    bool mc_explode_holding = false;
    bool mc_explode_ready = false;

    int selected_psi = 0;
    bool user_selected_psi = false;

    std::chrono::steady_clock::time_point last_reload_tp{};
    bool has_reloaded_once = false;

    uint32_t player = 0u;
    uint32_t wpn = 0u;
    uint32_t wpn_type = 0u;
    uint32_t psi_fn = 0u;
    uint32_t ground_wpn = 0u;
    uint32_t mc_puppet = 0u;
    uint32_t turret_ptr = 0u;
    bool sniper_scoped = false;
};

GuestMkbInputFrame g_im{};

int ParsePsiPowerTag(const uint8_t* base, uint32_t str_va) {
    if (!ValidGuest(str_va, 3u)) return -1;
    char c0 = char(ReadU8(base, str_va));
    char c1 = char(ReadU8(base, str_va + 1u));
    if (c0 == 'T' && c1 == 'K') return 0;
    if (c0 == 'P' && c1 == 'K') return 1;
    if (c0 == 'M' && c1 == 'D') return 2;
    if (c0 == 'M' && c1 == 'C') return 3;
    if (c0 == 'R' && c1 == 'V') return 4;
    if (c0 == 'A' && c1 == 'V') return 5;
    return -1;
}

void CallWeaponReloadOnce(X86Context& ctx, uint8_t* base, uint32_t wpn, uint32_t reload_fn) {
    if (!ValidGuest(wpn, 4u)) return;
    GuestCallFrame frame(ctx, base);
    if (!frame.ok()) return;
    ctx.ecx = wpn;
    frame.Call(reload_fn);  // Weapon::Reload is __thiscall ret 0
}

void TryManualWeaponReload(X86Context& ctx, uint8_t* base) {
    using namespace std::chrono;
    if (!g_im.wpn || !ValidGuest(g_im.wpn, 0x54u)) return;
    const auto now = steady_clock::now();
    if (g_im.has_reloaded_once && duration_cast<milliseconds>(now - g_im.last_reload_tp).count() < 400) {
        return;
    }

    uint32_t vtbl = ReadU32(base, g_im.wpn);
    if (!ValidGuest(vtbl + 0x1Cu, 4u)) return;
    uint32_t reload_fn = ReadU32(base, vtbl + 0x1Cu);
    if (reload_fn < 0x00011000u || reload_fn >= 0x004D0000u) return;

    int32_t clip = int32_t(ReadU32(base, g_im.wpn + 0x48u));
    int32_t clip_size = int32_t(ReadU32(base, g_im.wpn + 0x4Cu));
    uint32_t store = ReadU32(base, g_im.wpn + 0x44u);
    uint8_t inf_ammo = ReadU8(base, g_im.wpn + 0x50u);
    int32_t reserve = (store && ValidGuest(store, 4u)) ? int32_t(ReadU32(base, store)) : -1;

    if (clip < 0 || clip_size <= 0 || clip_size >= 1000 || clip >= clip_size) return;
    if (inf_ammo == 0u && store != 0u && reserve <= 0) return;

    if (vtbl == kVtblShotgun) {
        // Shotgun::Reload (0x000C9990) loads 2 shells per call -> loop cleanly to full clip or empty reserve.
        for (int iter = 0; iter < 8; ++iter) {
            int32_t before = int32_t(ReadU32(base, g_im.wpn + 0x48u));
            int32_t res_now = (store && ValidGuest(store, 4u)) ? int32_t(ReadU32(base, store)) : 1;
            if (before >= clip_size || (inf_ammo == 0u && store != 0u && res_now <= 0)) break;
            CallWeaponReloadOnce(ctx, base, g_im.wpn, reload_fn);
            int32_t after = int32_t(ReadU32(base, g_im.wpn + 0x48u));
            if (after <= before) break;
        }
    } else {
        CallWeaponReloadOnce(ctx, base, g_im.wpn, reload_fn);
    }

    g_im.last_reload_tp = now;
    g_im.has_reloaded_once = true;
    g_mkb_reload_count.fetch_add(1u, std::memory_order_relaxed);
}

}  // namespace

uint64_t PsiOpsConsumeMkbActionsFired() { return g_mkb_actions_fired_mask.exchange(0u, std::memory_order_acq_rel); }

uint32_t PsiOpsGetMkbReloadCount() { return g_mkb_reload_count.load(std::memory_order_acquire); }

// InputManager::Update @ 0x00294130 (edi = InputManager*, single ret @ 0x294154).
// Snapshots decoupled M&K state once per guest frame, computes 1-frame rising edges,
// updates active Psi slot (1..6) + wheel zoom/push latches, and executes manual Weapon::Reload on R tap.
void Hook_InputManagerUpdate(X86Context& ctx, uint8_t* base) {
    using mkb::Action;
    if (input_rec::ShouldBypassMkb()) {
        g_im.valid = false;
        g_im_sniper_weapon_or_scoped.store(false, std::memory_order_relaxed);
        return;
    }

    mkb::GameplayInputSnapshot snap{};
    mkb::PollGameplayInput(snap);

    for (size_t i = 0; i < size_t(Action::Count); ++i) {
        g_im.held[i] = snap.held[i];
        g_im.edge[i] = snap.held[i] && !g_im.prev_held[i];
        g_im.prev_held[i] = snap.held[i];
    }
    g_im.wheel_delta = snap.wheel_delta;
    g_im.any_input = snap.any_key_or_mouse;
    g_im.wasd_x = snap.wasd_x;
    g_im.wasd_y = snap.wasd_y;
    g_im.recent_flick_peak = snap.recent_flick_peak;
    g_im.recent_flick_disp = snap.recent_flick_disp;

    uint32_t im = ctx.edi;
    if (!ValidGuest(im, 0x28u)) im = kInputMgrSingletonVa;
    uint32_t mode = ValidGuest(im + 0x14u, 4u) ? ReadU32(base, im + 0x14u) : 0u;
    uint32_t override_cnt = ValidGuest(im + 0x24u, 4u) ? ReadU32(base, im + 0x24u) : 0u;

    g_im.valid = snap.mkb_active && !snap.overlay_open && (mode == 2u || mode == 3u || mode == 4u) &&
                 (override_cnt == 0u);
    if (!g_im.valid) {
        g_im.sniper_zoom_in_frames = 0;
        g_im.sniper_zoom_out_frames = 0;
        g_im.tk_push_frames = 0;
        g_im.tk_pull_frames = 0;
        g_im.mc_explode_holding = false;
        g_im.mc_explode_ready = false;
        g_im_sniper_weapon_or_scoped.store(false, std::memory_order_relaxed);
        return;
    }

    // Read binary-verified live Player / Weapon / PsiManager / Camera context
    g_im.player = 0u;
    g_im.wpn = 0u;
    g_im.wpn_type = 0u;
    g_im.psi_fn = 0u;
    g_im.ground_wpn = 0u;
    g_im.mc_puppet = 0u;
    g_im.turret_ptr = 0u;
    g_im.sniper_scoped = false;

    uint32_t app = ValidGuest(0x00586A04u, 4u) ? ReadU32(base, 0x00586A04u) : 0u;
    if (app && ValidGuest(app + 0xD0u, 4u)) {
        g_im.player = ReadU32(base, app + 0xD0u);
    }
    uint32_t psi_mgr = 0u;
    if (g_im.player && ValidGuest(g_im.player, 0x870u)) {
        uint32_t wpn = ReadU32(base, g_im.player + 0x6FCu);
        if (!wpn) {
            uint32_t holder = ReadU32(base, g_im.player + 0x788u);
            if (holder && ValidGuest(holder + 0x4u, 4u)) wpn = ReadU32(base, holder + 0x4u);
        }
        g_im.wpn = wpn;
        if (wpn && ValidGuest(wpn, 0x14u)) {
            g_im.wpn_type = ReadU32(base, wpn + 0x10u);
        }
        psi_mgr = ReadU32(base, g_im.player + 0x6F4u);
        if (psi_mgr && ValidGuest(psi_mgr, 8u)) {
            g_im.psi_fn = ReadU32(base, psi_mgr + 0x4u);
        }
        g_im.ground_wpn = ReadU32(base, g_im.player + 0x74Cu);
        g_im.mc_puppet = ReadU32(base, g_im.player + 0x764u);
        g_im.turret_ptr = ReadU32(base, g_im.player + 0x854u);
    }

    uint32_t cam = PsiOpsLiveCameraVa();
    if (cam && ValidGuest(cam + 0x37Cu, 4u)) {
        float sniper_timer = ReadF32(base, cam + 0x37Cu);
        g_im.sniper_scoped = std::isfinite(sniper_timer) && sniper_timer > 0.8f;
    }
    g_im_sniper_weapon_or_scoped.store(g_im.wpn_type == 3u || g_im.sniper_scoped, std::memory_order_relaxed);

    // Sync active Psi slot (0..5: TK/PK/MD/MC/RV/AV) for LShift/Mouse5 PsiUse
    if (!g_im.user_selected_psi && psi_mgr && ValidGuest(psi_mgr + 0x158u, 0x38u)) {
        uint32_t idx = ReadU32(base, psi_mgr + 0x158u);
        if (idx < 6u) {
            uint32_t tag_va = ReadU32(base, psi_mgr + 0x160u + idx * 8u);
            int parsed = ParsePsiPowerTag(base, tag_va);
            if (parsed >= 0) {
                g_im.selected_psi = parsed;
                mkb::SetSelectedPsiIndex(parsed);
            }
        }
    }
    for (int k = 0; k < 6; ++k) {
        if (g_im.edge[size_t(Action::PsiTk) + size_t(k)]) {
            g_im.selected_psi = k;
            g_im.user_selected_psi = true;
            mkb::SetSelectedPsiIndex(k);
        }
    }

    // Multi-frame wheel latches for held-mode Sniper Zoom (0x34/0x35) and TK Push/Pull (0x05/0x06)
    if (g_im.sniper_zoom_in_frames > 0) --g_im.sniper_zoom_in_frames;
    if (g_im.sniper_zoom_out_frames > 0) --g_im.sniper_zoom_out_frames;
    if (g_im.tk_push_frames > 0) --g_im.tk_push_frames;
    if (g_im.tk_pull_frames > 0) --g_im.tk_pull_frames;

    if (g_im.sniper_scoped) {
        g_im.tk_push_frames = 0;
        g_im.tk_pull_frames = 0;
        if (g_im.wheel_delta > 0) {
            g_im.sniper_zoom_in_frames = 6;
            g_im.sniper_zoom_out_frames = 0;
        } else if (g_im.wheel_delta < 0) {
            g_im.sniper_zoom_out_frames = 6;
            g_im.sniper_zoom_in_frames = 0;
        }
    } else if (g_im.psi_fn == kPsiFnTkHold) {
        g_im.sniper_zoom_in_frames = 0;
        g_im.sniper_zoom_out_frames = 0;
        if (g_im.wheel_delta > 0) {
            g_im.tk_push_frames = 5;
            g_im.tk_pull_frames = 0;
        } else if (g_im.wheel_delta < 0) {
            g_im.tk_pull_frames = 5;
            g_im.tk_push_frames = 0;
        }
    } else {
        g_im.sniper_zoom_in_frames = 0;
        g_im.sniper_zoom_out_frames = 0;
        g_im.tk_push_frames = 0;
        g_im.tk_pull_frames = 0;
    }

    // MC Head-Pop (0x38) 500ms hold guard so tapping X never kills a mind-controlled puppet by accident
    if (g_im.mc_puppet != 0u && g_im.held[size_t(Action::McExplode)]) {
        const auto now = std::chrono::steady_clock::now();
        if (!g_im.mc_explode_holding) {
            g_im.mc_explode_holding = true;
            g_im.mc_explode_start = now;
        }
        g_im.mc_explode_ready =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_im.mc_explode_start).count() >= 500;
    } else {
        g_im.mc_explode_holding = false;
        g_im.mc_explode_ready = false;
    }

    // Instant Manual Weapon Reload on Action::Reload rising edge (never switches weapons!)
    if (g_im.edge[size_t(Action::Reload)] && g_im.mc_puppet == 0u && g_im.turret_ptr == 0u &&
        g_im.psi_fn != kPsiFnRvFly && g_im.psi_fn != kPsiFnTkHold) {
        TryManualWeaponReload(ctx, base);
    }
}

// InputManager::GetActionValue @ 0x00294160 (with on_true = "ret" -> ret 4).
// ABI: esi = InputManager*, [esp+4] = uint32_t action_id, returns float in x87 st(0).
// Decouples every PC M&K gameplay action cleanly regardless of Xbox Config 1/2/3 while
// passing through 100% untouched for Controller mode, .rec replays, UI/menus, and script overrides.
bool Hook_InputManagerGetActionValue(X86Context& ctx, uint8_t* base) {
    using mkb::Action;
    if (input_rec::ShouldBypassMkb() || !mkb::IsMkbActiveForGameplay() || mkb::IsOverlayOpen()) return false;
    if (!g_im.valid) return false;

    uint32_t im = ctx.esi;
    if (!ValidGuest(im, 0x28u)) im = kInputMgrSingletonVa;
    if (!ValidGuest(im, 0x28u)) return false;
    uint32_t mode = ReadU32(base, im + 0x14u);
    if (mode != 2u && mode != 3u && mode != 4u) return false;
    if (ReadU32(base, im + 0x24u) != 0u) return false;
    if (!ValidGuest(ctx.esp + 0x4u, 4u)) return false;

    uint32_t action_id = ReadU32(base, ctx.esp + 0x4u);
    float val = 0.0f;

    switch (action_id) {
        case 0x00u:  // Break Free (Grab Wiggle)
            val = (g_im.edge[size_t(Action::MoveLeft)] || g_im.edge[size_t(Action::MoveRight)] ||
                   g_im.edge[size_t(Action::Jump)])
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x01u:  // Move Forward
        case 0x09u:  // Remote View Move Forward
            val = std::max(0.0f, g_im.wasd_y);
            break;
        case 0x02u:  // Move Backward
        case 0x0Au:  // Remote View Move Backward
            val = std::max(0.0f, -g_im.wasd_y);
            break;
        case 0x03u:  // Strafe Left
        case 0x0Bu:  // Remote View Strafe Left
            val = std::max(0.0f, -g_im.wasd_x);
            break;
        case 0x04u:  // Strafe Right
        case 0x0Cu:  // Remote View Strafe Right
            val = std::max(0.0f, g_im.wasd_x);
            break;
        case 0x05u:  // TK Hold Push Away (WheelUp)
            val = (g_im.psi_fn == kPsiFnTkHold && g_im.tk_push_frames > 0) ? 1.0f : 0.0f;
            break;
        case 0x06u:  // TK Hold Pull Closer (WheelDown)
            val = (g_im.psi_fn == kPsiFnTkHold && g_im.tk_pull_frames > 0) ? 1.0f : 0.0f;
            break;
        case 0x07u:  // Psi Aim Left (handled by raw mouse hooks)
        case 0x08u:  // Psi Aim Right
            val = 0.0f;
            break;
        case 0x0Du:  // TK Throw / Flick during 0.15s release window (0x00117380)
            val = (g_im.recent_flick_peak >= 8.0f || g_im.recent_flick_disp >= 20.0f) ? 1.0f : 0.0f;
            break;
        case 0x0Eu:  // Psi Force-Cancel (unbound in SP)
            val = 0.0f;
            break;
        case 0x10u:  // Use Psi (Held)
            val = (g_im.held[size_t(Action::PsiUse)] || g_im.held[size_t(Action::PsiTk)] ||
                   g_im.held[size_t(Action::PsiPk)] || g_im.held[size_t(Action::PsiMd)] ||
                   g_im.held[size_t(Action::PsiMc)] || g_im.held[size_t(Action::PsiRv)] ||
                   g_im.held[size_t(Action::PsiAv)])
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x11u:  // Use Psi (Edge / Cast / Exit MC, RV, AV)
            val = (g_im.edge[size_t(Action::PsiUse)] || g_im.edge[size_t(Action::PsiTk)] ||
                   g_im.edge[size_t(Action::PsiPk)] || g_im.edge[size_t(Action::PsiMd)] ||
                   g_im.edge[size_t(Action::PsiMc)] || g_im.edge[size_t(Action::PsiRv)] ||
                   g_im.edge[size_t(Action::PsiAv)])
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x12u:  // Psi: TK
        case 0x13u:  // Psi: PK
        case 0x14u:  // Psi: MD
        case 0x15u:  // Psi: MC
        case 0x16u:  // Psi: RV
        case 0x17u: {  // Psi: AV
            const int idx = int(action_id - 0x12u);
            val = (g_im.held[size_t(Action::PsiTk) + size_t(idx)] ||
                   (g_im.held[size_t(Action::PsiUse)] && g_im.selected_psi == idx))
                      ? 1.0f
                      : 0.0f;
            break;
        }
        case 0x18u:  // Next Weapon (never triggered by Reload R!)
            val = (!g_im.sniper_scoped && g_im.psi_fn != kPsiFnTkHold && g_im.edge[size_t(Action::WeaponNext)]) ? 1.0f
                                                                                                                : 0.0f;
            break;
        case 0x19u:  // Previous Weapon (engine-native 0x0FC970, never triggered by Reload R!)
            val = (!g_im.sniper_scoped && g_im.psi_fn != kPsiFnTkHold && g_im.edge[size_t(Action::WeaponPrev)]) ? 1.0f
                                                                                                                : 0.0f;
            break;
        case 0x1Au:  // Fire Weapon (Held)
            val = g_im.held[size_t(Action::Fire)] ? 1.0f : 0.0f;
            break;
        case 0x1Bu:  // Fire Weapon (Edge)
            val = g_im.edge[size_t(Action::Fire)] ? 1.0f : 0.0f;
            break;
        case 0x1Du:  // Sniper Scope Toggle (only when holding Sniper Rifle [wpn+0x10]==3 or scoped)
            val = ((g_im.wpn_type == 3u || g_im.sniper_scoped) && g_im.edge[size_t(Action::FineAim)]) ? 1.0f : 0.0f;
            break;
        case 0x1Eu:  // Pick Up Ground Weapon (E when standing over droppable weapon [player+0x74C]!=0)
            val = (g_im.ground_wpn != 0u && g_im.edge[size_t(Action::Interact)]) ? 1.0f : 0.0f;
            break;
        case 0x1Fu:  // Interact / Use / Ladder / Turret ONLY (NEVER triggers Jump 0x23!)
            val = g_im.edge[size_t(Action::Interact)] ? 1.0f : 0.0f;
            break;
        case 0x20u:  // Melee Attack ONLY (NEVER triggered by Space!)
            val = g_im.edge[size_t(Action::Melee)] ? 1.0f : 0.0f;
            break;
        case 0x21u:  // Open Mission Objectives (Tab)
            val = g_im.edge[size_t(Action::Objectives)] ? 1.0f : 0.0f;
            break;
        case 0x23u:  // Jump / Climb / Mantle / Vault ONLY (NEVER triggers Melee 0x20 or Interact 0x1F!)
            val = g_im.edge[size_t(Action::Jump)] ? 1.0f : 0.0f;
            break;
        case 0x26u:  // Crouch / Sneak Toggle (LCtrl / C)
            val = g_im.edge[size_t(Action::Crouch)] ? 1.0f : 0.0f;
            break;
        case 0x27u:  // Camera Look Right (handled 1:1 by Hook_CameraApplyManualAim)
        case 0x28u:  // Camera Look Left
        case 0x29u:  // Camera Look Up
        case 0x2Au:  // Camera Look Down
            val = 0.0f;
            break;
        case 0x2Bu:  // Target Lock-On / Strafe (Held, non-sniper weapons)
            val = ((g_im.wpn_type != 3u && !g_im.sniper_scoped) && g_im.held[size_t(Action::FineAim)]) ? 1.0f : 0.0f;
            break;
        case 0x2Cu:  // Target Lock-On (Edge)
        case 0x33u:  // Target Lock-On Strafe (Edge)
            val = ((g_im.wpn_type != 3u && !g_im.sniper_scoped) && g_im.edge[size_t(Action::FineAim)]) ? 1.0f : 0.0f;
            break;
        case 0x2Eu:  // TK Camera Control Toggle (G during TK Hold)
            val = (g_im.psi_fn == kPsiFnTkHold && g_im.edge[size_t(Action::TkCamControl)]) ? 1.0f : 0.0f;
            break;
        case 0x34u:  // Sniper Scope Zoom In (WheelUp / WeaponNext while scoped)
            val = (g_im.sniper_scoped && (g_im.held[size_t(Action::WeaponNext)] || g_im.sniper_zoom_in_frames > 0))
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x35u:  // Sniper Scope Zoom Out (WheelDown / WeaponPrev while scoped)
            val = (g_im.sniper_scoped && (g_im.held[size_t(Action::WeaponPrev)] || g_im.sniper_zoom_out_frames > 0))
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x36u:  // Wall Move / Wall Cover (LAlt)
            val = g_im.edge[size_t(Action::WallMove)] ? 1.0f : 0.0f;
            break;
        case 0x37u:  // MC Puppet Secondary Attack / Grenade (R/MMB held while in Mind Control)
            val = (g_im.mc_puppet != 0u && g_im.held[size_t(Action::Reload)]) ? 1.0f : 0.0f;
            break;
        case 0x38u:  // MC Head-Explode / Suicide (X held >= 0.5s while in Mind Control)
            val = (g_im.mc_puppet != 0u && g_im.mc_explode_ready) ? 1.0f : 0.0f;
            break;
        case 0x7Fu:  // Skip Cutscene / QuickTip / Credits
            val = (g_im.edge[size_t(Action::Jump)] || g_im.edge[size_t(Action::Interact)] ||
                   g_im.edge[size_t(Action::Pause)])
                      ? 1.0f
                      : 0.0f;
            break;
        case 0x80u:  // Pause Menu (Escape)
            val = g_im.edge[size_t(Action::Pause)] ? 1.0f : 0.0f;
            break;
        case 0x86u:  // Idle Timer Reset
            val = g_im.any_input ? 1.0f : 0.0f;
            break;
        default:
            return false;
    }

    if (val > 0.0f && action_id < 64u) {
        g_mkb_actions_fired_mask.fetch_or(1ull << action_id, std::memory_order_relaxed);
    }
    x87_push(ctx, double(val));
    return true;
}
