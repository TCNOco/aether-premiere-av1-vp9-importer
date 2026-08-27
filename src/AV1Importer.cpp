// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Импортёр AV1 для Adobe Premiere Pro — слой между Premiere и AV1Decoder.
//
// Premiere общается с импортёром одной функцией xImportEntry: присылает номер
// запроса (селектор) и структуру с полями "вход/выход". Здесь обработаны только
// те запросы, без которых файл не встанет на таймлайн; на всё остальное отвечаем
// imUnsupported, и Premiere просто не будет этим пользоваться.

#include "AV1Importer.h"
#include "AV1Async.h"
#include "AV1Log.h"
#include "AV1Settings.h"
#include "ImporterMath.h"
#include "PreviewCache.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>     // std::size — длина буфера пути берётся из него самого
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <condition_variable>
#include <vector>

// ---------------------------------------------------------------------------
// Вспомогательное
// ---------------------------------------------------------------------------

namespace {

// Premiere отдаёт пути в UTF-16, ffmpeg ждёт UTF-8
std::string Utf8FromUtf16(const prUTF16Char* path)
{
    if (!path) return std::string();

    const int need = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(path), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();

    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(path), -1,
                        &out[0], need, nullptr, nullptr);
    return out;
}

// Хост один раз отказал в буфере P010 — до конца процесса не дразним его этим
// форматом снова (иначе каждый кадр отказывает и предпросмотр замирает).
std::atomic<bool> g_p010Unavailable{false};

struct HostRequestProfile {
    std::mutex mutex;
    uint64_t total = 0;
    uint64_t reduced = 0;
    uint64_t full = 0;
    uint64_t bgra8 = 0;
    uint64_t bgra16 = 0;
    uint64_t yuv = 0;
    uint64_t p010 = 0;
    uint64_t otherFmt = 0;
    uint64_t qDraft = 0;
    uint64_t qLow = 0;
    uint64_t qMedium = 0;
    uint64_t qHigh = 0;
    uint64_t qOther = 0;
    uint64_t sequential = 0;
    uint64_t jump = 0;
    uint64_t first = 0;
    bool haveLast = false;
    csSDK_int32 lastFrame = 0;
    std::map<std::pair<int, int>, uint64_t> sizes;
};

HostRequestProfile g_hostReq;

void FinishImporterShutdown()
{
    av1imp::LogHostRequestProfile();
    av1imp::PreviewCache::Instance().Shutdown();
    av1imp::LogClose();
}

} // namespace

namespace av1imp {

void NoteHostVideoRequest(csSDK_int32 frameIndex,
                          int nativeWidth, int nativeHeight,
                          int askedWidth, int askedHeight,
                          PrPixelFormat pixelFormat,
                          PrRenderQuality quality)
{
    std::lock_guard<std::mutex> lock(g_hostReq.mutex);
    ++g_hostReq.total;
    const bool reduced = askedWidth > 0 && askedHeight > 0 &&
                         nativeWidth > 0 && nativeHeight > 0 &&
                         (askedWidth < nativeWidth || askedHeight < nativeHeight);
    if (reduced) ++g_hostReq.reduced;
    else ++g_hostReq.full;

    if (pixelFormat == PrPixelFormat_BGRA_4444_8u) ++g_hostReq.bgra8;
    else if (pixelFormat == PrPixelFormat_BGRA_4444_16u) ++g_hostReq.bgra16;
    else if (IsNativeP010(pixelFormat)) ++g_hostReq.p010;
    else if (IsNativeYUV(pixelFormat)) ++g_hostReq.yuv;
    else ++g_hostReq.otherFmt;

    switch (quality) {
        case kPrRenderQuality_Draft:  ++g_hostReq.qDraft; break;
        case kPrRenderQuality_Low:    ++g_hostReq.qLow; break;
        case kPrRenderQuality_Medium: ++g_hostReq.qMedium; break;
        case kPrRenderQuality_High:   ++g_hostReq.qHigh; break;
        default:                      ++g_hostReq.qOther; break;
    }

    if (!g_hostReq.haveLast) {
        ++g_hostReq.first;
        g_hostReq.haveLast = true;
    } else if (frameIndex == g_hostReq.lastFrame + 1) {
        ++g_hostReq.sequential;
    } else {
        ++g_hostReq.jump;
    }
    g_hostReq.lastFrame = frameIndex;

    if (askedWidth > 0 && askedHeight > 0) {
        ++g_hostReq.sizes[std::make_pair(askedWidth, askedHeight)];
    }
}

void LogHostRequestProfile()
{
    std::lock_guard<std::mutex> lock(g_hostReq.mutex);
    if (g_hostReq.total == 0) return;

    Log("host requests: %llu total, %llu reduced, %llu full; "
        "BGRA8 %llu BGRA16 %llu YUV %llu P010 %llu other %llu; "
        "quality draft %llu low %llu medium %llu high %llu other %llu; "
        "seq %llu jump %llu first %llu",
        (unsigned long long)g_hostReq.total,
        (unsigned long long)g_hostReq.reduced,
        (unsigned long long)g_hostReq.full,
        (unsigned long long)g_hostReq.bgra8,
        (unsigned long long)g_hostReq.bgra16,
        (unsigned long long)g_hostReq.yuv,
        (unsigned long long)g_hostReq.p010,
        (unsigned long long)g_hostReq.otherFmt,
        (unsigned long long)g_hostReq.qDraft,
        (unsigned long long)g_hostReq.qLow,
        (unsigned long long)g_hostReq.qMedium,
        (unsigned long long)g_hostReq.qHigh,
        (unsigned long long)g_hostReq.qOther,
        (unsigned long long)g_hostReq.sequential,
        (unsigned long long)g_hostReq.jump,
        (unsigned long long)g_hostReq.first);

    std::vector<std::pair<uint64_t, std::pair<int, int>>> top;
    top.reserve(g_hostReq.sizes.size());
    for (const auto& kv : g_hostReq.sizes) {
        top.push_back({kv.second, kv.first});
    }
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t n = top.size() < 8 ? top.size() : 8;
    std::string sizes;
    for (size_t i = 0; i < n; ++i) {
        if (!sizes.empty()) sizes += ", ";
        char buf[64];
        sprintf_s(buf, "%dx%d=%llu",
                  top[i].second.first, top[i].second.second,
                  (unsigned long long)top[i].first);
        sizes += buf;
    }
    if (!sizes.empty()) {
        Log("host request sizes: %s", sizes.c_str());
    }
}

} // namespace av1imp

namespace {

// Удачный рецепт создания P010: после первого кадра не гоняем весь перебор
// и не спамим журнал на каждый кадр скраба.
std::atomic<int> g_p010Recipe{-1};   // -1 неизвестно, >=0 индекс в таблице попыток
std::atomic<int> g_p010ProbeLogs{0}; // сколько подробных строк уже написали

} // namespace

void av1imp::MarkP010Unavailable()
{
    bool expected = false;
    if (g_p010Unavailable.compare_exchange_strong(expected, true)) {
        av1imp::Log("10-bit planes: host refused a writable buffer - falling back to BGRA 16u for this session");
    }
}

bool av1imp::IsP010Unavailable()
{
    return g_p010Unavailable.load(std::memory_order_relaxed);
}

static const char* AccessName(PrPPixBufferAccess a)
{
    switch (a) {
        case PrPPixBufferAccess_ReadOnly:  return "RO";
        case PrPPixBufferAccess_WriteOnly: return "WO";
        case PrPPixBufferAccess_ReadWrite: return "RW";
        default: return "?";
    }
}

// Пробуем достать указатель всеми режимами доступа.
//
// На Premiere 26 CreatePPix для P010 отдаёт живой buf и нормальный size,
// но GetRowBytes врёт нулём. Раньше мы из-за row==0 сами выкидывали кадр,
// хотя писать было куда. Шаг тогда берём как width*2 (P010), сверяя с size.
static bool ProbeP010Pixels(PrSDKPPixSuite* ppix, PrSDKPPix2Suite* ppix2,
                            PPixHand frame, int width, int height,
                            const char* howCreated,
                            csSDK_int32* outRowBytes,
                            bool verbose)
{
    if (!ppix || !frame) return false;
    if (outRowBytes) *outRowBytes = 0;

    size_t size = 0;
    if (ppix2 && ppix2->GetSize) ppix2->GetSize(frame, &size);

    csSDK_int32 rowBytes = 0;
    ppix->GetRowBytes(frame, &rowBytes);

    const PrPPixBufferAccess modes[] = {
        PrPPixBufferAccess_WriteOnly,
        PrPPixBufferAccess_ReadWrite,
        PrPPixBufferAccess_ReadOnly,
    };
    for (PrPPixBufferAccess mode : modes) {
        char* buffer = nullptr;
        const prSuiteError err = ppix->GetPixels(frame, mode, &buffer);
        if (verbose) {
            av1imp::Log("10-bit planes: create=%s GetPixels(%s) err=0x%08X buf=%p "
                        "row=%d size=%zu %dx%d",
                        howCreated, AccessName(mode), (unsigned)err, (void*)buffer,
                        (int)rowBytes, size, width, height);
        }
        if (err != suiteError_NoError || !buffer) continue;

        csSDK_int32 stride = rowBytes;
        if (stride <= 0 && width > 0) {
            const int guess = width * (int)sizeof(uint16_t);
            const size_t chromaH = ((size_t)height + 1) / 2;
            const size_t need = (size_t)guess * (size_t)height + (size_t)guess * chromaH;
            if (size == 0 || size >= need) {
                stride = guess;
                if (verbose) {
                    av1imp::Log("10-bit planes: GetRowBytes was %d, using stride %d from width "
                                "(size %zu, need %zu)",
                                (int)rowBytes, stride, size, need);
                }
            }
        }
        if (stride <= 0) continue;

        if (size > 0) {
            const size_t chromaH = ((size_t)height + 1) / 2;
            const size_t need = (size_t)stride * (size_t)height + (size_t)stride * chromaH;
            if (size < need) {
                if (verbose) {
                    av1imp::Log("10-bit planes: size %zu < need %zu at stride %d - skip",
                                size, need, (int)stride);
                }
                continue;
            }
        }

        if (outRowBytes) *outRowBytes = stride;
        return true;
    }
    return false;
}

