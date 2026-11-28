// SPDX-License-Identifier: GPL-2.0-or-later
// Host-boundary controller .rec recording, deterministic replay, and --state-log JSONL emitter.

#include "common/input_recorder.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace input_rec {

namespace {

constexpr char kMagic[8] = {'X', 'B', 'E', 'R', 'E', 'C', '0', '1'};
constexpr uint32_t kVersion = 1u;
constexpr uint64_t kMaxEffDeltaUs = 100000ull;  // clamp single-frame stalls to 100ms for replay stability

#pragma pack(push, 1)
struct RecHeader {
    char magic[8];
    uint32_t version;
    uint32_t flags;
    uint32_t title_id;
    uint32_t reserved;
    uint64_t total_frames;
};

struct RecSample {
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint64_t eff_us;
    uint8_t port;
    uint8_t connected;
    uint16_t buttons;
    uint8_t analog[8];
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
};
#pragma pack(pop)

static_assert(sizeof(RecHeader) == 32, "RecHeader must be 32 bytes");
static_assert(sizeof(RecSample) == 44, "RecSample must be 44 bytes");

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> ReadProcessArgs() {
    std::vector<std::string> out;
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return out;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) ++p;
        if (!*p) break;
        std::wstring w;
        bool q = false;
        while (*p && (q || (*p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n'))) {
            if (*p == L'"') q = !q;
            else w.push_back(*p);
            ++p;
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string s(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
            out.push_back(std::move(s));
        } else {
            out.emplace_back();
        }
    }
#else
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return out;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nul = raw.find('\0', pos);
        if (nul == std::string::npos) nul = raw.size();
        out.emplace_back(raw.data() + pos, nul - pos);
        pos = nul + 1;
    }
#endif
    return out;
}

std::string GetEnvUtf8Local(const char* name) {
    if (!name || !*name) return {};
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wn > 1) {
        std::wstring wname(size_t(wn - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wn - 1);
        if (const wchar_t* wv = _wgetenv(wname.c_str()); wv && *wv) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wv, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                std::string s(size_t(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wv, -1, s.data(), n - 1, nullptr, nullptr);
                return s;
            }
        }
    }
#endif
    if (const char* v = std::getenv(name); v && *v) return std::string(v);
    return {};
}

std::string GetArgValue(const std::vector<std::string>& args, const char* flag, const char* env_fallback) {
    size_t n = std::strlen(flag);
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == flag && i + 1 < args.size()) return args[i + 1];
        if (args[i].size() > n + 1 && args[i].compare(0, n, flag) == 0 && args[i][n] == '=') {
            return args[i].substr(n + 1);
        }
    }
    if (env_fallback) return GetEnvUtf8Local(env_fallback);
    return {};
}

bool HasArgFlag(const std::vector<std::string>& args, const char* flag, const char* env_fallback) {
    size_t n = std::strlen(flag);
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == flag) return true;
        if (args[i].size() > n + 1 && args[i].compare(0, n, flag) == 0 && args[i][n] == '=') {
            return args[i].substr(n + 1) != "0";
        }
    }
    if (env_fallback) {
        std::string v = GetEnvUtf8Local(env_fallback);
        if (!v.empty() && v != "0") return true;
    }
    return false;
}

struct RecorderState {
    std::atomic<bool> initialized{false};
    std::atomic<bool> record_active{false};
    std::atomic<bool> replay_active{false};
    std::atomic<bool> state_log_active{false};
    bool replay_exit = false;
    bool replay_exit_posted = false;
    ReplayMode replay_mode = ReplayMode::Hybrid;

    std::filesystem::path record_path;
    std::filesystem::path replay_path;
    std::filesystem::path state_log_path;

    std::atomic<GuestStateCallback> guest_cb{nullptr};
    std::atomic<MkbStateCallback> mkb_cb{nullptr};

    std::mutex lock;
    bool clock_started = false;
    std::chrono::steady_clock::time_point start_tp{};
    std::chrono::steady_clock::time_point last_tp{};
    std::atomic<uint64_t> cur_frame{0};
    std::atomic<uint64_t> cur_ts_us{0};
    std::atomic<uint64_t> cur_eff_us{0};

