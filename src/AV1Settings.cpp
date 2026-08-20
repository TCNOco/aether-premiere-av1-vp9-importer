#include "AV1Settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace av1imp {

namespace {

// Ниже этого числа логических процессоров считаем, что программный декодер
// не вытянет, и берём видеокарту. Порог взят с потолка: замерить получилось
// только на одной машине (16 потоков), где процессор выигрывал вшестеро.
const DWORD kSoftwareNeedsThreads = 8;

bool LooksDisabled(const wchar_t* v)
{
    return _wcsicmp(v, L"0") == 0 || _wcsicmp(v, L"off") == 0 ||
           _wcsicmp(v, L"false") == 0 || _wcsicmp(v, L"no") == 0;
}

const wchar_t* SettingsFolder()
{
    static wchar_t folder[MAX_PATH] = {};
    if (folder[0]) return folder;

    wchar_t* base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        swprintf_s(folder, MAX_PATH, L"%s\\AV1Importer", base);
        CoTaskMemFree(base);
    }
    return folder;
}

} // namespace

const wchar_t* SettingsFilePath()
{
    static wchar_t path[MAX_PATH] = {};
    if (path[0]) return path;

    const wchar_t* folder = SettingsFolder();
    if (folder[0]) swprintf_s(path, MAX_PATH, L"%s\\settings.ini", folder);
    return path;
}

DecodeMode CurrentMode()
{
    // Переменная среды главнее файла и ничего не сохраняет
    wchar_t env[64] = {};
    if (GetEnvironmentVariableW(L"AV1IMPORTER_HARDWARE", env, 64) > 0) {
        return LooksDisabled(env) ? DecodeMode::Software : DecodeMode::Hardware;
    }

    const wchar_t* path = SettingsFilePath();
    if (!path[0]) return DecodeMode::Auto;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt") != 0 || !f) return DecodeMode::Auto;

    DecodeMode mode = DecodeMode::Auto;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, "decode", 6) != 0) continue;

        p += 6;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;

        char value[32] = {};
        if (sscanf_s(p, "%31s", value, (unsigned)sizeof(value)) == 1) {
            if (_stricmp(value, "software") == 0)      mode = DecodeMode::Software;
            else if (_stricmp(value, "hardware") == 0) mode = DecodeMode::Hardware;
        }
        break;
    }

    fclose(f);
    return mode;
}

bool SaveMode(DecodeMode mode)
{
    const wchar_t* folder = SettingsFolder();
    if (!folder[0]) return false;
    CreateDirectoryW(folder, nullptr);   // если уже есть — не беда

    const wchar_t* path = SettingsFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt") != 0 || !f) return false;

    const char* value = mode == DecodeMode::Software ? "software"
                      : mode == DecodeMode::Hardware ? "hardware"
                                                     : "auto";

    // Файл человекочитаемый: его могут править руками по совету из issue
    fprintf(f, "; AV1 / VP9 Importer for Premiere Pro\n");
    fprintf(f, "; decode = auto | software | hardware\n");
    fprintf(f, "decode=%s\n", value);
    fclose(f);
    return true;
}

bool PreferHardware()
{
    switch (CurrentMode()) {
        case DecodeMode::Software: return false;
        case DecodeMode::Hardware: return true;
        default: break;
    }

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors < kSoftwareNeedsThreads;
}

} // namespace av1imp
