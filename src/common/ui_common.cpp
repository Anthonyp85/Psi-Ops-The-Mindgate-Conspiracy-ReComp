// SPDX-License-Identifier: GPL-2.0-or-later
// Shared Mindgate/Psi-Ops ImGui theme, font loader, BMP writer, gamepad nav helper,
// and live-synced Settings panels for Pre-Game Launcher and in-game F1 Overlay.

#include "common/ui_common.h"
#include "../game/cheats.h"
#include "../game/guest_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>

#include "audio/audio.h"
#include "core/input.h"
#include "gpu/backend.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ui_common {

namespace {

namespace fs = std::filesystem;

constexpr ImVec4 kBadgeGreen(0.11f, 0.45f, 0.28f, 1.0f);
constexpr ImVec4 kBadgeCyan(0.10f, 0.42f, 0.52f, 1.0f);
constexpr ImVec4 kBadgeAmber(0.55f, 0.36f, 0.08f, 1.0f);
constexpr ImVec4 kBadgeGray(0.24f, 0.28f, 0.34f, 1.0f);

void SectionHeading(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

}  // namespace

void ApplyMindgateTheme(float dpi_scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);

    st.WindowRounding = 8.0f;
    st.ChildRounding = 6.0f;
    st.FrameRounding = 5.0f;
    st.PopupRounding = 6.0f;
    st.ScrollbarRounding = 6.0f;
    st.GrabRounding = 5.0f;
    st.TabRounding = 6.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(14.0f, 12.0f);
    st.FramePadding = ImVec2(10.0f, 6.0f);
    st.ItemSpacing = ImVec2(10.0f, 8.0f);
    st.ScrollbarSize = 14.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.62f, 0.70f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.075f, 0.105f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.080f, 0.108f, 0.150f, 0.95f);
    c[ImGuiCol_PopupBg] = ImVec4(0.070f, 0.095f, 0.135f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.16f, 0.27f, 0.38f, 0.85f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.07f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.24f, 0.85f, 0.96f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.76f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.32f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.22f, 0.33f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.36f, 0.50f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.52f, 0.66f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.26f, 0.38f, 0.90f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.38f, 0.52f, 0.95f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.50f, 0.66f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.18f, 0.30f, 0.42f, 0.80f);
    c[ImGuiCol_Tab] = ImVec4(0.09f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.16f, 0.36f, 0.50f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.44f, 0.58f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.16f, 0.24f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.07f, 0.10f, 0.14f, 0.50f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.09f, 0.13f, 0.18f, 0.65f);
    c[ImGuiCol_NavHighlight] = ImVec4(0.28f, 0.88f, 0.98f, 1.00f);

    float sc = std::clamp(dpi_scale, 0.85f, 2.5f);
    st.ScaleAllSizes(sc);
}

void LoadUiFont(ImGuiIO& io, float dpi_scale) {
    float sc = std::clamp(dpi_scale, 0.85f, 2.5f);
    float px = std::round(16.0f * sc);
    std::error_code ec;
    // Safe to call again on DPI change: rebuild the atlas from scratch (callers invalidate the backend's font
    // texture before and recreate it after).
    io.Fonts->Clear();

#ifdef _WIN32
    static const char* const kFonts[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
#else
    static const char* const kFonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    };
#endif

    for (const char* fpath : kFonts) {
        if (fs::is_regular_file(fpath, ec)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            if (io.Fonts->AddFontFromFileTTF(fpath, px, &cfg)) return;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = px;
    io.Fonts->AddFontDefault(&cfg);
}

bool WriteBmp32File(const fs::path& path, uint32_t width, uint32_t height, const uint8_t* pixels, uint32_t row_pitch,
                    bool is_rgba, bool top_down) {
    if (!pixels || width == 0 || height == 0) return false;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    const uint32_t dst_stride = width * 4u;
    const uint32_t img_bytes = dst_stride * height;
    const uint32_t file_bytes = 54u + img_bytes;

    uint8_t hdr[54] = {};
    hdr[0] = 'B';
    hdr[1] = 'M';
    std::memcpy(hdr + 2, &file_bytes, 4);
    uint32_t off = 54u;
    std::memcpy(hdr + 10, &off, 4);
    uint32_t dib = 40u;
    std::memcpy(hdr + 14, &dib, 4);
    int32_t w_i = int32_t(width);
    int32_t h_i = int32_t(height);  // positive = standard bottom-up BMP
    std::memcpy(hdr + 18, &w_i, 4);
    std::memcpy(hdr + 22, &h_i, 4);
    uint16_t planes = 1, bpp = 32;
    std::memcpy(hdr + 26, &planes, 2);
    std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &img_bytes, 4);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

    std::vector<uint8_t> row(dst_stride);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t src_y = top_down ? (height - 1u - y) : y;
        const uint8_t* src = pixels + size_t(src_y) * row_pitch;
        if (is_rgba) {
            for (uint32_t x = 0; x < width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 2];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 0];
                row[x * 4 + 3] = 255;
            }
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 0];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 2];
                row[x * 4 + 3] = 255;
            }
        }
        f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    return bool(f);
}

