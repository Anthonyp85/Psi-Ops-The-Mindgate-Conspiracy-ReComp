// SPDX-License-Identifier: GPL-2.0-or-later
// Win64 D3D12 9-Upscaler Manager implementing gpu::Upscaler:
// 6 zero-DLL spatial upscalers (FSR 1.0, NIS v1.0.3, CAS, Bicubic 5-Tap, Integer, Bilinear)
// + 3 retail temporal backends (DLSS 4, FSR 3.1, XeSS 2) with Halton (2,3) jitter,
// RG16F camera motion vectors, R8_UNORM HUD reactive mask, camera-cut reset,
// and seamless FSR 1.0 fallback for menus/FMVs/loading or unavailable DLLs.

#ifdef _WIN32

#include "upscale/upscaler_manager.h"
#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"

namespace upscale {

namespace {

using Microsoft::WRL::ComPtr;

class D3D12UpscalerManager final : public gpu::Upscaler {
public:
    ~D3D12UpscalerManager() override { Shutdown(); }

    bool Init(const gpu::UpscalerInitDesc& desc) override {
        Shutdown();
        dev_ = static_cast<ID3D12Device*>(desc.device);
        queue_ = static_cast<ID3D12CommandQueue*>(desc.queue);
        caps_ = desc.caps;
        host_ = desc.host;
        if (!dev_) return false;

        if (!spatial_.Init(dev_, host_)) {
            LOGE("upscale: SpatialD3D12 init failed");
            return false;
        }

        dlss_ = CreateDlssBackend();
        fsr3_ = CreateFsr3Backend();
        xess_ = CreateXessBackend();
        dlss_tried_ = false;
        fsr3_tried_ = false;
        xess_tried_ = false;

        // Fast presence probe for UI badges; initialize only the requested temporal backend at boot
        // (other temporal backends initialize lazily on demand if switched live in the F1 overlay).
        PluginProbe probe = ProbePlugins(false);
        dlss_avail_hint_ = probe.dlss_ready && (caps_.vendor_id == 0x10DEu);
        fsr3_avail_hint_ = probe.fsr3_ready;
        xess_avail_hint_ = probe.xess_ready;

        const app::UpscalerMode boot_req = app::Get().upscaler;
        if (IsTemporalMode(boot_req)) {
            EnsureBackendInitialized(boot_req);
        }

        {
            std::lock_guard<std::mutex> lk(status_lock_);
            status_ = gpu::UpscalerStatus{};
            status_.dlss_available = dlss_tried_ ? (dlss_ && dlss_->IsAvailable()) : dlss_avail_hint_;
            status_.fsr3_available = fsr3_tried_ ? (fsr3_ && fsr3_->IsAvailable()) : fsr3_avail_hint_;
            status_.xess_available = xess_tried_ ? (xess_ && xess_->IsAvailable()) : xess_avail_hint_;
            status_.requested_mode = uint32_t(boot_req);
            status_.active_mode = status_.requested_mode;
            status_.active_label = ModeShortName(boot_req);
        }

        inited_ = true;
        LOGR("upscale: D3D12 manager ready (adapter=%s, dlss=%d, fsr3=%d, xess=%d, boot=%s)",
             caps_.adapter_name.c_str(), status_.dlss_available ? 1 : 0, status_.fsr3_available ? 1 : 0,
             status_.xess_available ? 1 : 0, ModeShortName(boot_req));
        return true;
    }

    void Shutdown() override {
        if (host_.flush && inited_) host_.flush(host_.ctx);
        ReleaseTemporalTargets();
        if (dlss_) dlss_->Shutdown();
        if (fsr3_) fsr3_->Shutdown();
        if (xess_) xess_->Shutdown();
        dlss_.reset();
        fsr3_.reset();
        xess_.reset();
        spatial_.Shutdown();
        inited_ = false;
        has_prev_cam_ = false;
        has_prev_vp_ = false;
        dev_ = nullptr;
        queue_ = nullptr;
    }

