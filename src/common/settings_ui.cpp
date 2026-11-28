// SPDX-License-Identifier: GPL-2.0-or-later
// Live-Synced In-Game F1 / F2 / BACK+L3 Settings & Cheats Overlay (6 tabs) and Upgraded Live HUD
// (FPS | ms | Upscaler | Render -> Display + Active Cheats Pill).

#include "common/settings_ui.h"
#include "../game/cheats.h"
#include "../game/guest_settings.h"
#include "common/input_recorder.h"
#include "common/mkb_input.h"
#include "common/ui_common.h"
#include "common/update_checker.h"
#include "common/version.h"
#include "upscale/upscaler_common.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
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
#else
#include <SDL3/SDL.h>
#endif

namespace settings_ui {

namespace {

constexpr int kSettingsTabCount = 6;
constexpr int kTabControls = 3;
constexpr int kTabDiagnostics = 4;
constexpr int kTabCheats = 5;

struct OverlayState {
    bool initialized = false;
    bool theme_applied = false;
    bool show_hud = false;
    int active_tab = 0;
    bool tab_select_pending = false;
    bool chord_prev = false;
    bool f2_prev_down = false;
    bool was_open = false;
    upscale::PluginProbe probe{};
    std::vector<app::GpuAdapterInfo> gpus;
    std::vector<app::MonitorInfo> monitors;
    std::string diag_status;
    bool upd_manual_pending = false;
    psiops::UpdateState prev_upd_state = psiops::UpdateState::Idle;
};

std::atomic<bool> g_show_hud{false};
std::atomic<bool> g_suppress_warmup_toast{false};
std::atomic<bool> g_deferred_guest_save{false};
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
           arg.rfind("--state-log", 0) == 0 || arg == "--open-settings" || arg == "--open-cheats" ||
           arg.rfind("--settings-tab", 0) == 0;
}

bool CheckSuppressWarmupToastCliOrEnv() {
#ifdef _WIN32
    static constexpr const wchar_t* kEnvVars[] = {
        L"XBE_OPEN_SETTINGS",    L"XBE_OPEN_CHEATS",      L"XBE_SETTINGS_TAB", L"XBE_CAPTURE_UI_BMP",
        L"XBE_CAPTURE_DIR",      L"XBE_AUTOTEST_OUT_DIR", L"XBE_AUTOTEST_SCRIPT", L"XBE_INPUT_RECORD",
        L"XBE_INPUT_REPLAY",     L"XBE_STATE_LOG",
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
        "XBE_OPEN_SETTINGS",    "XBE_OPEN_CHEATS",      "XBE_SETTINGS_TAB", "XBE_CAPTURE_UI_BMP",
        "XBE_CAPTURE_DIR",      "XBE_AUTOTEST_OUT_DIR", "XBE_AUTOTEST_SCRIPT", "XBE_INPUT_RECORD",
        "XBE_INPUT_REPLAY",     "XBE_STATE_LOG",
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

bool HasMockUpdateEnv() {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(L"PSIOPS_UPDATE_MOCK_JSON");
    return v && *v;
#else
    const char* v = std::getenv("PSIOPS_UPDATE_MOCK_JSON");
    return v && *v;
#endif
}

std::mutex g_update_toast_lock;
std::string g_update_toast_tag;
uint64_t g_update_toast_start_ms = 0;
bool g_update_toast_dismissed = false;

bool ShouldShowUpdateAvailableToast(psiops::UpdateStatusSnapshot* out_snap, float* out_alpha) {
    if (g_suppress_warmup_toast.load(std::memory_order_relaxed) || app::IsReplayOrAutotestActive() ||
        input::AutotestActive() || mkb::IsAutotestActive() || input_rec::IsReplayActive() ||
        input_rec::IsRecordActive()) {
        return false;
    }

    psiops::UpdateStatusSnapshot snap = psiops::GetUpdateSnapshot();
    if (snap.state != psiops::UpdateState::UpdateAvailable || snap.manual_check || snap.latest_tag.empty()) {
        return false;
    }
    const std::string& dismissed_tag = ui_common::GetUpdatePrefs().dismissed_update_tag;
    if (!dismissed_tag.empty() && snap.latest_tag == dismissed_tag) return false;

    const bool overlay_open = mkb::IsOverlayOpen();
    const uint64_t now_ms = SteadyNowMs();
    uint64_t start_ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_update_toast_lock);
        if (g_update_toast_tag != snap.latest_tag) {
            g_update_toast_tag = snap.latest_tag;
            g_update_toast_start_ms = now_ms;
            g_update_toast_dismissed = false;
        }
        if (overlay_open) {
            g_update_toast_dismissed = true;
            return false;
        }
        if (g_update_toast_dismissed) return false;
        start_ms = g_update_toast_start_ms;
    }

    const uint64_t age_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0ull;
    constexpr uint64_t kShowDurationMs = 6000;
    constexpr uint64_t kFadeStartMs = 5100;
    if (age_ms >= kShowDurationMs) {
        std::lock_guard<std::mutex> lk(g_update_toast_lock);
        g_update_toast_dismissed = true;
        return false;
    }
    const float alpha =
        (age_ms <= kFadeStartMs)
            ? 1.0f
            : std::clamp(float(kShowDurationMs - age_ms) / float(kShowDurationMs - kFadeStartMs), 0.0f, 1.0f);
    if (alpha <= 0.02f) return false;

    if (out_snap) *out_snap = std::move(snap);
    if (out_alpha) *out_alpha = alpha;
    return true;
}

void DrawUpdateAvailableToast(const psiops::UpdateStatusSnapshot& snap, float alpha) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.145f, 0.118f, 0.072f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.94f, 0.70f, 0.22f, 0.92f));
    constexpr ImGuiWindowFlags kToastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                             ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##PsiOpsUpdateAvailableToast", nullptr, kToastFlags)) {
        ImGui::TextColored(ImVec4(1.00f, 0.84f, 0.36f, 1.0f),
                           "Update %s available - press F1 -> Diagnostics", snap.latest_tag.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void PollInGameUpdateTransitions(OverlayState& st, const psiops::UpdateStatusSnapshot& snap) {
    if (st.upd_manual_pending && snap.state != psiops::UpdateState::Checking &&
        snap.state != psiops::UpdateState::Idle) {
        st.upd_manual_pending = false;
        if (snap.state == psiops::UpdateState::UpToDate) {
            st.diag_status = std::string("Up to date (") + PSIOPS_VERSION_TAG + ")";
        } else if (snap.state == psiops::UpdateState::UpdateAvailable) {
            std::string tag = !snap.latest_tag.empty() ? snap.latest_tag : ("v" + snap.latest_version);
            st.diag_status = "Update available: " + tag;
        } else if (snap.state == psiops::UpdateState::Error) {
            st.diag_status = snap.status_message.empty()
                                 ? "Update check failed (network error)."
                                 : ui_common::SanitizeUiText(snap.status_message, true, 140);
        }
    } else if (st.prev_upd_state == psiops::UpdateState::Downloading &&
               snap.state != psiops::UpdateState::Downloading) {
        if (snap.state == psiops::UpdateState::ReadyToRestart) {
            st.diag_status =
                "Update " + snap.latest_tag +
                " staged! Save your game progress first, then click 'Save & Exit to Update'.";
        } else if (snap.state == psiops::UpdateState::Error) {
            st.diag_status = snap.status_message.empty()
                                 ? "Self-update download/staging failed."
                                 : ui_common::SanitizeUiText(snap.status_message, true, 140);
        }
    }
    st.prev_upd_state = snap.state;
}

int ParseSettingsTabToken(std::string_view raw) {
    if (raw.empty()) return -1;
    std::string low;
    low.reserve(raw.size());
    bool all_digits = true;
    for (char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isdigit(uc)) all_digits = false;
        low.push_back(static_cast<char>(std::tolower(uc)));
    }
    if (all_digits) {
        return std::clamp(std::atoi(low.c_str()), 0, kSettingsTabCount - 1);
    }
    if (low == "upscaling" || low == "graphics") return 0;
    if (low == "display" || low == "gpu") return 1;
    if (low == "audio") return 2;
    if (low == "controls" || low == "input") return kTabControls;
    if (low == "diagnostics" || low == "tools") return kTabDiagnostics;
    if (low == "cheats" || low == "cheat" || low == "trainer") return kTabCheats;
    return -1;
}