    // Latest observed Port 0 pad state (for --state-log pad_* fields)
    input::Pad last_logged_pad0 = {};
    // Pending Port 0..3 pad observation merged across polls within a frame (for recording)
    input::Pad pending_pad[4] = {};
    bool pending_pad_seen[4] = {};
    input::Pad last_emitted_pad[4] = {};
    std::vector<RecSample> recorded_samples;

    // Replay timeline (Port 0 primary, plus multi-port lookup)
    std::vector<RecSample> replay_samples;
    uint64_t replay_total_frames = 0;
    uint64_t replay_total_eff_us = 0;
    size_t replay_cursor = 0;
    input::Pad active_replay_pad[4] = {};
    // Edge-latch state for Hybrid mode so slow frames never miss brief button taps
    uint16_t edge_buttons = 0;
    uint8_t edge_analog[8] = {};
    int edge_hold_frames = 0;

    // Rumble state
    std::atomic<uint16_t> rumble_l{0};
    std::atomic<uint16_t> rumble_r{0};
    std::atomic<uint16_t> frame_rumble_peak_l{0};
    std::atomic<uint16_t> frame_rumble_peak_r{0};

    // Buffered state-log output
    FILE* state_fp = nullptr;
    bool flushed = false;
};

RecorderState& St() {
    static RecorderState s;
    return s;
}

input::Pad SampleToPad(const RecSample& s) {
    input::Pad p{};
    p.connected = (s.connected != 0);
    p.buttons = s.buttons;
    std::memcpy(p.analog, s.analog, 8);
    p.lx = s.lx;
    p.ly = s.ly;
    p.rx = s.rx;
    p.ry = s.ry;
    return p;
}

RecSample PadToSample(uint64_t frame_id, uint64_t ts_us, uint64_t eff_us, uint8_t port, const input::Pad& p) {
    RecSample s{};
    s.frame_id = frame_id;
    s.timestamp_us = ts_us;
    s.eff_us = eff_us;
    s.port = port;
    s.connected = p.connected ? 1u : 0u;
    s.buttons = p.buttons;
    std::memcpy(s.analog, p.analog, 8);
    s.lx = p.lx;
    s.ly = p.ly;
    s.rx = p.rx;
    s.ry = p.ry;
    return s;
}

bool LoadReplayFileLocked(RecorderState& st) {
    std::ifstream f(st.replay_path, std::ios::binary);
    if (!f) {
        LOGE("input_rec: failed to open replay file %s", app::PathToUtf8(st.replay_path).c_str());
        return false;
    }
    RecHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || std::memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0 || hdr.version != kVersion) {
        LOGE("input_rec: invalid replay header in %s", app::PathToUtf8(st.replay_path).c_str());
        return false;
    }
    st.replay_samples.clear();
    RecSample s{};
    while (f.read(reinterpret_cast<char*>(&s), sizeof(s))) {
        st.replay_samples.push_back(s);
    }
    st.replay_total_frames = hdr.total_frames;
    if (!st.replay_samples.empty()) {
        if (st.replay_total_frames == 0) st.replay_total_frames = st.replay_samples.back().frame_id + 1;
        st.replay_total_eff_us = st.replay_samples.back().eff_us;
    }
    st.replay_cursor = 0;
    st.active_replay_pad[0].connected = true;
    LOGR("input_rec: loaded replay %s (samples=%zu frames=%llu eff_ms=%.1f mode=%s exit=%d)",
         app::PathToUtf8(st.replay_path).c_str(), st.replay_samples.size(), (unsigned long long)st.replay_total_frames,
         double(st.replay_total_eff_us) / 1000.0,
         st.replay_mode == ReplayMode::Frame ? "frame" : (st.replay_mode == ReplayMode::Time ? "time" : "hybrid"),
         st.replay_exit ? 1 : 0);
    return !st.replay_samples.empty();
}

