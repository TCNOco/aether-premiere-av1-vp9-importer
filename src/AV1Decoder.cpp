// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

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
#include <chrono>
#include <cmath>
#include <cstring>

namespace av1imp {

namespace {

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Какие кодеки берём на себя и чем их распаковывать.
//
// Список намеренно короткий: плагин закрывает ровно те кодеки, которых Premiere
// не умеет сам. Расширять его «на всякий случай» вредно — приоритет у плагина
// выше штатного импортёра, и каждый лишний кодек означает перехват файлов,
// которые Premiere прекрасно открывал и без нас.
//
// Внутри кодека аппаратный декодер идёт первым: на видеокарте 1440p60 быстрее
// в разы, а от этого зависит, можно ли монтировать без прокси. Тип устройства
// обязан соответствовать декодеру — иначе на машине без нужного железа контекст
// просто не создаётся, и декодер открывается пустым.
struct HardwareDecoder {
    const char*        name;
    AVHWDeviceType     device;
};

struct SupportedCodec {
    AVCodecID              id;
    const char*            name;          // как показываем в сведениях и журнале
    const HardwareDecoder* hardware;      // список, конец — name == nullptr
    const char* const*     software;      // список, конец — nullptr
};

const HardwareDecoder kAV1Hardware[] = {
    { "av1_cuvid", AV_HWDEVICE_TYPE_CUDA    },
    { "av1_qsv",   AV_HWDEVICE_TYPE_QSV     },
    { "av1_amf",   AV_HWDEVICE_TYPE_D3D11VA },
    { nullptr,     AV_HWDEVICE_TYPE_NONE    },
};
const char* const kAV1Software[] = { "libdav1d", "av1", nullptr };

const HardwareDecoder kVP9Hardware[] = {
    { "vp9_cuvid", AV_HWDEVICE_TYPE_CUDA    },
    { "vp9_qsv",   AV_HWDEVICE_TYPE_QSV     },
    { "vp9_amf",   AV_HWDEVICE_TYPE_D3D11VA },
    { nullptr,     AV_HWDEVICE_TYPE_NONE    },
};
const char* const kVP9Software[] = { "libvpx-vp9", "vp9", nullptr };

const SupportedCodec kSupportedCodecs[] = {
    { AV_CODEC_ID_AV1, "AV1", kAV1Hardware, kAV1Software },
    { AV_CODEC_ID_VP9, "VP9", kVP9Hardware, kVP9Software },
};

const SupportedCodec* FindCodec(int codecId) {
    for (const SupportedCodec& c : kSupportedCodecs) {
        if (c.id == codecId) return &c;
    }
    return nullptr;
}

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
        SetError("cannot open file", err);
        return false;
    }

    err = avformat_find_stream_info(fmt_, nullptr);
    if (err < 0) {
        SetError("cannot read stream info", err);
        CloseLocked();
        return false;
    }

    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    // Файл без видео — тоже наш случай, но не всякий.
    //
    // Правило то же, что и для видео: берём только то, что Premiere не открывает
    // сам. Matroska и WebM он не понимает вовсе, поэтому звуковая дорожка в них
    // без нас пропадёт совсем. А вот m4a или mp4 со звуком он открывает
    // прекрасно, и лезть туда значит отбирать работающее.
    if (videoStream_ < 0) {
        const bool ourContainer =
            fmt_->iformat && fmt_->iformat->name &&
            (strstr(fmt_->iformat->name, "matroska") || strstr(fmt_->iformat->name, "webm"));

        if (!ourContainer) {
            SetError("no video stream, and this container is not ours");
            CloseLocked();
            return false;
        }
        if (av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) < 0) {
            SetError("no video and no audio in file");
            CloseLocked();
            return false;
        }

        info_.hasVideo = false;

