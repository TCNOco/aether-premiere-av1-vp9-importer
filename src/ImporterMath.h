// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#pragma once

#include <cstdint>
#include <limits>

namespace av1imp {

inline int32_t SaturatingFrameDuration(int64_t frameCount,
                                       int32_t sampleSize,
                                       bool* saturated = nullptr)
{
    if (saturated) *saturated = false;
    if (frameCount <= 0 || sampleSize <= 0) return 0;

    const int64_t max = std::numeric_limits<int32_t>::max();
    if (frameCount > max / sampleSize) {
        if (saturated) *saturated = true;
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(frameCount * sampleSize);
}

} // namespace av1imp
