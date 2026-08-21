// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Exercises the decoding core without Premiere.
//
//   decoder_test.exe <file.mp4> [frame]
//
// Prints media parameters, decodes a frame, measures the access patterns an
// editor actually produces, lists audio tracks — and then runs correctness
// checks that either pass or fail the whole run.
//
// The checks matter more than the timings: a wrong frame is a bug you cannot
// see in a benchmark, and every one of them covers a mistake this code has
// actually made.

#include "../src/AV1Decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    printf("  %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

// BMP is written by hand: no dependencies, and any viewer opens it
bool SaveBMP(const char* path, const uint8_t* bgra, int w, int h, int stride)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    const int rowBytes  = w * 4;
    const int imageSize = rowBytes * h;
    const int fileSize  = 54 + imageSize;

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    memcpy(&header[2], &fileSize, 4);
    int offset = 54;      memcpy(&header[10], &offset, 4);
    int hdrSize = 40;     memcpy(&header[14], &hdrSize, 4);
    memcpy(&header[18], &w, 4);
    int negH = -h;        memcpy(&header[22], &negH, 4);   // negative = top-down rows
    uint16_t planes = 1;  memcpy(&header[26], &planes, 2);
    uint16_t bpp = 32;    memcpy(&header[28], &bpp, 2);
    memcpy(&header[34], &imageSize, 4);

    fwrite(header, 1, sizeof(header), f);
    for (int y = 0; y < h; ++y) {
        fwrite(bgra + (size_t)y * stride, 1, rowBytes, f);
    }
    fclose(f);
    return true;
}

// --- correctness checks -----------------------------------------------------

// A frame served from the cache must be identical to one decoded from scratch.
// The cache is filled only while scrubbing backwards, so we drive that motion
// and compare against a second decoder that has no history at all.
void CheckCacheMatchesFreshDecode(av1imp::Decoder& dec, const std::string& path,
                                  const av1imp::MediaInfo& info, bool hardware)
{
    if (!info.hasVideo) {
        printf("  %-46s SKIP (no video)\n", "cached frame == freshly decoded frame");
        return;
    }
    if (info.frameCount < 400) {
        printf("  %-46s SKIP (clip too short)\n", "cached frame == freshly decoded frame");
        return;
    }

    const int stride = info.width * 4;
    std::vector<uint8_t> cached((size_t)stride * info.height);
    std::vector<uint8_t> fresh((size_t)stride * info.height);

    // Walking backwards populates the cache for the surrounding keyframe run
    dec.GetFrameBGRA(300, cached.data(), stride, 0, 0);
    dec.GetFrameBGRA(299, cached.data(), stride, 0, 0);
    const bool gotCached = dec.GetFrameBGRA(290, cached.data(), stride, 0, 0);

    av1imp::Decoder clean;
    const bool opened = clean.Open(path, hardware);
    const bool gotFresh = opened && clean.GetFrameBGRA(290, fresh.data(), stride, 0, 0);

    Check(gotCached && gotFresh && cached == fresh,
          "cached frame == freshly decoded frame");
}

// Premiere asks for a reduced frame at lower playback quality. Writing the full
// resolution into that smaller buffer overruns it — this is exactly the bug the
// guard bytes below would have caught.
// У 10-битного файла шестнадцатибитный вывод обязан нести больше, чем восемь бит,
// и укладываться в диапазон Adobe: белое у них на 32768, а не на 65535.
void CheckDeepColour(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    const char* name16 = "16-bit output carries more than 8 bits";
    const char* nameRange = "16-bit output stays inside Adobe's 0..32768";

    if (info.bitDepth <= 8) {
        printf("  %-46s SKIP (source is 8-bit)\n", name16);
        return;
    }

    const int stride = info.width * 4 * (int)sizeof(uint16_t);
    std::vector<uint16_t> frame((size_t)stride / sizeof(uint16_t) * info.height, 0);

    if (!dec.GetFrameBGRA(0, reinterpret_cast<uint8_t*>(frame.data()), stride,
                          info.width, info.height, av1imp::FrameFormat::BGRA16)) {
        Check(false, name16);
        return;
    }

    // Считаем различные значения зелёного канала: у 8 бит их не может быть
    // больше 256, сколько бы места ни занимал буфер
    std::vector<bool> seen(65536, false);
    uint16_t peak = 0;
    int distinct = 0;
    for (size_t i = 1; i < frame.size(); i += 4) {      // B G R A -> зелёный
        const uint16_t v = frame[i];
        if (!seen[v]) { seen[v] = true; ++distinct; }
        if (v > peak) peak = v;
    }

    printf("  distinct green values: %d, peak %u (white is 32768)\n", distinct, peak);
    Check(distinct > 256, name16);
    Check(peak <= 32768,  nameRange);
}