void ApplyStartupOverlayCliAndEnv(OverlayState& st) {
    bool open_overlay = false;
    int req_tab = -1;
#ifdef _WIN32
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_SETTINGS"); v && *v && std::wcscmp(v, L"0") != 0) {
        open_overlay = true;
    }
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_CHEATS"); v && *v && std::wcscmp(v, L"0") != 0) {
        open_overlay = true;
        req_tab = kTabCheats;
    }
    if (const wchar_t* v = _wgetenv(L"XBE_SETTINGS_TAB"); v && *v) {
        int parsed = ParseSettingsTabToken(ui_common::WideToUtf8Str(v));
        if (parsed >= 0) req_tab = parsed;
    }
#else
    if (const char* v = std::getenv("XBE_OPEN_SETTINGS"); v && *v && std::strcmp(v, "0") != 0) {
        open_overlay = true;
    }
    if (const char* v = std::getenv("XBE_OPEN_CHEATS"); v && *v && std::strcmp(v, "0") != 0) {
        open_overlay = true;
        req_tab = kTabCheats;
    }
    if (const char* v = std::getenv("XBE_SETTINGS_TAB"); v && *v) {
        int parsed = ParseSettingsTabToken(v);
        if (parsed >= 0) req_tab = parsed;
    }
#endif
    const std::vector<std::string> args = ui_common::GetProcessArgsUtf8();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--open-settings") {
            open_overlay = true;
        } else if (a == "--open-cheats") {
            open_overlay = true;
            req_tab = kTabCheats;
        } else if (a == "--settings-tab" && i + 1 < args.size()) {
            int parsed = ParseSettingsTabToken(args[++i]);
            if (parsed >= 0) {
                req_tab = parsed;
                open_overlay = true;
            }
        } else if (a.rfind("--settings-tab=", 0) == 0) {
            int parsed = ParseSettingsTabToken(std::string_view(a).substr(15));
            if (parsed >= 0) {
                req_tab = parsed;
                open_overlay = true;
            }
        }
    }
    if (req_tab >= 0) {
        st.active_tab = req_tab;
        st.tab_select_pending = true;
    }
    if (open_overlay) {
        mkb::SetOverlayOpen(true);
    }
}

