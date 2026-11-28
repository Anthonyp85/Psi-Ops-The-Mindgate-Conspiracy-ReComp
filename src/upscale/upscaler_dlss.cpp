// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA DLSS 4 (NGX Super Resolution / DLAA, Presets Auto/E/J/K) Win64 D3D12 backend
// via signed DriverStore _nvngx.dll and retail /rel/ plugins/nvngx_dlss.dll
// (with Authenticode-verified %ProgramData%\NVIDIA\NGX\models\dlss\ -> cache\ngx\nvngx_dlss.dll fallback).

#ifdef _WIN32

#include "upscale/ngx_abi.h"
#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "app/app.h"
#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

constexpr unsigned long long kPsiOpsNgxAppId = 0x4D570016ull;

// NVSDK_NGX_PerfQuality_Value_UltraQuality (4) is reserved/unimplemented in retail nvngx_dlss.dll builds and
// CreateFeature rejects it with 0xBAD00010 (NVSDK_NGX_Result_FAIL_InvalidParameter). DLSS takes the explicit
// render/output dimensions we pass, so the PerfQuality value is only a hint bucket: Ultra Quality (77%) and any
// Custom ratio above the Quality bucket are created as MaxQuality with their real (larger) input size.
ngx_abi::PerfQuality MapQuality(app::UpscaleQuality q, uint32_t render_w, uint32_t render_h, uint32_t out_w,
                                uint32_t out_h) {
    // Input == output (+/-1 px rounding) is DLAA regardless of the selected tier.
    if (out_w > 0 && out_h > 0 && render_w + 1u >= out_w && render_h + 1u >= out_h) {
        return ngx_abi::kPerfQualityDlaa;
    }
    switch (q) {
        case app::UpscaleQuality::NativeAA: return ngx_abi::kPerfQualityDlaa;
        case app::UpscaleQuality::UltraQuality: return ngx_abi::kPerfQualityMaxQuality;
        case app::UpscaleQuality::Quality: return ngx_abi::kPerfQualityMaxQuality;
        case app::UpscaleQuality::Balanced: return ngx_abi::kPerfQualityBalanced;
        case app::UpscaleQuality::Performance: return ngx_abi::kPerfQualityMaxPerf;
        case app::UpscaleQuality::UltraPerformance: return ngx_abi::kPerfQualityUltraPerformance;
        case app::UpscaleQuality::Custom:
        default: {
            float r = (out_w > 0) ? (float(render_w) / float(out_w)) : 1.0f;
            if (r >= 0.95f) return ngx_abi::kPerfQualityDlaa;
            if (r >= 0.62f) return ngx_abi::kPerfQualityMaxQuality;
            if (r >= 0.54f) return ngx_abi::kPerfQualityBalanced;
            if (r >= 0.42f) return ngx_abi::kPerfQualityMaxPerf;
            return ngx_abi::kPerfQualityUltraPerformance;
        }
    }
}

uint32_t MapRenderPreset(app::DlssPreset p) {
    switch (p) {
        case app::DlssPreset::PresetE: return ngx_abi::kPresetE;
        case app::DlssPreset::PresetJ: return ngx_abi::kPresetJ;
        case app::DlssPreset::PresetK: return ngx_abi::kPresetK;
        case app::DlssPreset::Auto:
        default: return ngx_abi::kPresetDefault;
    }
}

