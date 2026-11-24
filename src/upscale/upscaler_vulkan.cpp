// SPDX-License-Identifier: GPL-2.0-or-later
// Native Linux Vulkan 1.2 Spatial Upscaler Suite implementing gpu::Upscaler:
// 6 zero-DLL spatial upscalers (FSR 1.0 EASU+RCAS, NIS v1.0.3, CAS, Bicubic 5-Tap incl. SSAA,
// Integer, Bilinear) compiled via VulkanBackend's glslang HLSL->SPIR-V pipeline +
// graceful automatic fallback to FSR 1.0 when a Win64-only temporal mode is requested.

#ifndef _WIN32

#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "upscale/upscaler_common.h"
#include "upscale/upscaler_manager.h"

namespace upscale {

namespace {

struct SpatialConstants {
    float src_w, src_h, inv_src_w, inv_src_h;
    float dst_w, dst_h, inv_dst_w, inv_dst_h;
    float sharpness;
    float gamma_on;
    float mode_param0;
    float mode_param1;
};

// Mirrors D3D12 spatial HLSL compiled by VulkanBackend::CompileHlslToSpirv:
// b0 -> binding 0 (UBO), t0..t1 -> bindings 4..5 (SAMPLED_IMAGE), s0..s1 -> bindings 12..13 (SAMPLER).
const char kVkSpatialHlsl[] = R"HLSL(
cbuffer SpatialCB : register(b0) {
    float4 SrcInfo;
    float4 DstInfo;
    float4 Params;
};

Texture2D<float4> SrcTex : register(t0);
Texture2D<float4> LutTex : register(t1);
SamplerState SmpLinear : register(s0);
SamplerState SmpPoint  : register(s1);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut vs_fullscreen(uint vid : SV_VertexID) {
    VsOut o;
    float2 p = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? -3.0 : 1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv  = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}

float3 ApplyGammaLut(float3 c) {
    if (Params.y < 0.5) return saturate(c);
    float3 sc = saturate(c);
    float r = LutTex.SampleLevel(SmpLinear, float2((sc.r * 255.0 + 0.5) / 256.0, 0.5), 0).r;
    float g = LutTex.SampleLevel(SmpLinear, float2((sc.g * 255.0 + 0.5) / 256.0, 0.5), 0).g;
    float b = LutTex.SampleLevel(SmpLinear, float2((sc.b * 255.0 + 0.5) / 256.0, 0.5), 0).b;
    return float3(r, g, b);
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 ps_bilinear(VsOut i) : SV_Target {
    return float4(ApplyGammaLut(SrcTex.SampleLevel(SmpLinear, i.uv, 0).rgb), 1.0);
}

float4 ps_integer(VsOut i) : SV_Target {
    return float4(ApplyGammaLut(SrcTex.SampleLevel(SmpPoint, i.uv, 0).rgb), 1.0);
}

float3 SampleBicubic5Tap(float2 uv) {
    float2 texPos = uv * SrcInfo.xy - 0.5;
    float2 tc1 = floor(texPos) + 0.5;
    float2 f = texPos - floor(texPos);
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;

    float2 w12 = w1 + w2;
    float2 tc0 = (tc1 - 1.0) * SrcInfo.zw;
    float2 tc3 = (tc1 + 2.0) * SrcInfo.zw;
    float2 tc12 = (tc1 + w2 / max(w12, 1e-5)) * SrcInfo.zw;

    float3 result = 0.0;
    float weightSum = 0.0;
    float wt = w12.x * w0.y;
    result += SrcTex.SampleLevel(SmpLinear, float2(tc12.x, tc0.y), 0).rgb * wt; weightSum += wt;
    wt = w0.x * w12.y;
    result += SrcTex.SampleLevel(SmpLinear, float2(tc0.x, tc12.y), 0).rgb * wt; weightSum += wt;
    wt = w12.x * w12.y;
    result += SrcTex.SampleLevel(SmpLinear, float2(tc12.x, tc12.y), 0).rgb * wt; weightSum += wt;
    wt = w3.x * w12.y;
    result += SrcTex.SampleLevel(SmpLinear, float2(tc3.x, tc12.y), 0).rgb * wt; weightSum += wt;
    wt = w12.x * w3.y;
    result += SrcTex.SampleLevel(SmpLinear, float2(tc12.x, tc3.y), 0).rgb * wt; weightSum += wt;
    return saturate(result / max(weightSum, 1e-5));
}

float4 ps_bicubic(VsOut i) : SV_Target {
    return float4(ApplyGammaLut(SampleBicubic5Tap(i.uv)), 1.0);
}

float4 ps_cas(VsOut i) : SV_Target {
    float2 d = SrcInfo.zw;
    float3 a = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x, -d.y), 0).rgb;
    float3 b = SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0, -d.y), 0).rgb;
    float3 c = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x, -d.y), 0).rgb;
    float3 d0 = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  0.0), 0).rgb;
    float3 e = SrcTex.SampleLevel(SmpLinear, i.uv, 0).rgb;
    float3 f = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  0.0), 0).rgb;
    float3 g = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  d.y), 0).rgb;
    float3 h = SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0,  d.y), 0).rgb;
    float3 k = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  d.y), 0).rgb;

    float3 mn = min(min(min(d0, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d0, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));

    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(Params.x));
    float3 w = amp * peak;
    float3 outC = saturate(((b + d0 + f + h) * w + e) / (1.0 + 4.0 * w));
    return float4(ApplyGammaLut(outC), 1.0);
}

