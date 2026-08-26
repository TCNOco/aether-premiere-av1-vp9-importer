// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "AV1Async.h"
#include "AV1Log.h"
#include "AV1Settings.h"
#include "AV1Importer.h"

#include <PrSDKAsyncImporter.h>
#include <PrSDKPPixCreator2Suite.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace av1imp {

namespace {

// Сколько распакованных кадров держим наготове.
//
// Считаем штуками, а не байтами, и намеренно немного. Асинхронный импортёр
// существует ОТДЕЛЬНО от обычного, то есть на каждый клип получается второй
// декодер со своим кэшем; складывать сюда ещё сотню кадров значит удваивать
// расход памяти на ровном месте. Восьми хватает, чтобы хост успевал заказывать
// вперёд, — дальше него он всё равно не забегает.
const size_t kReadyLimit = 8;

// Данные асинхронного импортёра. Своя копия всего нужного: ссылаться на
// обычный импортёр нельзя, он может закрыться раньше.
struct AsyncState {
    Decoder      decoder;
    std::string  path;

    // Наборы функций хоста — ВЗЯТЫЕ СЕБЕ, а не одолженные у обычного импортёра.
    //
    // Копировать указатели было мало, и это ровно тот случай, когда правило
    // из заголовка нарушалось буквой, соблюдённой на словах. Обычный импортёр
    // в imCloseFile эти наборы ОСВОБОЖДАЕТ. Закройся он раньше нас — а SDK
    // прямо говорит, что время жизни у нас развязано, — и мы продолжили бы
    // звать CreatePPix и GetYUV420PlanarBuffers через ссылки, которых больше
    // не держим. Живёт такое ровно до хоста, который считает ссылки честно.
    //
    // Поэтому здесь свои AcquireSuite при создании и свои ReleaseSuite
    // в aiClose, как со всем остальным: путь свой, декодер свой, наборы свои.
    SPBasicSuite*          BasicSuite       = nullptr;
    PrSDKPPixCreatorSuite* PPixCreatorSuite = nullptr;
    PrSDKPPixCreator2Suite* PPixCreator2Suite = nullptr;
    PrSDKPPixSuite*        PPixSuite        = nullptr;
    PrSDKPPix2Suite*       PPix2Suite       = nullptr;   // адреса плоскостей

    // Версии, которыми наборы выданы: отдавать положено ровно ими, а ноль
    // означает «не выдали, возвращать нечего»
    csSDK_int32            PPixCreatorSuiteVersion  = 0;
    csSDK_int32            PPixCreator2SuiteVersion = 0;
    csSDK_int32            PPixSuiteVersion         = 0;
    csSDK_int32            PPix2SuiteVersion        = 0;

    int   width  = 0;
    int   height = 0;
    PrTime ticksPerFrame = 0;

    std::thread             worker;
    std::mutex              mutex;
    std::condition_variable wake;      // будит работника
    std::condition_variable done;      // будит ждущих готовности кадра

    std::condition_variable idle;      // будит закрытие, когда никто не работает

    std::deque<int64_t>          queue;    // что заказано и ещё не начато
    std::map<int64_t, bool>      ready;    // номер -> удалось ли распаковать
    int64_t                      busyWith = -1;   // что работник распаковывает прямо сейчас
    bool                         stopping = false;
    bool                         flushing = false;

    // Сколько вызовов хоста сейчас внутри нас.
    //
    // SDK разрешает позвать aiClose, «пока другие вызовы ещё выполняются», а
    // закрытие освобождает эту самую структуру. Без счётчика это обращение
    // к освобождённой памяти внутри Premiere — то есть падение хоста, которое
    // никто не свяжет с плагином.
    int                          inFlight = 0;
};

// Отмечает, что вызов хоста работает внутри нас, и не даёт закрытию освободить
// состояние раньше времени. Возвращает false, если закрытие уже началось.
class CallGuard {
public:
    explicit CallGuard(AsyncState* s) : state_(s), entered_(false)
    {
        if (!s) return;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->stopping) return;
        ++s->inFlight;
        entered_ = true;
    }
    ~CallGuard()
    {
        if (!entered_) return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            --state_->inFlight;
        }
        state_->idle.notify_all();
    }
    bool entered() const { return entered_; }

    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;

private:
    AsyncState* state_;
    bool        entered_;
};