// Шаг строки, которым реально писать в P010-кадр: либо то, что сказал хост,
// либо width*2, если он врёт нулём при живом буфере.
static csSDK_int32 ResolveP010Stride(PrSDKPPixSuite* ppix, PrSDKPPix2Suite* ppix2,
                                     PPixHand frame, int width, int height,
                                     csSDK_int32 reportedRow)
{
    if (reportedRow > 0) return reportedRow;
    if (width <= 0) return 0;

    size_t size = 0;
    if (ppix2 && ppix2->GetSize) ppix2->GetSize(frame, &size);

    const int guess = width * (int)sizeof(uint16_t);
    const size_t chromaH = ((size_t)height + 1) / 2;
    const size_t need = (size_t)guess * (size_t)height + (size_t)guess * chromaH;
    if (size == 0 || size >= need) return guess;
    return 0;
}

prMALError av1imp::CreateVideoPPix(PrSDKPPixCreatorSuite* creator,
                                   PrSDKPPixCreator2Suite* creator2,
                                   PrSDKPPixSuite* ppixSuite,
                                   PrSDKPPix2Suite* ppix2Suite,
                                   PPixHand* outFrame,
                                   PrPixelFormat pixelFormat,
                                   int width, int height)
{
    if (!outFrame || !UsableFrameSize(width, height)) return imOtherErr;
    *outFrame = nullptr;

    if (!av1imp::IsNativeP010(pixelFormat)) {
        if (!creator || !creator->CreatePPix) return imOtherErr;
        prRect rect;
        prSetRect(&rect, 0, 0, width, height);
        const prSuiteError err = creator->CreatePPix(outFrame, PrPPixBufferAccess_ReadWrite,
                                                     pixelFormat, &rect);
        return (err == suiteError_NoError && *outFrame) ? malNoError : imOtherErr;
    }

    // --- P010 ----------------------------------------------------------------
    // CreateCustomPPix — только для своих opaque FourCC (InvalidParms на BIPLANAR).
    // Штатный CreatePPix + WriteOnly работает; GetRowBytes на 26 врёт нулём.

    struct Recipe {
        const char* tag;
        int which; // 0 C2 WO, 1 C2 RW, 2 C1 WO, 3 C1 RW
    };
    static const Recipe kRecipes[] = {
        { "C2.CreatePPix(WO)", 0 },
        { "C2.CreatePPix(RW)", 1 },
        { "C1.CreatePPix(WO)", 2 },
        { "C1.CreatePPix(RW)", 3 },
    };

    auto runRecipe = [&](int which) -> prSuiteError {
        *outFrame = nullptr;
        switch (which) {
            case 0:
                if (!creator2 || !creator2->CreatePPix) return suiteError_NotImplemented;
                return creator2->CreatePPix(outFrame, PrPPixBufferAccess_WriteOnly,
                                            pixelFormat, width, height, false, 0, 1, 1);
            case 1:
                if (!creator2 || !creator2->CreatePPix) return suiteError_NotImplemented;
                return creator2->CreatePPix(outFrame, PrPPixBufferAccess_ReadWrite,
                                            pixelFormat, width, height, false, 0, 1, 1);
            case 2: {
                if (!creator || !creator->CreatePPix) return suiteError_NotImplemented;
                prRect rect; prSetRect(&rect, 0, 0, width, height);
                return creator->CreatePPix(outFrame, PrPPixBufferAccess_WriteOnly,
                                           pixelFormat, &rect);
            }
            case 3: {
                if (!creator || !creator->CreatePPix) return suiteError_NotImplemented;
                prRect rect; prSetRect(&rect, 0, 0, width, height);
                return creator->CreatePPix(outFrame, PrPPixBufferAccess_ReadWrite,
                                           pixelFormat, &rect);
            }
            default:
                return suiteError_NotImplemented;
        }
    };

    const bool verbose = g_p010ProbeLogs.load(std::memory_order_relaxed) < 3;
    const int known = g_p010Recipe.load(std::memory_order_relaxed);

    auto tryOne = [&](int index) -> bool {
        const Recipe& r = kRecipes[index];
        const prSuiteError err = runRecipe(r.which);
        if (err != suiteError_NoError || !*outFrame) {
            if (verbose) {
                av1imp::Log("10-bit planes: %s -> err=0x%08X hand=%p",
                            r.tag, (unsigned)err, (void*)*outFrame);
            }
            *outFrame = nullptr;
            return false;
        }
        csSDK_int32 stride = 0;
        if (!ProbeP010Pixels(ppixSuite, ppix2Suite, *outFrame, width, height,
                             r.tag, &stride, verbose)) {
            if (ppixSuite) ppixSuite->Dispose(*outFrame);
            *outFrame = nullptr;
            return false;
        }
        if (verbose) {
            av1imp::Log("10-bit planes: WRITABLE via %s (stride %d)", r.tag, (int)stride);
            g_p010ProbeLogs.fetch_add(1, std::memory_order_relaxed);
        }
        g_p010Recipe.store(index, std::memory_order_relaxed);
        return true;
    };

    if (known >= 0 && known < (int)(sizeof(kRecipes) / sizeof(kRecipes[0]))) {
        if (tryOne(known)) return malNoError;
        g_p010Recipe.store(-1, std::memory_order_relaxed);
    }

    for (int i = 0; i < (int)(sizeof(kRecipes) / sizeof(kRecipes[0])); ++i) {
        if (tryOne(i)) return malNoError;
    }

    if (verbose) {
        av1imp::Log("10-bit planes: all create paths failed for %dx%d fmt=0x%08X",
                    width, height, (unsigned)pixelFormat);
        g_p010ProbeLogs.fetch_add(1, std::memory_order_relaxed);
    }
    return imOtherErr;
}

namespace {

// Возвращает false, если путь не поместился целиком.
//
// Прежде обрезание было молчаливым, и это худший вид отказа: ffmpeg получал
// осмысленный, но чужой путь — либо не открывал ничего, либо открывал не тот
// файл, — а в журнале не было ни строчки о том, что путь вообще резали.
bool CopyUtf16(prUTF16Char* dst, size_t dstCount, const prUTF16Char* src)
{
    // Нулевой размер: писать некуда, даже завершающий нуль. Без этой строки
    // dstCount - 1 уходит в переполнение беззнакового и превращается в
    // «почти бесконечность», а dst[0] = 0 пишет мимо буфера.
    if (!dst || dstCount == 0) return false;

    size_t i = 0;
    if (src) {
        for (; src[i] && i < dstCount - 1; ++i) dst[i] = src[i];
    }
    dst[i] = 0;

    // Не поместилось, если исходник на этом месте ещё не кончился
    return !src || src[i] == 0;
}

// Замки на ленивое открытие — набор, а не один на всех.
//
// Зачем замок вообще. Один и тот же экземпляр импортёра Premiere зовёт из
// РАЗНЫХ потоков разом: конформирование звука идёт в своём, показ клипа просит
// кадры в другом, и оба приходят к потоку 0, где живут и видео, и первая
// дорожка. Обе функции ниже проверяют «уже открыто?» и открывают, если нет, —
// то есть классическая проверка-и-действие, а между ними успевает вклиниться
// сосед. Два потока входят в открытие вместе, второй сносит построенное первым
// (Open и OpenAudio начинают со сноса прежнего состояния), и запрос возвращает
// отказ. Для пользователя это «An unspecified error occurred while performing
// a conform action» и пропавшая дорожка звука, а при повторном импорте всё
// хорошо — потому что совпасть по времени во второй раз не обязано.
//
// Почему НЕ один на весь плагин, как было. Замок держится всё время, пока идёт
// Open(), а внутри него avformat_find_stream_info, который по умолчанию читает
// и декодирует до пяти секунд материала. Прежний комментарий говорил «цена
// нулевая, сюда заходят один раз на файл» — и это верно ровно для одного файла.
// В журнале живого сеанса 45 вызовов imOpenFile8, и все их открытия вставали
// в одну очередь. Плюс у записи с четырьмя дорожками звука экземпляров четыре,
// и каждый разбирает контейнер.
//
// Защищать надо не «открытие вообще», а КОНКРЕТНУЮ структуру от самой себя.
// Положить замок в ImporterLocalRec нельзя — Premiere выдаёт под неё сырую
// память без вызова конструкторов, — поэтому берём набор замков и выбираем
// по адресу структуры. Один и тот же ldata всегда попадает на один и тот же
// замок (это и требовалось), разные почти всегда на разные. Совпадение адресов
// по остатку стоит немного лишнего ожидания и ничего не ломает.
std::mutex g_openMutexes[64];
std::condition_variable g_openCvs[64];

std::mutex& OpenMutexFor(const void* ldata)
{
    // Младшие биты адреса выделения почти всегда нули (выравнивание), поэтому
    // сдвигаем, прежде чем брать остаток: иначе все структуры сели бы
    // на несколько замков из шестидесяти четырёх.
    const size_t bits = reinterpret_cast<size_t>(ldata) >> 5;
    return g_openMutexes[bits % (sizeof(g_openMutexes) / sizeof(g_openMutexes[0]))];
}

std::condition_variable& OpenCvFor(const void* ldata)
{
    const size_t bits = reinterpret_cast<size_t>(ldata) >> 5;
    return g_openCvs[bits % (sizeof(g_openCvs) / sizeof(g_openCvs[0]))];
}

bool EnsureDecoderLocked(ImporterLocalRecPtr ldata);

// Держит Decoder* живым, пока хост читает кадр или звук. CloseFile ждёт
// inFlight==0, потом delete. Без этого Premiere закрывает файл на одном
// потоке, а GetFrame на другом уже держит указатель.
struct DecoderPin {
    ImporterLocalRecPtr ldata = nullptr;
    av1imp::Decoder* decoder = nullptr;

    DecoderPin() = default;
    DecoderPin(const DecoderPin&) = delete;
    DecoderPin& operator=(const DecoderPin&) = delete;
    DecoderPin(DecoderPin&& other) noexcept
        : ldata(other.ldata), decoder(other.decoder)
    {
        other.ldata = nullptr;
        other.decoder = nullptr;
    }
    DecoderPin& operator=(DecoderPin&& other) noexcept
    {
        if (this != &other) {
            reset();
            ldata = other.ldata;
            decoder = other.decoder;
            other.ldata = nullptr;
            other.decoder = nullptr;
        }
        return *this;
    }
    ~DecoderPin() { reset(); }

    explicit operator bool() const { return decoder != nullptr; }

