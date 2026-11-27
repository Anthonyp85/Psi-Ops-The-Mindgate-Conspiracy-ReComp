// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace psiops {

inline constexpr char kUsaXbeSha1[] = "42dd95fa444039b82f843ff02a31833da97d41cf";
inline constexpr char kEuropeXbeSha1[] = "c3a77918a64407a209c5edfda6a9d2123c912e63";
inline constexpr uint32_t kEuropeEntry = 0x36F017u;

bool EuropeanGameLoaded();

}  // namespace psiops
