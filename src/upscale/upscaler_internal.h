// SPDX-License-Identifier: GPL-2.0-or-later
// Internal interfaces shared across D3D12 spatial and temporal upscaler backends.

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

#include "app/app.h"
#include "gpu/backend.h"

namespace upscale {

struct TemporalExecDesc {
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* color = nullptr;
    D3D12_RESOURCE_STATES* color_state = nullptr;
    DXGI_FORMAT color_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES* depth_state = nullptr;
    ID3D12Resource* motion_vectors = nullptr;
    D3D12_RESOURCE_STATES* mv_state = nullptr;
    ID3D12Resource* reactive_mask = nullptr;
    D3D12_RESOURCE_STATES* reactive_state = nullptr;
    ID3D12Resource* output = nullptr;
    D3D12_RESOURCE_STATES* output_state = nullptr;
    uint32_t render_w = 0;
    uint32_t render_h = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float sharpness = 0.0f;
    float dt_ms = 16.667f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    float fov_y_rad = 1.0f;
    bool reset = false;
    app::UpscaleQuality quality = app::UpscaleQuality::Quality;
    app::DlssPreset dlss_preset = app::DlssPreset::PresetK;
};

class ITemporalBackend {
public:
    virtual ~ITemporalBackend() = default;
    virtual bool Init(ID3D12Device* dev, ID3D12CommandQueue* queue, const gpu::GpuCaps& caps,
                      const gpu::UpscalerHostOps& host) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsAvailable() const = 0;
    virtual const std::string& StatusDetail() const = 0;
    virtual bool Execute(const TemporalExecDesc& td) = 0;
    virtual bool Prewarm(ID3D12GraphicsCommandList* cmd, uint32_t render_w, uint32_t render_h, uint32_t out_w,
                         uint32_t out_h, app::UpscaleQuality quality, app::DlssPreset dlss_preset) {
        (void)cmd;
        (void)render_w;
        (void)render_h;
        (void)out_w;
        (void)out_h;
        (void)quality;
        (void)dlss_preset;
        return false;
    }
    virtual void ResetHistory() = 0;
};

class SpatialD3D12 {
public:
    SpatialD3D12();
    ~SpatialD3D12();

    bool Init(ID3D12Device* dev, const gpu::UpscalerHostOps& host);
    void Prewarm(uint32_t vp_w, uint32_t vp_h);
    void Shutdown();

    // Executes one of the 6 zero-DLL spatial upscalers (FSR 1.0, NIS v1.0.3, CAS, Bicubic, Integer, Bilinear)
    // and composites directly into the swapchain backbuffer at (vp_x, vp_y, vp_w, vp_h) with optional gamma LUT.
    bool Execute(app::UpscalerMode mode, float sharpness, const gpu::UpscalerFrameDesc& fd);

    // Composites an already-upscaled temporal output texture (vp_w x vp_h) into the swapchain backbuffer
    // with optional RCAS sharpening and Xbox gamma LUT.
    bool CompositeOutput(ID3D12Resource* src_tex, D3D12_RESOURCE_STATES& src_state, DXGI_FORMAT src_fmt,
                         float extra_sharpness, const gpu::UpscalerFrameDesc& fd);

    // Generates render-resolution RG16F camera motion vectors (prev_px - curr_px), R8_UNORM HUD reactive mask,
    // and clean R32_FLOAT reversed-Z depth copy for universal DLSS 4 / FSR 3.1 / XeSS 2 compatibility.
    bool BuildTemporalInputs(ID3D12GraphicsCommandList* cmd, ID3D12Resource* front_res,
                             D3D12_RESOURCE_STATES& front_state, DXGI_FORMAT front_fmt, ID3D12Resource* scene_color_res,
                             D3D12_RESOURCE_STATES* scene_color_state, DXGI_FORMAT scene_color_fmt,
                             ID3D12Resource* depth_res, D3D12_RESOURCE_STATES& depth_state,
                             ID3D12Resource* out_mv, D3D12_RESOURCE_STATES& mv_state, ID3D12Resource* out_reactive,
                             D3D12_RESOURCE_STATES& reactive_state, ID3D12Resource* out_depth_r32f,
                             D3D12_RESOURCE_STATES& depth_r32f_state, uint32_t w, uint32_t h,
                             const float cur_vp_inv[16], const float prev_vp[16], bool has_prev_vp, float jx, float jy);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<ITemporalBackend> CreateDlssBackend();
std::unique_ptr<ITemporalBackend> CreateFsr3Backend();
std::unique_ptr<ITemporalBackend> CreateXessBackend();

}  // namespace upscale

#endif  // _WIN32