    void reset()
    {
        if (!ldata) return;
        ImporterLocalRecPtr hold = ldata;
        ldata = nullptr;
        decoder = nullptr;
        std::lock_guard<std::mutex> lock(OpenMutexFor(hold));
        if (hold->inFlight > 0) --hold->inFlight;
        OpenCvFor(hold).notify_all();
    }
};

bool PinDecoder(ImporterLocalRecPtr ldata, DecoderPin* pin)
{
    if (!ldata || !pin) return false;
    pin->reset();
    std::unique_lock<std::mutex> lock(OpenMutexFor(ldata));
    if (ldata->closing) return false;
    if (!EnsureDecoderLocked(ldata)) return false;
    ++ldata->inFlight;
    pin->ldata = ldata;
    pin->decoder = ldata->decoder;
    return pin->decoder != nullptr;
}

void WaitUntilIdleLocked(ImporterLocalRecPtr ldata, std::unique_lock<std::mutex>& lock)
{
    OpenCvFor(ldata).wait(lock, [&] { return ldata->inFlight == 0; });
}

// Premiere может спросить сведения о файле раньше, чем откроет его,
// поэтому декодер создаём по требованию из сохранённого пути.
bool EnsureDecoderLocked(ImporterLocalRecPtr ldata)
{
    if (ldata->closing) return false;
    if (ldata->decoder && ldata->decoder->IsOpen()) return true;

    if (!ldata->decoder) ldata->decoder = new av1imp::Decoder();

    const std::string path = Utf8FromUtf16(ldata->filePath);
    if (path.empty()) return false;

    // Кадры отдаёт только поток 0; остальным нужен лишь звук, и декодер
    // на видеокарте им создавать незачем
    const bool needVideo = (ldata->streamIdx == 0);
    return ldata->decoder->Open(path, av1imp::PreferHardware(), needVideo);
}

std::string SafeDecoderError(ImporterLocalRecPtr ldata)
{
    std::lock_guard<std::mutex> lock(OpenMutexFor(ldata));
    if (!ldata || !ldata->decoder) return "no decoder";
    return ldata->decoder->LastError();
}

std::string SafeDecoderAudioError(ImporterLocalRecPtr ldata)
{
    std::lock_guard<std::mutex> lock(OpenMutexFor(ldata));
    if (!ldata || !ldata->decoder) return "no decoder";
    return ldata->decoder->LastAudioError();
}

// Дорожка звука открывается по требованию: Premiere спрашивает отсчёты
// не у всех потоков и не сразу. Вызывать, пока жив DecoderPin: иначе
// CloseFile может удалить декодер между проверкой и OpenAudio.
bool EnsureAudio(ImporterLocalRecPtr ldata)
{
    if (!ldata || !ldata->decoder) return false;

    // Сравниваем НОМЕР дорожки, а не просто «звук уже открыт».
    //
    // Premiere перебирает потоки, зовя imGetInfo8 с номерами 0,1,2,3 на одной
    // и той же записи, и номер дорожки при этом переписывается. Прежняя проверка
    // «HasAudio?» со второго раза отвечала «да» — и сведения о дорожках 1..N
    // брались от дорожки 0: число каналов, частота, длина.
    //
    // На записях OBS не видно, там все дорожки одинаковые. Вылезло бы на файле,
    // где дорожки разной длины или разной раскладки каналов.
    if (ldata->decoder->HasAudio() &&
        ldata->decoder->OpenAudioTrack() == ldata->audioTrack) {
        return true;
    }
    return ldata->decoder->OpenAudio(ldata->audioTrack);
}

// Перебор версий набора переехал в заголовок: его теперь зовёт и асинхронный
// импортёр, который берёт наборы себе, а не одалживает наши. Имя короче не
// стало, зато копии больше нет.
using av1imp::AcquireNewestSuite;

// Плагин лежит в общей папке MediaCore, а значит попадает разом во все
// установленные приложения Adobe и во все их версии. Что именно нас загрузило —
// первое, что нужно знать по чужому журналу.
void LogHost(imStdParms* stdParms)
{
    av1imp::Log("host: import interface version %d (SDK headers know %d)",
                stdParms->imInterfaceVer, IMPORTMOD_VERSION);

    if (!stdParms->piSuites || !stdParms->piSuites->utilFuncs) return;
    SPBasicSuite* basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
    if (!basic) return;

    PrSDKAppInfoSuite* appInfo = nullptr;
    const csSDK_int32 version = AcquireNewestSuite(basic, kPrSDKAppInfoSuite,
                                              kPrSDKAppInfoSuiteVersion, &appInfo);
    if (!version) {
        av1imp::Log("host: app info suite unavailable");
        return;
    }

    csSDK_uint32 fourCC = 0;
    VersionInfo appVersion = {};
    appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, &fourCC);
    appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_Version, &appVersion);

    const char name[5] = { static_cast<char>((fourCC >> 24) & 0xFF),
                           static_cast<char>((fourCC >> 16) & 0xFF),
                           static_cast<char>((fourCC >> 8)  & 0xFF),
                           static_cast<char>(fourCC & 0xFF), 0 };
    av1imp::Log("host: %s %u.%u.%u", name,
                appVersion.major, appVersion.minor, appVersion.patch);

    basic->ReleaseSuite(kPrSDKAppInfoSuite, version);
}

ImporterLocalRecH AllocLocalRec(imStdParms* stdParms, void** privateData)
{
    if (*privateData) {
        return reinterpret_cast<ImporterLocalRecH>(*privateData);
    }
    ImporterLocalRecH h = reinterpret_cast<ImporterLocalRecH>(
        stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec)));
    if (h) {
        // newHandle отдаёт сырую память: обнуляем сами, конструкторов тут нет
        memset(*h, 0, sizeof(ImporterLocalRec));
        *privateData = reinterpret_cast<void*>(h);
    }
    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// imInit — что плагин умеет вообще
// ---------------------------------------------------------------------------

static prMALError AV1Init(imStdParms* stdParms, imImportInfoRec* importInfo)
{
    importInfo->canSave         = kPrFalse;
    importInfo->canDelete       = kPrFalse;
    importInfo->canTrim         = kPrFalse;
    importInfo->canCalcSizes    = kPrFalse;
    importInfo->hasSetup        = kPrFalse;
    importInfo->setupOnDblClk   = kPrFalse;
    importInfo->dontCache       = kPrFalse;
    // Хост иначе выгружает .prm, не дожидаясь aiClose / GetFrame: асинхронный
    // работник и пины декодера тогда остаются внутри уже снятого модуля.
    importInfo->keepLoaded      = kPrTrue;

    // Чтобы получить .mp4 раньше штатного импортёра Premiere, нужно ровно это:
    // в документации SDK сказано, что для перебивания импортёров самой Adobe
    // приоритет должен быть 100 и выше. Со значением ниже Premiere разбирает
    // файл сам и до плагина не доходит.
    // Файлы не с AV1 мы тут же вернём обратно через imBadFile, и разбирать
    // их будет всё тот же штатный импортёр.
    importInfo->priority        = 100;

    av1imp::Log("imInit: priority %d", importInfo->priority);
    LogHost(stdParms);
    return imIsCacheable;
}

// ---------------------------------------------------------------------------
// imGetIndFormat — какие файлы забирать
// ---------------------------------------------------------------------------

static prMALError AV1GetIndFormat(imStdParms* stdParms, csSDK_size_t index,
                                  imIndFormatRec* rec)
{
    if (index != 0) return imBadFormatIndex;

    // Расширения перечисляются через нули: AV1 кладут в эти контейнеры
    static const char kExtensions[] = "mp4\0mkv\0webm\0mov\0m4v\0mka\0";

    rec->filetype         = AV1_FILE_TYPE;
    rec->canWriteTimecode = kPrFalse;
    rec->canWriteMetaData = kPrFalse;
    rec->flags            = xfCanOpen + xfCanImport + xfIsMovie;

    strcpy_s(rec->FormatName,      sizeof(rec->FormatName),      AV1_IMPORTER_NAME);
    strcpy_s(rec->FormatShortName, sizeof(rec->FormatShortName), AV1_IMPORTER_SHORT);
    memcpy(rec->PlatformExtension, kExtensions, sizeof(kExtensions));

    return malNoError;
}

// ---------------------------------------------------------------------------
// imOpenFile8 / imQuietFile / imCloseFile
// ---------------------------------------------------------------------------

static prMALError AV1OpenFile8(imStdParms* stdParms, imFileRef* fileRef,
                               imFileOpenRec8* openRec)
{
    ImporterLocalRecH ldataH = AllocLocalRec(stdParms, &openRec->privatedata);
    if (!ldataH) return imMemErr;

    ImporterLocalRecPtr ldata = *ldataH;
    if (!CopyUtf16(ldata->filePath, std::size(ldata->filePath),
                   openRec->fileinfo.filepath)) {
        av1imp::Log("imOpenFile8: path longer than %d characters - refusing rather than "
                    "opening whatever the cut path points at", AV1_PATH_CHARS);
        return imBadFile;
    }

    // См. раскладку в AV1GetInfo8: поток 0 несёт видео и дорожку звука 0,
    // потоки 1..N — только звук соответствующей дорожки
    ldata->streamIdx  = openRec->inStreamIdx;
    ldata->audioTrack = openRec->inStreamIdx;

    // Дескриптор нужен самому Premiere; читаем мы через ffmpeg, поэтому
    // открываем на чтение и разрешаем параллельное чтение другим.
    if (ldata->fileRef == nullptr || ldata->fileRef == imInvalidHandleValue) {
        ldata->fileRef = CreateFileW(reinterpret_cast<LPCWSTR>(openRec->fileinfo.filepath),
                                     GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (ldata->fileRef == imInvalidHandleValue) {
        return imBadFile;
    }

    // Главная проверка: файл наш, только если внутри поддерживаемый кодек.
    // Иначе честно отдаём его обратно — Premiere передаст штатному импортёру.
    DecoderPin pin;
    if (!PinDecoder(ldata, &pin)) {
        av1imp::Log("imOpenFile8: refused - %s", SafeDecoderError(ldata).c_str());
        CloseHandle(ldata->fileRef);
        ldata->fileRef = imInvalidHandleValue;
        return imBadFile;
    }
    const av1imp::MediaInfo mi = pin.decoder->Info();
    if (mi.hasVideo) {
        av1imp::Log("imOpenFile8: accepted, %s %dx%d, decoder %s",
                    mi.codecName.c_str(), mi.width, mi.height,
                    mi.decoderName.c_str());
    } else {
        av1imp::Log("imOpenFile8: accepted, no video, %d audio track(s)",
                    mi.audioStreamCount);
    }

    *fileRef                   = ldata->fileRef;
    openRec->fileinfo.fileref  = ldata->fileRef;
    openRec->fileinfo.filetype = AV1_FILE_TYPE;

    return malNoError;
}

// Premiere просит освободить ресурсы, но файл ещё понадобится
static prMALError AV1QuietFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData)
{
    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);
    if (!ldataH) return malNoError;

    ImporterLocalRecPtr ldata = *ldataH;
    av1imp::Log("imQuietFile: stream %d", ldata->streamIdx);
    {
        std::unique_lock<std::mutex> lock(OpenMutexFor(ldata));
        WaitUntilIdleLocked(ldata, lock);
        if (ldata->decoder) {
            ldata->decoder->Close();  // сам объект оставляем: путь известен, откроемся заново
        }
    }
    if (ldata->fileRef && ldata->fileRef != imInvalidHandleValue) {
        CloseHandle(ldata->fileRef);
        ldata->fileRef = imInvalidHandleValue;
    }
    *fileRef = imInvalidHandleValue;
    return malNoError;
}

