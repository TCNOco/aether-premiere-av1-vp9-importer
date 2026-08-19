#include "AV1Decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
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

bool Decoder::Open(const std::string& utf8Path, bool preferHardware, bool needVideo) {
    std::scoped_lock lock(mutex_, audioMutex_);
    CloseLocked();

    int err = avformat_open_input(&fmt_, utf8Path.c_str(), nullptr, nullptr);
    if (err < 0) {
        SetError("не удалось открыть файл", err);
        return false;
    }

    err = avformat_find_stream_info(fmt_, nullptr);
    if (err < 0) {
        SetError("не удалось прочитать сведения о потоках", err);
        CloseLocked();
        return false;
    }

    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream_ < 0) {
        SetError("в файле нет видеопотока");
        CloseLocked();
        return false;
    }

    AVStream* st = fmt_->streams[videoStream_];

    // Плагин отвечает только за AV1. Всё остальное отдаём штатным импортёрам Premiere —
    // иначе перехватим форматы, которые он и сам прекрасно открывает.
    if (st->codecpar->codec_id != AV_CODEC_ID_AV1) {
        SetError("видеопоток не AV1");
        CloseLocked();
        return false;
    }

    // Дорожкам звука декодер кадров не нужен — проверки на AV1 выше достаточно
    if (needVideo && !OpenCodec(preferHardware)) {
        CloseLocked();
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

    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++info_.audioStreamCount;
        }
    }

    frame_   = av_frame_alloc();
    swFrame_ = av_frame_alloc();
    packet_  = av_packet_alloc();
    if (!frame_ || !swFrame_ || !packet_) {
        SetError("не хватило памяти под буферы кадров");
        CloseLocked();
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
    std::scoped_lock lock(mutex_, audioMutex_);
    CloseLocked();
}

void Decoder::ClearCache() {
    for (auto& item : frameCache_) {
        av_frame_free(&item.second);
    }
    frameCache_.clear();
    cacheBytes_ = 0;
}

// Сохранить кадр в кэше. Кадр с видеокарты сначала переносится в обычную память:
// держать полсотни кадров в памяти видеокарты нельзя, её быстро не хватит.
void Decoder::StoreInCache(int64_t index, AVFrame* src) {
    if (frameCache_.count(index)) return;

    AVFrame* copy = av_frame_alloc();
    if (!copy) return;

    if (src->hw_frames_ctx) {
        if (av_hwframe_transfer_data(copy, src, 0) < 0) {
            av_frame_free(&copy);
            return;
        }
    } else if (av_frame_ref(copy, src) < 0) {
        av_frame_free(&copy);
        return;
    }

    const int size = av_image_get_buffer_size((AVPixelFormat)copy->format,
                                              copy->width, copy->height, 1);
    if (size <= 0) {
        av_frame_free(&copy);
        return;
    }

    // Тесним самые дальние от текущей позиции: при отматывании назад
    // пригодятся ближайшие
    while (cacheBytes_ + size > cacheBudget_ && !frameCache_.empty()) {
        auto first = frameCache_.begin();
        auto last  = std::prev(frameCache_.end());
        auto drop  = (index - first->first) >= (last->first - index) ? first : last;

        const int dropSize = av_image_get_buffer_size((AVPixelFormat)drop->second->format,
                                                      drop->second->width,
                                                      drop->second->height, 1);
        cacheBytes_ -= (dropSize > 0 ? dropSize : 0);
        av_frame_free(&drop->second);
        frameCache_.erase(drop);
    }

    frameCache_[index] = copy;
    cacheBytes_ += size;
}

void Decoder::CloseLocked() {
    ClearCache();
    CloseAudioLocked();
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

            if (cacheFill_) StoreInCache(idx, frame_);

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

    // Отматывание назад — единственный случай, когда кэш окупается: при чтении
    // вперёд кадры и так идут подряд, а кэш только тратил бы память
    const bool backward = (lastRequested_ >= 0 && frameIndex < lastRequested_);
    lastRequested_ = frameIndex;

    auto cached = frameCache_.find(frameIndex);
    if (cached != frameCache_.end()) {
        return ConvertToBGRA(cached->second, dst, dstStride);
    }

    cacheFill_ = backward;
    const bool ok = DecodeUntil(frameIndex);
    cacheFill_ = false;

    if (!ok) return false;
    return ConvertToBGRA(frame_, dst, dstStride);
}

