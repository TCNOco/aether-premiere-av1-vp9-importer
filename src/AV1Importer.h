// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Импортёр AV1 для Adobe Premiere Pro.
//
// Premiere сам умеет разбирать контейнер MP4/MKV, но не умеет распаковывать
// видеопотоки AV1 и VP9 — на таймлайне это выглядит как "unsupported compression
// type av01". Плагин закрывает ровно эту дыру: объявляет себя обработчиком тех же
// расширений с более высоким приоритетом, а файлы с чужими кодеками возвращает
// через imBadFile,
// после чего Premiere отдаёт файл своему штатному импортёру. Так мы добавляем
// поддержку одного кодека, ничего не ломая для остальных.
//
// Вся распаковка живёт в AV1Decoder — этот файл только переводит запросы Premiere
// в вызовы декодера.

#ifndef AV1IMPORTER_H
#define AV1IMPORTER_H

#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixCreator2Suite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKAppInfoSuite.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKAudioSuite.h"

#include <windows.h>

#include "AV1Decoder.h"

#define AV1_IMPORTER_NAME   "Aether - AV1 / VP9 (ffmpeg)"
#define AV1_IMPORTER_SHORT  "Aether"

// Свой код формата. Premiere различает импортёры по нему, с настоящим
// содержимым файла он никак не связан.
#define AV1_FILE_TYPE       'AV01'

// Сколько символов UTF-16 помещается в сохранённый путь.
//
// Число было записано трижды: один раз в объявлении массива и дважды в
// вызовах CopyUtf16. Разъедься они — путь резался бы по меньшему из трёх,
// и никто бы не заметил. Теперь оно одно, а длина буфера в вызовах берётся
// из самого массива.
//
// Почему 2048, а не MAX_PATH: Premiere присылает пути с приставкой \\?\
// (видно в журнале), то есть в режиме длинных путей, где предел Windows —
// 32767 символов. Полный предел брать в структуру расточительно, а 2048
// покрывает всё, кроме нарочно закопанных папок. Но обрезание теперь
// ЗАМЕТНО: раньше файл просто не открывался без единого слова о причине.
#define AV1_PATH_CHARS      2048

