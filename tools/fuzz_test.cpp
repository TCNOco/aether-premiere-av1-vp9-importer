// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Feeds the decoding core deliberately broken files.
//
//   fuzz_test.exe <good file> [--seed N]
//
// The only thing being checked is that the process survives. Opening a damaged
// file may succeed or fail, a frame may come back or not — all of that is fine.
// What is not fine is taking the host down, and that is not a hypothetical: a
// frame with no dimensions used to reach swscale, which does not return an
// error for that but calls av_assert0 and ends the process. Inside Premiere it
// looked like "Premiere just closed itself", with nothing in any log.
//
// Every case is derived from a real file by a fixed rule and a fixed seed, so a
// failure reproduces exactly. Output is unbuffered: if the process does die, the
// last line printed names the case that did it.

#include "../src/AV1Decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

std::vector<uint8_t> ReadWhole(const std::string& path)
{
    std::vector<uint8_t> data;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return data;

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize((size_t)size);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return data;
}

bool WriteWhole(const std::string& path, const std::vector<uint8_t>& data)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

// Свой генератор, а не rand(): нужен один и тот же ряд на любой машине,
// иначе «упало на случае 7» ничего не значит
struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed ? seed : 1) {}
    uint32_t Next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (uint32_t)(state >> 32);
    }
    size_t Below(size_t n) { return n ? (size_t)(Next() % n) : 0; }
};

// Прогнать по сломанному файлу всё, что умеет декодер. Любой ответ годится,
// кроме падения; поэтому здесь нет ни одной проверки результата — есть только
// требование доработать до конца функции.
void Exercise(const std::string& path)
{
    av1imp::Decoder dec;
    if (!dec.Open(path, /*preferHardware=*/false)) return;   // отказ — законный ответ

    const av1imp::MediaInfo& info = dec.Info();

    if (info.hasVideo && info.width > 0 && info.height > 0) {
        // Размер берём из файла, но не верим ему безоглядно: у битого файла
        // в заголовке может стоять что угодно
        const int w = info.width  > 8192 ? 8192 : info.width;
        const int h = info.height > 8192 ? 8192 : info.height;
        const int stride = w * 4;

        // Под самый широкий из форматов, а не под текущий: ниже тот же буфер
        // получает шестнадцатибитный кадр, у которого строка вдвое длиннее.
        // Первый же прогон этого инструмента «нашёл падение» — и падением
        // оказался он сам, ровно на этой строчке.
        std::vector<uint8_t> buf((size_t)stride * 2 * h + 64, 0);

        // Начало, середина, конец и заведомо за концом
        const int64_t total = info.frameCount > 0 ? info.frameCount : 8;
        const int64_t spots[] = { 0, total / 2, total - 1, total + 5, 0 };
        for (int64_t at : spots) {
            dec.GetFrameBGRA(at, buf.data(), stride, w, h);
        }
        dec.GetFrameBGRA(0, buf.data(), stride * 2, w, h, av1imp::FrameFormat::BGRA16);
    }

    for (int track = 0; track < info.audioStreamCount && track < 4; ++track) {
        if (!dec.OpenAudio(track)) continue;

        const int channels = info.audioChannels > 0 ? info.audioChannels : 1;
        const int32_t block = 1024;
        std::vector<std::vector<float>> chans(channels, std::vector<float>(block, 0.0f));
        std::vector<float*> ptrs(channels);
        for (int c = 0; c < channels; ++c) ptrs[c] = chans[c].data();

        dec.GetAudio(0, block, ptrs.data());
        dec.GetAudio(info.audioSampleRate, block, ptrs.data());
        dec.GetAudio(info.audioSampleCount + 1000, block, ptrs.data());
    }
}

void RunCase(int number, const char* what, const std::string& path,
             const std::vector<uint8_t>& data)
{
    if (!WriteWhole(path, data)) {
        printf("  case %-2d %-34s CANNOT WRITE\n", number, what);
        ++g_failures;
        return;
    }
    printf("  case %-2d %-34s %8zu bytes ... ", number, what, data.size());
    Exercise(path);
    printf("survived\n");
}

} // namespace

int main(int argc, char** argv)
{
    // Без буфера: если процесс умрёт, последняя строка должна быть на экране,
    // а не в буфере, который никто уже не сольёт
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: fuzz_test <good file> [--seed N]\n");
        return 1;
    }

    std::string source;
    uint64_t seed = 20260821;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) seed = _strtoui64(argv[++i], nullptr, 10);
        else if (source.empty()) source = arg;
    }

    const std::vector<uint8_t> good = ReadWhole(source);
    if (good.size() < 1024) {
        printf("cannot read %s, or it is too small to damage usefully\n", source.c_str());
        return 2;
    }

    printf("source: %s (%zu bytes), seed %llu\n\n", source.c_str(), good.size(),
           (unsigned long long)seed);

    // Соседний файл с тем же расширением: по расширению плагин ничего не решает,
    // но пусть путь выглядит настоящим
    const size_t dot = source.find_last_of('.');
    const std::string ext = (dot == std::string::npos) ? std::string() : source.substr(dot);
    const std::string work = source.substr(0, dot == std::string::npos ? source.size() : dot)
                           + "-damaged" + ext;

    Random rng(seed);
    int number = 0;

    // Пустой и почти пустой — самая частая беда: файл ещё пишется или скачался
    // наполовину
    RunCase(++number, "empty", work, {});
    RunCase(++number, "sixteen bytes of nothing", work, std::vector<uint8_t>(16, 0));

    {
        std::vector<uint8_t> junk(4096);
        for (auto& b : junk) b = (uint8_t)rng.Next();
        RunCase(++number, "pure noise", work, junk);
    }

    // Обрыв на разной глубине: заголовок цел, дальше пусто
    const int cuts[] = { 1, 5, 25, 50, 75, 95, 99 };
    for (int percent : cuts) {
        std::vector<uint8_t> cut(good.begin(),
                                 good.begin() + (size_t)(good.size() / 100.0 * percent));
        char label[64];
        snprintf(label, sizeof(label), "truncated at %d%%", percent);
        RunCase(++number, label, work, cut);
    }

    // Испорченные байты в разном количестве
    const int flips[] = { 1, 16, 256, 4096 };
    for (int count : flips) {
        std::vector<uint8_t> damaged = good;
        for (int i = 0; i < count; ++i) {
            damaged[rng.Below(damaged.size())] ^= (uint8_t)(1 << (rng.Next() & 7));
        }
        char label[64];
        snprintf(label, sizeof(label), "%d flipped bits", count);
        RunCase(++number, label, work, damaged);
    }

    // Дыра посередине — так выглядит файл с потерянным куском на диске
    {
        std::vector<uint8_t> holed = good;
        const size_t hole = holed.size() / 8;
        const size_t at = holed.size() / 2;
        memset(holed.data() + at, 0, hole < holed.size() - at ? hole : holed.size() - at);
        RunCase(++number, "hole in the middle", work, holed);
    }

    // Заголовок от одного файла, содержимое от шума
    {
        std::vector<uint8_t> head(good.begin(), good.begin() + 1024);
        head.resize(good.size() / 4);
        for (size_t i = 1024; i < head.size(); ++i) head[i] = (uint8_t)rng.Next();
        RunCase(++number, "good header, noise body", work, head);
    }

    remove(work.c_str());

    printf("\n%d cases, all survived\n", number);
    if (g_failures > 0) {
        printf("%d could not even be written\n", g_failures);
        return 3;
    }
    return 0;
}
