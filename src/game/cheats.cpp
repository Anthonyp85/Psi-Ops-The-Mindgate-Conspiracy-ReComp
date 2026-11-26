// SPDX-License-Identifier: GPL-2.0-or-later
// Thread-safe host state, action queue, runtime telemetry snapshot, and [cheats] TOML persistence
// for Psi-Ops: The Mindgate Conspiracy (F2 Cheat Menu).

#include "game/cheats.h"

#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/input_recorder.h"
#include "common/mkb_input.h"

namespace psiops::cheats {

namespace {

const std::array<CheatToggleInfo, kCheatToggleCount> kToggleInfos = {{
    {CheatToggle::GodMode, "god_mode", "God Mode (Invulnerable)",
     "Nick takes zero damage from bullets, melee, fire, explosions, and hazards, staying at 100% Health."},
    {CheatToggle::InfinitePsi, "infinite_psi", "Infinite Psi Energy",
     "Psi energy stays at 100% and never drains while using TK, PK, RV, MD, MC, or AV."},
    {CheatToggle::BottomlessClip, "bottomless_clip", "Bottomless Clip (No Reload)",
     "Equipped and owned weapon magazines stay full and never need reloading."},
    {CheatToggle::InfiniteReserveAmmo, "infinite_reserve_ammo", "Infinite Reserve Ammo",
     "Reserve ammo pools stay at maximum capacity across all owned weapons."},
    {CheatToggle::SuperPsi, "super_psi", "Super Psi Power",
     "Enables Psi-Ops' built-in Super Psi amplifier for boosted telekinetic & pyrokinetic power."},
    {CheatToggle::OneHitKills, "one_hit_kills", "One-Hit Kills (Hostiles Only)",
     "Normal combat enemies die in one hit from weapons, melee, TK throws, or PK fire (Bosses, MC puppets, & scripted NPCs protected)."},
}};

const std::array<CheatActionInfo, kCheatActionCount> kActionInfos = {{
    {CheatAction::FullHealAndPsi, "Full Heal + Full Psi",
     "Instantly restore Nick's Health and Psi Energy to 100%."},
    {CheatAction::RefillAllAmmo, "Refill All Weapon Ammo",
     "Fill magazines and reserve ammo pools to maximum for all owned weapons."},
    {CheatAction::UnlockAllPsiPowers, "Unlock All 6 Psi Powers",
     "Grant Telekinesis, Remote View, Mind Drain, Pyrokinesis, Mind Control, and Aura View in the current mission."},
    {CheatAction::GiveAllWeaponsAndAmmo, "Give All Weapons & Max Ammo",
     "Grant the full weapon arsenal and fill every magazine and reserve pool."},
}};

struct CheatsState {
    std::array<std::atomic<bool>, kCheatToggleCount> toggles{};
    std::atomic<bool> has_saved_values{false};
    std::atomic<bool> pending_host_save{false};

    std::mutex mtx;
    std::vector<CheatAction> action_queue;
    CheatRuntimeStatus status{};
    uint64_t feedback_seq = 0;
    bool feedback_is_warning = false;
    std::string feedback_msg;
};

CheatsState& St() {
    static CheatsState s{};
    return s;
}

bool ShouldSuppressCheats() {
    return app::IsReplayOrAutotestActive() || input_rec::IsReplayActive() || input_rec::IsRecordActive() ||
           input_rec::ShouldBypassMkb() || mkb::IsAutotestActive();
}

uint32_t CountRawToggles(const CheatsState& st) {
    uint32_t n = 0u;
    for (const auto& t : st.toggles) {
        if (t.load(std::memory_order_relaxed)) ++n;
    }
    return n;
}

void RequestDeferredSave(CheatsState& st) {
    st.has_saved_values.store(true, std::memory_order_release);
    if (!ShouldSuppressCheats()) {
        st.pending_host_save.store(true, std::memory_order_release);
    }
}

}  // namespace

const std::array<CheatToggleInfo, kCheatToggleCount>& GetToggleInfos() { return kToggleInfos; }

const std::array<CheatActionInfo, kCheatActionCount>& GetActionInfos() { return kActionInfos; }

void SetToggle(CheatToggle toggle, bool enabled) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return;
    CheatsState& st = St();
    const bool prev = st.toggles[idx].exchange(enabled, std::memory_order_acq_rel);
    if (prev != enabled) {
        RequestDeferredSave(st);
    }
}

