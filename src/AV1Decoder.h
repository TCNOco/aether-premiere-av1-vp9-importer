// Слой декодирования AV1 поверх ffmpeg.
//
// Намеренно ничего не знает про Adobe SDK: его можно собрать и проверить обычной
// консольной программой (см. tools/decoder_test.cpp), а уже потом подключить к плагину.
// Так отлаживается 90% логики без запуска Premiere.

#pragma once

#include <cstdint>
#include <string>
#include <mutex>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace av1imp {

// Что удалось узнать о файле после открытия
struct MediaInfo {
    int      width          = 0;
    int      height         = 0;
    double   fps            = 0.0;   // кадров в секунду
    int64_t  frameCount     = 0;     // может быть оценкой, если в контейнере нет точного числа
    double   durationSec    = 0.0;
    bool     hardwareDecode = false; // декодирует видеокарта, а не процессор
    std::string codecName;
    std::string decoderName;
};

class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Открыть файл. Путь — UTF-8. Возвращает false, если файл не открылся
    // или видеопоток не AV1 (импортёр не должен перехватывать чужие форматы).
    bool Open(const std::string& utf8Path, bool preferHardware = true);
    void Close();

    bool IsOpen() const { return fmt_ != nullptr; }
    const MediaInfo& Info() const { return info_; }

    // Выдать кадр по индексу в буфер вызывающего.
    // Формат — BGRA, 8 бит на канал, строка dstStride байт, начало кадра сверху.
    // Возвращает false, если кадр получить не удалось.
    bool GetFrameBGRA(int64_t frameIndex, uint8_t* dst, int dstStride);

    // Текст последней ошибки — для журнала плагина
    const std::string& LastError() const { return lastError_; }

private:
    bool OpenCodec(bool preferHardware);
    bool DecodeUntil(int64_t targetFrame);
    bool ConvertToBGRA(AVFrame* src, uint8_t* dst, int dstStride);
    void SetError(const std::string& msg, int averr = 0);

    AVFormatContext* fmt_        = nullptr;
    AVCodecContext*  codec_      = nullptr;
    AVFrame*         frame_      = nullptr;  // кадр из декодера (может быть в памяти видеокарты)
    AVFrame*         swFrame_    = nullptr;  // копия в обычной памяти, если декодировала видеокарта
    AVPacket*        packet_     = nullptr;
    SwsContext*      sws_        = nullptr;
    AVBufferRef*     hwDevice_   = nullptr;

    int      videoStream_ = -1;
    MediaInfo info_;
    std::string lastError_;

    // Кэш позиции: Premiere почти всегда читает кадры подряд, и без этого
    // на каждый кадр приходилось бы перематывать файл с начала.
    int64_t  lastDecodedFrame_ = -1;
    bool     eofReached_       = false;

    std::mutex mutex_;  // Premiere дёргает импортёр из нескольких потоков
};

} // namespace av1imp
