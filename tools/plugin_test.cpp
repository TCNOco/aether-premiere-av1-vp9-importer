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

#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKMALErrors.h"

typedef prMALError (*ImportEntryProc)(csSDK_int32, imStdParms*, void*, void*);

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

    FreeLibrary(plugin);
    return ffmpegOk ? 0 : 5;
}