// ---------------------------------------------------------------------------
// Звук
// ---------------------------------------------------------------------------

void Decoder::CloseAudioLocked() {
    if (swr_)         { swr_free(&swr_); }
    if (audioPacket_) { av_packet_free(&audioPacket_); }
    if (audioFrame_)  { av_frame_free(&audioFrame_); }
    if (audioCodec_)  { avcodec_free_context(&audioCodec_); }
    if (audioFmt_)    { avformat_close_input(&audioFmt_); }

    audioStreamIndex_ = -1;
    pending_.clear();
    pendingOffset_ = 0;
    pendingCount_  = 0;
    audioCursor_   = -1;
}

bool Decoder::OpenAudio(int ordinal) {
    // Оба замка: читаем путь из состояния видео, а меняем состояние звука
    std::scoped_lock lock(mutex_, audioMutex_);
    CloseAudioLocked();

    if (!fmt_) {
        SetError("файл не открыт");
        return false;
    }

    // Свой разбор контейнера для звука. Общий с видео не годится: чтение
    // пакетов двигает одну общую позицию, и перемотка видео сбивала бы звук.
    int err = avformat_open_input(&audioFmt_, fmt_->url, nullptr, nullptr);
    if (err < 0) {
        SetError("не удалось открыть файл для звука", err);
        return false;
    }
    if ((err = avformat_find_stream_info(audioFmt_, nullptr)) < 0) {
        SetError("не удалось прочитать сведения о звуке", err);
        CloseAudioLocked();
        return false;
    }

    int seen = 0;
    for (unsigned i = 0; i < audioFmt_->nb_streams; ++i) {
        if (audioFmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (seen++ == ordinal) { audioStreamIndex_ = (int)i; break; }
    }
    if (audioStreamIndex_ < 0) {
        SetError("нет звуковой дорожки с таким номером");
        CloseAudioLocked();
        return false;
    }

    AVStream* st = audioFmt_->streams[audioStreamIndex_];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        SetError("нет декодера для этой звуковой дорожки");
        CloseAudioLocked();
        return false;
    }

    audioCodec_ = avcodec_alloc_context3(dec);
    if (!audioCodec_ ||
        avcodec_parameters_to_context(audioCodec_, st->codecpar) < 0 ||
        avcodec_open2(audioCodec_, dec, nullptr) < 0) {
        SetError("не удалось открыть декодер звука");
        CloseAudioLocked();
        return false;
    }

    info_.audioChannels    = audioCodec_->ch_layout.nb_channels;
    info_.audioSampleRate  = audioCodec_->sample_rate;
    info_.audioSampleCount = st->duration != AV_NOPTS_VALUE
        ? (int64_t)(st->duration * av_q2d(st->time_base) * info_.audioSampleRate + 0.5)
        : (int64_t)(info_.durationSec * info_.audioSampleRate + 0.5);

    // Premiere работает только с 32-битными float по каналам, поэтому приводим
    // к этому виду всегда — какой бы формат ни выдал декодер
    AVChannelLayout outLayout;
    av_channel_layout_copy(&outLayout, &audioCodec_->ch_layout);
    err = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLTP, info_.audioSampleRate,
                              &audioCodec_->ch_layout, audioCodec_->sample_fmt,
                              audioCodec_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (err < 0 || swr_init(swr_) < 0) {
        SetError("не удалось настроить преобразование звука", err);
        CloseAudioLocked();
        return false;
    }

    audioFrame_  = av_frame_alloc();
    audioPacket_ = av_packet_alloc();
    if (!audioFrame_ || !audioPacket_) {
        SetError("не хватило памяти под буферы звука");
        CloseAudioLocked();
        return false;
    }

    pending_.assign(info_.audioChannels, std::vector<float>());
    audioCursor_ = -1;
    return true;
}

