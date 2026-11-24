// SPDX-License-Identifier: GPL-2.0-or-later
// Intel XeSS 2 (libxess.dll) Win64 D3D12 backend via clean-room xess_abi.h.

#ifdef _WIN32

#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"
#include "upscale/xess_abi.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/log.h"

namespace upscale {

namespace {

xess_abi::QualitySetting MapXessQuality(app::UpscaleQuality q, uint32_t rw, uint32_t ow) {
    switch (q) {
        case app::UpscaleQuality::NativeAA: return xess_abi::kQualityAA;
        case app::UpscaleQuality::UltraQuality: return xess_abi::kQualityUltraQuality;
        case app::UpscaleQuality::Quality: return xess_abi::kQualityQuality;
        case app::UpscaleQuality::Balanced: return xess_abi::kQualityBalanced;
        case app::UpscaleQuality::Performance: return xess_abi::kQualityPerformance;
        case app::UpscaleQuality::UltraPerformance: return xess_abi::kQualityUltraPerformance;
        case app::UpscaleQuality::Custom:
        default: {
            float r = (ow > 0) ? (float(rw) / float(ow)) : 1.0f;
            if (r >= 0.95f) return xess_abi::kQualityAA;
            if (r >= 0.72f) return xess_abi::kQualityUltraQuality;
            if (r >= 0.62f) return xess_abi::kQualityQuality;
            if (r >= 0.54f) return xess_abi::kQualityBalanced;
            if (r >= 0.42f) return xess_abi::kQualityPerformance;
            return xess_abi::kQualityUltraPerformance;
        }
    }
}

class XessBackend final : public ITemporalBackend {
public:
    ~XessBackend() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& /*caps*/,
              const gpu::UpscalerHostOps& host) override {
        Shutdown();
        dev_ = dev;
        host_ = host;
        if (!dev) return false;

        PluginProbe probe = ProbePlugins(false);
        if (!probe.xess_ready) {
            detail_ = probe.xess_detail;
            LOGW("upscale: XeSS 2 unavailable: %s", detail_.c_str());
            return false;
        }

        mod_ = LoadLibraryExW(probe.xess_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod_) {
            detail_ = "LoadLibrary(libxess.dll) failed";
            LOGW("upscale: %s (err=%lu)", detail_.c_str(), GetLastError());
            return false;
        }

        auto load = [&](auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(mod_, name));
            return fn != nullptr;
        };
        bool ok = load(fn_create_, "xessD3D12CreateContext") && load(fn_destroy_, "xessDestroyContext") &&
                  load(fn_init_, "xessD3D12Init") && load(fn_exec_, "xessD3D12Execute");
        load(fn_ver_, "xessGetVersion");
        load(fn_vel_scale_, "xessSetVelocityScale");
        load(fn_jit_scale_, "xessSetJitterScale");
        if (!ok) {
            detail_ = "Missing required xessD3D12* exports in libxess.dll";
            Shutdown();
            return false;
        }

        xess_abi::Version ver{};
        if (fn_ver_) fn_ver_(&ver);

