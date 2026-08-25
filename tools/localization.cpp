// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "localization.h"

#include <windows.h>

#include <cwchar>

namespace aether {

namespace {

UiLanguage g_language = UiLanguage::English;

} // namespace

UiLanguage DetectUiLanguage()
{
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_RUSSIAN
        ? UiLanguage::Russian
        : UiLanguage::English;
}

UiLanguage ParseUiLanguage(const wchar_t* code)
{
    if (code && (_wcsnicmp(code, L"ru", 2) == 0)) return UiLanguage::Russian;
    return UiLanguage::English;
}

void SetUiLanguage(UiLanguage language)
{
    g_language = language;
}

UiLanguage CurrentUiLanguage()
{
    return g_language;
}

const wchar_t* Tr(const wchar_t* english, const wchar_t* russian)
{
    return g_language == UiLanguage::Russian ? russian : english;
}

} // namespace aether
