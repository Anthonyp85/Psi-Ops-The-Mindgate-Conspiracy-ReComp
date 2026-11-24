// SPDX-License-Identifier: GPL-2.0-or-later
// Clean-room C ABI declarations for dynamic loading of Intel XeSS 2 (libxess.dll) on Win64 D3D12.

#pragma once

#ifdef _WIN32

#include <cstdint>

struct ID3D12DescriptorHeap;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Heap;
struct ID3D12PipelineLibrary;
struct ID3D12Resource;

namespace xess_abi {

using ContextHandle = struct ContextOpaque*;
using Result = int32_t;

constexpr Result kResultSuccess = 0;

inline bool Succeeded(Result r) { return r >= 0; }

enum QualitySetting : int32_t {
    kQualityUltraPerformance = 100,
    kQualityPerformance = 101,
    kQualityBalanced = 102,
    kQualityQuality = 103,
    kQualityUltraQuality = 104,
    kQualityUltraQualityPlus = 105,
    kQualityAA = 106,
};

enum InitFlags : uint32_t {
    kInitNone = 0u,
    kInitHighResMv = 1u << 0,
    kInitInvertedDepth = 1u << 1,
    kInitExposureScaleTexture = 1u << 2,
    kInitResponsivePixelMask = 1u << 3,
    kInitUseNdcVelocity = 1u << 4,
    kInitExternalDescriptorHeap = 1u << 5,
    kInitLdrInputColor = 1u << 6,
    kInitJitteredMv = 1u << 7,
    kInitEnableAutoExposure = 1u << 8,
};

struct Extent2D {
    uint32_t x = 0;
    uint32_t y = 0;
};

struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    uint16_t reserved = 0;
};

struct D3D12InitParams {
    Extent2D outputResolution{};
    QualitySetting qualitySetting = kQualityQuality;
    uint32_t initFlags = kInitNone;
    uint32_t creationNodeMask = 0;
    uint32_t visibleNodeMask = 0;
    ID3D12Heap* pTempBufferHeap = nullptr;
    uint64_t bufferHeapOffset = 0;
    ID3D12Heap* pTempTextureHeap = nullptr;
    uint64_t textureHeapOffset = 0;
    ID3D12PipelineLibrary* pPipelineLibrary = nullptr;
};

struct D3D12ExecuteParams {
    ID3D12Resource* pColorTexture = nullptr;
    ID3D12Resource* pVelocityTexture = nullptr;
    ID3D12Resource* pDepthTexture = nullptr;
    ID3D12Resource* pExposureScaleTexture = nullptr;
    ID3D12Resource* pResponsivePixelMaskTexture = nullptr;
    ID3D12Resource* pOutputTexture = nullptr;
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    float exposureScale = 1.0f;
    uint32_t resetHistory = 0;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    Extent2D inputColorBase{};
    Extent2D inputMotionVectorBase{};
    Extent2D inputDepthBase{};
    Extent2D inputResponsiveMaskBase{};
    Extent2D outputColorBase{};
    ID3D12DescriptorHeap* pDescriptorHeap = nullptr;
    uint32_t descriptorHeapOffset = 0;
};

using PfnGetVersion = Result(__cdecl*)(Version* pVersion);
using PfnD3D12CreateContext = Result(__cdecl*)(ID3D12Device* pDevice, ContextHandle* phContext);
using PfnDestroyContext = Result(__cdecl*)(ContextHandle hContext);
using PfnD3D12Init = Result(__cdecl*)(ContextHandle hContext, const D3D12InitParams* pInitParams);
using PfnD3D12Execute = Result(__cdecl*)(ContextHandle hContext, ID3D12GraphicsCommandList* pCommandList,
                                         const D3D12ExecuteParams* pExecParams);
using PfnGetOptimalInputResolution = Result(__cdecl*)(ContextHandle hContext, const Extent2D* pOutputResolution,
                                                      QualitySetting qualitySettings, Extent2D* pInputResolutionOptimal,
                                                      Extent2D* pInputResolutionMin, Extent2D* pInputResolutionMax);
using PfnSetVelocityScale = Result(__cdecl*)(ContextHandle hContext, float x, float y);
using PfnSetJitterScale = Result(__cdecl*)(ContextHandle hContext, float x, float y);

}  // namespace xess_abi

#endif  // _WIN32
