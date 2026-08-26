// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "../src/PreviewCache.h"
#include "../src/AV1Settings.h"

#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(const char* name, bool ok)
{
    printf("  %-40s %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) ++g_failures;
}

std::wstring KeyPath(const std::wstring& root, const std::array<uint8_t, 32>& key)
{
    const std::string hex = av1imp::PreviewKeyHex(key);
    std::wstring wh(hex.begin(), hex.end());
    return root + L"\\v1\\" +
           std::wstring(1, wh[0]) + std::wstring(1, wh[1]) + L"\\" +
           std::wstring(1, wh[2]) + std::wstring(1, wh[3]) + L"\\" +
           wh + L".aepv";
}

} // namespace

int main()
{
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring root = temp;
    root += L"aether-preview-cache-test";
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_DIR", root.c_str());

    std::wstring settings = root + L"-settings.ini";
    av1imp::SetSettingsFilePathForTests(settings.c_str());
    av1imp::Settings set;
    set.previewCache = true;
    set.previewCacheMB = 256;
    Check("save test settings", av1imp::SaveSettings(set));

    av1imp::PreviewCache& cache = av1imp::PreviewCache::Instance();
    cache.Clear();

    av1imp::PreviewKeyInput in;
    in.source.valid = true;
    for (size_t i = 0; i < in.source.hash.size(); ++i) in.source.hash[i] = (uint8_t)i;
    in.frameIndex = 42;
    in.width = 4;
    in.height = 3;
    in.fastScaling = true;
    in.decoderName = "libdav1d";
    in.ffmpegVersion = "test";
    in.aetherVersion = "test";
    const auto key = av1imp::MakePreviewKey(in);

    auto changed = in;
    changed.frameIndex++;
    Check("key changes with frame", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.hardware = true;
    Check("key changes with backend", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.width++;
    Check("key changes with size", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.fastScaling = false;
    Check("key changes with scaling", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.decoderName = "other";
    Check("key changes with decoder", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.ffmpegVersion = "other";
    Check("key changes with runtime version", key != av1imp::MakePreviewKey(changed));
    changed = in;
    changed.source.hash[0] ^= 0xFF;
    Check("key changes with source identity", key != av1imp::MakePreviewKey(changed));

    const int width = 4, height = 3, stride = width * 4;
    std::vector<uint8_t> pixels((size_t)stride * height);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (uint8_t)(i * 7 + 3);

    Check("queue tight write", cache.QueueWrite(key, width, height, pixels.data(), stride));
    cache.FlushForTests();

    std::vector<uint8_t> read(pixels.size(), 0);
    Check("round trip tight", cache.TryRead(key, width, height, read.data(), stride) &&
                              read == pixels);

    std::vector<uint8_t> bottomUp(pixels.size(), 0);
    uint8_t* lastRow = bottomUp.data() + (height - 1) * stride;
    Check("read into negative stride",
          cache.TryRead(key, width, height, lastRow, -stride));
    bool negativeOk = true;
    for (int y = 0; y < height; ++y) {
        if (memcmp(bottomUp.data() + y * stride,
                   pixels.data() + (height - 1 - y) * stride,
                   stride) != 0) negativeOk = false;
    }
    Check("negative stride orientation", negativeOk);

    const std::wstring file = KeyPath(root, key);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        const uint32_t bad = 0;
        WriteFile(h, &bad, sizeof(bad), &wrote, nullptr);
        CloseHandle(h);
    }
    Check("bad magic becomes miss",
          !cache.TryRead(key, width, height, read.data(), stride));
    Check("bad record removed", GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);

    Check("rewrite after corruption", cache.QueueWrite(key, width, height, pixels.data(), stride));
    cache.FlushForTests();
    h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER shortFile;
        shortFile.QuadPart = 10;
        SetFilePointerEx(h, shortFile, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
        CloseHandle(h);
    }
    Check("truncated record becomes miss",
          !cache.TryRead(key, width, height, read.data(), stride));

    Check("first concurrent-key write",
          cache.QueueWrite(key, width, height, pixels.data(), stride));
    Check("second concurrent-key write",
          cache.QueueWrite(key, width, height, pixels.data(), stride));
    cache.FlushForTests();
    Check("same-key writes remain readable",
          cache.TryRead(key, width, height, read.data(), stride) && read == pixels);
    h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER end;
        end.QuadPart = -1;
        SetFilePointerEx(h, end, nullptr, FILE_END);
        uint8_t last = 0;
        DWORD got = 0;
        ReadFile(h, &last, 1, &got, nullptr);
        end.QuadPart = -1;
        SetFilePointerEx(h, end, nullptr, FILE_END);
        last ^= 0xFF;
        WriteFile(h, &last, 1, &got, nullptr);
        CloseHandle(h);
    }
    Check("bad CRC becomes miss",
          !cache.TryRead(key, width, height, read.data(), stride));

    Check("reject oversize payload",
          !cache.QueueWrite(key, 2048, 2048, pixels.data(), stride));
    Check("reject overflowing dimensions",
          !cache.QueueWrite(key, INT_MAX, INT_MAX, pixels.data(), stride));

    wchar_t sourcePath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, sourcePath);
    wcscat_s(sourcePath, L"аэтер кэш fingerprint.bin");
    h = CreateFileW(sourcePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wrote = 0;
    const char first[] = "first";
    WriteFile(h, first, sizeof(first), &wrote, nullptr);
    CloseHandle(h);
    const auto fp1 = av1imp::FingerprintSource([&] {
        const int need = WideCharToMultiByte(CP_UTF8, 0, sourcePath, -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)need, '\0');
        WideCharToMultiByte(CP_UTF8, 0, sourcePath, -1, &s[0], need, nullptr, nullptr);
        s.pop_back();
        return s;
    }());
    Sleep(10);
    h = CreateFileW(sourcePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    const char second[] = "second";
    WriteFile(h, second, sizeof(second), &wrote, nullptr);
    CloseHandle(h);
    const auto fp2 = av1imp::FingerprintSource([&] {
        const int need = WideCharToMultiByte(CP_UTF8, 0, sourcePath, -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)need, '\0');
        WideCharToMultiByte(CP_UTF8, 0, sourcePath, -1, &s[0], need, nullptr, nullptr);
        s.pop_back();
        return s;
    }());
    Check("source replacement changes fingerprint",
          fp1.valid && fp2.valid && fp1.hash != fp2.hash);

    const std::wstring lruDir = root + L"\\v1\\ff\\ee";
    SHCreateDirectoryExW(nullptr, lruDir.c_str(), nullptr);
    for (int i = 0; i < 3; ++i) {
        const std::wstring sparse = lruDir + L"\\lru-" + std::to_wstring(i) + L".aepv";
        h = CreateFileW(sparse.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD ignored = 0;
            DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
            LARGE_INTEGER end;
            end.QuadPart = 100ll * 1024ll * 1024ll;
            SetFilePointerEx(h, end, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            FILETIME when = {};
            ULARGE_INTEGER u;
            u.QuadPart = 132000000000000000ull + (uint64_t)i * 10000000ull;
            when.dwLowDateTime = u.LowPart;
            when.dwHighDateTime = u.HighPart;
            SetFileTime(h, nullptr, nullptr, &when);
            CloseHandle(h);
        }
    }
    cache.CleanupNowForTests();
    const av1imp::PreviewCacheUsage afterCleanup = cache.Usage();
    Check("LRU cleanup returns below 90 percent",
          afterCleanup.bytes <= (uint64_t)(256 * 1024 * 1024) * 9 / 10);

    std::wstring cyrRoot = temp;
    cyrRoot += L"аэтер preview cache dir";
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_DIR", cyrRoot.c_str());
    Check("cyrillic spaced dir write",
          cache.QueueWrite(key, width, height, pixels.data(), stride));
    cache.FlushForTests();
    Check("cyrillic spaced dir read",
          cache.TryRead(key, width, height, read.data(), stride) && read == pixels);

    cache.Clear();
    set.previewCache = false;
    Check("save disabled settings", av1imp::SaveSettings(set));
    av1imp::ReloadSettingsForTests();
    Check("disabled queue is rejected",
          !cache.QueueWrite(key, width, height, pixels.data(), stride));
    Check("disabled read is a miss",
          !cache.TryRead(key, width, height, read.data(), stride));

    cache.Clear();
    set.previewCache = true;
    Check("restore enabled settings", av1imp::SaveSettings(set));
    av1imp::ReloadSettingsForTests();
    const std::wstring asFile = root + L"-not-a-dir";
    h = CreateFileW(asFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_DIR", asFile.c_str());
    bool threw = false;
    try {
        cache.QueueWrite(key, width, height, pixels.data(), stride);
        cache.FlushForTests();
    } catch (...) {
        threw = true;
    }
    Check("file-as-dir does not throw", !threw);
    Check("file-as-dir read is a miss",
          !cache.TryRead(key, width, height, read.data(), stride));

    DeleteFileW(sourcePath);
    DeleteFileW(asFile.c_str());
    cache.Clear();
    cache.Shutdown();
    DeleteFileW(settings.c_str());
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_DIR", nullptr);
    av1imp::SetSettingsFilePathForTests(nullptr);

    printf("\n%s\n", g_failures == 0 ? "ALL PREVIEW CACHE CHECKS PASSED"
                                     : "PREVIEW CACHE CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