    void ResolveRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, bool /*widescreen*/,
                           uint32_t& out_render_w, uint32_t& out_render_h) override {
        const app::Settings& s = app::Get();
        app::UpscalerMode eff_mode = s.upscaler;
        if (IsTemporalMode(eff_mode)) {
            bool avail = (eff_mode == app::UpscalerMode::Dlss)
                             ? (dlss_tried_ ? (dlss_ && dlss_->IsAvailable()) : dlss_avail_hint_)
                         : (eff_mode == app::UpscalerMode::Fsr3)
                             ? (fsr3_tried_ ? (fsr3_ && fsr3_->IsAvailable()) : fsr3_avail_hint_)
                             : (xess_tried_ ? (xess_ && xess_->IsAvailable()) : xess_avail_hint_);
            if (!avail) eff_mode = app::UpscalerMode::Fsr1;
        }
        ComputeRenderSize(vp_w, vp_h, guest_w, guest_h, eff_mode, s.upscale_quality, s.render_percent,
                          s.aspect_correct_render, out_render_w, out_render_h);
    }

    void BeginFrame(uint32_t render_w, uint32_t render_h, uint32_t vp_w, uint32_t vp_h,
                    const gpu::CameraFrameData& cam, bool& out_jitter_active, float& out_jitter_x,
                    float& out_jitter_y) override {
        out_jitter_active = false;
        out_jitter_x = 0.0f;
        out_jitter_y = 0.0f;
        if (!inited_) return;

        const app::Settings& s = app::Get();
        if (!IsTemporalMode(s.upscaler)) return;
        EnsureBackendInitialized(s.upscaler);
        if (!BackendFor(s.upscaler)) return;
        if (!cam.valid || !cam.is_3d_gameplay) return;

        ComputeJitter(jitter_phase_++, render_w, vp_w, out_jitter_x, out_jitter_y);
        out_jitter_active = true;
    }

    bool Execute(const gpu::UpscalerFrameDesc& fd) override {
        if (!inited_ || !fd.cmd_list || !fd.front_res || !fd.backbuffer_res) return false;

        const app::Settings& s = app::Get();
        const app::UpscalerMode req = s.upscaler;
        const float sharp = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);

        app::UpscalerMode active = req;
        bool temporal_used = false;
        bool fallback = false;
        bool bypass_2d = false;
        std::string detail;