void DrawBadge(const char* text, const ImVec4& bg_color, const ImVec4& text_color) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_color);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::SmallButton(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void DrawUpscalingGraphicsPanel(const upscale::PluginProbe& probe, bool in_game, bool& dirty,
                                bool& needs_display_apply) {
    app::Settings& s = app::Get();

    SectionHeading("Hardware Presets");
    static constexpr app::HardwarePreset kPresets[6] = {
        app::HardwarePreset::SteamDeck, app::HardwarePreset::HandheldIgpu, app::HardwarePreset::Mainstream1080p,
        app::HardwarePreset::High1440p, app::HardwarePreset::Ultra4k, app::HardwarePreset::Custom};
    static constexpr const char* kPresetShort[6] = {
        "Steam Deck (1280x800)", "Handheld / iGPU", "1080p Mainstream", "1440p High", "4K Ultra", "Custom"};
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine();
        // Snapshot before the button: clicking it changes s.hardware_preset, and Push/Pop must stay paired.
        const bool active = (s.hardware_preset == kPresets[i]);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.50f, 0.64f, 1.0f));
        }
        if (ImGui::Button(kPresetShort[i])) {
            if (kPresets[i] == app::HardwarePreset::Custom) {
                s.hardware_preset = app::HardwarePreset::Custom;
            } else {
                upscale::ApplyHardwarePreset(s, kPresets[i], probe);
                needs_display_apply = true;
            }
            app::MarkHardwarePresetUserEdited();
            dirty = true;
        }
        if (active) ImGui::PopStyleColor();
    }

    SectionHeading("9-Upscaler Suite & Resolution Scaling");

    // Upscaler backend selector with live readiness badges
    const char* cur_mode_label = upscale::ModeName(s.upscaler);
    if (ImGui::BeginCombo("Upscaler Backend", cur_mode_label)) {
        for (uint32_t i = 0; i < 9; ++i) {
            auto m = app::UpscalerMode(i);
            bool ready = true;
            const char* tag = " [Ready]";
            if (m == app::UpscalerMode::Dlss) {
                ready = probe.dlss_ready;
                tag = ready ? " [Ready]" : " [Fallback -> FSR 1.0]";
            } else if (m == app::UpscalerMode::Fsr3) {
                ready = probe.fsr3_ready;
                tag = ready ? " [Ready]" : " [Fallback -> FSR 1.0]";
            } else if (m == app::UpscalerMode::Xess) {
                ready = probe.xess_ready;
                tag = ready ? " [Ready]" : " [Fallback -> FSR 1.0]";
            }
            std::string item = std::string(upscale::ModeName(m)) + tag;
            bool selected = (s.upscaler == m);
            if (ImGui::Selectable(item.c_str(), selected)) {
                s.upscaler = m;
                s.hardware_preset = app::HardwarePreset::Custom;
                app::MarkUpscalerUserEdited();
                dirty = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Live badge row for the selected backend
    ImGui::SameLine();
    if (!upscale::IsTemporalMode(s.upscaler)) {
        DrawBadge("Built-In [Ready]", kBadgeGreen);
    } else if ((s.upscaler == app::UpscalerMode::Dlss && probe.dlss_ready) ||
               (s.upscaler == app::UpscalerMode::Fsr3 && probe.fsr3_ready) ||
               (s.upscaler == app::UpscalerMode::Xess && probe.xess_ready)) {
        DrawBadge("Plugin [Ready]", kBadgeGreen);
    } else {
        DrawBadge("Auto-Fallback -> FSR 1.0", kBadgeAmber);
    }

    // Quality tier selector
    if (ImGui::BeginCombo("Upscale Quality", upscale::QualityName(s.upscale_quality))) {
        for (uint32_t i = 0; i < 7; ++i) {
            auto q = app::UpscaleQuality(i);
            bool selected = (s.upscale_quality == q);
            if (ImGui::Selectable(upscale::QualityName(q), selected)) {
                s.upscale_quality = q;
                s.hardware_preset = app::HardwarePreset::Custom;
                app::MarkUpscalerUserEdited();
                dirty = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (s.upscale_quality == app::UpscaleQuality::Custom) {
        int pct = int(s.render_percent);
        int max_pct = upscale::IsTemporalMode(s.upscaler) ? 100 : 200;
        if (ImGui::SliderInt("Custom Render Scale (%)", &pct, 33, max_pct, "%d%%")) {
            s.render_percent = uint32_t(std::clamp(pct, 33, max_pct));
            s.hardware_preset = app::HardwarePreset::Custom;
            app::MarkUpscalerUserEdited();
            dirty = true;
        }
        if (!upscale::IsTemporalMode(s.upscaler) && pct > 100) {
            ImGui::SameLine();
            DrawBadge("SSAA Supersampling", kBadgeCyan);
        }
    }

    // Live computed internal -> display resolution preview
    app::Mode out_m = app::Output();
    uint32_t disp_w = out_m.width ? out_m.width : 1920u;
    uint32_t disp_h = out_m.height ? out_m.height : 1080u;
    bool wide = (disp_w * 2u >= disp_h * 3u);
    float asp = wide ? (16.0f / 9.0f) : (4.0f / 3.0f);
    uint32_t vp_w = std::max(320u, uint32_t(std::lround(std::min<float>(float(disp_w), float(disp_h) * asp))));
    uint32_t vp_h = std::max(240u, uint32_t(std::lround(float(vp_w) / asp)));
    uint32_t rw = 0, rh = 0;
    upscale::ComputeRenderSize(vp_w, vp_h, 640u, 480u, s.upscaler, s.upscale_quality, s.render_percent,
                               s.aspect_correct_render, rw, rh);
    ImGui::TextDisabled("Internal Render: %ux%u  ->  Viewport: %ux%u (Display: %ux%u)", rw, rh, vp_w, vp_h, disp_w,
                        disp_h);

    if (s.upscaler == app::UpscalerMode::Dlss) {
        if (ImGui::BeginCombo("DLSS 4 Render Preset", upscale::DlssPresetName(s.dlss_preset))) {
            for (uint32_t i = 0; i < 4; ++i) {
                auto dp = app::DlssPreset(i);
                bool selected = (s.dlss_preset == dp);
                if (ImGui::Selectable(upscale::DlssPresetName(dp), selected)) {
                    s.dlss_preset = dp;
                    app::MarkUpscalerUserEdited();
                    dirty = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    int sharp = int(s.upscale_sharpness);
    if (ImGui::SliderInt("Upscaler Sharpness (%)", &sharp, 0, 100, "%d%%")) {
        s.upscale_sharpness = uint32_t(std::clamp(sharp, 0, 100));
        s.hardware_preset = app::HardwarePreset::Custom;
        app::MarkUpscalerUserEdited();
        dirty = true;
    }

    if (ImGui::Checkbox("Square-Pixel 16:9 Aspect-Correct Rendering (Eliminates CRT PAR Stretch)",
                        &s.aspect_correct_render)) {
        app::MarkUpscalerUserEdited();
        dirty = true;
    }

    SectionHeading("Textures & Filtering");
    static constexpr uint32_t kAnisoVals[5] = {0, 2, 4, 8, 16};
    static constexpr const char* kAnisoNames[5] = {"Game Default", "2x", "4x", "8x", "16x"};
    int aniso_idx = 0;
    for (int i = 0; i < 5; ++i) {
        if (s.aniso == kAnisoVals[i]) aniso_idx = i;
    }
    if (ImGui::Combo("Anisotropic Filtering", &aniso_idx, kAnisoNames, 5)) {
        s.aniso = kAnisoVals[aniso_idx];
        app::MarkUpscalerUserEdited();
        dirty = true;
    }
    if (ImGui::Checkbox("Enable Custom Replacement Textures (textures/replacement/)", &s.custom_textures)) {
        dirty = true;
    }
    if (ImGui::Checkbox("Dump Textures on Load (textures/dumps/)", &s.dump_textures)) {
        dirty = true;
    }
    if (s.dump_textures) {
        const char* fmts[] = {"PNG", "DDS"};
        int fmt_i = int(std::min(s.texture_dump_format, 1u));
        if (ImGui::Combo("Texture Dump Format", &fmt_i, fmts, 2)) {
            s.texture_dump_format = uint32_t(fmt_i);
            dirty = true;
        }
    }

    if (in_game) {
        gpu::UpscalerStatus st = gpu::GetUpscalerStatus();
        ImGui::Spacing();
        ImGui::TextDisabled("Live Active Backend: %s (%ux%u -> %ux%u) | %s",
                            st.active_label.empty() ? upscale::ModeShortName(s.upscaler) : st.active_label.c_str(),
                            st.render_w, st.render_h, st.vp_w, st.vp_h, st.status_detail.c_str());
    }
}

void DrawDisplayGpuPanel(const std::vector<app::GpuAdapterInfo>& gpus,
                         const std::vector<app::MonitorInfo>& monitors, bool in_game, bool& dirty,
                         bool& needs_display_apply) {
    app::Settings& s = app::Get();

    SectionHeading("Display Output & Frame Pacing");

    const char* dmodes[] = {"Exclusive Fullscreen", "Borderless Fullscreen", "Windowed"};
    int dm = std::clamp(int(s.display_mode), 0, 2);
    if (ImGui::Combo("Display Mode", &dm, dmodes, 3)) {
        s.display_mode = app::DisplayMode(dm);
        dirty = true;
        needs_display_apply = true;
    }

    // Monitor selector
    std::string mon_label = "Auto (Primary Display)";
    for (const auto& m : monitors) {
        if (!s.monitor.empty() && m.device == s.monitor) {
            mon_label = m.label;
            break;
        }
    }
    if (ImGui::BeginCombo("Target Display", mon_label.c_str())) {
        if (ImGui::Selectable("Auto (Primary Display)", s.monitor.empty())) {
            s.monitor.clear();
            dirty = true;
            needs_display_apply = true;
        }
        for (const auto& m : monitors) {
            bool sel = (s.monitor == m.device);
            // Identical monitor models share a label; scope the ID by the unique device name.
            ImGui::PushID(m.device.c_str());
            if (ImGui::Selectable(m.label.c_str(), sel)) {
                s.monitor = m.device;
                dirty = true;
                needs_display_apply = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Resolution selector
    auto modes = app::EnumerateResolutions(s.monitor);
    char cur_res[64];
    if (s.width == 0 || s.height == 0) std::snprintf(cur_res, sizeof(cur_res), "Desktop Native");
    else std::snprintf(cur_res, sizeof(cur_res), "%ux%u", s.width, s.height);
    if (ImGui::BeginCombo("Window / Fullscreen Resolution", cur_res)) {
        if (ImGui::Selectable("Desktop Native", s.width == 0 && s.height == 0)) {
            s.width = 0;
            s.height = 0;
            dirty = true;
            needs_display_apply = true;
        }
        for (size_t mi = 0; mi < modes.size(); ++mi) {
            const auto& m = modes[mi];
            char lb[64];
            std::snprintf(lb, sizeof(lb), "%ux%u", m.width, m.height);
            bool sel = (s.width == m.width && s.height == m.height);
            // The mode list can repeat a WxH (one entry per refresh rate); scope IDs by index.
            ImGui::PushID(int(mi));
            if (ImGui::Selectable(lb, sel)) {
                s.width = m.width;
                s.height = m.height;
                dirty = true;
                needs_display_apply = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // FPS Cap selector
    static constexpr uint32_t kCaps[7] = {30, 60, 90, 120, 144, app::kFpsCapRefresh, 0};
    static constexpr const char* kCapNames[7] = {"30 FPS (Xbox Original)", "60 FPS (Recommended)", "90 FPS (OLED Deck)",
                                                 "120 FPS", "144 FPS", "Monitor Refresh Rate", "Unlimited"};
    int cap_idx = 1;
    for (int i = 0; i < 7; ++i) {
        if (s.fps_cap == kCaps[i]) cap_idx = i;
    }
    if (ImGui::Combo("Frame Rate Cap", &cap_idx, kCapNames, 7)) {
        s.fps_cap = kCaps[cap_idx];
        app::MarkFpsCapUserEdited();
        dirty = true;
    }

    // Present Mode selector
    const char* pmodes[] = {"Auto (Smart VSync / VRR)", "VSync (Locked)", "VRR / Allow Tearing", "Tear-Free Mailbox"};
    int pm = std::clamp(int(s.present_mode), 0, 3);
    if (ImGui::Combo("Present / VSync Mode", &pm, pmodes, 4)) {
        s.present_mode = app::PresentMode(pm);
        s.vsync = (s.present_mode == app::PresentMode::Auto || s.present_mode == app::PresentMode::VSync);
        app::MarkPresentModeUserEdited();
        dirty = true;
    }

    SectionHeading("Graphics Adapter (GPU)");
    std::string gpu_label = "Auto (High-Performance GPU)";
    for (const auto& g : gpus) {
        if (!s.gpu_adapter.empty() && g.key == s.gpu_adapter) {
            gpu_label = g.name;
            break;
        }
    }
    if (ImGui::BeginCombo("Preferred GPU Adapter", gpu_label.c_str())) {
        if (ImGui::Selectable("Auto (High-Performance GPU)", s.gpu_adapter.empty())) {
            s.gpu_adapter.clear();
            app::MarkGpuAdapterUserEdited();
            dirty = true;
        }
        for (const auto& g : gpus) {
            char lb[256];
            std::snprintf(lb, sizeof(lb), "%s (%llu MB VRAM%s)", g.name.c_str(),
                          (unsigned long long)(g.dedicated_vram >> 20), g.likely_uma ? ", UMA/iGPU" : "");
            bool sel = (s.gpu_adapter == g.key);
            // Two identical GPUs produce the same label; scope the ID by the unique adapter key.
            ImGui::PushID(g.key.c_str());
            if (ImGui::Selectable(lb, sel)) {
                s.gpu_adapter = g.key;
                app::MarkGpuAdapterUserEdited();
                dirty = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (in_game) {
        gpu::GpuCaps caps = gpu::GetGpuCaps();
        if (!caps.adapter_name.empty()) {
            ImGui::TextDisabled("Active Runtime GPU: %s (%04X:%04X, %llu MB VRAM) - changing adapter applies on restart.",
                                caps.adapter_name.c_str(), caps.vendor_id, caps.device_id,
                                (unsigned long long)(caps.dedicated_vram >> 20));
        }
    }
}

void DrawAudioPanel(bool& dirty) {
    app::Settings& s = app::Get();
    SectionHeading("Audio Output & Mixing");

    const char* outs[] = {"Auto (System Default)", "Stereo (2.0)", "Surround (5.1)", "Mono (1.0)"};
    int out_idx = int(std::min(s.audio_output, audio::kOutputMono));
    if (ImGui::Combo("Speaker Configuration", &out_idx, outs, 4)) {
        s.audio_output = uint32_t(out_idx);
        audio::SetOutputMode(s.audio_output);
        dirty = true;
    }

    int vol = int(std::min(s.master_volume, 100u));
    if (ImGui::SliderInt("Master Volume (%)", &vol, 0, 100, "%d%%")) {
        s.master_volume = uint32_t(std::clamp(vol, 0, 100));
        audio::SetMasterVolume(float(s.master_volume) / 100.0f);
        dirty = true;
    }

    if (ImGui::Checkbox("Mute Audio", &s.audio_mute)) {
        audio::SetMuted(s.audio_mute);
        dirty = true;
    }

    if (ImGui::Checkbox("Mute When Window Loses Focus", &s.mute_unfocused)) {
        dirty = true;
    }

    SectionHeading("In-Game Audio (Synced with Xbox Profile & [game_options])");
    psiops::guest_settings::GuestUserSettings gs = psiops::guest_settings::GetSettings();
    bool gs_changed = false;

    int sfx_vol = std::clamp(int(gs.sfx_volume), 0, 100);
    if (ImGui::SliderInt("SFX / Voice / FMV Volume (%)", &sfx_vol, 0, 100, "%d%%")) {
        gs.sfx_volume = std::clamp(sfx_vol, 0, 100);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Synced 1:1 with in-game Options -> Audio -> SFX Volume and persisted in settings.toml [game_options].");
    }

    int mus_vol = std::clamp(int(gs.music_volume), 0, 100);
    if (ImGui::SliderInt("Music Volume (%)", &mus_vol, 0, 100, "%d%%")) {
        gs.music_volume = std::clamp(mus_vol, 0, 100);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Synced 1:1 with in-game Options -> Audio -> Music Volume and persisted in settings.toml [game_options].");
    }

    if (gs_changed) {
        psiops::guest_settings::ApplyUiSettings(gs);
        dirty = true;
    }
}

void DrawInGameControlsOptionsPanel(bool& dirty) {
    SectionHeading("In-Game Controller & Gameplay Options ([game_options])");
    ImGui::TextDisabled(
        "Live-synced with Psi-Ops Xbox profile options and persisted in settings.toml [game_options].");

    psiops::guest_settings::GuestUserSettings gs = psiops::guest_settings::GetSettings();
    bool gs_changed = false;

    static constexpr const char* kLayouts[3] = {"Config 1 (Default)", "Config 2", "Config 3"};
    int layout_idx = std::clamp(int(gs.controller_layout) - 2, 0, 2);
    if (ImGui::Combo("Controller Button Layout", &layout_idx, kLayouts, 3)) {
        gs.controller_layout = uint32_t(layout_idx + 2);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Synced 1:1 with in-game Options -> Controller Config (1, 2, or 3).");
    }

    static constexpr const char* kCameras[3] = {"Camera A", "Camera B (Default)", "Camera C"};
    int cam_idx = std::clamp(int(gs.camera_config), 0, 2);
    if (ImGui::Combo("Camera Preset", &cam_idx, kCameras, 3)) {
        gs.camera_config = uint32_t(cam_idx);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Synced 1:1 with in-game Options -> Camera Preset (A, B, or C).");
    }

    int yaw_spd = std::clamp(int(gs.yaw_speed), 0, 100);
    if (ImGui::SliderInt("Horizontal Look Sensitivity", &yaw_spd, 0, 100)) {
        gs.yaw_speed = std::clamp(yaw_spd, 0, 100);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Controller right-stick horizontal turn speed (0..100, default 44).");
    }

    int pitch_spd = std::clamp(int(gs.pitch_speed), 0, 100);
    if (ImGui::SliderInt("Vertical Look Sensitivity", &pitch_spd, 0, 100)) {
        gs.pitch_speed = std::clamp(pitch_spd, 0, 100);
        gs_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Controller right-stick vertical pitch speed (0..100, default 43).");
    }

    if (ImGui::BeginTable("##InGameOptsToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("Controller Invert Y-Axis", &gs.controller_invert_y)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Controller right-stick pitch inversion (Xbox default: Inverted). Independent from PC Mouse Invert Y.");
        }

        if (ImGui::Checkbox("Controller Invert X-Axis", &gs.controller_invert_x)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controller right-stick yaw inversion (default: Normal).");
        }

        if (ImGui::Checkbox("Controller Vibration", &gs.vibration)) {
            if (!gs.vibration) input::Rumble(0, 0, 0);
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enables or disables in-game Xbox controller rumble motors.");
        }

        if (ImGui::Checkbox("Controller Look Spring", &gs.look_spring)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-centers controller camera pitch while moving (automatically bypassed while aiming with Mouse & Keyboard).");
        }

        ImGui::TableNextColumn();
        if (ImGui::Checkbox("Quick Tips", &gs.quick_tips)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show in-game tutorial Quick Tips popups.");
        }

        if (ImGui::Checkbox("HUD Visibility", &gs.hud_visible)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show health, Psi energy, and weapon HUD in gameplay.");
        }

        if (ImGui::Checkbox("Auto-Hide HUD", &gs.auto_hide_hud)) {
            gs_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically fade the HUD out when out of combat.");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset In-Game Options to Defaults")) {
        psiops::guest_settings::ResetToDefaults(true);
        psiops::guest_settings::ApplyUiSettings(psiops::guest_settings::MakeDefaultSettings());
        dirty = true;
    }

    if (gs_changed) {
        psiops::guest_settings::ApplyUiSettings(gs);
        dirty = true;
    }
}

void DrawToolsDiagnosticsPanel(const upscale::PluginProbe& probe, bool in_game, std::string& action_status) {
    SectionHeading("Retail Upscaler Plugins Status");

    auto draw_plugin_row = [](const char* name, bool ready, const std::string& detail) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%-18s", name);
        ImGui::SameLine(170.0f);
        DrawBadge(ready ? "[Ready]" : "[Unavailable]", ready ? kBadgeGreen : kBadgeGray);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", detail.c_str());
    };
    draw_plugin_row("NVIDIA DLSS 4", probe.dlss_ready, probe.dlss_detail);
    draw_plugin_row("AMD FSR 3.1", probe.fsr3_ready, probe.fsr3_detail);
    draw_plugin_row("Intel XeSS 2", probe.xess_ready, probe.xess_detail);
    ImGui::TextDisabled("Built-in spatial upscalers (FSR 1.0, NIS v1.0.3, CAS, Bicubic 5-Tap, Integer, Bilinear) are always [Ready].");

    SectionHeading("Shader Cache, Shortcuts & Diagnostics");
    ImGui::Text("Build Version: v%s", PSIOPS_VERSION_STR);
    gpu::CacheStats cs = gpu::GetCacheStats();
    ImGui::Text("Shader/PSO Disk Cache: %s | Shaders: %u | PSOs: %u | Size: %.2f MB",
                cs.enabled ? "Enabled" : "Standby", cs.shaders, cs.psos, double(cs.disk_bytes) / (1024.0 * 1024.0));
    ImGui::Text("Recommended Parallel Recompile Workers: %u", app::RecommendedCompileJobs());
    ImGui::Text("Executable Directory: %s", app::ExeDir().string().c_str());

    if (ImGui::Button("Create Desktop / App Shortcut")) {
        fs::path out_lnk;
        std::string err;
        if (app::CreateShortcut({}, out_lnk, err)) {
            action_status = "Shortcut created: " + out_lnk.string();
        } else {
            action_status = "Shortcut error: " + err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Shader & PSO Cache")) {
        gpu::RequestCacheReset();
        std::error_code ec;
        fs::remove_all(app::ExeDir() / "cache" / "shaders", ec);
        action_status = "Shader & PSO cache cleared (will rebuild cleanly on next frames).";
    }

    if (in_game) {
        ImGui::Spacing();
        gpu::GpuCaps caps = gpu::GetGpuCaps();
        ImGui::TextDisabled("GPU: %s (VID %04X DID %04X, UMA=%s, Tearing=%s)", caps.adapter_name.c_str(),
                            caps.vendor_id, caps.device_id, caps.uma ? "yes" : "no",
                            caps.allow_tearing ? "yes" : "no");
    }

    if (!action_status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.90f, 0.65f, 1.0f));
        ImGui::TextWrapped("%s", action_status.c_str());
        ImGui::PopStyleColor();
    }
}

namespace {

uint16_t g_nav_prev_mask = 0;
bool g_nav_primed = false;

}  // namespace

void ResetGamepadNavState(bool prime_held) {
    g_nav_prev_mask = prime_held ? input::HeldOnAnyController() : 0u;
    g_nav_primed = true;
}

void SyncGamepadBackendFlag(ImGuiIO& io) {
    if (input::GetDeviceInfo(0).connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
}

void ReleaseGamepadNavKeys(ImGuiIO& io) {
    static constexpr ImGuiKey kPadKeys[] = {
        ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown,  ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
        ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
        ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,
    };
    for (ImGuiKey k : kPadKeys) io.AddKeyEvent(k, false);
    // Next poll primes from whatever is physically held then, so a held chord isn't seen as a fresh press.
    g_nav_primed = false;
}

GamepadNavEvents PollGamepadNavToImGui(ImGuiIO& io) {
    // On disconnect HeldOnAnyController() drops to 0, so every key below is released on the same frame;
    // HasGamepad is cleared too so ImGui stops gamepad navigation until a pad is back.
    SyncGamepadBackendFlag(io);
    uint16_t now = input::HeldOnAnyController();
    if (!g_nav_primed) {
        g_nav_prev_mask = now;
        g_nav_primed = true;
    }
    uint16_t pressed = uint16_t(now & ~g_nav_prev_mask);
    g_nav_prev_mask = now;

    // Standard XInput / SDL gamepad bitmask from core/input.h:
    // 0x0001 UP, 0x0002 DOWN, 0x0004 LEFT, 0x0008 RIGHT, 0x0010 START, 0x0020 BACK,
    // 0x0100 LB, 0x0200 RB, 0x1000 A, 0x2000 B, 0x4000 X, 0x8000 Y
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (now & 0x0001u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (now & 0x0002u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (now & 0x0004u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (now & 0x0008u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (now & 0x1000u) != 0);   // A
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (now & 0x2000u) != 0);  // B
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (now & 0x4000u) != 0);   // X
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (now & 0x8000u) != 0);     // Y
    io.AddKeyEvent(ImGuiKey_GamepadL1, (now & 0x0100u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1, (now & 0x0200u) != 0);

    GamepadNavEvents ev{};
    ev.tab_prev = (pressed & 0x0100u) != 0;
    ev.tab_next = (pressed & 0x0200u) != 0;
    ev.start_pressed = (pressed & 0x0010u) != 0;
    ev.back_pressed = (pressed & 0x0020u) != 0;
    ev.confirm_pressed = (pressed & 0x1000u) != 0;
    ev.cancel_pressed = (pressed & 0x2000u) != 0;
    return ev;
}

#ifdef _WIN32
std::wstring Utf8ToWideStr(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8Str(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::vector<std::string> GetProcessArgsUtf8() {
    std::vector<std::string> args;
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return args;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p && std::iswspace(*p)) ++p;
        if (!*p) break;
        std::wstring a;
        bool q = false;
        while (*p && (q || !std::iswspace(*p))) {
            if (*p == L'"') q = !q;
            else a.push_back(*p);
            ++p;
        }
        args.push_back(WideToUtf8Str(a));
    }
#else
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return args;
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nul = raw.find('\0', pos);
        if (nul == std::string::npos) nul = raw.size();
        args.emplace_back(raw.substr(pos, nul - pos));
        pos = nul + 1;
    }
#endif
    return args;
}

bool IsGamescope() {
#ifdef _WIN32
    return false;
#else
    if (const char* gs = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) return true;
    if (const char* sd = std::getenv("SteamDeck"); sd && std::strcmp(sd, "1") == 0) return true;
    return false;
#endif
}

namespace {
std::mutex g_update_prefs_lock;
UpdateUiPrefs g_update_prefs{};

std::string LowerAsciiUi(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

UpdateUiPrefs& GetUpdatePrefs() { return g_update_prefs; }

void LoadUpdatePrefs(const void* toml_table) {
    if (!toml_table) return;
    const auto& root = *static_cast<const toml::table*>(toml_table);
    std::lock_guard<std::mutex> lk(g_update_prefs_lock);
    g_update_prefs.check_updates_on_startup =
        root["check_updates_on_startup"].value_or(g_update_prefs.check_updates_on_startup);
    if (auto tag = root["dismissed_update_tag"].value<std::string>()) {
        g_update_prefs.dismissed_update_tag = *tag;
    } else {
        g_update_prefs.dismissed_update_tag.clear();
    }
    g_update_prefs.show_cheats_hud =
        root["show_cheats_hud"].value_or(g_update_prefs.show_cheats_hud);
}

void SaveUpdatePrefs(void* toml_table) {
    if (!toml_table) return;
    auto& root = *static_cast<toml::table*>(toml_table);
    std::lock_guard<std::mutex> lk(g_update_prefs_lock);
    root.insert_or_assign("check_updates_on_startup", g_update_prefs.check_updates_on_startup);
    root.insert_or_assign("dismissed_update_tag", g_update_prefs.dismissed_update_tag);
    root.insert_or_assign("show_cheats_hud", g_update_prefs.show_cheats_hud);
}

std::string SanitizeUiText(const std::string& s, bool single_line, size_t max_chars) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        // Normalize common UTF-8 typographic punctuation into crisp ASCII equivalents for Latin-1 fonts
        if (c == 0xE2u && i + 2 < s.size()) {
            uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
            uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
            if (b1 == 0x80u && (b2 == 0x93u || b2 == 0x94u)) {
                out.push_back('-');
                prev_space = false;
                i += 3;
                continue;
            }
            if (b1 == 0x80u && (b2 == 0x98u || b2 == 0x99u)) {
                out.push_back('\'');
                prev_space = false;
                i += 3;
                continue;
            }
            if (b1 == 0x80u && (b2 == 0x9Cu || b2 == 0x9Du)) {
                out.push_back('"');
                prev_space = false;
                i += 3;
                continue;
            }
            if (b1 == 0x80u && b2 == 0xA2u) {
                out += "*";
                prev_space = false;
                i += 3;
                continue;
            }
            if (b1 == 0x80u && b2 == 0xA6u) {
                out += "...";
                prev_space = false;
                i += 3;
                continue;
            }
            if (b1 == 0x86u && b2 == 0x92u) {
                out += "->";
                prev_space = false;
                i += 3;
                continue;
            }
        }
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n') {
            if (single_line) {
                if (!out.empty() && !prev_space) {
                    out += "  |  ";
                    prev_space = true;
                }
            } else if (!out.empty() && out.back() != '\n') {
                out.push_back('\n');
                prev_space = true;
            }
            ++i;
            continue;
        }
        if (c < 0x20u || c == 0x7Fu) {
            if (c == '\t' && !prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            ++i;
            continue;
        }
        if (c == ' ') {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            ++i;
            continue;
        }
        // Keep printable ASCII + valid 2-byte UTF-8 Latin-1 (U+00A0..U+00FF); skip higher planes
        if (c < 0x80u) {
            out.push_back(static_cast<char>(c));
            prev_space = false;
            ++i;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < s.size()) {
            uint32_t cp = ((uint32_t(c) & 0x1Fu) << 6) | (uint32_t(static_cast<uint8_t>(s[i + 1])) & 0x3Fu);
            if (cp >= 0xA0u && cp <= 0xFFu) {
                out.push_back(static_cast<char>(c));
                out.push_back(s[i + 1]);
                prev_space = false;
            }
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < s.size()) {
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < s.size()) {
            i += 4;
        } else {
            ++i;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '|' || out.back() == '\n')) {
        out.pop_back();
    }
    if (max_chars > 4 && out.size() > max_chars) {
        size_t cut = max_chars - 3;
        while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0u) == 0x80u) --cut;
        while (cut > max_chars / 2 && out[cut] != ' ') --cut;
        while (cut > 0 && (out[cut - 1] == ' ' || out[cut - 1] == '|')) --cut;
        out.resize(cut);
        out += "...";
    }
    return out;
}

bool ShouldShowUpdateBanner(const psiops::UpdateStatusSnapshot& snap, const std::string& dismissed_tag) {
    if (snap.state != psiops::UpdateState::UpdateAvailable &&
        snap.state != psiops::UpdateState::Downloading &&
        snap.state != psiops::UpdateState::ReadyToRestart) {
        return false;
    }
    if (snap.manual_check) return true;
    if (dismissed_tag.empty()) return true;
    return snap.latest_tag != dismissed_tag;
}

void DrawLauncherHeaderUpdateButton(const psiops::UpdateStatusSnapshot& snap, float ui_scale,
                                    const std::function<void()>& on_manual_check) {
    float s = std::clamp(ui_scale, 1.0f, 2.5f);
    const UpdateUiPrefs& o = GetUpdatePrefs();
    const bool checking = (snap.state == psiops::UpdateState::Checking);
    const bool downloading = (snap.state == psiops::UpdateState::Downloading);
    const bool ready = (snap.state == psiops::UpdateState::ReadyToRestart);
    const bool has_banner = ShouldShowUpdateBanner(snap, o.dismissed_update_tag);

    const char* visible_text = "Check for Updates";
    const char* btn_id = "Check for Updates##LauncherHdrUpd";
    if (checking) {
        visible_text = "Checking...";
        btn_id = "Checking...##LauncherHdrUpd";
    } else if (downloading) {
        visible_text = "Updating...";
        btn_id = "Updating...##LauncherHdrUpd";
    } else if (ready) {
        visible_text = "Update Staged!";
        btn_id = "Update Staged!##LauncherHdrUpd";
    } else if (has_banner) {
        visible_text = "Update Available!";
        btn_id = "Update Available!##LauncherHdrUpd";
    }

    float text_w = ImGui::CalcTextSize(visible_text).x;
    float btn_w = std::max(text_w + ImGui::GetStyle().FramePadding.x * 2.0f + 12.0f * s, 148.0f * s);
    float right_x = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - btn_w;
    ImGui::SameLine();
    if (right_x > ImGui::GetCursorPosX() + 8.0f * s) {
        ImGui::SetCursorPosX(right_x);
    }

    const bool disabled = checking || downloading;
    if (disabled) ImGui::BeginDisabled(true);

    if (ready) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.48f, 0.28f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.62f, 0.36f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.72f, 0.42f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.88f, 0.52f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 1.00f, 0.96f, 1.00f));
    } else if (has_banner) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.36f, 0.08f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.48f, 0.11f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.56f, 0.14f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.96f, 0.74f, 0.24f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.94f, 0.76f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.21f, 0.32f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.35f, 0.50f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.48f, 0.64f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.44f, 0.60f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.96f, 0.99f, 1.00f));
    }

    if (ImGui::Button(btn_id, ImVec2(btn_w, 0.0f))) {
        if (on_manual_check) on_manual_check();
    }

    ImGui::PopStyleColor(5);
    if (disabled) ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (checking) {
            ImGui::SetTooltip("Querying GitHub Releases (%s)...", PSIOPS_GITHUB_REPO);
        } else if (has_banner && !snap.latest_tag.empty()) {
            ImGui::SetTooltip("New release %s is available (Current: %s). Click to re-check.",
                              snap.latest_tag.c_str(), PSIOPS_VERSION_TAG);
        } else if (snap.state == psiops::UpdateState::UpToDate) {
            ImGui::SetTooltip("Up to date (%s). Click to check GitHub Releases again.", PSIOPS_VERSION_TAG);
        } else {
            ImGui::SetTooltip("Check GitHub Releases (%s) for a newer build.", PSIOPS_GITHUB_REPO);
        }
    }
}

void DrawUpdateAlertCard(const psiops::UpdateStatusSnapshot& snap, const UpdateCardContext& uctx) {
    float s = std::clamp(uctx.ui_scale, 1.0f, 2.5f);
    std::string tag = !snap.latest_tag.empty()
                          ? snap.latest_tag
                          : (!snap.latest_version.empty() ? ("v" + snap.latest_version) : std::string("New Release"));

    const bool ready = (snap.state == psiops::UpdateState::ReadyToRestart);
    const bool downloading = (snap.state == psiops::UpdateState::Downloading);
    const bool show_primary = ready || downloading || snap.can_self_update;
    const bool show_dismiss = !downloading;

    std::string clean_title = SanitizeUiText(snap.release_title, true, 76);
    std::string clean_notes = SanitizeUiText(snap.release_notes_summary, true, 118);
    std::string summary_line;
    if (ready) {
        if (uctx.in_launcher) {
            summary_line =
                "Verified & staged in update_staging/ - click 'Restart & Apply Update' to install (saves, settings & ISO preserved).";
        } else {
            summary_line =
                "Update staged! Save your game progress first, then click 'Save & Exit to Update' to install & relaunch.";
        }
    } else if (!clean_notes.empty() && !clean_title.empty() && LowerAsciiUi(clean_title) != LowerAsciiUi(tag) &&
               clean_notes.find(clean_title) == std::string::npos) {
        summary_line = SanitizeUiText(clean_title + " - " + clean_notes, true, 118);
    } else if (!clean_notes.empty()) {
        summary_line = clean_notes;
    } else if (!clean_title.empty()) {
        summary_line = clean_title;
    } else {
        summary_line =
            "A newer release of Psi-Ops: The Mindgate Conspiracy ReComp is available on GitHub Releases.";
    }

    std::string full_tooltip = SanitizeUiText(snap.release_notes_summary, false, 600);

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ready ? ImVec4(0.080f, 0.160f, 0.120f, 0.97f) : ImVec4(0.145f, 0.118f, 0.072f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ready ? ImVec4(0.36f, 0.88f, 0.50f, 0.95f) : ImVec4(0.94f, 0.70f, 0.22f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, std::max(1.5f, std::round(1.5f * s)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 8.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 5.0f * s));

    const char* card_id = uctx.in_launcher ? "##LauncherUpdateAlertCard" : "##InGameUpdateAlertCard";
    constexpr ImGuiChildFlags kCardFlags =
        ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened;
    if (ImGui::BeginChild(card_id, ImVec2(0.0f, 0.0f), kCardFlags)) {
        ImGui::AlignTextToFramePadding();
        if (ready) {
            ImGui::TextColored(ImVec4(0.48f, 0.96f, 0.60f, 1.0f), "Update Ready: %s (Current: %s)", tag.c_str(),
                               PSIOPS_VERSION_TAG);
        } else if (downloading) {
            ImGui::TextColored(ImVec4(1.00f, 0.84f, 0.36f, 1.0f), "Downloading Update: %s (Current: %s)", tag.c_str(),
                               PSIOPS_VERSION_TAG);
        } else {
            ImGui::TextColored(ImVec4(1.00f, 0.84f, 0.36f, 1.0f), "Update Available: %s (Current: %s)", tag.c_str(),
                               PSIOPS_VERSION_TAG);
        }

        const char* prim_lbl = uctx.in_launcher ? "Update Now" : "Stage Update";
        if (ready) {
            prim_lbl = uctx.in_launcher ? "Restart & Apply Update" : "Save & Exit to Update";
        } else if (downloading) {
            prim_lbl = "Downloading...";
        }
        const char* rel_lbl = "Open Release Page";
        const char* dis_lbl = "Dismiss";

        auto calc_btn_w = [&](const char* txt) -> float {
            return ImGui::CalcTextSize(txt).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f * s;
        };
        float prim_w = show_primary ? calc_btn_w(prim_lbl) : 0.0f;
        float rel_w = calc_btn_w(rel_lbl);
        float dis_w = show_dismiss ? calc_btn_w(dis_lbl) : 0.0f;

        int btn_count = (show_primary ? 1 : 0) + 1 + (show_dismiss ? 1 : 0);
        float total_btns_w = prim_w + rel_w + dis_w + ImGui::GetStyle().ItemSpacing.x * float(btn_count - 1);
        float right_x = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - total_btns_w;

        ImGui::SameLine();
        if (right_x >= ImGui::GetCursorPosX() + 8.0f * s) {
            ImGui::SetCursorPosX(right_x);
        } else {
            ImGui::NewLine();
        }

        if (show_primary) {
            if (downloading) ImGui::BeginDisabled(true);
            if (ready) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.54f, 0.28f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.68f, 0.36f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.78f, 0.42f, 1.00f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.68f, 0.44f, 0.08f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.84f, 0.56f, 0.12f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.94f, 0.64f, 0.16f, 1.00f));
            }
            if (ImGui::Button(prim_lbl, ImVec2(prim_w, 0.0f))) {
                if (ready) {
                    if (uctx.on_apply_and_restart) uctx.on_apply_and_restart();
                } else {
                    if (uctx.on_update_now) uctx.on_update_now();
                }
            }
            ImGui::PopStyleColor(3);
            if (downloading) ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (ready) {
                    ImGui::SetTooltip(
                        uctx.in_launcher
                            ? "Closes the launcher, installs the verified update (preserving settings, saves & ISO), and relaunches."
                            : "Ensure you saved your game progress first! Closes the game, applies the update, and relaunches.");
                } else if (!downloading) {
                    if (!snap.asset_name.empty() && snap.asset_size > 0) {
                        ImGui::SetTooltip(
                            uctx.in_launcher
                                ? "Download & stage %s (%.1f MB) for one-click update."
                                : "Download & verify %s (%.1f MB) in the background without closing your game.",
                            snap.asset_name.c_str(), double(snap.asset_size) / (1024.0 * 1024.0));
                    } else {
                        ImGui::SetTooltip("Download & stage the latest release archive.");
                    }
                }
            }
            ImGui::SameLine();
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.24f, 0.34f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.19f, 0.36f, 0.50f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.46f, 0.62f, 1.00f));
        if (ImGui::Button(rel_lbl, ImVec2(rel_w, 0.0f))) {
            if (uctx.on_open_release_page) uctx.on_open_release_page(snap.html_url);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open GitHub release notes & manual downloads in your browser.");
        }

        if (show_dismiss) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.22f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.24f, 0.28f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.36f, 0.28f, 0.32f, 1.00f));
            if (ImGui::Button(dis_lbl, ImVec2(dis_w, 0.0f))) {
                if (uctx.on_dismiss) uctx.on_dismiss(tag);
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hide this alert for %s until you click 'Check for Updates' manually.", tag.c_str());
            }
        }

        if (downloading) {
            double dl_mb = double(snap.downloaded_bytes) / (1024.0 * 1024.0);
            float frac = 0.35f;
            char prog_buf[256];
            if (snap.asset_size > 0) {
                double tot_mb = double(snap.asset_size) / (1024.0 * 1024.0);
                frac = std::clamp(float(double(snap.downloaded_bytes) / double(snap.asset_size)), 0.0f, 1.0f);
                std::snprintf(prog_buf, sizeof(prog_buf), "%.2f / %.2f MB (%.0f%%)  %s", dl_mb, tot_mb,
                              double(frac * 100.0f), snap.asset_name.c_str());
            } else {
                std::snprintf(prog_buf, sizeof(prog_buf), "%.2f MB downloaded  %s", dl_mb, snap.asset_name.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.88f, 0.60f, 0.14f, 1.00f));
            ImGui::ProgressBar(frac, ImVec2(-1.0f, std::round(18.0f * s)), prog_buf);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.90f, 0.86f, 1.0f));
            ImGui::TextWrapped("%s", summary_line.c_str());
            ImGui::PopStyleColor();
            if (!full_tooltip.empty() && ImGui::IsItemHovered()) {
                if (ImGui::BeginTooltip()) {
                    ImGui::PushTextWrapPos(460.0f * s);
                    ImGui::TextUnformatted(full_tooltip.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            if (!uctx.in_launcher && !ready) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.74f, 0.30f, 1.0f));
                ImGui::TextWrapped("Save your progress in-game before closing the game to apply an update.");
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    ImVec2 p_min = ImGui::GetItemRectMin();
    ImVec2 p_max = ImGui::GetItemRectMax();
    if (p_max.x > p_min.x + 8.0f && p_max.y > p_min.y + 8.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 glow_col = ready ? IM_COL32(90, 224, 128, 52) : IM_COL32(245, 182, 58, 58);
        ImU32 bar_col = ready ? IM_COL32(96, 236, 136, 235) : IM_COL32(248, 190, 64, 235);
        dl->AddRect(ImVec2(p_min.x + 1.0f, p_min.y + 1.0f), ImVec2(p_max.x - 1.0f, p_max.y - 1.0f), glow_col,
                    6.0f * s, 0, std::max(1.0f, s));
        dl->AddRectFilled(ImVec2(p_min.x + 1.0f, p_min.y + 3.0f * s),
                          ImVec2(p_min.x + std::max(3.5f, std::round(3.5f * s)), p_max.y - 3.0f * s), bar_col,
                          2.0f * s);
    }

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

void DrawUpdatesDiagnosticsSection(const psiops::UpdateStatusSnapshot& snap, bool in_launcher, float ui_scale,
                                   bool& dirty, bool& manual_pending, std::string& status_msg,
                                   const std::function<void()>& on_apply_and_restart) {
    SectionHeading("Updates & Self-Updater (" PSIOPS_VERSION_TAG ")");

    UpdateUiPrefs& o = GetUpdatePrefs();
    if (!in_launcher && ShouldShowUpdateBanner(snap, o.dismissed_update_tag)) {
        UpdateCardContext uctx{};
        uctx.in_launcher = false;
        uctx.ui_scale = ui_scale;
        uctx.on_update_now = [&] {
            if (psiops::StartDownloadAndStageUpdate()) {
                status_msg = "Downloading & staging " +
                             (snap.asset_name.empty() ? snap.latest_tag : snap.asset_name) +
                             " in the background...";
            } else {
                psiops::UpdateStatusSnapshot err_s = psiops::GetUpdateSnapshot();
                status_msg = err_s.status_message.empty() ? "Could not start self-update download."
                                                          : SanitizeUiText(err_s.status_message, true, 140);
            }
        };
        uctx.on_apply_and_restart = on_apply_and_restart;
        uctx.on_open_release_page = [&](const std::string& url) {
            std::string target = url.empty() ? std::string(PSIOPS_RELEASES_URL) : url;
            psiops::OpenReleasePageInBrowser(target);
            status_msg = IsGamescope() ? ("Release page: " + target + " (browse in Desktop Mode if hidden)")
                                       : ("Opened release page in browser: " + target);
        };
        uctx.on_dismiss = [&](const std::string& tag) {
            o.dismissed_update_tag = tag;
            dirty = true;
            status_msg = "Dismissed update alert for " + tag + " (click 'Check for Updates' to show again).";
        };
        DrawUpdateAlertCard(snap, uctx);
        ImGui::Spacing();
    }

    const bool upd_busy =
        (snap.state == psiops::UpdateState::Checking || snap.state == psiops::UpdateState::Downloading);
    if (upd_busy) ImGui::BeginDisabled(true);
    if (ImGui::Button(snap.state == psiops::UpdateState::Checking ? "Checking...##DiagUpdBtn"
                                                                  : "Check for Updates##DiagUpdBtn")) {
        if (!o.dismissed_update_tag.empty()) {
            o.dismissed_update_tag.clear();
            dirty = true;
        }
        manual_pending = true;
        status_msg = "Checking GitHub Releases for updates...";
        psiops::StartUpdateCheck(true);
    }
    if (upd_busy) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Open Release Page##DiagRelBtn")) {
        std::string target = snap.html_url.empty() ? std::string(PSIOPS_RELEASES_URL) : snap.html_url;
        psiops::OpenReleasePageInBrowser(target);
        status_msg = IsGamescope() ? ("GitHub Releases: " + target + " (browse in Desktop Mode if hidden)")
                                   : ("Opened GitHub Releases: " + target);
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Automatically Check for Updates on Startup", &o.check_updates_on_startup)) {
        dirty = true;
    }

    switch (snap.state) {
        case psiops::UpdateState::Checking:
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.36f, 1.0f), "Status: Querying GitHub Releases (%s)...",
                               PSIOPS_GITHUB_REPO);
            break;
        case psiops::UpdateState::UpToDate:
            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.65f, 1.0f), "Status: Up to date (%s)", PSIOPS_VERSION_TAG);
            break;
        case psiops::UpdateState::UpdateAvailable:
            ImGui::TextColored(
                ImVec4(1.00f, 0.84f, 0.36f, 1.0f),
                in_launcher ? "Status: Update available (%s) - use the alert banner above to download & install."
                            : "Status: Update available (%s) - save your progress in-game before updating.",
                snap.latest_tag.c_str());
            break;
        case psiops::UpdateState::Downloading:
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.36f, 1.0f), "Status: Downloading %s...",
                               snap.asset_name.c_str());
            break;
        case psiops::UpdateState::ReadyToRestart:
            ImGui::TextColored(
                ImVec4(0.48f, 0.96f, 0.60f, 1.0f),
                in_launcher ? "Status: Update %s staged! Click 'Restart & Apply Update' above to finish."
                            : "Status: Update %s staged! Save your game progress, then click 'Save & Exit to Update'.",
                snap.latest_tag.c_str());
            break;
        case psiops::UpdateState::Error:
            ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.38f, 1.0f), "Status: %s",
                               snap.status_message.empty() ? "Update check failed."
                                                           : SanitizeUiText(snap.status_message, true, 120).c_str());
            break;
        case psiops::UpdateState::Idle:
        default:
            ImGui::TextDisabled("Installed Version: %s  |  GitHub: %s", PSIOPS_VERSION_TAG, PSIOPS_GITHUB_REPO);
            break;
    }
}

