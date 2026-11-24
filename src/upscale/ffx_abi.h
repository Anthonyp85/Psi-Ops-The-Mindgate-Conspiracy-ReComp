// SPDX-License-Identifier: GPL-2.0-or-later
// Clean-room C ABI declarations for dynamic loading of AMD FidelityFX API v1.x (FSR 3.1) on Win64 D3D12.

#pragma once

#ifdef _WIN32

#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;

namespace ffx_abi {

using Context = void*;
using ReturnCode = uint32_t;

constexpr ReturnCode kReturnOk = 0u;

constexpr uint64_t kDescTypeBackendDx12 = 0x00000002ull;
constexpr uint64_t kDescTypeCreateUpscale = 0x00010000ull;
constexpr uint64_t kDescTypeDispatchUpscale = 0x00010001ull;

enum ResourceType : uint32_t {
    kResourceBuffer = 0u,
    kResourceTexture1D = 1u,
    kResourceTexture2D = 2u,
    kResourceTextureCube = 3u,
    kResourceTexture3D = 4u,
};

enum SurfaceFormat : uint32_t {
    kFmtUnknown = 0u,
    kFmtR16G16B16A16Float = 4u,
    kFmtR8G8B8A8Unorm = 10u,
    kFmtB8G8R8A8Unorm = 14u,
    kFmtR16G16Float = 18u,
    kFmtR8Unorm = 25u,
    kFmtR32Float = 28u,
};

enum ResourceUsage : uint32_t {
    kUsageReadOnly = 0u,
    kUsageRenderTarget = 1u << 0,
    kUsageUav = 1u << 1,
    kUsageDepthTarget = 1u << 2,
};

enum ResourceState : uint32_t {
    kStateCommon = 1u << 0,
    kStateUnorderedAccess = 1u << 1,
    kStateComputeRead = 1u << 2,
    kStatePixelRead = 1u << 3,
    kStatePixelComputeRead = kStateComputeRead | kStatePixelRead,
    kStateCopySrc = 1u << 4,
    kStateCopyDest = 1u << 5,
    kStateRenderTarget = 1u << 8,
};

enum CreateUpscaleFlags : uint32_t {
    kCreateHdr = 1u << 0,
    kCreateDisplayResMv = 1u << 1,
    kCreateMvJitterOffset = 1u << 2,
    kCreateDepthInverted = 1u << 3,
    kCreateDepthInfinite = 1u << 4,
    kCreateAutoExposure = 1u << 5,
    kCreateDynamicResolution = 1u << 6,
};

struct Header {
    uint64_t type = 0;
    Header* pNext = nullptr;
};

struct Dimensions2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FloatCoords2D {
    float x = 0.0f;
    float y = 0.0f;
};

struct ResourceDescription {
    uint32_t type = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipCount = 1;
    uint32_t flags = 0;
    uint32_t usage = 0;
};

struct Resource {
    void* resource = nullptr;
    ResourceDescription description{};
    uint32_t state = 0;
};

using MessageCallback = void(__cdecl*)(uint32_t type, const wchar_t* message);

struct AllocationCallbacks {
    void* pUserData = nullptr;
    void*(__cdecl* alloc)(void* pUserData, uint64_t size) = nullptr;
    void(__cdecl* dealloc)(void* pUserData, void* pMem) = nullptr;
};

struct CreateBackendDx12Desc {
    Header header{kDescTypeBackendDx12, nullptr};
    ID3D12Device* device = nullptr;
};

struct CreateContextDescUpscale {
    Header header{kDescTypeCreateUpscale, nullptr};
    uint32_t flags = 0;
    Dimensions2D maxRenderSize{};
    Dimensions2D maxUpscaleSize{};
    MessageCallback fpMessage = nullptr;
};

struct DispatchDescUpscale {
    Header header{kDescTypeDispatchUpscale, nullptr};
    void* commandList = nullptr;
    Resource color{};
    Resource depth{};
    Resource motionVectors{};
    Resource exposure{};
    Resource reactive{};
    Resource transparencyAndComposition{};
    Resource output{};
    FloatCoords2D jitterOffset{};
    FloatCoords2D motionVectorScale{};
    Dimensions2D renderSize{};
    Dimensions2D upscaleSize{};
    bool enableSharpening = false;
    float sharpness = 0.0f;
    float frameTimeDelta = 16.667f;
    float preExposure = 1.0f;
    bool reset = false;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    float cameraFovAngleVertical = 1.0f;
    float viewSpaceToMetersFactor = 1.0f;
    uint32_t flags = 0;
};

using PfnCreateContext = ReturnCode(__cdecl*)(Context* context, Header* desc, const AllocationCallbacks* mem_cb);
using PfnDestroyContext = ReturnCode(__cdecl*)(Context* context, const AllocationCallbacks* mem_cb);
using PfnConfigure = ReturnCode(__cdecl*)(Context* context, const Header* desc);
using PfnQuery = ReturnCode(__cdecl*)(Context* context, Header* desc);
using PfnDispatch = ReturnCode(__cdecl*)(Context* context, const Header* desc);

}  // namespace ffx_abi

#endif  // _WIN32