        // Длительность обычно считается по видеодорожке; здесь её нет, а
        // без длины Premiere не покажет клип вовсе
        if (fmt_->duration != AV_NOPTS_VALUE) {
            info_.durationSec = fmt_->duration / (double)AV_TIME_BASE;
        }

        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ++info_.audioStreamCount;
            }
        }
        info_.codecName = "audio only";
        return true;
    }

    AVStream* st = fmt_->streams[videoStream_];

    // Всё, чего нет в списке, отдаём штатным импортёрам Premiere — иначе
    // перехватим форматы, которые он и сам прекрасно открывает.
    const SupportedCodec* codec = FindCodec(st->codecpar->codec_id);
    if (!codec) {
        SetError("video codec is not supported by this plug-in");
        CloseLocked();
        return false;
    }
    codecId_ = st->codecpar->codec_id;

    // Глубину и прозрачность выясняем ДО открытия декодера: от них зависит,
    // какой декодер вообще годится.
    //
    // Глубина берётся из потока, а не из декодера: у аппаратного пути pix_fmt
    // это AV_PIX_FMT_CUDA, и глубины там нет.
    if (const AVPixFmtDescriptor* desc =
            av_pix_fmt_desc_get((AVPixelFormat)st->codecpar->format)) {
        info_.bitDepth = desc->comp[0].depth;
        info_.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    }

    // У VP9 прозрачность лежит не в самом потоке, а рядом с ним: Matroska несёт
    // альфу отдельным довеском к блоку и помечает дорожку тегом alpha_mode.
    // По формату потока её не видно — там обычный yuv420p.
    if (const AVDictionaryEntry* tag = av_dict_get(st->metadata, "alpha_mode", nullptr, 0)) {
        if (tag->value && atoi(tag->value) != 0) info_.hasAlpha = true;
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

    // Общий ноль клипа. Не всякий файл начинается с нуля: запись из
    // транспортного потока или мультиплексирование с -copyts оставляют начало
    // где угодно. Считать от нуля контейнера, а не каждый поток от своего
    // начала, важно вдвойне: между видео и звуком бывает настоящий сдвиг
    // (у AAC, например, свой разгон), и он обязан сохраниться.
    //
    // Без этого файл со сдвинутым началом разваливался целиком: любой запрос
    // возвращал первый кадр, а звук — тишину.
    startTimeSec_ = 0.0;
    if (fmt_->start_time != AV_NOPTS_VALUE) {
        startTimeSec_ = fmt_->start_time / (double)AV_TIME_BASE;
    } else if (st->start_time != AV_NOPTS_VALUE) {
        startTimeSec_ = st->start_time * av_q2d(st->time_base);
    }

    if (info_.fps > 0) {
        toleranceTicks_ = (int64_t)(0.25 / info_.fps / av_q2d(st->time_base));
    }

    if (st->duration != AV_NOPTS_VALUE) {
        info_.durationSec = st->duration * av_q2d(st->time_base);
    } else if (fmt_->duration != AV_NOPTS_VALUE) {
        info_.durationSec = fmt_->duration / (double)AV_TIME_BASE;
    }

    // Длина клипа в кадрах ТАЙМЛАЙНА, а не в кадрах источника. При постоянной
    // частоте это одно и то же, при плавающей — нет: в нашем проверочном файле
    // 350 кадров источника на 10 секунд, а таймлайн 60 fps держит их 601.
    // Поэтому длительность важнее счётчика кадров, а не наоборот; nb_frames
    // остаётся на случай, когда длительности в контейнере нет.
    info_.frameCount = (info_.fps > 0 && info_.durationSec > 0)
        ? (int64_t)(info_.durationSec * info_.fps + 0.5)
        : st->nb_frames;

    info_.codecName = codec->name;

    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++info_.audioStreamCount;
        }
    }

    frame_   = av_frame_alloc();
    swFrame_ = av_frame_alloc();
    lastFrame_ = av_frame_alloc();
    aheadFrame_ = av_frame_alloc();
    packet_  = av_packet_alloc();
    if (!frame_ || !swFrame_ || !packet_) {
        SetError("out of memory for frame buffers");
        CloseLocked();
        return false;
    }

    // Бюджет кэша считаем от разрешения, а не берём константой: кадр 4K весит
    // вчетверо больше, чем 1440p, и фиксированные 256 МБ вмещали бы уже не
    // полсекунды видео, а пятую часть. Цель — около секунды при 60 кадрах/с,
    // с потолком, чтобы не отъедать память у самого монтажа.
    const size_t bytesPerFrame = (size_t)info_.width * info_.height * 3 / 2;  // NV12
    const size_t wanted = bytesPerFrame * 60;
    cacheBudget_ = std::min<size_t>(std::max<size_t>(wanted, 128u * 1024 * 1024),
                                    512u * 1024 * 1024);

    lastDecodedFrame_ = -1;
    eofReached_ = false;
    return true;
}