void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * -dir.y) + (off.y * dir.x);
    v *= len;
    float d2 = min(dot(v, v), clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
                float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    float lenX = max(abs(lD - lC), abs(lC - lB));
    lenX = (lenX > 1e-5) ? (1.0 / lenX) : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += (lenX * lenX) * w;

    float lenY = max(abs(lE - lC), abs(lC - lA));
    lenY = (lenY > 1e-5) ? (1.0 / lenY) : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += (lenY * lenY) * w;
}

float4 ps_fsr1_easu(VsOut i) : SV_Target {
    float2 pp = i.uv * SrcInfo.xy - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    int2 base = int2(fp);
    int2 maxC = int2(SrcInfo.xy) - 1;

    float3 b  = SrcTex.Load(int3(clamp(base + int2( 0, -1), 0, maxC), 0)).rgb;
    float3 c  = SrcTex.Load(int3(clamp(base + int2( 1, -1), 0, maxC), 0)).rgb;
    float3 e  = SrcTex.Load(int3(clamp(base + int2(-1,  0), 0, maxC), 0)).rgb;
    float3 f  = SrcTex.Load(int3(clamp(base + int2( 0,  0), 0, maxC), 0)).rgb;
    float3 g  = SrcTex.Load(int3(clamp(base + int2( 1,  0), 0, maxC), 0)).rgb;
    float3 h  = SrcTex.Load(int3(clamp(base + int2( 2,  0), 0, maxC), 0)).rgb;
    float3 i0 = SrcTex.Load(int3(clamp(base + int2(-1,  1), 0, maxC), 0)).rgb;
    float3 j  = SrcTex.Load(int3(clamp(base + int2( 0,  1), 0, maxC), 0)).rgb;
    float3 k  = SrcTex.Load(int3(clamp(base + int2( 1,  1), 0, maxC), 0)).rgb;
    float3 l  = SrcTex.Load(int3(clamp(base + int2( 2,  1), 0, maxC), 0)).rgb;
    float3 n  = SrcTex.Load(int3(clamp(base + int2( 0,  2), 0, maxC), 0)).rgb;
    float3 o  = SrcTex.Load(int3(clamp(base + int2( 1,  2), 0, maxC), 0)).rgb;

    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSet(dir, len, pp, true,  false, false, false, Luma(b), Luma(e),  Luma(f), Luma(g), Luma(j));
    FsrEasuSet(dir, len, pp, false, true,  false, false, Luma(c), Luma(f),  Luma(g), Luma(h), Luma(k));
    FsrEasuSet(dir, len, pp, false, false, true,  false, Luma(f), Luma(i0), Luma(j), Luma(k), Luma(n));
    FsrEasuSet(dir, len, pp, false, false, false, true,  Luma(g), Luma(j),  Luma(k), Luma(l), Luma(o));

    float dirR = dot(dir, dir);
    dir = (dirR < (1.0 / 32768.0)) ? float2(1.0, 0.0) : (dir * rsqrt(dirR));
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float3 min4 = min(min(f, g), min(j, k));
    float3 max4 = max(max(f, g), max(j, k));
    float3 aC = 0.0;
    float aW = 0.0;
    FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, i0);
    FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
    return float4(saturate(clamp(aC / max(aW, 1e-5), min4, max4)), 1.0);
}

float4 ps_fsr1_rcas(VsOut i) : SV_Target {
    int2 p = int2(i.uv * SrcInfo.xy);
    int2 maxC = int2(SrcInfo.xy) - 1;
    float3 b = SrcTex.Load(int3(clamp(p + int2( 0, -1), 0, maxC), 0)).rgb;
    float3 d = SrcTex.Load(int3(clamp(p + int2(-1,  0), 0, maxC), 0)).rgb;
    float3 e = SrcTex.Load(int3(clamp(p,                0, maxC), 0)).rgb;
    float3 f = SrcTex.Load(int3(clamp(p + int2( 1,  0), 0, maxC), 0)).rgb;
    float3 h = SrcTex.Load(int3(clamp(p + int2( 0,  1), 0, maxC), 0)).rgb;

    float bL = Luma(b), dL = Luma(d), eL = Luma(e), fL = Luma(f), hL = Luma(h);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float mxL = max(max(bL, dL), max(eL, max(fL, hL)));
    float mnL = min(min(bL, dL), min(eL, min(fL, hL)));
    nz = -0.5 * saturate(abs(nz) / max(mxL - mnL, 1e-5)) + 1.0;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * saturate(Params.x) * nz;
    float3 outC = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    return float4(ApplyGammaLut(outC), 1.0);
}

