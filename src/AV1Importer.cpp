// Импортёр AV1 для Adobe Premiere Pro — слой между Premiere и AV1Decoder.
//
// Premiere общается с импортёром одной функцией xImportEntry: присылает номер
// запроса (селектор) и структуру с полями "вход/выход". Здесь обработаны только
// те запросы, без которых файл не встанет на таймлайн; на всё остальное отвечаем
// imUnsupported, и Premiere просто не будет этим пользоваться.

#include "AV1Importer.h"
#include "AV1Log.h"

#include <string>

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

void CopyUtf16(prUTF16Char* dst, size_t dstCount, const prUTF16Char* src)
{
    size_t i = 0;
    if (src) {
        for (; src[i] && i < dstCount - 1; ++i) dst[i] = src[i];
    }
    dst[i] = 0;
}

// Premiere может спросить сведения о файле раньше, чем откроет его,
// поэтому декодер создаём по требованию из сохранённого пути.
bool EnsureDecoder(ImporterLocalRecPtr ldata)
{
    if (ldata->decoder && ldata->decoder->IsOpen()) return true;

    if (!ldata->decoder) ldata->decoder = new av1imp::Decoder();

    const std::string path = Utf8FromUtf16(ldata->filePath);
    if (path.empty()) return false;

    // Кадры отдаёт только поток 0; остальным нужен лишь звук, и декодер
    // на видеокарте им создавать незачем
    const bool needVideo = (ldata->streamIdx == 0);
    return ldata->decoder->Open(path, /*preferHardware=*/true, needVideo);
}

