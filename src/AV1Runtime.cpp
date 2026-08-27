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
//
// Шире MAX_PATH: Premiere отдаёт медиа с приставкой \\?\, и портабельная
// установка тоже может лежать глубже 260 символов. GetModuleFileNameW
// принимает любой размер буфера.
constexpr DWORD kPluginDirChars = 2048;
wchar_t g_pluginDir[kPluginDirChars] = {};

// Порядок важен ровно настолько, насколько мы хотим понятных ошибок:
// зависимости всё равно подтянутся сами благодаря LOAD_WITH_ALTERED_SEARCH_PATH
const wchar_t* kFFmpegModules[] = {
    L"avutil-60.dll",
    L"swresample-6.dll",
    L"swscale-9.dll",
    L"avcodec-62.dll",
    L"avformat-62.dll",
};

bool PreloadFFmpeg()
{
    const wchar_t* dir = g_pluginDir;
    if (dir[0] == 0) {
        av1imp::Log("ffmpeg: FAILED - plug-in directory is unavailable");
        return false;
    }

    bool ready = true;
    for (const wchar_t* name : kFFmpegModules) {
        wchar_t full[kPluginDirChars + 64] = {};
        if (swprintf_s(full, L"%s%s", dir, name) < 0) {
            av1imp::Log("ffmpeg: FAILED to compose path for %s",
                        av1imp::Utf8(name).c_str());
            ready = false;
            continue;
        }

        // Только полный путь рядом с плагином. Поиск по имени (CWD, PATH,
        // папка Premiere) — это подмена DLL внутри процесса Adobe.
        // Стенд, где .prm собирается без DLL, должен копировать их сюда же,
        // как делает установщик, а не учить плагин искать «где получится».
        HMODULE m = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            av1imp::Log("ffmpeg: FAILED to load %s from %s (error %lu)",
                        av1imp::Utf8(name).c_str(), av1imp::Utf8(dir).c_str(),
                        GetLastError());
            ready = false;
            continue;
        }

        // LoadLibrary по полному пути всё равно вернёт уже загруженный модуль
        // с тем же именем (другой MediaCore-плагин успел раньше). Тогда это
        // не наши DLL, и delay-load ffmpeg внутри Premiere смешает две сборки.
        wchar_t loaded[kPluginDirChars + 64] = {};
        const DWORD n = GetModuleFileNameW(m, loaded, ARRAYSIZE(loaded));
        if (n == 0 || n >= ARRAYSIZE(loaded) || _wcsicmp(loaded, full) != 0) {
            av1imp::Log("ffmpeg: %s is already loaded from elsewhere (%s), refusing",
                        av1imp::Utf8(name).c_str(),
                        n ? av1imp::Utf8(loaded).c_str() : "?");
            FreeLibrary(m);
            ready = false;
            continue;
        }
    }

    if (!ready) {
        av1imp::Log("ffmpeg: runtime incomplete, importer disabled");
    } else {
        av1imp::Log("ffmpeg: loaded from %s", av1imp::Utf8(dir).c_str());
    }
    return ready;
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
void EnsureLog()
{
    static std::once_flag once;
    std::call_once(once, []() {
        LogReset();
        Log("Aether %s, plug-in ready in process", AETHER_VERSION_STR);
    });
}

bool EnsureRuntime()
{
    EnsureLog();
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, []() {
        ready = PreloadFFmpeg();
    });
    return ready;
}

} // namespace av1imp

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);

        // Единственное, что здесь осталось: собственный путь. GetModuleFileNameW
        // замка загрузчика не трогает, а знать папку нужно и журналу, и ffmpeg.
        // n >= kPluginDirChars означает обрезку: такой путь для LoadLibrary
        // врал бы, лучше отказаться и честно не найти ffmpeg.
        const DWORD n = GetModuleFileNameW(module, g_pluginDir, kPluginDirChars);
        if (n == 0 || n >= kPluginDirChars) {
            g_pluginDir[0] = 0;
        } else {
            wchar_t* lastSlash = wcsrchr(g_pluginDir, L'\\');
            if (lastSlash) *(lastSlash + 1) = 0;
            else g_pluginDir[0] = 0;
        }
    }
    // PROCESS_DETACH нарочно ничего не закрывает. Журнал закрывает imShutdown:
    // брать mutex лога из DllMain, пока другой поток пишет строку, — это
    // взаимная блокировка внутри Premiere на выгрузке.
    return TRUE;
}
