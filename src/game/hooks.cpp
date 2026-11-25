// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific function hooks for Psi-Ops: The Mindgate Conspiracy (default.xbe, Title ID 0x4D570016).
// All addresses and ABIs are binary-verified against retail USA default.xbe (XDK 5849).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Saved guest LookSpring (GameStatus+0x240 @ 0x005ECEB8) so controller mode restores user's preference.
std::atomic<bool> g_look_spring_overridden{false};
std::atomic<uint32_t> g_saved_look_spring{1u};
// Last pitch delta consumed by Camera::ApplyManualAim on this frame, shared with Player::UpdateTKPitch during TK.
std::atomic<float> g_pending_tk_pitch{0.0f};
// Exact live Camera* VA observed in Camera::ApplyManualAim (shared with game_info.cpp telemetry).
std::atomic<uint32_t> g_live_camera_va{0u};

void SyncLookSpring(uint8_t* base, bool mkb_active) {
    constexpr uint32_t kLookSpringVa = 0x005ECEB8u;
    if (!mem::IsCommitted(kLookSpringVa)) return;
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
    if (ctx.edi && mem::IsCommitted(ctx.edi + 0x380u)) {
        g_live_camera_va.store(ctx.edi, std::memory_order_release);
    }
    if (input_rec::ShouldBypassMkb()) {
        SyncLookSpring(base, false);
        return;
    }
    const bool mkb_active = mkb::IsMkbActiveForGameplay();
    SyncLookSpring(base, mkb_active);
    if (!mkb_active) return;

    if (!mem::IsCommitted(ctx.esp + 0x4u) || !mem::IsCommitted(ctx.esp + 0xCu)) return;
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
    if (cam && mem::IsCommitted(cam + 0x380u)) {
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
    if (mem::IsCommitted(0x005ECD74u) && mem::IsCommitted(0x005CD28Cu)) {
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
    if (!mem::IsCommitted(ctx.esp) || !mem::IsCommitted(ctx.esp + 0x4u)) return;

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
    if (!player || !mem::IsCommitted(player + 0x744u) || !mem::IsCommitted(player + 0x6F4u)) return;

    // Only apply when Telekinesis psi state is active ({0x116BE0, 0x117380, 0x1174F0})
    uint32_t psi_mgr = ReadU32(base, player + 0x6F4u);
    if (!psi_mgr || !mem::IsCommitted(psi_mgr + 0x4u)) return;
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
// When M&K is active with Raw 1:1 Aim enabled, zeroes *out_yaw/*out_pitch and returns early to bypass auto-steer.
bool Hook_CameraAutoSteerBypass(X86Context& ctx, uint8_t* base) {
    if (input_rec::ShouldBypassMkb() || !mkb::IsMkbActiveForGameplay() || !mkb::IsRawAimEnabled()) {
        return false;
    }
    if (mem::IsCommitted(ctx.esp + 0x18u) && mem::IsCommitted(ctx.esp + 0x1Cu)) {
        uint32_t out_yaw_va = ReadU32(base, ctx.esp + 0x18u);
        uint32_t out_pitch_va = ReadU32(base, ctx.esp + 0x1Cu);
        if (out_yaw_va && mem::IsCommitted(out_yaw_va)) WriteF32(base, out_yaw_va, 0.0f);
        if (out_pitch_va && mem::IsCommitted(out_pitch_va)) WriteF32(base, out_pitch_va, 0.0f);
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

bool SafeAddr(uint32_t va, uint32_t len) {
    if (va < 0x10000u || len == 0u || va > 0xFFFFFFFFu - len) return false;
    if (!mem::IsCommitted(va)) return false;
    if (len > 1u && !mem::IsCommitted(va + len - 1u)) return false;
    return true;
}

void WriteU32(uint8_t* base, uint32_t va, uint32_t v) { std::memcpy(base + va, &v, sizeof(v)); }

uint8_t ReadU8(const uint8_t* base, uint32_t va) { return base[va]; }

bool IsUiBusy(const uint8_t* base, uint32_t ui_mgr) {
    // Mirrors UIActionQueue::IsBusy @ 0x00250DE0(ui_mgr + 0x22C).
    if (!SafeAddr(ui_mgr + 0x234u, 4u)) return false;
    uint32_t node = ReadU32(base, ui_mgr + 0x234u);
    for (int i = 0; i < 256 && SafeAddr(node, 0x3Cu); ++i) {
        if (ReadU32(base, node + 0x4u) != 0u && ReadU32(base, node + 0xCu) != 0u) return true;
        node = ReadU32(base, node + 0x38u);
    }
    return false;
}

void CallSetFocusedChild(X86Context& ctx, uint8_t* base, uint32_t active_parent, uint32_t child, uint32_t ui_mgr) {
    if (!SafeAddr(ctx.esp - 0x20u, 0x20u)) return;
    X86Context saved = ctx;
    ctx.ebx = active_parent;
    ctx.edi = child;
    ctx.esi = 0u;
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, 1u);           // [esp+8] = send_events
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, ui_mgr);       // [esp+4] = ui_mgr
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, 0xFFFFFFF0u);  // dummy return address
    x86_icall(ctx, base, 0x0024CAD0u);
    ctx = saved;
}

void EnsurePlayerPortBound(uint8_t* base) {
    if (!SafeAddr(0x0055673Cu, 16u)) return;
    uint32_t p0 = ReadU32(base, 0x0055673Cu);
    if (p0 == 0u) return;
    bool p0_disconnected =
        (p0 >= 4u) || (SafeAddr(0x005DED18u + p0 * 0x94u + 0x14u, 1u) && ReadU8(base, 0x005DED18u + p0 * 0x94u + 0x14u) == 0u);
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
    if (!SafeAddr(ctx.esp - 0x20u, 0x20u)) return;
    X86Context saved = ctx;
    ctx.esi = ui_mgr;
    ctx.eax = 0u;
    ctx.edx = 0u;
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, event_id);     // [esp+4] = event_id
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, 0xFFFFFFF0u);  // dummy return address
    x86_icall(ctx, base, 0x0024DDC0u);
    ctx = saved;
    EnsurePlayerPortBound(base);
}

