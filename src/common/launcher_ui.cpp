// SPDX-License-Identifier: GPL-2.0-or-later
// 6-Tab Pre-Game Graphical Launcher (Win32 + D3D12 PerMonitorV2 & Linux SDL3 SDL_Renderer 1280x800).
// Supports mouse, touch, keyboard, and gamepad (LB/RB tabs, D-Pad, A/B, Start=Play),
// 5 Hardware Presets, live [Ready] plugin badges, Psi-Ops 25-action Click-to-Rebind Controls tab,
// In-App XISO File Browser, --launcher-tab, --open-file-browser, --capture-launcher-bmp,
// and strictly ZERO settings.toml writes on unconfigured Exit.

#include "common/launcher_ui.h"
#include "common/file_browser_ui.h"
#include "common/mkb_input.h"
#include "common/ui_common.h"
#include "common/update_checker.h"
#include "common/version.h"
#include "upscale/upscaler_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#else
#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#endif

namespace launcher_ui {

namespace {

namespace fs = std::filesystem;

struct LauncherCli {
    int initial_tab = 0;
    bool open_file_browser = false;
    fs::path capture_bmp;
    int autoplay_ms = 0;
};

void DebounceStartRelease() {
    // If gamepad Start triggered PLAY PSI-OPS, wait briefly for release so Start
    // cannot misfire on frame 0 in-game, then re-prime UI gamepad nav edge state.
    if ((input::HeldOnAnyController() & 0x0010u) != 0u) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while ((input::HeldOnAnyController() & 0x0010u) != 0u && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ui_common::ResetGamepadNavState(true);
}

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

LauncherCli ParseLauncherCli() {
    LauncherCli cli{};
    if (const wchar_t* env_ap = _wgetenv(L"XBE_LAUNCHER_AUTOPLAY_MS"); env_ap && *env_ap) {
        int ms = _wtoi(env_ap);
        if (ms > 0) cli.autoplay_ms = ms;
    }
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return cli;
    std::vector<std::wstring> args;
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
        args.push_back(std::move(a));
    }
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--launcher-tab" && i + 1 < args.size()) {
            cli.initial_tab = std::clamp(_wtoi(args[++i].c_str()), 0, 5);
        } else if (args[i].rfind(L"--launcher-tab=", 0) == 0) {
            cli.initial_tab = std::clamp(_wtoi(args[i].c_str() + 15), 0, 5);
        } else if (args[i] == L"--open-file-browser") {
            cli.open_file_browser = true;
        } else if (args[i] == L"--capture-launcher-bmp" && i + 1 < args.size()) {
            cli.capture_bmp = fs::path(args[++i]);
        } else if (args[i].rfind(L"--capture-launcher-bmp=", 0) == 0) {
            cli.capture_bmp = fs::path(args[i].substr(23));
        } else if (args[i] == L"--launcher-autoplay-ms" && i + 1 < args.size()) {
            int ms = _wtoi(args[++i].c_str());
            if (ms > 0) cli.autoplay_ms = ms;
        } else if (args[i].rfind(L"--launcher-autoplay-ms=", 0) == 0) {
            int ms = _wtoi(args[i].c_str() + 23);
            if (ms > 0) cli.autoplay_ms = ms;
        }
    }
    return cli;
}
#else
LauncherCli ParseLauncherCli() {
    LauncherCli cli{};
    if (const char* env_ap = std::getenv("XBE_LAUNCHER_AUTOPLAY_MS"); env_ap && *env_ap) {
        int ms = std::atoi(env_ap);
        if (ms > 0) cli.autoplay_ms = ms;
    }
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return cli;
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    std::vector<std::string> args;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nul = raw.find('\0', pos);
        if (nul == std::string::npos) nul = raw.size();
        args.push_back(raw.substr(pos, nul - pos));
        pos = nul + 1;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--launcher-tab" && i + 1 < args.size()) {
            cli.initial_tab = std::clamp(std::atoi(args[++i].c_str()), 0, 5);
        } else if (args[i].rfind("--launcher-tab=", 0) == 0) {
            cli.initial_tab = std::clamp(std::atoi(args[i].c_str() + 15), 0, 5);
        } else if (args[i] == "--open-file-browser") {
            cli.open_file_browser = true;
        } else if (args[i] == "--capture-launcher-bmp" && i + 1 < args.size()) {
            cli.capture_bmp = fs::path(args[++i]);
        } else if (args[i].rfind("--capture-launcher-bmp=", 0) == 0) {
            cli.capture_bmp = fs::path(args[i].substr(23));
        } else if (args[i] == "--launcher-autoplay-ms" && i + 1 < args.size()) {
            int ms = std::atoi(args[++i].c_str());
            if (ms > 0) cli.autoplay_ms = ms;
        } else if (args[i].rfind("--launcher-autoplay-ms=", 0) == 0) {
            int ms = std::atoi(args[i].c_str() + 23);
            if (ms > 0) cli.autoplay_ms = ms;
        }
    }
    return cli;
}
#endif

struct LauncherState {
    int active_tab = 0;
    bool tab_switch_requested = true;
    bool settings_modified = false;
    bool request_play = false;
    bool request_exit = false;
    bool has_valid_game = false;
    app::DiscValidation disc_info{};
    bool has_disc_info = false;
    upscale::PluginProbe probe{};
    std::vector<app::GpuAdapterInfo> gpus;
    std::vector<app::MonitorInfo> monitors;
    file_browser_ui::FileBrowserUi browser;
    std::string status_banner;
    std::string tools_status;
    bool upd_manual_pending = false;
    psiops::UpdateState prev_upd_state = psiops::UpdateState::Idle;

