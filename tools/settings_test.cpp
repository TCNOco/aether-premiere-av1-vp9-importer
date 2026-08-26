// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/IniSettings.h"
#include "../src/AV1Settings.h"

namespace {

int g_failures = 0;

void Check(const char* name, const std::string& actual, const std::string& expected)
{
    if (actual == expected) {
        printf("  %-36s OK\n", name);
        return;
    }
    printf("  %-36s FAIL\n    expected: %s\n    actual:   %s\n",
           name, expected.c_str(), actual.c_str());
    ++g_failures;
}

void CheckBool(const char* name, bool ok)
{
    printf("  %-36s %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) ++g_failures;
}

} // namespace

int main()
{
    using av1imp::SetIniValue;

    Check("new file",
          SetIniValue("", "decode", "auto"),
          "decode=auto\n");

    Check("replace while preserving switches",
          SetIniValue("; settings\r\ndecode=auto\r\nasync=off\r\nyuv=off\r\n",
                      "decode", "hardware"),
          "; settings\r\ndecode=hardware\r\nasync=off\r\nyuv=off\r\n");

    Check("append after unknown settings",
          SetIniValue("async=off\nyuv=off", "decode", "software"),
          "async=off\nyuv=off\ndecode=software\n");

    Check("case and whitespace",
          SetIniValue("  DeCoDe \t= auto\nother=value\n", "decode", "hardware"),
          "decode=hardware\nother=value\n");

    Check("collapse duplicate keys",
          SetIniValue("decode=auto\nasync=off\ndecode=software\nyuv=off\n",
                      "decode", "hardware"),
          "decode=hardware\nasync=off\nyuv=off\n");

    Check("do not confuse key prefixes",
          SetIniValue("decoder=custom\ndecode_mode=manual\n", "decode", "auto"),
          "decoder=custom\ndecode_mode=manual\ndecode=auto\n");

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, L"aether-settings-test.ini");
    DeleteFileW(tmp);
    av1imp::SetSettingsFilePathForTests(tmp);
    av1imp::ReloadSettingsForTests();

    av1imp::Settings defaults = av1imp::CurrentSettings();
    CheckBool("settings defaults",
              defaults.enabled &&
              defaults.decode == av1imp::DecodeMode::Auto &&
              defaults.memoryCacheMB == 512 &&
              !defaults.previewCache &&
              defaults.previewCacheMB == 2048);

    FILE* f = nullptr;
    _wfopen_s(&f, tmp, L"wb");
    const char* ini =
        "; keep me\r\n"
        "enabled=off\r\n"
        "decode=software\r\n"
        "cache_memory_mb=0\r\n"
        "preview_cache=off\r\n"
        "preview_cache_mb=99999\r\n"
        "unknown=yes\r\n";
    fwrite(ini, 1, strlen(ini), f);
    fclose(f);
    av1imp::ReloadSettingsForTests();
    av1imp::Settings parsed = av1imp::CurrentSettings();
    CheckBool("parse and clamp",
              !parsed.enabled &&
              parsed.decode == av1imp::DecodeMode::Software &&
              parsed.memoryCacheMB == 0 &&
              !parsed.previewCache &&
              parsed.previewCacheMB == 20480 &&
              av1imp::MemoryCacheLimitBytes() == 0);

    SetEnvironmentVariableW(L"AETHER_ENABLED", L"on");
    SetEnvironmentVariableW(L"AETHER_CACHE_MB", L"1024");
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE", L"on");
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_MB", L"256");
    av1imp::ReloadSettingsForTests();
    parsed = av1imp::CurrentSettings();
    CheckBool("environment wins",
              parsed.enabled && parsed.memoryCacheMB == 1024 &&
              parsed.previewCache && parsed.previewCacheMB == 256);
    SetEnvironmentVariableW(L"AETHER_ENABLED", nullptr);
    SetEnvironmentVariableW(L"AETHER_CACHE_MB", nullptr);
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE", nullptr);
    SetEnvironmentVariableW(L"AETHER_PREVIEW_CACHE_MB", nullptr);
    av1imp::ReloadSettingsForTests();

    parsed.enabled = true;
    parsed.decode = av1imp::DecodeMode::Hardware;
    parsed.memoryCacheMB = 256;
    parsed.previewCache = true;
    parsed.previewCacheMB = 4096;
    CheckBool("save shared model", av1imp::SaveSettings(parsed));

    std::string saved;
    _wfopen_s(&f, tmp, L"rb");
    char buffer[4096] = {};
    const size_t got = fread(buffer, 1, sizeof(buffer), f);
    saved.assign(buffer, got);
    fclose(f);
    CheckBool("save preserves unknown lines",
              saved.find("; keep me") != std::string::npos &&
              saved.find("unknown=yes") != std::string::npos &&
              saved.find("enabled=on") != std::string::npos &&
              saved.find("cache_memory_mb=256") != std::string::npos);

    DeleteFileW(tmp);
    av1imp::SetSettingsFilePathForTests(nullptr);

    printf("\n%s\n", g_failures == 0 ? "ALL SETTINGS CHECKS PASSED"
                                     : "SETTINGS CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