// Работник. Распаковывает заказанные кадры по одному и складывает их в кэш
// самого декодера.
//
// Именно распаковку SDK разрешает планировать заранее: она не зависит от
// размера и формата, которые хост попросит потом. Пересчёт в буфер хоста — уже
// в aiGetFrame, потому что вот он от формата зависит целиком.
//
// Работник один, и это не лень: у декодера одна позиция в файле, и второй
// поток потребовал бы второго декодера со своим дескриптором и своим кэшем.
// Выигрыш здесь не в параллельной распаковке, а в том, что она идёт, пока
// хост занят своим.
void WorkerLoop(AsyncState* s)
{
    try {
    for (;;) {
        int64_t frame = -1;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->wake.wait(lock, [s] { return s->stopping || !s->queue.empty(); });
            if (s->stopping) return;

            frame = s->queue.front();
            s->queue.pop_front();
            s->busyWith = frame;
        }

        const bool ok = s->decoder.PrefetchFrame(frame);

        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->ready[frame] = ok;
            s->busyWith = -1;

            // Не даём очереди готовых расти без предела: самые дальние от
            // последнего заказа уже никому не нужны
            while (s->ready.size() > kReadyLimit) {
                s->ready.erase(s->ready.begin());
            }
        }
        s->done.notify_all();
    }
    } catch (const std::exception& e) {
        Log("async worker: exception: %s", e.what());
        std::lock_guard<std::mutex> lock(s->mutex);
        s->stopping = true;
        s->busyWith = -1;
        s->done.notify_all();
        s->wake.notify_all();
    } catch (...) {
        Log("async worker: unknown exception");
        std::lock_guard<std::mutex> lock(s->mutex);
        s->stopping = true;
        s->busyWith = -1;
        s->done.notify_all();
        s->wake.notify_all();
    }
}

// --- селекторы ------------------------------------------------------------

prMALError InitiateAsyncRead(AsyncState* s, aiAsyncRequest* req)
{
    if (!req || s->ticksPerFrame <= 0) return aiUnknownError;

    const int64_t frame = req->inSourceRec.inFrameTime / s->ticksPerFrame;

    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->flushing) return aiNoError;
    if (s->ready.count(frame) || s->busyWith == frame) return aiNoError;

    for (int64_t queued : s->queue) {
        if (queued == frame) return aiNoError;
    }

    s->queue.push_back(frame);
    s->wake.notify_one();
    return aiNoError;
}

prMALError CancelAsyncRead(AsyncState* s, aiAsyncRequest* req)
{
    if (!req || s->ticksPerFrame <= 0) return aiNoError;
    const int64_t frame = req->inSourceRec.inFrameTime / s->ticksPerFrame;

    // Только из очереди: то, что уже распаковывается, обрывать нельзя,
    // да и SDK называет этот вызов подсказкой, а не приказом
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        for (auto it = s->queue.begin(); it != s->queue.end(); ++it) {
            if (*it == frame) { s->queue.erase(it); break; }
        }
    }
    // Будим тех, кто ждал именно этот кадр: делать его больше некому
    s->done.notify_all();
    return aiNoError;
}

prMALError Flush(AsyncState* s)
{
    std::unique_lock<std::mutex> lock(s->mutex);
    s->flushing = true;
    s->queue.clear();

    // Ждём именно текущий кадр: обрывать распаковку на полпути нечем, а SDK
    // разрешает здесь заблокироваться — «until all executing requests are
    // completed or abandoned»
    s->done.wait(lock, [s] { return s->busyWith < 0; });

    s->ready.clear();
    s->flushing = false;
    lock.unlock();

    // Очередь только что опустела: все, кто ждал своих кадров, должны
    // проснуться и пойти распаковывать сами
    s->done.notify_all();
    return aiNoError;
}