// Дорожка звука открывается по требованию: Premiere спрашивает отсчёты
// не у всех потоков и не сразу
bool EnsureAudio(ImporterLocalRecPtr ldata)
{
    if (!ldata->decoder) return false;
    if (ldata->decoder->HasAudio()) return true;
    return ldata->decoder->OpenAudio(ldata->audioTrack);
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
    importInfo->keepLoaded      = kPrFalse;

    // Чтобы получить .mp4 раньше штатного импортёра Premiere, нужно ровно это:
    // в документации SDK сказано, что для перебивания импортёров самой Adobe
    // приоритет должен быть 100 и выше. Со значением ниже Premiere разбирает
    // файл сам и до плагина не доходит.
    // Файлы не с AV1 мы тут же вернём обратно через imBadFile, и разбирать
    // их будет всё тот же штатный импортёр.
    importInfo->priority        = 100;

    av1imp::Log("imInit: приоритет %d", importInfo->priority);
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
    static const char kExtensions[] = "mp4\0mkv\0webm\0mov\0m4v\0";

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
    CopyUtf16(ldata->filePath, 2048, openRec->fileinfo.filepath);

    ldata->streamIdx  = openRec->inStreamIdx;
    ldata->audioTrack = openRec->inStreamIdx;   // поток N обслуживает дорожку N

    // Дескриптор нужен самому Premiere; читаем мы через ffmpeg, поэтому
    // открываем на чтение и разрешаем параллельное чтение другим.
    if (ldata->fileRef == nullptr || ldata->fileRef == imInvalidHandleValue) {
        ldata->fileRef = CreateFileW(reinterpret_cast<LPCWSTR>(openRec->fileinfo.filepath),
                                     GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (ldata->fileRef == imInvalidHandleValue) {
        return imBadFile;
    }

    // Главная проверка: файл наш, только если внутри действительно AV1.
    // Иначе честно отдаём его обратно — Premiere передаст штатному импортёру.
    if (!EnsureDecoder(ldata)) {
        av1imp::Log("imOpenFile8: отказ — %s",
                    ldata->decoder ? ldata->decoder->LastError().c_str() : "нет декодера");
        CloseHandle(ldata->fileRef);
        ldata->fileRef = imInvalidHandleValue;
        return imBadFile;
    }
    av1imp::Log("imOpenFile8: принят, %dx%d, декодер %s",
                ldata->decoder->Info().width, ldata->decoder->Info().height,
                ldata->decoder->Info().decoderName.c_str());

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
    av1imp::Log("imQuietFile: поток %d", ldata->streamIdx);
    if (ldata->decoder) {
        ldata->decoder->Close();  // сам объект оставляем: путь известен, откроемся заново
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
    av1imp::Log("imCloseFile: поток %d", ldata->streamIdx);

    if (ldata->decoder) {
        delete ldata->decoder;
        ldata->decoder = nullptr;
    }
    if (ldata->fileRef && ldata->fileRef != imInvalidHandleValue) {
        CloseHandle(ldata->fileRef);
        ldata->fileRef = imInvalidHandleValue;
    }
    if (ldata->BasicSuite) {
        ldata->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
        ldata->BasicSuite->ReleaseSuite(kPrSDKPPixCacheSuite,   kPrSDKPPixCacheSuiteVersion);
        ldata->BasicSuite->ReleaseSuite(kPrSDKPPixSuite,        kPrSDKPPixSuiteVersion);
        ldata->BasicSuite->ReleaseSuite(kPrSDKTimeSuite,        kPrSDKTimeSuiteVersion);
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
        CopyUtf16(ldata->filePath, 2048, fileAccess->filepath);
    }

    av1imp::Log("imGetInfo8: спрашивают про %S (поток %d)",
                ldata->filePath, fileInfo->streamIdx);

    // Записать номер потока нужно до открытия: от него зависит,
    // создавать ли декодер кадров
    ldata->streamIdx  = fileInfo->streamIdx;
    ldata->audioTrack = fileInfo->streamIdx;

    if (!EnsureDecoder(ldata)) {
        av1imp::Log("imGetInfo8: отказ — %s",
                    ldata->decoder ? ldata->decoder->LastError().c_str() : "нет декодера");
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        return imBadFile;
    }

    const av1imp::MediaInfo& mi = ldata->decoder->Info();

    ldata->memFuncs = stdParms->piSuites->memFuncs;

    // Premiere спрашивает сведения о файле не единожды, а наборы функций
    // положено брать и отдавать поровну — иначе накопим лишние ссылки
    if (!ldata->BasicSuite) {
        ldata->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
    }
    if (ldata->BasicSuite && !ldata->PPixSuite) {
        ldata->BasicSuite->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion,
                                        (const void**)&ldata->PPixCreatorSuite);
        ldata->BasicSuite->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion,
                                        (const void**)&ldata->PPixCacheSuite);
        ldata->BasicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                                        (const void**)&ldata->PPixSuite);
        ldata->BasicSuite->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                                        (const void**)&ldata->TimeSuite);
    }

    // Какой поток спрашивают. Поток 0 — видео плюс первая дорожка звука,
    // потоки 1..N — остальные дорожки. Отдавать их порознь обязательно:
    // OBS пишет микрофон, игру, дискорд и музыку отдельно, и сводить их
    // в одну дорожку означало бы потерять саму возможность их разделить.
    const int audioTracks = mi.audioStreamCount;

    if (fileInfo->streamIdx > 0 && fileInfo->streamIdx >= audioTracks) {
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        return imBadStreamIndex;
    }

    fileInfo->hasVideo    = (fileInfo->streamIdx == 0) ? kPrTrue : kPrFalse;
    fileInfo->hasAudio    = kPrFalse;
    fileInfo->accessModes = kRandomAccessImport;
    fileInfo->hasDataRate = kPrFalse;

    if (audioTracks > 0 && EnsureAudio(ldata)) {
        fileInfo->hasAudio            = kPrTrue;
        fileInfo->audInfo.numChannels = mi.audioChannels;
        fileInfo->audInfo.sampleRate  = static_cast<float>(mi.audioSampleRate);
        fileInfo->audInfo.sampleType  = kPrAudioSampleType_32BitFloat;
        fileInfo->audDuration         = mi.audioSampleCount;
    }

    if (!fileInfo->hasVideo) {
        av1imp::Log("imGetInfo8: поток %d — только звук, %d кан",
                    fileInfo->streamIdx, mi.audioChannels);
        stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
        // Есть ли ещё дорожки после этой
        return (fileInfo->streamIdx + 1 < audioTracks) ? imIterateStreams : malNoError;
    }

    fileInfo->vidInfo.imageWidth     = mi.width;
    fileInfo->vidInfo.imageHeight    = mi.height;
    fileInfo->vidInfo.depth          = 32;
    // Ключевой момент. Сказать здесь 'av01' — значит отправить Premiere искать
    // декодер AV1, которого у него нет: он его не находит и отказывает файлу
    // словами "unsupported compression type av01", даже не спросив у нас кадр.
    // Но распаковываем-то мы, а наружу отдаём готовые пиксели, поэтому с точки
    // зрения Premiere источник несжатый.
    fileInfo->vidInfo.subType        = imUncompressed;
    fileInfo->vidInfo.fieldType      = prFieldsNone;
    fileInfo->vidInfo.alphaType      = alphaNone;
    fileInfo->vidInfo.pixelAspectNum = 1;
    fileInfo->vidInfo.pixelAspectDen = 1;
    fileInfo->vidInfo.isStill        = kPrFalse;
    fileInfo->vidInfo.noDuration     = imNoDurationFalse;
    fileInfo->vidInfo.hasPulldown    = kPrFalse;

    // Кадры выдаём через imGetSourceVideo; асинхронного пути пока нет
    fileInfo->vidInfo.supportsGetSourceVideo = kPrTrue;
    fileInfo->vidInfo.supportsAsyncIO        = kPrFalse;

    // Частоту передаём дробью — иначе 59.94 превратится в 60 и картинка уедет
    fileInfo->vidScale      = mi.fpsNum > 0 ? mi.fpsNum : 30;
    fileInfo->vidSampleSize = mi.fpsDen > 0 ? mi.fpsDen : 1;
    fileInfo->vidDuration   = static_cast<csSDK_int32>(mi.frameCount) * fileInfo->vidSampleSize;
    fileInfo->vidDurationInFrames = mi.frameCount;

    ldata->importerID = fileInfo->vidInfo.importerID;

    // Длительность кадра в тактах Premiere — по ней переводим время в номер кадра
    if (ldata->TimeSuite && mi.fpsNum > 0) {
        PrTime ticksPerSecond = 0;
        ldata->TimeSuite->GetTicksPerSecond(&ticksPerSecond);
        ldata->ticksPerFrame = ticksPerSecond * mi.fpsDen / mi.fpsNum;
    }

    av1imp::Log("imGetInfo8: %dx%d, %d/%d кадр/с, %lld кадров, тип сжатия RAW",
                mi.width, mi.height, fileInfo->vidScale, fileInfo->vidSampleSize,
                (long long)mi.frameCount);

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
    if (!EnsureDecoder(ldata) || !EnsureAudio(ldata)) {
        av1imp::Log("imImportAudio7: звук недоступен — %s",
                    ldata->decoder ? ldata->decoder->LastError().c_str() : "нет декодера");
        return imFileReadFailed;
    }

    if (!ldata->decoder->GetAudio(audioRec->position,
                                  static_cast<int32_t>(audioRec->size),
                                  audioRec->buffer)) {
        av1imp::Log("звук с %lld: ОШИБКА — %s", (long long)audioRec->position,
                    ldata->decoder->LastError().c_str());
        return imFileReadFailed;
    }

    // Пишем каждый запрос: конформирование читает дорожку целиком, и по этим
    // строкам видно, на каком месте оно оборвалось, если оборвалось
    av1imp::Log("звук: дорожка %d, с %lld, %u отсчётов",
                ldata->audioTrack, (long long)audioRec->position, audioRec->size);
    return malNoError;
}

