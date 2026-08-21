// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// AV1ImporterSettings.exe — окно настроек плагина.
//
// Отдельная программа, а не окно внутри Premiere, и это осознанно: настройка
// нужна ровно тогда, когда Premiere из-за поломки в драйвере не запускается.
// Подробнее — в комментарии к AV1Settings.h.
//
// Чистый Win32 без библиотек: программа ставится вместе с плагином, и тащить
// ради галочки лишние зависимости незачем.

#include "../src/AV1Settings.h"

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace {

enum {
    ID_AUTO = 1001,
    ID_SOFTWARE,
    ID_HARDWARE,
    ID_SAVE,
    ID_CLOSE,
};

HFONT g_font = nullptr;

HWND Add(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
         int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void Build(HWND w)
{
    Add(w, L"STATIC", L"Как распаковывать видео AV1 и VP9:",
        0, 16, 14, 400, 20, 0);

    Add(w, L"BUTTON", L"Автоматически (рекомендуется)",
        BS_AUTORADIOBUTTON | WS_GROUP, 16, 42, 400, 22, ID_AUTO);
    Add(w, L"STATIC",
        L"Процессором на машинах от 8 потоков, иначе видеокартой.",
        0, 36, 64, 400, 18, 0);

    Add(w, L"BUTTON", L"Процессором (dav1d)",
        BS_AUTORADIOBUTTON, 16, 88, 400, 22, ID_SOFTWARE);
    Add(w, L"STATIC",
        L"По замерам быстрее: видеокарте приходится копировать каждый кадр\n"
        L"в обычную память, а процессор сразу пишет туда.",
        0, 36, 110, 400, 34, 0);

    Add(w, L"BUTTON", L"Видеокартой (NVIDIA / Intel / AMD)",
        BS_AUTORADIOBUTTON, 16, 150, 400, 22, ID_HARDWARE);
    Add(w, L"STATIC",
        L"Разгружает процессор. Выбирайте, если он слабый или занят другим.",
        0, 36, 172, 400, 18, 0);

    Add(w, L"STATIC", L"Изменения вступят в силу после перезапуска Premiere Pro.",
        SS_LEFTNOWORDWRAP, 16, 204, 400, 18, 0);

    Add(w, L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON, 236, 232, 100, 28, ID_SAVE);
    Add(w, L"BUTTON", L"Закрыть",   0,                 344, 232, 100, 28, ID_CLOSE);

    int checked = ID_AUTO;
    switch (av1imp::CurrentMode()) {
        case av1imp::DecodeMode::Software: checked = ID_SOFTWARE; break;
        case av1imp::DecodeMode::Hardware: checked = ID_HARDWARE; break;
        default: break;
    }
    CheckRadioButton(w, ID_AUTO, ID_HARDWARE, checked);
}

void Save(HWND w)
{
    av1imp::DecodeMode mode = av1imp::DecodeMode::Auto;
    if (IsDlgButtonChecked(w, ID_SOFTWARE) == BST_CHECKED) mode = av1imp::DecodeMode::Software;
    else if (IsDlgButtonChecked(w, ID_HARDWARE) == BST_CHECKED) mode = av1imp::DecodeMode::Hardware;

    if (av1imp::SaveMode(mode)) {
        MessageBoxW(w, L"Сохранено.\n\nПерезапустите Premiere Pro, чтобы настройка применилась.",
                    L"Готово", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(w, L"Не удалось записать файл настроек.",
                    L"Ошибка", MB_OK | MB_ICONERROR);
    }
}

LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:  Build(w); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_SAVE)  { Save(w); return 0; }
            if (LOWORD(wp) == ID_CLOSE) { DestroyWindow(w); return 0; }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    InitCommonControls();
    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = Proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AetherSettings";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, 470, 278 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    HWND w = CreateWindowExW(0, wc.lpszClassName,
                             L"Aether — настройки",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, inst, nullptr);
    if (!w) return 1;

    ShowWindow(w, show);
    UpdateWindow(w);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