namespace {

const char* ShortCheatToggleFallbackLabel(psiops::cheats::CheatToggle id) {
    switch (id) {
        case psiops::cheats::CheatToggle::GodMode: return "God Mode (Invulnerability)";
        case psiops::cheats::CheatToggle::InfinitePsi: return "Infinite Psi Energy";
        case psiops::cheats::CheatToggle::BottomlessClip: return "Bottomless Magazine";
        case psiops::cheats::CheatToggle::InfiniteReserveAmmo: return "Infinite Reserve Ammo";
        case psiops::cheats::CheatToggle::SuperPsi: return "Super Psi Power";
        case psiops::cheats::CheatToggle::OneHitKills: return "One-Hit Kills (Hostiles Only)";
        case psiops::cheats::CheatToggle::Count:
        default: return "Cheat Toggle";
    }
}

const char* ShortCheatToggleSubtitle(psiops::cheats::CheatToggle id) {
    switch (id) {
        case psiops::cheats::CheatToggle::GodMode:
            return "Zero damage taken; Nick stays at 100% Max Health";
        case psiops::cheats::CheatToggle::InfinitePsi:
            return "Psi energy stays at 100% and never drains";
        case psiops::cheats::CheatToggle::BottomlessClip:
            return "Active & holstered weapon magazines never deplete";
        case psiops::cheats::CheatToggle::InfiniteReserveAmmo:
            return "Reserve ammo pools stay at max capacity";
        case psiops::cheats::CheatToggle::SuperPsi:
            return "Amplified Psi power damage & telekinetic force";
        case psiops::cheats::CheatToggle::OneHitKills:
            return "Hostile minions die in one hit (bosses & NPCs safe)";
        case psiops::cheats::CheatToggle::Count:
        default:
            return "";
    }
}

const psiops::cheats::CheatActionInfo* FindCheatActionInfo(psiops::cheats::CheatAction action) {
    const auto& infos = psiops::cheats::GetActionInfos();
    const size_t idx = static_cast<size_t>(action);
    if (idx < infos.size() && infos[idx].id == action) return &infos[idx];
    for (const auto& info : infos) {
        if (info.id == action) return &info;
    }
    return nullptr;
}

void DrawCheatActionButton(psiops::cheats::CheatAction action, const char* fallback_label, float s, bool can_run,
                           std::string* out_status) {
    const psiops::cheats::CheatActionInfo* info = FindCheatActionInfo(action);
    const char* label = (info && info->label && *info->label) ? info->label : fallback_label;
    ImGui::PushID(static_cast<int>(action));
    if (ImGui::Button(label, ImVec2(-1.0f, std::round(30.0f * s)))) {
        psiops::cheats::EnqueueAction(action);
        if (out_status) {
            *out_status = std::string("Queued cheat action: ") + label;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(420.0f * s);
            if (info && info->description && *info->description) {
                ImGui::TextUnformatted(info->description);
            } else {
                ImGui::TextUnformatted(label);
            }
            if (!can_run) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.30f, 1.0f),
                                   "Requires an active gameplay mission with Nick Scryer alive.");
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::PopID();
}

}  // namespace

