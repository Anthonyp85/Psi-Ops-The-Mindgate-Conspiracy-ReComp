// SPDX-License-Identifier: GPL-2.0-or-later
// In-App Gamepad/Touch/Mouse XISO File Browser with Steam Deck microSD Quick-Jump
// and live app::ValidateDiscImage() status badges.

#include "common/file_browser_ui.h"
#include "common/ui_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace file_browser_ui {

namespace {

namespace fs = std::filesystem;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsIsoLikeName(const std::string& name) {
    std::string l = LowerAscii(name);
    if (l.size() >= 4 && l.compare(l.size() - 4, 4, ".iso") == 0) return true;
    if (l.size() >= 5 && l.compare(l.size() - 5, 5, ".xiso") == 0) return true;
    return false;
}

std::string FormatBytes(uint64_t b) {
    char buf[48];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(b) / double(1ull << 20));
    else if (b >= (1ull << 10)) std::snprintf(buf, sizeof(buf), "%.1f KB", double(b) / double(1ull << 10));
    else std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    return buf;
}

struct QuickJump {
    std::string label;
    fs::path dir;
};

std::vector<QuickJump> BuildQuickJumps() {
    std::vector<QuickJump> qj;
    std::error_code ec;
    auto add_unique = [&](const std::string& label, const fs::path& p, bool require_dir) {
        if (p.empty()) return;
        if (require_dir && !fs::is_directory(p, ec)) return;
        for (const auto& existing : qj) {
            if (existing.dir == p) return;
        }
        qj.push_back({label, p});
    };

    add_unique("Game Folder", app::ExeDir(), true);

#ifdef _WIN32
    auto known_dir = [](REFKNOWNFOLDERID fid) -> fs::path {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(fid, 0, nullptr, &raw)) && raw) {
            fs::path p(raw);
            CoTaskMemFree(raw);
            return p;
        }
        return {};
    };
    fs::path home = known_dir(FOLDERID_Profile);
    if (home.empty()) {
        if (const wchar_t* up = _wgetenv(L"USERPROFILE"); up && *up) {
            home = fs::path(up);
        } else if (const wchar_t* hd = _wgetenv(L"HOMEDRIVE"), *hp = _wgetenv(L"HOMEPATH"); hd && hp && *hd && *hp) {
            home = fs::path(std::wstring(hd) + std::wstring(hp));
        } else if (const wchar_t* hm = _wgetenv(L"HOME"); hm && *hm) {
            home = fs::path(hm);
        }
    }
    fs::path downloads = known_dir(FOLDERID_Downloads);
    if (downloads.empty() && !home.empty()) downloads = home / "Downloads";
    fs::path desktop = known_dir(FOLDERID_Desktop);
    if (desktop.empty() && !home.empty()) desktop = home / "Desktop";

    add_unique("Home", home, false);
    add_unique("Downloads", downloads, false);
    add_unique("Desktop", desktop, false);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i)) {
            char root[4] = {char('A' + i), ':', '\\', '\0'};
            UINT dt = GetDriveTypeA(root);
            if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE || dt == DRIVE_REMOTE) {
                std::string lbl = std::string("Drive ") + char('A' + i) + ":";
                add_unique(lbl, fs::path(root), true);
            }
        }
    }
#else
    fs::path home;
    if (const char* hm = std::getenv("HOME"); hm && *hm) {
        home = fs::path(hm);
    } else if (fs::is_directory("/home/deck", ec)) {
        home = fs::path("/home/deck");
    }
    if (!home.empty()) {
        add_unique("Home", home, false);
        add_unique("Downloads", home / "Downloads", false);
        add_unique("Desktop", home / "Desktop", false);
        add_unique("Games", home / "Games", true);
    }

    // Auto-detected Steam Deck microSD mounts (/run/media/mmcblk0p1 & /run/media/<user>/*)
    add_unique("Steam Deck microSD", fs::path("/run/media/mmcblk0p1"), true);
    auto scan_media_user = [&](const fs::path& base) {
        if (!fs::is_directory(base, ec)) return;
        for (auto& e : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_directory(ec)) {
                add_unique("microSD: " + e.path().filename().string(), e.path(), true);
            }
        }
    };
    if (const char* user = std::getenv("USER"); user && *user) {
        scan_media_user(fs::path("/run/media") / user);
    }
    scan_media_user(fs::path("/run/media/deck"));
    if (fs::is_directory("/run/media", ec)) {
        for (auto& e : fs::directory_iterator("/run/media", fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_directory(ec) && e.path().filename() != "mmcblk0p1") {
                scan_media_user(e.path());
            }
        }
    }

    add_unique("Filesystem /", fs::path("/"), true);