void ToggleOrSwitchCheatsOverlay(OverlayState& st) {
    const bool open_now = mkb::IsOverlayOpen();
    if (!open_now) {
        mkb::CancelRebind();
        st.active_tab = kTabCheats;
        st.tab_select_pending = true;
        mkb::SetOverlayOpen(true);
    } else if (st.active_tab == kTabCheats) {
        mkb::CancelRebind();
        mkb::SetOverlayOpen(false);
    } else {
        mkb::CancelRebind();
        st.active_tab = kTabCheats;
        st.tab_select_pending = true;
    }
}

void PollF2QuickCheatsHotkey(OverlayState& st) {
    if (app::IsReplayOrAutotestActive() || input::AutotestActive() || mkb::IsAutotestActive() ||
        input_rec::IsReplayActive() || input_rec::IsRecordActive()) {
        st.f2_prev_down = false;
        return;
    }
    bool down = false;
#ifdef _WIN32
    if (HWND fg = GetForegroundWindow()) {
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(fg, &fg_pid);
        if (fg_pid == GetCurrentProcessId() && !IsIconic(fg)) {
            down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        }
    }
#else
    if (SDL_GetKeyboardFocus() != nullptr) {
        int num_keys = 0;
        const bool* ks = SDL_GetKeyboardState(&num_keys);
        if (ks && int(SDL_SCANCODE_F2) < num_keys) {
            down = ks[SDL_SCANCODE_F2];
        }
    }
#endif
    const bool rising = down && !st.f2_prev_down;
    st.f2_prev_down = down;
    if (rising && !mkb::IsRebinding()) {
        ToggleOrSwitchCheatsOverlay(st);
    }
}

