// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Подгрузка библиотек ffmpeg из папки самого плагина.
//
// Зачем это нужно. Плагин лежит в MediaCore, а Windows ищет зависимые DLL рядом
// с исполняемым файлом (то есть рядом с Premiere), в системных папках и в PATH —
// но НЕ рядом с самой загружаемой DLL. Если ничего не делать, Premiere просто
// молча не загрузит плагин: библиотеки ffmpeg он не найдёт.
//
// Решение из двух половин:
//   1) здесь, при загрузке плагина, мы сами грузим нужные DLL по полному пути;
//   2) в настройках сборки ffmpeg подключён отложенно (/DELAYLOAD), поэтому
//      к моменту первого настоящего вызова библиотеки уже в процессе, и загрузчик
//      находит их по имени.
//
// Складывать копии ffmpeg в системные папки или дописывать PATH нельзя:
// это влияет на весь компьютер и ломается при обновлении Premiere.

#include <windows.h>

#include <mutex>

#include "AV1Log.h"
#include "AV1Version.h"
#include "AV1Settings.h"

namespace {

HMODULE g_selfModule = nullptr;

// Путь к папке плагина со слэшем на конце. Считается один раз при загрузке:
// рядом лежат и библиотеки ffmpeg, и файл настроек.
wchar_t g_pluginDir[MAX_PATH] = {};

// Порядок важен ровно настолько, насколько мы хотим понятных ошибок:
// зависимости всё равно подтянутся сами благодаря LOAD_WITH_ALTERED_SEARCH_PATH
const wchar_t* kFFmpegModules[] = {
    L"avutil-60.dll",
    L"swresample-6.dll",
    L"swscale-9.dll",
    L"avcodec-62.dll",
    L"avformat-62.dll",
};

void PreloadFFmpeg()
{
    const wchar_t* dir = g_pluginDir;
    if (dir[0] == 0) return;

    for (const wchar_t* name : kFFmpegModules) {
        wchar_t full[MAX_PATH] = {};
        wcscpy_s(full, MAX_PATH, dir);
        wcscat_s(full, MAX_PATH, name);

        // LOAD_WITH_ALTERED_SEARCH_PATH заставляет искать зависимости
        // этой библиотеки в её же папке, а не рядом с Premiere
        HMODULE m = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            av1imp::Log("ffmpeg: FAILED to load %ls (error %lu)", name, GetLastError());
        }
    }
    av1imp::Log("ffmpeg: loaded from %ls", dir);
}

} // namespace

namespace av1imp {

const wchar_t* PluginDirectory() { return g_pluginDir; }

// Подготовка вынесена из DllMain, и это не вкусовщина.
//
// Пока модуль загружается, поток держит замок загрузчика Windows. Вызывать
// оттуда LoadLibrary Microsoft прямо запрещает: загружаемая библиотека тоже
// хочет этот замок для собственного DllMain, и в неудачном порядке два потока
// встают друг напротив друга навсегда. Что это работало во всех пяти
// приложениях Adobe — везение, а не гарантия: у ffmpeg свои зависимости и своя
// инициализация, а замок один на процесс.
//
// SHGetKnownFolderPath для пути к журналу оттуда же вызывать не стоит по той
// же причине — shell32 к этому моменту может быть ещё не готова.
//
// Взамен всё делается при первом настоящем запросе от Premiere. Плата за это
// одна: строчка «плагин загружен» появляется не в момент загрузки, а в момент
// первого вопроса. На практике одно следует за другим сразу же, а пустой
// журнал по-прежнему означает, что плагин не спросили вовсе.
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() {
        LogReset();
        Log("Aether %s, plug-in ready in process", AETHER_VERSION_STR);
        PreloadFFmpeg();
    });
}

} // namespace av1imp

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);

        // Единственное, что здесь осталось: собственный путь. GetModuleFileNameW
        // замка загрузчика не трогает, а знать папку нужно и журналу, и ffmpeg.
        if (GetModuleFileNameW(module, g_pluginDir, MAX_PATH) != 0) {
            wchar_t* lastSlash = wcsrchr(g_pluginDir, L'\\');
            if (lastSlash) *(lastSlash + 1) = 0;
            else g_pluginDir[0] = 0;
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        av1imp::LogClose();
    }
    return TRUE;
}
