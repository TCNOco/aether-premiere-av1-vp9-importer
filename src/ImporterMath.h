// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#pragma once

#include <cstdint>
#include <limits>

namespace av1imp {

// Потолок ребра кадра. 8K DCI — 8192 по ширине, поэтому равенство проходит.
constexpr int kMaxFrameEdge = 8192;

inline bool UsableFrameSize(int width, int height)
{
    return width > 0 && height > 0 &&
           width <= kMaxFrameEdge && height <= kMaxFrameEdge;
}

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

// Такты Premiere на один кадр: ticksPerSecond * fpsDen / fpsNum.
// Знаковое переполнение на кривом AVRational — UB, не «странный fps».
inline bool TicksPerFrame(int64_t ticksPerSecond, int fpsNum, int fpsDen,
                          int64_t* out)
{
    if (out) *out = 0;
    if (!out || ticksPerSecond <= 0 || fpsNum <= 0 || fpsDen <= 0) return false;
    const int64_t max = std::numeric_limits<int64_t>::max();
    if (fpsDen > 0 && ticksPerSecond > max / fpsDen) return false;
    const int64_t ticks = (ticksPerSecond * static_cast<int64_t>(fpsDen)) / fpsNum;
    if (ticks <= 0) return false;
    *out = ticks;
    return true;
}

} // namespace av1imp
