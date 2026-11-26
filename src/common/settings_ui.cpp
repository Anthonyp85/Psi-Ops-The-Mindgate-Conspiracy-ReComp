// SPDX-License-Identifier: GPL-2.0-or-later
// Live-Synced In-Game F1 / BACK+L3 Settings Overlay (5 tabs) and Upgraded Live HUD
// (FPS | ms | Upscaler | Render -> Display).

#include "common/settings_ui.h"
#include "common/input_recorder.h"
#include "common/mkb_input.h"
#include "common/ui_common.h"
#include "upscale/upscaler_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "app/app.h"
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

namespace settings_ui {

namespace {

struct OverlayState {
    bool initialized = false;
    bool theme_applied = false;
    bool show_hud = false;
    int active_tab = 0;
    bool chord_prev = false;
    bool was_open = false;
    upscale::PluginProbe probe{};
    std::vector<app::GpuAdapterInfo> gpus;
    std::vector<app::MonitorInfo> monitors;
    std::string diag_status;
};

std::atomic<bool> g_show_hud{false};
std::atomic<bool> g_suppress_warmup_toast{false};
// 0 = waiting for boot AOT completion, 1..UINT64_MAX-1 = steady_clock start ms, UINT64_MAX = finished/suppressed.
std::atomic<uint64_t> g_warmup_toast_start_ms{0};

OverlayState& St() {
    static OverlayState s;
    return s;
}

uint64_t SteadyNowMs() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    uint64_t u = (ms > 0) ? uint64_t(ms) : 1ull;
    return (u == UINT64_MAX) ? (UINT64_MAX - 1ull) : u;
}

bool IsAutomationCliArg(std::string_view arg) {
    return arg.rfind("--capture-", 0) == 0 || arg == "--replay" || arg.rfind("--replay=", 0) == 0 ||
           arg.rfind("--replay-", 0) == 0 || arg.rfind("--record-input", 0) == 0 ||
           arg.rfind("--state-log", 0) == 0 || arg == "--open-settings";
}

bool CheckSuppressWarmupToastCliOrEnv() {
#ifdef _WIN32
    static constexpr const wchar_t* kEnvVars[] = {
        L"XBE_OPEN_SETTINGS",    L"XBE_CAPTURE_UI_BMP",  L"XBE_CAPTURE_DIR",  L"XBE_AUTOTEST_OUT_DIR",
        L"XBE_AUTOTEST_SCRIPT",  L"XBE_INPUT_RECORD",    L"XBE_INPUT_REPLAY", L"XBE_STATE_LOG",
    };
    for (const wchar_t* name : kEnvVars) {
        if (const wchar_t* v = _wgetenv(name); v && *v && std::wcscmp(v, L"0") != 0) return true;
    }
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return false;
    const wchar_t* p = cmd;
    bool first = true;
    while (*p) {
        while (*p && std::iswspace(*p)) ++p;
        if (!*p) break;
        std::string tok;
        bool q = false;
        while (*p && (q || !std::iswspace(*p))) {
            if (*p == L'"') q = !q;
            else tok.push_back((*p < 0x80) ? char(*p) : '?');
            ++p;
        }
        if (!first && IsAutomationCliArg(tok)) return true;
        first = false;
    }
    return false;
#else
    static constexpr const char* kEnvVars[] = {
        "XBE_OPEN_SETTINGS",    "XBE_CAPTURE_UI_BMP",  "XBE_CAPTURE_DIR",  "XBE_AUTOTEST_OUT_DIR",
        "XBE_AUTOTEST_SCRIPT",  "XBE_INPUT_RECORD",    "XBE_INPUT_REPLAY", "XBE_STATE_LOG",
    };
    for (const char* name : kEnvVars) {
        if (const char* v = std::getenv(name); v && *v && std::strcmp(v, "0") != 0) return true;
    }
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    size_t pos = 0;
    bool first = true;
    while (pos < raw.size()) {
        size_t nul = raw.find('\0', pos);
        if (nul == std::string::npos) nul = raw.size();
        std::string_view arg(raw.data() + pos, nul - pos);
        if (!first && IsAutomationCliArg(arg)) return true;
        first = false;
        pos = nul + 1;
    }
    return false;
#endif
}

bool ShouldShowShaderWarmupToast(gpu::CacheStats* out_cs, float* out_alpha) {
    uint64_t start_ms = g_warmup_toast_start_ms.load(std::memory_order_relaxed);
    if (start_ms == UINT64_MAX) return false;

    if (g_suppress_warmup_toast.load(std::memory_order_relaxed) || app::IsReplayOrAutotestActive() ||
        input::AutotestActive() || mkb::IsAutotestActive() || input_rec::IsReplayActive() ||
        input_rec::IsRecordActive()) {
        g_warmup_toast_start_ms.store(UINT64_MAX, std::memory_order_relaxed);
        return false;
    }

    gpu::CacheStats cs{};
    bool have_cs = false;
    if (start_ms == 0) {
        cs = gpu::GetCacheStats();
        have_cs = true;
        if (!cs.enabled) {
            g_warmup_toast_start_ms.store(UINT64_MAX, std::memory_order_relaxed);
            return false;
        }
        if (!cs.aot_ready) return false;
        if (cs.shaders == 0 && cs.psos == 0 && cs.aot_compiled_shaders == 0 && cs.aot_built_psos == 0) {
            g_warmup_toast_start_ms.store(UINT64_MAX, std::memory_order_relaxed);
            return false;
        }
        uint64_t now_init = SteadyNowMs();
        uint64_t expected = 0;
        if (g_warmup_toast_start_ms.compare_exchange_strong(expected, now_init, std::memory_order_relaxed)) {
            start_ms = now_init;
        } else {
            start_ms = expected;
            if (start_ms == UINT64_MAX) return false;
        }
    }

    const uint64_t now_ms = SteadyNowMs();
    const uint64_t age_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0ull;
    constexpr uint64_t kShowDurationMs = 3500;
    constexpr uint64_t kFadeStartMs = 2750;
    if (age_ms >= kShowDurationMs) {
        g_warmup_toast_start_ms.store(UINT64_MAX, std::memory_order_relaxed);
        return false;
    }
    const float alpha =
        (age_ms <= kFadeStartMs)
            ? 1.0f
            : std::clamp(float(kShowDurationMs - age_ms) / float(kShowDurationMs - kFadeStartMs), 0.0f, 1.0f);
    if (alpha <= 0.02f) {
        g_warmup_toast_start_ms.store(UINT64_MAX, std::memory_order_relaxed);
        return false;
    }

    if (out_cs) *out_cs = have_cs ? cs : gpu::GetCacheStats();
    if (out_alpha) *out_alpha = alpha;
    return true;
}

void DrawShaderWarmupToast(const gpu::CacheStats& cs, float alpha) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, io.DisplaySize.y - 16.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.82f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    constexpr ImGuiWindowFlags kToastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                             ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##PsiOpsShaderWarmupToast", nullptr, kToastFlags)) {
        if (cs.aot_compiled_shaders > 0) {
            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.65f, 1.0f),
                               "Shader Cache Ready: %u compiled | %u Shaders, %u PSOs (%.0f ms)",
                               cs.aot_compiled_shaders, cs.shaders, cs.psos, cs.aot_ms);
        } else if (cs.aot_built_psos > 0) {
            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.65f, 1.0f),
                               "Shader Cache Ready: %u PSOs built | %u Shaders, %u PSOs (%.0f ms)",
                               cs.aot_built_psos, cs.shaders, cs.psos, cs.aot_ms);
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.65f, 1.0f),
                               "Shader Cache Loaded: %u Shaders | %u PSOs (%.0f ms)", cs.shaders, cs.psos, cs.aot_ms);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

