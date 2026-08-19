// Проверка слоя декодирования без Premiere.
//
//   decoder_test.exe <файл.mp4> [номер_кадра]
//
// Печатает параметры файла, декодирует кадр, замеряет скорость последовательного
// чтения и сохраняет кадр в BMP — чтобы глазами убедиться, что картинка верная,
// а не зелёная каша из-за перепутанных плоскостей цвета.

#include "../src/AV1Decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// BMP пишем вручную: без зависимостей, и файл открывается любым просмотрщиком
bool SaveBMP(const char* path, const uint8_t* bgra, int w, int h, int stride) {
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
    int negH = -h;        memcpy(&header[22], &negH, 4);   // минус = строки сверху вниз
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: decoder_test <file> [frame]\n");
        return 1;
    }

    const std::string path = argv[1];
    const int64_t wanted = (argc > 2) ? _atoi64(argv[2]) : 0;

    av1imp::Decoder dec;
    if (!dec.Open(path, /*preferHardware=*/true)) {
        printf("OPEN FAILED: %s\n", dec.LastError().c_str());
        return 2;
    }

    const auto& info = dec.Info();
    printf("resolution : %dx%d\n", info.width, info.height);
    printf("fps        : %.3f\n", info.fps);
    printf("duration   : %.2f s\n", info.durationSec);
    printf("frames     : %lld\n", (long long)info.frameCount);
    printf("decoder    : %s (%s)\n", info.decoderName.c_str(),
           info.hardwareDecode ? "GPU" : "CPU");

    const int stride = info.width * 4;
    std::vector<uint8_t> buf((size_t)stride * info.height);

    // одиночный кадр
    auto t0 = std::chrono::steady_clock::now();
    if (!dec.GetFrameBGRA(wanted, buf.data(), stride)) {
        printf("FRAME FAILED: %s\n", dec.LastError().c_str());
        return 3;
    }
    auto t1 = std::chrono::steady_clock::now();
    double seekMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("frame %lld  : %.1f ms (with seek)\n", (long long)wanted, seekMs);

    if (SaveBMP("frame_out.bmp", buf.data(), info.width, info.height, stride)) {
        printf("saved      : frame_out.bmp\n");
    }

    // последовательное чтение — так Premiere читает при воспроизведении
    const int kSeq = 60;
    t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 1; i <= kSeq; ++i) {
        if (dec.GetFrameBGRA(wanted + i, buf.data(), stride)) ++ok;
    }
    t1 = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("sequential : %d/%d frames, %.1f ms total, %.1f fps\n",
           ok, kSeq, totalMs, ok * 1000.0 / totalMs);

    // --- звук ---
    printf("\naudio      : %d дорожек\n", info.audioStreamCount);

    for (int track = 0; track < info.audioStreamCount; ++track) {
        if (!dec.OpenAudio(track)) {
            printf("  дорожка %d: ОШИБКА - %s\n", track, dec.LastError().c_str());
            continue;
        }

        const int ch = info.audioChannels;
        const int32_t want = info.audioSampleRate;   // одна секунда

        std::vector<std::vector<float>> buf(ch, std::vector<float>(want));
        std::vector<float*> ptrs(ch);
        for (int c = 0; c < ch; ++c) ptrs[c] = buf[c].data();

        // Читаем с 10-й секунды: начало записи часто тихое, и по нему нельзя
        // отличить рабочую дорожку от пустой
        const int64_t start = (int64_t)info.audioSampleRate * 10;
        if (!dec.GetAudio(start, want, ptrs.data())) {
            printf("  дорожка %d: ОШИБКА - %s\n", track, dec.LastError().c_str());
            continue;
        }

        float peak = 0.0f;
        for (int c = 0; c < ch; ++c) {
            for (int i = 0; i < want; ++i) {
                float v = buf[c][i] < 0 ? -buf[c][i] : buf[c][i];
                if (v > peak) peak = v;
            }
        }

        // В децибелах, а не в долях единицы: тихая, но живая дорожка
        // в долях выглядит нулём, и её не отличить от пустой
        const double db = peak > 0 ? 20.0 * log10(peak) : -120.0;

        printf("  дорожка %d: %d кан, %d Hz, %lld отсчётов, пик %.1f дБ%s\n",
               track, ch, info.audioSampleRate, (long long)info.audioSampleCount,
               db, db < -90.0 ? "  (цифровая тишина)" : "");
    }

    return 0;
}
