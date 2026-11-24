// SPDX-License-Identifier: GPL-2.0-or-later
// Win64 D3D12 implementation of the 6 zero-DLL spatial upscalers
// (FSR 1.0 EASU+RCAS, NIS v1.0.3, CAS, Bicubic 5-Tap incl. SSAA, Integer, Bilinear),
// temporal output composite + Xbox gamma LUT, and camera MV / HUD reactive / R32F depth prep.

#ifdef _WIN32

#include "upscale/upscaler_internal.h"
#include "upscale/upscaler_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"

namespace upscale {

namespace {

using Microsoft::WRL::ComPtr;

struct SpatialConstants {
    float src_w, src_h, inv_src_w, inv_src_h;
    float dst_w, dst_h, inv_dst_w, inv_dst_h;
    float sharpness;
    float gamma_on;
    float mode_param0;
    float mode_param1;
};

struct TemporalPrepConstants {
    float cur_vp_inv[16];
    float prev_vp[16];
    uint32_t width;
    uint32_t height;
    uint32_t has_prev_vp;
    uint32_t has_scene_color;
    float jitter_x;
    float jitter_y;
    float pad0;
    float pad1;
};

const char kSpatialHlsl[] = R"HLSL(
cbuffer SpatialCB : register(b0) {
    float4 SrcInfo;   // w, h, 1/w, 1/h
    float4 DstInfo;   // w, h, 1/w, 1/h
    float4 Params;    // sharpness, gamma_on, mode_param0, mode_param1
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

float Luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

// 1. Bilinear + Gamma LUT
float4 ps_bilinear(VsOut i) : SV_Target {
    float3 c = SrcTex.SampleLevel(SmpLinear, i.uv, 0).rgb;
    return float4(ApplyGammaLut(c), 1.0);
}

// 2. Integer Nearest Pixel + Gamma LUT
float4 ps_integer(VsOut i) : SV_Target {
    float3 c = SrcTex.SampleLevel(SmpPoint, i.uv, 0).rgb;
    return float4(ApplyGammaLut(c), 1.0);
}

// 3. 5-Tap Optimized Catmull-Rom Bicubic (supports both upscaling and >100% SSAA downsampling)
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
    float3 c = SampleBicubic5Tap(i.uv);
    return float4(ApplyGammaLut(c), 1.0);
}

// 4. AMD FidelityFX Contrast Adaptive Sharpening (CAS)
float3 EvalCas(float2 uv, float sharpness) {
    float2 d = SrcInfo.zw;
    float3 a = SrcTex.SampleLevel(SmpLinear, uv + float2(-d.x, -d.y), 0).rgb;
    float3 b = SrcTex.SampleLevel(SmpLinear, uv + float2( 0.0, -d.y), 0).rgb;
    float3 c = SrcTex.SampleLevel(SmpLinear, uv + float2( d.x, -d.y), 0).rgb;
    float3 d0 = SrcTex.SampleLevel(SmpLinear, uv + float2(-d.x,  0.0), 0).rgb;
    float3 e = SrcTex.SampleLevel(SmpLinear, uv, 0).rgb;
    float3 f = SrcTex.SampleLevel(SmpLinear, uv + float2( d.x,  0.0), 0).rgb;
    float3 g = SrcTex.SampleLevel(SmpLinear, uv + float2(-d.x,  d.y), 0).rgb;
    float3 h = SrcTex.SampleLevel(SmpLinear, uv + float2( 0.0,  d.y), 0).rgb;
    float3 k = SrcTex.SampleLevel(SmpLinear, uv + float2( d.x,  d.y), 0).rgb;

    float3 mn = min(min(min(d0, e), min(f, b)), h);
    float3 mn2 = min(mn, min(min(a, c), min(g, k)));
    mn += mn2;

    float3 mx = max(max(max(d0, e), max(f, b)), h);
    float3 mx2 = max(mx, max(max(a, c), max(g, k)));
    mx += mx2;

    float3 rcpMx = rcp(max(mx, 1e-5));
    float3 amp = saturate(min(mn, 2.0 - mx) * rcpMx);
    amp = sqrt(amp);

    float peak = -rcp(lerp(8.0, 5.0, saturate(sharpness)));
    float3 w = amp * peak;
    float3 rcpW = rcp(1.0 + 4.0 * w);
    float3 outC = saturate(((b + d0 + f + h) * w + e) * rcpW);
    return outC;
}

float4 ps_cas(VsOut i) : SV_Target {
    float3 c = EvalCas(i.uv, Params.x);
    return float4(ApplyGammaLut(c), 1.0);
}

// 5a. FSR 1.0 EASU (12-tap edge-adaptive directional elliptical Lanczos-2 reconstruction)
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

    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = (lenX > 1e-5) ? rcp(lenX) : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = (lenY > 1e-5) ? rcp(lenY) : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float4 ps_fsr1_easu(VsOut i) : SV_Target {
    float2 pp = i.uv * SrcInfo.xy - 0.5;
    float2 fp = floor(pp);
    pp -= fp;

    int2 base = int2(fp);
    int2 maxC = int2(SrcInfo.xy) - 1;

    float3 b = SrcTex.Load(int3(clamp(base + int2( 0, -1), 0, maxC), 0)).rgb;
    float3 c = SrcTex.Load(int3(clamp(base + int2( 1, -1), 0, maxC), 0)).rgb;
    float3 e = SrcTex.Load(int3(clamp(base + int2(-1,  0), 0, maxC), 0)).rgb;
    float3 f = SrcTex.Load(int3(clamp(base + int2( 0,  0), 0, maxC), 0)).rgb;
    float3 g = SrcTex.Load(int3(clamp(base + int2( 1,  0), 0, maxC), 0)).rgb;
    float3 h = SrcTex.Load(int3(clamp(base + int2( 2,  0), 0, maxC), 0)).rgb;
    float3 i0 = SrcTex.Load(int3(clamp(base + int2(-1, 1), 0, maxC), 0)).rgb;
    float3 j = SrcTex.Load(int3(clamp(base + int2( 0,  1), 0, maxC), 0)).rgb;
    float3 k = SrcTex.Load(int3(clamp(base + int2( 1,  1), 0, maxC), 0)).rgb;
    float3 l = SrcTex.Load(int3(clamp(base + int2( 2,  1), 0, maxC), 0)).rgb;
    float3 n = SrcTex.Load(int3(clamp(base + int2( 0,  2), 0, maxC), 0)).rgb;
    float3 o = SrcTex.Load(int3(clamp(base + int2( 1,  2), 0, maxC), 0)).rgb;

    float bL = Luma(b), cL = Luma(c), eL = Luma(e), fL = Luma(f), gL = Luma(g), hL = Luma(h);
    float iL = Luma(i0), jL = Luma(j), kL = Luma(k), lL = Luma(l), nL = Luma(n), oL = Luma(o);

    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : rsqrt(dirR);
    dir = zro ? float2(1.0, 0.0) : (dir * dirR);

    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp(lob);

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

    float3 outC = clamp(aC / max(aW, 1e-5), min4, max4);
    return float4(saturate(outC), 1.0);
}

// 5b. FSR 1.0 RCAS (Robust Contrast Adaptive Sharpening + Gamma LUT composite)
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
    nz = saturate(abs(nz) / max(mxL - mnL, 1e-5));
    nz = -0.5 * nz + 1.0;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) * rcp(max(4.0 * mx4, 1e-5));
    float3 hitMax = (1.0 - max(mx4, e)) * rcp(max(4.0 * mn4 - 4.0, -1e-5));
    float3 lobeRGB = max(-hitMin, hitMax);
    float sharp = saturate(Params.x);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharp * nz;

