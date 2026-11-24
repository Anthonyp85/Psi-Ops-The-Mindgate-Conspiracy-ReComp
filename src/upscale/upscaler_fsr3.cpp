// SPDX-License-Identifier: GPL-2.0-or-later
// AMD FSR 3.1 (FidelityFX API v1.x) Win64 D3D12 backend via plugins/amd_fidelityfx_{loader,upscaler}_dx12.dll.

#ifdef _WIN32

#include "upscale/ffx_abi.h"
#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "app/app.h"
#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

ffx_abi::SurfaceFormat MapDxgiToFfxFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return ffx_abi::kFmtR16G16B16A16Float;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return ffx_abi::kFmtR8G8B8A8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return ffx_abi::kFmtB8G8R8A8Unorm;
        case DXGI_FORMAT_R16G16_FLOAT: return ffx_abi::kFmtR16G16Float;
        case DXGI_FORMAT_R8_UNORM: return ffx_abi::kFmtR8Unorm;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return ffx_abi::kFmtR32Float;
        default: return ffx_abi::kFmtB8G8R8A8Unorm;
    }
}

ffx_abi::Resource MakeFfxTex(ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t usage,
                             uint32_t state) {
    ffx_abi::Resource r{};
    if (!res) return r;
    r.resource = res;
    r.description.type = ffx_abi::kResourceTexture2D;
    r.description.format = MapDxgiToFfxFormat(fmt);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = 0;
    r.description.usage = usage;
    r.state = state;
    return r;
}

class Fsr3Backend final : public ITemporalBackend {
public:
    ~Fsr3Backend() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& /*caps*/,
              const gpu::UpscalerHostOps& host) override {
        Shutdown();
        dev_ = dev;
        host_ = host;
        if (!dev) return false;

        PluginProbe probe = ProbePlugins(false);
        if (!probe.fsr3_ready) {
            detail_ = probe.fsr3_detail;
            LOGW("upscale: FSR 3.1 unavailable: %s", detail_.c_str());
            return false;
        }

        // Preload sibling upscaler backend DLL first so the loader resolves it strictly from plugins/
        upscaler_mod_ = LoadLibraryExW(probe.fsr3_upscaler_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        loader_mod_ = LoadLibraryExW(probe.fsr3_loader_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!loader_mod_) {
            detail_ = "LoadLibrary(amd_fidelityfx_loader_dx12.dll) failed";
            LOGW("upscale: %s (err=%lu)", detail_.c_str(), GetLastError());
            Shutdown();
            return false;
        }

        fn_create_ = reinterpret_cast<ffx_abi::PfnCreateContext>(GetProcAddress(loader_mod_, "ffxCreateContext"));
        fn_destroy_ = reinterpret_cast<ffx_abi::PfnDestroyContext>(GetProcAddress(loader_mod_, "ffxDestroyContext"));
        fn_dispatch_ = reinterpret_cast<ffx_abi::PfnDispatch>(GetProcAddress(loader_mod_, "ffxDispatch"));
        if (!fn_create_ || !fn_destroy_ || !fn_dispatch_) {
            detail_ = "Missing ffxCreateContext/ffxDestroyContext/ffxDispatch exports";
            Shutdown();
            return false;
        }

        available_ = true;
        detail_ = "FSR 3.1 Ready (FidelityFX API DX12)";
        LOGR("upscale: FSR 3.1 initialized (%s)", probe.fsr3_loader_path.string().c_str());
        return true;
    }

    void Shutdown() override {
        DestroyCtx();
        if (loader_mod_) {
            FreeLibrary(loader_mod_);
            loader_mod_ = nullptr;
        }
        if (upscaler_mod_) {
            FreeLibrary(upscaler_mod_);
            upscaler_mod_ = nullptr;
        }
        fn_create_ = nullptr;
        fn_destroy_ = nullptr;
        fn_dispatch_ = nullptr;
        available_ = false;
        dev_ = nullptr;
    }

    bool IsAvailable() const override { return available_; }
    const std::string& StatusDetail() const override { return detail_; }
    void ResetHistory() override { force_reset_ = true; }

    bool Prewarm(ID3D12GraphicsCommandList* /*cmd*/, uint32_t render_w, uint32_t render_h, uint32_t out_w,
                 uint32_t out_h, app::UpscaleQuality /*quality*/, app::DlssPreset /*dlss_preset*/) override {
        if (!available_ || render_w == 0 || render_h == 0 || out_w == 0 || out_h == 0 || render_w > out_w ||
            render_h > out_h) {
            return false;
        }
        if (!ctx_ || cur_ow_ != out_w || cur_oh_ != out_h) {
            if (!RecreateCtx(out_w, out_h)) return false;
            force_reset_ = true;
        }
        return true;
    }

    bool Execute(const TemporalExecDesc& td) override {
        if (!available_ || !td.cmd || !td.color || !td.depth || !td.motion_vectors || !td.output) return false;
        if (!ctx_ || cur_ow_ != td.out_w || cur_oh_ != td.out_h) {
            if (!RecreateCtx(td.out_w, td.out_h)) return false;
            force_reset_ = true;
        }

        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* s, D3D12_RESOURCE_STATES want) {
            if (!r || !s) return;
            uint32_t u = uint32_t(*s);
            host_.transition(host_.ctx, r, &u, uint32_t(want));
            *s = static_cast<D3D12_RESOURCE_STATES>(u);
        };
        const D3D12_RESOURCE_STATES kComputeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        tr(td.color, td.color_state, kComputeRead);
        tr(td.depth, td.depth_state, kComputeRead);
        tr(td.motion_vectors, td.mv_state, kComputeRead);
        if (td.reactive_mask) tr(td.reactive_mask, td.reactive_state, kComputeRead);
        tr(td.output, td.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ffx_abi::DispatchDescUpscale dd{};
        dd.header.type = ffx_abi::kDescTypeDispatchUpscale;
        dd.header.pNext = nullptr;
        dd.commandList = td.cmd;
        dd.color = MakeFfxTex(td.color, td.color_fmt, td.render_w, td.render_h, ffx_abi::kUsageReadOnly,
                              ffx_abi::kStateComputeRead);
        dd.depth = MakeFfxTex(td.depth, DXGI_FORMAT_R32_FLOAT, td.render_w, td.render_h, ffx_abi::kUsageReadOnly,
                              ffx_abi::kStateComputeRead);
        dd.motionVectors = MakeFfxTex(td.motion_vectors, DXGI_FORMAT_R16G16_FLOAT, td.render_w, td.render_h,
                                      ffx_abi::kUsageReadOnly, ffx_abi::kStateComputeRead);
        if (td.reactive_mask) {
            dd.reactive = MakeFfxTex(td.reactive_mask, DXGI_FORMAT_R8_UNORM, td.render_w, td.render_h,
                                     ffx_abi::kUsageReadOnly, ffx_abi::kStateComputeRead);
        }
        dd.output = MakeFfxTex(td.output, DXGI_FORMAT_R8G8B8A8_UNORM, td.out_w, td.out_h, ffx_abi::kUsageUav,
                               ffx_abi::kStateUnorderedAccess);
        dd.jitterOffset = {td.jitter_x, td.jitter_y};
        dd.motionVectorScale = {1.0f, 1.0f};
        dd.renderSize = {td.render_w, td.render_h};
        dd.upscaleSize = {td.out_w, td.out_h};
        dd.enableSharpening = (td.sharpness > 0.01f);
        dd.sharpness = std::clamp(td.sharpness, 0.0f, 1.0f);
        dd.frameTimeDelta = std::clamp(td.dt_ms, 1.0f, 100.0f);
        dd.preExposure = 1.0f;
        dd.reset = (td.reset || force_reset_);
        // With kCreateDepthInverted, FSR 3.1 expects cameraNear = far plane and cameraFar = near plane
        dd.cameraNear = std::max(td.far_plane, 10.0f);
        dd.cameraFar = std::max(td.near_plane, 0.01f);
        dd.cameraFovAngleVertical = std::clamp(td.fov_y_rad, 0.2f, 2.5f);
        dd.viewSpaceToMetersFactor = 1.0f;
        dd.flags = 0;

        ffx_abi::ReturnCode rc = fn_dispatch_(&ctx_, &dd.header);
        force_reset_ = false;
        if (rc != ffx_abi::kReturnOk) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "FSR 3.1 ffxDispatch failed (rc=%u)", rc);
            detail_ = buf;
            LOGW("upscale: %s", detail_.c_str());
            return false;
        }

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = td.output;
        td.cmd->ResourceBarrier(1, &uav);
        return true;
    }

