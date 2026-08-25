// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "AV1Log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>      // towlower — сверка пути без учёта регистра
#include <share.h>      // _SH_DENYWR для _wfsopen
#include <mutex>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace av1imp {

namespace {

std::mutex g_mutex;
std::wstring g_path;

// Файл держим открытым.
//
// Раньше каждая строчка открывала и закрывала его заново — «чтобы при падении
// Premiere журнал остался целым». Цель верная, цена оказалась несуразной:
// замер дал 117 мкс на строчку против 5 мкс, если файл открыт и после записи
// делается сброс на диск. А пишем мы не по праздникам — минимум две строчки
// на каждый выданный кадр, при том что сама распаковка кадра 1440p занимает
// 650 мкс. То есть журнал добавлял к работе декодера около трети.
//
// Сброс после каждой строки оставлен намеренно: он и даёт ту самую целость
// при падении, ради которой всё затевалось, и стоит в двадцать четыре раза
// дешевле открытия файла.
FILE* g_file = nullptr;

// %LOCALAPPDATA%\Aether\log.txt — не в папке плагина: та лежит
// в Program Files, а Premiere работает без прав администратора
std::wstring MakeLogPath()
{
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        return std::wstring();
    }
    std::wstring dir = local;
    CoTaskMemFree(local);

    dir += L"\\Aether";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\log.txt";
}

// _wfsopen, а НЕ _wfopen_s — и это не вкусовщина.
//
// _wfopen_s открывает файл монопольно: пока Premiere работает, журнал нельзя
// ни прочитать, ни скопировать, ни приложить к issue. Открывается он один раз
// на весь сеанс, значит запрет держится всё время, пока Premiere запущен, —
// то есть ровно тогда, когда журнал и нужен. Проверено вживую: попытка
// прочитать его при живом Premiere даёт «файл используется другим процессом».
//
// _SH_DENYWR запрещает другим писать, но разрешает читать: посторонний
// не испортит запись, а прочесть и скопировать сможет кто угодно.
FILE* OpenLog(const wchar_t* mode)
{
    if (g_path.empty()) return nullptr;
    return _wfsopen(g_path.c_str(), mode, _SH_DENYWR);
}

void CloseLogFile()
{
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

} // namespace

// Перевод широкой строки в UTF-8 — тем же вызовом Windows, каким импортёр
// готовит путь для ffmpeg. Не локалью C, и в этом всё дело: локаль знает
// только ASCII, а WideCharToMultiByte знает всё.
//
// Сам файл журнала и так UTF-8: заголовок «=== Aether, запуск …» пишется
// из исходника, а он в UTF-8. То есть кодировка у файла была одна, а пути
// в него попадали по другому правилу — теперь по тому же.
std::string Utf8(const wchar_t* wide)
{
    if (!wide || !*wide) return std::string();

    const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();

    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], need, nullptr, nullptr);
    return out;
}

std::string LogPath(const wchar_t* path)
{
    if (!path || !*path) return std::string();

    std::wstring out = path;

    // Домашняя папка целиком: она содержит имя пользователя, а иногда и
    // фамилию. Меняем на переменную — путь остаётся понятным, имя уходит.
    wchar_t profile[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring from = profile;
        if (!from.empty()) {
            size_t at = 0;
            // Ищем без учёта регистра: Premiere присылает пути с приставкой
            // \\?\ и не обязан совпадать по регистру с переменной среды.
            std::wstring lowerOut = out, lowerFrom = from;
            for (auto& c : lowerOut)  c = (wchar_t)towlower(c);
            for (auto& c : lowerFrom) c = (wchar_t)towlower(c);
            while ((at = lowerOut.find(lowerFrom, at)) != std::wstring::npos) {
                out.replace(at, from.size(), L"%USERPROFILE%");
                lowerOut.replace(at, lowerFrom.size(), L"%userprofile%");
                at += 13;
            }
        }
    }
    return Utf8(out.c_str());
}

void LogReset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseLogFile();
    g_path = MakeLogPath();

    g_file = OpenLog(L"w");
    if (g_file) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_file, "=== Aether, запуск %02d:%02d:%02d ===\n",
                t.wHour, t.wMinute, t.wSecond);
        fflush(g_file);
    }
}

void LogClose()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseLogFile();
}

void Log(const char* format, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    // Плагин мог не пройти через LogReset — например, если журнал понадобился
    // раньше загрузки. Тогда дописываем в существующий файл.
    if (!g_file) {
        if (g_path.empty()) g_path = MakeLogPath();
        g_file = OpenLog(L"a");
        if (!g_file) return;
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02d:%02d:%02d.%03d  ",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(g_file, format, args);
    va_end(args);

    fputc('\n', g_file);

    // Сброс на диск после каждой строки: при падении хоста журнал остаётся
    // целым до последней записи, а это единственный инструмент разбора
    fflush(g_file);
}

} // namespace av1imp

namespace av1imp {

const char* SelectorName(int selector)
{
    switch (selector) {
        case 0:  return "imInit";
        case 1:  return "imShutdown";
        case 5:  return "imImportImage";
        case 8:  return "imQuietFile";
        case 9:  return "imCloseFile";
        case 12: return "imAnalysis";
        case 13: return "imDataRateAnalysis";
        case 14: return "imGetIndFormat";
        case 23: return "imGetMetaData";
        case 25: return "imImportAudio7";
        case 27: return "imGetIndPixelFormat";
        case 28: return "imGetSupports7";
        case 29: return "imResetSequentialAudio";
        case 30: return "imGetSequentialAudio";
        case 36: return "imGetSupports8";
        case 37: return "imGetInfo8";
        case 38: return "imGetPrefs8";
        case 39: return "imOpenFile8";
        case 42: return "imCalcSize8";
        case 45: return "imGetPreferredFrameSize";
        case 46: return "imCreateAsyncImporter";
        case 47: return "imGetSourceVideo";
        case 48: return "imDeferredProcessing";
        case 49: return "imGetPeakAudio";
        case 52: return "imGetTimeInfo8";
        case 54: return "imGetSubTypeNames";
        case 55: return "imGetFileAttributes";
        case 57: return "imQueryContentState";
        case 58: return "imGetIndColorProfile";
        case 59: return "imQueryInputFileList";
        case 60: return "imQueryStreamLabel";
        case 64: return "imGetAudioChannelLayout";
        // 65 и 83 Premiere 26 спрашивает по три десятка раз за сеанс, и в
        // журнале они выглядели голыми числами. Читать журнал по цифрам
        // невыносимо — ради этого таблица и заведена.
        case 65: return "imSelectClipFrameDescriptor";
        case 66: return "imPerformSourceSettingsCommand";
        case 67: return "imGetExtendedFormatInfo";
        case 68: return "imGetDataStreamsInfo";
        case 71: return "imGetSupportsPerInstancePrefs";
        case 73: return "imGetIndColorSpace";
        case 80: return "imGetCurrentSystemState";
        case 81: return "imGetInfo9";
        case 82: return "imGetEmbeddedLUT";
        case 83: return "imSelectClipFrameDescriptor2";
        case 84: return "imGetColorSpaceFromOpaqueData";
        default: break;
    }
    // Незнакомые номера всё равно показываем — их можно найти в PrSDKImport.h
    static thread_local char buf[32];
    sprintf_s(buf, sizeof(buf), "selector %d", selector);
    return buf;
}

} // namespace av1imp
