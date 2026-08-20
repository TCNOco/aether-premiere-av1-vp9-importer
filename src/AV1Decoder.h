// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

// Слой декодирования AV1 поверх ffmpeg.
//
// Намеренно ничего не знает про Adobe SDK: его можно собрать и проверить обычной
// консольной программой (см. tools/decoder_test.cpp), а уже потом подключить к плагину.
// Так отлаживается 90% логики без запуска Premiere.

#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <vector>
#include <map>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;
struct SwrContext;

namespace av1imp {

// Что удалось узнать о файле после открытия
struct MediaInfo {
    int      width          = 0;
    int      height         = 0;
    double   fps            = 0.0;   // кадров в секунду
    int      fpsNum         = 0;     // та же частота точной дробью: fpsNum/fpsDen.
    int      fpsDen         = 0;     // Premiere считает время в них, и 60000/1001 округлять нельзя
    int64_t  frameCount     = 0;     // может быть оценкой, если в контейнере нет точного числа
    double   durationSec    = 0.0;
    bool     hardwareDecode = false; // декодирует видеокарта, а не процессор
    std::string codecName;
    std::string decoderName;

    // Звук. Дорожек в файле может быть несколько: OBS пишет микрофон, игру,
    // дискорд и музыку отдельными потоками, и сводить их в одну нельзя.
    int      audioStreamCount = 0;
    int      audioChannels    = 0;
    int      audioSampleRate  = 0;
    int64_t  audioSampleCount = 0;   // длина в кадрах отсчётов (не в байтах)
};

class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Открыть файл. Путь — UTF-8. Возвращает false, если файл не открылся
    // или кодек не из списка поддерживаемых (импортёр не должен перехватывать
    // форматы, которые Premiere открывает сам).
    //
    // needVideo=false открывает только разбор контейнера, без декодера кадров.
    // Так работают экземпляры, обслуживающие дорожки звука: файл проверяется
    // на AV1 по-прежнему, но лишний декодер на видеокарте не создаётся —
    // иначе на четырёхдорожечной записи их было бы четыре вместо одного.
    bool Open(const std::string& utf8Path, bool preferHardware = true,
              bool needVideo = true);

    // Закрыть всё. Можно звать из любого потока: Premiere закрывает файл
    // не дожидаясь, пока другие его потоки договорят с декодером.
    void Close();

    bool IsOpen() const { return fmt_ != nullptr; }
    const MediaInfo& Info() const { return info_; }

    // Выдать кадр по индексу в буфер вызывающего.
    // Формат — BGRA, 8 бит на канал, строка dstStride байт.
    //
    // dstWidth/dstHeight — размер БУФЕРА, а не файла. Premiere при пониженном
    // качестве воспроизведения просит уменьшенный кадр, и писать туда полное
    // разрешение — выход за пределы буфера. Кадр масштабируется под запрос.
    //
    // Отрицательный dstStride переворачивает кадр по вертикали — так его ждёт
    // Premiere, у которого у 32-битных буферов начало координат внизу слева.
    // Возвращает false, если кадр получить не удалось.
    bool GetFrameBGRA(int64_t frameIndex, uint8_t* dst, int dstStride,
                      int dstWidth, int dstHeight);

    // Открыть дорожку звука по её номеру среди звуковых (0 — первая).
    // Отдельно от видео: у звука свой разбор контейнера, иначе перемотка
    // видео сбивала бы позицию звука и наоборот.
    bool OpenAudio(int ordinal);
    bool HasAudio() const { return audioCodec_ != nullptr; }

    // Выдать отсчёты в буферы вызывающего: dst[канал][отсчёт], 32-битные float.
    // Именно в таком виде их ждёт Premiere.
    bool GetAudio(int64_t startSample, int32_t sampleCount, float* const* dst);

