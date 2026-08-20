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
void CheckReducedSizeStaysInBuffer(av1imp::Decoder& dec, const av1imp::MediaInfo& info)
{
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
    const int64_t at = (int64_t)info.audioSampleRate * 5;

    std::vector<std::vector<float>> a(ch, std::vector<float>(count));
    std::vector<std::vector<float>> b(ch, std::vector<float>(count));
    std::vector<std::vector<float>> scratch(ch, std::vector<float>(count));

    std::vector<float*> pa(ch), pb(ch), ps(ch);
    for (int c = 0; c < ch; ++c) { pa[c] = a[c].data(); pb[c] = b[c].data(); ps[c] = scratch[c].data(); }

    const bool first = dec.GetAudio(at, count, pa.data());
    dec.GetAudio((int64_t)info.audioSampleRate * 15, count, ps.data());  // force a seek away
    const bool again = dec.GetAudio(at, count, pb.data());

    Check(first && again && a == b, "same audio range reads identically");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: decoder_test <file> [frame] [--sw]\n");
        printf("  --sw  force the software decoder, as the plug-in does when\n");
        printf("        hardware decoding is switched off in its settings\n");
        return 1;
    }

    std::string path;
    int64_t wanted = 0;
    bool preferHardware = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sw")      preferHardware = false;
        else if (path.empty())  path = arg;
        else                    wanted = _atoi64(arg.c_str());
    }

    av1imp::Decoder dec;
    if (!dec.Open(path, preferHardware)) {
        printf("OPEN FAILED: %s\n", dec.LastError().c_str());
        return 2;
    }

    const auto& info = dec.Info();
    printf("resolution : %dx%d\n", info.width, info.height);
    printf("fps        : %.3f (%d/%d)\n", info.fps, info.fpsNum, info.fpsDen);
    printf("duration   : %.2f s\n", info.durationSec);
    printf("frames     : %lld\n", (long long)info.frameCount);
    printf("decoder    : %s (%s)\n", info.decoderName.c_str(),
           info.hardwareDecode ? "GPU" : "CPU");

    const int stride = info.width * 4;
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
        if (dec.GetFrameBGRA(wanted + i, buf.data(), stride, 0, 0)) ++ok;
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
        const int64_t start = (int64_t)info.audioSampleRate * 10;
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
    CheckReducedSizeStaysInBuffer(dec, info);
    CheckCacheMatchesFreshDecode(dec, path, info, preferHardware);
    CheckAudioIsRepeatable(dec, info);

    printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                     : "SOME CHECKS FAILED");
    return g_failures == 0 ? 0 : 4;
}