// ---------------------------------------------------------------------------
// В каком виде отдаём пиксели и какого размера
// ---------------------------------------------------------------------------

static prMALError AV1GetIndPixelFormat(imStdParms* stdParms, csSDK_size_t idx,
                                       imIndPixelFormatRec* rec)
{
    if (idx != 0) return imBadFormatIndex;
    rec->outPixelFormat = PrPixelFormat_BGRA_4444_8u;
    return malNoError;
}

static prMALError AV1PreferredFrameSize(imStdParms* stdParms,
                                        imPreferredFrameSizeRec* rec)
{
    if (rec->inIndex != 0) return imOtherErr;

    ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(rec->inPrivateData);
    if (!ldataH || !(*ldataH)->decoder) return imOtherErr;

    const av1imp::MediaInfo& mi = (*ldataH)->decoder->Info();
    rec->outWidth  = mi.width;
    rec->outHeight = mi.height;
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
    if (!ldata->PPixSuite || !ldata->PPixCreatorSuite || !ldata->PPixCacheSuite) {
        return imOtherErr;  // imGetInfo8 не отработал: наборов функций нет
    }
    if (!EnsureDecoder(ldata)) return imBadFile;

    const av1imp::MediaInfo& mi = ldata->decoder->Info();

    if (ldata->ticksPerFrame <= 0) return imOtherErr;
    const csSDK_int32 frameIndex =
        static_cast<csSDK_int32>(videoRec->inFrameTime / ldata->ticksPerFrame);

    // Если кадр уже разбирали — Premiere вернёт его из своего кэша,
    // и на перемотке туда-сюда мы не декодируем одно и то же дважды
    prMALError result = ldata->PPixCacheSuite->GetFrameFromCache(
        ldata->importerID, 0, frameIndex, 1,
        videoRec->inFrameFormats, videoRec->outFrame, nullptr, 0);

    if (result == suiteError_NoError) return result;

    imFrameFormat* format = &videoRec->inFrameFormats[0];
    if (format->inFrameWidth == 0 && format->inFrameHeight == 0) {
        format->inFrameWidth  = mi.width;
        format->inFrameHeight = mi.height;
    }

    prRect rect;
    prSetRect(&rect, 0, 0, format->inFrameWidth, format->inFrameHeight);

    result = ldata->PPixCreatorSuite->CreatePPix(videoRec->outFrame,
                                                 PrPPixBufferAccess_ReadWrite,
                                                 PrPixelFormat_BGRA_4444_8u, &rect);
    if (result != malNoError) return result;

    char* buffer = nullptr;
    csSDK_int32 rowBytes = 0;
    ldata->PPixSuite->GetPixels(*videoRec->outFrame, PrPPixBufferAccess_ReadWrite, &buffer);
    ldata->PPixSuite->GetRowBytes(*videoRec->outFrame, &rowBytes);
    if (!buffer || rowBytes <= 0) return imOtherErr;

    // У Premiere 32-битные буферы идут снизу вверх: пишем с последней строки
    // отрицательным шагом, и переворот делает сам масштабатор ffmpeg
    uint8_t* lastRow = reinterpret_cast<uint8_t*>(buffer)
                     + static_cast<size_t>(format->inFrameHeight - 1) * rowBytes;

    if (!ldata->decoder->GetFrameBGRA(frameIndex, lastRow, -rowBytes)) {
        av1imp::Log("кадр %d: ОШИБКА — %s", frameIndex, ldata->decoder->LastError().c_str());
        return imFileReadFailed;
    }
    if (frameIndex < 3) {
        av1imp::Log("кадр %d выдан (%dx%d, шаг %d)", frameIndex,
                    format->inFrameWidth, format->inFrameHeight, rowBytes);
    }

    ldata->PPixCacheSuite->AddFrameToCache(ldata->importerID, 0, *videoRec->outFrame,
                                           frameIndex, nullptr, 0);
    return malNoError;
}