static prMALError AV1CloseFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData)
{
    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);
    if (!ldataH) return malNoError;

    ImporterLocalRecPtr ldata = *ldataH;
    av1imp::Log("imCloseFile: stream %d", ldata->streamIdx);

    {
        std::unique_lock<std::mutex> lock(OpenMutexFor(ldata));
        ldata->closing = 1;
        WaitUntilIdleLocked(ldata, lock);
        if (ldata->decoder) {
            const av1imp::Decoder::Stats stats = ldata->decoder->GetStats();
            if (stats.previewCacheHits || stats.previewCacheMisses ||
                stats.previewCacheWritesQueued || stats.previewCacheWritesDropped) {
                av1imp::Log(
                    "preview cache: hit %lld, miss %lld, queued %lld, dropped %lld, read %.2f ms",
                    (long long)stats.previewCacheHits,
                    (long long)stats.previewCacheMisses,
                    (long long)stats.previewCacheWritesQueued,
                    (long long)stats.previewCacheWritesDropped,
                    stats.previewCacheReadMs);
            }
            delete ldata->decoder;
            ldata->decoder = nullptr;
        }
    }
    if (ldata->fileRef && ldata->fileRef != imInvalidHandleValue) {
        CloseHandle(ldata->fileRef);
        ldata->fileRef = imInvalidHandleValue;
    }
    // Отдаём ровно те наборы, которые взяли, и той версией, какой брали.
    // Ноль в версии означает «не выдали» — тогда и возвращать нечего.
    if (ldata->BasicSuite) {
        if (ldata->PPixCreatorSuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, ldata->PPixCreatorSuiteVersion);
        }
        if (ldata->PPixCreator2SuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKPPixCreator2Suite, ldata->PPixCreator2SuiteVersion);
        }
        if (ldata->PPixCacheSuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKPPixCacheSuite, ldata->PPixCacheSuiteVersion);
        }
        if (ldata->PPix2SuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKPPix2Suite, ldata->PPix2SuiteVersion);
        }
        if (ldata->PPixSuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, ldata->PPixSuiteVersion);
        }
        if (ldata->TimeSuiteVersion) {
            ldata->BasicSuite->ReleaseSuite(kPrSDKTimeSuite, ldata->TimeSuiteVersion);
        }
        ldata->PPixCreatorSuite = nullptr;
        ldata->PPixCreator2Suite = nullptr;
        ldata->PPixSuite        = nullptr;
        ldata->PPix2Suite       = nullptr;
        ldata->PPixCacheSuite   = nullptr;
        ldata->TimeSuite        = nullptr;
        ldata->PPixCreatorSuiteVersion  = 0;
        ldata->PPixCreator2SuiteVersion = 0;
        ldata->PPixSuiteVersion         = 0;
        ldata->TimeSuiteVersion         = 0;
        ldata->PPixCacheSuiteVersion    = 0;
        ldata->PPix2SuiteVersion        = 0;
        ldata->BasicSuite = nullptr;
    }

    *fileRef = imInvalidHandleValue;
    return malNoError;
}

// ---------------------------------------------------------------------------
// imGetInfo8 — размер кадра, частота, длительность
// ---------------------------------------------------------------------------

