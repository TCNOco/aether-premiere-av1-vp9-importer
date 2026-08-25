// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include <cstdio>
#include <string>

#include "../src/IniSettings.h"

namespace {

int g_failures = 0;

void Check(const char* name, const std::string& actual, const std::string& expected)
{
    if (actual == expected) {
        printf("  %-36s OK\n", name);
        return;
    }
    printf("  %-36s FAIL\n    expected: %s\n    actual:   %s\n",
           name, expected.c_str(), actual.c_str());
    ++g_failures;
}

} // namespace

int main()
{
    using av1imp::SetIniValue;

    Check("new file",
          SetIniValue("", "decode", "auto"),
          "decode=auto\n");

    Check("replace while preserving switches",
          SetIniValue("; settings\r\ndecode=auto\r\nasync=off\r\nyuv=off\r\n",
                      "decode", "hardware"),
          "; settings\r\ndecode=hardware\r\nasync=off\r\nyuv=off\r\n");

    Check("append after unknown settings",
          SetIniValue("async=off\nyuv=off", "decode", "software"),
          "async=off\nyuv=off\ndecode=software\n");

    Check("case and whitespace",
          SetIniValue("  DeCoDe \t= auto\nother=value\n", "decode", "hardware"),
          "decode=hardware\nother=value\n");

    Check("collapse duplicate keys",
          SetIniValue("decode=auto\nasync=off\ndecode=software\nyuv=off\n",
                      "decode", "hardware"),
          "decode=hardware\nasync=off\nyuv=off\n");

    Check("do not confuse key prefixes",
          SetIniValue("decoder=custom\ndecode_mode=manual\n", "decode", "auto"),
          "decoder=custom\ndecode_mode=manual\ndecode=auto\n");

    printf("\n%s\n", g_failures == 0 ? "ALL SETTINGS CHECKS PASSED"
                                     : "SETTINGS CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