// Отдать кадр хосту. Здесь и только здесь известен нужный размер и формат.
prMALError GetFrame(AsyncState* s, imSourceVideoRec* videoRec)
{
    if (!videoRec || !s->PPixCreatorSuite || !s->PPixSuite) return aiUnknownError;
    if (s->ticksPerFrame <= 0) return aiUnknownError;

    if (!videoRec->inFrameFormats || videoRec->inNumFrameFormats < 1) {
        return aiUnknownError;
    }

    const int64_t frame = videoRec->inFrameTime / s->ticksPerFrame;

    // Если кадр уже заказан и ещё в работе — подождём его: это дешевле, чем
    // распаковывать заново.
    //
    // Ждать можно только пока кто-то действительно собирается его сделать.
    // Первая версия ждала «пока не появится в готовых» — и засыпала навсегда,
    // если заказ успевали отменить или слить: тогда его никто уже не сделает,
    // а разбудить спящего некому. Нашёл это стресс-сценарий в поддельном хосте,
    // ровно за тем и написанный.
    {
        auto queued = [s, frame] {
            if (s->busyWith == frame) return true;
            for (int64_t q : s->queue) if (q == frame) return true;
            return false;
        };

        std::unique_lock<std::mutex> lock(s->mutex);
        s->done.wait(lock, [&] {
            return s->stopping || s->ready.count(frame) != 0 || !queued();
        });
        if (s->stopping) return aiUnknownError;
    }
    // Дальше в любом случае идём в декодер: он либо возьмёт готовый кадр
    // из кэша, либо распакует сам. Второе медленнее, но всегда верно.

    // Выбор формата и качества — теми же функциями, что и у обычного пути.
    int pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                       videoRec->inNumFrameFormats);

    {
        imFrameFormat* offered = &videoRec->inFrameFormats[pick >= 0 ? pick : 0];
        const int askedW = (offered->inFrameWidth  > 0) ? offered->inFrameWidth  : s->width;
        const int askedH = (offered->inFrameHeight > 0) ? offered->inFrameHeight : s->height;
        const PrPixelFormat askedFmt =
            (pick >= 0) ? offered->inPixelFormat : PrPixelFormat_BGRA_4444_8u;
        NoteHostVideoRequest((csSDK_int32)frame, s->width, s->height,
                             askedW, askedH, askedFmt, videoRec->inQuality);
    }

    s->decoder.SetScaling(av1imp::ScalingFor(videoRec->inQuality));

    for (;;) {
        imFrameFormat* offered = &videoRec->inFrameFormats[pick >= 0 ? pick : 0];
        const int frameW = (offered->inFrameWidth  > 0) ? offered->inFrameWidth  : s->width;
        const int frameH = (offered->inFrameHeight > 0) ? offered->inFrameHeight : s->height;

        const PrPixelFormat pixelFormat =
            (pick >= 0) ? offered->inPixelFormat : PrPixelFormat_BGRA_4444_8u;

        prMALError result = av1imp::CreateVideoPPix(
            s->PPixCreatorSuite, s->PPixCreator2Suite,
            s->PPixSuite, s->PPix2Suite,
            videoRec->outFrame, pixelFormat, frameW, frameH);
        if (result != malNoError) {
            if (av1imp::IsNativeP010(pixelFormat)) {
                av1imp::MarkP010Unavailable();
                av1imp::DiscardHostFrame(s->PPixSuite, videoRec->outFrame);
                pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                               videoRec->inNumFrameFormats,
                                               pick + 1);
                if (pick >= 0) continue;
            }
            return aiUnknownError;
        }

        const char* why = nullptr;
        if (!av1imp::WriteFrameToBuffer(s->decoder, frame,
                                        s->PPixSuite, s->PPix2Suite,
                                        *videoRec->outFrame, pixelFormat,
                                        frameW, frameH, &why)) {
            const bool p010NoBuf = av1imp::IsNativeP010(pixelFormat) && why &&
                (strstr(why, "no buffer") != nullptr ||
                 strstr(why, "buffer too small") != nullptr);
            av1imp::DiscardHostFrame(s->PPixSuite, videoRec->outFrame);
            if (p010NoBuf) {
                av1imp::MarkP010Unavailable();
                pick = av1imp::PickFrameFormat(videoRec->inFrameFormats,
                                               videoRec->inNumFrameFormats,
                                               pick + 1);
                if (pick >= 0) continue;
            }
            Log("async frame %lld: FAILED - %s", (long long)frame,
                why ? why : s->decoder.LastError().c_str());
            return aiFrameNotFound;
        }

        if (frame < 3) {
            Log("async frame %lld delivered (%dx%d, %s)", (long long)frame,
                frameW, frameH,
                av1imp::IsNativeP010(pixelFormat) ? "10u planes (P010)"
                : av1imp::IsNativeYUV(pixelFormat) ? "planar YUV"
                : (pixelFormat == PrPixelFormat_BGRA_4444_16u ? "BGRA 16u" : "BGRA 8u"));
        }

        std::lock_guard<std::mutex> lock(s->mutex);
        s->ready.erase(frame);
        return aiNoError;
    }
}

