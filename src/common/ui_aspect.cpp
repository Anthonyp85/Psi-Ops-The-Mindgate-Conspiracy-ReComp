// SPDX-License-Identifier: GPL-2.0-or-later
// Thread-safe host configuration, live menu pillarbox state, and [ui_aspect] TOML persistence
// for widescreen 2D UI, menu pillarboxing, and 1:1 circular weapon crosshairs.

#include "common/ui_aspect.h"

#include <atomic>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

namespace psiops::ui_aspect {

namespace {

std::atomic<uint8_t> g_mode{static_cast<uint8_t>(Mode::AspectCorrect)};
std::atomic<bool> g_menu_pillarboxed_43{false};

Mode SanitizeMode(Mode mode) {
    return (static_cast<uint8_t>(mode) < static_cast<uint8_t>(Mode::Count)) ? mode : Mode::AspectCorrect;
}

Mode ParseModeKey(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isspace(uc)) {
            s.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    if (s == "menus_and_crosshair" || s == "menus_crosshair" || s == "hybrid" || s == "widescreen_hud") {
        return Mode::MenusAndCrosshair;
    }
    if (s == "stretched" || s == "stretched_16x9" || s == "16:9" || s == "16x9" || s == "original" || s == "xbox") {
        return Mode::Stretched16x9;
    }
    return Mode::AspectCorrect;
}

}  // namespace

void SetMode(Mode mode) {
    const Mode clean = SanitizeMode(mode);
    g_mode.store(static_cast<uint8_t>(clean), std::memory_order_release);
    if (clean == Mode::Stretched16x9) {
        g_menu_pillarboxed_43.store(false, std::memory_order_release);
    }
}

Mode GetMode() {
    const uint8_t raw = g_mode.load(std::memory_order_acquire);
    return (raw < static_cast<uint8_t>(Mode::Count)) ? static_cast<Mode>(raw) : Mode::AspectCorrect;
}

const char* ModeToKey(Mode mode) {
    switch (SanitizeMode(mode)) {
        case Mode::AspectCorrect: return "aspect_correct";
        case Mode::MenusAndCrosshair: return "menus_and_crosshair";
        case Mode::Stretched16x9: return "stretched";
        case Mode::Count:
        default: return "aspect_correct";
    }
}

const char* ModeLabel(Mode mode) {
    switch (SanitizeMode(mode)) {
        case Mode::AspectCorrect:
            return "Aspect-Correct 4:3 UI & 1:1 Crosshair (Recommended)";
        case Mode::MenusAndCrosshair:
            return "4:3 Menus & 1:1 Crosshair + 16:9 Edge HUD";
        case Mode::Stretched16x9:
            return "Stretched 16:9 (Original Xbox Widescreen)";
        case Mode::Count:
        default:
            return "Aspect-Correct 4:3 UI & 1:1 Crosshair (Recommended)";
    }
}

const char* ModeDescription(Mode mode) {
    switch (SanitizeMode(mode)) {
        case Mode::AspectCorrect:
            return "Pillarboxes 2D menus, modals, and Sniper Scope to 4:3 with black sidebars, keeps HUD widgets at "
                   "unstretched 4:3 proportions, and renders circular 1:1 weapon crosshairs.";
        case Mode::MenusAndCrosshair:
            return "Pillarboxes 2D menus, pause/modal screens, and Sniper Scope to 4:3 with black sidebars and renders "
                   "circular 1:1 crosshairs, while keeping active gameplay HUD bars at the 16:9 edges.";
        case Mode::Stretched16x9:
            return "Preserves Original Xbox anamorphic widescreen behavior (stretches 4:3 2D menus, HUD widgets, and "
                   "crosshairs across the 16:9 viewport).";
        case Mode::Count:
        default:
            return "";
    }
}

void SetMenuPillarboxed43Active(bool active) {
    g_menu_pillarboxed_43.store(active && (GetMode() != Mode::Stretched16x9), std::memory_order_release);
}

bool IsCurrentMenuPillarboxed43() {
    return (GetMode() != Mode::Stretched16x9) && g_menu_pillarboxed_43.load(std::memory_order_acquire);
}

void LoadSettings(const void* toml_table) {
    Mode parsed = Mode::AspectCorrect;
    if (toml_table != nullptr) {
        const auto& root = *static_cast<const toml::table*>(toml_table);
        if (const auto* tbl = root["ui_aspect"].as_table()) {
            if (auto m = (*tbl)["mode"].value<std::string>()) {
                parsed = ParseModeKey(*m);
            }
        } else if (auto direct = root["ui_aspect"].value<std::string>()) {
            parsed = ParseModeKey(*direct);
        }
    }
    SetMode(parsed);
}

void SaveSettings(void* toml_table) {
    if (toml_table == nullptr) return;
    auto& root = *static_cast<toml::table*>(toml_table);
    const Mode mode = GetMode();
    if (mode == Mode::AspectCorrect) {
        // Omit [ui_aspect] on default AspectCorrect (and erase any prior [ui_aspect] entry on roundtrip back to default).
        root.erase("ui_aspect");
    } else {
        toml::table tbl;
        tbl.insert("mode", ModeToKey(mode));
        root.insert_or_assign("ui_aspect", std::move(tbl));
    }
}

}  // namespace psiops::ui_aspect