float4 ps_nis(VsOut i) : SV_Target {
    float3 baseC = SampleBicubic5Tap(i.uv);
    float2 d = SrcInfo.zw;
    float lC = Luma(baseC);
    float lN  = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0, -d.y), 0).rgb);
    float lS  = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0,  d.y), 0).rgb);
    float lW  = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  0.0), 0).rgb);
    float lE  = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  0.0), 0).rgb);
    float lNW = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x, -d.y), 0).rgb);
    float lNE = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x, -d.y), 0).rgb);
    float lSW = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  d.y), 0).rgb);
    float lSE = Luma(SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  d.y), 0).rgb);

    float edge = saturate(max(max(abs(lE - lW), abs(lS - lN)), max(abs(lNE - lSW), abs(lSE - lNW))) * 2.5);
    float blur = (lN + lS + lW + lE) * 0.15 + (lNW + lNE + lSW + lSE) * 0.10;
    float delta = clamp((lC - blur) * saturate(Params.x) * lerp(0.45, 1.15, edge), -0.18, 0.18);
    return float4(ApplyGammaLut(saturate(baseC + delta)), 1.0);
}
)HLSL";

struct VkFns {
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    PFN_vkCreateDescriptorSetLayout create_dsl = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_dsl = nullptr;
    PFN_vkCreatePipelineLayout create_pl = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pl = nullptr;
    PFN_vkCreateDescriptorPool create_dp = nullptr;
    PFN_vkDestroyDescriptorPool destroy_dp = nullptr;
    PFN_vkAllocateDescriptorSets alloc_ds = nullptr;
    PFN_vkUpdateDescriptorSets update_ds = nullptr;
    PFN_vkCreateSampler create_sampler = nullptr;
    PFN_vkDestroySampler destroy_sampler = nullptr;
    PFN_vkCreateRenderPass create_rp = nullptr;
    PFN_vkDestroyRenderPass destroy_rp = nullptr;
    PFN_vkCreateImage create_img = nullptr;
    PFN_vkDestroyImage destroy_img = nullptr;
    PFN_vkGetImageMemoryRequirements get_img_mem_req = nullptr;
    PFN_vkAllocateMemory alloc_mem = nullptr;
    PFN_vkFreeMemory free_mem = nullptr;
    PFN_vkBindImageMemory bind_img_mem = nullptr;
    PFN_vkCreateImageView create_img_view = nullptr;
    PFN_vkDestroyImageView destroy_img_view = nullptr;
    PFN_vkCreateFramebuffer create_fb = nullptr;
    PFN_vkDestroyFramebuffer destroy_fb = nullptr;
    PFN_vkDestroyPipeline destroy_pipe = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_rp = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_rp = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipe = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_ds = nullptr;
    PFN_vkCmdSetViewport cmd_set_vp = nullptr;
    PFN_vkCmdSetScissor cmd_set_sc = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;
};

class VulkanUpscalerManager final : public gpu::Upscaler {
public:
    ~VulkanUpscalerManager() override { Shutdown(); }

    bool Init(const gpu::UpscalerInitDesc& desc) override {
        Shutdown();
        dev_ = reinterpret_cast<VkDevice>(desc.device);
        phys_ = reinterpret_cast<VkPhysicalDevice>(desc.queue);
        host_ = desc.host;
        inst_ = host_.get_pipeline_library
                    ? reinterpret_cast<VkInstance>(host_.get_pipeline_library(host_.ctx))
                    : VK_NULL_HANDLE;
        if (!dev_ || !phys_ || !inst_ || !host_.compile || !host_.upload || !host_.transition ||
            !host_.create_or_load_graphics_pso) {
            return false;
        }
        if (!LoadVkFns()) return false;
        vk_.get_mem_props(phys_, &mem_props_);

        if (!CreateStaticObjects()) {
            Shutdown();
            return false;
        }

        vs_mod_ = CompileMod("vs_fullscreen", "vs_5_0");
        ps_bilinear_ = CompileMod("ps_bilinear", "ps_5_0");
        ps_integer_ = CompileMod("ps_integer", "ps_5_0");
        ps_bicubic_ = CompileMod("ps_bicubic", "ps_5_0");
        ps_cas_ = CompileMod("ps_cas", "ps_5_0");
        ps_easu_ = CompileMod("ps_fsr1_easu", "ps_5_0");
        ps_rcas_ = CompileMod("ps_fsr1_rcas", "ps_5_0");
        ps_nis_ = CompileMod("ps_nis", "ps_5_0");
        if (!vs_mod_ || !ps_bilinear_ || !ps_integer_ || !ps_bicubic_ || !ps_cas_ || !ps_easu_ || !ps_rcas_ ||
            !ps_nis_) {
            LOGE("upscale: Vulkan spatial shader compilation failed");
            Shutdown();
            return false;
        }

        if (!CreateEasuRenderPass() || !CreatePipelineFor(ps_easu_, easu_rp_, pipe_easu_)) {
            Shutdown();
            return false;
        }

        inited_ = true;
        LOGR("upscale: Vulkan 1.2 spatial suite ready (FSR 1.0, NIS v1.0.3, CAS, Bicubic 5-Tap, Integer, Bilinear)");
        return true;
    }