static prMALError AV1GetInfo8(imStdParms* stdParms, imFileAccessRec8* fileAccess,
                              imFileInfoRec8* fileInfo)
{
    ImporterLocalRecH ldataH = AllocLocalRec(stdParms, &fileInfo->privatedata);
    if (!ldataH) return imMemErr;

    stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));
    ImporterLocalRecPtr ldata = *ldataH;

    if (ldata->filePath[0] == 0) {
        if (!CopyUtf16(ldata->filePath, std::size(ldata->filePath), fileAccess->filepath)) {
            av1imp::Log("imGetInfo8: path longer than %d characters", AV1_PATH_CHARS);
            stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
            return imBadFile;
        }
    }

    av1imp::Log("imGetInfo8: asked about %s (stream %d)",
                av1imp::LogPath(reinterpret_cast<const wchar_t*>(ldata->filePath)).c_str(),
                fileInfo->streamIdx);

    // Записать номер потока нужно до открытия: от него зависит,
    // создавать ли декодер кадров
    // Раскладка потоков, явно: поток 0 = видео + дорожка звука 0,
    // потоки 1..N = только дорожка звука N. Номер дорожки совпадает с номером
    // потока не случайно, а по этому правилу.
    ldata->streamIdx  = fileInfo->streamIdx;
    ldata->audioTrack = fileInfo->streamIdx;

    DecoderPin pin;
    if (!PinDecoder(ldata, &pin)) {
        av1imp::Log("imGetInfo8: refused - %s", SafeDecoderError(ldata).c_str());
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        return imBadFile;
    }

    const av1imp::MediaInfo mi = pin.decoder->Info();

    ldata->memFuncs = stdParms->piSuites->memFuncs;

    // Premiere спрашивает сведения о файле не единожды, а наборы функций
    // положено брать и отдавать поровну — иначе накопим лишние ссылки
    if (!ldata->BasicSuite) {
        ldata->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
    }
    if (ldata->BasicSuite && !ldata->PPixSuite) {
        // Эти три набора первой версии и есть во всех Premiere, где плагины
        // вообще живут — но «есть везде» не повод не проверять ответ.
        //
        // Прежде результат AcquireSuite не смотрели вовсе, а отдавали набор
        // в imCloseFile безусловно. Не выдай хост набор — мы бы вернули
        // ссылку, которой не брали, то есть уронили бы чужой счётчик ниже
        // нуля. Для двух наборов с перебором версий это уже учитывалось
        // (по ненулевой версии), для трёх остальных — нет, без всякой
        // причины, кроме недосмотра. Теперь запоминаем версию и у них,
        // и отдаём ровно то, что взяли.
        ldata->PPixCreatorSuiteVersion =
            (ldata->BasicSuite->AcquireSuite(kPrSDKPPixCreatorSuite,
                                             kPrSDKPPixCreatorSuiteVersion,
                                             (const void**)&ldata->PPixCreatorSuite) == kSPNoError
             && ldata->PPixCreatorSuite) ? kPrSDKPPixCreatorSuiteVersion : 0;

        // CreateCustomPPix — единственный способ получить ПИСАЕМЫЙ буфер под
        // P010 на Premiere 26. Без этого набора десятибитный родной путь
        // предлагать нельзя: обычный CreatePPix формат принимает и молчит.
        ldata->PPixCreator2SuiteVersion =
            AcquireNewestSuite(ldata->BasicSuite, kPrSDKPPixCreator2Suite,
                               kPrSDKPPixCreator2SuiteVersion, &ldata->PPixCreator2Suite);

        ldata->PPixSuiteVersion =
            (ldata->BasicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                                             (const void**)&ldata->PPixSuite) == kSPNoError
             && ldata->PPixSuite) ? kPrSDKPPixSuiteVersion : 0;

        ldata->TimeSuiteVersion =
            (ldata->BasicSuite->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                                             (const void**)&ldata->TimeSuite) == kSPNoError
             && ldata->TimeSuite) ? kPrSDKTimeSuiteVersion : 0;

        if (!ldata->PPixCreatorSuiteVersion || !ldata->PPixSuiteVersion ||
            !ldata->TimeSuiteVersion) {
            av1imp::Log("suites: host refused a basic suite (creator %d, ppix %d, time %d)",
                        ldata->PPixCreatorSuiteVersion, ldata->PPixSuiteVersion,
                        ldata->TimeSuiteVersion);
        }
        av1imp::Log("suites: PPixCreator2 version %d%s", ldata->PPixCreator2SuiteVersion,
                    ldata->PPixCreator2SuiteVersion ? "" : " - 10-bit planes unavailable");

        // А вот кэш кадров дорос до восьмой версии, и просить восьмую у Premiere
        // 2019 бессмысленно: набор не выдадут вовсе. Нужны нам только две первые
        // функции набора, а они есть с самой первой версии.
        ldata->PPixCacheSuiteVersion =
            AcquireNewestSuite(ldata->BasicSuite, kPrSDKPPixCacheSuite,
                               kPrSDKPPixCacheSuiteVersion, &ldata->PPixCacheSuite);
        av1imp::Log("suites: frame cache version %d%s", ldata->PPixCacheSuiteVersion,
                    ldata->PPixCacheSuiteVersion ? "" : " - working without it");

        // Тем же перебором и по той же причине: адреса плоскостей появились
        // во второй версии набора, и на хосте, где есть только первая, родной
        // YUV предлагать нельзя — писать в него было бы некуда.
        ldata->PPix2SuiteVersion =
            AcquireNewestSuite(ldata->BasicSuite, kPrSDKPPix2Suite,
                               kPrSDKPPix2SuiteVersion, &ldata->PPix2Suite);
        av1imp::Log("suites: PPix2 version %d%s", ldata->PPix2SuiteVersion,
                    (ldata->PPix2SuiteVersion >= kPrSDKPPix2SuiteVersion2)
                        ? "" : " - planar YUV unavailable");
    }

    // Какой поток спрашивают. Поток 0 — видео плюс первая дорожка звука,
    // потоки 1..N — остальные дорожки. Отдавать их порознь обязательно:
    // раздельно их и пишут, чтобы сводить на монтаже, и слить их в одну
    // дорожку означало бы потерять саму возможность их разделить.
    const int audioTracks = mi.audioStreamCount;

    if (fileInfo->streamIdx > 0 && fileInfo->streamIdx >= audioTracks) {
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        return imBadStreamIndex;
    }

    // Поток 0 обычно несёт видео плюс первую дорожку звука — но у файла без
    // видеодорожки (звук в Matroska или WebM) видео нет и в нулевом потоке
    fileInfo->hasVideo    = (fileInfo->streamIdx == 0 && mi.hasVideo) ? kPrTrue : kPrFalse;
    fileInfo->hasAudio    = kPrFalse;
    fileInfo->accessModes = kRandomAccessImport;
    fileInfo->hasDataRate = kPrFalse;

    if (audioTracks > 0 && EnsureAudio(ldata)) {
        fileInfo->hasAudio            = kPrTrue;
        fileInfo->audInfo.numChannels = mi.audioChannels;
        // Запоминаем ровно то, что сказали: по этому числу хост выделит
        // буферы, и по нему же мы потом проверим, во что пишем
        ldata->declaredAudioChannels  = mi.audioChannels;
        fileInfo->audInfo.sampleRate  = static_cast<float>(mi.audioSampleRate);
        fileInfo->audInfo.sampleType  = kPrAudioSampleType_32BitFloat;
        fileInfo->audDuration         = mi.audioSampleCount;
    } else if (audioTracks > 0) {
        av1imp::Log("imGetInfo8: stream %d has audio, but it is not usable: %s",
                    fileInfo->streamIdx,
                    pin.decoder->LastAudioError().c_str());
    }

    if (!fileInfo->hasVideo) {
        av1imp::Log("imGetInfo8: stream %d is audio only, %d ch",
                    fileInfo->streamIdx, mi.audioChannels);
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        // Есть ли ещё дорожки после этой
        return (fileInfo->streamIdx + 1 < audioTracks) ? imIterateStreams : malNoError;
    }

    fileInfo->vidInfo.imageWidth     = mi.width;
    fileInfo->vidInfo.imageHeight    = mi.height;
    // Поле справочное («image buffers are 32bpp so this is informational only»),
    // но пусть говорит правду о самом файле
    fileInfo->vidInfo.depth          = (mi.bitDepth > 8) ? 64 : 32;
    // Ключевой момент. Сказать здесь 'av01' — значит отправить Premiere искать
    // декодер AV1, которого у него нет: он его не находит и отказывает файлу
    // словами "unsupported compression type av01", даже не спросив у нас кадр.
    // Но распаковываем-то мы, а наружу отдаём готовые пиксели, поэтому с точки
    // зрения Premiere источник несжатый.
    fileInfo->vidInfo.subType        = imUncompressed;
    fileInfo->vidInfo.fieldType      = prFieldsNone;
    // Прозрачность объявляем прямой, а не умноженной: ffmpeg отдаёт альфу
    // как есть, ни на что цвет не домножая
    fileInfo->vidInfo.alphaType      = mi.hasAlpha ? alphaStraight : alphaNone;
    fileInfo->vidInfo.pixelAspectNum = 1;
    fileInfo->vidInfo.pixelAspectDen = 1;
    fileInfo->vidInfo.isStill        = kPrFalse;
    fileInfo->vidInfo.noDuration     = imNoDurationFalse;
    fileInfo->vidInfo.hasPulldown    = kPrFalse;

    // Цветовое пространство мы хосту сообщаем — см. AV1GetIndColorSpace.
    // Без этого Premiere подставляет своё рабочее пространство, и материал
    // BT.2020 PQ приезжает вылинявшим, а лог-кривые плоскими.
    fileInfo->vidInfo.colorSpaceSupport = imColorSpaceSupport_Fixed;

    // Оба пути сразу: обычный остаётся, асинхронный хост использует, если
    // захочет. Заявляем асинхронный только когда он включён в настройках —
    // иначе Premiere спросит imCreateAsyncImporter, получит отказ и будет
    // спрашивать снова.
    fileInfo->vidInfo.supportsGetSourceVideo = kPrTrue;
    fileInfo->vidInfo.supportsAsyncIO        =
        av1imp::AsyncEnabled() ? kPrTrue : kPrFalse;

    // Частоту передаём дробью — иначе 59.94 превратится в 60 и картинка уедет
    fileInfo->vidScale      = mi.fpsNum > 0 ? mi.fpsNum : 30;
    fileInfo->vidSampleSize = mi.fpsDen > 0 ? mi.fpsDen : 1;
    // Длительность в единицах видеовремени: считается в 64 битах и насыщается,
    // а не переполняется.
    //
    // Раньше оба множителя были 32-битными, и умножение шло в 32 битах: при
    // 59.94 кадра в секунду vidSampleSize равен 1001, значит потолок — 2 145 000
    // кадров, то есть 9 часов 56 минут. Дальше длительность уходила в минус.
    //
    // ⚠ На нынешних Premiere это поле, скорее всего, не читают вовсе: в SDK про
    // vidDurationInFrames сказано «vidDuration will be ignored if this is set»,
    // а его мы заполняем 64-битным числом строкой ниже. Но «скорее всего, не
    // читают» — не повод отдавать отрицательное число тому, кто всё-таки прочтёт.
    bool durationSaturated = false;
    fileInfo->vidDuration = av1imp::SaturatingFrameDuration(
        mi.frameCount, fileInfo->vidSampleSize, &durationSaturated);
    fileInfo->vidDurationInFrames = mi.frameCount;
    if (durationSaturated) {
        av1imp::Log("imGetInfo8: vidDuration exceeds 32-bit field, clamped; "
                    "%lld frames * sample size %d",
                    (long long)mi.frameCount, fileInfo->vidSampleSize);
    }

    ldata->importerID = fileInfo->vidInfo.importerID;

    // Длительность кадра в тактах Premiere — по ней переводим время в номер кадра
    if (ldata->TimeSuite && mi.fpsNum > 0) {
        PrTime ticksPerSecond = 0;
        ldata->TimeSuite->GetTicksPerSecond(&ticksPerSecond);
        int64_t ticks = 0;
        if (!av1imp::TicksPerFrame(ticksPerSecond, mi.fpsNum, mi.fpsDen, &ticks)) {
            av1imp::Log("imGetInfo8: cannot compute ticksPerFrame from %lld * %d / %d",
                        (long long)ticksPerSecond, mi.fpsDen, mi.fpsNum);
            ldata->ticksPerFrame = 0;
        } else {
            ldata->ticksPerFrame = ticks;
        }
    }

    av1imp::Log("imGetInfo8: %s %dx%d %d-bit, %d/%d fps, %lld frames, subType RAW",
                mi.codecName.c_str(),
                mi.width, mi.height, mi.bitDepth,
                fileInfo->vidScale, fileInfo->vidSampleSize,
                (long long)mi.frameCount);

    // Поворот сообщаем в журнал, но кадр отдаём как есть — объявить его хосту
    // нечем, в SDK импортёра такого поля нет. Строка нужна, чтобы «ролик
    // приехал боком» перестало быть загадкой: причина видна сразу.
    if (mi.rotationDegrees != 0) {
        av1imp::Log("imGetInfo8: the file asks to be shown rotated %d degrees, and "
                    "the importer SDK has no field to pass that on - delivering "
                    "unrotated", mi.rotationDegrees);
    }

    stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

    // imIterateStreams — «спроси меня про следующий поток». Без этого Premiere
    // остановится на первой дорожке и остальные три просто не увидит.
    return (audioTracks > 1) ? imIterateStreams : malNoError;
}

// ---------------------------------------------------------------------------
// Звук
// ---------------------------------------------------------------------------

static prMALError AV1ImportAudio7(imStdParms* stdParms, imFileRef fileRef,
                                  imImportAudioRec7* audioRec)
{
    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec->privateData);
    if (!ldataH) return imOtherErr;

    ImporterLocalRecPtr ldata = *ldataH;
    const csSDK_int32 nth = ldata->audioRequests++;

    DecoderPin pin;
    if (!PinDecoder(ldata, &pin) || !EnsureAudio(ldata)) {
        av1imp::Log("imImportAudio7: track %d request %d at %lld - audio unavailable: %s",
                    ldata->audioTrack, nth, (long long)audioRec->position,
                    SafeDecoderAudioError(ldata).c_str());
        return imFileReadFailed;
    }

    // Число каналов передаём то, которое сами и объявили: только оно
    // описывает буферы, что выделил хост
    if (!pin.decoder->GetAudio(audioRec->position,
                                  static_cast<int32_t>(audioRec->size),
                                  audioRec->buffer,
                                  ldata->declaredAudioChannels)) {
        av1imp::Log("imImportAudio7: track %d request %d at %lld, %u samples - FAILED: %s",
                    ldata->audioTrack, nth, (long long)audioRec->position,
                    audioRec->size, pin.decoder->LastAudioError().c_str());
        return imFileReadFailed;
    }

    // Начало подробно, дальше редко: по этим строкам видно, докуда дошло
    // конформирование, но они не заслоняют собой всё остальное.
    if (nth < 3 || (nth % 200) == 0) {
        av1imp::Log("audio: track %d, request %d, from %lld, %u samples",
                    ldata->audioTrack, nth, (long long)audioRec->position, audioRec->size);
    }
    return malNoError;
}

// ---------------------------------------------------------------------------
// В каком виде отдаём пиксели и какого размера
// ---------------------------------------------------------------------------

// Какой константой Premiere называет то, что уже лежит у нас в кадре.
//
// Смысл в том, что при выдаче плоскостями мы цвет не переводим, а называем:
// матрица и размах перестают быть нашей работой и становятся частью имени
// формата. Ошибиться тут — то же самое, что раньше было бы ошибкой в матрице
// swscale, только последствие ровно одно и то же: цвета уедут.
//
// MPEG2 против MPEG4 в названии — это не про кодек, а про то, где стоит
// цветность: слева (почти везде) или по центру (MPEG-1, JPEG).
static bool NativeYUVFormat(const av1imp::MediaInfo& mi, PrPixelFormat* out)
{
    // Коды здесь свои, а не из заголовков ffmpeg: слой SDK намеренно про них
    // не знает, а числа эти — не выдумка ffmpeg, а те же коды ITU, какими
    // описание цвета записано в самом файле.
    const int kMatrixBT709      = 1;
    const int kChromaCentre     = 2;   // 1 — слева, 2 — по центру

    const bool bt709   = (mi.colourMatrix == kMatrixBT709);
    const bool centred = (mi.chromaLocation == kChromaCentre);

    if (centred) {
        if (bt709) {
            *out = mi.fullRange ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709_FullRange
                                : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709;
        } else {
            *out = mi.fullRange ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_601_FullRange
                                : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_601;
        }
    } else {
        if (bt709) {
            *out = mi.fullRange ? PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709_FullRange
                                : PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709;
        } else {
            *out = mi.fullRange ? PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_601_FullRange
                                : PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_601;
        }
    }
    return true;
}