    // Где на самом деле уходит время. Нужно, чтобы оптимизировать по замерам,
    // а не по догадкам: декодирует видеокарта, и узким местом оказывается
    // не она, а перенос кадра в обычную память и пересчёт цвета.
    struct Stats {
        double  decodeMs   = 0.0;   // разбор и декодирование
        double  transferMs = 0.0;   // перенос из памяти видеокарты
        double  convertMs  = 0.0;   // пересчёт в BGRA
        int64_t frames     = 0;
    };
    const Stats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Stats(); }

    // Текст последней ошибки — для журнала плагина
    const std::string& LastError() const { return lastError_; }

private:
    bool OpenCodec(bool preferHardware);
    bool DecodeMoreAudio();

    // Варианты без захвата замка — для вызова изнутри, когда он уже взят
    void CloseLocked();
    void CloseAudioLocked();
    bool DecodeUntil(int64_t targetFrame);
    bool ConvertToBGRA(AVFrame* src, uint8_t* dst, int dstStride, int dstW, int dstH);
    void StoreInCache(int64_t index, AVFrame* src);
    void ClearCache();
    void SetError(const std::string& msg, int averr = 0);

    AVFormatContext* fmt_        = nullptr;
    AVCodecContext*  codec_      = nullptr;
    AVFrame*         frame_      = nullptr;  // кадр из декодера (может быть в памяти видеокарты)
    AVFrame*         swFrame_    = nullptr;  // копия в обычной памяти, если декодировала видеокарта
    AVPacket*        packet_     = nullptr;
    SwsContext*      sws_        = nullptr;
    int              swsSrcW_    = 0;   // с какими параметрами создан пересчётчик
    int              swsSrcH_    = 0;
    int              swsSrcFmt_  = -1;
    int              swsDstW_    = 0;
    int              swsDstH_    = 0;
    AVBufferRef*     hwDevice_   = nullptr;

    int      videoStream_ = -1;
    int      codecId_     = 0;   // AVCodecID открытого потока; таблица — в .cpp
    MediaInfo info_;
    std::string lastError_;

    // Кэш позиции: Premiere почти всегда читает кадры подряд, и без этого
    // на каждый кадр приходилось бы перематывать файл с начала.
    int64_t  lastDecodedFrame_ = -1;
    bool     eofReached_       = false;

    // Кэш кадров. Заполняется только при отматывании назад: там мы всё равно
    // декодируем весь отрезок от опорного кадра, и без кэша выбрасывали бы
    // ровно те кадры, которые попросят следующим шагом.
    std::map<int64_t, AVFrame*> frameCache_;
    size_t   cacheBytes_    = 0;
    size_t   cacheBudget_   = 0;    // считается от разрешения, см. Open()
    bool     cacheFill_     = false;
    int64_t  lastRequested_ = -1;

    // --- звук ---
    AVFormatContext* audioFmt_   = nullptr;  // свой разбор контейнера, см. OpenAudio
    AVCodecContext*  audioCodec_ = nullptr;
    AVFrame*         audioFrame_ = nullptr;
    AVPacket*        audioPacket_ = nullptr;
    SwrContext*      swr_        = nullptr;
    int              audioStreamIndex_ = -1;

    // Готовые отсчёты, оставшиеся от предыдущего запроса: Premiere просит
    // произвольные куски, а декодер выдаёт кадрами по ~1024 отсчёта
    std::vector<std::vector<float>> pending_;
    int64_t pendingOffset_ = 0;
    int64_t pendingCount_  = 0;
    int64_t audioCursor_   = -1;   // номер следующего готового отсчёта

    // Замки раздельные, и это важно. Видео и звук разбираются полностью
    // независимо (у каждого свой контекст контейнера), а Premiere читает их
    // одновременно: кадры для показа и звук для конформирования. Один общий
    // замок ставил бы звук в очередь за непрерывным потоком кадров.
    // Порядок захвата, когда нужны оба: сначала видео, потом звук.
    Stats stats_;

    std::mutex mutex_;
    std::mutex audioMutex_;
};

} // namespace av1imp
