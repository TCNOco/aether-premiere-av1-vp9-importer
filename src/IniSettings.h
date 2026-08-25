// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace av1imp {

inline bool IsIniKeyLine(const std::string& line, const char* key)
{
    size_t at = 0;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;

    const size_t keyLen = strlen(key);
    if (line.size() - at < keyLen) return false;
    for (size_t i = 0; i < keyLen; ++i) {
        const unsigned char actual = static_cast<unsigned char>(line[at + i]);
        const unsigned char wanted = static_cast<unsigned char>(key[i]);
        if (std::tolower(actual) != std::tolower(wanted)) return false;
    }

    at += keyLen;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    return at < line.size() && line[at] == '=';
}

// Replaces one key without discarding comments or unrelated/unknown settings.
// Duplicate copies of the key are collapsed so readers cannot disagree about
// whether the first or the last one wins. Existing line endings are retained.
inline std::string SetIniValue(const std::string& text,
                               const char* key,
                               const std::string& value)
{
    const std::string replacement = std::string(key) + "=" + value;
    const std::string defaultEnding =
        text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

    std::string out;
    bool replaced = false;
    size_t begin = 0;

    while (begin < text.size()) {
        const size_t lf = text.find('\n', begin);
        const size_t contentEnd =
            (lf != std::string::npos && lf > begin && text[lf - 1] == '\r')
                ? lf - 1
                : (lf != std::string::npos ? lf : text.size());
        const std::string line = text.substr(begin, contentEnd - begin);
        const std::string ending =
            lf == std::string::npos ? std::string()
            : (contentEnd < lf ? "\r\n" : "\n");

        if (IsIniKeyLine(line, key)) {
            if (!replaced) {
                out += replacement;
                out += ending;
                replaced = true;
            }
        } else {
            out += line;
            out += ending;
        }

        if (lf == std::string::npos) break;
        begin = lf + 1;
    }

    if (!replaced) {
        if (!out.empty() && out.back() != '\n') out += defaultEnding;
        out += replacement;
        out += defaultEnding;
    }
    return out;
}

} // namespace av1imp
