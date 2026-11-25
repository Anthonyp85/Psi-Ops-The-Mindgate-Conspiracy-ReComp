// SPDX-License-Identifier: GPL-2.0-or-later
// Live-Synced In-Game F1 / BACK+L3 Settings Overlay (5 tabs) and Upgraded Live HUD
// (FPS | ms | Upscaler | Render -> Display).

#pragma once

#include <cstdint>

#include "core/input.h"

namespace settings_ui {

// Initializes CLI/env flags (--open-settings / XBE_OPEN_SETTINGS) and caches plugin/GPU probes.
void Init();

// Lock-free visibility query used by GPU backends to skip ImGui frame overhead when UI is closed.
bool HasVisibleUi();

// Detects gamepad BACK + L3 chord to toggle the live F1 Settings Overlay.
void OnPadPoll(int port, const input::Pad& pad);

// Draws the Live HUD and (when open) the 5-tab Live-Synced Settings Overlay inside app::kGame.ui.
void Draw(double fps);

}  // namespace settings_ui
