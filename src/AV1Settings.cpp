#include "AV1Settings.h"
#include "AV1Log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace av1imp {

namespace {

// «Выключено» понимаем широко: человек, отключающий аппаратный декодер по совету
// из issue, напишет что угодно из этого, и спорить с ним — плохая идея.
bool LooksDisabled(const wchar_t* value)
{
    return _wcsicmp(value, L"0")     == 0 ||
           _wcsicmp(value, L"off")   == 0 ||
           _wcsicmp(value, L"false") == 0 ||
           _wcsicmp(value, L"no")    == 0;
}

// Ищем hardware=... в AV1Importer.ini рядом с плагином.
// Формат намеренно простейший: файл правят руками, часто в спешке.
bool ReadHardwareFromFile(bool& outEnabled)
{
    wchar_t path[MAX_PATH] = {};
    if (swprintf_s(path, MAX_PATH, L"%sAV1Importer.ini", PluginDirectory()) < 0) {
        return false;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt") != 0 || !f) return false;

    bool found = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, "hardware", 8) != 0) continue;

        p += 8;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;

        char value[32] = {};
        sscanf_s(p, "%31s", value, (unsigned)sizeof(value));

        wchar_t wide[32] = {};
        MultiByteToWideChar(CP_ACP, 0, value, -1, wide, 32);

        outEnabled = !LooksDisabled(wide);
        found = true;
        break;
    }

    fclose(f);
    return found;
}

} // namespace

bool HardwareDecodingEnabled()
{
    static bool resolved = false;
    static bool enabled  = true;

    if (resolved) return enabled;
    resolved = true;

    // Переменная среды главнее файла: ею удобно проверить догадку, не трогая
    // установленный плагин, и она же работает, если папка защищена от записи
    wchar_t env[64] = {};
    if (GetEnvironmentVariableW(L"AV1IMPORTER_HARDWARE", env, 64) > 0) {
        enabled = !LooksDisabled(env);
        Log("settings: hardware decoding %s (AV1IMPORTER_HARDWARE)",
            enabled ? "on" : "OFF");
        return enabled;
    }

    bool fromFile = true;
    if (ReadHardwareFromFile(fromFile)) {
        enabled = fromFile;
        Log("settings: hardware decoding %s (AV1Importer.ini)",
            enabled ? "on" : "OFF");
        return enabled;
    }

    return enabled;   // по умолчанию включено, в журнал не пишем
}

} // namespace av1imp