bool Decoder::OpenCodec(bool preferHardware) {
    AVStream* st = fmt_->streams[videoStream_];

    auto tryDecoder = [&](const char* name, AVHWDeviceType device) -> bool {
        const bool hardware = (device != AV_HWDEVICE_TYPE_NONE);
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
            if (av_hwdevice_ctx_create(&hw, device, nullptr, nullptr, 0) < 0) {
                // Нет такого устройства на этой машине — этот декодер не наш,
                // пробуем следующий, а не открываем его без контекста
                avcodec_free_context(&ctx);
                return false;
            }
            ctx->hw_device_ctx = av_buffer_ref(hw);
            av_buffer_unref(&hw);
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

    const SupportedCodec* codec = FindCodec(codecId_);
    if (!codec) {
        SetError("video codec is not supported by this plug-in");
        return false;
    }

    // Прозрачность умеет только программный путь. Аппаратные декодеры про
    // довесок Matroska не знают вовсе и отдают кадр без альфы — проверено:
    // vp9_cuvid на файле с прозрачностью выдаёт сплошную непрозрачность.
    // Молча потерять альфу хуже, чем декодировать медленнее.
    if (info_.hasAlpha) preferHardware = false;

    if (preferHardware) {
        for (const HardwareDecoder* hw = codec->hardware; hw->name; ++hw) {
            if (tryDecoder(hw->name, hw->device)) return true;
        }
    }
    for (const char* const* name = codec->software; *name; ++name) {
        if (tryDecoder(*name, AV_HWDEVICE_TYPE_NONE)) return true;
    }

    SetError("no decoder available for this codec");
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
        // Перенос забирает только пиксели: без этого у кадра из кэша не
        // осталось бы метки времени, и он не смог бы сказать, откуда он
        av_frame_copy_props(copy, src);
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
    if (lastFrame_){ av_frame_free(&lastFrame_); }
    if (aheadFrame_){ av_frame_free(&aheadFrame_); }
    if (codec_)    { avcodec_free_context(&codec_); }
    if (hwDevice_) { av_buffer_unref(&hwDevice_); }
    if (fmt_)      { avformat_close_input(&fmt_); }

    videoStream_      = -1;
    codecId_          = 0;
    lastDecodedFrame_ = -1;
    eofReached_       = false;
    lastFrameTimeSec_ = 0.0;
    startTimeSec_     = 0.0;
    curValid_         = false;
    aheadValid_       = false;
    toleranceTicks_   = 0;
    info_ = MediaInfo{};
}

// Метка времени исходника, в которую попадает кадр таймлайна с этим номером.
int64_t Decoder::TargetTicks(int64_t timelineFrame) const {
    if (!fmt_ || videoStream_ < 0 || info_.fps <= 0) return timelineFrame;
    const double tb = av_q2d(fmt_->streams[videoStream_]->time_base);
    return (int64_t)llround((timelineFrame / info_.fps + startTimeSec_) / tb);
}

// Номер кадра таймлайна, с которого начинает показываться кадр с этой меткой.
int64_t Decoder::TimelineIndexOf(int64_t ts) const {
    if (!fmt_ || videoStream_ < 0) return 0;
    // Без частоты кадров метка и есть номер, см. DecodeUntil
    if (info_.fps <= 0) return ts > 0 ? ts : 0;
    const double tb  = av_q2d(fmt_->streams[videoStream_]->time_base);
    const double sec = ts * tb - startTimeSec_;
    // Вверх, а не вниз: кадр становится нужным начиная с ПЕРВОГО момента
    // таймлайна, который не раньше него. Округление вниз давало кадр вперёд
    // на файле, где видео начинается позже общего нуля клипа, — и кэш отдавал
    // не то, что посчитал бы сам декодер.
    const int64_t idx = (int64_t)ceil(sec * info_.fps - 0.25);
    return idx > 0 ? idx : 0;
}

// Время кадра в секундах от общего нуля клипа.
double Decoder::FrameTimeSec(const AVFrame* f) const {
    if (!f || !fmt_ || videoStream_ < 0) return 0.0;
    const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE
                        ? f->best_effort_timestamp : f->pts;
    if (pts == AV_NOPTS_VALUE) return 0.0;
    return pts * av_q2d(fmt_->streams[videoStream_]->time_base) - startTimeSec_;
}

// Найти кадр источника, который виден в момент, когда Premiere показывает
// кадр таймлайна с номером targetFrame.
//
// Это не одно и то же, и в этом вся суть. Таймлайн идёт с постоянной частотой,
// источник — как получится: запись с экрана выдаёт то десять кадров в секунду,
// то шестьдесят, а в заголовке контейнера при этом стоит одно число. Считать
// номер кадра как «метка × частота» можно ровно до первого такого файла: у нас
// на нём каждый второй запрос возвращал самый первый кадр записи, в том числе
// на пятой секунде.
//
// Правило вместо арифметики: показать последний кадр, чья метка не позже
// нужного момента. Узнать, что он последний, можно только заглянув на кадр
// вперёд — длительность кадра из контейнера для этого не годится, Matroska
// проставляет её из заголовка и на плавающей частоте пишет всем кадрам одно
// и то же. Заглянувший кадр не выбрасывается, а остаётся до следующего
// запроса: при обычном воспроизведении подряд он и окажется нужным.
bool Decoder::DecodeUntil(int64_t targetFrame) {
    // Частота кадров известна не всегда. Без неё времени нет, и остаётся
    // единственное, что есть у любого файла, — порядок кадров: тогда «метка»
    // это просто номер по счёту, и вся логика ниже работает как была.
    const bool    byTime     = info_.fps > 0;
    const int64_t targetTs   = byTime ? TargetTicks(targetFrame) : targetFrame;
    const int64_t frameTicks = byTime ? (TargetTicks(1) - TargetTicks(0)) : 1;

    // Отдать выбранный кадр наружу. Ссылка, а не копия: пикселей не трогаем.
    auto deliver = [this]() {
        av_frame_unref(frame_);
        return av_frame_ref(frame_, lastFrame_) == 0;
    };

    // Назад или далеко вперёд — перематываем. Вперёд на несколько кадров дешевле
    // просто домотать декодированием: перемотка всегда идёт до ключевого кадра,
    // и на длинном интервале между ними обходится дороже.
    const int64_t kForwardScanLimit = 64;
    const bool needSeek = !curValid_ ||
                          targetTs < curTs_ ||
                          (targetFrame - lastDecodedFrame_ > kForwardScanLimit);

    if (needSeek) {
        // Без частоты кадров цель в метках времени не выразить — отматываем
        // к началу и доходим по порядку
        const int64_t seekTs = byTime ? targetTs : 0;
        if (av_seek_frame(fmt_, videoStream_, seekTs, AVSEEK_FLAG_BACKWARD) < 0) {
            SetError("seek failed");
            return false;
        }
        avcodec_flush_buffers(codec_);
        curValid_         = false;
        aheadValid_       = false;
        lastDecodedFrame_ = -1;
        eofReached_       = false;
    }

    while (true) {
        // Сначала разбираемся с уже раскодированным кадром, если он есть
        if (aheadValid_) {
            if (aheadTs_ <= targetTs + toleranceTicks_) {
                // До цели не дошли — этот кадр становится текущим
                av_frame_unref(lastFrame_);
                av_frame_move_ref(lastFrame_, aheadFrame_);
                curTs_            = aheadTs_;
                curValid_         = true;
                aheadValid_       = false;
                lastDecodedFrame_ = TimelineIndexOf(curTs_);

                if (cacheFill_) StoreInCache(lastDecodedFrame_, lastFrame_);

                // Кадр лёг ровно в запрошенный момент — заглядывать вперёд
                // незачем: следующий будет уже за целью, а лучше «точно в
                // момент» ничего не бывает. Так постоянная частота не платит
                // за разведку ничего, а платила заметно: лишний кадр после
                // перемотки стоил 3% на случайных прыжках, потому что
                // многопоточный декодер отдаёт кадры не по одному.
                //
                // Допуск здесь тот же, что и при выборе кадра, и по той же
                // причине: метки в Matroska округлены до миллисекунд.
                if (llabs(curTs_ - targetTs) <= toleranceTicks_) return deliver();

                continue;
            }

            // Перешагнули цель: нужный кадр — текущий, а этот подождёт
            if (curValid_) return deliver();

            // Цель раньше самого первого кадра файла — отдаём первый.
            // Так бывает у файлов, где видео начинается позже звука.
            av_frame_unref(lastFrame_);
            av_frame_move_ref(lastFrame_, aheadFrame_);
            curTs_            = aheadTs_;
            curValid_         = true;
            aheadValid_       = false;
            lastDecodedFrame_ = TimelineIndexOf(curTs_);
            return deliver();
        }

        int err = avcodec_receive_frame(codec_, aheadFrame_);

        if (err == 0) {
            const int64_t pts = aheadFrame_->best_effort_timestamp != AV_NOPTS_VALUE
                                ? aheadFrame_->best_effort_timestamp
                                : aheadFrame_->pts;

            // Кадр без метки времени внутри файла, где метки есть, — считаем
            // его на кадр позже предыдущего, а не на один тик
            if (!byTime) {
                aheadTs_ = curValid_ ? curTs_ + 1 : 0;
            } else if (pts != AV_NOPTS_VALUE) {
                aheadTs_ = pts;
            } else {
                aheadTs_ = curValid_ ? curTs_ + frameTicks : targetTs;
            }
            aheadValid_ = true;
            continue;
        }

        if (err == AVERROR_EOF) {
            eofReached_ = true;

            // Здесь пряталось падение. Комментарий раньше уверял, что «последний
            // кадр всё ещё в frame_», но avcodec_receive_frame очищает кадр
            // ПЕРЕД тем, как вернуть ошибку: в frame_ оставалось 0x0 с форматом
            // -1. Дальше он уходил в swscale, а тот на такое не отвечает ошибкой,
            // а падает по av_assert0 — то есть уносит с собой весь Premiere.
            // Поэтому отдаём последний целый кадр: на таймлайне повтор лучше
            // чёрного провала.
            if (curValid_ && lastFrame_->width > 0 && lastFrame_->format >= 0) {
                return deliver();
            }
            SetError("end of file reached before the requested frame");
            return false;
        }

        if (err != AVERROR(EAGAIN)) {
            SetError("decode error", err);
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
            SetError("file read error", rerr);
            return false;
        }
        if (packet_->stream_index != videoStream_) continue;

        if (avcodec_send_packet(codec_, packet_) < 0) {
            SetError("decoder rejected packet");
            return false;
        }
    }
}
bool Decoder::ConvertToBGRA(AVFrame* src, uint8_t* dst, int dstStride, int dstW, int dstH,
                            FrameFormat format) {
    AVFrame* srcFrame = src;

    // Если декодировала видеокарта, кадр лежит в её памяти — забираем в обычную
    if (src->hw_frames_ctx) {
        const auto t0 = std::chrono::steady_clock::now();
        av_frame_unref(swFrame_);
        if (av_hwframe_transfer_data(swFrame_, src, 0) < 0) {
            SetError("cannot transfer frame from GPU memory");
            return false;
        }
        srcFrame = swFrame_;
        stats_.transferMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // Вторая линия обороны. Кадр без формата или без размеров swscale не
    // отвергает, а роняет процесс по av_assert0 — внутри Premiere это была бы
    // не ошибка импорта, а закрывшийся Premiere. Настоящую причину чинит
    // DecodeUntil, но проверка тут стоит и остаётся: цена ей ноль.
    if (srcFrame->width <= 0 || srcFrame->height <= 0 ||
        srcFrame->format < 0 ||
        !av_pix_fmt_desc_get((AVPixelFormat)srcFrame->format)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "decoded frame is unusable: %dx%d, pixel format %d",
                 srcFrame->width, srcFrame->height, srcFrame->format);
        SetError(msg);
        return false;
    }

    const auto tConv = std::chrono::steady_clock::now();

    // Размер берём запрошенный, а не размер файла: Premiere при пониженном
    // качестве воспроизведения создаёт буфер поменьше, и запись туда полного
    // кадра выходила бы за его пределы.
    //
    // SWS_BICUBIC, а не BILINEAR: пока масштабирования не было, флаг ни на что
    // не влиял, а при уменьшении разница видна.
    const AVPixelFormat dstFmt = (format == FrameFormat::BGRA16)
                               ? AV_PIX_FMT_BGRA64LE
                               : AV_PIX_FMT_BGRA;

    // Пересчётчик кэшируется по всем своим параметрам, включая формат выхода:
    // иначе переключение между 8 и 16 битами молча продолжило бы писать в старом
    if (swsDstFmt_ != dstFmt) {
        sws_freeContext(sws_);
        sws_ = nullptr;
        swsDstFmt_ = dstFmt;
    }
    sws_ = sws_getCachedContext(sws_,
                                srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
                                dstW, dstH, dstFmt,
                                SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_) {
        SetError("cannot create colour converter");
        return false;
    }

    uint8_t* dstData[4] = { dst, nullptr, nullptr, nullptr };
    int dstLinesize[4]  = { dstStride, 0, 0, 0 };

    sws_scale(sws_, srcFrame->data, srcFrame->linesize, 0, srcFrame->height, dstData, dstLinesize);

    if (format == FrameFormat::BGRA16) {
        // swscale отдаёт полный шестнадцатибитный размах, а Adobe ждёт белое
        // на 32768. (v + 1) >> 1 переводит точно по краям: 65535 -> 32768, 0 -> 0.
        // Отдельный проход по кадру, но только для 10-битного пути.
        const int components = dstW * 4;
        for (int y = 0; y < dstH; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(dst + (ptrdiff_t)y * dstStride);
            for (int i = 0; i < components; ++i) {
                row[i] = (uint16_t)((row[i] + 1) >> 1);
            }
        }
    }

    stats_.convertMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tConv).count();
    ++stats_.frames;
    return true;
}