        if (IsTemporalMode(req)) {
            EnsureBackendInitialized(req);
            ITemporalBackend* tb = BackendFor(req);
            bool scene_ok = fd.camera.valid && fd.camera.is_3d_gameplay && fd.scene_depth_res != nullptr &&
                            fd.render_w <= fd.vp_w && fd.render_h <= fd.vp_h;
            bypass_2d = (tb != nullptr) && !scene_ok;
            if (tb && scene_ok && EnsureTemporalTargets(fd.render_w, fd.render_h, fd.vp_w, fd.vp_h)) {
                bool cut = force_reset_ || !has_prev_cam_ || IsCameraCut(prev_cam_, fd.camera);
                if (cut) {
                    has_prev_vp_ = false;
                    force_reset_ = false;
                }

                float cur_vp[16];
                float cur_vp_inv[16];
                MulMatrix4x4(fd.camera.proj_rev, fd.camera.view, cur_vp);
                bool inv_ok = InvertMatrix4x4(cur_vp, cur_vp_inv);

                auto* cmd = static_cast<ID3D12GraphicsCommandList*>(fd.cmd_list);
                auto* front = static_cast<ID3D12Resource*>(fd.front_res);
                auto* sc = static_cast<ID3D12Resource*>(fd.scene_color_res);
                auto* depth = static_cast<ID3D12Resource*>(fd.scene_depth_res);

                D3D12_RESOURCE_STATES front_st = static_cast<D3D12_RESOURCE_STATES>(*fd.front_state);
                D3D12_RESOURCE_STATES sc_st =
                    fd.scene_color_state ? static_cast<D3D12_RESOURCE_STATES>(*fd.scene_color_state)
                                         : D3D12_RESOURCE_STATE_COMMON;
                D3D12_RESOURCE_STATES depth_st = static_cast<D3D12_RESOURCE_STATES>(*fd.scene_depth_state);

                bool prep_ok = spatial_.BuildTemporalInputs(
                    cmd, front, front_st, static_cast<DXGI_FORMAT>(fd.front_fmt), sc,
                    fd.scene_color_state ? &sc_st : nullptr, static_cast<DXGI_FORMAT>(fd.scene_color_fmt), depth,
                    depth_st, mv_tex_.Get(), mv_state_, reactive_tex_.Get(), reactive_state_, depth_r32f_tex_.Get(),
                    depth_r32f_state_, fd.render_w, fd.render_h, cur_vp_inv, prev_vp_, has_prev_vp_ && inv_ok && !cut,
                    fd.jitter_active ? fd.jitter_x : 0.0f, fd.jitter_active ? fd.jitter_y : 0.0f);

                if (prep_ok) {
                    TemporalExecDesc td{};
                    td.cmd = cmd;
                    td.color = front;
                    td.color_state = &front_st;
                    td.color_fmt = static_cast<DXGI_FORMAT>(fd.front_fmt);
                    td.depth = depth_r32f_tex_.Get();
                    td.depth_state = &depth_r32f_state_;
                    td.motion_vectors = mv_tex_.Get();
                    td.mv_state = &mv_state_;
                    td.reactive_mask = reactive_tex_.Get();
                    td.reactive_state = &reactive_state_;
                    td.output = temporal_out_tex_.Get();
                    td.output_state = &temporal_out_state_;
                    td.render_w = fd.render_w;
                    td.render_h = fd.render_h;
                    td.out_w = fd.vp_w;
                    td.out_h = fd.vp_h;
                    td.jitter_x = fd.jitter_active ? fd.jitter_x : 0.0f;
                    td.jitter_y = fd.jitter_active ? fd.jitter_y : 0.0f;
                    td.sharpness = sharp;
                    td.dt_ms = fd.camera.dt_ms > 0.5f ? fd.camera.dt_ms : 16.667f;
                    td.near_plane = fd.camera.near_plane > 1e-4f ? fd.camera.near_plane : 0.1f;
                    td.far_plane = 1000.0f;
                    td.fov_y_rad = fd.camera.fov_y_rad > 0.1f ? fd.camera.fov_y_rad : 1.0f;
                    td.reset = cut;
                    td.quality = s.upscale_quality;
                    td.dlss_preset = s.dlss_preset;

                    if (tb->Execute(td)) {
                        *fd.front_state = uint32_t(front_st);
                        if (fd.scene_color_state) *fd.scene_color_state = uint32_t(sc_st);
                        if (fd.scene_depth_state) *fd.scene_depth_state = uint32_t(depth_st);

                        // FSR 3.1 applies its own RCAS pass; DLSS 4 and XeSS 2 get optional host RCAS sharpening
                        float post_sharp = (req == app::UpscalerMode::Fsr3) ? 0.0f : sharp;
                        if (spatial_.CompositeOutput(temporal_out_tex_.Get(), temporal_out_state_,
                                                     DXGI_FORMAT_R8G8B8A8_UNORM, post_sharp, fd)) {
                            temporal_used = true;
                            detail = tb->StatusDetail();
                            if (inv_ok) {
                                std::memcpy(prev_vp_, cur_vp, sizeof(prev_vp_));
                                has_prev_vp_ = true;
                            }
                            prev_cam_ = fd.camera;
                            has_prev_cam_ = true;
                        }
                    } else {
                        *fd.front_state = uint32_t(front_st);
                        if (fd.scene_color_state) *fd.scene_color_state = uint32_t(sc_st);
                        if (fd.scene_depth_state) *fd.scene_depth_state = uint32_t(depth_st);
                        detail = tb->StatusDetail();
                    }
                } else {
                    *fd.front_state = uint32_t(front_st);
                    if (fd.scene_color_state) *fd.scene_color_state = uint32_t(sc_st);
                    if (fd.scene_depth_state) *fd.scene_depth_state = uint32_t(depth_st);
                }
            } else {
                has_prev_cam_ = false;
                has_prev_vp_ = false;
                if (!tb) {
                    detail = UnavailableReason(req);
                } else if (bypass_2d) {
                    detail = std::string(ModeShortName(req)) + " ready - 2D/menu/FMV frames use FSR 1.0 spatial pass";
                } else {
                    detail = "Temporal target allocation failed -> FSR 1.0";
                }
            }

            if (!temporal_used) {
                active = app::UpscalerMode::Fsr1;
                fallback = (tb == nullptr) || (scene_ok && tb != nullptr);
                if (!spatial_.Execute(app::UpscalerMode::Fsr1, sharp, fd)) return false;
            } else {
                ever_temporal_active_ = true;
                last_temporal_req_ = req;
            }
        } else {
            ever_temporal_active_ = false;
            has_prev_cam_ = false;
            has_prev_vp_ = false;
            if (!spatial_.Execute(req, sharp, fd)) return false;
            detail = ModeName(req);
        }

