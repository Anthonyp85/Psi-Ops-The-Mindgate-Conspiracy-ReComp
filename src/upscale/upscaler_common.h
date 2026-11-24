// SPDX-License-Identifier: GPL-2.0-or-later
// Shared upscaler configuration helpers, Halton (2,3) jitter, camera-cut detection,
// 4x4 matrix utilities, plugin probing, and Authenticode-verified NGX fallback.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "app/app.h"
#include "gpu/backend.h"

namespace upscale {

const char* ModeName(app::UpscalerMode mode);
const char* ModeShortName(app::UpscalerMode mode);
const char* QualityName(app::UpscaleQuality quality);
const char* DlssPresetName(app::DlssPreset preset);
const char* HardwarePresetName(app::HardwarePreset preset);
bool IsTemporalMode(app::UpscalerMode mode);

float QualityScaleFactor(app::UpscaleQuality quality, uint32_t custom_percent);

void ComputeRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, app::UpscalerMode mode,
                       app::UpscaleQuality quality, uint32_t custom_percent, bool aspect_correct,
                       uint32_t& out_render_w, uint32_t& out_render_h);

float Halton(uint32_t index, uint32_t base);
uint32_t JitterPhaseCount(uint32_t render_w, uint32_t vp_w);
void ComputeJitter(uint32_t phase_index, uint32_t render_w, uint32_t vp_w, float& out_jx, float& out_jy);

bool IsCameraCut(const gpu::CameraFrameData& prev, const gpu::CameraFrameData& cur);

bool InvertMatrix4x4(const float m[16], float out[16]);
void MulMatrix4x4(const float a[16], const float b[16], float out[16]);

struct PluginProbe {
    bool dlss_plugin_present = false;
    bool dlss_programdata_available = false;
    bool dlss_driver_present = false;
    bool dlss_ready = false;
    bool fsr3_ready = false;
    bool xess_ready = false;
    std::filesystem::path dlss_path;
    std::filesystem::path ngx_core_path;
    std::filesystem::path fsr3_loader_path;
    std::filesystem::path fsr3_upscaler_path;
    std::filesystem::path xess_path;
    std::string dlss_detail;
    std::string fsr3_detail;
    std::string xess_detail;
};

// Probes Win64 retail upscaler plugins beside the executable (plus NVIDIA DriverStore + ProgramData fallback).
// On Linux/Vulkan, temporal Win64 plugins report unavailable and spatial upscalers remain ready.
PluginProbe ProbePlugins(bool stage_programdata_fallback = false);

// Resolves nvngx_dlss.dll from plugins/ or stages an Authenticode-verified NVIDIA retail model from
// %ProgramData%\NVIDIA\NGX\models\dlss\ into cache/ngx/nvngx_dlss.dll.
std::filesystem::path ResolveNgxDlssPath(bool* out_used_programdata = nullptr, std::string* out_detail = nullptr);

// Locates NVIDIA's signed DriverStore _nvngx.dll via HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore.
std::filesystem::path LocateNgxCoreDll();

// Validates PE32+ x64 header, zero "/dev/" watermark markers, and offline Authenticode signature on Win64.
bool VerifyPe64AndAuthenticode(const std::filesystem::path& path, const char* expected_signer_substr,
                               std::string* out_reason = nullptr);

// Applies one of the 5 Hardware Presets cleanly to app::Settings.
void ApplyHardwarePreset(app::Settings& s, app::HardwarePreset preset, const PluginProbe& probe);

}  // namespace upscale