prMALError Close(AsyncState* s)
{
    if (!s) return aiUnknownError;

    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->stopping = true;
        s->wake.notify_all();
        s->done.notify_all();

        // Ждём, пока из нас выйдут все, кто уже внутри. После этого новых
        // не будет: CallGuard не пускает, когда stopping уже поднят.
        s->idle.wait(lock, [s] { return s->inFlight == 0; });
    }

    if (s->worker.joinable()) s->worker.join();

    // Наборы отдаём после того, как работник встал и все вызовы вышли:
    // пока внутри нас кто-то есть, он может ими пользоваться.
    if (s->BasicSuite) {
        if (s->PPixCreatorSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, s->PPixCreatorSuiteVersion);
        }
        if (s->PPixCreator2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreator2Suite, s->PPixCreator2SuiteVersion);
        }
        if (s->PPixSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, s->PPixSuiteVersion);
        }
        if (s->PPix2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPix2Suite, s->PPix2SuiteVersion);
        }
    }

    Log("async: closed");
    delete s;
    return aiNoError;
}

// Единственная точка входа асинхронного импортёра. Реентерабельна вся, кроме
// aiClose, — так сказано в SDK, и состояние под замком именно поэтому.
PREMPLUGENTRY AsyncEntry(int selector, void* param)
{
    try {
    switch (selector) {
        case aiInitiateAsyncRead: {
            aiAsyncRequest* req = reinterpret_cast<aiAsyncRequest*>(param);
            if (!req) return aiUnknownError;
            AsyncState* s = reinterpret_cast<AsyncState*>(req->inPrivateData);
            CallGuard guard(s);
            if (!guard.entered()) return aiUnknownError;
            return InitiateAsyncRead(s, req);
        }

        case aiCancelAsyncRead: {
            aiAsyncRequest* req = reinterpret_cast<aiAsyncRequest*>(param);
            if (!req) return aiNoError;
            AsyncState* s = reinterpret_cast<AsyncState*>(req->inPrivateData);
            CallGuard guard(s);
            if (!guard.entered()) return aiNoError;
            return CancelAsyncRead(s, req);
        }

        case aiFlush: {
            AsyncState* s = reinterpret_cast<AsyncState*>(param);
            CallGuard guard(s);
            if (!guard.entered()) return aiNoError;
            return Flush(s);
        }

        case aiGetFrame: {
            imSourceVideoRec* videoRec = reinterpret_cast<imSourceVideoRec*>(param);
            if (!videoRec) return aiUnknownError;
            AsyncState* s = reinterpret_cast<AsyncState*>(videoRec->inPrivateData);
            CallGuard guard(s);
            if (!guard.entered()) return aiUnknownError;
            return GetFrame(s, videoRec);
        }

        case aiClose:
            return Close(reinterpret_cast<AsyncState*>(param));

        default:
            // Остальное необязательно: подсказки про удобные точки перемотки
            // и про ожидание. Отказ здесь ничего не ломает.
            return aiUnsupported;
    }
    } catch (const std::exception& e) {
        Log("async selector %d: exception: %s", selector, e.what());
        return selector == aiClose ? aiNoError : aiUnknownError;
    } catch (...) {
        Log("async selector %d: unknown exception", selector);
        return selector == aiClose ? aiNoError : aiUnknownError;
    }
}

} // namespace

bool AsyncEnabled()
{
    return AsyncDeliveryEnabled();
}

