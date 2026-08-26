// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Сколько памяти съедает кэш кадров, когда клипов на таймлайне много.
//
//   cache_probe.exe <файл> [сколько клипов]
//
// Зачем. Бюджет кэша считался у каждого декодера свой, от разрешения: на 1440p
// это 331 МБ. Общего предела не было вовсе, и десять клипов давали три с лишним
// гигабайта — поверх собственного кэша Premiere. Проверка гоняет ровно тот
// случай: открывает N клипов и по каждому отматывает назад, потому что кэш
// заполняется только при движении назад.
//
// Мерим пик рабочего набора процесса, а не наши же счётчики: цифра, которую
// видно в диспетчере задач, и есть та, на которую жалуется пользователь.

#include "../src/AV1Decoder.h"
#include "../src/AV1Settings.h"
#include "utf8_args.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

size_t PeakWorkingSetMB()
{
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PeakWorkingSetSize / (1024 * 1024);
}

} // namespace

// wmain: узкий argv приходит в кодировке ANSI, а ядру нужен UTF-8 —
// см. tools/utf8_args.h
int wmain(int argc, wchar_t** wargv)
{
    tools::Utf8Args args(argc, wargv);
    char** argv = args.Ptrs();

    if (argc < 2) {
        printf("usage: cache_probe <file> [clips]\n");
        return 2;
    }
    const std::string path = argv[1];
    const int clips = (argc > 2) ? atoi(argv[2]) : 8;

    const size_t before = PeakWorkingSetMB();

    std::vector<std::unique_ptr<av1imp::Decoder>> decoders;
    for (int i = 0; i < clips; ++i) {
        auto d = std::make_unique<av1imp::Decoder>();
        if (!d->Open(path, /*preferHardware=*/false, /*needVideo=*/true)) {
            printf("OPEN %d FAILED: %s\n", i, d->LastError().c_str());
            return 3;
        }
        decoders.push_back(std::move(d));
    }

    const av1imp::MediaInfo& info = decoders[0]->Info();
    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height);

    // Отматывание назад по каждому клипу: только оно наполняет кэш
    const int64_t from = 200;
    for (auto& d : decoders) {
        d->GetFrameBGRA(from, buf.data(), stride, 0, 0);
        for (int64_t f = from - 1; f >= from - 90; --f) {
            d->GetFrameBGRA(f, buf.data(), stride, 0, 0);
        }
    }

    const size_t peak = PeakWorkingSetMB();
    printf("%dx%d, клипов %d: пик рабочего набора %zu МБ (до открытия %zu)\n",
           info.width, info.height, clips, peak, before);
    printf("на клип: %.1f МБ\n", clips ? (double)(peak - before) / clips : 0.0);
    const size_t held = av1imp::Decoder::RamCacheBytesHeld();
    const uint64_t limit = av1imp::MemoryCacheLimitBytes();
    printf("внутренний кэш Aether: %.1f МБ, предел %.1f МБ\n",
           held / (1024.0 * 1024.0), limit / (1024.0 * 1024.0));
    if (held > limit) {
        printf("FAIL: внутренний счётчик пробил общий предел\n");
        return 4;
    }
    return 0;
}
