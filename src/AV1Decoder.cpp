#include "AV1Decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace av1imp {

namespace {

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Порядок предпочтения декодеров. Аппаратный первым: на видеокарте AV1 1440p60
// идёт в разы быстрее, а именно от этого зависит, можно ли монтировать без прокси.
const char* kHardwareDecoders[] = { "av1_cuvid", "av1_qsv", "av1_amf" };
const char* kSoftwareDecoders[] = { "libdav1d", "av1" };

} // namespace

Decoder::~Decoder() {
    Close();
}

void Decoder::SetError(const std::string& msg, int averr) {
    lastError_ = averr ? msg + ": " + AvErr(averr) : msg;
}

bool Decoder::Open(const std::string& utf8Path, bool preferHardware) {
    std::lock_guard<std::mutex> lock(mutex_);
    Close();

    int err = avformat_open_input(&fmt_, utf8Path.c_str(), nullptr, nullptr);
    if (err < 0) {
        SetError("не удалось открыть файл", err);
        return false;
    }

    err = avformat_find_stream_info(fmt_, nullptr);
    if (err < 0) {
        SetError("не удалось прочитать сведения о потоках", err);
        Close();
        return false;
    }

    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream_ < 0) {
        SetError("в файле нет видеопотока");
        Close();
        return false;
    }

    AVStream* st = fmt_->streams[videoStream_];

    // Плагин отвечает только за AV1. Всё остальное отдаём штатным импортёрам Premiere —
    // иначе перехватим форматы, которые он и сам прекрасно открывает.
    if (st->codecpar->codec_id != AV_CODEC_ID_AV1) {
        SetError("видеопоток не AV1");
        Close();
        return false;
    }

    if (!OpenCodec(preferHardware)) {
        Close();
        return false;
    }

    info_.width  = st->codecpar->width;
    info_.height = st->codecpar->height;

    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    info_.fps    = (fr.num && fr.den) ? av_q2d(fr) : 0.0;
    info_.fpsNum = fr.num;
    info_.fpsDen = fr.den;

    if (st->duration != AV_NOPTS_VALUE) {
        info_.durationSec = st->duration * av_q2d(st->time_base);
    } else if (fmt_->duration != AV_NOPTS_VALUE) {
        info_.durationSec = fmt_->duration / (double)AV_TIME_BASE;
    }

    // nb_frames заполнен не всегда (например, во фрагментированном MP4) —
    // тогда считаем по длительности и частоте кадров.
    info_.frameCount = st->nb_frames > 0
        ? st->nb_frames
        : (info_.fps > 0 ? (int64_t)(info_.durationSec * info_.fps + 0.5) : 0);

    info_.codecName = "AV1";

    frame_   = av_frame_alloc();
    swFrame_ = av_frame_alloc();
    packet_  = av_packet_alloc();
    if (!frame_ || !swFrame_ || !packet_) {
        SetError("не хватило памяти под буферы кадров");
        Close();
        return false;
    }

    lastDecodedFrame_ = -1;
    eofReached_ = false;
    return true;
}