    void RefreshDiscStatus() {
        has_valid_game = app::HasValidConfiguredGame();
        has_disc_info = false;
        disc_info = app::DiscValidation{};
        const std::string& iso = app::Get().iso_path;
        if (!iso.empty()) {
            fs::path p(iso);
            if (p.is_relative()) p = (app::ExeDir() / p).lexically_normal();
            app::ValidateDiscImage(p, disc_info);
            has_disc_info = true;
            if (disc_info.valid) has_valid_game = true;
        }
    }
};

bool HasMockUpdateEnv() {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(L"PSIOPS_UPDATE_MOCK_JSON");
    return v && *v;
#else
    const char* v = std::getenv("PSIOPS_UPDATE_MOCK_JSON");
    return v && *v;
#endif
}

void MaybeStartLauncherUpdateCheck() {
    if ((ui_common::GetUpdatePrefs().check_updates_on_startup && psiops::ShouldAutoCheckForUpdates()) ||
        HasMockUpdateEnv()) {
        if (psiops::GetUpdateSnapshot().state == psiops::UpdateState::Idle) {
            psiops::StartUpdateCheck(false);
        }
    }
}

void PollLauncherUpdateTransitions(LauncherState& st, const psiops::UpdateStatusSnapshot& snap) {
    if (st.upd_manual_pending && snap.state != psiops::UpdateState::Checking &&
        snap.state != psiops::UpdateState::Idle) {
        st.upd_manual_pending = false;
        if (snap.state == psiops::UpdateState::UpToDate) {
            st.status_banner = std::string("Up to date (") + PSIOPS_VERSION_TAG + ")";
        } else if (snap.state == psiops::UpdateState::UpdateAvailable) {
            std::string tag = !snap.latest_tag.empty() ? snap.latest_tag : ("v" + snap.latest_version);
            st.status_banner = "Update available: " + tag;
        } else if (snap.state == psiops::UpdateState::Error) {
            st.status_banner = snap.status_message.empty()
                                   ? "Update check failed (network error)."
                                   : ui_common::SanitizeUiText(snap.status_message, true, 140);
        }
    } else if (st.prev_upd_state == psiops::UpdateState::Downloading &&
               snap.state != psiops::UpdateState::Downloading) {
        if (snap.state == psiops::UpdateState::ReadyToRestart) {
            st.status_banner =
                "Update " + snap.latest_tag + " verified & staged! Click 'Restart & Apply Update' to install.";
        } else if (snap.state == psiops::UpdateState::Error) {
            st.status_banner = snap.status_message.empty()
                                   ? "Self-update download/staging failed."
                                   : ui_common::SanitizeUiText(snap.status_message, true, 140);
        }
    }
    st.prev_upd_state = snap.state;
}

bool DrawLauncherUpdateAlertIfNeeded(LauncherState& st, const psiops::UpdateStatusSnapshot& snap, float ui_scale) {
    if (!ui_common::ShouldShowUpdateBanner(snap, ui_common::GetUpdatePrefs().dismissed_update_tag)) {
        return false;
    }
    ui_common::UpdateCardContext uctx{};
    uctx.in_launcher = true;
    uctx.ui_scale = ui_scale;
    uctx.on_update_now = [&] {
        if (psiops::StartDownloadAndStageUpdate()) {
            st.status_banner =
                "Downloading & staging " + (snap.asset_name.empty() ? snap.latest_tag : snap.asset_name) + "...";
        } else {
            psiops::UpdateStatusSnapshot err_s = psiops::GetUpdateSnapshot();
            st.status_banner = err_s.status_message.empty()
                                   ? "Could not start self-update download."
                                   : ui_common::SanitizeUiText(err_s.status_message, true, 140);
        }
    };
    uctx.on_apply_and_restart = [&] {
        if (st.settings_modified || st.has_valid_game) {
            app::Save();
            st.settings_modified = false;
        }
        if (psiops::ApplyStagedUpdateAndRelaunch()) {
            st.request_play = false;
            st.request_exit = true;
        } else {
            psiops::UpdateStatusSnapshot err_s = psiops::GetUpdateSnapshot();
            st.status_banner = err_s.status_message.empty()
                                   ? "Failed to launch staged update helper."
                                   : ui_common::SanitizeUiText(err_s.status_message, true, 140);
        }
    };
    uctx.on_open_release_page = [&](const std::string& url) {
        std::string target = url.empty() ? std::string(PSIOPS_RELEASES_URL) : url;
        psiops::OpenReleasePageInBrowser(target);
        st.status_banner = ui_common::IsGamescope()
                               ? ("Release page: " + target + " (browse in Desktop Mode if hidden)")
                               : ("Opened release page in browser: " + target);
    };
    uctx.on_dismiss = [&](const std::string& tag) {
        ui_common::GetUpdatePrefs().dismissed_update_tag = tag;
        st.settings_modified = true;
        st.status_banner = "Dismissed update alert for " + tag + " (click 'Check for Updates' to show again).";
    };
    ui_common::DrawUpdateAlertCard(snap, uctx);
    ImGui::Spacing();
    return true;
}

