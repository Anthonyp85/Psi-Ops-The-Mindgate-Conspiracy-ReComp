// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace psiops::guest_settings {

struct GuestUserSettings {
    bool has_saved_values = false;

    // Audio (GlobalIntMap opt[4], opt[5] @ 0x005ECD4C, 0x005ECD50; 0..100, default 100)
    int32_t sfx_volume = 100;
    int32_t music_volume = 100;

    // Controller & Aiming
    uint32_t controller_layout = 2u;    // 0x005ECEB4: 2 = Config 1 (default), 3 = Config 2, 4 = Config 3
    bool controller_invert_y = true;    // opt[8] @ 0x005ECD5C: true = Inverted (guest 0, Xbox default), false = Normal (guest 1)
    bool controller_invert_x = false;   // opt[9] @ 0x005ECD60: false = Normal (guest 0, default), true = Inverted (guest 1)
    int32_t yaw_speed = 44;             // opt[13] @ 0x005ECD70: 0..100, default 44
    int32_t pitch_speed = 43;           // opt[14] @ 0x005ECD74: 0..100, default 43
    bool vibration = true;              // bool_opt[2] @ 0x005ECEA2 (+ RumbleMgr): default true
    bool look_spring = true;            // bool_opt[18] @ 0x005ECEB8 (synced with g_saved_look_spring): default true

    // Camera & HUD
    uint32_t camera_config = 1u;        // 0x005ECEB0: 0 = Camera A, 1 = Camera B (default), 2 = Camera C
    bool quick_tips = true;             // bool_opt[16] @ 0x005ECEA3: default true
    bool hud_visible = true;            // bool_opt[19] @ 0x005ECEA5: default true
    bool auto_hide_hud = false;         // bool_opt[21] @ 0x005ECEAB: default false
};

GuestUserSettings MakeDefaultSettings();
GuestUserSettings SanitizeSettings(const GuestUserSettings& s);
GuestUserSettings GetSettings();
void SetSettingsFromConfig(const GuestUserSettings& s);
void ApplyUiSettings(const GuestUserSettings& s);   // sanitizes, sets has_saved_values=true, queues guest sync
void ResetToDefaults(bool push_to_guest = true);
bool ConsumePendingHostSaveRequest();

// Guest-hook side helpers (used by src/game/hooks.cpp on the guest thread):
void NotifyGuestCapturedSettings(const GuestUserSettings& captured, bool defer_host_save_to_ui);
bool ConsumePendingGuestSync(GuestUserSettings* out);

}  // namespace psiops::guest_settings