bool ShouldShowActiveCheatsHud(uint32_t* out_count) {
    if (!ui_common::GetUpdatePrefs().show_cheats_hud) return false;
    if (g_suppress_warmup_toast.load(std::memory_order_relaxed) || app::IsReplayOrAutotestActive() ||
        input::AutotestActive() || mkb::IsAutotestActive() || input_rec::IsReplayActive() ||
        input_rec::IsRecordActive()) {
        return false;
    }
    const uint32_t active_cnt = psiops::cheats::GetActiveToggleCount();
    if (active_cnt == 0) return false;
    if (out_count) *out_count = active_cnt;
    return true;
}

void DrawActiveCheatsHud(uint32_t active_cnt) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(14.0f, io.DisplaySize.y - 14.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.118f, 0.100f, 0.072f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.94f, 0.68f, 0.22f, 0.88f));
    constexpr ImGuiWindowFlags kPillFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##PsiOpsActiveCheatsHud", nullptr, kPillFlags)) {
        ImGui::TextColored(ImVec4(1.00f, 0.86f, 0.38f, 1.0f), "CHEATS ACTIVE (%u)", active_cnt);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
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
        ImGui::Text("| %.1f ms | %s | %ux%u -> %ux%u | %s | F1 Settings | F2 Cheats", ms, up_name, rw, rh, dw, dh,
                    dev_str);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace

bool HasVisibleUi() {
    OverlayState& st = St();
    if (st.initialized) {
        PollF2QuickCheatsHotkey(st);
    }
    if (psiops::guest_settings::ConsumePendingHostSaveRequest() ||
        psiops::cheats::ConsumePendingHostSaveRequest()) {
        g_deferred_guest_save.store(true, std::memory_order_release);
    }
    return mkb::IsOverlayOpen() || g_show_hud.load(std::memory_order_relaxed) ||
           g_deferred_guest_save.load(std::memory_order_acquire) || ShouldShowActiveCheatsHud(nullptr) ||
           ShouldShowShaderWarmupToast(nullptr, nullptr) || ShouldShowUpdateAvailableToast(nullptr, nullptr);
}

