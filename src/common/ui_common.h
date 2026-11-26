// SPDX-License-Identifier: GPL-2.0-or-later
// Shared Mindgate/Psi-Ops ImGui dark visual theme, font loading, BMP capture writer,
// gamepad navigation helpers, and live-synced Settings panels shared between
// the 6-Tab Pre-Game Launcher and the in-game F1 / BACK+L3 Overlay.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/update_checker.h"
#include "common/version.h"
#include "upscale/upscaler_common.h"

namespace ui_common {

// Applies the dark Mindgate/Psi-Ops cyan-slate ImGui theme scaled to dpi_scale.
void ApplyMindgateTheme(float dpi_scale);

// Loads a crisp OS sans-serif font (Segoe UI on Win64, DejaVu/Liberation Sans on Linux) scaled by dpi_scale.
void LoadUiFont(ImGuiIO& io, float dpi_scale);

// Writes a 32-bit uncompressed BMP file (supports both BGRA and RGBA pixel layouts, top-down or bottom-up).
bool WriteBmp32File(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* pixels,
                    uint32_t row_pitch, bool is_rgba, bool top_down);

// Draws a compact color-coded pill badge (e.g. "[Ready]", "[Verified Retail]", "[Fallback]").
void DrawBadge(const char* text, const ImVec4& bg_color, const ImVec4& text_color = ImVec4(0.96f, 0.98f, 1.0f, 1.0f));

// Shared Tab Panels (used identically in Pre-Game Launcher and Live F1 Overlay).
void DrawUpscalingGraphicsPanel(const upscale::PluginProbe& probe, bool in_game, bool& dirty,
                                bool& needs_display_apply);
void DrawDisplayGpuPanel(const std::vector<app::GpuAdapterInfo>& gpus,
                         const std::vector<app::MonitorInfo>& monitors, bool in_game, bool& dirty,
                         bool& needs_display_apply);
void DrawAudioPanel(bool& dirty);
void DrawToolsDiagnosticsPanel(const upscale::PluginProbe& probe, bool in_game, std::string& action_status);

// Gamepad poll edge-detector for UI navigation (D-Pad, A/B/X/Y, LB/RB tab switch, Start).
struct GamepadNavEvents {
    bool tab_prev = false;
    bool tab_next = false;
    bool start_pressed = false;
    bool back_pressed = false;
    bool confirm_pressed = false;
    bool cancel_pressed = false;
};
GamepadNavEvents PollGamepadNavToImGui(ImGuiIO& io);
void ResetGamepadNavState(bool prime_held = true);

// Sets ImGuiBackendFlags_HasGamepad while a controller is connected and clears it on disconnect (ImGui only
// runs gamepad navigation with that flag). Call between the platform backend's NewFrame and ImGui::NewFrame.
void SyncGamepadBackendFlag(ImGuiIO& io);

// Releases every gamepad key previously fed by PollGamepadNavToImGui and re-primes the edge detector, so a
// button held while a UI closes can't stay latched "down" or fire a spurious press when the UI reopens.
void ReleaseGamepadNavKeys(ImGuiIO& io);

// Cross-platform UTF-8 / CLI / Gamescope helpers.
#ifdef _WIN32
std::wstring Utf8ToWideStr(std::string_view s);
std::string WideToUtf8Str(std::wstring_view w);
#endif
std::vector<std::string> GetProcessArgsUtf8();
bool IsGamescope();

// Persistent update-checker preferences stored in settings.toml.
struct UpdateUiPrefs {
    bool check_updates_on_startup = true;
    std::string dismissed_update_tag;
};
UpdateUiPrefs& GetUpdatePrefs();
void LoadUpdatePrefs(const void* toml_table);
void SaveUpdatePrefs(void* toml_table);

// Update Checker & Self-Updater UI components shared by Pre-Game Launcher and In-Game F1 Overlay.
struct UpdateCardContext {
    bool in_launcher = true;
    float ui_scale = 1.0f;
    std::function<void()> on_update_now;
    std::function<void()> on_apply_and_restart;
    std::function<void(const std::string&)> on_open_release_page;
    std::function<void(const std::string&)> on_dismiss;
};

std::string SanitizeUiText(const std::string& s, bool single_line = true, size_t max_chars = 160);
bool ShouldShowUpdateBanner(const psiops::UpdateStatusSnapshot& snap, const std::string& dismissed_tag);
void DrawLauncherHeaderUpdateButton(const psiops::UpdateStatusSnapshot& snap, float ui_scale,
                                    const std::function<void()>& on_manual_check);
void DrawUpdateAlertCard(const psiops::UpdateStatusSnapshot& snap, const UpdateCardContext& uctx);
void DrawUpdatesDiagnosticsSection(const psiops::UpdateStatusSnapshot& snap, bool in_launcher, float ui_scale,
                                   bool& dirty, bool& manual_pending, std::string& status_msg,
                                   const std::function<void()>& on_apply_and_restart);

}  // namespace ui_common