    float3 outC = (lobe * (b + d + f + h) + e) * rcp(4.0 * lobe + 1.0);
    return float4(ApplyGammaLut(outC), 1.0);
}

// 6. NVIDIA Image Scaling (NIS v1.0.3 directional spatial upscaler + USM sharpen + Gamma LUT)
float4 ps_nis(VsOut i) : SV_Target {
    float3 baseC = SampleBicubic5Tap(i.uv);
    float2 d = SrcInfo.zw;
    float3 n  = SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0, -d.y), 0).rgb;
    float3 s  = SrcTex.SampleLevel(SmpLinear, i.uv + float2( 0.0,  d.y), 0).rgb;
    float3 w  = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  0.0), 0).rgb;
    float3 e  = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  0.0), 0).rgb;
    float3 nw = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x, -d.y), 0).rgb;
    float3 ne = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x, -d.y), 0).rgb;
    float3 sw = SrcTex.SampleLevel(SmpLinear, i.uv + float2(-d.x,  d.y), 0).rgb;
    float3 se = SrcTex.SampleLevel(SmpLinear, i.uv + float2( d.x,  d.y), 0).rgb;

    float lC = Luma(baseC);
    float lN = Luma(n), lS = Luma(s), lW = Luma(w), lE = Luma(e);
    float lNW = Luma(nw), lNE = Luma(ne), lSW = Luma(sw), lSE = Luma(se);

    // Directional edge variance across 4 axes (0, 45, 90, 135 deg)
    float g0   = abs(lE - lW);
    float g90  = abs(lS - lN);
    float g45  = abs(lNE - lSW);
    float g135 = abs(lSE - lNW);
    float edgeStrength = saturate(max(max(g0, g90), max(g45, g135)) * 2.5);

    float blur = (lN + lS + lW + lE) * 0.15 + (lNW + lNE + lSW + lSE) * 0.10;
    float usm = lC - blur;
    float sharpAmt = saturate(Params.x) * lerp(0.45, 1.15, edgeStrength);
    float delta = clamp(usm * sharpAmt, -0.18, 0.18);

    float3 outC = saturate(baseC + delta);
    return float4(ApplyGammaLut(outC), 1.0);
}
)HLSL";