// ---------------------------------------------------------------------------
// Точка входа
// ---------------------------------------------------------------------------

PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms,
                                     void* param1, void* param2)
{
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

        case imGetSourceVideo:
            result = AV1GetSourceVideo(stdParms, reinterpret_cast<imFileRef>(param1),
                                       reinterpret_cast<imSourceVideoRec*>(param2));
            break;

        // Рукопожатие: плагин объявляет, какой версией интерфейса владеет.
        // Без ответа Premiere считает импортёр устаревшим и не спрашивает у него
        // ни формат пикселей, ни кадры — просто прочитает сведения о файле и уйдёт.
        case imImportAudio7:
            result = AV1ImportAudio7(stdParms, reinterpret_cast<imFileRef>(param1),
                                     reinterpret_cast<imImportAudioRec7*>(param2));
            break;

        case imGetSupports7:
            result = malSupports7;
            break;

        case imGetSupports8:
            result = malSupports8;
            break;

        case imShutdown:
            result = malNoError;
            break;

        default:
            result = imUnsupported;
            break;
    }

    // Пишем ВСЕ запросы, включая отвергнутые. Первая версия журнала их прятала
    // ради чистоты — и спрятала как раз то, что нужно: отказ на нужном запросе
    // выглядит для Premiere так же, как отсутствие плагина.
    av1imp::Log("  запрос %s -> %d", av1imp::SelectorName(selector), result);
    return result;
}
