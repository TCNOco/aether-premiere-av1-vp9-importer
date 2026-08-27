// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include <cstdint>
#include <cstdio>
#include <limits>

#include "../src/ImporterMath.h"

namespace {

int g_failures = 0;

void CheckTicks(const char* name, int64_t ticksPerSecond, int fpsNum, int fpsDen,
                bool expectOk, int64_t expected)
{
    int64_t actual = -1;
    const bool ok = av1imp::TicksPerFrame(ticksPerSecond, fpsNum, fpsDen, &actual);
    if (ok == expectOk && (!expectOk || actual == expected)) {
        printf("  %-36s OK\n", name);
        return;
    }
    printf("  %-36s FAIL (ok %s, value %lld)\n",
           name, ok ? "yes" : "no", (long long)actual);
    ++g_failures;
}

void Check(const char* name, int64_t frameCount, int32_t sampleSize,
           int32_t expected, bool expectedSaturated)
{
    bool saturated = false;
    const int32_t actual =
        av1imp::SaturatingFrameDuration(frameCount, sampleSize, &saturated);
    if (actual == expected && saturated == expectedSaturated) {
        printf("  %-36s OK\n", name);
        return;
    }
    printf("  %-36s FAIL (value %d, saturated %s)\n",
           name, actual, saturated ? "yes" : "no");
    ++g_failures;
}

} // namespace

int main()
{
    const int32_t max = std::numeric_limits<int32_t>::max();

    Check("ordinary fractional-rate clip",
          36000, 1001, 36036000, false);

    const int64_t exactFrames = max / 1001;
    Check("largest representable duration",
          exactFrames, 1001, static_cast<int32_t>(exactFrames * 1001), false);

    Check("long 59.94 fps recording clamps",
          exactFrames + 1, 1001, max, true);

    Check("zero frames",
          0, 1001, 0, false);

    Check("invalid sample size",
          100, 0, 0, false);

    CheckTicks("ordinary 60 fps", 254016000000LL, 60, 1, true, 4233600000LL);
    CheckTicks("zero denominator", 254016000000LL, 30, 0, false, 0);
    CheckTicks("overflowing product",
               (std::numeric_limits<int64_t>::max() / 2) + 1, 1, 3, false, 0);

    printf("\n%s\n", g_failures == 0 ? "ALL IMPORTER MATH CHECKS PASSED"
                                     : "IMPORTER MATH CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