// То же для десяти бит. Здесь у Premiere различается ещё и кривая переноса:
// у BT.2020 отдельные константы для PQ и для HLG, и это не прихоть — по ним
// хост понимает, что материал HDR, и не тащит его через тон-маппинг как SDR.
//
// Коды кривой те же, что в файле: 16 — PQ (SMPTE 2084), 18 — HLG.
static bool NativeP010Format(const av1imp::MediaInfo& mi, PrPixelFormat* out)
{
    const int kMatrixBT709 = 1;
    const int kTransferPQ  = 16;
    const int kTransferHLG = 18;

    const bool full = mi.fullRange;

    if (mi.colourMatrix == kMatrixBT709) {
        *out = full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709;
        return true;
    }

    // Дальше только BT.2020 — CanDeliverP010 других сюда не пускает
    if (mi.colourTransfer == kTransferPQ) {
        *out = full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR;
    } else if (mi.colourTransfer == kTransferHLG) {
        *out = full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG;
    } else {
        *out = full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020;
    }
    return true;
}

// Наш ли это родной формат. Восемь констант — все, какие Premiere знает для
// восьмибитного YUV 4:2:0 кадром: две расстановки цветности × две матрицы ×
// два размаха. Перечислены поимённо, а не диапазоном: числовые значения
// в заголовке идут не подряд, и «от и до» сломалось бы на первом же обновлении
// SDK, причём молча.
bool av1imp::IsNativeYUV(PrPixelFormat f)
{
    switch (f) {
        case PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_601:
        case PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_601_FullRange:
        case PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709:
        case PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709_FullRange:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_601:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_601_FullRange:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709_FullRange:
            return true;
        default:
            return false;
    }
}

// Всё, во что мы умеем положить кадр: плоскости YUV, шестнадцать бит BGRA
// и восемь бит BGRA. Больше ничего, и это честно перечислено, а не выведено
// из «ну BGRA-то мы точно можем».
bool av1imp::IsNativeP010(PrPixelFormat f)
{
    switch (f) {
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709_FullRange:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_FullRange:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_FullRange:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG:
        case PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG_FullRange:
            return true;
        default:
            return false;
    }
}

bool av1imp::CanProduce(PrPixelFormat f)
{
    return av1imp::IsNativeYUV(f) ||
           av1imp::IsNativeP010(f) ||
           f == PrPixelFormat_BGRA_4444_16u ||
           f == PrPixelFormat_BGRA_4444_8u;
}

int av1imp::PickFrameFormat(const imFrameFormat* formats, int count, int startFrom)
{
    if (!formats || count < 1 || startFrom < 0) return -1;
    for (int i = startFrom; i < count; ++i) {
        const PrPixelFormat f = formats[i].inPixelFormat;
        if (av1imp::IsNativeP010(f) && av1imp::IsP010Unavailable()) continue;
        if (av1imp::CanProduce(f)) return i;
    }
    return -1;
}

bool av1imp::WriteFrameToBuffer(av1imp::Decoder& decoder, int64_t frameIndex,
                                PrSDKPPixSuite* ppixSuite, PrSDKPPix2Suite* ppix2Suite,
                                PPixHand frame, PrPixelFormat pixelFormat,
                                int width, int height, const char** outWhy)
{
    const char* dummy = nullptr;
    if (!outWhy) outWhy = &dummy;
    *outWhy = nullptr;

    if (av1imp::IsNativeYUV(pixelFormat)) {
        if (!ppix2Suite) { *outWhy = "planar asked for without the PPix2 suite"; return false; }

        char* planeY = nullptr; char* planeU = nullptr; char* planeV = nullptr;
        csSDK_uint32 rowY = 0, rowU = 0, rowV = 0;
        const prSuiteError err = ppix2Suite->GetYUV420PlanarBuffers(
            frame, PrPPixBufferAccess_ReadWrite,
            &planeY, &rowY, &planeU, &rowU, &planeV, &rowV);

        if (err != suiteError_NoError || !planeY || !planeU || !planeV) {
            *outWhy = "GetYUV420PlanarBuffers refused";
            return false;
        }

        // Шаги приходят беззнаковыми, но означать могут и движение вверх —
        // так задокументировано. Приводим к знаковому, иначе отрицательный
        // шаг стал бы четырьмя миллиардами и запись ушла бы мимо буфера.
        return decoder.GetFrameYUV420(frameIndex,
                                      reinterpret_cast<uint8_t*>(planeY), (int)rowY,
                                      reinterpret_cast<uint8_t*>(planeU), (int)rowU,
                                      reinterpret_cast<uint8_t*>(planeV), (int)rowV,
                                      width, height);
    }

    char* buffer = nullptr;
    csSDK_int32 rowBytes = 0;
    // Для P010 сначала WriteOnly — так пишут сэмплы Adobe и OpenEXR.
    // ReadWrite оставляем запасным и для обычных форматов.
    if (av1imp::IsNativeP010(pixelFormat)) {
        if (ppixSuite->GetPixels(frame, PrPPixBufferAccess_WriteOnly, &buffer) != suiteError_NoError
            || !buffer) {
            buffer = nullptr;
            ppixSuite->GetPixels(frame, PrPPixBufferAccess_ReadWrite, &buffer);
        }
    } else {
        ppixSuite->GetPixels(frame, PrPPixBufferAccess_ReadWrite, &buffer);
    }
    ppixSuite->GetRowBytes(frame, &rowBytes);

    // --- десять бит, две плоскости -----------------------------------------
    //
    // Premiere 26: CreatePPix принимает P010, GetPixels отдаёт указатель,
    // а GetRowBytes врёт нулём. Шаг тогда считаем сами (width*2), сверяя
    // с GetSize. GetYUV420PlanarBuffers для двух плоскостей не существует.
    if (av1imp::IsNativeP010(pixelFormat)) {
        if (!buffer) {
            size_t probe = 0;
            if (ppix2Suite && ppix2Suite->GetSize) ppix2Suite->GetSize(frame, &probe);
            av1imp::Log("10-bit planes: host gave buffer=%p rowBytes=%d size=%zu "
                        "for %dx%d - no pixel pointer",
                        (void*)buffer, (int)rowBytes, probe, width, height);
            *outWhy = "host gives no buffer for a two-plane frame";
            return false;
        }

        const csSDK_int32 stride = ResolveP010Stride(ppixSuite, ppix2Suite, frame,
                                                     width, height, rowBytes);
        if (stride <= 0) {
            *outWhy = "cannot resolve P010 row stride";
            return false;
        }

        if (!ppix2Suite || !ppix2Suite->GetSize) {
            *outWhy = "10-bit planes asked for without GetSize to confirm the layout";
            return false;
        }

        const size_t strideY  = (size_t)stride;
        const size_t chromaH  = ((size_t)height + 1) / 2;
        const size_t needed   = strideY * (size_t)height + strideY * chromaH;

        size_t actual = 0;
        if (ppix2Suite->GetSize(frame, &actual) != suiteError_NoError || actual == 0) {
            *outWhy = "GetSize refused, cannot confirm the two-plane layout";
            return false;
        }
        if (actual < needed) {
            av1imp::Log("10-bit planes: buffer is %zu bytes, the two-plane layout needs "
                        "%zu (%dx%d, row %d) - refusing and falling back",
                        actual, needed, width, height, (int)stride);
            *outWhy = "buffer too small for two planes";
            return false;
        }

        uint8_t* planeY  = reinterpret_cast<uint8_t*>(buffer);
        uint8_t* planeUV = planeY + strideY * (size_t)height;

        return decoder.GetFrameP010(frameIndex,
                                    planeY,  (int)strideY,
                                    planeUV, (int)strideY,
                                    width, height);
    }

    if (!buffer || rowBytes <= 0) { *outWhy = "host gave no buffer"; return false; }

    // Строка обязана вмещать кадр целиком. Буфер создаём мы сами и тем же
    // прямоугольником, так что это про случай «хост отдал не то, что просили»:
    // цена проверки — одно умножение, цена ошибки — запись за пределы буфера
    // внутри чужого процесса.
    const int bytesPerPixel = (pixelFormat == PrPixelFormat_BGRA_4444_16u) ? 8 : 4;
    if (width <= 0 || height <= 0 || rowBytes <= 0 ||
        static_cast<int64_t>(rowBytes) < static_cast<int64_t>(width) * bytesPerPixel) {
        *outWhy = "row too small for the frame";
        return false;
    }

    // У Premiere 32-битные буферы идут снизу вверх: пишем с последней строки
    // отрицательным шагом, и переворот делает сам масштабатор ffmpeg
    uint8_t* lastRow = reinterpret_cast<uint8_t*>(buffer)
                     + static_cast<size_t>(height - 1) * rowBytes;

    const av1imp::FrameFormat frameFormat =
        (pixelFormat == PrPixelFormat_BGRA_4444_16u) ? av1imp::FrameFormat::BGRA16
                                                     : av1imp::FrameFormat::BGRA8;
    return decoder.GetFrameBGRA(frameIndex, lastRow, -rowBytes, width, height, frameFormat);
}

// Собираемся ли мы отдавать этот файл плоскостями.
//
// Спрашивают в двух местах — при перечислении форматов и при объявлении цвета,
// — и ответ обязан быть один и тот же. Разойдись они, вышло бы худшее из
// возможного: отдаём YUV, а хосту сказали, что это RGB, и он не станет
// переводить. Картинка при этом не пропадёт, а позеленеет.
static bool PrefersNativeYUV(ImporterLocalRecPtr ldata)
{
    if (!ldata || !ldata->decoder || !ldata->decoder->IsOpen()) return false;

    // Без второй версии набора PPix2 адреса плоскостей узнать нечем
    if (!ldata->PPix2Suite || ldata->PPix2SuiteVersion < kPrSDKPPix2SuiteVersion2) {
        return false;
    }
    return av1imp::YuvEnabled() && ldata->decoder->CanDeliverYUV420();
}

// То же для десяти бит. Условие на версию набора здесь ДРУГОЕ, и это не
// недосмотр: трёхплоскостному пути нужен GetYUV420PlanarBuffers, а он появился
// во второй версии; двухплоскостному нужен только GetSize, а он есть с первой.
static bool PrefersNativeP010(ImporterLocalRecPtr ldata)
{
    if (!ldata || !ldata->decoder || !ldata->decoder->IsOpen()) return false;
    if (!ldata->PPix2Suite) return false;
    // Без Creator2 нет CreateCustomPPix — а обычный CreatePPix на Premiere 26
    // под P010 буфер не отдаёт. Предлагать формат в таком случае нельзя.
    if (!ldata->PPixCreator2Suite || !ldata->PPixCreator2Suite->CreateCustomPPix) return false;
    if (av1imp::IsP010Unavailable()) return false;

    return av1imp::YuvEnabled() && av1imp::Yuv10Enabled() &&
           ldata->decoder->CanDeliverP010();
}

