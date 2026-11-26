// SPDX-License-Identifier: GPL-2.0-or-later
// Shared Mindgate/Psi-Ops ImGui dark visual theme, font loading, BMP capture writer,
// gamepad navigation helpers, and live-synced Settings panels shared between
// the 6-Tab Pre-Game Launcher and the in-game F1 / BACK+L3 Overlay.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "upscale/upscaler_common.h"

#ifndef PSIOPS_VERSION_STR
#define PSIOPS_VERSION_STR "1.1.0"
#endif

#define PSIOPS_WIDE_IMPL(s) L##s
#define PSIOPS_WIDE(s) PSIOPS_WIDE_IMPL(s)
#define PSIOPS_WVERSION_STR PSIOPS_WIDE(PSIOPS_VERSION_STR)

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

}  // namespace ui_common
