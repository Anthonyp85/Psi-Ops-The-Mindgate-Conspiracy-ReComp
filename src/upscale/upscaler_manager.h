// SPDX-License-Identifier: GPL-2.0-or-later
// Cross-platform 9-Upscaler Manager (Win64 D3D12 + Native Linux Vulkan 1.2)
// implementing gpu::Upscaler for XboxXBERecomp.

#pragma once

#include "gpu/backend.h"

namespace upscale {

// Returns the process-wide singleton gpu::Upscaler implementation for the active platform
// (Win64 D3D12 9-upscaler manager or Linux Vulkan 1.2 6-spatial + graceful temporal fallback manager).
gpu::Upscaler* GetManager();

// Registers GetManager() with gpu::SetUpscaler(). Safe to call from app::kGame.init().
void RegisterUpscaler();

}  // namespace upscale