#endif
    return qj;
}

}  // namespace

void FileBrowserUi::Open(const fs::path& start_dir) {
    open_ = true;
    status_error_.clear();
    has_validation_ = false;
    inspected_path_.clear();
    selected_idx_ = -1;

    std::error_code ec;
    fs::path target = start_dir;
    if (target.empty() && !app::Get().iso_path.empty()) {
        fs::path p(app::Get().iso_path);
        if (fs::is_regular_file(p, ec)) target = p.parent_path();
        else if (fs::is_directory(p, ec)) target = p;
    }
    if (target.empty() || !fs::is_directory(target, ec)) {
        target = app::ExeDir();
    }
    NavigateTo(target);
}

void FileBrowserUi::Close() { open_ = false; }

void FileBrowserUi::NavigateTo(const fs::path& dir) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(dir, ec);
    if (ec || !fs::is_directory(norm, ec)) norm = dir;
    current_dir_ = norm;
    selected_idx_ = -1;
    has_validation_ = false;
    inspected_path_.clear();
    status_error_.clear();
    RefreshEntries();
}

void FileBrowserUi::RefreshEntries() {
    entries_.clear();
    std::error_code ec;
    if (!fs::is_directory(current_dir_, ec)) {
        status_error_ = "Directory is not readable: " + current_dir_.string();
        return;
    }
    std::vector<Entry> dirs;
    std::vector<Entry> files;
    for (auto& de : fs::directory_iterator(current_dir_, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code iec;
        bool is_d = de.is_directory(iec);
        std::string name = de.path().filename().string();
        if (name.empty()) continue;
        if (is_d) {
            dirs.push_back({de.path(), name, true, 0});
        } else if (de.is_regular_file(iec)) {
            if (!iso_filter_only_ || IsIsoLikeName(name)) {
                files.push_back({de.path(), name, false, de.file_size(iec)});
            }
        }
    }
    auto cmp = [](const Entry& a, const Entry& b) { return LowerAscii(a.name) < LowerAscii(b.name); };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);
    for (auto& d : dirs) entries_.push_back(std::move(d));
    for (auto& f : files) entries_.push_back(std::move(f));
}

void FileBrowserUi::InspectSelectedFile(const fs::path& p) {
    if (inspected_path_ == p && has_validation_) return;
    inspected_path_ = p;
    validation_ = app::DiscValidation{};
    app::ValidateDiscImage(p, validation_);
    has_validation_ = true;
}