        // Keep reported active backend anchored to the live temporal backend during brief 2D/shutdown frames
        if (IsTemporalMode(req) && !temporal_used && !fallback && ever_temporal_active_ && last_temporal_req_ == req) {
            active = req;
        }

        UpdateStatus(req, active, fd, temporal_used, fallback, detail,
                     BuildHudLabel(req, active, fd, temporal_used, bypass_2d, fallback, s.upscale_quality));
        return true;
    }

    void Prewarm(void* cmd_list, uint32_t render_w, uint32_t render_h, uint32_t vp_w, uint32_t vp_h) override {
        if (!inited_) return;
        spatial_.Prewarm(vp_w, vp_h);
        const app::Settings& s = app::Get();
        if (!IsTemporalMode(s.upscaler)) return;
        EnsureBackendInitialized(s.upscaler);
        ITemporalBackend* tb = BackendFor(s.upscaler);
        if (!tb) return;
        if (render_w == 0 || render_h == 0 || vp_w == 0 || vp_h == 0 || render_w > vp_w || render_h > vp_h) return;
        if (!EnsureTemporalTargets(render_w, render_h, vp_w, vp_h)) return;
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(cmd_list);
        if (cmd && tb->Prewarm(cmd, render_w, render_h, vp_w, vp_h, s.upscale_quality, s.dlss_preset)) {
            // Execute one dummy temporal evaluation at boot so retail upscaler drivers (e.g. DLSS 4 NGX)
            // JIT their internal compute/tensor pipelines during startup instead of on frame 1 of 3D gameplay.
            ComPtr<ID3D12Resource> dummy_color;
            D3D12_RESOURCE_STATES dummy_state = D3D12_RESOURCE_STATE_COMMON;
            if (MakeTex2D(DXGI_FORMAT_B8G8R8A8_UNORM, render_w, render_h, D3D12_RESOURCE_FLAG_NONE, dummy_color,
                          dummy_state)) {
                TemporalExecDesc td{};
                td.cmd = cmd;
                td.color = dummy_color.Get();
                td.color_state = &dummy_state;
                td.color_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.depth = depth_r32f_tex_.Get();
                td.depth_state = &depth_r32f_state_;
                td.motion_vectors = mv_tex_.Get();
                td.mv_state = &mv_state_;
                td.reactive_mask = reactive_tex_.Get();
                td.reactive_state = &reactive_state_;
                td.output = temporal_out_tex_.Get();
                td.output_state = &temporal_out_state_;
                td.render_w = render_w;
                td.render_h = render_h;
                td.out_w = vp_w;
                td.out_h = vp_h;
                td.sharpness = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);
                td.dt_ms = 16.667f;
                td.near_plane = 0.1f;
                td.far_plane = 1000.0f;
                td.fov_y_rad = 1.0f;
                td.reset = true;
                td.quality = s.upscale_quality;
                td.dlss_preset = s.dlss_preset;
                tb->Execute(td);
                if (host_.flush) host_.flush(host_.ctx);
                tb->ResetHistory();
                force_reset_ = true;
            }
            std::lock_guard<std::mutex> lk(status_lock_);
            status_.dlss_available = dlss_tried_ ? (dlss_ && dlss_->IsAvailable()) : dlss_avail_hint_;
            status_.fsr3_available = fsr3_tried_ ? (fsr3_ && fsr3_->IsAvailable()) : fsr3_avail_hint_;
            status_.xess_available = xess_tried_ ? (xess_ && xess_->IsAvailable()) : xess_avail_hint_;
            status_.render_w = render_w;
            status_.render_h = render_h;
            status_.vp_w = vp_w;
            status_.vp_h = vp_h;
            status_.status_detail = tb->StatusDetail();
            LOGR("upscale: temporal feature prewarmed (%s, %ux%u -> %ux%u, detail=%s)",
                 ModeShortName(s.upscaler), render_w, render_h, vp_w, vp_h, status_.status_detail.c_str());
        }
    }

    void ResetHistory() override {
        force_reset_ = true;
        has_prev_cam_ = false;
        has_prev_vp_ = false;
        jitter_phase_ = 0;
        if (dlss_) dlss_->ResetHistory();
        if (fsr3_) fsr3_->ResetHistory();
        if (xess_) xess_->ResetHistory();
    }

    gpu::UpscalerStatus GetStatus() const override {
        std::lock_guard<std::mutex> lk(status_lock_);
        return status_;
    }