// Чтение за концом файла. Проверка выглядит бедно — «не упало и ответило» —
// но именно этим и ценна: раньше на этом месте процесс умирал целиком.
// avcodec_receive_frame очищает кадр перед тем, как сообщить о конце файла,
// а пустой кадр в swscale не ошибка, а av_assert0 и конец процесса. В Premiere
// это выглядело бы как «Premiere просто закрылся», без единого слова в журнале.
// Если в файле заявлена прозрачность, она должна дойти до выхода живой.
// Проверка нужна, потому что потерять альфу можно молча в двух местах сразу:
// на аппаратном декодере и на неверно выбранном декодере VP9 — родной `vp9`
// отдаёт кадр без неё, альфу вытаскивает только libvpx-vp9.
void CheckAlphaSurvives(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    const char* name = "declared alpha reaches the output";
    if (!info.hasAlpha) {
        printf("  %-46s SKIP (no alpha in the file)\n", name);
        return;
    }

    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height, 0);
    if (!dec.GetFrameBGRA(0, buf.data(), stride, info.width, info.height)) {
        Check(false, name);
        return;
    }

    int lo = 255, hi = 0;
    for (size_t i = 3; i < buf.size(); i += 4) {     // B G R A
        const int a = buf[i];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
    }
    printf("      alpha range: %d..%d\n", lo, hi);
    Check(hi > lo, name);
}

void CheckReadingPastTheEnd(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    if (!info.hasVideo) {
        printf("  %-46s SKIP (no video)\n", "reading past the end");
        return;
    }
    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height, 0);

    // Заведомо дальше конца, и ещё раз — сразу за последним кадром
    const bool farPast  = dec.GetFrameBGRA(info.frameCount + 1000, buf.data(), stride,
                                           info.width, info.height);
    const bool justPast = dec.GetFrameBGRA(info.frameCount + 1, buf.data(), stride,
                                           info.width, info.height);

    // Любой ответ годится, кроме падения: вернуть последний кадр или честно
    // отказать — оба поведения разумны
    Check(true, "reading past the end does not take the process down");
    printf("      past the end: %s, one frame past: %s\n",
           farPast ? "frame returned" : "refused",
           justPast ? "frame returned" : "refused");
}

// Кадр таймлайна и кадр источника - разные вещи, и связывает их время.
// Premiere просит кадр номер N постоянной частоты; отдать нужно тот кадр
// источника, который в этот момент виден, то есть последний с меткой не
// позже N/fps. Пока частота постоянная, разницы не видно - поэтому ошибку
// в этом месте легко не заметить годами.
//
// Правило простое: картинка не имеет права убегать вперёд запроса и не имеет
// права отстать сильнее, чем длится один кадр источника. Запас в 0.25 с взят
// под материал из tools\make-test-media.ps1, где самый редкий кадр держится
// 0.1 с; на постоянной частоте расхождение выходит нулевым.
void CheckTimelinePicksFrameByTime(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    const char* nameAhead = "picture never runs ahead of the timeline";
    const char* nameLag   = "picture never falls behind the timeline";
    const char* nameMoves = "picture advances together with the timeline";

    if (!info.hasVideo || info.fps <= 0 || info.frameCount <= 2) {
        printf("  %-46s SKIP (no video)\n", nameAhead);
        return;
    }

    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height, 0);

    const double frameSec = 1.0 / info.fps;
    const double ahead    = frameSec * 0.5;   // допуск на округление меток в контейнере
    const double lag      = 0.25;

    double worstAhead = 0.0, worstLag = 0.0;
    double firstSeen = 0.0, lastSeen = 0.0;
    int64_t firstIdx = 0, lastIdx = 0;
    bool haveFirst = false, decoded = true;

    // Идём по клипу с шагом, а не подряд: нужен весь ролик, а не его начало
    const int64_t step = info.frameCount > 40 ? info.frameCount / 40 : 1;
    for (int64_t n = 0; n < info.frameCount - 1; n += step) {
        if (!dec.GetFrameBGRA(n, buf.data(), stride, info.width, info.height)) {
            decoded = false;
            break;
        }
        const double want = n * frameSec;
        const double got  = dec.LastFrameTimeSec();

        if (!haveFirst) { firstSeen = got; firstIdx = n; haveFirst = true; }

        // Пока не начался сам поток видео, показывать нечего, кроме первого
        // кадра, и «убежал вперёд» тут не про ошибку: в файле со сдвинутым
        // началом видео может стартовать на десятки миллисекунд позже общего
        // нуля клипа, и это правда о файле, а не о нас.
        if (got > firstSeen + 1e-6 && got - want > worstAhead) worstAhead = got - want;
        if (want - got > worstLag) worstLag = want - got;
        lastSeen = got; lastIdx = n;
    }

    printf("      worst ahead %.3f s, worst behind %.3f s\n", worstAhead, worstLag);

    Check(decoded && worstAhead <= ahead, nameAhead);
    Check(decoded && worstLag   <= lag,   nameLag);

    // Отдельно - застревание: при неверном начале отсчёта декодер отдавал
    // один и тот же первый кадр на любой запрос, и оба правила выше это
    // пропускали, потому что расхождение считалось от неверного нуля
    const double travelled = lastSeen - firstSeen;
    const double expected  = (double)(lastIdx - firstIdx) * frameSec;
    printf("      timeline moved %.2f s, picture moved %.2f s\n", expected, travelled);
    Check(decoded && expected > 0 && travelled > expected * 0.9, nameMoves);
}