void AdvanceReplayLocked(RecorderState& st, uint64_t frame_id, uint64_t eff_us) {
    if (st.replay_samples.empty()) return;

    auto apply_target = [&](size_t next_idx) {
        if (next_idx >= st.replay_samples.size()) next_idx = st.replay_samples.size() - 1;
        // In Hybrid mode, preserve any button press edge that occurred in intermediate skipped samples
        uint16_t skipped_btns = 0;
        uint8_t skipped_analog[8] = {};
        if (st.replay_mode == ReplayMode::Hybrid && next_idx > st.replay_cursor + 1) {
            for (size_t i = st.replay_cursor + 1; i < next_idx; ++i) {
                const RecSample& mid = st.replay_samples[i];
                if (mid.port != 0) continue;
                skipped_btns |= mid.buttons;
                for (int b = 0; b < 8; ++b) skipped_analog[b] = std::max(skipped_analog[b], mid.analog[b]);
            }
        }

        st.replay_cursor = next_idx;
        const RecSample& cur = st.replay_samples[next_idx];
        if (cur.port < 4) {
            input::Pad p = SampleToPad(cur);
            if (cur.port == 0) p.connected = true;
            if (st.replay_mode == ReplayMode::Hybrid) {
                bool has_skipped = (skipped_btns != 0);
                for (int b = 0; b < 8; ++b)
                    if (skipped_analog[b] > 30u) has_skipped = true;
                if (has_skipped) {
                    st.edge_buttons |= skipped_btns;
                    for (int b = 0; b < 8; ++b) st.edge_analog[b] = std::max(st.edge_analog[b], skipped_analog[b]);
                    st.edge_hold_frames = std::max(st.edge_hold_frames, 2);
                }
                if (st.edge_hold_frames > 0) {
                    p.buttons |= st.edge_buttons;
                    for (int b = 0; b < 8; ++b) p.analog[b] = std::max(p.analog[b], st.edge_analog[b]);
                    --st.edge_hold_frames;
                    if (st.edge_hold_frames == 0) {
                        st.edge_buttons = 0;
                        std::memset(st.edge_analog, 0, sizeof(st.edge_analog));
                    }
                }
            }
            st.active_replay_pad[cur.port] = p;
        }
    };

    if (st.replay_mode == ReplayMode::Frame) {
        size_t idx = st.replay_cursor;
        while (idx + 1 < st.replay_samples.size() && st.replay_samples[idx + 1].frame_id <= frame_id) {
            ++idx;
        }
        apply_target(idx);
    } else {
        // Both Time and Hybrid advance on stall-clamped eff_us so 60 FPS Release, Debug, cold cache,
        // and 30 FPS Linux Vulkan stay locked to the recorded 1.00x real-time timeline.
        size_t idx = st.replay_cursor;
        while (idx + 1 < st.replay_samples.size() && st.replay_samples[idx + 1].eff_us <= eff_us) {
            ++idx;
        }
        apply_target(idx);
    }
}