private:
    void EnsureBackendInitialized(app::UpscalerMode m) {
        if (m == app::UpscalerMode::Dlss && !dlss_tried_) {
            dlss_tried_ = true;
            if (dlss_) dlss_->Init(dev_, queue_, caps_, host_);
        } else if (m == app::UpscalerMode::Fsr3 && !fsr3_tried_) {
            fsr3_tried_ = true;
            if (fsr3_) fsr3_->Init(dev_, queue_, caps_, host_);
        } else if (m == app::UpscalerMode::Xess && !xess_tried_) {
            xess_tried_ = true;
            if (xess_) xess_->Init(dev_, queue_, caps_, host_);
        }
    }

    ITemporalBackend* BackendFor(app::UpscalerMode m) const {
        if (m == app::UpscalerMode::Dlss && dlss_ && dlss_->IsAvailable()) return dlss_.get();
        if (m == app::UpscalerMode::Fsr3 && fsr3_ && fsr3_->IsAvailable()) return fsr3_.get();
        if (m == app::UpscalerMode::Xess && xess_ && xess_->IsAvailable()) return xess_.get();
        return nullptr;
    }

    std::string UnavailableReason(app::UpscalerMode m) const {
        if (m == app::UpscalerMode::Dlss && dlss_) return dlss_->StatusDetail();
        if (m == app::UpscalerMode::Fsr3 && fsr3_) return fsr3_->StatusDetail();
        if (m == app::UpscalerMode::Xess && xess_) return xess_->StatusDetail();
        return "plugin unavailable -> FSR 1.0";
    }

    void ReleaseTemporalTargets() {
        mv_tex_.Reset();
        reactive_tex_.Reset();
        depth_r32f_tex_.Reset();
        temporal_out_tex_.Reset();
        t_rw_ = t_rh_ = t_ow_ = t_oh_ = 0;
    }

    bool MakeTex2D(DXGI_FORMAT fmt, uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags,
                   ComPtr<ID3D12Resource>& out_res, D3D12_RESOURCE_STATES& out_st) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = flags;
        out_st = D3D12_RESOURCE_STATE_COMMON;
        return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, out_st, nullptr,
                                                       IID_PPV_ARGS(&out_res)));
    }

    bool EnsureTemporalTargets(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
        if (mv_tex_ && reactive_tex_ && depth_r32f_tex_ && temporal_out_tex_ && t_rw_ == rw && t_rh_ == rh &&
            t_ow_ == ow && t_oh_ == oh) {
            return true;
        }
        if ((mv_tex_ || temporal_out_tex_) && host_.flush) host_.flush(host_.ctx);
        ReleaseTemporalTargets();
        bool ok = MakeTex2D(DXGI_FORMAT_R16G16_FLOAT, rw, rh, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, mv_tex_,
                            mv_state_) &&
                  MakeTex2D(DXGI_FORMAT_R8_UNORM, rw, rh, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, reactive_tex_,
                            reactive_state_) &&
                  MakeTex2D(DXGI_FORMAT_R32_FLOAT, rw, rh, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, depth_r32f_tex_,
                            depth_r32f_state_) &&
                  MakeTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, ow, oh,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                            temporal_out_tex_, temporal_out_state_);
        if (!ok) {
            ReleaseTemporalTargets();
            return false;
        }
        t_rw_ = rw;
        t_rh_ = rh;
        t_ow_ = ow;
        t_oh_ = oh;
        return true;
    }

    // Live HUD / F1 overlay label: says whether the temporal upscaler is actually running, running as native-res
    // anti-aliasing (DLAA / Native AA), idling on a 2D/menu/FMV frame (FSR 1.0 spatial pass), or fell back.
    static std::string BuildHudLabel(app::UpscalerMode req, app::UpscalerMode active, const gpu::UpscalerFrameDesc& fd,
                                     bool temporal, bool bypass_2d, bool fallback, app::UpscaleQuality quality) {
        if (!IsTemporalMode(req)) return ModeShortName(active);
        const bool native_aa = (quality == app::UpscaleQuality::NativeAA) ||
                               (fd.render_w + 1u >= fd.vp_w && fd.render_h + 1u >= fd.vp_h);
        const char* aa_tag = (req == app::UpscalerMode::Dlss) ? "DLAA" : "Native AA";
        std::string label = ModeShortName(req);
        if (temporal) {
            if (native_aa) label += std::string(" (") + aa_tag + ")";
            return label;
        }
        if (bypass_2d) {
            label += native_aa ? std::string(" (") + aa_tag + ", 2D)" : std::string(" (2D)");
            return label;
        }
        if (fallback) return "FSR 1.0 (Fallback)";
        return ModeShortName(active);
    }

    void UpdateStatus(app::UpscalerMode req, app::UpscalerMode active, const gpu::UpscalerFrameDesc& fd, bool temporal,
                      bool fallback, const std::string& detail, std::string label) {
        std::lock_guard<std::mutex> lk(status_lock_);
        // Only emit LOGR on genuine mode/fallback/resolution changes, not transient 2D/menu/FMV temporal toggles.
        bool changed = (status_.requested_mode != uint32_t(req) || status_.active_mode != uint32_t(active) ||
                        status_.fallback_active != fallback || status_.render_w != fd.render_w ||
                        status_.render_h != fd.render_h || status_.vp_w != fd.vp_w || status_.vp_h != fd.vp_h);
        status_.requested_mode = uint32_t(req);
        status_.active_mode = uint32_t(active);
        status_.render_w = fd.render_w;
        status_.render_h = fd.render_h;
        status_.vp_w = fd.vp_w;
        status_.vp_h = fd.vp_h;
        status_.bb_w = fd.bb_w;
        status_.bb_h = fd.bb_h;
        status_.temporal_active = temporal;
        status_.fallback_active = fallback;
        status_.dlss_available = dlss_tried_ ? (dlss_ && dlss_->IsAvailable()) : dlss_avail_hint_;
        status_.fsr3_available = fsr3_tried_ ? (fsr3_ && fsr3_->IsAvailable()) : fsr3_avail_hint_;
        status_.xess_available = xess_tried_ ? (xess_ && xess_->IsAvailable()) : xess_avail_hint_;
        status_.active_label = std::move(label);
        status_.status_detail = detail;
        if (changed) {
            LOGR("upscale: active=%s (req=%s, temporal=%d, fallback=%d, %ux%u -> %ux%u) %s",
                 status_.active_label.c_str(), ModeShortName(req), temporal ? 1 : 0, fallback ? 1 : 0, fd.render_w,
                 fd.render_h, fd.vp_w, fd.vp_h, detail.c_str());
        }
    }

    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    gpu::GpuCaps caps_{};
    gpu::UpscalerHostOps host_{};
    SpatialD3D12 spatial_;
    std::unique_ptr<ITemporalBackend> dlss_;
    std::unique_ptr<ITemporalBackend> fsr3_;
    std::unique_ptr<ITemporalBackend> xess_;

    ComPtr<ID3D12Resource> mv_tex_;
    ComPtr<ID3D12Resource> reactive_tex_;
    ComPtr<ID3D12Resource> depth_r32f_tex_;
    ComPtr<ID3D12Resource> temporal_out_tex_;
    D3D12_RESOURCE_STATES mv_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES reactive_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depth_r32f_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES temporal_out_state_ = D3D12_RESOURCE_STATE_COMMON;
    uint32_t t_rw_ = 0, t_rh_ = 0, t_ow_ = 0, t_oh_ = 0;

    bool inited_ = false;
    bool dlss_tried_ = false;
    bool fsr3_tried_ = false;
    bool xess_tried_ = false;
    bool dlss_avail_hint_ = false;
    bool fsr3_avail_hint_ = false;
    bool xess_avail_hint_ = false;
    bool force_reset_ = true;
    bool has_prev_cam_ = false;
    bool has_prev_vp_ = false;
    bool ever_temporal_active_ = false;
    app::UpscalerMode last_temporal_req_ = app::UpscalerMode::Fsr1;
    gpu::CameraFrameData prev_cam_{};
    float prev_vp_[16] = {};
    uint32_t jitter_phase_ = 0;

    mutable std::mutex status_lock_;
    gpu::UpscalerStatus status_{};
};

}  // namespace

gpu::Upscaler* GetManager() {
    static D3D12UpscalerManager mgr;
    return &mgr;
}

void RegisterUpscaler() { gpu::SetUpscaler(GetManager()); }

}  // namespace upscale

#endif  // _WIN32