bool Decoder::GetFrameBGRA(int64_t frameIndex, uint8_t* dst, int dstStride,
                           int dstWidth, int dstHeight, FrameFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsOpen() || !dst) {
        SetError("file is not open");
        return false;
    }
    if (!info_.hasVideo || !codec_) {
        SetError("this file has no video");
        return false;
    }
    if (frameIndex < 0) frameIndex = 0;

    // Ноль означает «как в файле» — удобно для проверочных программ
    if (dstWidth  <= 0) dstWidth  = info_.width;
    if (dstHeight <= 0) dstHeight = info_.height;

    // Отматывание назад — единственный случай, когда кэш окупается: при чтении
    // вперёд кадры и так идут подряд, а кэш только тратил бы память
    const bool backward = (lastRequested_ >= 0 && frameIndex < lastRequested_);
    lastRequested_ = frameIndex;

    auto cached = frameCache_.find(frameIndex);
    if (cached != frameCache_.end()) {
        lastFrameTimeSec_ = FrameTimeSec(cached->second);
        return ConvertToBGRA(cached->second, dst, dstStride, dstWidth, dstHeight, format);
    }

    cacheFill_ = backward;
    const auto tDec = std::chrono::steady_clock::now();
    const bool ok = DecodeUntil(frameIndex);
    stats_.decodeMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tDec).count();
    cacheFill_ = false;

    // При обрыве в конце файла DecodeUntil отдаёт последний удачно
    // раскодированный кадр и сообщает об успехе — на таймлайне это лучше
    // чёрного провала, а Premiere всё равно не запрашивает кадры за длиной.
    if (!ok) return false;
    lastFrameTimeSec_ = FrameTimeSec(frame_);
    return ConvertToBGRA(frame_, dst, dstStride, dstWidth, dstHeight, format);
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
        SetError("file is not open");
        return false;
    }

    // Свой разбор контейнера для звука. Общий с видео не годится: чтение
    // пакетов двигает одну общую позицию, и перемотка видео сбивала бы звук.
    int err = avformat_open_input(&audioFmt_, fmt_->url, nullptr, nullptr);
    if (err < 0) {
        SetError("cannot open file for audio", err);
        return false;
    }
    if ((err = avformat_find_stream_info(audioFmt_, nullptr)) < 0) {
        SetError("cannot read audio stream info", err);
        CloseAudioLocked();
        return false;
    }

    int seen = 0;
    for (unsigned i = 0; i < audioFmt_->nb_streams; ++i) {
        if (audioFmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (seen++ == ordinal) { audioStreamIndex_ = (int)i; break; }
    }
    if (audioStreamIndex_ < 0) {
        SetError("no audio track with that index");
        CloseAudioLocked();
        return false;
    }

    AVStream* st = audioFmt_->streams[audioStreamIndex_];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        SetError("no decoder for this audio track");
        CloseAudioLocked();
        return false;
    }

    audioCodec_ = avcodec_alloc_context3(dec);
    if (!audioCodec_ ||
        avcodec_parameters_to_context(audioCodec_, st->codecpar) < 0 ||
        avcodec_open2(audioCodec_, dec, nullptr) < 0) {
        SetError("cannot open audio decoder");
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
        SetError("cannot set up audio conversion", err);
        CloseAudioLocked();
        return false;
    }

    audioFrame_  = av_frame_alloc();
    audioPacket_ = av_packet_alloc();
    if (!audioFrame_ || !audioPacket_) {
        SetError("out of memory for audio buffers");
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
                SetError("audio conversion error", got);
                return false;
            }

            // Позицию берём из метки времени: так не накапливается ошибка
            // после перемотки и не важен порядок пакетов.
            //
            // Отсчёт номер ноль — это общий ноль клипа, тот же, от которого
            // считаются кадры. Без вычитания startTimeSec_ файл, начинающийся
            // не с нуля, отдавал одну тишину: запрошенный отсчёт 0 оказывался
            // за сотни тысяч отсчётов до всего, что есть в файле.
            if (audioFrame_->pts != AV_NOPTS_VALUE) {
                AVStream* st = audioFmt_->streams[audioStreamIndex_];
                const double sec = audioFrame_->pts * av_q2d(st->time_base) - startTimeSec_;
                audioCursor_ = (int64_t)llround(sec * info_.audioSampleRate);
            } else if (audioCursor_ < 0) {
                audioCursor_ = 0;
            }

            pendingOffset_ = 0;
            pendingCount_  = got;
            return true;
        }

        if (err == AVERROR_EOF) return false;

        if (err != AVERROR(EAGAIN)) {
            SetError("audio decode error", err);
            return false;
        }

        av_packet_unref(audioPacket_);
        int rerr = av_read_frame(audioFmt_, audioPacket_);
        if (rerr == AVERROR_EOF) {
            avcodec_send_packet(audioCodec_, nullptr);  // слить остаток
            continue;
        }
        if (rerr < 0) {
            SetError("audio read error", rerr);
            return false;
        }
        if (audioPacket_->stream_index != audioStreamIndex_) continue;

        if (avcodec_send_packet(audioCodec_, audioPacket_) < 0) {
            SetError("audio decoder rejected packet");
            return false;
        }
    }
}

bool Decoder::GetAudio(int64_t startSample, int32_t sampleCount, float* const* dst) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (!audioCodec_ || !dst) {
        SetError("audio is not open");
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

        // Разгон: перематываем на 200 мс раньше нужного места и доходим до него
        // декодированием. Без этого чтение одного и того же куска дважды давало
        // РАЗНЫЕ отсчёты — окна AAC перекрываются, и первые кадры после
        // перемотки неточны, а сколько именно отбросит декодер, зависит от того,
        // на какой пакет он попал. Лишние отсчёты отбрасывает цикл ниже.
        const int64_t preroll = info_.audioSampleRate / 5;
        const int64_t seekSample = startSample > preroll ? startSample - preroll : 0;

        const double seekSec = (double)seekSample / info_.audioSampleRate + startTimeSec_;
        int64_t ts = (int64_t)llround(seekSec / av_q2d(st->time_base));
        if (av_seek_frame(audioFmt_, audioStreamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            SetError("audio seek failed");
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