void EmitStateLogLineLocked(RecorderState& st, uint64_t frame_id, uint64_t ts_us, uint64_t eff_us) {
    if (!st.state_fp) return;
    GuestStateSnapshot gs{};
    if (GuestStateCallback cb = st.guest_cb.load(std::memory_order_acquire)) {
        gs = cb();
    }
    MkbStateSnapshot ms{};
    if (MkbStateCallback mcb = st.mkb_cb.load(std::memory_order_acquire)) {
        ms = mcb();
    }
    const char* mode_s = st.replay_active.load(std::memory_order_relaxed)
                             ? "replay"
                             : (st.record_active.load(std::memory_order_relaxed) ? "record" : "live");
    if (st.replay_active.load(std::memory_order_relaxed)) ms.mkb_hook_bypassed = true;

    const input::Pad& p0 = st.replay_active.load(std::memory_order_relaxed) ? st.active_replay_pad[0] : st.last_logged_pad0;
    uint16_t rl = std::max(st.rumble_l.load(std::memory_order_relaxed),
                           st.frame_rumble_peak_l.exchange(0, std::memory_order_relaxed));
    uint16_t rr = std::max(st.rumble_r.load(std::memory_order_relaxed),
                           st.frame_rumble_peak_r.exchange(0, std::memory_order_relaxed));

    std::fprintf(st.state_fp,
                 "{\"frame_id\":%llu,\"timestamp_us\":%llu,\"eff_us\":%llu,\"mode\":\"%s\",\"phase\":\"%s\","
                 "\"state\":%u,\"in_shell\":%d,\"fmv_active\":%d,\"sim_elapsed_s\":%.4f,\"g_frame\":%u,"
                 "\"pad_buttons\":%u,\"pad_lx\":%d,\"pad_ly\":%d,\"pad_rx\":%d,\"pad_ry\":%d,"
                 "\"pad_lt\":%u,\"pad_rt\":%u,\"rumble_left\":%u,\"rumble_right\":%u,"
                 "\"input_device\":\"%s\",\"cursor_captured\":%s,\"os_cursor_visible\":%s,\"overlay_open\":%s,\"window_focused\":%s,"
                 "\"mouse_dx\":%d,\"mouse_dy\":%d,\"d_yaw\":%.6f,\"d_pitch\":%.6f,"
                 "\"wasd_x\":%.5f,\"wasd_y\":%.5f,\"wasd_mag\":%.5f,"
                 "\"mouse_invert_y\":%s,\"mouse_raw_aim\":%s,\"mouse_sens\":%.4f,\"cm_per_360\":%.4f,"
                 "\"wheel_pulse\":%d,\"esc_pulse\":%s,\"mkb_hook_bypassed\":%s",
                 (unsigned long long)frame_id, (unsigned long long)ts_us, (unsigned long long)eff_us, mode_s,
                 gs.phase ? gs.phase : "title_menu", gs.state, gs.in_shell, gs.fmv_active, gs.sim_elapsed_s,
                 gs.g_frame, unsigned(p0.buttons), int(p0.lx), int(p0.ly), int(p0.rx), int(p0.ry),
                 unsigned(p0.analog[6]), unsigned(p0.analog[7]), unsigned(rl), unsigned(rr),
                 ms.input_device ? ms.input_device : "mkb", ms.cursor_captured ? "true" : "false",
                 ms.os_cursor_visible ? "true" : "false", ms.overlay_open ? "true" : "false",
                 ms.window_focused ? "true" : "false", int(ms.mouse_dx), int(ms.mouse_dy), ms.d_yaw, ms.d_pitch,
                 ms.wasd_x, ms.wasd_y, ms.wasd_mag, ms.mouse_invert_y ? "true" : "false",
                 ms.mouse_raw_aim ? "true" : "false", ms.mouse_sens, ms.cm_per_360, int(ms.wheel_pulse),
                 ms.esc_pulse ? "true" : "false", ms.mkb_hook_bypassed ? "true" : "false");
    if (gs.has_player_pos && std::isfinite(gs.player_x) && std::isfinite(gs.player_y) && std::isfinite(gs.player_z)) {
        std::fprintf(st.state_fp, ",\"player_x\":%.4f,\"player_y\":%.4f,\"player_z\":%.4f", gs.player_x, gs.player_y,
                     gs.player_z);
    }
    if (gs.has_camera && std::isfinite(gs.cam_yaw) && std::isfinite(gs.cam_pitch)) {
        std::fprintf(st.state_fp, ",\"cam_yaw\":%.6f,\"cam_pitch\":%.6f,\"cam_mode\":%u", gs.cam_yaw, gs.cam_pitch,
                     gs.cam_mode);
    }
    if (gs.has_vitals) {
        if (std::isfinite(gs.health)) std::fprintf(st.state_fp, ",\"health\":%.3f", gs.health);
        if (std::isfinite(gs.psi)) std::fprintf(st.state_fp, ",\"psi\":%.3f", gs.psi);
        if (gs.ammo_clip >= 0) std::fprintf(st.state_fp, ",\"ammo_clip\":%d", int(gs.ammo_clip));
        if (gs.ammo_reserve >= 0) std::fprintf(st.state_fp, ",\"ammo_reserve\":%d", int(gs.ammo_reserve));
        if (gs.ammo_clip_size >= 0) std::fprintf(st.state_fp, ",\"ammo_clip_size\":%d", int(gs.ammo_clip_size));
        if (gs.weapon_type >= 0) std::fprintf(st.state_fp, ",\"weapon_type\":%d", int(gs.weapon_type));
    }
    if (gs.has_player_actions) {
        std::fprintf(st.state_fp,
                     ",\"anim_state\":%u,\"psi_fn\":%u,\"psi_selected\":%d,\"lock_on_active\":%s,"
                     "\"sniper_scoped\":%s,\"mkb_actions_fired\":%llu,\"mkb_reload_count\":%u",
                     unsigned(gs.anim_state), unsigned(gs.psi_fn), int(gs.psi_selected),
                     gs.lock_on_active ? "true" : "false", gs.sniper_scoped ? "true" : "false",
                     (unsigned long long)gs.mkb_actions_fired, unsigned(gs.mkb_reload_count));
        if (std::isfinite(gs.melee_timer) && gs.melee_timer >= 0.0f) {
            std::fprintf(st.state_fp, ",\"melee_timer\":%.4f", gs.melee_timer);
        }
    }
    if (gs.has_guest_invert_y) {
        std::fprintf(st.state_fp, ",\"guest_invert_y\":%s", gs.guest_invert_y ? "true" : "false");
    }
    if (!ms.ui_screen.empty()) {
        std::fprintf(st.state_fp, ",\"ui_screen\":\"%s\",\"ui_hovered_idx\":%d,\"ui_item_count\":%d",
                     ms.ui_screen.c_str(), ms.ui_hovered_idx, ms.ui_item_count);
    }
    std::fprintf(st.state_fp, "}\n");
}

