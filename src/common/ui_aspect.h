// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace psiops::ui_aspect {

enum class Mode : uint8_t {
    AspectCorrect = 0,     // Default: 4:3 pillarboxed menus/modals/scope + 4:3 1:1 HUD + 1:1 crosshair
    MenusAndCrosshair = 1, // 4:3 pillarboxed menus/pause/modals/scope + 1:1 crosshair; 16:9 full-width gameplay HUD
    Stretched16x9 = 2,     // Original Xbox anamorphic widescreen (stretched 2D UI & crosshair)
    Count = 3
};

void SetMode(Mode mode);
Mode GetMode();
const char* ModeToKey(Mode mode);         // "aspect_correct", "menus_and_crosshair", "stretched"
const char* ModeLabel(Mode mode);
const char* ModeDescription(Mode mode);

// Published by guest hooks for host mouse mapping (true when widescreen is active AND Layer 0x17 is 4:3 pillarboxed).
void SetMenuPillarboxed43Active(bool active);
bool IsCurrentMenuPillarboxed43();

void LoadSettings(const void* toml_table);
void SaveSettings(void* toml_table);

}  // namespace psiops::ui_aspect