const char kTemporalPrepHlsl[] = R"HLSL(
cbuffer PrepCB : register(b0) {
    float4x4 CurVpInv;
    float4x4 PrevVp;
    uint Width;
    uint Height;
    uint HasPrevVp;
    uint HasSceneColor;
    float JitterX;
    float JitterY;
    float Pad0;
    float Pad1;
};

Texture2D<float4> FrontTex      : register(t0);
Texture2D<float4> SceneColorTex : register(t1);
Texture2D<float>  DepthTex      : register(t2);

RWTexture2D<float2> OutMv       : register(u0);
RWTexture2D<float>  OutReactive : register(u1);
RWTexture2D<float>  OutDepth    : register(u2);

[numthreads(8, 8, 1)]
void cs_temporal_prep(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= Width || tid.y >= Height) return;
    int3 coord = int3(tid.xy, 0);

    float z = DepthTex.Load(coord);
    if (!isfinite(z)) z = 0.0;
    z = saturate(z);
    OutDepth[tid.xy] = z;

    // HUD / 2D overlay reactive mask
    float reactive = 0.0;
    if (HasSceneColor != 0) {
        float3 f = FrontTex.Load(coord).rgb;
        float3 s = SceneColorTex.Load(coord).rgb;
        float3 diff = abs(f - s);
        float maxd = max(diff.r, max(diff.g, diff.b));
        reactive = smoothstep(0.02, 0.08, maxd);
    } else if (z <= 1e-6) {
        reactive = 0.5;
    }
    OutReactive[tid.xy] = reactive;

    // Camera reprojection motion vector (prev_px - curr_px in render-resolution pixels)
    float2 mv = float2(0.0, 0.0);
    if (HasPrevVp != 0 && z > 1e-6) {
        float2 px = float2(tid.xy) + 0.5;
        float2 unj = (px - float2(JitterX, JitterY)) / float2(float(Width), float(Height));
        float4 ndc = float4(unj.x * 2.0 - 1.0, 1.0 - unj.y * 2.0, z, 1.0);
        float4 world = mul(CurVpInv, ndc);
        if (abs(world.w) > 1e-6) {
            world /= world.w;
            float4 prevClip = mul(PrevVp, world);
            if (abs(prevClip.w) > 1e-6) {
                float2 prevNdc = prevClip.xy / prevClip.w;
                float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
                float2 prevPx = prevUv * float2(float(Width), float(Height));
                float2 rawMv = prevPx - px;
                if (all(isfinite(rawMv))) {
                    mv = clamp(rawMv, float2(-2048.0, -2048.0), float2(2048.0, 2048.0));
                }
            }
        }
    }
    OutMv[tid.xy] = mv;
}
)HLSL";

DXGI_FORMAT CleanSrvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        default: return fmt == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : fmt;
    }
}

}  // namespace

