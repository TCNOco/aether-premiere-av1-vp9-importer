// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Аргументы командной строки в UTF-8 — для проверочных программ.
//
// Зачем это нужно, и почему обычного main недостаточно.
//
// Ядро принимает путь в UTF-8: так его отдаёт ffmpeg и так его готовит сам
// плагин — WideCharToMultiByte(CP_UTF8, ...) от той UTF-16 строки, что
// прислал Premiere. Этот путь правильный, и в плагине он всегда был правильным.
//
// А вот проверочные программы объявляли int main(int argc, char** argv), и
// argv у него приходит В КОДИРОВКЕ ANSI СИСТЕМЫ, а не в UTF-8. Пока все
// проверочные файлы назывались латиницей, разницы не было вовсе: ASCII
// одинаков в обеих. Стоило появиться первому файлу с кириллицей в пути —
// и ядро получало cp1251 там, где ждало UTF-8, то есть недопустимую
// последовательность, и честно отвечало «cannot open file: Invalid argument».
//
// Причём ошибка была не в ядре и не в плагине, а в самом измерительном
// приборе: он был устроен так, что проверить эту дорогу не мог в принципе.
// Проверка, которая не в состоянии провалиться по настоящей причине, ничего
// и не доказывает.
//
// wmain отдаёт argv в UTF-16 — тем же, чем его отдаёт Premiere, — и дальше
// мы переводим его тем же вызовом, что и плагин. То есть проверочная
// программа наконец проходит по той же дороге, что и рабочий код.

#pragma once

// NOMINMAX обязателен: без него windows.h объявляет min и max макросами,
// и любой std::max( в том же файле разваливается на C2589 «illegal token on
// right side of ::». Проверочные программы про Windows до этого заголовка
// не знали вовсе, так что притащили его сюда мы — нам и убирать за собой.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

inline std::string Utf8FromWide(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, nullptr, nullptr);
    return out;
}

// Весь argv разом. Держать его надо, пока живы указатели: строки лежат
// в векторе, а Ptrs() только раздаёт на них ссылки.
class Utf8Args {
public:
    Utf8Args(int argc, wchar_t** argv)
    {
        values_.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) values_.push_back(Utf8FromWide(argv[i]));
        pointers_.reserve(values_.size() + 1);
        for (auto& v : values_) pointers_.push_back(const_cast<char*>(v.c_str()));
        pointers_.push_back(nullptr);
    }

    int    Count() const { return static_cast<int>(values_.size()); }
    char** Ptrs()        { return pointers_.data(); }
    const std::string& operator[](int i) const { return values_[static_cast<size_t>(i)]; }

private:
    std::vector<std::string> values_;
    std::vector<char*>       pointers_;
};

} // namespace tools