// Premiere опрашивает форматы по одному, пока не получит imBadFormatIndex,
// и порядок здесь означает предпочтение.
//
// Первым, где можно, идёт родной YUV: кадр уходит хосту как есть, а перевод
// в RGB делает он сам — по нашему замеру этот перевод стоил три четверти
// всего времени (3.02 мс из 4 на 1440p). Хост волен его не взять; тогда он
// спросит следующий формат, и всё пойдёт прежним путём. Поэтому BGRA из
// списка никуда не девается — он запасной, а не бывший.
//
// Для 10-битного файла порядок прежний: шестнадцать бит, потом восемь.
static prMALError AV1GetIndPixelFormat(imStdParms* stdParms, csSDK_size_t idx,
                                       imIndPixelFormatRec* rec)
{
    bool deep = false;
    bool native = false;
    PrPixelFormat nativeFormat = PrPixelFormat_BGRA_4444_8u;

    if (rec->privatedata) {
        ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(rec->privatedata);
        DecoderPin pin;
        if (*ldataH && PinDecoder(*ldataH, &pin)) {
            const av1imp::MediaInfo mi = pin.decoder->Info();
            deep = mi.bitDepth > 8;

            // Десятибитный родной путь проверяем первым: он и есть тот случай,
            // где выигрыш наибольший (15.95 мс против 0.42 на 1440p).
            if (PrefersNativeP010(*ldataH)) {
                native = NativeP010Format(mi, &nativeFormat);
            } else if (PrefersNativeYUV(*ldataH)) {
                native = NativeYUVFormat(mi, &nativeFormat);
            }
        }
    }

    // Список строится по порядку предпочтения, а потом из него берётся idx-й.
    // Так понятнее, чем лесенка из if по номеру: видно сам порядок.
    PrPixelFormat order[3];
    csSDK_size_t count = 0;
    if (native) order[count++] = nativeFormat;
    if (deep)   order[count++] = PrPixelFormat_BGRA_4444_16u;
    order[count++] = PrPixelFormat_BGRA_4444_8u;

    if (idx >= count) return imBadFormatIndex;
    rec->outPixelFormat = order[idx];
    return malNoError;
}

static prMALError AV1PreferredFrameSize(imStdParms* stdParms,
                                        imPreferredFrameSizeRec* rec)
{
    if (rec->inIndex != 0) return imOtherErr;

    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(rec->inPrivateData);
    DecoderPin pin;
    if (!ldataH || !PinDecoder(*ldataH, &pin)) return imOtherErr;

    const av1imp::MediaInfo mi = pin.decoder->Info();
    rec->outWidth  = mi.width;
    rec->outHeight = mi.height;
    return malNoError;
}

// ---------------------------------------------------------------------------
// imGetIndColorSpace — какого цвета то, что мы отдаём
// ---------------------------------------------------------------------------
//
// Тон-маппинг сюда не входит и входить не должен. Свести HDR к SDR — решение
// творческое, и принимать его за монтажёра импортёру не по чину: Premiere умеет
// это сам и делает по настройкам секвенции. Наше дело — честно сказать, что
// именно лежит в отданных пикселях.
//
// А лежит там вот что: RGB полного размаха, с ИСХОДНОЙ кривой переноса и
// исходными первичными цветами. Мы применяем только матрицу яркость-цветность,
// больше ничего не трогаем. Поэтому кривую и первичные цвета передаём как есть,
// а матрицу указываем единичной — после пересчёта в RGB её уже нет.
//
// Коды те же, что в потоке: SEI из H.265, они же у AV1 и VP9, они же в ffmpeg.
static prMALError AV1GetIndColorSpace(imStdParms* stdParms, csSDK_size_t index,
                                      imIndColorSpaceRec* rec)
{
    if (!rec) return imOtherErr;

    // Хост перебирает пространства, пока не получит отказ. У нас оно одно.
    if (index > 0) return imBadFormatIndex;

    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(rec->inPrivateData);
    if (!ldataH) return imOtherErr;
    ImporterLocalRecPtr ldata = *ldataH;

    DecoderPin pin;
    if (!PinDecoder(ldata, &pin)) return imOtherErr;
    const av1imp::MediaInfo mi = pin.decoder->Info();
    if (!mi.hasVideo) return imOtherErr;

    rec->outColorSpaceType = kPrSDKColorSpaceType_SEITags;

    prSEIColorCodesRec& sei = rec->outSEICodesRec;
    sei.colorPrimariesCode        = mi.colourPrimaries;
    sei.transferCharacteristicCode = mi.colourTransfer;

    // ITU «unspecified» = 2. Раньше сюда подставляли BT.709, и хост
    // получал чужую кривую. Честнее отдать «не указано».
    if (mi.colourTransfer == 2) {
        av1imp::Log("imGetIndColorSpace: transfer is unspecified in the file - not guessing BT.709");
    }

    // Описание обязано совпадать с тем, что мы на самом деле кладём в буфер.
    // Отдавая плоскости, мы не переводим цвет — значит матрица никуда не делась
    // и размах остался тем, что в файле. Отдавая RGB, наоборот: матрица уже
    // применена, и объявлять её второй раз значит применить дважды.
    // Оба родных пути — трёхплоскостной восьмибитный и двухплоскостной
    // десятибитный — одинаковы в главном: цвет мы НЕ переводим. Значит
    // и объявляются они одинаково.
    const bool planar = PrefersNativeYUV(ldata) || PrefersNativeP010(ldata);
    if (planar) {
        sei.matrixEquationsCode = mi.colourMatrix;
        sei.isFullRange         = mi.fullRange ? kPrTrue : kPrFalse;
        sei.isRGB               = kPrFalse;
    } else {
        sei.matrixEquationsCode = static_cast<csSDK_int32>(PrMatrixEquations::kIdentity);
        sei.isFullRange         = kPrTrue;    // RGB у нас всегда полного размаха
        sei.isRGB               = kPrTrue;
    }

    sei.bitDepth        = (mi.bitDepth > 8) ? 16 : 8;
    sei.isSceneReferred = kPrFalse;   // PQ, HLG и BT.709 — все про показ

    av1imp::Log("imGetIndColorSpace: primaries %d, transfer %d, matrix %d, %d-bit %s%s",
                sei.colorPrimariesCode, sei.transferCharacteristicCode,
                sei.matrixEquationsCode, sei.bitDepth,
                planar ? "YUV" : "RGB",
                (planar && !mi.fullRange) ? " (limited range)" : "");
    return malNoError;
}

// ---------------------------------------------------------------------------
// imGetSourceVideo — собственно кадр
// ---------------------------------------------------------------------------

static prMALError AV1GetSourceVideo(imStdParms* stdParms, imFileRef fileRef,
                                    imSourceVideoRec* videoRec)
{
    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(videoRec->inPrivateData);
    if (!ldataH) return imOtherErr;

    ImporterLocalRecPtr ldata = *ldataH;
    if (!ldata->PPixSuite || !ldata->PPixCreatorSuite) {
        return imOtherErr;  // imGetInfo8 не отработал: наборов функций нет
    }
    DecoderPin pin;
    if (!PinDecoder(ldata, &pin)) return imBadFile;

    const av1imp::MediaInfo mi = pin.decoder->Info();

    if (ldata->ticksPerFrame <= 0) return imOtherErr;
    const csSDK_int32 frameIndex =
        static_cast<csSDK_int32>(videoRec->inFrameTime / ldata->ticksPerFrame);

    // Сколько форматов прислали, столько и читаем. Обращение к нулевому без
    // этой проверки — чтение чужой памяти, если хост однажды пришлёт пустой
    // список. Сейчас так не делает ни один, но полагаться на это незачем.
    if (!videoRec->inFrameFormats || videoRec->inNumFrameFormats < 1) {
        av1imp::Log("imGetSourceVideo: host asked without a frame format");
        return imOtherErr;
    }

    // Берём из СПИСКА первый формат, который умеем, а не всегда нулевой.
    // Список хост присылает в порядке предпочтения — см. PickFrameFormat.
    // При отказе P010 (нет буфера) пробуем следующий формат в том же запросе.
    int pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                       videoRec->inNumFrameFormats);
    if (pick < 0 && videoRec->inNumFrameFormats > 0) {
        av1imp::Log("imGetSourceVideo: host offered %d format(s), none of them ours "
                    "(first is 0x%08X) - falling back to BGRA 8u",
                    videoRec->inNumFrameFormats,
                    (unsigned)videoRec->inFrameFormats[0].inPixelFormat);
    }

    {
        imFrameFormat* offered = &videoRec->inFrameFormats[pick >= 0 ? pick : 0];
        const int askedW = (offered->inFrameWidth  > 0) ? offered->inFrameWidth  : mi.width;
        const int askedH = (offered->inFrameHeight > 0) ? offered->inFrameHeight : mi.height;
        const PrPixelFormat askedFmt =
            (pick >= 0) ? offered->inPixelFormat : PrPixelFormat_BGRA_4444_8u;
        av1imp::NoteHostVideoRequest(frameIndex, mi.width, mi.height,
                                     askedW, askedH, askedFmt, videoRec->inQuality);
    }

    // Если кадр уже разбирали — Premiere вернёт его из своего кэша,
    // и на перемотке туда-сюда мы не декодируем одно и то же дважды
    // Кэш кадров самого Premiere — не обязательное условие: без него мы просто
    // распаковываем заново, опираясь на собственный кэш в декодере
    prMALError result = imOtherErr;
    if (ldata->PPixCacheSuite) {
        result = ldata->PPixCacheSuite->GetFrameFromCache(
            ldata->importerID, 0, frameIndex, 1,
            videoRec->inFrameFormats, videoRec->outFrame, nullptr, 0);

        if (result == suiteError_NoError) return result;
    }

    // Качество, которое просит хост: при черновом кадр уменьшается дешевле.
    pin.decoder->SetScaling(av1imp::ScalingFor(videoRec->inQuality));

    for (;;) {
        imFrameFormat* offered = &videoRec->inFrameFormats[pick >= 0 ? pick : 0];
        const int frameW = (offered->inFrameWidth  > 0) ? offered->inFrameWidth  : mi.width;
        const int frameH = (offered->inFrameHeight > 0) ? offered->inFrameHeight : mi.height;
        if (!av1imp::UsableFrameSize(frameW, frameH)) {
            av1imp::Log("imGetSourceVideo: host asked for unusable frame size %dx%d",
                        frameW, frameH);
            return imOtherErr;
        }

        const PrPixelFormat pixelFormat =
            (pick >= 0) ? offered->inPixelFormat : PrPixelFormat_BGRA_4444_8u;
        const bool wantsNative = av1imp::IsNativeYUV(pixelFormat) ||
                                 av1imp::IsNativeP010(pixelFormat);
        const av1imp::FrameFormat frameFormat =
            (pixelFormat == PrPixelFormat_BGRA_4444_16u) ? av1imp::FrameFormat::BGRA16
                                                         : av1imp::FrameFormat::BGRA8;

        result = av1imp::CreateVideoPPix(ldata->PPixCreatorSuite,
                                         ldata->PPixCreator2Suite,
                                         ldata->PPixSuite,
                                         ldata->PPix2Suite,
                                         videoRec->outFrame,
                                         pixelFormat, frameW, frameH);
        if (result != malNoError) {
            if (av1imp::IsNativeP010(pixelFormat)) {
                av1imp::MarkP010Unavailable();
                av1imp::DiscardHostFrame(ldata->PPixSuite, videoRec->outFrame);
                pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                               videoRec->inNumFrameFormats,
                                               pick + 1);
                if (pick >= 0) continue;
            }
            return result;
        }

        const char* why = nullptr;
        if (!av1imp::WriteFrameToBuffer(*pin.decoder, frameIndex,
                                        ldata->PPixSuite, ldata->PPix2Suite,
                                        *videoRec->outFrame, pixelFormat,
                                        frameW, frameH, &why)) {
            const bool p010NoBuf = av1imp::IsNativeP010(pixelFormat) && why &&
                (strstr(why, "no buffer") != nullptr ||
                 strstr(why, "buffer too small") != nullptr);
            av1imp::DiscardHostFrame(ldata->PPixSuite, videoRec->outFrame);
            if (p010NoBuf) {
                av1imp::MarkP010Unavailable();
                pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                               videoRec->inNumFrameFormats,
                                               pick + 1);
                if (pick >= 0) continue;
            }
            av1imp::Log("frame %d: FAILED - %s", frameIndex,
                        why ? why : pin.decoder->LastError().c_str());
            return why ? imOtherErr : imFileReadFailed;
        }
        if (frameIndex < 3) {
            av1imp::Log("frame %d delivered (%dx%d, %s)", frameIndex,
                        frameW, frameH,
                        av1imp::IsNativeP010(pixelFormat) ? "10u planes (P010)"
                        : wantsNative ? "planar YUV"
                        : (frameFormat == av1imp::FrameFormat::BGRA16 ? "BGRA 16u"
                                                                      : "BGRA 8u"));
        }

        if (ldata->PPixCacheSuite) {
            ldata->PPixCacheSuite->AddFrameToCache(ldata->importerID, 0, *videoRec->outFrame,
                                                   frameIndex, nullptr, 0);
        }
        return malNoError;
    }
}