        xess_abi::Result r = fn_create_(dev_, &ctx_);
        if (!xess_abi::Succeeded(r) || !ctx_) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "xessD3D12CreateContext failed (rc=%d)", r);
            detail_ = buf;
            LOGW("upscale: %s", detail_.c_str());
            Shutdown();
            return false;
        }

        available_ = true;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "XeSS %u.%u.%u Ready (plugins/libxess.dll)", ver.major, ver.minor, ver.patch);
        detail_ = buf;
        LOGR("upscale: %s", detail_.c_str());
        return true;
    }

    void Shutdown() override {
        if (ctx_ && fn_destroy_) {
            if (host_.flush) host_.flush(host_.ctx);
            fn_destroy_(ctx_);
            ctx_ = nullptr;
        }
        if (mod_) {
            FreeLibrary(mod_);
            mod_ = nullptr;
        }
        available_ = false;
        inited_ = false;
        cur_ow_ = cur_oh_ = 0;
        dev_ = nullptr;
    }

    bool IsAvailable() const override { return available_; }
    const std::string& StatusDetail() const override { return detail_; }
    void ResetHistory() override { force_reset_ = true; }

    bool Prewarm(ID3D12GraphicsCommandList* /*cmd*/, uint32_t render_w, uint32_t render_h, uint32_t out_w,
                 uint32_t out_h, app::UpscaleQuality quality, app::DlssPreset /*dlss_preset*/) override {
        if (!available_ || !ctx_ || render_w == 0 || render_h == 0 || out_w == 0 || out_h == 0 || render_w > out_w ||
            render_h > out_h) {
            return false;
        }
        xess_abi::QualitySetting qs = MapXessQuality(quality, render_w, out_w);
        const bool want_mask = true;  // D3D12UpscalerManager always passes reactive_tex_
        if (!inited_ || cur_ow_ != out_w || cur_oh_ != out_h || cur_qs_ != qs || cur_mask_ != want_mask) {
            if (!ReinitCtx(out_w, out_h, qs, want_mask)) return false;
            force_reset_ = true;
        }
        return true;
    }

    bool Execute(const TemporalExecDesc& td) override {
        if (!available_ || !ctx_ || !td.cmd || !td.color || !td.depth || !td.motion_vectors || !td.output) {
            return false;
        }
        xess_abi::QualitySetting qs = MapXessQuality(td.quality, td.render_w, td.out_w);
        bool want_mask = (td.reactive_mask != nullptr);
        if (!inited_ || cur_ow_ != td.out_w || cur_oh_ != td.out_h || cur_qs_ != qs || cur_mask_ != want_mask) {
            if (!ReinitCtx(td.out_w, td.out_h, qs, want_mask)) return false;
            force_reset_ = true;
        }

        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* s, D3D12_RESOURCE_STATES want) {
            if (!r || !s) return;
            uint32_t u = uint32_t(*s);
            host_.transition(host_.ctx, r, &u, uint32_t(want));
            *s = static_cast<D3D12_RESOURCE_STATES>(u);
        };
        const D3D12_RESOURCE_STATES kSrv =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        tr(td.color, td.color_state, kSrv);
        tr(td.depth, td.depth_state, kSrv);
        tr(td.motion_vectors, td.mv_state, kSrv);
        if (td.reactive_mask) tr(td.reactive_mask, td.reactive_state, kSrv);
        tr(td.output, td.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        xess_abi::D3D12ExecuteParams ep{};
        ep.pColorTexture = td.color;
        ep.pVelocityTexture = td.motion_vectors;
        ep.pDepthTexture = td.depth;
        ep.pExposureScaleTexture = nullptr;
        ep.pResponsivePixelMaskTexture = want_mask ? td.reactive_mask : nullptr;
        ep.pOutputTexture = td.output;
        ep.jitterOffsetX = td.jitter_x;
        ep.jitterOffsetY = td.jitter_y;
        ep.exposureScale = 1.0f;
        ep.resetHistory = (td.reset || force_reset_) ? 1u : 0u;
        ep.inputWidth = td.render_w;
        ep.inputHeight = td.render_h;

        xess_abi::Result r = fn_exec_(ctx_, td.cmd, &ep);
        force_reset_ = false;
        if (!xess_abi::Succeeded(r)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "xessD3D12Execute failed (rc=%d)", r);
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
    bool ReinitCtx(uint32_t ow, uint32_t oh, xess_abi::QualitySetting qs, bool want_mask) {
        if (inited_ && host_.flush) host_.flush(host_.ctx);
        xess_abi::D3D12InitParams ip{};
        ip.outputResolution = {ow, oh};
        ip.qualitySetting = qs;
        ip.initFlags = xess_abi::kInitInvertedDepth | xess_abi::kInitLdrInputColor |
                       (want_mask ? xess_abi::kInitResponsivePixelMask : 0u);
        ip.creationNodeMask = 1;
        ip.visibleNodeMask = 1;
        ip.pPipelineLibrary = host_.get_pipeline_library
                                  ? static_cast<ID3D12PipelineLibrary*>(host_.get_pipeline_library(host_.ctx))
                                  : nullptr;

        xess_abi::Result r = fn_init_(ctx_, &ip);
        if (!xess_abi::Succeeded(r) && ip.pPipelineLibrary != nullptr) {
            ip.pPipelineLibrary = nullptr;
            r = fn_init_(ctx_, &ip);
        }
        if (!xess_abi::Succeeded(r) && qs == xess_abi::kQualityAA) {
            ip.qualitySetting = xess_abi::kQualityUltraQuality;
            r = fn_init_(ctx_, &ip);
        }
        if (!xess_abi::Succeeded(r)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "xessD3D12Init failed (rc=%d)", r);
            detail_ = buf;
            LOGW("upscale: %s (%ux%u, q=%d)", detail_.c_str(), ow, oh, int(qs));
            inited_ = false;
            return false;
        }
        if (fn_vel_scale_) fn_vel_scale_(ctx_, 1.0f, 1.0f);
        if (fn_jit_scale_) fn_jit_scale_(ctx_, 1.0f, 1.0f);
        inited_ = true;
        cur_ow_ = ow;
        cur_oh_ = oh;
        cur_qs_ = qs;
        cur_mask_ = want_mask;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "XeSS 2 active (%ux%u)", ow, oh);
        detail_ = buf;
        LOGR("upscale: %s", detail_.c_str());
        return true;
    }

    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE mod_ = nullptr;
    xess_abi::ContextHandle ctx_ = nullptr;
    xess_abi::PfnGetVersion fn_ver_ = nullptr;
    xess_abi::PfnD3D12CreateContext fn_create_ = nullptr;
    xess_abi::PfnDestroyContext fn_destroy_ = nullptr;
    xess_abi::PfnD3D12Init fn_init_ = nullptr;
    xess_abi::PfnD3D12Execute fn_exec_ = nullptr;
    xess_abi::PfnSetVelocityScale fn_vel_scale_ = nullptr;
    xess_abi::PfnSetJitterScale fn_jit_scale_ = nullptr;
    uint32_t cur_ow_ = 0, cur_oh_ = 0;
    xess_abi::QualitySetting cur_qs_ = xess_abi::kQualityQuality;
    bool cur_mask_ = false;
    bool available_ = false;
    bool inited_ = false;
    bool force_reset_ = true;
    std::string detail_;
};

}  // namespace

std::unique_ptr<ITemporalBackend> CreateXessBackend() { return std::make_unique<XessBackend>(); }

}  // namespace upscale

#endif  // _WIN32
