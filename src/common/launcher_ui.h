// SPDX-License-Identifier: GPL-2.0-or-later
// 6-Tab Pre-Game Graphical Launcher (Win32 + D3D12 PerMonitorV2 & Linux SDL3 SDL_Renderer 1280x800).
// Torn down cleanly before game boot; guarantees zero settings.toml writes on unconfigured Exit.

#pragma once

namespace launcher_ui {

// Entry point assigned to app::kGame.launcher.
// Returns true to launch Psi-Ops, or false to exit cleanly without booting the game.
bool RunLauncher(void* hinstance);

}  // namespace launcher_ui