// ---------------------------------------------------------------------------
// Точка входа
// ---------------------------------------------------------------------------

static prMALError DispatchDisabled(csSDK_int32 selector, imStdParms* stdParms,
                                   void* param1, void* param2, const char* why)
{
    switch (selector) {
        case imInit: {
            imImportInfoRec* importInfo = reinterpret_cast<imImportInfoRec*>(param1);
            if (!importInfo) return imOtherErr;
            importInfo->canSave       = kPrFalse;
            importInfo->canDelete     = kPrFalse;
            importInfo->canTrim       = kPrFalse;
            importInfo->canCalcSizes  = kPrFalse;
            importInfo->hasSetup      = kPrFalse;
            importInfo->setupOnDblClk = kPrFalse;
            importInfo->dontCache     = kPrFalse;
            importInfo->keepLoaded    = kPrFalse;
            importInfo->priority      = 0;
            av1imp::Log("imInit: %s, priority 0, FFmpeg not loaded",
                        why ? why : "importer unavailable");
            return imIsCacheable;
        }
        case imGetIndFormat:
            return imBadFormatIndex;
        case imOpenFile8:
        case imGetInfo8:
        case imGetSourceVideo:
        case imImportAudio7:
            av1imp::Log("%s: %s, handing the file back",
                        av1imp::SelectorName(selector),
                        why ? why : "importer unavailable");
            return imBadFile;
        case imQuietFile:
            return AV1QuietFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
        case imCloseFile:
            return AV1CloseFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
        case imGetSupports7:
            return malSupports7;
        case imGetSupports8:
            return malSupports8;
        case imShutdown:
            FinishImporterShutdown();
            return malNoError;
        default:
            return imUnsupported;
    }
}

static prMALError DispatchImport(csSDK_int32 selector, imStdParms* stdParms,
                                 void* param1, void* param2)
{
    // Журнал открываем до ffmpeg: выключенный импортёр должен уметь отказаться
    // от файла, не загружая библиотеки в процесс Adobe.
    av1imp::EnsureLog();
    if (!av1imp::PluginEnabled()) {
        const prMALError result = DispatchDisabled(selector, stdParms, param1, param2,
                                                   "disabled by settings");
        if (selector != imGetSourceVideo && selector != imImportAudio7) {
            av1imp::Log("  selector %s -> %d (disabled)",
                        av1imp::SelectorName(selector), result);
        }
        return result;
    }

    if (!av1imp::EnsureRuntime()) {
        // Delay-load исключение на первом вызове ffmpeg завершило бы весь
        // процесс Adobe. Отказываем ещё до dispatch: битая установка должна
        // выглядеть как неработающий импортёр, а не как закрывшийся Premiere.
        const prMALError result = DispatchDisabled(selector, stdParms, param1, param2,
                                                   "ffmpeg runtime unavailable");
        av1imp::Log("  selector %s -> %d (ffmpeg runtime unavailable)",
                    av1imp::SelectorName(selector), result);
        return result;
    }

    prMALError result = imUnsupported;

    switch (selector)
    {
        case imInit:
            result = AV1Init(stdParms, reinterpret_cast<imImportInfoRec*>(param1));
            break;

        case imGetIndFormat:
            result = AV1GetIndFormat(stdParms, reinterpret_cast<csSDK_size_t>(param1),
                                     reinterpret_cast<imIndFormatRec*>(param2));
            break;

        case imOpenFile8:
            result = AV1OpenFile8(stdParms, reinterpret_cast<imFileRef*>(param1),
                                  reinterpret_cast<imFileOpenRec8*>(param2));
            break;

        case imQuietFile:
            result = AV1QuietFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
            break;

        case imCloseFile:
            result = AV1CloseFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
            break;

        case imGetInfo8:
            result = AV1GetInfo8(stdParms, reinterpret_cast<imFileAccessRec8*>(param1),
                                 reinterpret_cast<imFileInfoRec8*>(param2));
            break;

        case imGetIndPixelFormat:
            result = AV1GetIndPixelFormat(stdParms, reinterpret_cast<csSDK_size_t>(param1),
                                          reinterpret_cast<imIndPixelFormatRec*>(param2));
            break;

        case imGetPreferredFrameSize:
            result = AV1PreferredFrameSize(stdParms,
                        reinterpret_cast<imPreferredFrameSizeRec*>(param1));
            break;

        case imGetIndColorSpace:
            result = AV1GetIndColorSpace(stdParms,
                                         reinterpret_cast<csSDK_size_t>(param1),
                                         reinterpret_cast<imIndColorSpaceRec*>(param2));
            break;

        case imCreateAsyncImporter: {
            imAsyncImporterCreationRec* rec =
                reinterpret_cast<imAsyncImporterCreationRec*>(param1);
            if (!rec) { result = imOtherErr; break; }

            ImporterLocalRecH ldataH =
                reinterpret_cast<ImporterLocalRecH>(rec->inPrivateData);
            if (!ldataH) { result = imOtherErr; break; }

            DecoderPin pin;
            if (!PinDecoder(*ldataH, &pin)) { result = imOtherErr; break; }

            // Отказ здесь не беда: хост просто останется на обычном пути
            result = av1imp::CreateAsyncImporter(*ldataH, rec)
                     ? malNoError : imOtherErr;
            break;
        }

        case imGetSourceVideo:
            result = AV1GetSourceVideo(stdParms, reinterpret_cast<imFileRef>(param1),
                                       reinterpret_cast<imSourceVideoRec*>(param2));
            break;

        case imImportAudio7:
            result = AV1ImportAudio7(stdParms, reinterpret_cast<imFileRef>(param1),
                                     reinterpret_cast<imImportAudioRec7*>(param2));
            break;

        // Рукопожатие: плагин объявляет, какой версией интерфейса владеет.
        // Без ответа Premiere считает импортёр устаревшим и не спрашивает у него
        // ни формат пикселей, ни кадры — просто прочитает сведения о файле и уйдёт.
        case imGetSupports7:
            result = malSupports7;
            break;

        case imGetSupports8:
            result = malSupports8;
            break;

        case imShutdown:
            FinishImporterShutdown();
            result = malNoError;
            break;

        default:
            result = imUnsupported;
            break;
    }

    // Кадровые и звуковые запросы идут тысячами; отказ на них всё равно
    // пишем. Остальное — включая отвергнутое: отказ на нужном запросе
    // выглядит для Premiere так же, как отсутствие плагина.
    const bool hot = selector == imGetSourceVideo
                  || selector == imImportAudio7
                  || selector == 65   // imSelectClipFrameDescriptor
                  || selector == 83;  // imSelectClipFrameDescriptor2
    if (!hot || result != malNoError) {
        av1imp::Log("  selector %s -> %d", av1imp::SelectorName(selector), result);
    }
    return result;
}

PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms,
                                     void* param1, void* param2)
{
    // Исключение через C ABI Adobe = terminate всего Premiere.
    try {
        return DispatchImport(selector, stdParms, param1, param2);
    } catch (const std::exception& e) {
        av1imp::Log("xImportEntry %s: exception: %s",
                    av1imp::SelectorName(selector), e.what());
        if (selector == imShutdown) {
            FinishImporterShutdown();
            return malNoError;
        }
        return imOtherErr;
    } catch (...) {
        av1imp::Log("xImportEntry %s: unknown exception",
                    av1imp::SelectorName(selector));
        if (selector == imShutdown) {
            FinishImporterShutdown();
            return malNoError;
        }
        return imOtherErr;
    }
}