private:
    void DestroyCtx() {
        if (ctx_ && fn_destroy_) {
            if (host_.flush) host_.flush(host_.ctx);
            fn_destroy_(&ctx_, nullptr);
            ctx_ = nullptr;
        }
        cur_ow_ = cur_oh_ = 0;
    }

    bool RecreateCtx(uint32_t ow, uint32_t oh) {
        DestroyCtx();
        ffx_abi::CreateBackendDx12Desc backend{};
        backend.header.type = ffx_abi::kDescTypeBackendDx12;
        backend.header.pNext = nullptr;
        backend.device = dev_;

        ffx_abi::CreateContextDescUpscale up{};
        up.header.type = ffx_abi::kDescTypeCreateUpscale;
        up.header.pNext = &backend.header;
        up.flags = ffx_abi::kCreateDepthInverted | ffx_abi::kCreateDynamicResolution;
        up.maxRenderSize = {ow, oh};
        up.maxUpscaleSize = {ow, oh};
        up.fpMessage = nullptr;

        ffx_abi::ReturnCode rc = fn_create_(&ctx_, &up.header, nullptr);
        if (rc != ffx_abi::kReturnOk || !ctx_) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "FSR 3.1 ffxCreateContext failed (rc=%u)", rc);
            detail_ = buf;
            LOGW("upscale: %s (%ux%u)", detail_.c_str(), ow, oh);
            ctx_ = nullptr;
            return false;
        }
        cur_ow_ = ow;
        cur_oh_ = oh;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "FSR 3.1 active (max %ux%u)", ow, oh);
        detail_ = buf;
        LOGR("upscale: %s", detail_.c_str());
        return true;
    }

    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE upscaler_mod_ = nullptr;
    HMODULE loader_mod_ = nullptr;
    ffx_abi::PfnCreateContext fn_create_ = nullptr;
    ffx_abi::PfnDestroyContext fn_destroy_ = nullptr;
    ffx_abi::PfnDispatch fn_dispatch_ = nullptr;
    ffx_abi::Context ctx_ = nullptr;
    uint32_t cur_ow_ = 0, cur_oh_ = 0;
    bool available_ = false;
    bool force_reset_ = true;
    std::string detail_;
};

}  // namespace

std::unique_ptr<ITemporalBackend> CreateFsr3Backend() { return std::make_unique<Fsr3Backend>(); }

}  // namespace upscale

#endif  // _WIN32
