// SPDX-License-Identifier: GPL-2.0-or-later
// In-App Gamepad/Touch/Mouse XISO File Browser with Steam Deck microSD Quick-Jump
// and live app::ValidateDiscImage() status badges.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"

namespace file_browser_ui {

class FileBrowserUi {
public:
    void Open(const std::filesystem::path& start_dir = {});
    void Close();
    bool IsOpen() const { return open_; }

    // Draws the modal file browser inside an active ImGui frame.
    // Returns true when the user confirmed a valid disc image (path written to out_selected).
    bool Draw(std::filesystem::path& out_selected);

private:
    struct Entry {
        std::filesystem::path path;
        std::string name;
        bool is_dir = false;
        uint64_t size_bytes = 0;
    };

    void NavigateTo(const std::filesystem::path& dir);
    void RefreshEntries();
    void InspectSelectedFile(const std::filesystem::path& p);

    bool open_ = false;
    bool iso_filter_only_ = true;
    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    int selected_idx_ = -1;
    std::filesystem::path inspected_path_;
    app::DiscValidation validation_{};
    bool has_validation_ = false;
    std::string status_error_;
};

}  // namespace file_browser_ui
