// SPDX-License-Identifier: GPL-2.0-or-later
// Clean-room C/C++ ABI declarations for dynamic loading of NVIDIA NGX / DLSS 4 on Win64 D3D12.

#pragma once

#ifdef _WIN32

#include <cstdint>

struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace ngx_abi {

using Result = uint32_t;
constexpr Result kResultSuccess = 0x00000001u;
constexpr uint32_t kResultFailMask = 0xFFF00000u;
constexpr uint32_t kResultFailTag = 0xBAD00000u;

inline bool Succeeded(Result r) { return (r & kResultFailMask) != kResultFailTag && r != 0u; }

constexpr uint32_t kVersionApi21 = 0x00000015u;
constexpr uint32_t kVersionApi20 = 0x00000014u;
constexpr int kFeatureSuperSampling = 1;

enum PerfQuality : int {
    kPerfQualityMaxPerf = 0,
    kPerfQualityBalanced = 1,
    kPerfQualityMaxQuality = 2,
    kPerfQualityUltraPerformance = 3,
    kPerfQualityUltraQuality = 4,
    kPerfQualityDlaa = 5,
};

enum FeatureFlags : uint32_t {
    kFlagNone = 0u,
    kFlagIsHdr = 1u << 0,
    kFlagMvLowRes = 1u << 1,
    kFlagMvJittered = 1u << 2,
    kFlagDepthInverted = 1u << 3,
    kFlagDoSharpening = 1u << 5,
    kFlagAutoExposure = 1u << 6,
};

enum RenderPreset : uint32_t {
    kPresetDefault = 0u,
    kPresetE = 5u,
    kPresetJ = 10u,
    kPresetK = 11u,
};

struct Handle {
    uint32_t id;
};

struct PathListInfo {
    const wchar_t* const* path = nullptr;
    uint32_t length = 0;
};

using LoggingCallback = void(__cdecl*)(const char* message, int logging_level, int source_component);

struct LoggingInfo {
    LoggingCallback callback = nullptr;
    int minimum_logging_level = 0;
    bool disable_other_logging_sinks = false;
};

struct FeatureCommonInfo {
    PathListInfo path_list_info{};
    void* internal_data = nullptr;
    LoggingInfo logging_info{};
};

// Matches MSVC C++ vtable layout of NVSDK_NGX_Parameter in NVIDIA NGX D3D12 runtime.
struct Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;

    virtual Result Get(const char* name, unsigned long long* out_value) const = 0;
    virtual Result Get(const char* name, float* out_value) const = 0;
    virtual Result Get(const char* name, double* out_value) const = 0;
    virtual Result Get(const char* name, unsigned int* out_value) const = 0;
    virtual Result Get(const char* name, int* out_value) const = 0;
    virtual Result Get(const char* name, ID3D11Resource** out_value) const = 0;
    virtual Result Get(const char* name, ID3D12Resource** out_value) const = 0;
    virtual Result Get(const char* name, void** out_value) const = 0;

    virtual void Reset() = 0;
};

using ProgressCallback = void(__cdecl*)(float current_progress, bool& out_should_cancel);

using PfnD3D12InitExt = Result(__cdecl*)(unsigned long long app_id, const wchar_t* app_data_path,
                                         ID3D12Device* device, uint32_t sdk_version,
                                         const FeatureCommonInfo* feature_info);
using PfnD3D12Shutdown1 = Result(__cdecl*)(ID3D12Device* device);
using PfnD3D12Shutdown = Result(__cdecl*)();
using PfnD3D12GetCapabilityParameters = Result(__cdecl*)(Parameter** out_params);
using PfnD3D12AllocateParameters = Result(__cdecl*)(Parameter** out_params);
using PfnD3D12DestroyParameters = Result(__cdecl*)(Parameter* params);
using PfnD3D12CreateFeature = Result(__cdecl*)(ID3D12GraphicsCommandList* cmd_list, int feature_id, Parameter* params,
                                               Handle** out_handle);
using PfnD3D12ReleaseFeature = Result(__cdecl*)(Handle* handle);
using PfnD3D12EvaluateFeature = Result(__cdecl*)(ID3D12GraphicsCommandList* cmd_list, const Handle* handle,
                                                 const Parameter* params, ProgressCallback progress_cb);

// Standard NGX parameter string keys
inline constexpr const char* kParamSuperSamplingAvailable = "SuperSampling.Available";
inline constexpr const char* kParamSuperSamplingNeedsUpdatedDriver = "SuperSampling.NeedsUpdatedDriver";
inline constexpr const char* kParamSuperSamplingInitResult = "SuperSampling.FeatureInitResult";
inline constexpr const char* kParamWidth = "Width";
inline constexpr const char* kParamHeight = "Height";
inline constexpr const char* kParamOutWidth = "OutWidth";
inline constexpr const char* kParamOutHeight = "OutHeight";
inline constexpr const char* kParamPerfQualityValue = "PerfQualityValue";
inline constexpr const char* kParamCreationNodeMask = "CreationNodeMask";
inline constexpr const char* kParamVisibilityNodeMask = "VisibilityNodeMask";
inline constexpr const char* kParamDlssCreateFlags = "DLSS.Feature.Create.Flags";
inline constexpr const char* kParamRtxValue = "RTXValue";
inline constexpr const char* kParamColor = "Color";
inline constexpr const char* kParamOutput = "Output";
inline constexpr const char* kParamDepth = "Depth";
inline constexpr const char* kParamMotionVectors = "MotionVectors";
inline constexpr const char* kParamJitterOffsetX = "Jitter.Offset.X";
inline constexpr const char* kParamJitterOffsetY = "Jitter.Offset.Y";
inline constexpr const char* kParamReset = "Reset";
inline constexpr const char* kParamMvScaleX = "MV.Scale.X";
inline constexpr const char* kParamMvScaleY = "MV.Scale.Y";
inline constexpr const char* kParamSharpness = "Sharpness";
inline constexpr const char* kParamRenderSubrectW = "DLSS.Render.Subrect.Dimensions.Width";
inline constexpr const char* kParamRenderSubrectH = "DLSS.Render.Subrect.Dimensions.Height";
inline constexpr const char* kParamBiasCurrentColorMask = "DLSS.Input.Bias.Current.Color.Mask";
inline constexpr const char* kParamPresetDlaa = "DLSS.Hint.Render.Preset.DLAA";
inline constexpr const char* kParamPresetUltraQuality = "DLSS.Hint.Render.Preset.UltraQuality";
inline constexpr const char* kParamPresetQuality = "DLSS.Hint.Render.Preset.Quality";
inline constexpr const char* kParamPresetBalanced = "DLSS.Hint.Render.Preset.Balanced";
inline constexpr const char* kParamPresetPerformance = "DLSS.Hint.Render.Preset.Performance";
inline constexpr const char* kParamPresetUltraPerformance = "DLSS.Hint.Render.Preset.UltraPerformance";

}  // namespace ngx_abi

#endif  // _WIN32
