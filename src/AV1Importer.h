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
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixSuite.h"
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

// Данные одного открытого файла. Premiere хранит их за нас и возвращает
// в каждом вызове, поэтому глобального состояния у плагина нет.
typedef struct
{
    av1imp::Decoder*        decoder;        // создаётся через new: Premiere выдаёт сырую память без вызова конструкторов
    imFileRef               fileRef;        // дескриптор файла для самого Premiere; читаем мы через ffmpeg
    prUTF16Char             filePath[2048];

    csSDK_int32             importerID;
    PrTime                  ticksPerFrame;  // длительность кадра в тактах Premiere

    // Какой поток файла обслуживает этот экземпляр. Звуковых дорожек в записи
    // может быть несколько, и Premiere спрашивает про каждую отдельно:
    // поток 0 — видео плюс первая дорожка, дальше только звук.
    csSDK_int32             streamIdx;
    int                     audioTrack;     // номер дорожки звука для этого потока

    PlugMemoryFuncsPtr      memFuncs;
    SPBasicSuite*           BasicSuite;
    PrSDKPPixCreatorSuite*  PPixCreatorSuite;
    PrSDKPPixCacheSuite*    PPixCacheSuite;
    PrSDKPPixSuite*         PPixSuite;
    PrSDKTimeSuite*         TimeSuite;

    // Версия набора кэша, которую согласился отдать хост. Заголовки SDK знают
    // восьмую, но Premiere постарше отдаёт седьмую и ниже, а отдавать набор
    // положено ровно той версией, какой брали.
    csSDK_int32             PPixCacheSuiteVersion;
} ImporterLocalRec, *ImporterLocalRecPtr, **ImporterLocalRecH;

extern "C" {
PREMPLUGENTRY DllExport xImportEntry(csSDK_int32  selector,
                                     imStdParms*  stdParms,
                                     void*        param1,
                                     void*        param2);
}

#endif  // AV1IMPORTER_H
