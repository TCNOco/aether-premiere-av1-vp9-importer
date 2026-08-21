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
#include <vector>

#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKAppInfoSuite.h"

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

    g_frames.push_back(f);
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

struct RunResult
{
    bool                 opened     = false;
    bool                 gotInfo    = false;
    int                  framesGot  = 0;
    bool                 audioGot   = false;
    int                  width      = 0;
    int                  height     = 0;
    int                  cacheVersion = 0;
    PrPixelFormat        pixelFormat  = PrPixelFormat_BGRA_4444_8u;
    std::vector<uint8_t> firstFrame;   // для сверки между профилями
};

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
    out.width   = fileInfo.vidInfo.imageWidth;
    out.height  = fileInfo.vidInfo.imageHeight;
    out.cacheVersion = g_cacheVersionGiven;
    if (!out.gotInfo) return out;

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

    // Звук: одна десятая секунды с начала
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

    entry(imCloseFile, &stdParms, &fileRef, openRec.privatedata);

    for (FakeFrame* f : g_frames) delete f;
    g_frames.clear();
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        printf("Usage: host_test <path to AV1Importer.prm> <media file>\n");
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

        printf("    accepted the file, %dx%d, frame cache suite %d, pixels %s\n",
               r.width, r.height, r.cacheVersion,
               r.pixelFormat == PrPixelFormat_BGRA_4444_16u ? "16u" : "8u");

        Check(r.opened,           "file opened");
        Check(r.gotInfo,          "file info returned");
        Check(r.framesGot == 2,   "both frames delivered");
        Check(r.audioGot,         "audio delivered");

        if (profile.hasFrameCache) {
            Check(r.cacheVersion > 0 && r.cacheVersion <= profile.maxSuiteVersion,
                  "frame cache taken at a version the host has");
        } else {
            Check(r.cacheVersion == 0, "no frame cache, and the plug-in went on anyway");
        }

        // Кадр обязан быть тем же самым независимо от возраста хоста: версия
        // набора влияет на скорость, а не на пиксели
        if (reference.empty()) {
            reference = r.firstFrame;
        } else {
            Check(!r.firstFrame.empty() && r.firstFrame == reference,
                  "frame 0 identical to the newest host");
        }
    }

    printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_failures == 0 ? 0 : 4;
}