    void Shutdown() override {
        if (dev_ && vk_.destroy_pipe) {
            DestroyPresentPipelines();
            DestroyEasuTarget();
            if (pipe_easu_) { vk_.destroy_pipe(dev_, pipe_easu_, nullptr); pipe_easu_ = VK_NULL_HANDLE; }
            if (easu_rp_) { vk_.destroy_rp(dev_, easu_rp_, nullptr); easu_rp_ = VK_NULL_HANDLE; }
            if (lut_view_) { vk_.destroy_img_view(dev_, lut_view_, nullptr); lut_view_ = VK_NULL_HANDLE; }
            lut_img_cached_ = VK_NULL_HANDLE;
            if (smp_linear_) { vk_.destroy_sampler(dev_, smp_linear_, nullptr); smp_linear_ = VK_NULL_HANDLE; }
            if (smp_point_) { vk_.destroy_sampler(dev_, smp_point_, nullptr); smp_point_ = VK_NULL_HANDLE; }
            if (desc_pool_) { vk_.destroy_dp(dev_, desc_pool_, nullptr); desc_pool_ = VK_NULL_HANDLE; }
            if (pipe_layout_) { vk_.destroy_pl(dev_, pipe_layout_, nullptr); pipe_layout_ = VK_NULL_HANDLE; }
            if (dsl_) { vk_.destroy_dsl(dev_, dsl_, nullptr); dsl_ = VK_NULL_HANDLE; }
        }
        if (vk_dl_) {
            dlclose(vk_dl_);
            vk_dl_ = nullptr;
        }
        inited_ = false;
        dev_ = VK_NULL_HANDLE;
        phys_ = VK_NULL_HANDLE;
        inst_ = VK_NULL_HANDLE;
    }