void CallSldpStep(X86Context& ctx, uint8_t* base, uint32_t sldp, int32_t steps) {
    if (steps == 0 || !SafeAddr(ctx.esp - 0x20u, 0x20u)) return;
    uint32_t fn = (steps > 0) ? 0x0012D3E0u : 0x0012D490u;
    uint32_t count = uint32_t(steps > 0 ? steps : -steps);
    X86Context saved = ctx;
    ctx.ecx = sldp;
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, count);
    ctx.esp -= 4u;
    WriteU32(base, ctx.esp, 0xFFFFFFF0u);
    x86_icall(ctx, base, fn);
    ctx = saved;
}

void CallListStep(X86Context& ctx, uint8_t* base, uint32_t list_obj, int32_t steps) {
    if (steps == 0 || !SafeAddr(ctx.esp - 0x20u, 0x20u)) return;
    uint32_t fn = (steps > 0) ? 0x0012C270u : 0x0012C3F0u;
    int32_t n = std::clamp(steps > 0 ? steps : -steps, 1, 8);
    for (int32_t i = 0; i < n; ++i) {
        X86Context saved = ctx;
        ctx.ecx = list_obj;
        ctx.esp -= 4u;
        WriteU32(base, ctx.esp, 1u);
        ctx.esp -= 4u;
        WriteU32(base, ctx.esp, 0xFFFFFFF0u);
        x86_icall(ctx, base, fn);
        ctx = saved;
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
    if (SafeAddr(cur, 0x0Cu) && ReadU32(base, cur) != kVtblTransformNode) {
        uint32_t inner = ReadU32(base, cur + 0x4u);
        if (SafeAddr(inner, 0x0Cu) && ReadU32(base, inner) == kVtblTransformNode) {
            cur = inner;
        }
    }
    for (int depth = 0; depth < 32 && SafeAddr(cur, 0x10u); ++depth) {
        uint32_t m_va = ReadU32(base, cur + 0x4u);
        if (SafeAddr(m_va, 64u)) {
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
    if (!SafeAddr(data, 0x1Cu)) return false;
    uint32_t evt_arr = ReadU32(base, data + 0x10u);
    if (!SafeAddr(evt_arr, 4u)) return false;
    uint32_t count = ReadU32(base, evt_arr);
    if (count == 0u || count > 128u || !SafeAddr(evt_arr + 4u, count * 4u)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, evt_arr + 4u + i * 4u);
        if (SafeAddr(entry, 4u) && ReadU32(base, entry) == want_evt) return true;
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
    if (depth > 8 || !SafeAddr(obj, 0x4Cu)) return;
    uint32_t data = ReadU32(base, obj + 0x1Cu);
    if (!SafeAddr(data, 0x1Cu)) return;
    uint32_t children_arr = ReadU32(base, data + 0x18u);
    if (!SafeAddr(children_arr, 4u)) return;
    uint32_t count = ReadU32(base, children_arr);
    if (count == 0u || count > 256u || !SafeAddr(children_arr + 4u, count * 4u)) return;

    // Mirror CUIObject::Render @ 0x0024C660: inherit ancestor TransformNode when [obj+0x48] is NULL.
    uint32_t eff_xform = inherited_xform;
    uint32_t own_xform = ReadU32(base, obj + 0x48u);
    if (SafeAddr(own_xform, 0x10u)) eff_xform = own_xform;

    float wm[16];
    ComputeWorldMat(base, eff_xform, wm);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, children_arr + 4u + i * 4u);
        if (!SafeAddr(entry, 0x30u)) continue;
        uint32_t fourcc = ReadU32(base, entry);
        uint32_t child_ptr = ReadU32(base, entry + 0x8u);
        if (fourcc == kFourccPoly) {
            UiBounds pb{};
            bool got_vb = false;
            if (SafeAddr(child_ptr, 0x20u)) {
                uint32_t vb = ReadU32(base, child_ptr + 0x1Cu);
                float ox = ReadF32(base, child_ptr + 0x10u);
                float oy = ReadF32(base, child_ptr + 0x14u);
                if (!std::isfinite(ox)) ox = 0.0f;
                if (!std::isfinite(oy)) oy = 0.0f;
                if (SafeAddr(vb, 0x28u + 4u * 0x28u)) {
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
            if (!got_vb && SafeAddr(entry, 0x10u + 4u * 12u)) {
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
            if (SafeAddr(child_ptr, 0x18u)) {
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
        } else if ((fourcc == kFourccObj || fourcc == kFourccGrop) && SafeAddr(child_ptr, 0x4Cu)) {
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
    if (depth > 12 || !SafeAddr(node, 0x4Cu)) return;
    uint32_t data = ReadU32(base, node + 0x1Cu);
    if (!SafeAddr(data, 0x1Cu)) return;

    uint32_t fourcc = ReadU32(base, data);
    uint32_t flags = ReadU32(base, data + 0xCu);
    uint32_t eff_xform = inherited_xform;
    uint32_t own_xform = ReadU32(base, node + 0x48u);
    if (SafeAddr(own_xform, 0x10u)) eff_xform = own_xform;

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
            if (!cand.rect.valid && is_sldp && SafeAddr(node, 0xA4u)) {
                float wm[16];
                ComputeWorldMat(base, eff_xform, wm);
                float sx, sy, ex, ey;
                TransformUiPt(wm, ReadF32(base, node + 0x94u), ReadF32(base, node + 0x98u), sx, sy);
                TransformUiPt(wm, ReadF32(base, node + 0x9Cu), ReadF32(base, node + 0xA0u), ex, ey);
                cand.rect.Include(sx, sy - 8.0f);
                cand.rect.Include(ex, ey + 12.0f);
            }
            if (cand.rect.valid) {
                if (is_sldp && SafeAddr(node, 0xA4u)) {
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
    if (!SafeAddr(children_arr, 4u)) return;
    uint32_t count = ReadU32(base, children_arr);
    if (count == 0u || count > 256u || !SafeAddr(children_arr + 4u, count * 4u)) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = ReadU32(base, children_arr + 4u + i * 4u);
        if (!SafeAddr(entry, 0xCu)) continue;
        uint32_t c_fourcc = ReadU32(base, entry);
        uint32_t c_ptr = ReadU32(base, entry + 0x8u);
        if ((c_fourcc == kFourccObj || c_fourcc == kFourccGrop) && SafeAddr(c_ptr, 0x4Cu)) {
            CollectUiCandidates(base, c_ptr, active_parent, eff_xform, container_live, out, depth + 1);
        }
    }
}

bool TryHandleFocusedList(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t list_obj,
                          uint32_t fallback_xform, const mkb::MenuMouseState& mms, int& out_hovered, int& out_items) {
    if (!SafeAddr(list_obj, 0xD0u) || ReadU32(base, list_obj) != kVtblList) return false;
    int32_t sel = int32_t(ReadU32(base, list_obj + 0x64u));
    int32_t scroll_top = int32_t(ReadU32(base, list_obj + 0x68u));
    uint32_t slots_ptr = ReadU32(base, list_obj + 0x78u);
    uint32_t mode = ReadU32(base, list_obj + 0x94u);
    uint32_t item_begin = ReadU32(base, list_obj + 0x9Cu);
    uint32_t item_end = ReadU32(base, list_obj + 0xA0u);
    int32_t slot_count = int32_t(ReadU32(base, list_obj + 0xC8u));
    bool wrap = (ReadU8(base, list_obj + 0xCCu) != 0u);
    int32_t item_count = (item_end > item_begin) ? int32_t((item_end - item_begin) / 28u) : 0;
    if (item_count <= 0 || slot_count <= 0 || slot_count > 32 || !SafeAddr(slots_ptr, uint32_t(slot_count) * 4u)) {
        return false;
    }
    out_items = item_count;
    float wm[16];
    uint32_t lx = ReadU32(base, list_obj + 0x48u);
    ComputeWorldMat(base, SafeAddr(lx, 0x10u) ? lx : fallback_xform, wm);

    int32_t hit_item = -1;
    if (mms.valid) {
        for (int32_t b = 0; b < slot_count; ++b) {
            uint32_t uitext = ReadU32(base, slots_ptr + uint32_t(b) * 4u);
            if (!SafeAddr(uitext, 0x20u)) continue;
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
            if (SafeAddr(td, 0x2Cu)) {
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
            if (!SafeAddr(hud, 0x260u) && SafeAddr(0x005CCB90u, 4u)) {
                hud = ReadU32(base, 0x005CCB90u);
            }
            if (SafeAddr(hud, 0x300u)) {
                uint32_t ui_mgr = hud + 0xB0u;
                if (SafeAddr(ui_mgr, 0x240u) && ReadU32(base, ui_mgr + 0x10u) == 0u &&
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
    if (!SafeAddr(hud, 0x260u) && SafeAddr(0x005CCB90u, 4u)) {
        hud = ReadU32(base, 0x005CCB90u);
    }
    if (!SafeAddr(hud, 0x300u)) {
        mkb::ReportMenuUiTelemetry("", -1, 0);
        return;
    }
    uint32_t ui_mgr = hud + 0xB0u;
    if (!SafeAddr(ui_mgr, 0x240u)) {
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
    if (!SafeAddr(top_screen, 0x68u)) {
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
    uint32_t s0c = SafeAddr(s54, 0x10u) ? ReadU32(base, s54 + 0xCu) : 0u;
    uint32_t root_obj = SafeAddr(s0c, 0xCu) ? ReadU32(base, s0c + 0x8u) : 0u;

    uint32_t active_parent = root_obj;
    uint32_t active_parent_xform = 0u;
    uint32_t focused_leaf = 0u;
    for (int depth = 0; depth < 16 && SafeAddr(active_parent, 0x4Cu); ++depth) {
        uint32_t ax = ReadU32(base, active_parent + 0x48u);
        if (SafeAddr(ax, 0x10u)) active_parent_xform = ax;
        uint32_t next = ReadU32(base, active_parent + 0x28u);
        if (!SafeAddr(next, 0x4Cu)) {
            focused_leaf = 0u;
            break;
        }
        uint32_t next2 = ReadU32(base, next + 0x28u);
        if (!SafeAddr(next2, 0x4Cu)) {
            focused_leaf = next;
            break;
        }
        active_parent = next;
    }

    std::vector<UiCandidate> cands;
    if (SafeAddr(active_parent, 0x4Cu)) {
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
    if (SafeAddr(focused_leaf, 0xD0u) && ReadU32(base, focused_leaf) == kVtblList) {
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
        uint32_t cur_focus = SafeAddr(active_parent, 0x2Cu) ? ReadU32(base, active_parent + 0x28u) : 0u;
        // Only invoke SetFocusedChild on actual focus change -> zero repeat hover SFX spam.
        if (hit.obj != cur_focus) {
            CallSetFocusedChild(ctx, base, active_parent, hit.obj, ui_mgr);
        }

        if (hit.is_slider) {
            if ((mms.lmb_clicked || mms.lmb_down) && SafeAddr(hit.obj, 0xB0u) &&
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