// Данные одного открытого файла. Premiere хранит их за нас и возвращает
// в каждом вызове, поэтому глобального состояния у плагина нет.
typedef struct
{
    av1imp::Decoder*        decoder;        // создаётся через new: Premiere выдаёт сырую память без вызова конструкторов
    // Пин живых GetFrame/GetAudio. Premiere закрывает файл, не дожидаясь
    // других потоков; delete decoder без этого счётчика — UAF внутри хоста.
    // closing=1 после imCloseFile: новые пины не берутся, QuietFile не ставит.
    csSDK_int32             inFlight;
    csSDK_int32             closing;
    imFileRef               fileRef;        // дескриптор файла для самого Premiere; читаем мы через ffmpeg
    prUTF16Char             filePath[AV1_PATH_CHARS];

    csSDK_int32             importerID;
    PrTime                  ticksPerFrame;  // длительность кадра в тактах Premiere

    // Какой поток файла обслуживает этот экземпляр. Звуковых дорожек в записи
    // может быть несколько, и Premiere спрашивает про каждую отдельно:
    // поток 0 — видео плюс первая дорожка, дальше только звук.
    csSDK_int32             streamIdx;
    int                     audioTrack;     // номер дорожки звука для этого потока

    // Сколько запросов звука уже обслужено этим экземпляром.
    //
    // Нужен ради журнала. Конформирование читает дорожку целиком: пятиминутная
    // запись — это три с половиной тысячи запросов на дорожку, а дорожек
    // у записи OBS четыре. Четырнадцать тысяч строк не помогают разбирать
    // беду, они её прячут. Поэтому подробно пишем начало, дальше по одной
    // строке на каждые двести запросов, а отказы — всегда.
    csSDK_int32             audioRequests;

    // Сколько каналов мы ОБЪЯВИЛИ хосту в imGetInfo8.
    //
    // Ровно столько буферов он и выделит, а сказать нам об этом в
    // imImportAudioRec7 нечем — поля с числом каналов там нет. Значит помнить
    // это должны мы сами, иначе сверять нечего.
    csSDK_int32             declaredAudioChannels;

    PlugMemoryFuncsPtr      memFuncs;
    SPBasicSuite*           BasicSuite;
    PrSDKPPixCreatorSuite*  PPixCreatorSuite;
    PrSDKPPixCreator2Suite* PPixCreator2Suite; // CreateCustomPPix для P010
    PrSDKPPixCacheSuite*    PPixCacheSuite;
    PrSDKPPixSuite*         PPixSuite;
    PrSDKPPix2Suite*        PPix2Suite;     // адреса плоскостей; может не быть
    PrSDKTimeSuite*         TimeSuite;

    // Версия набора кэша, которую согласился отдать хост. Заголовки SDK знают
    // восьмую, но Premiere постарше отдаёт седьмую и ниже, а отдавать набор
    // положено ровно той версией, какой брали.
    csSDK_int32             PPixCacheSuiteVersion;

    // То же и для набора PPix2: адреса плоскостей появились только во второй
    // версии, а на Premiere постарше набор отдаётся первой — тогда родной YUV
    // мы просто не предлагаем и идём прежним путём.
    csSDK_int32             PPix2SuiteVersion;

    // И для остальных — не ради перебора версий, а ради учёта.
    //
    // Ноль здесь значит «хост набор НЕ выдал». Раньше результат AcquireSuite
    // не смотрели вовсе, а ReleaseSuite звали безусловно: на хосте, который
    // отказал, плагин возвращал бы ссылку, которой не брал. Счётчик ссылок
    // чужой, и уронить его ниже нуля — испортить набор всем остальным
    // плагинам в процессе, а не себе.
    csSDK_int32             PPixCreatorSuiteVersion;
    csSDK_int32             PPixCreator2SuiteVersion;
    csSDK_int32             PPixSuiteVersion;
    csSDK_int32             TimeSuiteVersion;
} ImporterLocalRec, *ImporterLocalRecPtr, **ImporterLocalRecH;

