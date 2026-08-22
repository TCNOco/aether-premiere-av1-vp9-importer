// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// AetherDiagnose.exe — диагностика без окна.
//
// Две работы сразу, и обе нужны:
//
//   1. Проверка без человека у экрана. То, что можно прогнать в консоли,
//      надо прогонять в консоли — по той же причине, по какой ядро отделено
//      от Premiere.
//   2. Движок для панели внутри Premiere. Панель написана на HTML и
//      распаковывать видео не умеет никак, поэтому зовёт эту программу
//      с ключом --json и читает ответ. Проверка у панели и у окна выходит
//      буквально одна и та же, расходиться нечему.
//
//   AetherDiagnose.exe [--json] [файл, на который жалуются]

#include "app_diagnose.h"

#include <windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>

int wmain(int argc, wchar_t** argv)
{
    bool json = false;
    std::wstring file;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--json") == 0) json = true;
        else if (file.empty())               file = argv[i];
    }

    // Вывод в UTF-8, а не в кодовую страницу консоли.
    //
    // Панель читает нас как поток байтов и ждёт именно UTF-8; консоль же
    // по умолчанию отдаёт то, что настроено в системе, и русский текст
    // приезжает мусором. Одна кодировка на оба случая проще, чем две.
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    const aether::Report r = aether::Run(file, [json](const std::wstring& step) {
        // Ход работы идёт в поток ошибок: в JSON ему не место, а видеть
        // его хочется в обоих случаях.
        fwprintf(stderr, L"  ... %s\n", step.c_str());
    });

    fwprintf(stdout, L"%s", json ? r.Json().c_str() : r.PlainText().c_str());
    return r.AnyFailed() ? 1 : 0;
}