    void ResolveRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, bool /*widescreen*/,
                           uint32_t& out_render_w, uint32_t& out_render_h) override {
        const app::Settings& s = app::Get();
        app::UpscalerMode eff = IsTemporalMode(s.upscaler) ? app::UpscalerMode::Fsr1 : s.upscaler;
        ComputeRenderSize(vp_w, vp_h, guest_w, guest_h, eff, s.upscale_quality, s.render_percent,
                          s.aspect_correct_render, out_render_w, out_render_h);
    }

    void BeginFrame(uint32_t /*render_w*/, uint32_t /*render_h*/, uint32_t /*vp_w*/, uint32_t /*vp_h*/,
                    const gpu::CameraFrameData& /*cam*/, bool& out_jitter_active, float& out_jitter_x,
                    float& out_jitter_y) override {
        out_jitter_active = false;
        out_jitter_x = 0.0f;
        out_jitter_y = 0.0f;
    }

    bool Execute(const gpu::UpscalerFrameDesc& fd) override {
        if (!inited_ || !fd.cmd_list || !fd.front_res || !fd.front_view || !fd.backbuffer_rtv_ptr) return false;

        void* raw_rp = nullptr;
        if (!host_.create_or_load_graphics_pso(host_.ctx, nullptr, nullptr, &raw_rp) || !raw_rp) return false;
        VkRenderPass cur_rp = reinterpret_cast<VkRenderPass>(raw_rp);
        if (cur_rp != present_rp_cached_) {
            if (!RebuildPresentPipelines(cur_rp)) return false;
        }

        VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(fd.cmd_list);
        VkImage front_img = reinterpret_cast<VkImage>(fd.front_res);
        VkImageView front_view = reinterpret_cast<VkImageView>(fd.front_view);
        VkFramebuffer bb_fb = reinterpret_cast<VkFramebuffer>(fd.backbuffer_rtv_ptr);

        VkImageView lut_view = EnsureLutView(reinterpret_cast<VkImage>(fd.lut_res), front_view);
        host_.transition(host_.ctx, front_img, fd.front_state, uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        if (fd.lut_res && fd.lut_state) {
            host_.transition(host_.ctx, fd.lut_res, fd.lut_state, uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }

        const app::Settings& s = app::Get();
        const app::UpscalerMode req = s.upscaler;
        const bool fallback = IsTemporalMode(req);
        const app::UpscalerMode active = fallback ? app::UpscalerMode::Fsr1 : req;
        const float sharp = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);

        bool ok = false;
        if (active == app::UpscalerMode::Fsr1) {
            bool need_easu = (fd.render_w != fd.vp_w || fd.render_h != fd.vp_h);
            if (need_easu && EnsureEasuTarget(fd.vp_w, fd.vp_h)) {
                uint32_t est = uint32_t(easu_layout_);
                host_.transition(host_.ctx, easu_img_, &est, uint32_t(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
                easu_layout_ = static_cast<VkImageLayout>(est);

                ok = DrawPass(cmd, easu_rp_, easu_fb_, pipe_easu_, front_view, lut_view, fd.render_w, fd.render_h, 0,
                              0, fd.vp_w, fd.vp_h, fd.vp_w, fd.vp_h, sharp, false);
                if (ok) {
                    est = uint32_t(easu_layout_);
                    host_.transition(host_.ctx, easu_img_, &est, uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                    easu_layout_ = static_cast<VkImageLayout>(est);
                    ok = DrawPass(cmd, present_rp_cached_, bb_fb, pipe_rcas_, easu_view_, lut_view, fd.vp_w, fd.vp_h,
                                  fd.vp_x, fd.vp_y, fd.vp_w, fd.vp_h, fd.bb_w, fd.bb_h, sharp, fd.gamma);
                }
            } else {
                ok = DrawPass(cmd, present_rp_cached_, bb_fb, pipe_rcas_, front_view, lut_view, fd.render_w,
                              fd.render_h, fd.vp_x, fd.vp_y, fd.vp_w, fd.vp_h, fd.bb_w, fd.bb_h, sharp, fd.gamma);
            }
        } else if (active == app::UpscalerMode::Integer) {
            uint32_t ox = fd.vp_x, oy = fd.vp_y, ow = fd.vp_w, oh = fd.vp_h;
            if (fd.render_w > 0 && fd.render_h > 0 && fd.render_w <= fd.vp_w && fd.render_h <= fd.vp_h) {
                uint32_t k = std::max(1u, std::min(fd.vp_w / fd.render_w, fd.vp_h / fd.render_h));
                ow = fd.render_w * k;
                oh = fd.render_h * k;
                ox = fd.vp_x + (fd.vp_w - ow) / 2u;
                oy = fd.vp_y + (fd.vp_h - oh) / 2u;
            }
            ok = DrawPass(cmd, present_rp_cached_, bb_fb, pipe_integer_, front_view, lut_view, fd.render_w, fd.render_h,
                          ox, oy, ow, oh, fd.bb_w, fd.bb_h, 0.0f, fd.gamma);
        } else {
            VkPipeline pipe = pipe_bilinear_;
            switch (active) {
                case app::UpscalerMode::Nis: pipe = pipe_nis_; break;
                case app::UpscalerMode::Cas: pipe = pipe_cas_; break;
                case app::UpscalerMode::Bicubic: pipe = pipe_bicubic_; break;
                case app::UpscalerMode::Bilinear:
                default: pipe = pipe_bilinear_; break;
            }
            ok = DrawPass(cmd, present_rp_cached_, bb_fb, pipe, front_view, lut_view, fd.render_w, fd.render_h, fd.vp_x,
                          fd.vp_y, fd.vp_w, fd.vp_h, fd.bb_w, fd.bb_h, sharp, fd.gamma);
        }

        if (ok) {
            std::lock_guard<std::mutex> lk(status_lock_);
            bool changed = (status_.active_mode != uint32_t(active) || status_.requested_mode != uint32_t(req) ||
                            status_.render_w != fd.render_w || status_.vp_w != fd.vp_w);
            status_.requested_mode = uint32_t(req);
            status_.active_mode = uint32_t(active);
            status_.render_w = fd.render_w;
            status_.render_h = fd.render_h;
            status_.vp_w = fd.vp_w;
            status_.vp_h = fd.vp_h;
            status_.bb_w = fd.bb_w;
            status_.bb_h = fd.bb_h;
            status_.temporal_active = false;
            status_.fallback_active = fallback;
            status_.dlss_available = false;
            status_.fsr3_available = false;
            status_.xess_available = false;
            status_.active_label = ModeShortName(active);
            status_.status_detail = fallback ? "Linux Vulkan spatial fallback -> FSR 1.0" : ModeName(active);
            if (changed) {
                LOGR("upscale: vulkan active=%s (req=%s, fallback=%d, %ux%u -> %ux%u)", status_.active_label.c_str(),
                     ModeShortName(req), fallback ? 1 : 0, fd.render_w, fd.render_h, fd.vp_w, fd.vp_h);
            }
        }
        return ok;
    }

    void Prewarm(void* /*cmd_list*/, uint32_t /*render_w*/, uint32_t /*render_h*/, uint32_t vp_w,
                 uint32_t vp_h) override {
        if (!inited_) return;
        void* raw_rp = nullptr;
        if (host_.create_or_load_graphics_pso &&
            host_.create_or_load_graphics_pso(host_.ctx, nullptr, nullptr, &raw_rp) && raw_rp) {
            VkRenderPass cur_rp = reinterpret_cast<VkRenderPass>(raw_rp);
            if (cur_rp != present_rp_cached_) {
                RebuildPresentPipelines(cur_rp);
            }
        }
        if (vp_w > 0 && vp_h > 0) {
            EnsureEasuTarget(vp_w, vp_h);
        }
    }

    void ResetHistory() override {}

    gpu::UpscalerStatus GetStatus() const override {
        std::lock_guard<std::mutex> lk(status_lock_);
        return status_;
    }

private:
    static constexpr uint32_t kDsRing = 16;

    bool LoadVkFns() {
        auto get_ipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!get_ipa) {
            vk_dl_ = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
            if (vk_dl_) get_ipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_dl_, "vkGetInstanceProcAddr"));
        }
        if (!get_ipa) return false;
        auto get_dpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_ipa(inst_, "vkGetDeviceProcAddr"));
        vk_.get_mem_props =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_ipa(inst_, "vkGetPhysicalDeviceMemoryProperties"));
        if (!get_dpa || !vk_.get_mem_props) return false;

        auto load = [&](auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(get_dpa(dev_, name));
            return fn != nullptr;
        };
        return load(vk_.create_dsl, "vkCreateDescriptorSetLayout") &&
               load(vk_.destroy_dsl, "vkDestroyDescriptorSetLayout") &&
               load(vk_.create_pl, "vkCreatePipelineLayout") &&
               load(vk_.destroy_pl, "vkDestroyPipelineLayout") &&
               load(vk_.create_dp, "vkCreateDescriptorPool") &&
               load(vk_.destroy_dp, "vkDestroyDescriptorPool") &&
               load(vk_.alloc_ds, "vkAllocateDescriptorSets") &&
               load(vk_.update_ds, "vkUpdateDescriptorSets") &&
               load(vk_.create_sampler, "vkCreateSampler") &&
               load(vk_.destroy_sampler, "vkDestroySampler") &&
               load(vk_.create_rp, "vkCreateRenderPass") &&
               load(vk_.destroy_rp, "vkDestroyRenderPass") &&
               load(vk_.create_img, "vkCreateImage") &&
               load(vk_.destroy_img, "vkDestroyImage") &&
               load(vk_.get_img_mem_req, "vkGetImageMemoryRequirements") &&
               load(vk_.alloc_mem, "vkAllocateMemory") &&
               load(vk_.free_mem, "vkFreeMemory") &&
               load(vk_.bind_img_mem, "vkBindImageMemory") &&
               load(vk_.create_img_view, "vkCreateImageView") &&
               load(vk_.destroy_img_view, "vkDestroyImageView") &&
               load(vk_.create_fb, "vkCreateFramebuffer") &&
               load(vk_.destroy_fb, "vkDestroyFramebuffer") &&
               load(vk_.destroy_pipe, "vkDestroyPipeline") &&
               load(vk_.cmd_begin_rp, "vkCmdBeginRenderPass") &&
               load(vk_.cmd_end_rp, "vkCmdEndRenderPass") &&
               load(vk_.cmd_bind_pipe, "vkCmdBindPipeline") &&
               load(vk_.cmd_bind_ds, "vkCmdBindDescriptorSets") &&
               load(vk_.cmd_set_vp, "vkCmdSetViewport") &&
               load(vk_.cmd_set_sc, "vkCmdSetScissor") &&
               load(vk_.cmd_draw, "vkCmdDraw");
    }

    bool CreateStaticObjects() {
        VkDescriptorSetLayoutBinding binds[5]{};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        binds[1] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        binds[2] = {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        binds[3] = {12, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        binds[4] = {13, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dci.bindingCount = 5;
        dci.pBindings = binds;
        if (vk_.create_dsl(dev_, &dci, nullptr, &dsl_) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl_;
        if (vk_.create_pl(dev_, &plci, nullptr, &pipe_layout_) != VK_SUCCESS) return false;

        VkDescriptorPoolSize psizes[3] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDsRing},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDsRing * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kDsRing * 2},
        };
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = kDsRing;
        dpci.poolSizeCount = 3;
        dpci.pPoolSizes = psizes;
        if (vk_.create_dp(dev_, &dpci, nullptr, &desc_pool_) != VK_SUCCESS) return false;

        VkDescriptorSetLayout layouts[kDsRing];
        for (uint32_t i = 0; i < kDsRing; ++i) layouts[i] = dsl_;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = desc_pool_;
        dai.descriptorSetCount = kDsRing;
        dai.pSetLayouts = layouts;
        if (vk_.alloc_ds(dev_, &dai, dsets_) != VK_SUCCESS) return false;

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vk_.create_sampler(dev_, &sci, nullptr, &smp_linear_) != VK_SUCCESS) return false;

        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        if (vk_.create_sampler(dev_, &sci, nullptr, &smp_point_) != VK_SUCCESS) return false;
        return true;
    }

    VkShaderModule CompileMod(const char* entry, const char* profile) {
        uint64_t h = 0;
        return reinterpret_cast<VkShaderModule>(host_.compile(host_.ctx, kVkSpatialHlsl, entry, profile, &h));
    }

    bool CreateEasuRenderPass() {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        return vk_.create_rp(dev_, &rpci, nullptr, &easu_rp_) == VK_SUCCESS;
    }

    bool CreatePipelineFor(VkShaderModule ps, VkRenderPass rp, VkPipeline& out_pipe) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                     vs_mod_, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                     ps, "main", nullptr};

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = pipe_layout_;
        ci.renderPass = rp;
        ci.subpass = 0;

        void* raw = nullptr;
        if (!host_.create_or_load_graphics_pso(host_.ctx, L"vk_upscale", &ci, &raw) || !raw) return false;
        out_pipe = reinterpret_cast<VkPipeline>(raw);
        return true;
    }

    void DestroyPresentPipelines() {
        auto kill = [&](VkPipeline& p) {
            if (p) {
                vk_.destroy_pipe(dev_, p, nullptr);
                p = VK_NULL_HANDLE;
            }
        };
        kill(pipe_bilinear_);
        kill(pipe_integer_);
        kill(pipe_bicubic_);
        kill(pipe_cas_);
        kill(pipe_rcas_);
        kill(pipe_nis_);
        present_rp_cached_ = VK_NULL_HANDLE;
    }

    bool RebuildPresentPipelines(VkRenderPass rp) {
        DestroyPresentPipelines();
        bool ok = CreatePipelineFor(ps_bilinear_, rp, pipe_bilinear_) &&
                  CreatePipelineFor(ps_integer_, rp, pipe_integer_) &&
                  CreatePipelineFor(ps_bicubic_, rp, pipe_bicubic_) && CreatePipelineFor(ps_cas_, rp, pipe_cas_) &&
                  CreatePipelineFor(ps_rcas_, rp, pipe_rcas_) && CreatePipelineFor(ps_nis_, rp, pipe_nis_);
        if (ok) present_rp_cached_ = rp;
        else DestroyPresentPipelines();
        return ok;
    }

    uint32_t FindMemType(uint32_t bits, VkMemoryPropertyFlags props) const {
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) return i;
        }
        return UINT32_MAX;
    }

    void DestroyEasuTarget() {
        if (easu_fb_) { vk_.destroy_fb(dev_, easu_fb_, nullptr); easu_fb_ = VK_NULL_HANDLE; }
        if (easu_view_) { vk_.destroy_img_view(dev_, easu_view_, nullptr); easu_view_ = VK_NULL_HANDLE; }
        if (easu_img_) { vk_.destroy_img(dev_, easu_img_, nullptr); easu_img_ = VK_NULL_HANDLE; }
        if (easu_mem_) { vk_.free_mem(dev_, easu_mem_, nullptr); easu_mem_ = VK_NULL_HANDLE; }
        easu_w_ = easu_h_ = 0;
        easu_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool EnsureEasuTarget(uint32_t w, uint32_t h) {
        if (easu_img_ && easu_w_ == w && easu_h_ == h) return true;
        if (easu_img_ && host_.flush) host_.flush(host_.ctx);
        DestroyEasuTarget();

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.create_img(dev_, &ici, nullptr, &easu_img_) != VK_SUCCESS) return false;

        VkMemoryRequirements mr{};
        vk_.get_img_mem_req(dev_, easu_img_, &mr);
        uint32_t mt = FindMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) {
            DestroyEasuTarget();
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        if (vk_.alloc_mem(dev_, &mai, nullptr, &easu_mem_) != VK_SUCCESS ||
            vk_.bind_img_mem(dev_, easu_img_, easu_mem_, 0) != VK_SUCCESS) {
            DestroyEasuTarget();
            return false;
        }

        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = easu_img_;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk_.create_img_view(dev_, &iv, nullptr, &easu_view_) != VK_SUCCESS) {
            DestroyEasuTarget();
            return false;
        }

        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = easu_rp_;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &easu_view_;
        fbci.width = w;
        fbci.height = h;
        fbci.layers = 1;
        if (vk_.create_fb(dev_, &fbci, nullptr, &easu_fb_) != VK_SUCCESS) {
            DestroyEasuTarget();
            return false;
        }
        easu_w_ = w;
        easu_h_ = h;
        easu_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    VkImageView EnsureLutView(VkImage lut_img, VkImageView fallback_view) {
        if (!lut_img) return fallback_view;
        if (lut_img == lut_img_cached_ && lut_view_) return lut_view_;
        if (lut_view_) {
            vk_.destroy_img_view(dev_, lut_view_, nullptr);
            lut_view_ = VK_NULL_HANDLE;
        }
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = lut_img;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VK_FORMAT_B8G8R8A8_UNORM;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk_.create_img_view(dev_, &iv, nullptr, &lut_view_) == VK_SUCCESS) {
            lut_img_cached_ = lut_img;
            return lut_view_;
        }
        return fallback_view;
    }

    bool DrawPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkPipeline pipe, VkImageView src_view,
                  VkImageView lut_view, uint32_t src_w, uint32_t src_h, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
                  uint32_t dst_h, uint32_t fb_w, uint32_t fb_h, float sharpness, bool gamma_on) {
        if (!pipe || !rp || !fb) return false;
        SpatialConstants cb{};
        cb.src_w = float(std::max(1u, src_w));
        cb.src_h = float(std::max(1u, src_h));
        cb.inv_src_w = 1.0f / cb.src_w;
        cb.inv_src_h = 1.0f / cb.src_h;
        cb.dst_w = float(std::max(1u, dst_w));
        cb.dst_h = float(std::max(1u, dst_h));
        cb.inv_dst_w = 1.0f / cb.dst_w;
        cb.inv_dst_h = 1.0f / cb.dst_h;
        cb.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
        cb.gamma_on = gamma_on ? 1.0f : 0.0f;

        void* up_buf = nullptr;
        uint64_t up_off = 0;
        uint8_t* up_cpu = nullptr;
        uint64_t up_gpu = 0;
        if (!host_.upload(host_.ctx, sizeof(cb), 256, &up_buf, &up_off, &up_cpu, &up_gpu)) return false;
        std::memcpy(up_cpu, &cb, sizeof(cb));

        VkDescriptorSet ds = dsets_[ds_cursor_++ % kDsRing];
        VkDescriptorBufferInfo bi{reinterpret_cast<VkBuffer>(up_buf), up_off, sizeof(cb)};
        VkDescriptorImageInfo ii0{VK_NULL_HANDLE, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo ii1{VK_NULL_HANDLE, lut_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo si0{smp_linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorImageInfo si1{smp_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};

        VkWriteDescriptorSet writes[5]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &bi, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                     &ii0, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                     &ii1, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 12, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si0,
                     nullptr, nullptr};
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 13, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si1,
                     nullptr, nullptr};
        vk_.update_ds(dev_, 5, writes, 0, nullptr);

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = rp;
        rpbi.framebuffer = fb;
        rpbi.renderArea = {{0, 0}, {fb_w, fb_h}};
        vk_.cmd_begin_rp(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        // Negative-height Vulkan 1.1+ viewport mirrors D3D12 Y-down orientation cleanly
        VkViewport vp{float(dst_x), float(dst_y + dst_h), float(dst_w), -float(dst_h), 0.0f, 1.0f};
        VkRect2D sc{{int32_t(dst_x), int32_t(dst_y)}, {dst_w, dst_h}};
        vk_.cmd_set_vp(cmd, 0, 1, &vp);
        vk_.cmd_set_sc(cmd, 0, 1, &sc);
        vk_.cmd_bind_pipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk_.cmd_bind_ds(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &ds, 0, nullptr);
        vk_.cmd_draw(cmd, 3, 1, 0, 0);
        vk_.cmd_end_rp(cmd);
        return true;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkInstance inst_ = VK_NULL_HANDLE;
    gpu::UpscalerHostOps host_{};
    VkFns vk_{};
    void* vk_dl_ = nullptr;
    VkPhysicalDeviceMemoryProperties mem_props_{};

    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet dsets_[kDsRing]{};
    uint32_t ds_cursor_ = 0;
    VkSampler smp_linear_ = VK_NULL_HANDLE;
    VkSampler smp_point_ = VK_NULL_HANDLE;

    VkShaderModule vs_mod_ = VK_NULL_HANDLE;
    VkShaderModule ps_bilinear_ = VK_NULL_HANDLE;
    VkShaderModule ps_integer_ = VK_NULL_HANDLE;
    VkShaderModule ps_bicubic_ = VK_NULL_HANDLE;
    VkShaderModule ps_cas_ = VK_NULL_HANDLE;
    VkShaderModule ps_easu_ = VK_NULL_HANDLE;
    VkShaderModule ps_rcas_ = VK_NULL_HANDLE;
    VkShaderModule ps_nis_ = VK_NULL_HANDLE;

    VkRenderPass easu_rp_ = VK_NULL_HANDLE;
    VkPipeline pipe_easu_ = VK_NULL_HANDLE;
    VkImage easu_img_ = VK_NULL_HANDLE;
    VkDeviceMemory easu_mem_ = VK_NULL_HANDLE;
    VkImageView easu_view_ = VK_NULL_HANDLE;
    VkFramebuffer easu_fb_ = VK_NULL_HANDLE;
    VkImageLayout easu_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t easu_w_ = 0, easu_h_ = 0;

    VkRenderPass present_rp_cached_ = VK_NULL_HANDLE;
    VkPipeline pipe_bilinear_ = VK_NULL_HANDLE;
    VkPipeline pipe_integer_ = VK_NULL_HANDLE;
    VkPipeline pipe_bicubic_ = VK_NULL_HANDLE;
    VkPipeline pipe_cas_ = VK_NULL_HANDLE;
    VkPipeline pipe_rcas_ = VK_NULL_HANDLE;
    VkPipeline pipe_nis_ = VK_NULL_HANDLE;

    VkImage lut_img_cached_ = VK_NULL_HANDLE;
    VkImageView lut_view_ = VK_NULL_HANDLE;

    bool inited_ = false;
    mutable std::mutex status_lock_;
    gpu::UpscalerStatus status_{};
};

}  // namespace

gpu::Upscaler* GetManager() {
    static VulkanUpscalerManager mgr;
    return &mgr;
}

void RegisterUpscaler() { gpu::SetUpscaler(GetManager()); }

}  // namespace upscale

#endif  // !_WIN32