void FlushLocked(RecorderState& st) {
    if (st.flushed) return;
    st.flushed = true;

    if (st.record_active.load(std::memory_order_relaxed) && !st.record_path.empty()) {
        std::error_code ec;
        if (st.record_path.has_parent_path()) std::filesystem::create_directories(st.record_path.parent_path(), ec);
        std::ofstream f(st.record_path, std::ios::binary | std::ios::trunc);
        if (f) {
            RecHeader hdr{};
            std::memcpy(hdr.magic, kMagic, sizeof(kMagic));
            hdr.version = kVersion;
            hdr.flags = 0u;
            hdr.title_id = app::kGame.expected_title_id;
            hdr.reserved = 0u;
            hdr.total_frames = st.cur_frame.load(std::memory_order_relaxed) + 1;
            f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            if (!st.recorded_samples.empty()) {
                f.write(reinterpret_cast<const char*>(st.recorded_samples.data()),
                        std::streamsize(st.recorded_samples.size() * sizeof(RecSample)));
            }
            f.flush();
            LOGR("input_rec: wrote %s (%zu samples, %llu frames)", app::PathToUtf8(st.record_path).c_str(),
                 st.recorded_samples.size(), (unsigned long long)hdr.total_frames);
        } else {
            LOGE("input_rec: failed to write %s", app::PathToUtf8(st.record_path).c_str());
        }
    }

    if (st.state_fp) {
        std::fflush(st.state_fp);
        std::fclose(st.state_fp);
        st.state_fp = nullptr;
        LOGR("input_rec: closed state log %s", app::PathToUtf8(st.state_log_path).c_str());
    }
}

}  // namespace

