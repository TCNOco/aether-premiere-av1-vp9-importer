// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Постоянный кэш уменьшенных BGRA8-превью.
//
// Это ускорение, не источник истины: любая ошибка диска — обычный miss.
// Полноразмерное воспроизведение сюда не ходит.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace av1imp {

constexpr uint32_t kPreviewCacheKeySchema = 2;
constexpr size_t   kPreviewCacheMaxPayload = 2u * 1024u * 1024u;

struct SourceFingerprint {
    std::array<uint8_t, 32> hash{};
    bool valid = false;
};

struct PreviewKeyInput {
    SourceFingerprint source;
    int         videoStream = 0;
    int64_t     frameIndex  = 0;
    int         width       = 0;
    int         height      = 0;
    bool        fastScaling = true;
    bool        hardware    = false;
    std::string decoderName;
    std::string ffmpegVersion;
    std::string aetherVersion;
};

struct PreviewCacheUsage {
    uint64_t     bytes = 0;
    uint64_t     files = 0;
    std::wstring directory;
};

struct PreviewCacheProcessStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t writesQueued = 0;
    uint64_t writesDropped = 0;
    double   readMs = 0;
    std::string lastWarning;
};

SourceFingerprint FingerprintSource(const std::string& utf8Path);
std::array<uint8_t, 32> MakePreviewKey(const PreviewKeyInput& in);
std::string PreviewKeyHex(const std::array<uint8_t, 32>& key);

void CopyBgraWithStride(const uint8_t* src, int srcStride,
                        uint8_t* dst, int dstStride,
                        int width, int height);

class PreviewCache {
public:
    static PreviewCache& Instance();

    void Shutdown();

    bool TryRead(const std::array<uint8_t, 32>& key,
                 int width, int height,
                 uint8_t* dst, int dstStride);

    bool QueueWrite(const std::array<uint8_t, 32>& key,
                    int width, int height,
                    const uint8_t* src, int srcStride);

    PreviewCacheUsage Usage() const;
    bool Clear();
    PreviewCacheProcessStats ProcessStats() const;
    std::wstring Directory() const;
    std::string Json() const;

    void FlushForTests();
    void CleanupNowForTests();

private:
    PreviewCache() = default;
};

} // namespace av1imp
