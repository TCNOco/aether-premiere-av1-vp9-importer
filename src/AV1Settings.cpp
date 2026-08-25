// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "AV1Settings.h"
#include "IniSettings.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

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
        swprintf_s(folder, MAX_PATH, L"%s\\Aether", base);

        // Переезд со старого имени. Настройка тут одна — процессор или
        // видеокарта, — но потерять её значит без причины вернуть человека
        // к окну настроек. Переносим один раз: если в новой папке файла ещё
        // нет, а в старой есть.
        wchar_t oldFile[MAX_PATH] = {};
        wchar_t newFile[MAX_PATH] = {};
        swprintf_s(oldFile, MAX_PATH, L"%s\\AV1Importer\\settings.ini", base);
        swprintf_s(newFile, MAX_PATH, L"%s\\Aether\\settings.ini", base);
        CoTaskMemFree(base);

        if (GetFileAttributesW(newFile) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(oldFile) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(folder, nullptr);
            CopyFileW(oldFile, newFile, TRUE);
        }
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
    if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    const wchar_t* path = SettingsFilePath();
    const char* value = mode == DecodeMode::Software ? "software"
                      : mode == DecodeMode::Hardware ? "hardware"
                                                     : "auto";

    // Читаем файл целиком до открытия на запись: кроме decode в нём живут
    // аварийные выключатели async/yuv и могут появиться будущие настройки.
    // Сохранение одного переключателя не имеет права стирать остальные.
    std::string existing;
    FILE* input = nullptr;
    const errno_t openResult = _wfopen_s(&input, path, L"rb");
    if (openResult == 0 && input) {
        char chunk[4096];
        size_t got = 0;
        while ((got = fread(chunk, 1, sizeof(chunk), input)) > 0) {
            existing.append(chunk, got);
        }
        if (ferror(input)) {
            fclose(input);
            return false;
        }
        fclose(input);
    } else {
        const DWORD attributes = GetFileAttributesW(path);
        const DWORD error = GetLastError();
        if (attributes != INVALID_FILE_ATTRIBUTES ||
            (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
            return false;
        }
    }

    if (existing.empty()) {
        existing = "; AV1 / VP9 Importer for Premiere Pro\r\n"
                   "; decode = auto | software | hardware\r\n";
    }
    const std::string updated = SetIniValue(existing, "decode", value);

    // Пишем сначала рядом, затем заменяем одним MoveFileEx: оборванная запись
    // не должна оставить Premiere с наполовину пустым settings.ini.
    std::wstring temporary = path;
    temporary += L".tmp";

    FILE* output = nullptr;
    if (_wfopen_s(&output, temporary.c_str(), L"wb") != 0 || !output) return false;
    const bool written =
        fwrite(updated.data(), 1, updated.size(), output) == updated.size() &&
        fflush(output) == 0;
    const bool closed = fclose(output) == 0;
    if (!written || !closed) {
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
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

namespace {

// Выключатель «включено, пока явно не выключили»: переменная среды главнее
// файла, как и у выбора декодера.
//
// Вынесено в одно место, потому что таких выключателей стало два, а разбор
// строки у них был бы посимвольно одинаковый. Второй копии достаточно, чтобы
// они разъехались — и разъезжаются такие двойники в сторону редкого случая,
// где никто не смотрит.
bool EnabledUnlessTurnedOff(const wchar_t* envName, const char* key)
{
    wchar_t env[64] = {};
    if (GetEnvironmentVariableW(envName, env, 64) > 0) {
        return !LooksDisabled(env);
    }

    const wchar_t* path = SettingsFilePath();
    if (!path[0]) return true;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt") != 0 || !f) return true;

    const size_t keyLen = strlen(key);
    bool enabled = true;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, key, keyLen) != 0) continue;

        p += keyLen;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;

        char value[32] = {};
        if (sscanf_s(p, "%31s", value, (unsigned)sizeof(value)) == 1) {
            if (_stricmp(value, "off")   == 0 || _stricmp(value, "0")     == 0 ||
                _stricmp(value, "false") == 0 || _stricmp(value, "no")    == 0) {
                enabled = false;
            }
        }
    }
    fclose(f);
    return enabled;
}

} // namespace

// Асинхронная выдача. По умолчанию включена — выключенная возможность никем
// не проверяется и тихо гниёт, — но выключатель есть, потому что это отдельный
// поток внутри чужого процесса, и если он однажды окажется виноват, человек
// должен уметь его отключить, не удаляя плагин.
bool AsyncDeliveryEnabled()
{
    return EnabledUnlessTurnedOff(L"AETHER_ASYNC", "async");
}

// Выдача кадров без перевода в RGB. По умолчанию включена по той же причине,
// что и асинхронная, и выключатель есть по той же: путь новый, а решение
// «беру или не беру» принимает хост, и предсказать его поведение на всех
// версиях Adobe мы не можем. Если где-то цвет уедет, человек должен уметь
// вернуться на прежний путь строкой в файле, а не удалением плагина.
bool YuvEnabled()
{
    return EnabledUnlessTurnedOff(L"AETHER_YUV", "yuv");
}

} // namespace av1imp