// Декодировать очередной кадр звука в очередь готовых отсчётов
bool Decoder::DecodeMoreAudio() {
    while (true) {
        int err = avcodec_receive_frame(audioCodec_, audioFrame_);

        if (err == 0) {
            const int n = audioFrame_->nb_samples;
            for (auto& ch : pending_) ch.resize(n);

            float* out[64] = {};
            const int channels = std::min<int>(info_.audioChannels, 64);
            for (int c = 0; c < channels; ++c) out[c] = pending_[c].data();

            const int got = swr_convert(swr_, reinterpret_cast<uint8_t**>(out), n,
                                        const_cast<const uint8_t**>(audioFrame_->data), n);
            if (got < 0) {
                SetError("ошибка преобразования звука", got);
                return false;
            }

            // Позицию берём из метки времени: так не накапливается ошибка
            // после перемотки и не важен порядок пакетов
            if (audioFrame_->pts != AV_NOPTS_VALUE) {
                AVStream* st = audioFmt_->streams[audioStreamIndex_];
                audioCursor_ = (int64_t)(audioFrame_->pts * av_q2d(st->time_base)
                                         * info_.audioSampleRate + 0.5);
            } else if (audioCursor_ < 0) {
                audioCursor_ = 0;
            }

            pendingOffset_ = 0;
            pendingCount_  = got;
            return true;
        }

        if (err == AVERROR_EOF) return false;

        if (err != AVERROR(EAGAIN)) {
            SetError("ошибка декодирования звука", err);
            return false;
        }

        av_packet_unref(audioPacket_);
        int rerr = av_read_frame(audioFmt_, audioPacket_);
        if (rerr == AVERROR_EOF) {
            avcodec_send_packet(audioCodec_, nullptr);  // слить остаток
            continue;
        }
        if (rerr < 0) {
            SetError("ошибка чтения звука", rerr);
            return false;
        }
        if (audioPacket_->stream_index != audioStreamIndex_) continue;

        if (avcodec_send_packet(audioCodec_, audioPacket_) < 0) {
            SetError("декодер звука не принял пакет");
            return false;
        }
    }
}

bool Decoder::GetAudio(int64_t startSample, int32_t sampleCount, float* const* dst) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (!audioCodec_ || !dst) {
        SetError("звук не открыт");
        return false;
    }
    if (startSample < 0) startSample = 0;

    const int channels = info_.audioChannels;

    // Тишина по умолчанию: если файл кончился раньше запроса, Premiere получит
    // нули, а не остатки прошлого куска
    for (int c = 0; c < channels; ++c) {
        std::fill_n(dst[c], sampleCount, 0.0f);
    }

    // Назад или далеко вперёд — перематываем. Небольшой прыжок вперёд дешевле
    // домотать декодированием, как и с видео.
    const int64_t available = audioCursor_ + pendingOffset_;
    const bool needSeek = audioCursor_ < 0 || startSample < available ||
                          startSample > available + info_.audioSampleRate;

    if (needSeek) {
        AVStream* st = audioFmt_->streams[audioStreamIndex_];
        int64_t ts = (int64_t)((double)startSample / info_.audioSampleRate / av_q2d(st->time_base));
        if (av_seek_frame(audioFmt_, audioStreamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            SetError("перемотка звука не удалась");
            return false;
        }
        avcodec_flush_buffers(audioCodec_);
        pendingCount_  = 0;
        pendingOffset_ = 0;
        audioCursor_   = -1;
    }

    int64_t written = 0;
    while (written < sampleCount) {
        if (pendingCount_ <= 0) {
            if (!DecodeMoreAudio()) break;   // файл кончился — остаток останется тишиной
        }

        const int64_t chunkStart = audioCursor_ + pendingOffset_;
        const int64_t want       = startSample + written;

        // Пропускаем всё, что лежит до запрошенной позиции: после перемотки
        // декодер начинает с ближайшего целого кадра, а не с нужного отсчёта
        if (chunkStart + pendingCount_ <= want) {
            pendingCount_ = 0;
            continue;
        }
        if (chunkStart > want) {
            // Провал во времени (бывает в записях с разрывами) — оставляем тишину
            const int64_t gap = std::min<int64_t>(chunkStart - want, sampleCount - written);
            written += gap;
            continue;
        }

        const int64_t skip = want - chunkStart;
        const int64_t take = std::min<int64_t>(pendingCount_ - skip, sampleCount - written);

        for (int c = 0; c < channels; ++c) {
            memcpy(dst[c] + written,
                   pending_[c].data() + pendingOffset_ + skip,
                   (size_t)take * sizeof(float));
        }

        written        += take;
        pendingOffset_ += skip + take;
        pendingCount_  -= skip + take;
    }

    return true;
}

} // namespace av1imp