void DrawPlayAndDiscTab(LauncherState& st, void* native_owner_win, bool show_upd_banner) {
    app::Settings& s = app::Get();

    // Hero Banner Card
    ImGui::BeginChild("HeroCard", ImVec2(0.0f, 108.0f), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.88f, 0.98f, 1.0f));
    ImGui::TextUnformatted("PSI-OPS: THE MINDGATE CONSPIRACY");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ui_common::DrawBadge("v" PSIOPS_VERSION_STR "  |  Native x64 Static Recompilation", ImVec4(0.11f, 0.34f, 0.48f, 1.0f));
    ImGui::TextDisabled("Original Xbox (Title ID 0x4D570016, XDK 5849)  |  Native 60 FPS  |  16:9 Widescreen  |  9-Upscaler Suite");
    ImGui::Spacing();

    // Quick Hardware Preset strip on Play tab (all 5 distinct one-click presets)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Quick Preset:");
    static constexpr app::HardwarePreset kQuick[5] = {
        app::HardwarePreset::SteamDeck, app::HardwarePreset::HandheldIgpu, app::HardwarePreset::Mainstream1080p,
        app::HardwarePreset::High1440p, app::HardwarePreset::Ultra4k};
    static constexpr const char* kQuickLbl[5] = {
        "Steam Deck (1280x800)", "Handheld / iGPU", "1080p Mainstream", "1440p High", "4K Ultra"};
    for (int i = 0; i < 5; ++i) {
        ImGui::SameLine();
        bool active = (s.hardware_preset == kQuick[i]);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.50f, 0.64f, 1.0f));
        if (ImGui::Button(kQuickLbl[i])) {
            upscale::ApplyHardwarePreset(s, kQuick[i], st.probe);
            app::MarkHardwarePresetUserEdited();
            st.settings_modified = true;
        }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Game Disc Image Card
    ImGui::BeginChild("DiscCard", ImVec2(0.0f, show_upd_banner ? 156.0f : 185.0f), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
    ImGui::TextUnformatted("Game Disc Image (.xiso.iso / .iso)");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    if (st.has_disc_info && st.disc_info.valid) {
        ui_common::DrawBadge("[Verified Retail]", ImVec4(0.11f, 0.48f, 0.28f, 1.0f));
        ImGui::SameLine();
        ImGui::Text("%s (Title ID 0x%08X, XDK %u)", st.disc_info.title_name.c_str(), st.disc_info.title_id,
                    unsigned(st.disc_info.xdk_build));
        ImGui::TextDisabled("Configured Disc: %s", s.iso_path.c_str());
    } else if (st.has_valid_game) {
        ui_common::DrawBadge("[Auto-Discovered Ready]", ImVec4(0.11f, 0.48f, 0.28f, 1.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted("Verified Psi-Ops retail USA Xbox disc image detected automatically.");
        ImGui::TextDisabled("Zero copyrighted assets are bundled - your local read-only disc image is mounted directly.");
    } else if (st.has_disc_info && !st.disc_info.valid) {
        ui_common::DrawBadge(st.disc_info.code.c_str(), ImVec4(0.60f, 0.14f, 0.16f, 1.0f));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", st.disc_info.error.c_str());
    } else {
        ui_common::DrawBadge("Disc Image Required", ImVec4(0.55f, 0.36f, 0.08f, 1.0f));
        ImGui::SameLine();
        ImGui::TextWrapped("Select your legally owned Psi-Ops: The Mindgate Conspiracy (USA) Xbox .xiso.iso / .iso.");
    }

    ImGui::Spacing();
    if (ImGui::Button("Browse Disc Image (In-App Gamepad/Touch)", ImVec2(320.0f, 36.0f))) {
        st.browser.Open();
    }
    ImGui::SameLine();
    if (ImGui::Button("System File Dialog...", ImVec2(190.0f, 36.0f))) {
        fs::path picked;
        if (app::BrowseDiscImage(native_owner_win, picked)) {
            app::DiscValidation v;
            if (app::ValidateDiscImage(picked, v)) {
                s.iso_path = picked.string();
                st.settings_modified = true;
                st.RefreshDiscStatus();
                st.status_banner = "Verified retail disc image configured: " + picked.filename().string();
            } else {
                st.disc_info = v;
                st.has_disc_info = true;
                st.status_banner = "Invalid disc image: " + v.error;
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Active Summary + Skip Launcher Toggle
    ImGui::BeginChild("SummaryCard", ImVec2(0.0f, 0.0f), true);
    ImGui::Text("Active Graphics: %s  |  Quality: %s  |  FPS Cap: %s  |  Present: %s",
                upscale::ModeName(s.upscaler), upscale::QualityName(s.upscale_quality),
                s.fps_cap == 0 ? "Unlimited"
                               : (s.fps_cap == app::kFpsCapRefresh ? "Monitor Hz" : std::to_string(s.fps_cap).c_str()),
                app::PresentModeName(s.present_mode));
    ImGui::Spacing();

    // Plugin badges at a glance
    ImGui::TextUnformatted("Upscaler Plugins:");
    ImGui::SameLine();
    ui_common::DrawBadge(st.probe.dlss_ready ? "DLSS 4 [Ready]" : "DLSS 4 [N/A]",
                         st.probe.dlss_ready ? ImVec4(0.11f, 0.45f, 0.28f, 1.0f) : ImVec4(0.24f, 0.28f, 0.34f, 1.0f));
    ImGui::SameLine();
    ui_common::DrawBadge(st.probe.fsr3_ready ? "FSR 3.1 [Ready]" : "FSR 3.1 [N/A]",
                         st.probe.fsr3_ready ? ImVec4(0.11f, 0.45f, 0.28f, 1.0f) : ImVec4(0.24f, 0.28f, 0.34f, 1.0f));
    ImGui::SameLine();
    ui_common::DrawBadge(st.probe.xess_ready ? "XeSS 2 [Ready]" : "XeSS 2 [N/A]",
                         st.probe.xess_ready ? ImVec4(0.11f, 0.45f, 0.28f, 1.0f) : ImVec4(0.24f, 0.28f, 0.34f, 1.0f));
    ImGui::SameLine();
    ui_common::DrawBadge("6 Spatial [Ready]", ImVec4(0.10f, 0.42f, 0.52f, 1.0f));

    ImGui::Spacing();
    if (ImGui::Checkbox("Skip Launcher on Future Starts (Hold Shift or pass --launcher to reopen)", &s.skip_launcher)) {
        st.settings_modified = true;
    }
    if (!st.status_banner.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.65f, 1.0f), "%s", st.status_banner.c_str());
    }
    ImGui::EndChild();
}

void DrawLauncherFrame(LauncherState& st, void* native_owner_win, float ui_scale) {
    psiops::UpdateStatusSnapshot upd_snap = psiops::GetUpdateSnapshot();
    PollLauncherUpdateTransitions(st, upd_snap);

    ImGuiIO& io = ImGui::GetIO();
    ui_common::GamepadNavEvents nav = ui_common::PollGamepadNavToImGui(io);
    // LB/RB/Start are suspended while a modal is up: the file browser, or a pending click-to-rebind capture.
    const bool modal_blocking = st.browser.IsOpen() || mkb::IsRebinding();
    if (!modal_blocking) {
        if (nav.tab_prev) {
            st.active_tab = (st.active_tab + 5) % 6;
            st.tab_switch_requested = true;
        }
        if (nav.tab_next) {
            st.active_tab = (st.active_tab + 1) % 6;
            st.tab_switch_requested = true;
        }
        if (nav.start_pressed && st.has_valid_game) {
            st.request_play = true;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##PsiOpsLauncherRoot", nullptr, root_flags)) {
        // Header strip
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.88f, 0.98f, 1.0f));
        ImGui::TextUnformatted("Psi-Ops: The Mindgate Conspiracy v" PSIOPS_VERSION_STR " - Pre-Game Launcher");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("   (LB / RB switch tabs  |  F1 in-game overlay  |  F2 quick cheats  |  Start to Play)");
        ui_common::DrawLauncherHeaderUpdateButton(upd_snap, ui_scale, [&] {
            if (!ui_common::GetUpdatePrefs().dismissed_update_tag.empty()) {
                ui_common::GetUpdatePrefs().dismissed_update_tag.clear();
                if (!HasMockUpdateEnv()) st.settings_modified = true;
            }
            st.upd_manual_pending = true;
            st.status_banner = "Checking GitHub Releases for updates...";
            psiops::StartUpdateCheck(true);
        });
        ImGui::Separator();

        static constexpr const char* kTabs[6] = {
            "Play & Game Disc", "Upscaling & Graphics", "Display & GPU",
            "Audio",            "Controls",             "Tools & Diagnostics",
        };

        float footer_h = 56.0f;
        float content_h = std::max(180.0f, ImGui::GetContentRegionAvail().y - footer_h);

        if (ImGui::BeginTabBar("LauncherTabBar")) {
            for (int t = 0; t < 6; ++t) {
                ImGuiTabItemFlags tf =
                    (st.tab_switch_requested && st.active_tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kTabs[t], nullptr, tf)) {
                    if (!st.tab_switch_requested) st.active_tab = t;
                    ImGui::BeginChild("LauncherTabContent", ImVec2(0.0f, content_h - 36.0f), false);
                    bool show_upd_banner = DrawLauncherUpdateAlertIfNeeded(st, upd_snap, ui_scale);
                    bool dummy_apply = false;
                    if (t == 0) {
                        DrawPlayAndDiscTab(st, native_owner_win, show_upd_banner);
                    } else if (t == 1) {
                        ui_common::DrawUpscalingGraphicsPanel(st.probe, false, st.settings_modified, dummy_apply);
                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
                        ImGui::TextUnformatted("Ahead-of-Time Shader & Upscaler Boot Pre-Warming");
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        gpu::CacheStats cs = gpu::GetCacheStats();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("Compiled Shaders: %u  |  Cached PSOs: %u  |  Disk: %.2f MB", cs.shaders, cs.psos,
                                    double(cs.disk_bytes) / (1024.0 * 1024.0));
                        ImGui::SameLine();
                        if (cs.rebuild_pending) {
                            ui_common::DrawBadge("[Rebuild Scheduled]", ImVec4(0.55f, 0.36f, 0.08f, 1.0f));
                        } else if (cs.aot_ready && cs.shaders > 0) {
                            ui_common::DrawBadge("[AOT Zero-Stutter Ready]", ImVec4(0.11f, 0.45f, 0.28f, 1.0f));
                        } else {
                            ui_common::DrawBadge("[Pre-Compiles on Boot]", ImVec4(0.10f, 0.42f, 0.52f, 1.0f));
                        }
                        ImGui::TextDisabled(
                            "NV2A shaders, GPU pipelines, and active temporal upscalers (DLSS 4 / FSR 3.1 / XeSS 2) "
                            "pre-compile eagerly at boot before gameplay begins.");
                    } else if (t == 2) {
                        ui_common::DrawDisplayGpuPanel(st.gpus, st.monitors, false, st.settings_modified, dummy_apply);
                    } else if (t == 3) {
                        ui_common::DrawAudioPanel(st.settings_modified);
                    } else if (t == 4) {
                        mkb::DrawControlsSettingsPanel(false, &st.settings_modified);
                        ImGui::Spacing();
                        ui_common::DrawInGameControlsOptionsPanel(st.settings_modified);
                    } else if (t == 5) {
                        ui_common::DrawUpdatesDiagnosticsSection(
                            upd_snap, true, ui_scale, st.settings_modified, st.upd_manual_pending, st.tools_status, [&] {
                                if (st.settings_modified || st.has_valid_game) {
                                    app::Save();
                                    st.settings_modified = false;
                                }
                                if (psiops::ApplyStagedUpdateAndRelaunch()) {
                                    st.request_play = false;
                                    st.request_exit = true;
                                } else {
                                    psiops::UpdateStatusSnapshot err_s = psiops::GetUpdateSnapshot();
                                    st.tools_status =
                                        err_s.status_message.empty()
                                            ? "Failed to launch staged update helper."
                                            : ui_common::SanitizeUiText(err_s.status_message, true, 140);
                                }
                            });
                        ui_common::DrawToolsDiagnosticsPanel(st.probe, false, st.tools_status);
                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
                        ImGui::TextUnformatted("Ahead-of-Time (AOT) Shader & Pipeline Pre-Compilation");
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        gpu::CacheStats cs = gpu::GetCacheStats();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("Compiled Shaders: %u  |  Cached PSOs: %u  |  Cache Size: %.2f MB", cs.shaders,
                                    cs.psos, double(cs.disk_bytes) / (1024.0 * 1024.0));
                        ImGui::SameLine();
                        if (cs.rebuild_pending) {
                            ui_common::DrawBadge("[Rebuild Scheduled on Next Launch]", ImVec4(0.55f, 0.36f, 0.08f, 1.0f));
                        } else if (cs.aot_ready && cs.shaders > 0) {
                            ui_common::DrawBadge("[AOT Ready]", ImVec4(0.11f, 0.45f, 0.28f, 1.0f));
                        } else {
                            ui_common::DrawBadge("[Cold Boot Pre-Compile]", ImVec4(0.10f, 0.42f, 0.52f, 1.0f));
                        }
                        if (ImGui::Button("Rebuild / Pre-Compile Shader Cache on Next Launch", ImVec2(390.0f, 32.0f))) {
                            gpu::ScheduleCacheRebuildOnNextLaunch();
                            st.tools_status =
                                "Shader & PSO cache cleared. Full AOT pre-compilation will run on next launch.";
                        }
                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
                        ImGui::TextUnformatted("In-Game Cheats & Psychic Trainer (F2)");
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        ui_common::DrawCheatsPanel(ui_scale, st.settings_modified, &st.tools_status);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            st.tab_switch_requested = false;
            ImGui::EndTabBar();
        }
        // A rebind capture only lives while the Controls tab is the one on screen.
        if (st.active_tab != 4) mkb::CancelRebind();

        // Bottom Footer Bar
        ImGui::Separator();
        ImGui::Spacing();
        if (st.settings_modified) {
            if (ImGui::Button("Save Settings", ImVec2(150.0f, 36.0f))) {
                app::Save();
                st.settings_modified = false;
                st.status_banner = "Settings saved to settings.toml.";
            }
            ImGui::SameLine();
        }
        float right_x = std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - 390.0f);
        ImGui::SetCursorPosX(right_x);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.50f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.62f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.42f, 0.28f, 1.0f));
        if (ImGui::Button("PLAY PSI-OPS (Start)", ImVec2(220.0f, 36.0f))) {
            st.request_play = true;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Exit", ImVec2(140.0f, 36.0f))) {
            st.request_exit = true;
        }
    }
    ImGui::End();

    // In-App XISO File Browser Modal
    fs::path picked_iso;
    if (st.browser.Draw(picked_iso)) {
        app::Get().iso_path = picked_iso.string();
        st.settings_modified = true;
        st.RefreshDiscStatus();
        st.status_banner = "Verified retail disc image selected: " + picked_iso.filename().string();
    }
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

struct Win32LauncherApp {
    static constexpr uint32_t kBuffers = 2;

    HWND hwnd = nullptr;
    uint32_t width = 1180;
    uint32_t height = 740;
    bool quit_msg = false;
    bool programmatic_destroy = false;
    bool launching_game = false;
    UINT pending_dpi = 0;  // set by WM_DPICHANGED, consumed on the render loop

    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    uint32_t rtv_inc = 0;
    ComPtr<ID3D12Resource> bb[kBuffers];
    ComPtr<ID3D12CommandAllocator> alloc[kBuffers];
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_ev = nullptr;
    uint64_t fence_val = 0;

    void WaitGpu() {
        if (!queue || !fence || !fence_ev) return;
        const uint64_t target = ++fence_val;
        if (SUCCEEDED(queue->Signal(fence.Get(), target)) && fence->GetCompletedValue() < target) {
            fence->SetEventOnCompletion(target, fence_ev);
            WaitForSingleObject(fence_ev, 5000);
        }
    }

    bool CaptureBackbuffer(ID3D12Resource* src, const fs::path& bmp_path) {
        D3D12_RESOURCE_DESC rd = src->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 row_sz = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_sz, &total);

        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&rb)))) {
            return false;
        }

        uint32_t bi = swap->GetCurrentBackBufferIndex();
        alloc[bi]->Reset();
        cl->Reset(alloc[bi].Get(), nullptr);

        D3D12_RESOURCE_BARRIER b0{};
        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0.Transition = {src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                         D3D12_RESOURCE_STATE_COPY_SOURCE};
        cl->ResourceBarrier(1, &b0);

        D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dl.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

        D3D12_RESOURCE_BARRIER b1 = b0;
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b1);
        cl->Close();

        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        uint8_t* ptr = nullptr;
        D3D12_RANGE rr{0, size_t(total)};
        if (FAILED(rb->Map(0, &rr, reinterpret_cast<void**>(&ptr))) || !ptr) return false;
        bool ok = ui_common::WriteBmp32File(bmp_path, uint32_t(rd.Width), uint32_t(rd.Height), ptr,
                                            fp.Footprint.RowPitch, false, true);
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        return ok;
    }

    // Resizes the swapchain to the current client rect and recreates the backbuffer RTVs. GPU must be idle.
    void ResizeSwapchain(uint32_t w, uint32_t h) {
        for (auto& b : bb) b.Reset();
        if (SUCCEEDED(swap->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            width = w;
            height = h;
        } else {
            LOGW("launcher: swapchain ResizeBuffers(%ux%u) failed; keeping %ux%u", w, h, width, height);
        }
        for (uint32_t i = 0; i < kBuffers; ++i) {
            swap->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE rh = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rh.ptr += size_t(i) * rtv_inc;
            dev->CreateRenderTargetView(bb[i].Get(), nullptr, rh);
        }
    }

    void ShutdownD3D() {
        WaitGpu();
        if (fence_ev) {
            CloseHandle(fence_ev);
            fence_ev = nullptr;
        }
        fence.Reset();
        cl.Reset();
        for (auto& a : alloc) a.Reset();
        for (auto& b : bb) b.Reset();
        srv_heap.Reset();
        rtv_heap.Reset();
        swap.Reset();
        queue.Reset();
        dev.Reset();
    }

    void DestroyLauncherWindow() {
        if (hwnd) {
            programmatic_destroy = true;
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        // Explicitly drain any stray WM_QUIT messages from this UI thread's Win32 message queue
        // so creating the game window and entering GetMessageW() never terminates on frame 0.
        MSG qmsg{};
        while (PeekMessageW(&qmsg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        }
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
        auto* self = reinterpret_cast<Win32LauncherApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_CLOSE) {
            if (self) self->quit_msg = true;
            return 0;
        }
        if (msg == WM_DPICHANGED) {
            // Per-Monitor-V2: moved to a monitor with a different scale. Adopt the suggested window rect now;
            // fonts, style and swapchain are rebuilt on the render loop (never mid-frame).
            if (self) self->pending_dpi = HIWORD(wp);
            if (const RECT* r = reinterpret_cast<const RECT*>(lp)) {
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        if (msg == WM_DESTROY) {
            if (self) self->quit_msg = true;
            // Only post WM_QUIT when the user/OS externally destroyed the launcher to quit,
            // never during programmatic DestroyWindow teardown after clicking PLAY PSI-OPS.
            if (!self || (!self->programmatic_destroy && !self->launching_game)) {
                PostQuitMessage(0);
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool Run(HINSTANCE inst, const LauncherCli& cli) {
        LauncherState st{};
        st.active_tab = cli.initial_tab;
        st.tab_switch_requested = true;
        st.probe = upscale::ProbePlugins(false);
        st.gpus = app::EnumerateGpuAdapters();
        st.monitors = app::EnumerateMonitors();
        st.RefreshDiscStatus();
        if (cli.open_file_browser) st.browser.Open();
        MaybeStartLauncherUpdateCheck();
        st.prev_upd_state = psiops::GetUpdateSnapshot().state;

        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PsiOpsLauncherWnd";
        RegisterClassExW(&wc);

        UINT dpi = GetDpiForSystem();
        float dpi_scale = std::clamp(float(dpi) / 96.0f, 1.0f, 2.0f);
        width = uint32_t(std::lround(1180.0f * dpi_scale));
        height = uint32_t(std::lround(740.0f * dpi_scale));
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT r{0, 0, LONG(width), LONG(height)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        int ww = r.right - r.left;
        int wh = r.bottom - r.top;
        int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2);
        int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2);

        hwnd = CreateWindowExW(0, wc.lpszClassName,
                               L"Psi-Ops: The Mindgate Conspiracy v" PSIOPS_WVERSION_STR L" - Launcher", style, x, y, ww,
                               wh, nullptr, nullptr, inst, nullptr);
        if (!hwnd) {
            UnregisterClassW(wc.lpszClassName, inst);
            return false;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
            FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            factory.Reset();
            ShutdownD3D();
            DestroyLauncherWindow();
            UnregisterClassW(wc.lpszClassName, inst);
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
            factory.Reset();
            ShutdownD3D();
            DestroyLauncherWindow();
            UnregisterClassW(wc.lpszClassName, inst);
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferCount = kBuffers;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scd, nullptr, nullptr, &sc1)) ||
            FAILED(sc1.As(&swap))) {
            sc1.Reset();
            factory.Reset();
            ShutdownD3D();
            DestroyLauncherWindow();
            UnregisterClassW(wc.lpszClassName, inst);
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        // Drop local swapchain/factory references immediately so ShutdownD3D releases every COM object before DestroyWindow.
        sc1.Reset();
        factory.Reset();

        D3D12_DESCRIPTOR_HEAP_DESC rhd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC shd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap))) ||
            FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)))) {
            ShutdownD3D();
            DestroyLauncherWindow();
            UnregisterClassW(wc.lpszClassName, inst);
            return false;
        }
        rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (uint32_t i = 0; i < kBuffers; ++i) {
            swap->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE rh = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rh.ptr += size_t(i) * rtv_inc;
            dev->CreateRenderTargetView(bb[i].Get(), nullptr, rh);
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
        }
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&cl));
        cl->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ui_common::ApplyMindgateTheme(dpi_scale);
        ui_common::LoadUiFont(io, dpi_scale);

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX12_Init(dev.Get(), int(kBuffers), DXGI_FORMAT_B8G8R8A8_UNORM, srv_heap.Get(),
                            srv_heap->GetCPUDescriptorHandleForHeapStart(),
                            srv_heap->GetGPUDescriptorHandleForHeapStart());

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        LOGR("launcher: opened Win64 D3D12 window (%ux%u, autoplay_ms=%d)", width, height, cli.autoplay_ms);

        auto t0 = std::chrono::steady_clock::now();
        uint32_t frames_rendered = 0;
        bool launch_game = false;
        while (!quit_msg && !st.request_exit && !st.request_play) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) quit_msg = true;
            }
            if (quit_msg) break;

            if (pending_dpi != 0) {
                const float new_scale = std::clamp(float(pending_dpi) / 96.0f, 1.0f, 2.0f);
                pending_dpi = 0;
                RECT cr{};
                GetClientRect(hwnd, &cr);
                const uint32_t cw = uint32_t(std::max<LONG>(1, cr.right - cr.left));
                const uint32_t ch = uint32_t(std::max<LONG>(1, cr.bottom - cr.top));
                const bool scale_changed = std::fabs(new_scale - dpi_scale) > 0.01f;
                if (scale_changed || cw != width || ch != height) {
                    WaitGpu();
                    // Release the backend's font texture/SRV *before* the atlas is rebuilt, then recreate it.
                    ImGui_ImplDX12_InvalidateDeviceObjects();
                    if (scale_changed) {
                        dpi_scale = new_scale;
                        ui_common::LoadUiFont(io, dpi_scale);
                        ui_common::ApplyMindgateTheme(dpi_scale);
                    }
                    if (cw != width || ch != height) ResizeSwapchain(cw, ch);
                    ImGui_ImplDX12_CreateDeviceObjects();
                    LOGR("launcher: DPI change -> scale %.2f, %ux%u", dpi_scale, width, height);
                }
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ui_common::SyncGamepadBackendFlag(io);  // after the backend (which only knows XInput pads)
            ImGui::NewFrame();
            DrawLauncherFrame(st, hwnd, dpi_scale);
            ImGui::Render();

            uint32_t bi = swap->GetCurrentBackBufferIndex();
            alloc[bi]->Reset();
            cl->Reset(alloc[bi].Get(), nullptr);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {bb[bi].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                            D3D12_RESOURCE_STATE_RENDER_TARGET};
            cl->ResourceBarrier(1, &b);

            D3D12_CPU_DESCRIPTOR_HANDLE rh = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rh.ptr += size_t(bi) * rtv_inc;
            const float clear[4] = {0.045f, 0.062f, 0.090f, 1.0f};
            cl->ClearRenderTargetView(rh, clear, 0, nullptr);
            cl->OMSetRenderTargets(1, &rh, FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
            cl->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl.Get());

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cl->ResourceBarrier(1, &b);
            cl->Close();

            ID3D12CommandList* lists[] = {cl.Get()};
            queue->ExecuteCommandLists(1, lists);
            swap->Present(1, 0);
            WaitGpu();

            ++frames_rendered;
            if (!cli.capture_bmp.empty() && frames_rendered >= 4) {
                bool cap_ok = CaptureBackbuffer(bb[bi].Get(), cli.capture_bmp);
                LOGR("launcher: captured Win64 BMP %s (%ux%u, %s)", cli.capture_bmp.string().c_str(), width, height,
                     cap_ok ? "ok" : "failed");
                st.request_exit = true;
                break;
            }
            if (cli.autoplay_ms > 0 && cli.capture_bmp.empty() && frames_rendered >= 2) {
                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                        .count();
                if (elapsed_ms >= cli.autoplay_ms) {
                    LOGR("launcher: autoplay triggered after %lld ms (%u frames) -> PLAY PSI-OPS",
                         (long long)elapsed_ms, frames_rendered);
                    st.request_play = true;
                }
            }
        }

        if (st.request_play) {
            if (st.settings_modified) app::Save();
            launch_game = true;
        } else {
            psiops::ShutdownUpdateChecker();
        }
        // Strictly ZERO settings.toml write on unconfigured Exit!

        mkb::CancelRebind();  // never carry a pending capture into the game window
        launching_game = launch_game;
        WaitGpu();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ShutdownD3D();
        DestroyLauncherWindow();
        UnregisterClassW(wc.lpszClassName, inst);
        if (launch_game) DebounceStartRelease();
        LOGR("launcher: closed Win64 launcher (launch_game=%s, frames=%u)", launch_game ? "true" : "false",
             frames_rendered);
        return launch_game;
    }
};

