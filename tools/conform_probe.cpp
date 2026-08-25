// Читает дорожку звука целиком кусками — так, как это делает конформирование
// в Premiere. Ищем место, где чтение впервые отказывает.
//
//   conform_probe.exe <файл> [дорожка] [кусок в отсчётах]

#include "../src/AV1Decoder.h"
#include "utf8_args.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// wmain: узкий argv приходит в кодировке ANSI, а ядру нужен UTF-8 —
// см. tools/utf8_args.h
int wmain(int argc, wchar_t** wargv)
{
    tools::Utf8Args args(argc, wargv);
    char** argv = args.Ptrs();

    if (argc < 2) {
        printf("usage: conform_probe <file> [track] [chunk]\n");
        return 2;
    }
    const std::string path = argv[1];
    const int track = (argc > 2) ? atoi(argv[2]) : 0;
    const int chunk = (argc > 3) ? atoi(argv[3]) : 4096;

    av1imp::Decoder dec;
    if (!dec.Open(path, /*preferHardware=*/false, /*needVideo=*/true)) {
        printf("OPEN FAILED: %s\n", dec.LastError().c_str());
        return 3;
    }
    if (!dec.OpenAudio(track)) {
        printf("OPEN AUDIO FAILED: %s\n", dec.LastAudioError().c_str());
        return 3;
    }

    const av1imp::MediaInfo& mi = dec.Info();
    const int ch = mi.audioChannels;
    const int64_t total = mi.audioSampleCount;
    printf("track %d: %d ch, %d Hz, %lld samples declared\n",
           track, ch, mi.audioSampleRate, (long long)total);

    std::vector<std::vector<float>> buf(ch, std::vector<float>(chunk, 0.0f));
    std::vector<float*> ptr(ch);
    for (int c = 0; c < ch; ++c) ptr[c] = buf[c].data();

    int64_t pos = 0;
    int64_t failures = 0;
    int64_t firstFail = -1;

    // Конформ читает ЗА объявленный конец тоже: он идёт, пока хост не решит,
    // что хватит. Добавляем секунду сверху.
    const int64_t limit = total + mi.audioSampleRate;

    while (pos < limit) {
        if (!dec.GetAudio(pos, chunk, ptr.data())) {
            if (firstFail < 0) {
                firstFail = pos;
                printf("FIRST FAILURE at sample %lld (%.3f s of %.3f): %s\n",
                       (long long)pos, (double)pos / mi.audioSampleRate,
                       (double)total / mi.audioSampleRate, dec.LastAudioError().c_str());
            }
            ++failures;
        }
        pos += chunk;
    }

    printf("read %lld chunks, %lld failed\n", (long long)(limit / chunk), (long long)failures);
    return failures ? 1 : 0;
}