struct SpatialD3D12::Impl {
    static constexpr uint32_t kHeapFrames = 4;
    static constexpr uint32_t kSlotsPerFrame = 16;
    static constexpr uint32_t kTotalSlots = kHeapFrames * kSlotsPerFrame;

    ID3D12Device* dev = nullptr;
    gpu::UpscalerHostOps host{};

    ComPtr<ID3D12RootSignature> gfx_rs;
    ComPtr<ID3D12RootSignature> cmp_rs;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    uint32_t srv_inc = 0;
    uint32_t rtv_inc = 0;
    uint32_t frame_ring = 0;

    ComPtr<ID3D12PipelineState> pso_bilinear;
    ComPtr<ID3D12PipelineState> pso_integer;
    ComPtr<ID3D12PipelineState> pso_bicubic;
    ComPtr<ID3D12PipelineState> pso_cas;
    ComPtr<ID3D12PipelineState> pso_fsr1_easu;
    ComPtr<ID3D12PipelineState> pso_fsr1_rcas;
    ComPtr<ID3D12PipelineState> pso_nis;
    ComPtr<ID3D12PipelineState> pso_temporal_prep;

    // Intermediate target for 2-pass FSR 1.0 (EASU -> RCAS)
    ComPtr<ID3D12Resource> easu_tex;
    D3D12_RESOURCE_STATES easu_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t easu_w = 0;
    uint32_t easu_h = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(uint32_t slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(slot) * srv_inc;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv(uint32_t slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += uint64_t(slot) * srv_inc;
        return h;
    }

    bool EnsureEasuTarget(uint32_t w, uint32_t h) {
        if (easu_tex && easu_w == w && easu_h == h) return true;
        if (easu_tex && host.flush) host.flush(host.ctx);
        easu_tex.Reset();
        easu_w = easu_h = 0;

        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = rd.Format;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                &cv, IID_PPV_ARGS(&easu_tex)))) {
            return false;
        }
        easu_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        easu_w = w;
        easu_h = h;
        dev->CreateRenderTargetView(easu_tex.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool CreateGraphicsPso(ID3DBlob* vs, const char* ps_entry, const wchar_t* name, DXGI_FORMAT rtv_fmt,
                           ComPtr<ID3D12PipelineState>& out_pso) {
        uint64_t ph = 0;
        auto* ps = static_cast<ID3DBlob*>(host.compile(host.ctx, kSpatialHlsl, ps_entry, "ps_5_0", &ph));
        if (!ps) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = gfx_rs.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtv_fmt;
        d.SampleDesc.Count = 1;
        void* raw = nullptr;
        if (host.create_or_load_graphics_pso && host.create_or_load_graphics_pso(host.ctx, name, &d, &raw) && raw) {
            out_pso.Attach(static_cast<ID3D12PipelineState*>(raw));
            return true;
        }
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out_pso)));
    }

    bool RunGfxPass(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, ID3D12Resource* src_tex,
                    DXGI_FORMAT src_fmt, ID3D12Resource* lut_tex, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                    uint32_t src_w, uint32_t src_h, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
                    float sharpness, bool gamma_on) {
        if (!pso || !src_tex) return false;
        uint32_t base_slot = (frame_ring % kHeapFrames) * kSlotsPerFrame + 0;

        D3D12_SHADER_RESOURCE_VIEW_DESC sv0{};
        sv0.Format = CleanSrvFormat(src_fmt);
        sv0.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv0.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv0.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(src_tex, &sv0, CpuSrv(base_slot));

        ID3D12Resource* lut_res = lut_tex ? lut_tex : src_tex;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv1{};
        sv1.Format = lut_tex ? DXGI_FORMAT_B8G8R8A8_UNORM : sv0.Format;
        sv1.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv1.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv1.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(lut_res, &sv1, CpuSrv(base_slot + 1));

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
        cb.gamma_on = (gamma_on && lut_tex) ? 1.0f : 0.0f;

        void* up_res = nullptr;
        uint64_t up_off = 0;
        uint8_t* up_cpu = nullptr;
        uint64_t up_gpu = 0;
        if (!host.upload(host.ctx, sizeof(cb), 256, &up_res, &up_off, &up_cpu, &up_gpu)) return false;
        std::memcpy(up_cpu, &cb, sizeof(cb));

        ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(gfx_rs.Get());
        cmd->SetPipelineState(pso);
        cmd->SetGraphicsRootConstantBufferView(0, up_gpu);
        cmd->SetGraphicsRootDescriptorTable(1, GpuSrv(base_slot));
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp{float(dst_x), float(dst_y), float(dst_w), float(dst_h), 0.0f, 1.0f};
        D3D12_RECT sc{LONG(dst_x), LONG(dst_y), LONG(dst_x + dst_w), LONG(dst_y + dst_h)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        return true;
    }
};