// Совпадают ли звук и картинка. В файле вспышка в кадре и щелчок в звуке
// поставлены в один и тот же момент; расстояние между ними на выходе и есть
// расхождение. Иначе синхронность приходится проверять глазами и ушами, а это
// не проверка.
//
// Запускается по флагу --sync, а не по догадке: на сломанном файле не найдётся
// ни вспышки, ни щелчка, и молчаливый пропуск спрятал бы ровно ту поломку,
// ради которой проверка написана.
void CheckSoundMatchesPicture(av1imp::Decoder& dec, const av1imp::MediaInfo& info,
                              bool required)
{
    const char* name = "the click in the sound and the flash in the picture meet";
    if (!required) return;

    if (!info.hasVideo || info.audioStreamCount == 0 || info.fps <= 0) {
        Check(false, name);
        return;
    }

    // Вспышка: самый светлый кадр таймлайна
    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height, 0);

    int64_t flashFrame = -1;
    double  brightest  = 0.0;
    for (int64_t n = 0; n < info.frameCount; ++n) {
        if (!dec.GetFrameBGRA(n, buf.data(), stride, info.width, info.height)) break;

        // Средняя яркость по зелёному каналу, по каждому сороковому пикселю:
        // вспышка занимает весь кадр, разглядывать его целиком незачем
        double sum = 0.0; int taken = 0;
        for (size_t i = 1; i < buf.size(); i += 4 * 40) { sum += buf[i]; ++taken; }
        const double avg = taken ? sum / taken : 0.0;
        if (avg > brightest) { brightest = avg; flashFrame = n; }
    }

    // Щелчок: первый отсчёт громче трети шкалы
    if (!dec.OpenAudio(0)) { Check(false, name); return; }

    const int    channels = info.audioChannels > 0 ? info.audioChannels : 1;
    const int32_t block   = 4096;
    std::vector<std::vector<float>> chans(channels, std::vector<float>(block));
    std::vector<float*> ptrs(channels);
    for (int c = 0; c < channels; ++c) ptrs[c] = chans[c].data();

    // Сначала пик всей дорожки, потом первый отсчёт громче его половины.
    // Порог от самой записи, а не заданный числом: у генератора синуса в
    // ffmpeg полная шкала это -18 дБ, и любое выбранное заранее число
    // оказалось бы либо слишком строгим, либо бессмысленным.
    double peak = 0.0;
    for (int64_t at = 0; at < info.audioSampleCount; at += block) {
        if (!dec.GetAudio(at, block, ptrs.data())) break;
        for (int32_t i = 0; i < block; ++i) {
            const double v = std::fabs(chans[0][i]);
            if (v > peak) peak = v;
        }
    }

    int64_t clickSample = -1;
    if (peak > 0.02) {
        for (int64_t at = 0; at < info.audioSampleCount && clickSample < 0; at += block) {
            if (!dec.GetAudio(at, block, ptrs.data())) break;
            for (int32_t i = 0; i < block; ++i) {
                if (std::fabs(chans[0][i]) > peak * 0.5) { clickSample = at + i; break; }
            }
        }
    }

    if (flashFrame < 0 || clickSample < 0 || brightest < 100.0) {
        printf("      nothing to compare: flash %s, click %s (peak %.3f)\n",
               brightest >= 100.0 ? "found" : "MISSING",
               clickSample >= 0 ? "found" : "MISSING", peak);
        Check(false, name);
        return;
    }

    const double flashSec = flashFrame / info.fps;
    const double clickSec = (double)clickSample / info.audioSampleRate;
    const double apart    = std::fabs(flashSec - clickSec);

    // Два кадра. Не строже: кодер звука размазывает начало щелчка на своё окно,
    // и требовать точности до отсчёта значило бы проверять AAC, а не нас
    const double allowed = 2.0 / info.fps;

    printf("      flash at %.3f s, click at %.3f s, %.0f ms apart (allowed %.0f)\n",
           flashSec, clickSec, apart * 1000.0, allowed * 1000.0);
    Check(apart <= allowed, name);
}

