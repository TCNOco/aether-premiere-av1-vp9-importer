// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Проверка готового .prm без запуска Premiere.
//
//   plugin_test.exe <путь к AV1Importer.prm>
//
// Отвечает на три вопроса, из-за которых плагин чаще всего "просто не появляется"
// в Premiere, причём молча:
//   1. библиотека вообще загружается;
//   2. она сама нашла ffmpeg в своей папке (иначе первый же кадр уронит Premiere);
//   3. точка входа на месте и отвечает на запросы.
//
// Premiere при отказе плагина ничего не сообщает — он просто отсутствует в списке.
// Поэтому дешевле проверять здесь, чем гадать в Premiere.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKMALErrors.h"

typedef prMALError (*ImportEntryProc)(csSDK_int32, imStdParms*, void*, void*);

// Чем плагин собирается связаться при загрузке. Читаем таблицу импорта прямо
// из файла, без запуска: список нужен раньше, чем LoadLibrary скажет своё слово.
bool ImportsMsvcRuntime(const wchar_t* path, std::string* found)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { CloseHandle(file); return false; }

    BYTE* base = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) { CloseHandle(mapping); CloseHandle(file); return false; }

    bool hit = false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        IMAGE_NT_HEADERS64* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            // Адреса в таблицах виртуальные, а файл лежит как есть — переводим
            // через таблицу секций
            IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            auto toOffset = [&](DWORD rva) -> BYTE* {
                for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    const DWORD start = sections[i].VirtualAddress;
                    const DWORD size  = sections[i].Misc.VirtualSize;
                    if (rva >= start && rva < start + size) {
                        return base + sections[i].PointerToRawData + (rva - start);
                    }
                }
                return nullptr;
            };

            const DWORD importRva =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            IMAGE_IMPORT_DESCRIPTOR* imp =
                reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(toOffset(importRva));

            for (; imp && imp->Name; ++imp) {
                const char* name = reinterpret_cast<const char*>(toOffset(imp->Name));
                if (!name) break;
                if (_strnicmp(name, "MSVCP", 5) == 0 || _strnicmp(name, "VCRUNTIME", 9) == 0) {
                    if (!found->empty()) *found += ", ";
                    *found += name;
                    hit = true;
                }
            }
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return hit;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        printf("Usage: plugin_test <path to AV1Importer.prm>\n");
        return 1;
    }

    HMODULE plugin = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!plugin) {
        printf("FAIL: library did not load, error %lu\n", GetLastError());
        return 2;
    }
    printf("load       : ok\n");

    // Если DllMain отработал, библиотеки ffmpeg уже в процессе.
    // Проверяем именно так: обращения к самим функциям здесь ещё не было.
    const wchar_t* modules[] = { L"avutil-60.dll", L"avcodec-62.dll",
                                 L"avformat-62.dll", L"swscale-9.dll" };
    bool ffmpegOk = true;
    for (const wchar_t* m : modules) {
        if (!GetModuleHandleW(m)) {
            wprintf(L"FAIL: %s was not preloaded\n", m);
            ffmpegOk = false;
        }
    }
    printf("ffmpeg     : %s\n", ffmpegOk ? "preloaded from the plug-in folder" : "NOT FOUND");

    ImportEntryProc entry = (ImportEntryProc)GetProcAddress(plugin, "xImportEntry");
    if (!entry) {
        printf("FAIL: no xImportEntry export\n");
        return 3;
    }
    printf("entry      : ok\n");

    // imInit: плагин рассказывает о себе. Полей stdParms он здесь не касается,
    // поэтому пустой структуры достаточно.
    imStdParms stdParms = {};
    stdParms.imInterfaceVer = IMPORTMOD_VERSION;

    imImportInfoRec info = {};
    prMALError r = entry(imInit, &stdParms, &info, nullptr);
    printf("imInit     : result=%d, priority=%d\n", r, info.priority);

    // imGetIndFormat: какие файлы плагин берёт на себя
    imIndFormatRec fmt = {};
    r = entry(imGetIndFormat, &stdParms, reinterpret_cast<void*>(0), &fmt);
    if (r != malNoError) {
        printf("FAIL: imGetIndFormat returned %d\n", r);
        return 4;
    }

    printf("format     : %s (%s)\n", fmt.FormatName, fmt.FormatShortName);
    printf("flags      : 0x%04x\n", fmt.flags);
    printf("extensions : ");
    for (const char* p = fmt.PlatformExtension; *p; p += strlen(p) + 1) {
        printf("%s ", p);
    }
    printf("\n");

    // Второй индекс должен отсекаться — иначе Premiere зациклится на опросе
    r = entry(imGetIndFormat, &stdParms, reinterpret_cast<void*>(1), &fmt);
    printf("index 1    : %s\n", r == imBadFormatIndex ? "correctly rejected" : "WRONG");

    // Среда выполнения C++ должна быть внутри плагина, а не снаружи.
    // Premiere Pro CC 2019 держит рядом с собой свою msvcp140.dll от VS2015,
    // Windows ищет библиотеки по имени среди уже загруженных в процесс — и наш
    // плагин получал именно её. Загрузка падала с 1114, а Premiere вёл себя так,
    // будто плагина нет вовсе: ни строчки в журнале, обычная ошибка про av01.
    // Проверяем по таблице импорта, потому что на этой машине всё работает
    // и без статической среды — беда видна только в чужом процессе.
    std::string runtime;
    const bool leaks = ImportsMsvcRuntime(argv[1], &runtime);
    printf("C++ runtime: %s\n", leaks ? runtime.c_str() : "static, no msvcp/vcruntime imports");
    if (leaks) {
        printf("FAIL: build with /MT, otherwise hosts with an older runtime will not load it\n");
    }

    FreeLibrary(plugin);
    if (leaks) return 6;
    return ffmpegOk ? 0 : 5;
}
