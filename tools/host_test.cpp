// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Поддельный хост: гоняет собранный .prm так, как это делали бы Premiere разных
// лет, не устанавливая ни одной из них.
//
//   host_test.exe <путь к AV1Importer.prm> <файл.mp4>
//
// Зачем. Плагин собран по SDK 26.0, а работать должен и на Premiere постарше.
// Проверить это по-настоящему можно только живой установкой, но одну конкретную
// беду поймать отсюда: **набор функций хоста имеет версию**, и хост, который
// знает только седьмую, на просьбу о восьмой не отдаёт ничего. Ровно на этом
// плагин однажды и молчал бы: набор кэша кадров запрашивался строго восьмой
// версии, и на Premiere без неё каждый кадр возвращал бы ошибку.
//
// Чего этой проверкой НЕ увидеть: присылает ли старый Premiere другие запросы
// и в другом порядке, совпадают ли размеры структур со старыми заголовками SDK,
// читает ли он ресурс IMPT так же. Это только установкой.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKAppInfoSuite.h"
#include "PrSDKAsyncImporter.h"

typedef prMALError (*ImportEntryProc)(csSDK_int32, imStdParms*, void*, void*);

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    printf("    %-44s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

// ---------------------------------------------------------------------------
// Профиль хоста: чем он притворяется
// ---------------------------------------------------------------------------

struct HostProfile
{
    const char*  name;
    csSDK_int32  interfaceVer;     // что хост объявляет в imStdParms
    int          maxSuiteVersion;  // выше этой версии набор не выдаётся вовсе
    bool         hasFrameCache;    // отдаёт ли хост набор кэша кадров
    csSDK_uint32 appFourCC;
    unsigned     appMajor;
};

const HostProfile* g_host = nullptr;
int  g_cacheVersionGiven = 0;   // какую версию кэша хост в итоге отдал
bool g_cacheWasUsed      = false;

// ---------------------------------------------------------------------------
// Пиксельные буферы хоста
// ---------------------------------------------------------------------------

// PPixHand — это PPix**, поэтому первым полем держим указатель на сам PPix:
// адрес поля совпадает с адресом всей структуры, и обратный путь от дескриптора
// к нашим данным получается простым приведением типа.
struct FakeFrame
{
    PPix*                pixPtr;
    PPix                 pix;
    std::vector<uint8_t> data;
};

std::vector<FakeFrame*> g_frames;

// Замок к нему нужен по-настоящему.
//
// Асинхронный импортёр создаёт кадры из нескольких потоков сразу, и первая
// версия этой проверки падала с нарушением доступа: четыре потока портили
// общий вектор. Ошибка была в самом поддельном хосте — настоящий Premiere
// раздаёт буферы из любого потока, — но найдена она была только потому, что
// проверка полезла в многопоточность.
std::mutex g_framesMutex;


prSuiteError CreatePPix(PPixHand* outHand, PrPPixBufferAccess, PrPixelFormat pixelFormat,
                        const prRect* rect)
{
    const int width  = rect->right - rect->left;
    const int height = rect->bottom - rect->top;
    if (width <= 0 || height <= 0) return suiteError_Fail;

    // Размер буфера — по запрошенному формату, а не по восьми битам. Первая
    // версия этого хоста всегда выделяла по 4 байта на пиксель, и на 10-битном
    // файле плагин честно писал 16 бит в буфер вдвое меньше нужного: падение
    // с нарушением доступа. Ошибка была в самом хосте, но настоящий Premiere
    // выделяет буфер точно так же по формату, и проверка обязана это повторять.
    const int bytesPerPixel = (pixelFormat == PrPixelFormat_BGRA_4444_16u) ? 8 : 4;

    FakeFrame* f = new FakeFrame();
    f->pixPtr = &f->pix;
    memset(&f->pix, 0, sizeof(f->pix));
    f->pix.bounds       = *rect;
    f->pix.rowbytes     = width * bytesPerPixel;
    f->pix.bitsperpixel = 32;

    // Заполняем узнаваемым мусором: так видно, что плагин действительно записал
    // кадр, а не оставил буфер как был
    f->data.assign(static_cast<size_t>(f->pix.rowbytes) * height, 0xCD);
    f->pix.pix = f->data.data();

    {
        std::lock_guard<std::mutex> lock(g_framesMutex);
        g_frames.push_back(f);
    }
    *outHand = reinterpret_cast<PPixHand>(f);
    return suiteError_NoError;
}

FakeFrame* FrameOf(PPixHand hand) { return reinterpret_cast<FakeFrame*>(hand); }

prSuiteError GetPixels(PPixHand hand, PrPPixBufferAccess, char** outAddress)
{
    if (!hand) return suiteError_Fail;
    *outAddress = reinterpret_cast<char*>(FrameOf(hand)->data.data());
    return suiteError_NoError;
}

prSuiteError GetRowBytes(PPixHand hand, csSDK_int32* outRowBytes)
{
    if (!hand) return suiteError_Fail;
    *outRowBytes = FrameOf(hand)->pix.rowbytes;
    return suiteError_NoError;
}

prSuiteError DisposePPix(PPixHand) { return suiteError_NoError; }

// ---------------------------------------------------------------------------
// Кэш кадров хоста
// ---------------------------------------------------------------------------

prSuiteError AddFrameToCache(csSDK_uint32, csSDK_int32, PPixHand, csSDK_int32,
                             void*, csSDK_int32)
{
    g_cacheWasUsed = true;
    return suiteError_NoError;
}

// Всегда промах. Нам интересно, что плагин делает сам, а не как он повторно
// достаёт уже отданное.
prSuiteError GetFrameFromCache(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32,
                               imFrameFormat*, PPixHand*, void*, csSDK_int32)
{
    g_cacheWasUsed = true;
    return suiteError_Fail;
}

prSuiteError GetTicksPerSecond(PrTime* outTicks)
{
    *outTicks = 254016000000LL;   // столько тактов в секунде у Premiere
    return suiteError_NoError;
}

prSuiteError GetAppInfo(int selector, void* out)
{
    if (selector == PrSDKAppInfoSuite::kAppInfo_AppFourCC) {
        *reinterpret_cast<csSDK_uint32*>(out) = g_host->appFourCC;
        return suiteError_NoError;
    }
    if (selector == PrSDKAppInfoSuite::kAppInfo_Version) {
        VersionInfo* v = reinterpret_cast<VersionInfo*>(out);
        v->major = g_host->appMajor;
        v->minor = 0;
        v->patch = 0;
        return suiteError_NoError;
    }
    return suiteError_Fail;
}

PrSDKPPixCreatorSuite g_creatorSuite = {};
PrSDKPPixSuite        g_ppixSuite    = {};
PrSDKPPixCacheSuite   g_cacheSuite   = {};
PrSDKTimeSuite        g_timeSuite    = {};
PrSDKAppInfoSuite     g_appInfoSuite = {};

// ---------------------------------------------------------------------------
// SPBasicSuite — здесь и живёт вся имитация возраста хоста
// ---------------------------------------------------------------------------

SPErr AcquireSuite(const char* name, int version, const void** suite)
{
    // Главное в этой проверке. Настоящий хост не «выдаёт что есть»: набор
    // версии, которой он не знает, он не выдаёт вовсе, и указатель остаётся
    // нулевым. Для плагина это неотличимо от «такой функции нет».
    if (version > g_host->maxSuiteVersion) return kSPBadParameterError;

    if (strcmp(name, kPrSDKPPixCreatorSuite) == 0) { *suite = &g_creatorSuite; return kSPNoError; }
    if (strcmp(name, kPrSDKPPixSuite) == 0)        { *suite = &g_ppixSuite;    return kSPNoError; }
    if (strcmp(name, kPrSDKTimeSuite) == 0)        { *suite = &g_timeSuite;    return kSPNoError; }
    if (strcmp(name, kPrSDKAppInfoSuite) == 0)     { *suite = &g_appInfoSuite; return kSPNoError; }

    if (strcmp(name, kPrSDKPPixCacheSuite) == 0) {
        if (!g_host->hasFrameCache) return kSPBadParameterError;
        g_cacheVersionGiven = version;
        *suite = &g_cacheSuite;
        return kSPNoError;
    }
    return kSPBadParameterError;
}

SPErr ReleaseSuite(const char*, int) { return kSPNoError; }

SPBasicSuite g_basicSuite = {};
SPBasicSuite* GetSPBasicSuite() { return &g_basicSuite; }

// ---------------------------------------------------------------------------
// Память хоста
// ---------------------------------------------------------------------------

char** NewHandle(csSDK_uint32 size)
{
    char*  block = static_cast<char*>(calloc(1, size ? size : 1));
    char** h     = static_cast<char**>(malloc(sizeof(char*)));
    if (h) *h = block;
    return h;
}

void DisposeHandle(PrMemoryHandle h)
{
    if (!h) return;
    free(*h);
    free(h);
}

void LockHandle(PrMemoryHandle)   {}
void UnlockHandle(PrMemoryHandle) {}

PlugMemoryFuncs g_memFuncs = {};
PlugUtilFuncs   g_utilFuncs = {};
piSuites        g_piSuites = {};

void BuildHostTables()
{
    g_creatorSuite.CreatePPix = CreatePPix;

    g_ppixSuite.GetPixels   = GetPixels;
    g_ppixSuite.GetRowBytes = GetRowBytes;
    g_ppixSuite.Dispose     = DisposePPix;

    g_cacheSuite.AddFrameToCache   = AddFrameToCache;
    g_cacheSuite.GetFrameFromCache = GetFrameFromCache;

    g_timeSuite.GetTicksPerSecond = GetTicksPerSecond;
    g_appInfoSuite.GetAppInfo     = GetAppInfo;

    g_basicSuite.AcquireSuite = AcquireSuite;
    g_basicSuite.ReleaseSuite = ReleaseSuite;

    g_memFuncs.newHandle     = NewHandle;
    g_memFuncs.disposeHandle = DisposeHandle;
    g_memFuncs.lockHandle    = LockHandle;
    g_memFuncs.unlockHandle  = UnlockHandle;

    g_utilFuncs.getSPBasicSuite = GetSPBasicSuite;

    g_piSuites.memFuncs  = &g_memFuncs;
    g_piSuites.utilFuncs = &g_utilFuncs;
}

// ---------------------------------------------------------------------------
// Один прогон: открыть файл, спросить сведения, взять кадры и звук
// ---------------------------------------------------------------------------

// Асинхронная выдача: заказать кадры вперёд, потом забрать.
//
// Проверяем не «не упало», а совпадение: те же кадры, полученные асинхронно,
// обязаны быть побайтово теми же, что и обычным путём. Иначе поток внутри
// чужого процесса — это просто новый способ отдать не тот кадр.
struct AsyncResult
{
    bool created   = false;
    int  delivered = 0;
    bool matchesSync = true;   // совпали ли пиксели с обычным путём
    bool survived  = false;    // дожили ли до aiClose
    bool stressSurvived = true; // пережили ли параллельные вызовы и закрытие
};

struct RunResult
{
    bool                 opened     = false;
    bool                 gotInfo    = false;
    int                  framesGot  = 0;
    bool                 audioGot   = false;
    bool                 hasAudio   = false;   // в файле вообще есть звук?
    bool                 hasVideo   = true;    // а видео?
    int                  width      = 0;
    int                  height     = 0;
    int                  cacheVersion = 0;
    PrPixelFormat        pixelFormat  = PrPixelFormat_BGRA_4444_8u;
    std::vector<uint8_t> firstFrame;   // для сверки между профилями
    AsyncResult          async;
};

// Гоняем асинхронный путь по уже открытому файлу.
//
// Порядок вызовов взят из SDK: создать, заказать, забрать, отменить, слить,
// закрыть. Заказываем ВРАЗБИВКУ и забираем не в том порядке, в каком
// заказывали, — именно так ведёт себя хост при перемотке, и именно на этом
// ломается наивная реализация с одним «текущим кадром».
AsyncResult RunAsync(ImportEntryProc entry, imStdParms* stdParms,
                     imFileRef fileRef, void* privateData,
                     PrTime ticksPerFrame,
                     const std::vector<uint8_t>& syncFrameZero,
                     int width, int height, PrPixelFormat pixelFormat)
{
    AsyncResult out;

    imAsyncImporterCreationRec creation = {};
    creation.inPrivateData = privateData;

    if (entry(imCreateAsyncImporter, stdParms, &creation, nullptr) != malNoError ||
        !creation.outAsyncEntry || !creation.outAsyncPrivateData) {
        return out;   // не создался — не беда, но и проверять нечего
    }
    out.created = true;

    AsyncImporterEntry async = creation.outAsyncEntry;
    void* asyncData = creation.outAsyncPrivateData;

    // Заказываем вперёд с запасом и не подряд
    const int wanted[] = { 0, 3, 1, 7, 2, 5 };
    for (int frame : wanted) {
        aiAsyncRequest req = {};
        req.inPrivateData = asyncData;
        req.inSourceRec.inFrameTime = static_cast<PrTime>(frame) * ticksPerFrame;
        async(aiInitiateAsyncRead, &req);
    }

    // Один заказ отменяем: отмена — подсказка, и после неё кадр всё равно
    // обязан отдаться, просто медленнее
    {
        aiAsyncRequest cancel = {};
        cancel.inPrivateData = asyncData;
        cancel.inSourceRec.inFrameTime = static_cast<PrTime>(7) * ticksPerFrame;
        async(aiCancelAsyncRead, &cancel);
    }

    // Забираем в другом порядке
    const int fetch[] = { 1, 0, 5, 7, 2, 3 };
    for (int frame : fetch) {
        PPixHand hand = nullptr;
        // Формат тот же, что взял обычный путь: у 10-битного файла он
        // шестнадцатибитный, и сравнивать его с восьмибитным бессмысленно —
        // первая версия этой проверки именно так и «нашла расхождение»
        imFrameFormat format = {};
        format.inPixelFormat  = pixelFormat;
        format.inFrameWidth   = width;
        format.inFrameHeight  = height;

        imSourceVideoRec videoRec = {};
        videoRec.inPrivateData     = asyncData;
        videoRec.inFrameFormats    = &format;
        videoRec.inNumFrameFormats = 1;
        videoRec.outFrame          = &hand;
        videoRec.inFrameTime       = static_cast<PrTime>(frame) * ticksPerFrame;

        if (async(aiGetFrame, &videoRec) == aiNoError && hand) {
            FakeFrame* f = FrameOf(hand);
            bool written = false;
            for (uint8_t b : f->data) { if (b != 0xCD) { written = true; break; } }
            if (written) {
                ++out.delivered;
                if (frame == 0 && !syncFrameZero.empty() && f->data != syncFrameZero) {
                    out.matchesSync = false;
                }
            }
        }
    }

    async(aiFlush, asyncData);
    async(aiClose, asyncData);
    out.survived = true;
    return out;
}

// Два злых сценария для асинхронного пути.
//
// Оба взяты из правил SDK, а не выдуманы: «все вызовы реентерабельны, кроме
// закрытия» и «закрытие может быть вызвано, пока другие вызовы ещё
// выполняются». Второе особенно опасно — закрытие освобождает состояние
// импортёра, и если это случится из-под работающего вызова, получится
// обращение к освобождённой памяти внутри Premiere.
//
// Чего здесь СОЗНАТЕЛЬНО нет: вызовов после закрытия. SDK обещает, что их не
// будет, состояние к тому моменту освобождено, и проверять этим плагин значит
// проверять не его, а собственную невнимательность.
bool RunAsyncStress(ImportEntryProc entry, imStdParms* stdParms,
                    void* privateData, PrTime ticksPerFrame,
                    int width, int height)
{
    auto askForFrame = [&](AsyncImporterEntry async, void* asyncData, int frame) {
        PPixHand hand = nullptr;
        imFrameFormat format = {};
        format.inPixelFormat = PrPixelFormat_BGRA_4444_8u;
        format.inFrameWidth  = width;
        format.inFrameHeight = height;

        imSourceVideoRec videoRec = {};
        videoRec.inPrivateData     = asyncData;
        videoRec.inFrameFormats    = &format;
        videoRec.inNumFrameFormats = 1;
        videoRec.outFrame          = &hand;
        videoRec.inFrameTime       = static_cast<PrTime>(frame) * ticksPerFrame;
        async(aiGetFrame, &videoRec);
    };

    // --- 1. Много потоков разом, закрытие уже после того, как все вышли -----
    {
        imAsyncImporterCreationRec creation = {};
        creation.inPrivateData = privateData;
        if (entry(imCreateAsyncImporter, stdParms, &creation, nullptr) != malNoError ||
            !creation.outAsyncEntry || !creation.outAsyncPrivateData) {
            return true;   // асинхронный путь не предложен — мучить нечего
        }

        AsyncImporterEntry async = creation.outAsyncEntry;
        void* asyncData = creation.outAsyncPrivateData;

        std::atomic<bool> stop(false);
        std::vector<std::thread> crowd;

        for (int t = 0; t < 4; ++t) {
            crowd.emplace_back([&, t]() {
                for (int i = 0; !stop.load(); ++i) {
                    const int frame = (i * 3 + t) % 24;

                    aiAsyncRequest req = {};
                    req.inPrivateData = asyncData;
                    req.inSourceRec.inFrameTime =
                        static_cast<PrTime>(frame) * ticksPerFrame;
                    async(aiInitiateAsyncRead, &req);

                    if ((i & 3) == 0) async(aiCancelAsyncRead, &req);
                    askForFrame(async, asyncData, frame);
                }
            });
        }

        Sleep(400);
        stop.store(true);
        for (std::thread& th : crowd) th.join();   // никого внутри не осталось

        async(aiFlush, asyncData);
        async(aiClose, asyncData);
    }

    // --- 2. Закрытие ровно во время работающего вызова ----------------------
    //
    // Один поток делает РОВНО ОДИН запрос кадра подальше от начала, чтобы он
    // заведомо был внутри; главный тем временем закрывает. Правило «никаких
    // вызовов после закрытия» соблюдено: больше этот поток не зовёт никого.
    {
        imAsyncImporterCreationRec creation = {};
        creation.inPrivateData = privateData;
        if (entry(imCreateAsyncImporter, stdParms, &creation, nullptr) != malNoError ||
            !creation.outAsyncEntry || !creation.outAsyncPrivateData) {
            return true;
        }

        AsyncImporterEntry async = creation.outAsyncEntry;
        void* asyncData = creation.outAsyncPrivateData;

        // Заказываем далёкий кадр, чтобы работнику было чем заняться
        for (int frame = 40; frame < 56; ++frame) {
            aiAsyncRequest req = {};
            req.inPrivateData = asyncData;
            req.inSourceRec.inFrameTime = static_cast<PrTime>(frame) * ticksPerFrame;
            async(aiInitiateAsyncRead, &req);
        }

        // Ждём, пока поток дойдёт до самого вызова, и только потом закрываем.
        // Полной гарантии тут быть не может — между поднятием флага и входом
        // в плагин остаются микросекунды, — но случай «поток ещё не запустился»
        // этим снимается, а он и есть основной.
        std::atomic<bool> about(false);
        std::thread one([&]() {
            about.store(true);
            askForFrame(async, asyncData, 55);
        });
        while (!about.load()) { Sleep(0); }
        Sleep(20);
        async(aiFlush, asyncData);
        async(aiClose, asyncData);

        one.join();
    }

    return true;
}

RunResult Run(ImportEntryProc entry, const HostProfile& profile,
              const wchar_t* mediaPath)
{
    g_host              = &profile;
    g_cacheVersionGiven = 0;
    g_cacheWasUsed      = false;

    imStdParms stdParms = {};
    stdParms.imInterfaceVer = profile.interfaceVer;
    stdParms.piSuites       = &g_piSuites;

    RunResult out;

    imImportInfoRec info = {};
    entry(imInit, &stdParms, &info, nullptr);

    imFileOpenRec8 openRec = {};
    openRec.fileinfo.filepath = reinterpret_cast<const prUTF16Char*>(mediaPath);
    openRec.inStreamIdx       = 0;
    openRec.inImporterID      = 1;

    imFileRef fileRef = imInvalidHandleValue;
    out.opened = entry(imOpenFile8, &stdParms, &fileRef, &openRec) == malNoError;
    if (!out.opened) return out;

    imFileInfoRec8 fileInfo = {};
    fileInfo.privatedata = openRec.privatedata;
    fileInfo.streamIdx   = 0;
    const prMALError infoResult = entry(imGetInfo8, &stdParms, &openRec.fileinfo, &fileInfo);
    // imIterateStreams означает «есть ещё потоки», это тоже успех
    out.gotInfo = (infoResult == malNoError || infoResult == imIterateStreams);
    out.hasVideo = (fileInfo.hasVideo != 0);
    out.width   = fileInfo.vidInfo.imageWidth;
    out.height  = fileInfo.vidInfo.imageHeight;
    out.cacheVersion = g_cacheVersionGiven;
    if (!out.gotInfo) return out;

    if (out.hasVideo) {
    imIndPixelFormatRec pixFmt = {};
    pixFmt.privatedata    = openRec.privatedata;
    pixFmt.outPixelFormat = PrPixelFormat_BGRA_4444_8u;
    entry(imGetIndPixelFormat, &stdParms, reinterpret_cast<void*>(0), &pixFmt);
    out.pixelFormat = pixFmt.outPixelFormat;

    // Кадры. Берём нулевой и десятый: нулевой ловит открытие, десятый —
    // последовательное чтение.
    const PrTime ticksPerSecond = 254016000000LL;
    const csSDK_int32 wanted[] = { 0, 10 };
    for (csSDK_int32 frame : wanted) {
        imFrameFormat format = {};
        format.inFrameWidth  = out.width;
        format.inFrameHeight = out.height;
        format.inPixelFormat = pixFmt.outPixelFormat;

        PPixHand hand = nullptr;
        imSourceVideoRec videoRec = {};
        videoRec.inPrivateData     = openRec.privatedata;
        videoRec.inFrameFormats    = &format;
        videoRec.inNumFrameFormats = 1;
        videoRec.outFrame          = &hand;
        videoRec.inFrameTime       = static_cast<PrTime>(frame) * ticksPerSecond / 60;

        if (entry(imGetSourceVideo, &stdParms, fileRef, &videoRec) == malNoError && hand) {
            FakeFrame* f = FrameOf(hand);
            // Кадр засчитываем, только если в буфере не осталась наша заливка
            bool written = false;
            for (uint8_t b : f->data) { if (b != 0xCD) { written = true; break; } }
            if (written) {
                ++out.framesGot;
                if (frame == 0) out.firstFrame = f->data;
            }
        }
    }

    }   // конец видеочасти

    // Звук: одна десятая секунды с начала. Файл без звуковой дорожки — случай
    // законный (графика с прозрачностью, к примеру), и требовать от него отсчёты
    // значит проверять не плагин, а свои ожидания.
    out.hasAudio = (fileInfo.audInfo.numChannels > 0);
    const int    channels = fileInfo.audInfo.numChannels > 0 ? fileInfo.audInfo.numChannels : 2;
    const size_t samples  = 4800;
    std::vector<std::vector<float>> audio(channels, std::vector<float>(samples, 0.0f));
    std::vector<float*> ptrs(channels);
    for (int c = 0; c < channels; ++c) ptrs[c] = audio[c].data();

    imImportAudioRec7 audioRec = {};
    audioRec.position    = 0;
    audioRec.size        = static_cast<csSDK_uint32>(samples);
    audioRec.buffer      = ptrs.data();
    audioRec.privateData = openRec.privatedata;
    out.audioGot = entry(imImportAudio7, &stdParms, fileRef, &audioRec) == malNoError;

    // Асинхронный путь гоняем на том же открытом файле и сравниваем нулевой
    // кадр с тем, что дал обычный путь чуть выше
    if (out.hasVideo && out.width > 0 && out.height > 0) {
        PrTime ticksPerSecond = 0;
        g_timeSuite.GetTicksPerSecond(&ticksPerSecond);
        const PrTime ticksPerFrame =
            (fileInfo.vidScale > 0)
                ? ticksPerSecond * fileInfo.vidSampleSize / fileInfo.vidScale
                : ticksPerSecond / 30;

        out.async = RunAsync(entry, &stdParms, fileRef, openRec.privatedata,
                             ticksPerFrame, out.firstFrame, out.width, out.height,
                             out.pixelFormat);

        // Злые сценарии гоняем только на самом свежем профиле: они одинаковы
        // для всех хостов, а времени берут заметно
        if (profile.interfaceVer >= 24) {
            out.async.stressSurvived =
                RunAsyncStress(entry, &stdParms, openRec.privatedata,
                               ticksPerFrame, out.width, out.height);
        } else {
            out.async.stressSurvived = true;
        }
    }

    entry(imCloseFile, &stdParms, &fileRef, openRec.privatedata);

    {
        std::lock_guard<std::mutex> lock(g_framesMutex);
        for (FakeFrame* f : g_frames) delete f;
        g_frames.clear();
    }
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        printf("Usage: host_test <path to Aether.prm> <media file>\n");
        return 1;
    }


    HMODULE plugin = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!plugin) {
        printf("FAIL: library did not load, error %lu\n", GetLastError());
        return 2;
    }
    ImportEntryProc entry = (ImportEntryProc)GetProcAddress(plugin, "xImportEntry");
    if (!entry) {
        printf("FAIL: no xImportEntry export\n");
        return 3;
    }

    BuildHostTables();


    // Версии интерфейса импортёра из PrSDKImport.h: 24 = Premiere 23.2 и новее
    // (с тех пор не менялась), 21 = 13.0, то есть 2019 год.
    const HostProfile profiles[] = {
        { "Premiere 2025 (as built)",   IMPORTMOD_VERSION, 99, true,  kAppPremierePro,  25 },
        { "host with cache suite 7",    IMPORTMOD_VERSION,  7, true,  kAppPremierePro,  23 },
        { "Premiere 13.0-era host",     21,                 2, true,  kAppPremierePro,  13 },
        { "host without a frame cache", IMPORTMOD_VERSION, 99, false, kAppPremierePro,  25 },
        { "After Effects",              IMPORTMOD_VERSION, 99, true,  kAppAfterEffects, 25 },
    };

    std::vector<uint8_t> reference;

    for (const HostProfile& profile : profiles) {
        printf("\n%s (interface %d, suites up to %d, frame cache %s)\n",
               profile.name, profile.interfaceVer, profile.maxSuiteVersion,
               profile.hasFrameCache ? "yes" : "no");

        const RunResult r = Run(entry, profile, argv[2]);

    if (r.hasVideo) {
            printf("    accepted the file, %dx%d, frame cache suite %d, pixels %s\n",
                   r.width, r.height, r.cacheVersion,
                   r.pixelFormat == PrPixelFormat_BGRA_4444_16u ? "16u" : "8u");
        } else {
            printf("    accepted the file, no video, frame cache suite %d\n", r.cacheVersion);
        }

        Check(r.opened,           "file opened");
        Check(r.gotInfo,          "file info returned");
        if (r.hasVideo) {
            Check(r.framesGot == 2, "both frames delivered");
        } else {
            printf("    %-44s SKIP (no video track)\n", "both frames delivered");
        }
        if (r.hasAudio) {
            Check(r.audioGot,     "audio delivered");
        } else {
            printf("    %-44s SKIP (no audio track)\n", "audio delivered");
        }

        if (profile.hasFrameCache) {
            Check(r.cacheVersion > 0 && r.cacheVersion <= profile.maxSuiteVersion,
                  "frame cache taken at a version the host has");
        } else {
            Check(r.cacheVersion == 0, "no frame cache, and the plug-in went on anyway");
        }

        // Асинхронная выдача. Хост вправе ею не пользоваться, поэтому
        // «не создался» — не провал; а вот созданный обязан отдать кадры
        // и отдать ТЕ ЖЕ САМЫЕ.
        if (!r.hasVideo) {
            printf("    %-44s SKIP (no video track)\n", "async delivery");
        } else if (!r.async.created) {
            printf("    %-44s SKIP (not offered)\n", "async delivery");
        } else {
            printf("    async: %d of 6 frames delivered\n", r.async.delivered);
            Check(r.async.delivered == 6, "async delivered every frame asked for");
            Check(r.async.matchesSync,    "async frame 0 identical to the plain path");
            Check(r.async.survived,       "async importer closed without taking us down");
            Check(r.async.stressSurvived, "async survives concurrent calls and a close mid-work");
        }

        // Кадр обязан быть тем же самым независимо от возраста хоста: версия
        // набора влияет на скорость, а не на пиксели
        if (!r.hasVideo) {
            // сверять нечего
        } else if (reference.empty()) {
            reference = r.firstFrame;
        } else {
            Check(!r.firstFrame.empty() && r.firstFrame == reference,
                  "frame 0 identical to the newest host");
        }
    }

    printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_failures == 0 ? 0 : 4;
}