void Init() {
    OverlayState& st = St();
    st.probe = upscale::ProbePlugins(false);
    st.gpus = app::EnumerateGpuAdapters();
    st.monitors = app::EnumerateMonitors();
    st.initialized = true;
    g_show_hud.store(st.show_hud, std::memory_order_relaxed);
    const bool suppress_auto = CheckSuppressWarmupToastCliOrEnv();
    g_suppress_warmup_toast.store(suppress_auto, std::memory_order_relaxed);
    gpu::SetUiVisibleCallback(HasVisibleUi);
    ApplyStartupOverlayCliAndEnv(st);
    const bool has_mock_upd = HasMockUpdateEnv();
    if ((!suppress_auto && !app::IsReplayOrAutotestActive()) || has_mock_upd) {
        if ((ui_common::GetUpdatePrefs().check_updates_on_startup && psiops::ShouldAutoCheckForUpdates()) ||
            has_mock_upd) {
            if (psiops::GetUpdateSnapshot().state == psiops::UpdateState::Idle) {
                psiops::StartUpdateCheck(false);
            }
        }
    }
    st.prev_upd_state = psiops::GetUpdateSnapshot().state;
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

    if (g_deferred_guest_save.exchange(false, std::memory_order_acq_rel) ||
        psiops::guest_settings::ConsumePendingHostSaveRequest() ||
        psiops::cheats::ConsumePendingHostSaveRequest()) {
        if (!app::IsReplayOrAutotestActive() && !input::AutotestActive() && !mkb::IsAutotestActive() &&
            !input_rec::IsReplayActive() && !input_rec::IsRecordActive()) {
            app::Save();
        }
    }

    if (!st.theme_applied) {
        ui_common::ApplyMindgateTheme(1.0f);
        st.theme_applied = true;
    }

    const bool open_now = mkb::IsOverlayOpen();
    if (st.show_hud || open_now) {
        DrawLiveHud(fps);
    }
    uint32_t active_cheats_cnt = 0;
    if (ShouldShowActiveCheatsHud(&active_cheats_cnt)) {
        DrawActiveCheatsHud(active_cheats_cnt);
    }
    gpu::CacheStats warmup_cs{};
    float warmup_alpha = 0.0f;
    if (ShouldShowShaderWarmupToast(&warmup_cs, &warmup_alpha)) {
        DrawShaderWarmupToast(warmup_cs, warmup_alpha);
    }
    psiops::UpdateStatusSnapshot upd_toast_snap{};
    float upd_toast_alpha = 0.0f;
    if (ShouldShowUpdateAvailableToast(&upd_toast_snap, &upd_toast_alpha)) {
        DrawUpdateAvailableToast(upd_toast_snap, upd_toast_alpha);
    }
    if (!open_now) {
        if (st.was_open) {
            // Overlay just closed (F1 / F2 / Esc / BACK+L3 / Resume): don't leave gamepad keys latched in ImGui,
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
    if (nav.tab_prev) {
        st.active_tab = (st.active_tab + kSettingsTabCount - 1) % kSettingsTabCount;
        st.tab_select_pending = true;
    }
    if (nav.tab_next) {
        st.active_tab = (st.active_tab + 1) % kSettingsTabCount;
        st.tab_select_pending = true;
    }

    ImVec2 ds = io.DisplaySize;
    float w = std::min(ds.x * 0.86f, 880.0f);
    float h = std::min(ds.y * 0.82f, 640.0f);
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);

    bool keep_open = true;
    bool dirty = false;
    bool needs_display_apply = false;
    psiops::UpdateStatusSnapshot upd_snap = psiops::GetUpdateSnapshot();
    PollInGameUpdateTransitions(st, upd_snap);

    if (ImGui::Begin("Psi-Ops: The Mindgate Conspiracy v" PSIOPS_VERSION_STR " - Live Settings (F1 / F2 / BACK+L3)",
                     &keep_open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::Checkbox("Show Performance & Upscaler HUD", &st.show_hud)) {
            g_show_hud.store(st.show_hud, std::memory_order_relaxed);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  |  F2 Quick Cheats  |  LB / RB switch tabs  |  Changes save & apply live");
        ImGui::Separator();

        static constexpr const char* kTabNames[kSettingsTabCount] = {
            "Upscaling & Graphics", "Display & GPU", "Audio", "Controls", "Diagnostics", "Cheats",
        };
        if (ImGui::BeginTabBar("LiveOverlayTabs")) {
            for (int t = 0; t < kSettingsTabCount; ++t) {
                ImGuiTabItemFlags tflags =
                    (st.tab_select_pending && st.active_tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kTabNames[t], nullptr, tflags)) {
                    if (!st.tab_select_pending) {
                        st.active_tab = t;
                    }
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
                    } else if (t == kTabControls) {
                        // auto_save=false: the overlay's own `if (dirty) app::Save()` below is the single save
                        // for this frame; saving inside the panel as well would write settings.toml twice.
                        mkb::DrawControlsSettingsPanel(false, &dirty);
                        ImGui::Spacing();
                        ui_common::DrawInGameControlsOptionsPanel(dirty);
                    } else if (t == kTabDiagnostics) {
                        ui_common::DrawUpdatesDiagnosticsSection(
                            upd_snap, false, 1.0f, dirty, st.upd_manual_pending, st.diag_status, [&] {
                                app::Save();
                                dirty = false;
                                if (psiops::ApplyStagedUpdateAndRelaunch()) {
                                    app::RequestExit();
                                } else {
                                    psiops::UpdateStatusSnapshot err_s = psiops::GetUpdateSnapshot();
                                    st.diag_status =
                                        err_s.status_message.empty()
                                            ? "Failed to launch staged update helper."
                                            : ui_common::SanitizeUiText(err_s.status_message, true, 140);
                                }
                            });
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
                    } else if (t == kTabCheats) {
                        ui_common::DrawCheatsPanel(1.0f, dirty, &st.diag_status);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            st.tab_select_pending = false;
            ImGui::EndTabBar();
        }
        // A rebind capture only lives while the Controls tab is the one on screen.
        if (st.active_tab != kTabControls) mkb::CancelRebind();

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