SpatialD3D12::SpatialD3D12() : impl_(std::make_unique<Impl>()) {}
SpatialD3D12::~SpatialD3D12() { Shutdown(); }

bool SpatialD3D12::Init(ID3D12Device* dev, const gpu::UpscalerHostOps& host) {
    Shutdown();
    impl_->dev = dev;
    impl_->host = host;
    if (!dev || !host.compile || !host.upload || !host.transition) return false;

    // Descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Impl::kTotalSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&impl_->srv_heap)))) return false;
    impl_->srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rhd.NumDescriptors = 2;
    if (FAILED(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&impl_->rtv_heap)))) return false;
    impl_->rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Graphics root signature: [0]=CBV(b0), [1]=SRV table(t0..t1), static samplers s0 (linear), s1 (point)
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 2;
    srv_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rps[2]{};
    rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rps[0].Descriptor.ShaderRegister = 0;
    rps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rps[1].DescriptorTable.NumDescriptorRanges = 1;
    rps[1].DescriptorTable.pDescriptorRanges = &srv_range;
    rps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC smps[2]{};
    smps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smps[0].AddressU = smps[0].AddressV = smps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smps[0].ShaderRegister = 0;
    smps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    smps[1] = smps[0];
    smps[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smps[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rps;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = smps;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&impl_->gfx_rs)))) {
        return false;
    }

    // Compute root signature: [0]=CBV(b0), [1]=SRV table(t0..t2), [2]=UAV table(u0..u2)
    D3D12_DESCRIPTOR_RANGE cs_srv{};
    cs_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cs_srv.NumDescriptors = 3;
    cs_srv.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE cs_uav{};
    cs_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    cs_uav.NumDescriptors = 3;
    cs_uav.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER crps[3]{};
    crps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    crps[0].Descriptor.ShaderRegister = 0;
    crps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    crps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    crps[1].DescriptorTable.NumDescriptorRanges = 1;
    crps[1].DescriptorTable.pDescriptorRanges = &cs_srv;
    crps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    crps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    crps[2].DescriptorTable.NumDescriptorRanges = 1;
    crps[2].DescriptorTable.pDescriptorRanges = &cs_uav;
    crps[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC crsd{};
    crsd.NumParameters = 3;
    crsd.pParameters = crps;
    sig.Reset();
    err.Reset();
    if (FAILED(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&impl_->cmp_rs)))) {
        return false;
    }

    uint64_t vh = 0;
    auto* vs = static_cast<ID3DBlob*>(host.compile(host.ctx, kSpatialHlsl, "vs_fullscreen", "vs_5_0", &vh));
    if (!vs) return false;

    const DXGI_FORMAT kBbFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool ok = impl_->CreateGraphicsPso(vs, "ps_bilinear", L"upscale_bilinear", kBbFmt, impl_->pso_bilinear) &&
              impl_->CreateGraphicsPso(vs, "ps_integer", L"upscale_integer", kBbFmt, impl_->pso_integer) &&
              impl_->CreateGraphicsPso(vs, "ps_bicubic", L"upscale_bicubic", kBbFmt, impl_->pso_bicubic) &&
              impl_->CreateGraphicsPso(vs, "ps_cas", L"upscale_cas", kBbFmt, impl_->pso_cas) &&
              impl_->CreateGraphicsPso(vs, "ps_fsr1_easu", L"upscale_fsr1_easu", DXGI_FORMAT_R8G8B8A8_UNORM,
                                       impl_->pso_fsr1_easu) &&
              impl_->CreateGraphicsPso(vs, "ps_fsr1_rcas", L"upscale_fsr1_rcas", kBbFmt, impl_->pso_fsr1_rcas) &&
              impl_->CreateGraphicsPso(vs, "ps_nis", L"upscale_nis", kBbFmt, impl_->pso_nis);
    if (!ok) {
        LOGE("upscale: failed creating D3D12 spatial upscaler PSOs");
        return false;
    }

    uint64_t ch = 0;
    auto* cs = static_cast<ID3DBlob*>(host.compile(host.ctx, kTemporalPrepHlsl, "cs_temporal_prep", "cs_5_0", &ch));
    if (cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = impl_->cmp_rs.Get();
        cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        void* raw = nullptr;
        if (host.create_or_load_compute_pso &&
            host.create_or_load_compute_pso(host.ctx, L"upscale_temporal_prep", &cd, &raw) && raw) {
            impl_->pso_temporal_prep.Attach(static_cast<ID3D12PipelineState*>(raw));
        } else {
            dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&impl_->pso_temporal_prep));
        }
    }

    LOGR("upscale: D3D12 spatial suite ready (FSR 1.0, NIS v1.0.3, CAS, Bicubic 5-Tap, Integer, Bilinear)");
    return true;
}