bool Decoder::OpenCodec(bool preferHardware) {
    AVStream* st = fmt_->streams[videoStream_];

    auto tryDecoder = [&](const char* name, bool hardware) -> bool {
        const AVCodec* dec = avcodec_find_decoder_by_name(name);
        if (!dec) return false;

        AVCodecContext* ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;

        if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        // Декодирование в несколько потоков — заметно ускоряет программный путь
        ctx->thread_count = 0;  // 0 = по числу ядер

        if (hardware) {
            AVBufferRef* hw = nullptr;
            // CUDA для NVIDIA; для других вендоров ffmpeg подставит своё через имя декодера
            if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
                ctx->hw_device_ctx = av_buffer_ref(hw);
                av_buffer_unref(&hw);
            }
        }

        if (avcodec_open2(ctx, dec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        codec_ = ctx;
        info_.decoderName    = name;
        info_.hardwareDecode = hardware;
        return true;
    };

    if (preferHardware) {
        for (const char* name : kHardwareDecoders) {
            if (tryDecoder(name, true)) return true;
        }
    }
    for (const char* name : kSoftwareDecoders) {
        if (tryDecoder(name, false)) return true;
    }

    SetError("не нашёлся ни один декодер AV1");
    return false;
}

void Decoder::Close() {
    if (sws_)      { sws_freeContext(sws_); sws_ = nullptr; }
    if (packet_)   { av_packet_free(&packet_); }
    if (frame_)    { av_frame_free(&frame_); }
    if (swFrame_)  { av_frame_free(&swFrame_); }
    if (codec_)    { avcodec_free_context(&codec_); }
    if (hwDevice_) { av_buffer_unref(&hwDevice_); }
    if (fmt_)      { avformat_close_input(&fmt_); }

    videoStream_      = -1;
    lastDecodedFrame_ = -1;
    eofReached_       = false;
    info_ = MediaInfo{};
}

bool Decoder::DecodeUntil(int64_t targetFrame) {
    AVStream* st = fmt_->streams[videoStream_];

    // Назад или далеко вперёд — перематываем. Вперёд на несколько кадров дешевле
    // просто домотать декодированием: перемотка всегда идёт до ключевого кадра,
    // и на длинном интервале между ними обходится дороже.
    const int64_t kForwardScanLimit = 64;
    bool needSeek = (targetFrame <= lastDecodedFrame_) ||
                    (targetFrame - lastDecodedFrame_ > kForwardScanLimit) ||
                    (lastDecodedFrame_ < 0);

    if (needSeek) {
        int64_t ts = info_.fps > 0
            ? (int64_t)((targetFrame / info_.fps) / av_q2d(st->time_base))
            : 0;
        if (av_seek_frame(fmt_, videoStream_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            SetError("перемотка не удалась");
            return false;
        }
        avcodec_flush_buffers(codec_);
        lastDecodedFrame_ = -1;
        eofReached_ = false;
    }

    while (true) {
        int err = avcodec_receive_frame(codec_, frame_);

        if (err == 0) {
            // Номер кадра считаем по метке времени: best_effort_timestamp
            // устойчивее к файлам без счётчика кадров.
            int64_t pts = frame_->best_effort_timestamp;
            int64_t idx = lastDecodedFrame_ + 1;
            if (pts != AV_NOPTS_VALUE && info_.fps > 0) {
                idx = (int64_t)(pts * av_q2d(st->time_base) * info_.fps + 0.5);
            }
            lastDecodedFrame_ = idx;

            if (idx >= targetFrame) return true;
            continue;  // ещё не дошли — декодируем дальше
        }

        if (err == AVERROR_EOF) {
            eofReached_ = true;
            // Последний кадр всё ещё в frame_ — отдаём его, это лучше чёрного экрана
            return lastDecodedFrame_ >= 0;
        }

        if (err != AVERROR(EAGAIN)) {
            SetError("ошибка декодирования", err);
            return false;
        }

        // Декодеру нужны новые данные
        av_packet_unref(packet_);
        int rerr = av_read_frame(fmt_, packet_);
        if (rerr == AVERROR_EOF) {
            avcodec_send_packet(codec_, nullptr);  // слить остаток
            continue;
        }
        if (rerr < 0) {
            SetError("ошибка чтения файла", rerr);
            return false;
        }
        if (packet_->stream_index != videoStream_) continue;

        if (avcodec_send_packet(codec_, packet_) < 0) {
            SetError("декодер не принял пакет");
            return false;
        }
    }
}

bool Decoder::ConvertToBGRA(AVFrame* src, uint8_t* dst, int dstStride) {
    AVFrame* srcFrame = src;

    // Если декодировала видеокарта, кадр лежит в её памяти — забираем в обычную
    if (src->hw_frames_ctx) {
        av_frame_unref(swFrame_);
        if (av_hwframe_transfer_data(swFrame_, src, 0) < 0) {
            SetError("не удалось забрать кадр из памяти видеокарты");
            return false;
        }
        srcFrame = swFrame_;
    }

    sws_ = sws_getCachedContext(sws_,
                                srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
                                info_.width, info_.height, AV_PIX_FMT_BGRA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        SetError("не удалось создать преобразователь цвета");
        return false;
    }

    uint8_t* dstData[4] = { dst, nullptr, nullptr, nullptr };
    int dstLinesize[4]  = { dstStride, 0, 0, 0 };

    sws_scale(sws_, srcFrame->data, srcFrame->linesize, 0, srcFrame->height, dstData, dstLinesize);
    return true;
}

bool Decoder::GetFrameBGRA(int64_t frameIndex, uint8_t* dst, int dstStride) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsOpen() || !dst) {
        SetError("файл не открыт");
        return false;
    }
    if (frameIndex < 0) frameIndex = 0;

    if (!DecodeUntil(frameIndex)) return false;
    return ConvertToBGRA(frame_, dst, dstStride);
}

} // namespace av1imp