bool GetToggle(CheatToggle toggle) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return false;
    return St().toggles[idx].load(std::memory_order_acquire);
}

bool IsToggleEffective(CheatToggle toggle) {
    if (ShouldSuppressCheats()) return false;
    return GetToggle(toggle);
}

bool ToggleCheat(CheatToggle toggle) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return false;
    CheatsState& st = St();
    bool prev = st.toggles[idx].load(std::memory_order_relaxed);
    while (!st.toggles[idx].compare_exchange_weak(prev, !prev, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    RequestDeferredSave(st);
    return !prev;
}

void DisableAllCheats() {
    CheatsState& st = St();
    bool any_was_on = false;
    for (auto& flag : st.toggles) {
        if (flag.exchange(false, std::memory_order_acq_rel)) {
            any_was_on = true;
        }
    }
    if (any_was_on || st.has_saved_values.load(std::memory_order_acquire)) {
        RequestDeferredSave(st);
    }
    RecordActionFeedback(false, "All persistent cheats disabled.");
}

uint32_t GetActiveToggleCount() {
    if (ShouldSuppressCheats()) return 0u;
    return CountRawToggles(St());
}

bool ConsumePendingHostSaveRequest() {
    if (ShouldSuppressCheats()) return false;
    return St().pending_host_save.exchange(false, std::memory_order_acq_rel);
}

void EnqueueAction(CheatAction action) {
    if (static_cast<size_t>(action) >= kCheatActionCount || ShouldSuppressCheats()) return;
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    if (st.action_queue.size() < 32u) {
        st.action_queue.push_back(action);
    }
}

void ClearActionFeedback() {
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.feedback_msg.clear();
    st.feedback_is_warning = false;
}

CheatRuntimeStatus GetRuntimeStatus() {
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    CheatRuntimeStatus out = st.status;
    out.replay_active = ShouldSuppressCheats();
    out.active_toggle_count = out.replay_active ? 0u : CountRawToggles(st);
    out.last_action_seq = st.feedback_seq;
    out.last_action_is_warning = st.feedback_is_warning;
    out.last_action_feedback = st.feedback_msg;
    return out;
}

void LoadSettings(const void* toml_table) {
    CheatsState& st = St();
    for (auto& flag : st.toggles) {
        flag.store(false, std::memory_order_release);
    }
    st.pending_host_save.store(false, std::memory_order_release);

    if (toml_table == nullptr) {
        st.has_saved_values.store(false, std::memory_order_release);
        return;
    }

    const auto& root = *static_cast<const toml::table*>(toml_table);
    if (const auto* ch = root["cheats"].as_table()) {
        for (const CheatToggleInfo& info : kToggleInfos) {
            if (auto v = (*ch)[info.key].value<bool>()) {
                st.toggles[static_cast<size_t>(info.id)].store(*v, std::memory_order_release);
            }
        }
        st.has_saved_values.store(true, std::memory_order_release);
    } else {
        st.has_saved_values.store(false, std::memory_order_release);
    }
}

void SaveSettings(void* toml_table) {
    if (toml_table == nullptr) return;
    auto& root = *static_cast<toml::table*>(toml_table);
    CheatsState& st = St();
    if (st.has_saved_values.load(std::memory_order_acquire)) {
        toml::table ch;
        for (const CheatToggleInfo& info : kToggleInfos) {
            ch.insert(info.key, st.toggles[static_cast<size_t>(info.id)].load(std::memory_order_acquire));
        }
        root.insert_or_assign("cheats", std::move(ch));
    } else {
        root.erase("cheats");
    }
}

bool ConsumeNextQueuedAction(CheatAction* out_action) {
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    if (ShouldSuppressCheats()) {
        st.action_queue.clear();
        return false;
    }
    if (st.action_queue.empty()) return false;
    if (out_action != nullptr) {
        *out_action = st.action_queue.front();
    }
    st.action_queue.erase(st.action_queue.begin());
    return true;
}

void RecordActionFeedback(bool is_warning, std::string message) {
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    ++st.feedback_seq;
    st.feedback_is_warning = is_warning;
    st.feedback_msg = std::move(message);
}

void PublishRuntimeStatus(const CheatRuntimeStatus& status) {
    CheatsState& st = St();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.status = status;
}

}  // namespace psiops::cheats