bool CreateAsyncImporter(ImporterLocalRecPtr source, imAsyncImporterCreationRec* rec)
{
    if (!source || !rec || !source->decoder) return false;

    // Выключатель проверяем и здесь, а не только там, где объявляем поддержку:
    // хост может позвать это и без объявления, а «выключено» должно означать
    // выключено, а не «обычно не спрашивают»
    if (!AsyncEnabled()) return false;

    const MediaInfo& mi = source->decoder->Info();
    if (!mi.hasVideo || source->ticksPerFrame <= 0) return false;

    AsyncState* s = new (std::nothrow) AsyncState();
    if (!s) return false;

    // Путь берём из своей копии: обычный импортёр может закрыться раньше нас
    const int need = WideCharToMultiByte(CP_UTF8, 0,
                                         reinterpret_cast<LPCWSTR>(source->filePath), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (need > 1) {
        s->path.resize(static_cast<size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(source->filePath), -1,
                            &s->path[0], need, nullptr, nullptr);
    }
    if (s->path.empty()) { delete s; return false; }

    // Свой декодер и свой дескриптор файла — см. комментарий в заголовке
    if (!s->decoder.Open(s->path, PreferHardware(), /*needVideo=*/true)) {
        Log("async: cannot open the file separately - %s", s->decoder.LastError().c_str());
        delete s;
        return false;
    }

    // Кэш ему нужен ровно под заказанные вперёд кадры, с небольшим запасом:
    // иначе второй декодер на клип удваивал бы расход памяти впустую
    s->decoder.LimitCacheToFrames(static_cast<int>(kReadyLimit) * 2);

    // Наборы берём себе. Без них выдавать кадры нечем, поэтому отказ здесь —
    // отказ создать асинхронный импортёр вовсе: хост просто останется
    // на обычном пути, и ничего страшного не случится.
    s->BasicSuite = source->BasicSuite;
    if (!s->BasicSuite) {
        Log("async: no basic suite to take our own references from");
        delete s;
        return false;
    }

    s->PPixCreatorSuiteVersion =
        (s->BasicSuite->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion,
                                     (const void**)&s->PPixCreatorSuite) == kSPNoError
         && s->PPixCreatorSuite) ? kPrSDKPPixCreatorSuiteVersion : 0;

    s->PPixCreator2SuiteVersion =
        AcquireNewestSuite(s->BasicSuite, kPrSDKPPixCreator2Suite,
                           kPrSDKPPixCreator2SuiteVersion, &s->PPixCreator2Suite);

    s->PPixSuiteVersion =
        (s->BasicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                                     (const void**)&s->PPixSuite) == kSPNoError
         && s->PPixSuite) ? kPrSDKPPixSuiteVersion : 0;

    // Плоскости — только со второй версии, как и у обычного пути. Первая
    // версия не беда: тогда кадры просто поедут через BGRA.
    s->PPix2SuiteVersion =
        AcquireNewestSuite(s->BasicSuite, kPrSDKPPix2Suite,
                           kPrSDKPPix2SuiteVersion, &s->PPix2Suite);
    if (s->PPix2SuiteVersion < kPrSDKPPix2SuiteVersion2) {
        s->PPix2Suite = nullptr;   // писать в плоскости всё равно нечем
    }

    if (!s->PPixCreatorSuiteVersion || !s->PPixSuiteVersion) {
        Log("async: host refused a suite (creator %d, ppix %d) - staying on the plain path",
            s->PPixCreatorSuiteVersion, s->PPixSuiteVersion);
        if (s->PPixCreatorSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, s->PPixCreatorSuiteVersion);
        }
        if (s->PPixCreator2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreator2Suite, s->PPixCreator2SuiteVersion);
        }
        if (s->PPixSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, s->PPixSuiteVersion);
        }
        if (s->PPix2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPix2Suite, s->PPix2SuiteVersion);
        }
        delete s;
        return false;
    }

    s->width            = mi.width;
    s->height           = mi.height;
    s->ticksPerFrame    = source->ticksPerFrame;

    try {
        s->worker = std::thread(WorkerLoop, s);
    } catch (const std::exception& e) {
        Log("async: cannot start worker - %s", e.what());
        if (s->PPixCreatorSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, s->PPixCreatorSuiteVersion);
        }
        if (s->PPixCreator2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixCreator2Suite, s->PPixCreator2SuiteVersion);
        }
        if (s->PPixSuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, s->PPixSuiteVersion);
        }
        if (s->PPix2SuiteVersion) {
            s->BasicSuite->ReleaseSuite(kPrSDKPPix2Suite, s->PPix2SuiteVersion);
        }
        delete s;
        return false;
    }

    rec->outAsyncEntry       = AsyncEntry;
    rec->outAsyncPrivateData = s;

    Log("async: created, %dx%d", s->width, s->height);
    return true;
}

} // namespace av1imp