namespace av1imp {

// Взять самую свежую версию набора, какую согласен отдать хост.
//
// Adobe новые функции дописывает в конец набора, поэтому просить можно сверху
// вниз, а пользоваться — только давно существующими. Без этого плагин,
// собранный по свежему SDK, молча остаётся без кэша кадров на Premiere
// постарше: восьмой версии там нет, а спрашивать седьмую никто не додумался.
//
// Живёт в заголовке, а не в AV1Importer.cpp, потому что нужна двоим:
// асинхронный импортёр берёт наборы САМ, а не одалживает чужие (см. ниже).
// Возвращает выданную версию или 0; ею же потом и отдавать — набор положено
// возвращать ровно той версией, какой брали.
template <typename T>
csSDK_int32 AcquireNewestSuite(SPBasicSuite* basic, const char* name,
                               csSDK_int32 newest, T** out)
{
    *out = nullptr;
    if (!basic) return 0;
    for (csSDK_int32 version = newest; version >= 1; --version) {
        if (basic->AcquireSuite(name, version, (const void**)out) == kSPNoError && *out) {
            return version;
        }
        *out = nullptr;
    }
    return 0;
}

// Записать кадр в буфер, который выдал хост.
//
// Одна на оба пути выдачи, обычный и асинхронный, и это не красота ради
// красоты: раньше запись была написана дважды, и когда появилась выдача
// плоскостями, асинхронный путь про неё не узнал. Поддельный хост поймал это
// сразу — «кадр асинхронного пути не совпадает с обычным», — но в Premiere
// это выглядело бы как позеленевшая картинка при включённом ускорении.
//
// pixelFormat решает, как писать: BGRA одним куском или тремя плоскостями.
// ppix2 нужен только плоскостям и может быть нулевым в остальных случаях.
bool WriteFrameToBuffer(Decoder& decoder, int64_t frameIndex,
                        PrSDKPPixSuite* ppixSuite, PrSDKPPix2Suite* ppix2Suite,
                        PPixHand frame, PrPixelFormat pixelFormat,
                        int width, int height, const char** outWhy);

// Наш ли это формат плоскостей — восемь констант YUV 4:2:0 кадром.
bool IsNativeYUV(PrPixelFormat f);

// Он же для десяти бит: две плоскости, P010. Констант тоже восемь —
// две матрицы (709 и 2020), у 2020 ещё PQ и HLG, и у каждой два размаха.
bool IsNativeP010(PrPixelFormat f);

// Умеем ли мы вообще положить кадр в такой формат.
bool CanProduce(PrPixelFormat f);

// Выбрать из списка, который прислал хост, первый формат, который мы умеем.
//
// SDK про этот массив говорит прямо: «в порядке предпочтения». А читали мы
// из него ровно нулевой элемент и на этом останавливались — то есть если
// хост первым назвал формат не из наших, мы молча подсовывали BGRA вместо
// того, чтобы посмотреть, не стоит ли вторым как раз родной YUV, ради
// которого всё и затевалось.
//
// Возвращает индекс выбранного или -1, если ничего из списка мы не умеем.
// Список короткий (у Premiere это единицы), перебор стоит ничего.
// startFrom — с какого индекса смотреть дальше (после отказа P010).
int PickFrameFormat(const imFrameFormat* formats, int count, int startFrom = 0);

// Хост не дал буфер под P010 — до перезапуска процесса больше не предлагаем.
void MarkP010Unavailable();
bool IsP010Unavailable();

// Создать буфер кадра. Для P010 пробуем несколько способов подряд и
// пишем в журнал код отказа — иначе снова гадаем вслепую.
prMALError CreateVideoPPix(PrSDKPPixCreatorSuite* creator,
                           PrSDKPPixCreator2Suite* creator2,
                           PrSDKPPixSuite* ppixSuite,
                           PrSDKPPix2Suite* ppix2Suite,
                           PPixHand* outFrame,
                           PrPixelFormat pixelFormat,
                           int width, int height);

// Кадр, который CreatePPix уже выдал, а записать в него не вышло.
// Хост на ошибке не обязан забирать outFrame — без Dispose это утечка
// его буфера, а иногда и полуживой указатель на следующем кадре.
inline void DiscardHostFrame(PrSDKPPixSuite* suite, PPixHand* frame)
{
    if (!suite || !frame || !*frame) return;
    suite->Dispose(*frame);
    *frame = nullptr;
}

// Перевод качества хоста в наше. PrRenderQuality приходит с каждым запросом
// кадра, и до сих пор мы его не читали.
//
// Порог по Low, а не по Draft: черновое и низкое — это скраб и предпросмотр,
// среднее и выше Premiere просит там, где картинку будут смотреть.
inline Scaling ScalingFor(PrRenderQuality q)
{
    return (q == kPrRenderQuality_Draft || q == kPrRenderQuality_Low)
           ? Scaling::Fast : Scaling::Good;
}

// Сводка запросов хоста за процесс: размер, формат, качество, seq vs jump.
// Без строки на каждый кадр — одна запись в журнал при выгрузке.
void NoteHostVideoRequest(csSDK_int32 frameIndex,
                          int nativeWidth, int nativeHeight,
                          int askedWidth, int askedHeight,
                          PrPixelFormat pixelFormat,
                          PrRenderQuality quality);
void LogHostRequestProfile();

} // namespace av1imp

extern "C" {
PREMPLUGENTRY DllExport xImportEntry(csSDK_int32  selector,
                                     imStdParms*  stdParms,
                                     void*        param1,
                                     void*        param2);
}

#endif  // AV1IMPORTER_H
