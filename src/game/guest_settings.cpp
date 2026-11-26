// SPDX-License-Identifier: GPL-2.0-or-later
// Thread-safe host state and bidirectional synchronization queue for Psi-Ops [game_options]
// (GameStatus @ 0x005ECC78 user preferences persisted in settings.toml).

#include "game/guest_settings.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "app/app.h"
#include "common/input_recorder.h"
#include "common/mkb_input.h"

namespace psiops::guest_settings {

namespace {

struct RuntimeState {
    std::mutex mtx;
    GuestUserSettings current{};
    GuestUserSettings last_saved{};
    std::atomic<bool> pending_guest_sync{false};
    std::atomic<bool> pending_host_save{false};
};

RuntimeState& Rt() {
    static RuntimeState s_rt{};
    return s_rt;
}

bool ShouldBypassPersistence() {
    return app::IsReplayOrAutotestActive() || input_rec::IsReplayActive() || input_rec::IsRecordActive() ||
           input_rec::ShouldBypassMkb() || mkb::IsAutotestActive();
}

bool SettingsValuesEqual(const GuestUserSettings& a, const GuestUserSettings& b) {
    return a.sfx_volume == b.sfx_volume && a.music_volume == b.music_volume &&
           a.controller_layout == b.controller_layout && a.controller_invert_y == b.controller_invert_y &&
           a.controller_invert_x == b.controller_invert_x && a.yaw_speed == b.yaw_speed &&
           a.pitch_speed == b.pitch_speed && a.vibration == b.vibration && a.look_spring == b.look_spring &&
           a.camera_config == b.camera_config && a.quick_tips == b.quick_tips &&
           a.hud_visible == b.hud_visible && a.auto_hide_hud == b.auto_hide_hud;
}

}  // namespace

GuestUserSettings MakeDefaultSettings() { return GuestUserSettings{}; }

GuestUserSettings SanitizeSettings(const GuestUserSettings& s) {
    GuestUserSettings out = s;
    out.sfx_volume = std::clamp<int32_t>(out.sfx_volume, 0, 100);
    out.music_volume = std::clamp<int32_t>(out.music_volume, 0, 100);
    if (out.controller_layout < 2u || out.controller_layout > 4u) {
        out.controller_layout = 2u;
    }
    out.yaw_speed = std::clamp<int32_t>(out.yaw_speed, 0, 100);
    out.pitch_speed = std::clamp<int32_t>(out.pitch_speed, 0, 100);
    if (out.camera_config > 2u) {
        out.camera_config = 1u;
    }
    return out;
}

GuestUserSettings GetSettings() {
    RuntimeState& rt = Rt();
    std::lock_guard<std::mutex> lk(rt.mtx);
    return rt.current;
}

void SetSettingsFromConfig(const GuestUserSettings& s) {
    RuntimeState& rt = Rt();
    const GuestUserSettings clean = SanitizeSettings(s);
    {
        std::lock_guard<std::mutex> lk(rt.mtx);
        rt.current = clean;
        rt.last_saved = clean;
    }
    rt.pending_guest_sync.store(clean.has_saved_values, std::memory_order_release);
}

void ApplyUiSettings(const GuestUserSettings& s) {
    RuntimeState& rt = Rt();
    GuestUserSettings clean = SanitizeSettings(s);
    clean.has_saved_values = true;
    {
        std::lock_guard<std::mutex> lk(rt.mtx);
        rt.current = clean;
        rt.last_saved = clean;
    }
    rt.pending_guest_sync.store(true, std::memory_order_release);
}

void ResetToDefaults(bool push_to_guest) {
    RuntimeState& rt = Rt();
    GuestUserSettings def = MakeDefaultSettings();
    def.has_saved_values = push_to_guest;
    {
        std::lock_guard<std::mutex> lk(rt.mtx);
        rt.current = def;
        rt.last_saved = def;
    }
    rt.pending_guest_sync.store(push_to_guest, std::memory_order_release);
}

bool ConsumePendingHostSaveRequest() {
    if (ShouldBypassPersistence()) return false;
    return Rt().pending_host_save.exchange(false, std::memory_order_acq_rel);
}

void NotifyGuestCapturedSettings(const GuestUserSettings& captured, bool defer_host_save_to_ui) {
    if (ShouldBypassPersistence()) return;
    RuntimeState& rt = Rt();
    GuestUserSettings clean = SanitizeSettings(captured);

    if (!clean.has_saved_values) {
        std::lock_guard<std::mutex> lk(rt.mtx);
        rt.current = clean;
        rt.last_saved = clean;
        return;
    }

    bool should_save = false;
    {
        std::lock_guard<std::mutex> lk(rt.mtx);
        const bool values_changed = !SettingsValuesEqual(clean, rt.last_saved) || !rt.last_saved.has_saved_values;
        rt.current = clean;
        if (values_changed) {
            rt.last_saved = clean;
            should_save = true;
        }
    }

    if (should_save) {
        if (defer_host_save_to_ui) {
            rt.pending_host_save.store(true, std::memory_order_release);
        } else {
            rt.pending_host_save.store(false, std::memory_order_release);
            app::Save();
        }
    }
}

bool ConsumePendingGuestSync(GuestUserSettings* out) {
    RuntimeState& rt = Rt();
    if (!rt.pending_guest_sync.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(rt.mtx);
    if (!rt.pending_guest_sync.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    if (!rt.current.has_saved_values) {
        return false;
    }
    if (out != nullptr) {
        *out = rt.current;
    }
    return true;
}

}  // namespace psiops::guest_settings