void Init() {
    RecorderState& st = St();
    if (st.initialized.exchange(true, std::memory_order_acq_rel)) return;

    auto args = ReadProcessArgs();
    std::string rec_s = GetArgValue(args, "--record-input", "XBE_INPUT_RECORD");
    std::string rep_s = GetArgValue(args, "--replay-input", "XBE_INPUT_REPLAY");
    std::string mode_s = LowerAscii(GetArgValue(args, "--replay-mode", "XBE_REPLAY_MODE"));
    std::string log_s = GetArgValue(args, "--state-log", "XBE_STATE_LOG");
    bool rep_exit = HasArgFlag(args, "--replay-exit", "XBE_REPLAY_EXIT");

    std::lock_guard<std::mutex> lk(st.lock);
    if (mode_s == "frame") st.replay_mode = ReplayMode::Frame;
    else if (mode_s == "time") st.replay_mode = ReplayMode::Time;
    else st.replay_mode = ReplayMode::Hybrid;
    st.replay_exit = rep_exit;

    if (!rep_s.empty()) {
        st.replay_path = app::Utf8ToPath(rep_s);
        if (LoadReplayFileLocked(st)) {
            st.replay_active.store(true, std::memory_order_release);
        }
    } else if (!rec_s.empty()) {
        st.record_path = app::Utf8ToPath(rec_s);
        st.recorded_samples.reserve(8192);
        st.record_active.store(true, std::memory_order_release);
        LOGR("input_rec: recording enabled -> %s", app::PathToUtf8(st.record_path).c_str());
    }

    if (!log_s.empty()) {
        st.state_log_path = app::Utf8ToPath(log_s);
        std::error_code ec;
        if (st.state_log_path.has_parent_path())
            std::filesystem::create_directories(st.state_log_path.parent_path(), ec);
#ifdef _WIN32
        st.state_fp = _wfopen(st.state_log_path.c_str(), L"wb");
#else
        st.state_fp = std::fopen(st.state_log_path.c_str(), "wb");
#endif
        if (st.state_fp) {
            st.state_log_active.store(true, std::memory_order_release);
            LOGR("input_rec: state log enabled -> %s", app::PathToUtf8(st.state_log_path).c_str());
        } else {
            LOGE("input_rec: failed to open state log %s", app::PathToUtf8(st.state_log_path).c_str());
        }
    }
}

