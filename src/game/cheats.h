// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace psiops::cheats {

enum class CheatToggle : uint8_t {
    GodMode = 0,              // Nick takes zero damage (sub_000FEF40) and stays at Max Health (+0x770 = +0x76C)
    InfinitePsi = 1,          // Psi energy stays at 100% (+0x778 = +0x774, +0x7C6 = 1) and never drains
    BottomlessClip = 2,       // Active & owned weapon magazines never deplete (wpn+0x50 = 1)
    InfiniteReserveAmmo = 3,  // Reserve AmmoPool (R32(wpn+0x44)) stays at max capacity across owned weapons
    SuperPsi = 4,             // Enhanced Psi power damage & telekinetic force (player+0x7C9 = 1)
    OneHitKills = 5,          // Hostile enemies (AI_Minion 0x54) die in one hit; excludes Player, MC puppet, scripted NPCs (+0x4BA), & Bosses (0x5E)
    Count = 6
};

inline constexpr size_t kCheatToggleCount = static_cast<size_t>(CheatToggle::Count);

struct CheatToggleInfo {
    CheatToggle id;
    const char* key;
    const char* label;
    const char* description;
};

enum class CheatAction : uint8_t {
    FullHealAndPsi = 0,       // Restore 100% Health & 100% Psi Energy
    RefillAllAmmo = 1,        // Fill magazine & reserve ammo to max for all owned weapons
    UnlockAllPsiPowers = 2,   // Grant all 6 Psi Powers (TK, RV, MD, PK, MC, AV) via sub_000DC630(GS, 3, 1)
    GiveAllWeaponsAndAmmo = 3,// Grant full weapon arsenal via sub_000DC630(GS, 7, 1) + max ammo
    Count = 4
};

inline constexpr size_t kCheatActionCount = static_cast<size_t>(CheatAction::Count);

struct CheatActionInfo {
    CheatAction id;
    const char* label;
    const char* description;
};

struct CheatRuntimeStatus {
    bool replay_active = false;
    bool in_level = false;
    bool player_alive = false;
    bool mind_controlling = false;
    float health = 0.0f;
    float max_health = 100.0f;
    float psi = 0.0f;
    float max_psi = 100.0f;
    int32_t clip_ammo = 0;
    int32_t max_clip_ammo = 0;
    int32_t reserve_ammo = 0;
    int32_t max_reserve_ammo = 0;
    uint32_t owned_weapon_count = 0;
    uint32_t active_toggle_count = 0;
    uint64_t last_action_seq = 0;
    bool last_action_is_warning = false;
    std::string last_action_feedback;
};

const std::array<CheatToggleInfo, kCheatToggleCount>& GetToggleInfos();
const std::array<CheatActionInfo, kCheatActionCount>& GetActionInfos();

void SetToggle(CheatToggle toggle, bool enabled);
bool GetToggle(CheatToggle toggle);
bool IsToggleEffective(CheatToggle toggle);
bool ToggleCheat(CheatToggle toggle);
void DisableAllCheats();
uint32_t GetActiveToggleCount();
bool ConsumePendingHostSaveRequest();

void EnqueueAction(CheatAction action);
void ClearActionFeedback();
CheatRuntimeStatus GetRuntimeStatus();

// TOML persistence & guest-thread backend hooks (used by game_info.cpp & hooks.cpp):
void LoadSettings(const void* toml_table);
void SaveSettings(void* toml_table);
bool ConsumeNextQueuedAction(CheatAction* out_action);
void RecordActionFeedback(bool is_warning, std::string message);
void PublishRuntimeStatus(const CheatRuntimeStatus& status);

}  // namespace psiops::cheats