bool FileBrowserUi::Draw(fs::path& out_selected) {
    if (!open_) return false;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 vp = io.DisplaySize;
    ImVec2 win_sz(std::min(vp.x * 0.92f, 1080.0f), std::min(vp.y * 0.88f, 680.0f));
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(win_sz, ImGuiCond_Always);

    bool confirmed = false;
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("Select Psi-Ops Xbox Disc Image (.xiso.iso / .iso)", &open_, wflags)) {
        // Top path bar + Up button + filter toggle
        if (ImGui::Button("Up (..)")) {
            if (current_dir_.has_parent_path() && current_dir_.parent_path() != current_dir_) {
                NavigateTo(current_dir_.parent_path());
                focus_list_pending_ = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Xbox Disc Images Only (.iso / .xiso)", &iso_filter_only_)) {
            RefreshEntries();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Path: %s", current_dir_.string().c_str());
        ImGui::Separator();

        float footer_h = 108.0f;
        float body_h = std::max(180.0f, ImGui::GetContentRegionAvail().y - footer_h);

        // Left Quick-Jump Sidebar (Steam Deck microSD / Drives / Game Folder)
        ImGui::BeginChild("QuickJumpSidebar", ImVec2(215.0f, body_h), true);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.86f, 0.96f, 1.0f));
        ImGui::TextUnformatted("Quick-Jump Locations");
        ImGui::PopStyleColor();
        ImGui::Separator();
        auto jumps = BuildQuickJumps();
        for (const auto& q : jumps) {
            if (ImGui::Button(q.label.c_str(), ImVec2(-1.0f, 34.0f))) {
                NavigateTo(q.dir);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right File & Folder List
        ImGui::BeginChild("FileListPanel", ImVec2(0.0f, body_h), true);
        if (!status_error_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", status_error_.c_str());
        }
        if (ImGui::BeginTable("BrowserTable", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 105.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < int(entries_.size()); ++i) {
                const Entry& e = entries_[i];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 32.0f);
                ImGui::TableSetColumnIndex(0);
                std::string label = (e.is_dir ? "[DIR]  " : "[ISO]  ") + e.name;
                bool sel = (selected_idx_ == i);
                if (i == 0 && focus_list_pending_) {
                    ImGui::SetKeyboardFocusHere();
                    focus_list_pending_ = false;
                }
                if (ImGui::Selectable(label.c_str(), sel,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0.0f, 28.0f))) {
                    selected_idx_ = i;
                    if (e.is_dir) {
                        has_validation_ = false;
                        inspected_path_.clear();
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            NavigateTo(e.path);
                            focus_list_pending_ = true;
                            break;
                        }
                    } else {
                        InspectSelectedFile(e.path);
                        if (ImGui::IsMouseDoubleClicked(0) && has_validation_ && validation_.valid) {
                            out_selected = e.path;
                            confirmed = true;
                            open_ = false;
                            break;
                        }
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", e.is_dir ? "Folder" : "Disc Image");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", e.is_dir ? "--" : FormatBytes(e.size_bytes).c_str());
            }
            ImGui::EndTable();
        }
        if (entries_.empty()) focus_list_pending_ = false;  // nothing to focus in an empty / unreadable folder
        ImGui::EndChild();

        // Live ValidateDiscImage Status Banner
        ImGui::Separator();
        if (has_validation_) {
            if (validation_.valid) {
                ui_common::DrawBadge("[Verified Retail]", ImVec4(0.11f, 0.48f, 0.28f, 1.0f));
                ImGui::SameLine();
                ImGui::Text("%s | Title ID 0x%08X | XDK %u | SHA-1 %.12s...", validation_.title_name.c_str(),
                            validation_.title_id, unsigned(validation_.xdk_build), validation_.sha1.c_str());
            } else {
                ui_common::DrawBadge(validation_.code.c_str(), ImVec4(0.60f, 0.14f, 0.16f, 1.0f));
                ImGui::SameLine();
                ImGui::TextWrapped("%s", validation_.error.c_str());
            }
        } else {
            ImGui::TextDisabled("Highlight an Xbox .xiso.iso / .iso file to validate Title ID 0x4D570016 & XBE SHA-1 live.");
        }

        // Bottom Action Buttons
        ImGui::Spacing();
        bool can_open_dir = (selected_idx_ >= 0 && selected_idx_ < int(entries_.size()) && entries_[selected_idx_].is_dir);
        bool can_confirm = (selected_idx_ >= 0 && selected_idx_ < int(entries_.size()) &&
                            !entries_[selected_idx_].is_dir && has_validation_ && validation_.valid);
        if (can_open_dir) {
            if (ImGui::Button("Open Folder (A)", ImVec2(180.0f, 36.0f))) {
                NavigateTo(entries_[selected_idx_].path);
                focus_list_pending_ = true;
            }
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!can_confirm);
        if (ImGui::Button("Select Verified Disc Image (A)", ImVec2(250.0f, 36.0f))) {
            out_selected = entries_[selected_idx_].path;
            confirmed = true;
            open_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Gamepad B (as labelled) and Esc cancel only while the browser has focus.
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::Button("Cancel (B / Esc)", ImVec2(160.0f, 36.0f)) ||
            (focused && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))) {
            open_ = false;
        }
    }
    ImGui::End();
    if (!open_) focus_list_pending_ = false;
    return confirmed;
}

}  // namespace file_browser_ui