void SpatialD3D12::Prewarm(uint32_t vp_w, uint32_t vp_h) {
    if (!impl_ || !impl_->dev || vp_w == 0 || vp_h == 0) return;
    impl_->EnsureEasuTarget(vp_w, vp_h);
}

void SpatialD3D12::Shutdown() {
    if (!impl_) return;
    impl_->easu_tex.Reset();
    impl_->pso_bilinear.Reset();
    impl_->pso_integer.Reset();
    impl_->pso_bicubic.Reset();
    impl_->pso_cas.Reset();
    impl_->pso_fsr1_easu.Reset();
    impl_->pso_fsr1_rcas.Reset();
    impl_->pso_nis.Reset();
    impl_->pso_temporal_prep.Reset();
    impl_->rtv_heap.Reset();
    impl_->srv_heap.Reset();
    impl_->gfx_rs.Reset();
    impl_->cmp_rs.Reset();
    impl_->dev = nullptr;
}

bool SpatialD3D12::Execute(app::UpscalerMode mode, float sharpness, const gpu::UpscalerFrameDesc& fd) {
    if (!impl_->dev || !fd.cmd_list || !fd.front_res || !fd.backbuffer_res) return false;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(fd.cmd_list);
    auto* front = static_cast<ID3D12Resource*>(fd.front_res);
    auto* bb = static_cast<ID3D12Resource*>(fd.backbuffer_res);
    auto* lut = static_cast<ID3D12Resource*>(fd.lut_res);
    D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv{fd.backbuffer_rtv_ptr};

    impl_->host.transition(impl_->host.ctx, front, fd.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    impl_->host.transition(impl_->host.ctx, bb, fd.backbuffer_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (lut && fd.lut_state) {
        impl_->host.transition(impl_->host.ctx, lut, fd.lut_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    impl_->frame_ring++;
    DXGI_FORMAT front_fmt = static_cast<DXGI_FORMAT>(fd.front_fmt);

    if (mode == app::UpscalerMode::Fsr1) {
        bool need_easu = (fd.render_w != fd.vp_w || fd.render_h != fd.vp_h);
        if (need_easu && impl_->EnsureEasuTarget(fd.vp_w, fd.vp_h)) {
            uint32_t st = uint32_t(impl_->easu_state);
            impl_->host.transition(impl_->host.ctx, impl_->easu_tex.Get(), &st, D3D12_RESOURCE_STATE_RENDER_TARGET);
            impl_->easu_state = static_cast<D3D12_RESOURCE_STATES>(st);

            D3D12_CPU_DESCRIPTOR_HANDLE easu_rtv = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
            if (!impl_->RunGfxPass(cmd, impl_->pso_fsr1_easu.Get(), front, front_fmt, nullptr, easu_rtv, fd.render_w,
                                   fd.render_h, 0, 0, fd.vp_w, fd.vp_h, sharpness, false)) {
                return false;
            }
            impl_->frame_ring++;
            st = uint32_t(impl_->easu_state);
            impl_->host.transition(impl_->host.ctx, impl_->easu_tex.Get(), &st,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            impl_->easu_state = static_cast<D3D12_RESOURCE_STATES>(st);
            return impl_->RunGfxPass(cmd, impl_->pso_fsr1_rcas.Get(), impl_->easu_tex.Get(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, lut, bb_rtv, fd.vp_w, fd.vp_h, fd.vp_x, fd.vp_y,
                                     fd.vp_w, fd.vp_h, sharpness, fd.gamma);
        }
        return impl_->RunGfxPass(cmd, impl_->pso_fsr1_rcas.Get(), front, front_fmt, lut, bb_rtv, fd.render_w,
                                 fd.render_h, fd.vp_x, fd.vp_y, fd.vp_w, fd.vp_h, sharpness, fd.gamma);
    }

    if (mode == app::UpscalerMode::Integer) {
        uint32_t ox = fd.vp_x, oy = fd.vp_y, ow = fd.vp_w, oh = fd.vp_h;
        if (fd.render_w > 0 && fd.render_h > 0 && fd.render_w <= fd.vp_w && fd.render_h <= fd.vp_h) {
            uint32_t k = std::max(1u, std::min(fd.vp_w / fd.render_w, fd.vp_h / fd.render_h));
            ow = fd.render_w * k;
            oh = fd.render_h * k;
            ox = fd.vp_x + (fd.vp_w - ow) / 2u;
            oy = fd.vp_y + (fd.vp_h - oh) / 2u;
        }
        return impl_->RunGfxPass(cmd, impl_->pso_integer.Get(), front, front_fmt, lut, bb_rtv, fd.render_w,
                                 fd.render_h, ox, oy, ow, oh, 0.0f, fd.gamma);
    }

    ID3D12PipelineState* pso = impl_->pso_bilinear.Get();
    switch (mode) {
        case app::UpscalerMode::Nis: pso = impl_->pso_nis.Get(); break;
        case app::UpscalerMode::Cas: pso = impl_->pso_cas.Get(); break;
        case app::UpscalerMode::Bicubic: pso = impl_->pso_bicubic.Get(); break;
        case app::UpscalerMode::Bilinear:
        default: pso = impl_->pso_bilinear.Get(); break;
    }
    return impl_->RunGfxPass(cmd, pso, front, front_fmt, lut, bb_rtv, fd.render_w, fd.render_h, fd.vp_x, fd.vp_y,
                             fd.vp_w, fd.vp_h, sharpness, fd.gamma);
}

bool SpatialD3D12::CompositeOutput(ID3D12Resource* src_tex, D3D12_RESOURCE_STATES& src_state, DXGI_FORMAT src_fmt,
                                   float extra_sharpness, const gpu::UpscalerFrameDesc& fd) {
    if (!impl_->dev || !fd.cmd_list || !src_tex || !fd.backbuffer_res) return false;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(fd.cmd_list);
    auto* bb = static_cast<ID3D12Resource*>(fd.backbuffer_res);
    auto* lut = static_cast<ID3D12Resource*>(fd.lut_res);
    D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv{fd.backbuffer_rtv_ptr};

    uint32_t st = uint32_t(src_state);
    impl_->host.transition(impl_->host.ctx, src_tex, &st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    src_state = static_cast<D3D12_RESOURCE_STATES>(st);
    impl_->host.transition(impl_->host.ctx, bb, fd.backbuffer_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (lut && fd.lut_state) {
        impl_->host.transition(impl_->host.ctx, lut, fd.lut_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    impl_->frame_ring++;
    ID3D12PipelineState* pso =
        (extra_sharpness > 0.01f) ? impl_->pso_fsr1_rcas.Get() : impl_->pso_bilinear.Get();
    return impl_->RunGfxPass(cmd, pso, src_tex, src_fmt, lut, bb_rtv, fd.vp_w, fd.vp_h, fd.vp_x, fd.vp_y, fd.vp_w,
                             fd.vp_h, extra_sharpness, fd.gamma);
}

bool SpatialD3D12::BuildTemporalInputs(ID3D12GraphicsCommandList* cmd, ID3D12Resource* front_res,
                                       D3D12_RESOURCE_STATES& front_state, DXGI_FORMAT front_fmt,
                                       ID3D12Resource* scene_color_res, D3D12_RESOURCE_STATES* scene_color_state,
                                       DXGI_FORMAT scene_color_fmt, ID3D12Resource* depth_res,
                                       D3D12_RESOURCE_STATES& depth_state, ID3D12Resource* out_mv,
                                       D3D12_RESOURCE_STATES& mv_state, ID3D12Resource* out_reactive,
                                       D3D12_RESOURCE_STATES& reactive_state, ID3D12Resource* out_depth_r32f,
                                       D3D12_RESOURCE_STATES& depth_r32f_state, uint32_t w, uint32_t h,
                                       const float cur_vp_inv[16], const float prev_vp[16], bool has_prev_vp, float jx,
                                       float jy) {
    if (!impl_->dev || !impl_->pso_temporal_prep || !cmd || !front_res || !depth_res || !out_mv || !out_reactive ||
        !out_depth_r32f || w == 0 || h == 0) {
        return false;
    }

    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES& s, D3D12_RESOURCE_STATES want) {
        uint32_t u = uint32_t(s);
        impl_->host.transition(impl_->host.ctx, r, &u, uint32_t(want));
        s = static_cast<D3D12_RESOURCE_STATES>(u);
    };

    tr(front_res, front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bool has_sc = (scene_color_res != nullptr && scene_color_state != nullptr && scene_color_res != front_res);
    if (has_sc) {
        tr(scene_color_res, *scene_color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    tr(depth_res, depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tr(out_mv, mv_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(out_reactive, reactive_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tr(out_depth_r32f, depth_r32f_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    uint32_t base = (impl_->frame_ring % Impl::kHeapFrames) * Impl::kSlotsPerFrame + 4;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;

    sv.Format = CleanSrvFormat(front_fmt);
    impl_->dev->CreateShaderResourceView(front_res, &sv, impl_->CpuSrv(base + 0));

    sv.Format = has_sc ? CleanSrvFormat(scene_color_fmt) : CleanSrvFormat(front_fmt);
    impl_->dev->CreateShaderResourceView(has_sc ? scene_color_res : front_res, &sv, impl_->CpuSrv(base + 1));

    sv.Format = CleanSrvFormat(depth_res->GetDesc().Format);
    impl_->dev->CreateShaderResourceView(depth_res, &sv, impl_->CpuSrv(base + 2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    impl_->dev->CreateUnorderedAccessView(out_mv, nullptr, &uv, impl_->CpuSrv(base + 3));
    uv.Format = DXGI_FORMAT_R8_UNORM;
    impl_->dev->CreateUnorderedAccessView(out_reactive, nullptr, &uv, impl_->CpuSrv(base + 4));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    impl_->dev->CreateUnorderedAccessView(out_depth_r32f, nullptr, &uv, impl_->CpuSrv(base + 5));

    TemporalPrepConstants cb{};
    std::memcpy(cb.cur_vp_inv, cur_vp_inv, sizeof(cb.cur_vp_inv));
    std::memcpy(cb.prev_vp, prev_vp, sizeof(cb.prev_vp));
    cb.width = w;
    cb.height = h;
    cb.has_prev_vp = has_prev_vp ? 1u : 0u;
    cb.has_scene_color = has_sc ? 1u : 0u;
    cb.jitter_x = jx;
    cb.jitter_y = jy;

    void* up_res = nullptr;
    uint64_t up_off = 0;
    uint8_t* up_cpu = nullptr;
    uint64_t up_gpu = 0;
    if (!impl_->host.upload(impl_->host.ctx, sizeof(cb), 256, &up_res, &up_off, &up_cpu, &up_gpu)) return false;
    std::memcpy(up_cpu, &cb, sizeof(cb));

    ID3D12DescriptorHeap* heaps[] = {impl_->srv_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(impl_->cmp_rs.Get());
    cmd->SetPipelineState(impl_->pso_temporal_prep.Get());
    cmd->SetComputeRootConstantBufferView(0, up_gpu);
    cmd->SetComputeRootDescriptorTable(1, impl_->GpuSrv(base + 0));
    cmd->SetComputeRootDescriptorTable(2, impl_->GpuSrv(base + 3));
    cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);

    D3D12_RESOURCE_BARRIER uavs[3]{};
    uavs[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavs[0].UAV.pResource = out_mv;
    uavs[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavs[1].UAV.pResource = out_reactive;
    uavs[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavs[2].UAV.pResource = out_depth_r32f;
    cmd->ResourceBarrier(3, uavs);
    return true;
}

}  // namespace upscale

#endif  // _WIN32