void DrawCheatsPanel(float ui_scale, bool& dirty, std::string* out_status) {
    const float s = std::clamp(ui_scale, 1.0f, 2.5f);
    UpdateUiPrefs& ui_prefs = GetUpdatePrefs();
    const psiops::cheats::CheatRuntimeStatus st = psiops::cheats::GetRuntimeStatus();
    const bool replay_locked = st.replay_active;
    const bool has_active_cheats = !replay_locked && st.active_toggle_count > 0;
    const bool can_run_actions = !replay_locked && st.in_level && st.player_alive;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 5.0f * s));

    // 1. Live Status Header Card
    ImVec4 card_bg(0.082f, 0.112f, 0.155f, 0.96f);
    ImVec4 card_border(0.24f, 0.56f, 0.72f, 0.84f);
    ImU32 accent_bar = IM_COL32(72, 206, 238, 230);
    ImU32 accent_glow = IM_COL32(72, 206, 238, 46);
    if (replay_locked) {
        card_bg = ImVec4(0.145f, 0.090f, 0.096f, 0.96f);
        card_border = ImVec4(0.78f, 0.30f, 0.30f, 0.88f);
        accent_bar = IM_COL32(224, 82, 82, 235);
        accent_glow = IM_COL32(224, 82, 82, 48);
    } else if (has_active_cheats) {
        card_bg = ImVec4(0.118f, 0.104f, 0.082f, 0.97f);
        card_border = ImVec4(0.94f, 0.68f, 0.22f, 0.92f);
        accent_bar = IM_COL32(246, 184, 58, 240);
        accent_glow = IM_COL32(246, 184, 58, 54);
    } else if (st.in_level) {
        card_bg = ImVec4(0.076f, 0.132f, 0.138f, 0.96f);
        card_border = ImVec4(0.30f, 0.74f, 0.58f, 0.86f);
        accent_bar = IM_COL32(84, 218, 152, 230);
        accent_glow = IM_COL32(84, 218, 152, 46);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, card_border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, std::max(1.5f, std::round(1.5f * s)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11.0f * s, 6.5f * s));

    constexpr ImGuiChildFlags kStatusFlags =
        ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened;
    if (ImGui::BeginChild("##PsiOpsCheatStatusCard", ImVec2(0.0f, 0.0f), kStatusFlags)) {
        ImGui::AlignTextToFramePadding();
        if (replay_locked) {
            ImGui::TextColored(ImVec4(0.96f, 0.44f, 0.42f, 1.0f),
                               "Deterministic Replay / Autotest Active - All cheats & actions are suppressed.");
        } else if (st.in_level) {
            if (!st.player_alive) {
                ImGui::TextColored(ImVec4(0.96f, 0.38f, 0.38f, 1.0f), "[PLAYER DOWN]");
            } else if (st.mind_controlling) {
                ImGui::TextColored(ImVec4(0.68f, 0.56f, 1.00f, 1.0f), "[MIND CONTROL]");
            } else {
                ImGui::TextColored(ImVec4(0.45f, 0.94f, 0.62f, 1.0f), "[IN LEVEL]");
            }
            ImGui::SameLine(0.0f, 8.0f * s);
            ImGui::Text("HP: %.0f/%.0f  |  Psi: %.0f/%.0f  |  Ammo: %d/%d (+%d/%d)  |  Weapons: %u",
                        st.health, st.max_health, st.psi, st.max_psi, st.clip_ammo, st.max_clip_ammo,
                        st.reserve_ammo, st.max_reserve_ammo, st.owned_weapon_count);
            if (ImGui::IsItemHovered()) {
                if (ImGui::BeginTooltip()) {
                    ImGui::Text("Active Magazine: %d / %d  |  Reserve Pool: %d / %d  |  Owned Weapons: %u  |  Armed Toggles: %u",
                                st.clip_ammo, st.max_clip_ammo, st.reserve_ammo, st.max_reserve_ammo,
                                st.owned_weapon_count, st.active_toggle_count);
                    ImGui::EndTooltip();
                }
            }
        } else {
            ImGui::TextColored(
                ImVec4(0.96f, 0.80f, 0.34f, 1.0f),
                "Load into a mission or training room for instant loadout/Psi actions - persistent toggles can be armed anytime.");
        }

        if (has_active_cheats) {
            char dis_vis[48];
            std::snprintf(dis_vis, sizeof(dis_vis), "Disable All Cheats (%u)", st.active_toggle_count);
            std::string dis_id = std::string(dis_vis) + "##DisableAllCheatsBtn";
            const float dis_w = ImGui::CalcTextSize(dis_vis).x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f * s;
            ImGui::SameLine();
            const float avail_x = ImGui::GetContentRegionAvail().x;
            if (avail_x >= dis_w + 8.0f * s) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_x - dis_w);
            } else {
                ImGui::NewLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.16f, 0.18f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.22f, 0.24f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.26f, 0.28f, 1.00f));
            if (ImGui::Button(dis_id.c_str(), ImVec2(dis_w, 0.0f))) {
                psiops::cheats::DisableAllCheats();
                dirty = true;
                if (out_status) *out_status = "Disabled all active persistent cheats.";
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Turn off all armed persistent psychic & combat cheat toggles immediately.");
            }
        }

        if (!replay_locked && !st.last_action_feedback.empty()) {
            const std::string clean_fb = SanitizeUiText(st.last_action_feedback, true, 118);
            const ImVec4 fb_col = st.last_action_is_warning ? ImVec4(0.98f, 0.78f, 0.30f, 1.0f)
                                                            : ImVec4(0.48f, 0.95f, 0.62f, 1.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(fb_col, "> %s", clean_fb.c_str());

            const char* clr_txt = "Dismiss";
            const float clr_w = ImGui::CalcTextSize(clr_txt).x + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f * s;
            ImGui::SameLine();
            const float clr_avail = ImGui::GetContentRegionAvail().x;
            if (clr_avail >= clr_w + 8.0f * s) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + clr_avail - clr_w);
            }
            if (ImGui::SmallButton("Dismiss##ClearCheatFeedback")) {
                psiops::cheats::ClearActionFeedback();
            }
        }
    }
    ImGui::EndChild();

    const ImVec2 c_min = ImGui::GetItemRectMin();
    const ImVec2 c_max = ImGui::GetItemRectMax();
    if (c_max.x > c_min.x + 8.0f && c_max.y > c_min.y + 8.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(ImVec2(c_min.x + 1.0f, c_min.y + 1.0f), ImVec2(c_max.x - 1.0f, c_max.y - 1.0f), accent_glow,
                    6.0f * s, 0, std::max(1.0f, s));
        dl->AddRectFilled(ImVec2(c_min.x + 1.0f, c_min.y + 3.0f * s),
                          ImVec2(c_min.x + std::max(3.5f, std::round(3.5f * s)), c_max.y - 3.0f * s), accent_bar,
                          2.0f * s);
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // 2. Persistent Psychic & Combat Toggles (2-Column Grid)
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.32f, 0.86f, 0.96f, 1.0f), "Psychic & Combat Toggles");
    ImGui::SameLine();
    ImGui::TextDisabled("(Saved in settings.toml; arm anytime)");

    if (replay_locked) ImGui::BeginDisabled(true);
    const auto& toggle_infos = psiops::cheats::GetToggleInfos();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f * s, 2.5f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * s, 3.5f * s));
    if (ImGui::BeginTable("##PsiOpsCheatTogglesGrid", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < toggle_infos.size(); ++i) {
            ImGui::TableNextColumn();
            const psiops::cheats::CheatToggleInfo& info = toggle_infos[i];
            const char* lbl = (info.label && *info.label) ? info.label : ShortCheatToggleFallbackLabel(info.id);
            bool armed = !replay_locked && psiops::cheats::GetToggle(info.id);
            const bool style_pushed = armed;
            ImGui::PushID(static_cast<int>(i));
            if (style_pushed) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.90f, 0.62f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.28f, 0.20f, 0.08f, 0.98f));
            }
            if (ImGui::Checkbox(lbl, &armed)) {
                psiops::cheats::SetToggle(info.id, armed);
                dirty = true;
            }
            if (style_pushed) {
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (ImGui::BeginTooltip()) {
                    ImGui::PushTextWrapPos(420.0f * s);
                    if (info.description && *info.description) {
                        ImGui::TextUnformatted(info.description);
                    } else {
                        ImGui::TextUnformatted(ShortCheatToggleSubtitle(info.id));
                    }
                    if (armed && st.in_level && psiops::cheats::IsToggleEffective(info.id)) {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.45f, 0.94f, 0.62f, 1.0f), "Status: Active in current mission");
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * s, 1.0f * s));
            ImGui::TextDisabled("  %s", ShortCheatToggleSubtitle(info.id));
            ImGui::PopStyleVar();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    if (replay_locked) ImGui::EndDisabled();

    // 3. Instant Arsenal & Psi Power Actions (2x2 Grid)
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.32f, 0.86f, 0.96f, 1.0f), "Instant Arsenal & Psi Power Actions");
    ImGui::SameLine();
    if (can_run_actions) {
        ImGui::TextColored(ImVec4(0.45f, 0.94f, 0.62f, 1.0f), "(Ready)");
    } else {
        ImGui::TextDisabled("(Requires active mission & live Nick Scryer)");
    }

    if (!can_run_actions) ImGui::BeginDisabled(true);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f * s, 3.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * s, 4.0f * s));
    if (ImGui::BeginTable("##PsiOpsCheatActionsGrid", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        DrawCheatActionButton(psiops::cheats::CheatAction::FullHealAndPsi, "Full Heal & Max Psi", s, can_run_actions,
                              out_status);
        ImGui::TableNextColumn();
        DrawCheatActionButton(psiops::cheats::CheatAction::RefillAllAmmo, "Refill All Weapon Ammo", s, can_run_actions,
                              out_status);
        ImGui::TableNextColumn();
        DrawCheatActionButton(psiops::cheats::CheatAction::UnlockAllPsiPowers, "Unlock All 6 Psi Powers", s,
                              can_run_actions, out_status);
        ImGui::TableNextColumn();
        DrawCheatActionButton(psiops::cheats::CheatAction::GiveAllWeaponsAndAmmo, "Give All Weapons & Max Ammo", s,
                              can_run_actions, out_status);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    if (!can_run_actions) ImGui::EndDisabled();

    // 4. Subtle HUD Badge Option & Shortcut Legend
    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * s, 3.5f * s));
    if (ImGui::Checkbox("Show Active Cheats Badge on HUD##CheatsHudToggle", &ui_prefs.show_cheats_hud)) {
        dirty = true;
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Displays a subtle 'CHEATS ACTIVE (N)' pill in the bottom-left corner while cheats are armed.");
    }

    const char* legend = "F2: Quick Cheats  |  F1 / BACK+L3: Overlay  |  LB/RB: Switch Tabs";
    const float leg_w = ImGui::CalcTextSize(legend).x;
    ImGui::SameLine();
    const float leg_avail = ImGui::GetContentRegionAvail().x;
    if (leg_avail >= leg_w + 10.0f * s) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + leg_avail - leg_w);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", legend);
    }

    ImGui::PopStyleVar();
}

}  // namespace ui_common
