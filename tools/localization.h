// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#pragma once

namespace aether {

enum class UiLanguage {
    English,
    Russian,
};

UiLanguage DetectUiLanguage();
UiLanguage ParseUiLanguage(const wchar_t* code);
void SetUiLanguage(UiLanguage language);
UiLanguage CurrentUiLanguage();

// English is the fallback for every locale except Russian. Keeping both
// strings at the call site makes missing translations visible in review and
// avoids a runtime catalogue/parser dependency in the diagnostic tools.
const wchar_t* Tr(const wchar_t* english, const wchar_t* russian);

} // namespace aether