bool CheckOpenSettingsCliOrEnv() {
#ifdef _WIN32
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_SETTINGS"); v && *v && std::wcscmp(v, L"0") != 0) return true;
    const wchar_t* cmd = GetCommandLineW();
    if (cmd && std::wcsstr(cmd, L"--open-settings") != nullptr) return true;
    return false;
#else
    if (const char* v = std::getenv("XBE_OPEN_SETTINGS"); v && *v && std::strcmp(v, "0") != 0) return true;
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    return raw.find("--open-settings") != std::string::npos;
#endif
}

void DrawLiveHud(double fps) {
    gpu::UpscalerStatus ust = gpu::GetUpscalerStatus();
    const app::Settings& s = app::Get();
    double ms = (fps > 0.1) ? (1000.0 / fps) : 0.0;
    const char* up_name =
        !ust.active_label.empty() ? ust.active_label.c_str() : upscale::ModeShortName(s.upscaler);
    uint32_t rw = ust.render_w ? ust.render_w : 640u;
    uint32_t rh = ust.render_h ? ust.render_h : 480u;
    uint32_t dw = ust.vp_w ? ust.vp_w : (ust.bb_w ? ust.bb_w : 1280u);
    uint32_t dh = ust.vp_h ? ust.vp_h : (ust.bb_h ? ust.bb_h : 720u);
    const char* dev_str = (mkb::GetActiveDevice() == mkb::ActiveDevice::Controller) ? "Gamepad" : "M&K";

    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    if (ImGui::Begin("##PsiOpsLiveHud", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.88f, 0.98f, 1.0f));
        ImGui::Text("%.1f FPS", fps);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("| %.1f ms | %s | %ux%u -> %ux%u | %s | F1 Settings", ms, up_name, rw, rh, dw, dh, dev_str);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace

bool HasVisibleUi() {
    return mkb::IsOverlayOpen() || g_show_hud.load(std::memory_order_relaxed) ||
           ShouldShowShaderWarmupToast(nullptr, nullptr);
}

void Init() {
    OverlayState& st = St();
    st.probe = upscale::ProbePlugins(false);
    st.gpus = app::EnumerateGpuAdapters();
    st.monitors = app::EnumerateMonitors();
    st.initialized = true;
    g_show_hud.store(st.show_hud, std::memory_order_relaxed);
    g_suppress_warmup_toast.store(CheckSuppressWarmupToastCliOrEnv(), std::memory_order_relaxed);
    gpu::SetUiVisibleCallback(HasVisibleUi);
    if (CheckOpenSettingsCliOrEnv()) {
        mkb::SetOverlayOpen(true);
    }
}

void OnPadPoll(int port, const input::Pad& pad) {
    if (port != 0) return;
    OverlayState& st = St();
    // Xbox pad bits: bit 5 = BACK (0x20), bit 6 = LTHUMB/L3 (0x40)
    bool chord = ((pad.buttons & 0x0060u) == 0x0060u);
    if (chord && !st.chord_prev) {
        mkb::SetOverlayOpen(!mkb::IsOverlayOpen());
    }
    st.chord_prev = chord;
}

void Draw(double fps) {
    OverlayState& st = St();
    if (!st.initialized) Init();

    if (!st.theme_applied) {
        ui_common::ApplyMindgateTheme(1.0f);
        st.theme_applied = true;
    }

    const bool open_now = mkb::IsOverlayOpen();
    if (st.show_hud || open_now) {
        DrawLiveHud(fps);
    }
    gpu::CacheStats warmup_cs{};
    float warmup_alpha = 0.0f;
    if (ShouldShowShaderWarmupToast(&warmup_cs, &warmup_alpha)) {
        DrawShaderWarmupToast(warmup_cs, warmup_alpha);
    }
    if (!open_now) {
        if (st.was_open) {
            // Overlay just closed (F1 / Esc / BACK+L3 / Resume): don't leave gamepad keys latched in ImGui,
            // and drop any half-finished click-to-rebind capture.
            st.was_open = false;
            ui_common::ReleaseGamepadNavKeys(ImGui::GetIO());
            mkb::CancelRebind();
        }
        return;
    }
    st.was_open = true;

    ImGuiIO& io = ImGui::GetIO();
    ui_common::GamepadNavEvents nav = ui_common::PollGamepadNavToImGui(io);
    // LB/RB tab switching is suspended while a rebind capture is pending on the Controls tab.
    if (mkb::IsRebinding()) nav.tab_prev = nav.tab_next = false;
    if (nav.tab_prev) st.active_tab = (st.active_tab + 4) % 5;
    if (nav.tab_next) st.active_tab = (st.active_tab + 1) % 5;

    ImVec2 ds = io.DisplaySize;
    float w = std::min(ds.x * 0.86f, 880.0f);
    float h = std::min(ds.y * 0.82f, 640.0f);
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);

    bool keep_open = true;
    bool dirty = false;
    bool needs_display_apply = false;

    if (ImGui::Begin("Psi-Ops: The Mindgate Conspiracy v" PSIOPS_VERSION_STR " - Live Settings (F1 / BACK+L3)",
                     &keep_open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::Checkbox("Show Performance & Upscaler HUD", &st.show_hud)) {
            g_show_hud.store(st.show_hud, std::memory_order_relaxed);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  |  LB / RB switch tabs  |  Changes save & apply live");
        ImGui::Separator();

        static constexpr const char* kTabNames[5] = {
            "Upscaling & Graphics", "Display & GPU", "Audio", "Controls", "Diagnostics",
        };
        if (ImGui::BeginTabBar("LiveOverlayTabs")) {
            for (int t = 0; t < 5; ++t) {
                ImGuiTabItemFlags tflags =
                    (nav.tab_prev || nav.tab_next) && (st.active_tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kTabNames[t], nullptr, tflags)) {
                    st.active_tab = t;
                    float scroll_h = std::max(140.0f, ImGui::GetContentRegionAvail().y - 44.0f);
                    ImGui::BeginChild("OverlayTabBody", ImVec2(0.0f, scroll_h), false);
                    if (t == 0) {
                        ui_common::DrawUpscalingGraphicsPanel(st.probe, true, dirty, needs_display_apply);
                        ImGui::Spacing();
                        gpu::CacheStats cs = gpu::GetCacheStats();
                        ImGui::TextDisabled("AOT Shader Cache: %u compiled shaders | %u cached PSOs (boot AOT: %.1f ms)",
                                            cs.shaders, cs.psos, cs.aot_ms);
                    } else if (t == 1) {
                        ui_common::DrawDisplayGpuPanel(st.gpus, st.monitors, true, dirty, needs_display_apply);
                    } else if (t == 2) {
                        ui_common::DrawAudioPanel(dirty);
                    } else if (t == 3) {
                        mkb::DrawControlsSettingsPanel(true, &dirty);
                    } else if (t == 4) {
                        ui_common::DrawToolsDiagnosticsPanel(st.probe, true, st.diag_status);
                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
                        ImGui::TextUnformatted("Ahead-of-Time (AOT) Shader & Pipeline Pre-Compilation");
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        gpu::CacheStats cs = gpu::GetCacheStats();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("Compiled Shaders: %u  |  Cached PSOs: %u  |  Boot AOT: %.1f ms  |  Disk: %.2f MB",
                                    cs.shaders, cs.psos, cs.aot_ms, double(cs.disk_bytes) / (1024.0 * 1024.0));
                        ImGui::SameLine();
                        if (cs.rebuild_pending) {
                            ui_common::DrawBadge("[Rebuild Scheduled]", ImVec4(0.55f, 0.36f, 0.08f, 1.0f));
                        } else if (cs.aot_ready) {
                            ui_common::DrawBadge("[AOT Ready]", ImVec4(0.11f, 0.45f, 0.28f, 1.0f));
                        } else {
                            ui_common::DrawBadge("[Standby]", ImVec4(0.24f, 0.28f, 0.34f, 1.0f));
                        }
                        if (ImGui::Button("Rebuild / Pre-Compile Shader Cache on Next Launch", ImVec2(390.0f, 32.0f))) {
                            gpu::ScheduleCacheRebuildOnNextLaunch();
                            st.diag_status =
                                "Shader & PSO cache reset scheduled. Full AOT pre-compilation will run on next launch.";
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        // A rebind capture only lives while the Controls tab is the one on screen.
        if (st.active_tab != 3) mkb::CancelRebind();

        ImGui::Separator();
        if (ImGui::Button("Resume Game (F1)", ImVec2(180.0f, 32.0f))) {
            keep_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit to Desktop", ImVec2(160.0f, 32.0f))) {
            app::RequestExit();
        }
    }
    ImGui::End();

    if (dirty) {
        app::Save();
    }
    if (needs_display_apply) {
        app::ApplyDisplay();
    }
    if (!keep_open) {
        mkb::SetOverlayOpen(false);
    }
}

}  // namespace settings_ui