class DlssBackend final : public ITemporalBackend {
public:
    ~DlssBackend() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& caps,
              const gpu::UpscalerHostOps& host) override {
        Shutdown();
        dev_ = dev;
        host_ = host;

        if (!dev || caps.vendor_id != 0x10DEu) {
            detail_ = "NVIDIA RTX GPU not selected";
            return false;
        }

        fs::path ngx_core = LocateNgxCoreDll();
        if (ngx_core.empty()) {
            detail_ = "NVIDIA NGX DriverStore _nvngx.dll not found";
            LOGW("upscale: DLSS unavailable: %s", detail_.c_str());
            return false;
        }

        bool used_pd = false;
        std::string resolve_det;
        fs::path dlss_dll = ResolveNgxDlssPath(&used_pd, &resolve_det);
        if (dlss_dll.empty()) {
            detail_ = resolve_det;
            LOGW("upscale: DLSS unavailable: %s", detail_.c_str());
            return false;
        }
        used_programdata_ = used_pd;
        dlss_dir_w_ = dlss_dll.parent_path().wstring();

        ngx_mod_ = LoadLibraryW(ngx_core.c_str());
        if (!ngx_mod_) {
            detail_ = "LoadLibrary(_nvngx.dll) failed";
            LOGW("upscale: DLSS LoadLibrary(%s) failed (err=%lu)", app::PathToUtf8(ngx_core).c_str(), GetLastError());
            return false;
        }

        auto load = [&](auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(ngx_mod_, name));
            return fn != nullptr;
        };

        bool ok = load(fn_init_ext_, "NVSDK_NGX_D3D12_Init_Ext") &&
                  load(fn_get_caps_, "NVSDK_NGX_D3D12_GetCapabilityParameters") &&
                  load(fn_alloc_params_, "NVSDK_NGX_D3D12_AllocateParameters") &&
                  load(fn_destroy_params_, "NVSDK_NGX_D3D12_DestroyParameters") &&
                  load(fn_create_feature_, "NVSDK_NGX_D3D12_CreateFeature") &&
                  load(fn_release_feature_, "NVSDK_NGX_D3D12_ReleaseFeature") &&
                  load(fn_eval_feature_, "NVSDK_NGX_D3D12_EvaluateFeature");
        load(fn_shutdown1_, "NVSDK_NGX_D3D12_Shutdown1");
        load(fn_shutdown_, "NVSDK_NGX_D3D12_Shutdown");
        if (!ok) {
            detail_ = "Missing required NVSDK_NGX_D3D12_* exports in _nvngx.dll";
            Shutdown();
            return false;
        }

        std::error_code ec;
        fs::path app_data = app::ExeDir() / "cache" / "ngx";
        fs::create_directories(app_data, ec);
        std::wstring app_data_w = app_data.wstring();

        const wchar_t* search_paths[2] = {dlss_dir_w_.c_str(), app_data_w.c_str()};
        ngx_abi::FeatureCommonInfo common{};
        common.path_list_info.path = search_paths;
        common.path_list_info.length = (dlss_dir_w_ == app_data_w) ? 1u : 2u;
        common.logging_info.minimum_logging_level = 0;

        ngx_abi::Result r =
            fn_init_ext_(kPsiOpsNgxAppId, app_data_w.c_str(), dev_, ngx_abi::kVersionApi21, &common);
        if (!ngx_abi::Succeeded(r)) {
            r = fn_init_ext_(kPsiOpsNgxAppId, app_data_w.c_str(), dev_, ngx_abi::kVersionApi20, &common);
        }
        if (!ngx_abi::Succeeded(r)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "NVSDK_NGX_D3D12_Init_Ext failed (0x%08X)", r);
            detail_ = buf;
            LOGW("upscale: DLSS init failed: %s", detail_.c_str());
            Shutdown();
            return false;
        }
        ngx_inited_ = true;

        ngx_abi::Parameter* caps_params = nullptr;
        r = fn_get_caps_(&caps_params);
        if (!ngx_abi::Succeeded(r) || !caps_params) {
            detail_ = "NVSDK_NGX_D3D12_GetCapabilityParameters failed";
            Shutdown();
            return false;
        }
        int ss_avail = 0;
        caps_params->Get(ngx_abi::kParamSuperSamplingAvailable, &ss_avail);
        if (!ss_avail) {
            int needs_drv = 0;
            caps_params->Get(ngx_abi::kParamSuperSamplingNeedsUpdatedDriver, &needs_drv);
            detail_ = needs_drv ? "NVIDIA driver update required for DLSS" : "DLSS SuperSampling not available on GPU";
            LOGW("upscale: %s", detail_.c_str());
            Shutdown();
            return false;
        }

        if (!ngx_abi::Succeeded(fn_alloc_params_(&params_)) || !params_) {
            detail_ = "NVSDK_NGX_D3D12_AllocateParameters failed";
            Shutdown();
            return false;
        }

        available_ = true;
        detail_ = used_programdata_ ? "DLSS 4 Ready (NGX ProgramData fallback)" : "DLSS 4 Ready (plugins/nvngx_dlss.dll)";
        LOGR("upscale: DLSS 4 initialized (core=%s, model=%s, programdata_fallback=%s)",
             app::PathToUtf8(ngx_core).c_str(), app::PathToUtf8(dlss_dll).c_str(),
             used_programdata_ ? "true" : "false");
        return true;
    }

    void Shutdown() override {
        ReleaseFeature();
        if (params_ && fn_destroy_params_) {
            fn_destroy_params_(params_);
            params_ = nullptr;
        }
        if (ngx_inited_) {
            if (fn_shutdown1_) fn_shutdown1_(dev_);
            else if (fn_shutdown_) fn_shutdown_();
            ngx_inited_ = false;
        }
        if (ngx_mod_) {
            FreeLibrary(ngx_mod_);
            ngx_mod_ = nullptr;
        }
        available_ = false;
        feature_failed_ = false;
        dev_ = nullptr;
    }

    bool IsAvailable() const override { return available_; }
    const std::string& StatusDetail() const override { return detail_; }
    void ResetHistory() override { force_reset_ = true; }

    bool Prewarm(ID3D12GraphicsCommandList* cmd, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h,
                 app::UpscaleQuality quality, app::DlssPreset dlss_preset) override {
        if (!available_ || !params_ || !cmd || render_w == 0 || render_h == 0 || out_w == 0 || out_h == 0 ||
            render_w > out_w || render_h > out_h) {
            return false;
        }
        ngx_abi::PerfQuality pq = MapQuality(quality, render_w, render_h, out_w, out_h);
        uint32_t preset_val = MapRenderPreset(dlss_preset);
        bool need_create = !handle_ || cur_rw_ != render_w || cur_rh_ != render_h || cur_ow_ != out_w ||
                           cur_oh_ != out_h || cur_pq_ != pq || cur_preset_ != preset_val;
        if (need_create) {
            if (IsKnownFailure(render_w, render_h, out_w, out_h, pq, preset_val)) return false;
            if (!RecreateFeature(cmd, render_w, render_h, out_w, out_h, pq, preset_val, dlss_preset)) {
                return false;
            }
            force_reset_ = true;
        }
        return true;
    }

    bool Execute(const TemporalExecDesc& td) override {
        if (!available_ || !params_ || !td.cmd || !td.color || !td.depth || !td.motion_vectors || !td.output) {
            return false;
        }
        ngx_abi::PerfQuality pq = MapQuality(td.quality, td.render_w, td.render_h, td.out_w, td.out_h);
        uint32_t preset_val = MapRenderPreset(td.dlss_preset);

        bool need_create = !handle_ || cur_rw_ != td.render_w || cur_rh_ != td.render_h || cur_ow_ != td.out_w ||
                           cur_oh_ != td.out_h || cur_pq_ != pq || cur_preset_ != preset_val;
        if (need_create) {
            // Don't retry a CreateFeature that already failed for this exact configuration every frame
            // (each attempt flushes the GPU queue and logs); the manager falls back to FSR 1.0 meanwhile.
            if (IsKnownFailure(td.render_w, td.render_h, td.out_w, td.out_h, pq, preset_val)) return false;
            if (!RecreateFeature(td.cmd, td.render_w, td.render_h, td.out_w, td.out_h, pq, preset_val,
                                 td.dlss_preset)) {
                return false;
            }
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

        params_->Reset();
        params_->Set(ngx_abi::kParamColor, td.color);
        params_->Set(ngx_abi::kParamOutput, td.output);
        params_->Set(ngx_abi::kParamDepth, td.depth);
        params_->Set(ngx_abi::kParamMotionVectors, td.motion_vectors);
        if (td.reactive_mask) {
            params_->Set(ngx_abi::kParamBiasCurrentColorMask, td.reactive_mask);
        }
        params_->Set(ngx_abi::kParamJitterOffsetX, td.jitter_x);
        params_->Set(ngx_abi::kParamJitterOffsetY, td.jitter_y);
        params_->Set(ngx_abi::kParamReset, (td.reset || force_reset_) ? 1 : 0);
        params_->Set(ngx_abi::kParamMvScaleX, 1.0f);
        params_->Set(ngx_abi::kParamMvScaleY, 1.0f);
        params_->Set(ngx_abi::kParamRenderSubrectW, td.render_w);
        params_->Set(ngx_abi::kParamRenderSubrectH, td.render_h);

        ngx_abi::Result r = fn_eval_feature_(td.cmd, handle_, params_, nullptr);
        force_reset_ = false;
        if (!ngx_abi::Succeeded(r)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DLSS EvaluateFeature failed (0x%08X)", r);
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
    void ReleaseFeature() {
        if (handle_ && fn_release_feature_) {
            if (host_.flush) host_.flush(host_.ctx);
            fn_release_feature_(handle_);
            handle_ = nullptr;
        }
        cur_rw_ = cur_rh_ = cur_ow_ = cur_oh_ = 0;
    }

    bool IsKnownFailure(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, ngx_abi::PerfQuality pq,
                        uint32_t preset_val) const {
        return feature_failed_ && fail_rw_ == rw && fail_rh_ == rh && fail_ow_ == ow && fail_oh_ == oh &&
               fail_pq_ == pq && fail_preset_ == preset_val;
    }

    bool RecreateFeature(ID3D12GraphicsCommandList* cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                         ngx_abi::PerfQuality pq, uint32_t preset_val, app::DlssPreset preset_enum) {
        ReleaseFeature();
        feature_failed_ = false;

        auto try_create = [&](ngx_abi::PerfQuality create_pq, uint32_t create_preset) -> ngx_abi::Result {
            params_->Reset();
            params_->Set(ngx_abi::kParamCreationNodeMask, 1u);
            params_->Set(ngx_abi::kParamVisibilityNodeMask, 1u);
            params_->Set(ngx_abi::kParamWidth, rw);
            params_->Set(ngx_abi::kParamHeight, rh);
            params_->Set(ngx_abi::kParamOutWidth, ow);
            params_->Set(ngx_abi::kParamOutHeight, oh);
            params_->Set(ngx_abi::kParamPerfQualityValue, int(create_pq));
            params_->Set(ngx_abi::kParamRtxValue, 1);

            uint32_t flags = ngx_abi::kFlagMvLowRes | ngx_abi::kFlagDepthInverted;
            params_->Set(ngx_abi::kParamDlssCreateFlags, int(flags));

            // Apply selected DLSS render preset across all quality buckets so DLAA + Quality..UltraPerf honor it
            params_->Set(ngx_abi::kParamPresetDlaa, create_preset);
            params_->Set(ngx_abi::kParamPresetUltraQuality, create_preset);
            params_->Set(ngx_abi::kParamPresetQuality, create_preset);
            params_->Set(ngx_abi::kParamPresetBalanced, create_preset);
            params_->Set(ngx_abi::kParamPresetPerformance, create_preset);
            params_->Set(ngx_abi::kParamPresetUltraPerformance, create_preset);
            handle_ = nullptr;
            ngx_abi::Result res = fn_create_feature_(cmd, ngx_abi::kFeatureSuperSampling, params_, &handle_);
            if (!ngx_abi::Succeeded(res)) handle_ = nullptr;
            return res;
        };

        ngx_abi::Result r = try_create(pq, preset_val);
        if ((!ngx_abi::Succeeded(r) || !handle_) && preset_val != ngx_abi::kPresetDefault) {
            // Retry with Auto preset if an older retail DLL doesn't recognize Preset J/K
            r = try_create(pq, ngx_abi::kPresetDefault);
        }
        if (!ngx_abi::Succeeded(r) || !handle_) {
            // Last resort: the plain DLAA / MaxQuality buckets accept any explicit input size on every DLSS build.
            ngx_abi::PerfQuality fb_pq =
                (rw >= ow && rh >= oh) ? ngx_abi::kPerfQualityDlaa : ngx_abi::kPerfQualityMaxQuality;
            if (fb_pq != pq) {
                LOGW("upscale: DLSS CreateFeature(PerfQuality=%d) failed (0x%08X); retrying with PerfQuality=%d",
                     int(pq), r, int(fb_pq));
                r = try_create(fb_pq, ngx_abi::kPresetDefault);
            }
        }
        if (!ngx_abi::Succeeded(r) || !handle_) {
            handle_ = nullptr;
            feature_failed_ = true;
            fail_rw_ = rw;
            fail_rh_ = rh;
            fail_ow_ = ow;
            fail_oh_ = oh;
            fail_pq_ = pq;
            fail_preset_ = preset_val;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DLSS CreateFeature failed (0x%08X)", r);
            detail_ = buf;
            LOGW("upscale: %s (%ux%u -> %ux%u)", detail_.c_str(), rw, rh, ow, oh);
            return false;
        }
        // Key the live feature by the *requested* quality/preset so a fallback-created feature isn't
        // recreated every frame.
        cur_rw_ = rw;
        cur_rh_ = rh;
        cur_ow_ = ow;
        cur_oh_ = oh;
        cur_pq_ = pq;
        cur_preset_ = preset_val;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "DLSS 4 %s (%ux%u -> %ux%u%s)", DlssPresetName(preset_enum), rw, rh, ow, oh,
                      used_programdata_ ? ", ProgramData fallback" : "");
        detail_ = buf;
        LOGR("upscale: DLSS 4 feature created: %s", detail_.c_str());
        return true;
    }

    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE ngx_mod_ = nullptr;
    bool ngx_inited_ = false;
    bool available_ = false;
    bool used_programdata_ = false;
    bool force_reset_ = true;
    std::wstring dlss_dir_w_;
    std::string detail_;

    ngx_abi::PfnD3D12InitExt fn_init_ext_ = nullptr;
    ngx_abi::PfnD3D12Shutdown1 fn_shutdown1_ = nullptr;
    ngx_abi::PfnD3D12Shutdown fn_shutdown_ = nullptr;
    ngx_abi::PfnD3D12GetCapabilityParameters fn_get_caps_ = nullptr;
    ngx_abi::PfnD3D12AllocateParameters fn_alloc_params_ = nullptr;
    ngx_abi::PfnD3D12DestroyParameters fn_destroy_params_ = nullptr;
    ngx_abi::PfnD3D12CreateFeature fn_create_feature_ = nullptr;
    ngx_abi::PfnD3D12ReleaseFeature fn_release_feature_ = nullptr;
    ngx_abi::PfnD3D12EvaluateFeature fn_eval_feature_ = nullptr;

    ngx_abi::Parameter* params_ = nullptr;
    ngx_abi::Handle* handle_ = nullptr;
    uint32_t cur_rw_ = 0, cur_rh_ = 0, cur_ow_ = 0, cur_oh_ = 0;
    ngx_abi::PerfQuality cur_pq_ = ngx_abi::kPerfQualityMaxQuality;
    uint32_t cur_preset_ = ngx_abi::kPresetK;

    // Last failed CreateFeature configuration (cleared on success, Shutdown, or any key change).
    bool feature_failed_ = false;
    uint32_t fail_rw_ = 0, fail_rh_ = 0, fail_ow_ = 0, fail_oh_ = 0;
    ngx_abi::PerfQuality fail_pq_ = ngx_abi::kPerfQualityMaxQuality;
    uint32_t fail_preset_ = ngx_abi::kPresetDefault;
};

}  // namespace

std::unique_ptr<ITemporalBackend> CreateDlssBackend() { return std::make_unique<DlssBackend>(); }

}  // namespace upscale

#endif  // _WIN32