#else

bool RunLinuxLauncher(const LauncherCli& cli) {
    LauncherState st{};
    st.active_tab = cli.initial_tab;
    st.tab_switch_requested = true;
    st.probe = upscale::ProbePlugins(false);
    st.gpus = app::EnumerateGpuAdapters();
    st.monitors = app::EnumerateMonitors();
    st.RefreshDiscStatus();
    if (cli.open_file_browser) st.browser.Open();
    MaybeStartLauncherUpdateCheck();
    st.prev_upd_state = psiops::GetUpdateSnapshot().state;

    bool is_deck = (std::getenv("SteamDeck") && std::strcmp(std::getenv("SteamDeck"), "1") == 0);
    app::Mode desk = app::Desktop();
    int win_w = 1280;
    int win_h = 800;
    if (!is_deck && cli.capture_bmp.empty() && desk.width > 1366 && desk.height > 860) {
        win_w = 1180;
        win_h = 740;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (is_deck || (desk.width == 1280 && desk.height == 800)) flags |= SDL_WINDOW_BORDERLESS;
    SDL_Window* win =
        SDL_CreateWindow("Psi-Ops: The Mindgate Conspiracy v" PSIOPS_VERSION_STR " - Launcher", win_w, win_h, flags);
    if (!win) {
        LOGE("launcher: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) {
        LOGE("launcher: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_EVENT_QUIT);
        SDL_FlushEvents(SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
        return false;
    }
    SDL_SetRenderVSync(ren, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ui_common::ApplyMindgateTheme(1.0f);
    ui_common::LoadUiFont(io, 1.0f);

    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    ImGui_ImplSDLRenderer3_Init(ren);
    LOGR("launcher: opened Linux SDL3 window (%dx%d, autoplay_ms=%d)", win_w, win_h, cli.autoplay_ms);

    auto t0 = std::chrono::steady_clock::now();
    uint32_t frames_rendered = 0;
    bool quit_msg = false;
    bool launch_game = false;

    while (!quit_msg && !st.request_exit && !st.request_play) {
        SDL_Event ev{};
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(win))) {
                quit_msg = true;
            }
        }
        if (quit_msg) break;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ui_common::SyncGamepadBackendFlag(io);  // SDL3 backend runs in Manual mode with no pads of its own
        ImGui::NewFrame();
        DrawLauncherFrame(st, win, 1.0f);
        ImGui::Render();

        SDL_SetRenderDrawColor(ren, 12, 16, 23, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);

        ++frames_rendered;
        if (!cli.capture_bmp.empty() && frames_rendered >= 4) {
            if (SDL_Surface* surf = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_Surface* bgra = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_BGRA32);
                SDL_DestroySurface(surf);
                if (bgra) {
                    bool cap_ok = ui_common::WriteBmp32File(cli.capture_bmp, uint32_t(bgra->w), uint32_t(bgra->h),
                                                            static_cast<const uint8_t*>(bgra->pixels),
                                                            uint32_t(bgra->pitch), false, true);
                    LOGR("launcher: captured Linux BMP %s (%dx%d, %s)", cli.capture_bmp.string().c_str(), bgra->w,
                         bgra->h, cap_ok ? "ok" : "failed");
                    SDL_DestroySurface(bgra);
                }
            }
            SDL_RenderPresent(ren);
            st.request_exit = true;
            break;
        }
        SDL_RenderPresent(ren);
        if (cli.autoplay_ms > 0 && cli.capture_bmp.empty() && frames_rendered >= 2) {
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (elapsed_ms >= cli.autoplay_ms) {
                LOGR("launcher: autoplay triggered after %lld ms (%u frames) -> PLAY PSI-OPS", (long long)elapsed_ms,
                     frames_rendered);
                st.request_play = true;
            }
        }
    }

    if (st.request_play) {
        if (st.settings_modified) app::Save();
        launch_game = true;
    } else {
        psiops::ShutdownUpdateChecker();
    }
    // Strictly ZERO settings.toml write on unconfigured Exit!

    mkb::CancelRebind();  // never carry a pending capture into the game window
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    // Pump & drain any SDL_EVENT_QUIT or SDL_EVENT_WINDOW_* events emitted when
    // tearing down the launcher window so the Vulkan game window cannot exit on frame 0.
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_EVENT_QUIT);
    SDL_FlushEvents(SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
    if (launch_game) DebounceStartRelease();
    LOGR("launcher: closed Linux launcher (launch_game=%s, frames=%u)", launch_game ? "true" : "false",
         frames_rendered);
    return launch_game;
}

#endif

}  // namespace

bool RunLauncher(void* hinstance) {
    LauncherCli cli = ParseLauncherCli();
    input::Init();
    ui_common::ResetGamepadNavState(true);
#ifdef _WIN32
    Win32LauncherApp app;
    HINSTANCE inst = hinstance ? static_cast<HINSTANCE>(hinstance) : GetModuleHandleW(nullptr);
    return app.Run(inst, cli);
#else
    (void)hinstance;
    return RunLinuxLauncher(cli);
#endif
}

}  // namespace launcher_ui