void CheckReducedSizeStaysInBuffer(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    if (!info.hasVideo) {
        printf("  %-46s SKIP (no video)\n", "reduced-size frame is produced");
        return;
    }
    const int w = info.width / 2;
    const int h = info.height / 2;
    const int stride = w * 4;

    const size_t needed = (size_t)stride * h;
    const size_t guard  = 4096;

    std::vector<uint8_t> buf(needed + guard, 0xAB);
    const bool ok = dec.GetFrameBGRA(120, buf.data(), stride, w, h);

    bool guardIntact = true;
    for (size_t i = needed; i < buf.size(); ++i) {
        if (buf[i] != 0xAB) { guardIntact = false; break; }
    }

    // A real frame is never uniformly one value; catches "wrote nothing"
    bool wroteSomething = false;
    for (size_t i = 0; i < needed; ++i) {
        if (buf[i] != 0xAB) { wroteSomething = true; break; }
    }

    Check(ok && wroteSomething, "reduced-size frame is produced");
    Check(guardIntact, "reduced-size frame stays inside the buffer");
}

// Reading the same range twice must give the same samples, even after the
// decoder has been sent somewhere else in between.
void CheckAudioIsRepeatable(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
    if (info.audioStreamCount == 0 || !dec.OpenAudio(0)) {
        printf("  %-46s SKIP (no audio)\n", "same audio range reads identically");
        return;
    }

    const int ch = info.audioChannels;
    const int32_t count = 4800;

    // Positions come from the clip, not from a fixed number of seconds. With
    // 5 and 15 seconds hard-coded, a short test file read past its own end and
    // the check reported a failure that was nothing but the end of the file.
    if (info.audioSampleCount < (int64_t)count * 4) {
        printf("  %-46s SKIP (clip too short)\n", "same audio range reads identically");
        return;
    }
    const int64_t at   = info.audioSampleCount / 4;
    const int64_t away = info.audioSampleCount * 3 / 4;

    std::vector<std::vector<float>> a(ch, std::vector<float>(count));
    std::vector<std::vector<float>> b(ch, std::vector<float>(count));
    std::vector<std::vector<float>> scratch(ch, std::vector<float>(count));

    std::vector<float*> pa(ch), pb(ch), ps(ch);
    for (int c = 0; c < ch; ++c) { pa[c] = a[c].data(); pb[c] = b[c].data(); ps[c] = scratch[c].data(); }

    const bool first = dec.GetAudio(at, count, pa.data());
    dec.GetAudio(away, count, ps.data());   // force a seek away
    const bool again = dec.GetAudio(at, count, pb.data());

    // Bit equality is the wrong thing to demand, and finding that out took an
    // experiment. Decoding a range straight through and decoding it again after
    // a seek differ in the first ~192 samples, by up to 0.005. That is not our
    // doing: plain ffmpeg on the same file, once with -ss before the input and
    // once after, differs in exactly the same 192 samples by 0.0055. An AAC
    // decoder restarted mid-stream simply needs a few frames to settle, and no
    // amount of pre-roll removes it - two seconds of pre-roll changed nothing.
    //
    // So what is checked is the shape of the difference. The first attempt here
    // demanded bit equality past the first AAC frame, and that was still wrong:
    // a pure sine read twice differs in the last bits of the float everywhere,
    // by about 6e-6. Size separates the two cases, not position - settling
    // reaches 0.005 inside the first frame and drops to rounding noise after it,
    // whereas the bug this check was written for, a seek landing on the wrong
    // range, moves every sample by a fraction of the signal itself.
    const int   kSettling     = 1024;    // one AAC frame
    const float kSettlingMax  = 0.02f;   // about -34 dBFS, well above the 0.005 seen
    const float kRoundingMax  = 1e-4f;   // -80 dBFS: what different arithmetic costs

    int   beyond   = 0;          // samples past the window that differ by more than noise
    float worst    = 0.0f;       // largest difference inside the window
    float worstFar = 0.0f;       // and outside it
    int   firstAt  = -1;
    int   lastAt   = -1;
    for (int c = 0; c < ch; ++c) {
        for (int i = 0; i < count; ++i) {
            const float diff = a[c][i] - b[c][i];
            const float mag  = diff < 0 ? -diff : diff;
            if (mag == 0.0f) continue;

            if (firstAt < 0) firstAt = i;
            lastAt = i;
            if (i >= kSettling) {
                if (mag > worstFar) worstFar = mag;
                if (mag > kRoundingMax) ++beyond;
            } else if (mag > worst) {
                worst = mag;
            }
        }
    }

    Check(first && again && beyond == 0 && worst <= kSettlingMax,
          "same audio range reads identically");

    if (firstAt >= 0) {
        printf("      after the seek: positions %d..%d, up to %.6f inside the first"
               " AAC frame, %.6f past it (%d above the noise floor)\n",
               firstAt, lastAt, worst, worstFar, beyond);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // Без буфера: при падении буферизованный вывод теряется целиком, и по
    // журналу кажется, будто программа упала в самом начале
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: decoder_test <file> [frame] [--sw]\n");
        printf("  --sw    force the software decoder, as the plug-in does when\n");
        printf("          hardware decoding is switched off in its settings\n");
        printf("  --sync  the file carries a flash and a click at the same\n");
        printf("          instant; fail unless they come out together\n");
        return 1;
    }

    std::string path;
    int64_t wanted = 0;
    bool preferHardware = true;
    // --16u гоняет замер в шестнадцатибитном выводе. Нужен, чтобы цена глубины
    // была измерена, а не оценена на глаз
    av1imp::FrameFormat format = av1imp::FrameFormat::BGRA8;
    // --sync: в файле есть вспышка и щелчок в один и тот же момент, и они
    // обязаны сойтись. Без флага проверять нечего, и она молчит
    bool requireSync = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sw")      preferHardware = false;
        else if (arg == "--16u") format = av1imp::FrameFormat::BGRA16;
        else if (arg == "--sync") requireSync = true;
        else if (path.empty())  path = arg;
        else                    wanted = _atoi64(arg.c_str());
    }

    av1imp::Decoder dec;
    if (!dec.Open(path, preferHardware)) {
        printf("OPEN FAILED: %s\n", dec.LastError().c_str());
        return 2;
    }

    const auto& info = dec.Info();
    if (!info.hasVideo) {
        printf("video      : none, %d audio track(s)\n", info.audioStreamCount);
    } else {
    printf("resolution : %dx%d, %d bit%s\n", info.width, info.height, info.bitDepth,
           info.hasAlpha ? ", with alpha" : "");
    printf("fps        : %.3f (%d/%d)\n", info.fps, info.fpsNum, info.fpsDen);
    printf("duration   : %.2f s\n", info.durationSec);
    printf("frames     : %lld\n", (long long)info.frameCount);
    printf("decoder    : %s (%s)\n", info.decoderName.c_str(),
           info.hardwareDecode ? "GPU" : "CPU");

    // Буфер по формату: в шестнадцати битах пиксель занимает вдвое больше
    const int stride = info.width * (format == av1imp::FrameFormat::BGRA16 ? 8 : 4);
    std::vector<uint8_t> buf((size_t)stride * info.height);

    auto t0 = std::chrono::steady_clock::now();
    if (!dec.GetFrameBGRA(wanted, buf.data(), stride, 0, 0)) {
        printf("FRAME FAILED: %s\n", dec.LastError().c_str());
        return 3;
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("frame %lld   : %.1f ms (with seek)\n", (long long)wanted,
           std::chrono::duration<double, std::milli>(t1 - t0).count());

    if (SaveBMP("frame_out.bmp", buf.data(), info.width, info.height, stride)) {
        printf("saved      : frame_out.bmp\n");
    }

    // Reading forward — how Premiere reads during playback
    dec.ResetStats();
    const int kSeq = 60;
    t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 1; i <= kSeq; ++i) {
        if (dec.GetFrameBGRA(wanted + i, buf.data(), stride, 0, 0, format)) ++ok;
    }
    t1 = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("sequential : %d/%d frames, %.1f ms total, %.1f fps\n",
           ok, kSeq, totalMs, ok * 1000.0 / totalMs);

    // Where the time actually goes. The GPU decodes; the cost is elsewhere.
    {
        const av1imp::Decoder::Stats& st = dec.GetStats();
        const double per = st.frames ? 1.0 / st.frames : 0.0;
        printf("  of which: decode %.2f ms, GPU transfer %.2f ms, colour %.2f ms (per frame)\n",
               st.decodeMs * per, st.transferMs * per, st.convertMs * per);
    }

    // Stepping backwards — the worst case for an inter-frame codec, and what an
    // editor does constantly
    const int kBack = 30;
    const int64_t backFrom = wanted + 300;
    dec.GetFrameBGRA(backFrom, buf.data(), stride, 0, 0);   // warm-up, not measured

    t0 = std::chrono::steady_clock::now();
    ok = 0;
    for (int i = 1; i <= kBack; ++i) {
        if (dec.GetFrameBGRA(backFrom - i, buf.data(), stride, 0, 0)) ++ok;
    }
    t1 = std::chrono::steady_clock::now();
    totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("backward   : %d/%d frames, %.1f ms total, %.1f ms/frame\n",
           ok, kBack, totalMs, totalMs / (ok ? ok : 1));

    // Random jumps — dragging the playhead around
    const int kJumps = 20;
    t0 = std::chrono::steady_clock::now();
    ok = 0;
    for (int i = 0; i < kJumps; ++i) {
        const int64_t f = (info.frameCount > 0) ? (i * 7919) % info.frameCount : 0;
        if (dec.GetFrameBGRA(f, buf.data(), stride, 0, 0)) ++ok;
    }
    t1 = std::chrono::steady_clock::now();
    totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("random     : %d/%d jumps, %.1f ms total, %.1f ms/jump\n",
           ok, kJumps, totalMs, totalMs / (ok ? ok : 1));

    }   // конец видеочасти: у файла без видео её нет вовсе

    // --- audio ---
    printf("\naudio      : %d track(s)\n", info.audioStreamCount);

    for (int track = 0; track < info.audioStreamCount; ++track) {
        if (!dec.OpenAudio(track)) {
            printf("  track %d: FAILED - %s\n", track, dec.LastError().c_str());
            continue;
        }

        const int ch = info.audioChannels;
        const int32_t want = info.audioSampleRate;   // one second

        std::vector<std::vector<float>> abuf(ch, std::vector<float>(want));
        std::vector<float*> ptrs(ch);
        for (int c = 0; c < ch; ++c) ptrs[c] = abuf[c].data();

        // Read from the 10 s mark: the start of a recording is often quiet, and
        // a quiet start cannot be told apart from an empty track
        // A third of the way in: far enough from the start to be representative,
        // and inside the file however short it is
        const int64_t start = info.audioSampleCount / 3;
        if (!dec.GetAudio(start, want, ptrs.data())) {
            printf("  track %d: FAILED - %s\n", track, dec.LastError().c_str());
            continue;
        }

        float peak = 0.0f;
        for (int c = 0; c < ch; ++c) {
            for (int i = 0; i < want; ++i) {
                const float v = std::fabs(abuf[c][i]);
                if (v > peak) peak = v;
            }
        }

        // Decibels, not a 0..1 fraction: a quiet but live track reads as zero in
        // fractions and looks identical to an empty one
        const double db = peak > 0 ? 20.0 * std::log10(peak) : -120.0;

        printf("  track %d: %d ch, %d Hz, %lld samples, peak %.1f dB%s\n",
               track, ch, info.audioSampleRate, (long long)info.audioSampleCount,
               db, db < -90.0 ? "  (digital silence)" : "");
    }

    // --- checks ---
    printf("\nchecks:\n");
    CheckAlphaSurvives(dec, info);
    CheckReadingPastTheEnd(dec, info);
    CheckDeepColour(dec, info);
    CheckReducedSizeStaysInBuffer(dec, info);
    CheckCacheMatchesFreshDecode(dec, path, info, preferHardware);
    CheckAudioIsRepeatable(dec, info);
    CheckTimelinePicksFrameByTime(dec, info);
    CheckSoundMatchesPicture(dec, info, requireSync);

    printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                     : "SOME CHECKS FAILED");
    return g_failures == 0 ? 0 : 4;
}