void Shutdown() {
    RecorderState& st = St();
    if (!st.initialized.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(st.lock);
    FlushLocked(st);
}

void SetGuestStateCallback(GuestStateCallback cb) { St().guest_cb.store(cb, std::memory_order_release); }
void SetMkbStateCallback(MkbStateCallback cb) { St().mkb_cb.store(cb, std::memory_order_release); }

bool IsRecordActive() { return St().record_active.load(std::memory_order_relaxed); }
bool IsReplayActive() { return St().replay_active.load(std::memory_order_relaxed); }
bool IsStateLogActive() { return St().state_log_active.load(std::memory_order_relaxed); }
bool ShouldBypassMkb() { return IsReplayActive() || input::AutotestActive(); }
ReplayMode GetReplayMode() { return St().replay_mode; }

bool OnPadPoll(int port, input::Pad& pad) {
    RecorderState& st = St();
    if (st.replay_active.load(std::memory_order_acquire)) {
        if (port < 0 || port >= 4) return true;
        std::lock_guard<std::mutex> lk(st.lock);
        pad = st.active_replay_pad[port];
        if (port == 0) pad.connected = true;
        return true;
    }
    if (st.state_log_active.load(std::memory_order_relaxed) && port == 0) {
        std::lock_guard<std::mutex> lk(st.lock);
        st.last_logged_pad0 = pad;
    }
    if (st.record_active.load(std::memory_order_acquire) && port >= 0 && port < 4) {
        std::lock_guard<std::mutex> lk(st.lock);
        if (!st.pending_pad_seen[port]) {
            st.pending_pad[port] = pad;
            st.pending_pad_seen[port] = true;
        } else {
            st.pending_pad[port].connected = st.pending_pad[port].connected || pad.connected;
            st.pending_pad[port].buttons |= pad.buttons;
            for (int i = 0; i < 8; ++i)
                st.pending_pad[port].analog[i] = std::max(st.pending_pad[port].analog[i], pad.analog[i]);
            st.pending_pad[port].lx = pad.lx;
            st.pending_pad[port].ly = pad.ly;
            st.pending_pad[port].rx = pad.rx;
            st.pending_pad[port].ry = pad.ry;
        }
    }
    return false;
}

void OnRumble(int port, uint16_t left, uint16_t right) {
    if (port != 0) return;
    RecorderState& st = St();
    st.rumble_l.store(left, std::memory_order_relaxed);
    st.rumble_r.store(right, std::memory_order_relaxed);
    uint16_t prev_l = st.frame_rumble_peak_l.load(std::memory_order_relaxed);
    while (left > prev_l && !st.frame_rumble_peak_l.compare_exchange_weak(prev_l, left, std::memory_order_relaxed)) {
    }
    uint16_t prev_r = st.frame_rumble_peak_r.load(std::memory_order_relaxed);
    while (right > prev_r && !st.frame_rumble_peak_r.compare_exchange_weak(prev_r, right, std::memory_order_relaxed)) {
    }
}

void OnPresent(uint64_t present_frame, std::filesystem::path& /*out_capture_bmp*/, bool& out_should_exit) {
    RecorderState& st = St();
    const bool rec = st.record_active.load(std::memory_order_acquire);
    const bool rep = st.replay_active.load(std::memory_order_acquire);
    const bool slog = st.state_log_active.load(std::memory_order_acquire);

    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(st.lock);
    if (!st.clock_started) {
        st.clock_started = true;
        st.start_tp = now;
        st.last_tp = now;
        st.cur_ts_us.store(0, std::memory_order_relaxed);
        st.cur_eff_us.store(0, std::memory_order_relaxed);
    } else {
        uint64_t ts_us =
            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - st.start_tp).count());
        uint64_t raw_dt =
            uint64_t(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(now - st.last_tp).count()));
        st.last_tp = now;
        st.cur_ts_us.store(ts_us, std::memory_order_relaxed);
        uint64_t eff = st.cur_eff_us.load(std::memory_order_relaxed) + std::min(raw_dt, kMaxEffDeltaUs);
        st.cur_eff_us.store(eff, std::memory_order_relaxed);
    }
    st.cur_frame.store(present_frame, std::memory_order_relaxed);

    if (!rec && !rep && !slog) return;

    uint64_t ts_us = st.cur_ts_us.load(std::memory_order_relaxed);
    uint64_t eff_us = st.cur_eff_us.load(std::memory_order_relaxed);

    if (rec) {
        for (int p = 0; p < 4; ++p) {
            if (p != 0 && !st.pending_pad_seen[p] && !st.last_emitted_pad[p].connected) continue;
            input::Pad cur = st.pending_pad_seen[p] ? st.pending_pad[p] : st.last_emitted_pad[p];
            if (p == 0 && !st.pending_pad_seen[0] && !cur.connected) cur.connected = true;
            st.recorded_samples.push_back(PadToSample(present_frame, ts_us, eff_us, uint8_t(p), cur));
            st.last_emitted_pad[p] = cur;
            st.pending_pad_seen[p] = false;
            st.pending_pad[p] = input::Pad{};
        }
    }

    if (rep) {
        AdvanceReplayLocked(st, present_frame, eff_us);
    }

    if (slog) {
        EmitStateLogLineLocked(st, present_frame, ts_us, eff_us);
    }

    if (rep && st.replay_exit && !st.replay_exit_posted) {
        bool done = false;
        if (st.replay_mode == ReplayMode::Frame) {
            done = (st.replay_total_frames > 0 && present_frame >= st.replay_total_frames);
        } else {
            done = (st.replay_total_eff_us > 0 && eff_us >= st.replay_total_eff_us &&
                    st.replay_cursor + 1 >= st.replay_samples.size());
        }
        if (done) {
            st.replay_exit_posted = true;
            out_should_exit = true;
            LOGR("input_rec: replay complete at frame=%llu eff_ms=%.1f -> requesting clean exit",
                 (unsigned long long)present_frame, double(eff_us) / 1000.0);
        }
    }
}

uint64_t CurrentPresentFrame() { return St().cur_frame.load(std::memory_order_relaxed); }
uint64_t ElapsedWallUs() { return St().cur_ts_us.load(std::memory_order_relaxed); }
uint64_t ElapsedEffUs() { return St().cur_eff_us.load(std::memory_order_relaxed); }

}  // namespace input_rec
