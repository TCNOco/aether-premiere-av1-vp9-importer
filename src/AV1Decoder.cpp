// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

#include "AV1Decoder.h"
#include "AV1Settings.h"
#include "AV1Version.h"
#include "ImporterMath.h"
#include "PreviewCache.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/display.h>
#include <libavutil/dict.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavcodec/packet.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <mutex>
#include <thread>

namespace av1imp {

// Счётчик живых декодеров видео на весь процесс, см. Decoder::Budget()
std::atomic<int> Decoder::videoDecoders_{0};
std::atomic<size_t> g_ramCacheBytes{0};

size_t Decoder::RamCacheBytesHeld() {
    return g_ramCacheBytes.load(std::memory_order_relaxed);
}

namespace {

bool ReserveRamCache(size_t bytes, size_t ceiling)
{
    size_t held = g_ramCacheBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > ceiling || held > ceiling - bytes) return false;
        if (g_ramCacheBytes.compare_exchange_weak(
                held, held + bytes,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

const char* PrimariesName(int v)
{
    switch (v) {
        case 1:  return "BT.709";
        case 2:  return "unspecified";
        case 5:  return "BT.601";
        case 6:  return "BT.601";
        case 9:  return "BT.2020";
        case 12: return "P3-DCI";
        case 13: return "P3-D65";
        default: return "other";
    }
}

const char* TransferName(int v)
{
    switch (v) {
        case 1:  return "BT.709";
        case 2:  return "unspecified";
        case 8:  return "linear";
        case 14: return "BT.2020-10";
        case 15: return "BT.2020-12";
        case 16: return "PQ";
        case 18: return "HLG";
        default: return "other";
    }
}

const char* MatrixName(int v)
{
    switch (v) {
        case 0:  return "RGB";
        case 1:  return "BT.709";
        case 2:  return "unspecified";
        case 5:  return "BT.601";
        case 6:  return "BT.601";
        case 9:  return "BT.2020";
        case 10: return "BT.2020 CL";
        default: return "other";
    }
}

void ReadHdrSideData(const AVCodecParameters* par, MediaInfo* info)
{
    if (!par || !info) return;

    if (const AVPacketSideData* sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
        AVContentLightMetadata cl{};
        const size_t n = (std::min)(sd->size, sizeof(cl));
        if (sd->data && n > 0) memcpy(&cl, sd->data, n);
        info->maxCll  = cl.MaxCLL;
        info->maxFall = cl.MaxFALL;
    }

    if (const AVPacketSideData* sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
        AVMasteringDisplayMetadata md{};
        const size_t n = (std::min)(sd->size, sizeof(md));
        if (sd->data && n > 0) memcpy(&md, sd->data, n);
        if (md.has_luminance) {
            info->masteringMaxNits = av_q2d(md.max_luminance);
            info->masteringMinNits = av_q2d(md.min_luminance);
        }
    }
}

} // namespace

std::string MediaInfo::ColourSummary() const
{
    std::string out = PrimariesName(colourPrimaries);
    out += " / ";
    out += TransferName(colourTransfer);
    if (colourTransfer == 16) out += " (HDR10)";
    else if (colourTransfer == 18) out += " (HLG)";
    out += " / ";
    out += MatrixName(colourMatrix);
    char bits[32];
    snprintf(bits, sizeof(bits), " / %d-bit / %s",
             bitDepth, fullRange ? "full range" : "limited");
    out += bits;
    if (maxCll > 0 || maxFall > 0) {
        char cll[64];
        snprintf(cll, sizeof(cll), ", MaxCLL %u MaxFALL %u", maxCll, maxFall);
        out += cll;
    }
    if (masteringMaxNits > 0) {
        char md[48];
        snprintf(md, sizeof(md), ", mastering %.0f nits", masteringMaxNits);
        out += md;
    }
    return out;
}

namespace {

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

    // false — отдельный фирменный декодер (av1_cuvid и подобные).
    // true  — ОБЫЧНЫЙ декодер плюс аппаратный ускоритель: сам разбор потока
    //         делает ffmpeg, а тяжёлую работу берёт видеокарта через
    //         драйвер. Так устроен D3D11VA, и этим он ценен: он один на
    //         всех — AMD, Intel, NVIDIA, — тогда как фирменных декодеров
    //         нужно по одному на вендора, и у каждого свои болезни.
    bool               hwaccel;
};

struct SupportedCodec {
    AVCodecID              id;
    const char*            name;          // как показываем в сведениях и журнале
    const HardwareDecoder* hardware;      // список, конец — name == nullptr
    const char* const*     software;      // список, конец — nullptr
};

// AMD (av1_amf, vp9_amf) в этом списке НЕТ, и это решение по замеру.
//
// Декодеры AMF в этой сборке ffmpeg падают: не «не заводятся», а роняют
// процесс нарушением доступа (0xC0000005) — сразу после того, как успешно
// распакуют кадры. Проверено 2026-08-22 на Radeon встроенного 9800X3D,
// драйвер 32.0.21045.1000, пять прогонов подряд, одинаково на av1_amf и
// vp9_amf, хоть на тридцати кадрах, хоть на одном.
//
// Пока рядом стоит NVIDIA, ветка недостижима: устройство D3D11 достаётся ей,
// а на чужом устройстве AMF не заводится вовсе. Но на машине ТОЛЬКО с AMD
// она была бы достижима — и падение случилось бы внутри Premiere, где оно
// выглядит как «Premiere сам закрылся», без единой строчки где-либо.
//
// Терять при этом нечего: по нашим же замерам процессор быстрее видеокарты
// вшестеро, так что аппаратный путь AMD не дал бы даже выигрыша, ради
// которого стоило бы рисковать. Обменять возможное падение хоста на
// программную распаковку — обмен в одну сторону.
//
// Совсем убирать поддержку не стали: имя декодера по-прежнему принимается
// через AETHER_DECODER, так что проверить его на исправной сборке ffmpeg
// можно в любой момент, не трогая код.
const HardwareDecoder kAV1Hardware[] = {
    { "av1_cuvid", AV_HWDEVICE_TYPE_CUDA,    false },
    { "av1_qsv",   AV_HWDEVICE_TYPE_QSV,     false },
    { "av1",       AV_HWDEVICE_TYPE_D3D11VA, true  },   // AMD, Intel, NVIDIA
    { nullptr,     AV_HWDEVICE_TYPE_NONE,    false },
};
const char* const kAV1Software[] = { "libdav1d", "av1", nullptr };

const HardwareDecoder kVP9Hardware[] = {
    { "vp9_cuvid", AV_HWDEVICE_TYPE_CUDA,    false },
    { "vp9_qsv",   AV_HWDEVICE_TYPE_QSV,     false },
    { "vp9",       AV_HWDEVICE_TYPE_D3D11VA, true  },   // AMD, Intel, NVIDIA
    { nullptr,     AV_HWDEVICE_TYPE_NONE,    false },
};
const char* const kVP9Software[] = { "libvpx-vp9", "vp9", nullptr };

const SupportedCodec kSupportedCodecs[] = {
    { AV_CODEC_ID_AV1, "AV1", kAV1Hardware, kAV1Software },
    { AV_CODEC_ID_VP9, "VP9", kVP9Hardware, kVP9Software },
};

// Какой формат кадра выбрать, когда декодер спрашивает.
//
// Обычный декодер с аппаратным ускорителем задаёт этот вопрос при первом
// кадре и предлагает список: сначала аппаратные поверхности, потом обычную
// память. Не ответить — значит согласиться на обычную, то есть получить
// распаковку процессором, продолжая считать её аппаратной.
//
// Нужный формат кладём в opaque при открытии: он известен заранее из
// avcodec_get_hw_config, и гадать в обратном вызове не о чем.
AVPixelFormat PickHardwareFormat(AVCodecContext* ctx, const AVPixelFormat* offered) {
    const AVPixelFormat wanted = (AVPixelFormat)(intptr_t)ctx->opaque;
    for (const AVPixelFormat* p = offered; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == wanted) return *p;
    }

    // Предложили не то, что обещали при открытии. Соглашаться на обычную
    // память нельзя: получилась бы «аппаратная» распаковка процессором, и
    // мы бы её так и назвали. Пусть лучше кадр не выйдет вовсе.
    return AV_PIX_FMT_NONE;
}

// Умеет ли этот декодер работать с таким устройством, и в каком формате
// он тогда отдаёт кадры. Спрашиваем ДО создания устройства: незачем заводить
// D3D11, чтобы через строчку выяснить, что декодер о нём не слышал.
bool HardwareFormatFor(const AVCodec* dec, AVHWDeviceType device,
                       AVPixelFormat* out) {
    for (int i = 0; ; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) return false;
        if (cfg->device_type == device &&
            (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            *out = cfg->pix_fmt;
            return true;
        }
    }
}

// Какой видеоадаптер брать под D3D11VA.
//
// По умолчанию — тот, что система считает основным, и в подавляющем
// большинстве случаев это правильно. Но на машине с двумя картами основной
// оказывается одна, а проверить надо другую: у нас встроенная AMD и дискретная
// NVIDIA, и без этого номера встройка недостижима вовсе.
//
// Пригодится и не только для проверок: у ноутбуков с переключаемой графикой
// «основной» бывает не тот, на котором хочется распаковывать.
const char* ForcedAdapter() {
    static const char* value = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("AETHER_D3D11_ADAPTER");
        value = (v && *v) ? v : nullptr;
    });
    return value;
}

// Принудительный выбор декодера через AETHER_DECODER.
//
// Нужен для двух вещей, и обе настоящие. Во-первых, проверить путь Intel или
// AMD на машине, где стоит ещё и NVIDIA, иначе никак: перебор доходит до
// cuvid и на нём останавливается. Во-вторых, поддержка — «запустите с
// AETHER_DECODER=libdav1d» короче любых объяснений.
//
// Перебор при этом НЕ продолжается: заданный декодер либо работает, либо
// файл не открывается. Тихий откат на другой означал бы, что проверка
// показывает не то, что проверяли.
// getenv, а не GetEnvironmentVariable: ядро намеренно не знает про Windows,
// и заводить эту зависимость ради одной переменной незачем.

const char* ForcedDecoder() {
    static const char* value = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("AETHER_DECODER");
        value = (v && *v) ? v : nullptr;
    });
    return value;
}

// Какие контейнеры вообще открываем. Имя в файле нам врёт: демультиплексор
// ffmpeg выбирает по содержимому, и файл «.mp4» может оказаться списком
// concat со ссылкой на соседний файл. Белый список — четыре наших формата
// и ничего больше. protocol_whitelist=file закрывает http/concat-url на
// будущее: в этой сборке ffmpeg оба пути и так выключены по умолчанию.
AVDictionary* ContainerOpenOptions()
{
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    av_dict_set(&opts, "format_whitelist",
                "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm", 0);
    return opts;
}

int OpenContainer(AVFormatContext** ctx, const char* path)
{
    AVDictionary* opts = ContainerOpenOptions();
    const int err = avformat_open_input(ctx, path, nullptr, &opts);
    if (err >= 0 &&
        (av_dict_get(opts, "protocol_whitelist", nullptr, 0) ||
         av_dict_get(opts, "format_whitelist", nullptr, 0))) {
        // Остаток в словаре значит, что эта сборка ffmpeg опцию не съела.
        // Тогда белый список не действует — закрываемся, а не открываем «как есть».
        avformat_close_input(ctx);
        av_dict_free(&opts);
        return AVERROR_OPTION_NOT_FOUND;
    }
    av_dict_free(&opts);
    return err;
}

// Потолок ребра кадра. 8K DCI — 8192 по ширине, поэтому равенство проходит.
// Фаззер берёг себя тем же числом, продакшн — нет: width из заголовка шёл
// в расчёт кэша и в вектор диагностики как есть.
// Само число — av1imp::kMaxFrameEdge в ImporterMath.h.

// Сколько шагов демультиплексора на один запрос. Битый контейнер без EOF
// иначе крутит поток хоста, пока Premiere не снимут.
const int kMaxDemuxSteps = 65536;

const SupportedCodec* FindCodec(int codecId) {
    for (const SupportedCodec& c : kSupportedCodecs) {
        if (c.id == codecId) return &c;
    }
    return nullptr;
}

} // namespace

// Сколько потоков дать декодеру, который открывается прямо сейчас.
//
// Прежде стоял ноль — «по числу ядер», — и это значило по числу ядер НА КАЖДЫЙ
// открытый клип. Premiere держит их открытыми столько, сколько лежит
// на таймлайне.
//
// Замер на 1440p, десять клипов (память) и один клип (скорость):
//
//     потоков   память 10 клипов   скорость 1 клипа
//     1              999 МБ            248 кадров/с
//     2             1387 МБ            401
//     4             1392 МБ            420
//     8             1624 МБ            549
//     все ядра      1842 МБ            662
//
// Отсюда два вывода. Первый: потоки стоят памяти всерьёз — 843 МБ между
// краями. Второй, важнее: **даже один поток даёт вчетверо больше, чем нужно
// для воспроизведения** 60 кадров/с. Полная скорость нужна не таймлайну,
// а экспорту, где Premiere читает так быстро, как может.
//
// Поэтому делим, а не режем: пока клип один — все ядра, как было; когда их
// много — поровну. Общее число потоков держится около числа ядер в обоих
// случаях, а редкий случай «один клип на экспорте» не платит ничего.
//
// ⚠ Число фиксируется при открытии декодера и потом не меняется: ffmpeg
// не даёт переоткрыть пул, а переоткрывать декодер на каждое изменение числа
// клипов означало бы терять его состояние посреди работы. Значит клип,
// открытый первым, сохранит широкий пул. Это осознанная неточность: она
// ошибается в сторону скорости, а не в сторону памяти.
int Decoder::ThreadsForNewDecoder() {
    if (const char* env = std::getenv("AETHER_THREADS")) {
        const long n = strtol(env, nullptr, 10);
        if (n >= 0 && n <= 64) return (int)n;
    }

    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;   // не смогли узнать — берём умеренно

    // Считаем вместе с тем декодером, который открывается сейчас
    const int live = videoDecoders_.load(std::memory_order_relaxed) + 1;
    int share = (int)cores / (live > 0 ? live : 1);
    if (share < 1) share = 1;
    return share;
}

Decoder::~Decoder() {
    Close();
}

// Текст ошибки собирается ДО захвата замка: av_strerror лезет в ffmpeg, и
// держать под замком чужой вызов незачем — замок должен покрывать ровно
// присваивание строки, ничего сверх.
void Decoder::SetError(const std::string& msg, int averr) {
    std::string text = averr ? msg + ": " + AvErr(averr) : msg;
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = std::move(text);
}

void Decoder::SetAudioError(const std::string& msg, int averr) {
    std::string text = averr ? msg + ": " + AvErr(averr) : msg;
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastAudioError_ = std::move(text);
}

std::string Decoder::LastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

// Если звук ещё ни разу не жаловался, отдаём общую ошибку: у отказа на
// открытии файла звуковой половины просто нет, а вызывающему нужен текст,
// а не пустая строка.
std::string Decoder::LastAudioError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastAudioError_.empty() ? lastError_ : lastAudioError_;
}

bool Decoder::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fmt_ != nullptr;
}

MediaInfo Decoder::Info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

Decoder::Stats Decoder::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void Decoder::ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats();
}

bool Decoder::Open(const std::string& utf8Path, bool preferHardware, bool needVideo) {
    std::scoped_lock lock(mutex_, audioMutex_);
    CloseLocked();

    int err = OpenContainer(&fmt_, utf8Path.c_str());
    if (err < 0) {
        SetError("cannot open file", err);
        return false;
    }
    path_ = utf8Path;

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

    // Цветовое описание берём из потока: в самом сжатом видео его может не
    // быть вовсе, и тогда кадр приедет с «не указано».
    streamColourspace_ = st->codecpar->color_space;
    streamColourRange_ = st->codecpar->color_range;

    // То же описание, но наружу — импортёру, чтобы он передал его хосту.
    //
    // Матрица: что сказано в файле, иначе догадка по высоте кадра. Ровно та
    // же лестница, что и у пересчёта в RGB, — иначе объявили бы одно, а
    // сделали другое.
    info_.colourMatrix = (st->codecpar->color_space != AVCOL_SPC_UNSPECIFIED)
                         ? st->codecpar->color_space
                         : (st->codecpar->height > 576 ? AVCOL_SPC_BT709
                                                       : AVCOL_SPC_BT470BG);

    // Первичные цвета: если их не записали, выводим ИЗ МАТРИЦЫ, а не гадаем
    // по высоте отдельно. Иначе выходила несуразица — у файла с матрицей
    // BT.709 объявлялись первичные цвета BT.601 просто потому, что кадр
    // невысокий. Матрица и первичные цвета в стандартах ходят парой.
    if (st->codecpar->color_primaries != AVCOL_PRI_UNSPECIFIED) {
        info_.colourPrimaries = st->codecpar->color_primaries;
    } else {
        switch (info_.colourMatrix) {
            case AVCOL_SPC_BT709:      info_.colourPrimaries = AVCOL_PRI_BT709;      break;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:  info_.colourPrimaries = AVCOL_PRI_BT2020;     break;
            case AVCOL_SPC_SMPTE170M:  info_.colourPrimaries = AVCOL_PRI_SMPTE170M;  break;
            case AVCOL_SPC_SMPTE240M:  info_.colourPrimaries = AVCOL_PRI_SMPTE240M;  break;
            default:                   info_.colourPrimaries = AVCOL_PRI_BT470BG;    break;
        }
    }

    // Кривая переноса: только то, что записано. Угадать её нельзя — разница
    // между обычной гаммой и PQ это разница между нормальной картинкой и
    // выцветшей, а по разрешению или матрице такое не выводится.
    //
    // Не указано — так и оставляем (ITU / AVCOL_TRC_UNSPECIFIED = 2).
    // Подставлять BT.709 здесь значит солгать хосту: файл без метки приедет
    // как SDR 709, даже если по факту PQ или лог.
    info_.colourTransfer = st->codecpar->color_trc;

    info_.fullRange = (st->codecpar->color_range == AVCOL_RANGE_JPEG);
    ReadHdrSideData(st->codecpar, &info_);

    // Положение цветности. Нужно только выдаче без пересчёта: пока мы сами
    // переводим в RGB, swscale разбирается с этим внутри себя, а вот отдавая
    // цветность как есть, надо сказать хосту, где она стоит.
    //
    // Не указано — считаем «слева»: так лежит цветность в MPEG-2, H.264 и
    // почти во всём остальном, и AV1 при неуказанном положении подразумевает
    // то же. По центру она стоит у MPEG-1 и JPEG, то есть у древностей.
    info_.chromaLocation = (st->codecpar->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
                           ? st->codecpar->chroma_location
                           : AVCHROMA_LOC_LEFT;

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

    // Размер — из контейнера, до декодера и до любых векторов под кадр.
    // Ноль или гигант из повреждённого заголовка раньше доходил до swscale.
    if (st->codecpar->width <= 0 || st->codecpar->height <= 0 ||
        st->codecpar->width > kMaxFrameEdge || st->codecpar->height > kMaxFrameEdge) {
        SetError("frame size is not usable");
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

    // Поворот при показе. Лежит рядом с потоком отдельным довеском —
    // матрицей 3x3, — а не в самом видео: кадры телефон пишет как есть,
    // горизонтально, и только эта матрица говорит, что смотреть на них надо
    // повёрнутыми.
    //
    // av_display_rotation_get уже отдаёт градусы ПРОТИВ часовой стрелки —
    // ровно то, что нам нужно, и переворачивать знак не надо. (Знак минус
    // встречается в самом ffmpeg, но там он нужен фильтру rotate, который
    // считает по часовой. Первая версия этой правки его скопировала, и файл
    // с меткой 90 объявлялся как 270. Поймано сверкой с ffprobe, который
    // печатает то же самое число.)
    //
    // Сверять есть с чем и впредь:
    //     ffprobe -select_streams v:0 -show_entries stream_side_data=rotation
    if (const AVPacketSideData* sd =
            av_packet_side_data_get(st->codecpar->coded_side_data,
                                    st->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_DISPLAYMATRIX)) {
        if (sd->data && sd->size >= sizeof(int32_t) * 9) {
            const double angle = av_display_rotation_get((const int32_t*)sd->data);
            if (!std::isnan(angle)) {
                // К ближайшей четверти оборота: матрица может нести и
                // произвольный угол, но контейнеры пишут только эти четыре.
                int deg = (int)llround(angle / 90.0) * 90;
                deg = ((deg % 360) + 360) % 360;
                info_.rotationDegrees    = deg;
                info_.rotationSwapsSides = (deg == 90 || deg == 270);
            }
        }
    }

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

    // Дорожкам звука декодер кадров, кэш и слот в общем бюджете не нужны:
    // иначе каждый audio-only поток клипа считался бы ещё одним видео.
    if (needVideo) {
        frame_   = av_frame_alloc();
        swFrame_ = av_frame_alloc();
        lastFrame_ = av_frame_alloc();
        aheadFrame_ = av_frame_alloc();
        packet_  = av_packet_alloc();
        if (!frame_ || !swFrame_ || !lastFrame_ || !aheadFrame_ || !packet_) {
            SetError("out of memory for frame buffers");
            CloseLocked();
            return false;
        }

        // Сколько кэша хочет ЭТОТ клип: около секунды видео. Считаем от разрешения,
        // а не константой — кадр 4K весит вчетверо больше, чем 1440p, и фиксированные
        // 256 МБ вмещали бы уже не полсекунды, а пятую часть.
        //
        // Прежде здесь стоял ещё и нижний порог в 128 МБ, и он был вреден: клипу
        // 640x360 нужно 20 МБ на секунду, а выдавалось 128 — вшестеро больше, чем
        // он просил. Порог убран; сколько просит, столько и хочет.
        //
        // Сколько он ПОЛУЧИТ, решает уже общий предел на процесс — см. Budget().
        const size_t bytesPerFrame = (size_t)info_.width * info_.height * 3 / 2;  // NV12
        cacheBudget_ = bytesPerFrame * 60;
        videoDecoders_.fetch_add(1, std::memory_order_relaxed);
        countedAsVideo_ = true;
    }

    lastDecodedFrame_ = -1;
    eofReached_ = false;
    lastPreviewFrame_ = -1;
    lastPreviewW_ = 0;
    lastPreviewH_ = 0;
    try {
        const SourceFingerprint fp = FingerprintSource(utf8Path);
        sourceFp_ = fp.hash;
        sourceFpValid_ = fp.valid;
    } catch (...) {
        // Preview cache is optional. Allocation/path/hash failures must not
        // turn a valid media file into an import failure.
        sourceFp_.fill(0);
        sourceFpValid_ = false;
    }
    return true;
}

bool Decoder::OpenCodec(bool preferHardware) {
    AVStream* st = fmt_->streams[videoStream_];

    auto tryDecoder = [&](const char* name, AVHWDeviceType device,
                          bool hwaccel) -> bool {
        const bool hardware = (device != AV_HWDEVICE_TYPE_NONE);
        const AVCodec* dec = avcodec_find_decoder_by_name(name);
        if (!dec) return false;

        // Для пути «обычный декодер плюс ускоритель» сначала выясняем, умеет
        // ли он такое устройство вовсе. У av1 умеет, у libdav1d нет.
        AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
        if (hwaccel && !HardwareFormatFor(dec, device, &hwFormat)) return false;

        AVCodecContext* ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;

        if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        // Декодирование в несколько потоков — заметно ускоряет программный путь.
        //
        // Но «по числу ядер» на каждый декодер — это по числу ядер НА КАЖДЫЙ
        // ОТКРЫТЫЙ КЛИП. Premiere держит их открытыми столько, сколько лежит
        // на таймлайне, и десять клипов заводят десять полных пулов: и потоки,
        // и буферы кадров внутри dav1d. Сколько это стоит на самом деле —
        // см. замер в ThreadsPerDecoder().
        ctx->thread_count = ThreadsForNewDecoder();

        if (hardware) {
            AVBufferRef* hw = nullptr;
            // Номер адаптера имеет смысл только у D3D11VA: у CUDA и QSV
            // строка устройства значит совсем другое.
            const char* adapter = (device == AV_HWDEVICE_TYPE_D3D11VA)
                                  ? ForcedAdapter() : nullptr;
            if (av_hwdevice_ctx_create(&hw, device, adapter, nullptr, 0) < 0) {
                // Нет такого устройства на этой машине — этот декодер не наш,
                // пробуем следующий, а не открываем его без контекста
                avcodec_free_context(&ctx);
                return false;
            }
            ctx->hw_device_ctx = av_buffer_ref(hw);
            av_buffer_unref(&hw);

            if (hwaccel) {
                ctx->opaque     = (void*)(intptr_t)hwFormat;
                ctx->get_format = PickHardwareFormat;

                // Многопоточность тут ни к чему: разбор идёт на видеокарте,
                // а лишние потоки некоторым ускорителям только мешают.
                ctx->thread_count = 1;
            }
        }

        if (avcodec_open2(ctx, dec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        codec_ = ctx;
        // Имя с добавкой, иначе в отчёте и в журнале «av1 (видеокарта)»
        // читалось бы как программный декодер, которому почему-то приписали
        // видеокарту.
        info_.decoderName = (hwaccel && device == AV_HWDEVICE_TYPE_D3D11VA)
                            ? std::string(name) + " + d3d11va"
                            : name;
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

    // Задан явно — пробуем только его. Тип устройства берём из таблицы:
    // аппаратному декодеру он обязан соответствовать, иначе контекст не
    // создастся и декодер откроется пустым.
    if (const char* forced = ForcedDecoder()) {
        // Отдельно принимаем НАЗВАНИЕ СПОСОБА, а не декодера: у AV1 и VP9
        // декодеры называются по-разному, и «прогнать весь набор через
        // D3D11VA» одним именем иначе не задать.
        if (strcmp(forced, "d3d11va") == 0) {
            for (const HardwareDecoder* hw = codec->hardware; hw->name; ++hw) {
                if (hw->device != AV_HWDEVICE_TYPE_D3D11VA) continue;
                if (tryDecoder(hw->name, hw->device, hw->hwaccel)) return true;
                break;
            }
            SetError("d3d11va forced by AETHER_DECODER did not open");
            return false;
        }

        AVHWDeviceType device = AV_HWDEVICE_TYPE_NONE;
        for (const HardwareDecoder* hw = codec->hardware; hw->name; ++hw) {
            if (strcmp(hw->name, forced) == 0) { device = hw->device; break; }
        }

        // AMD в автоматическом списке нет (см. выше), но задать его руками
        // должно быть можно — иначе проверить исправленную сборку ffmpeg
        // будет нечем.
        if (device == AV_HWDEVICE_TYPE_NONE && strstr(forced, "_amf")) {
            device = AV_HWDEVICE_TYPE_D3D11VA;
        }
        // Фирменный декодер или «обычный плюс ускоритель» — это РАЗНЫЕ пути,
        // и различать их надо по таблице, а не по вопросу «умеет ли он такое
        // устройство». Умеет и av1_cuvid — он тоже отдаёт кадры в память
        // видеокарты, — но ускорителем при этом не пользуется. Спутав их,
        // мы подписывали cuvid как «+ d3d11va» и зря запрещали ему потоки.
        bool hwaccel = false;
        bool known   = false;
        for (const HardwareDecoder* hw = codec->hardware; hw->name; ++hw) {
            if (strcmp(hw->name, forced) == 0) { hwaccel = hw->hwaccel; known = true; break; }
        }

        // Имени нет в таблице — это либо программный декодер, либо тот, что
        // мы из автовыбора убрали. Тогда спрашиваем у него самого.
        if (!known && device != AV_HWDEVICE_TYPE_NONE) {
            AVPixelFormat probe = AV_PIX_FMT_NONE;
            const AVCodec* dec = avcodec_find_decoder_by_name(forced);
            hwaccel = dec && HardwareFormatFor(dec, device, &probe);
        }

        if (tryDecoder(forced, device, hwaccel)) return true;

        SetError(std::string("decoder forced by AETHER_DECODER did not open: ") + forced);
        return false;
    }

    if (preferHardware) {
        for (const HardwareDecoder* hw = codec->hardware; hw->name; ++hw) {
            if (tryDecoder(hw->name, hw->device, hw->hwaccel)) return true;
        }
    }
    for (const char* const* name = codec->software; *name; ++name) {
        if (tryDecoder(*name, AV_HWDEVICE_TYPE_NONE, false)) return true;
    }

    SetError("no decoder available for this codec");
    return false;
}

void Decoder::Close() {
    std::scoped_lock lock(mutex_, audioMutex_);
    CloseLocked();
}

void Decoder::ClearCache() {
    if (cacheBytes_) {
        g_ramCacheBytes.fetch_sub(cacheBytes_, std::memory_order_relaxed);
        cacheBytes_ = 0;
    }
    for (auto& item : frameCache_) {
        av_frame_free(&item.second);
    }
    frameCache_.clear();
}

// Сколько памяти под кэш достаётся ЭТОМУ клипу прямо сейчас.
//
// Раньше бюджет считался только от разрешения и был у каждого клипа свой —
// то есть общего предела не было вовсе. На 1440p это 331 МБ, и десять клипов
// на таймлайне давали три с лишним гигабайта. Premiere держит свой кэш поверх
// нашего, так что отъедать столько нельзя.
//
// Теперь общий предел на процесс делится между живыми декодерами видео.
// Деление грубое, поровну, и это сознательно: узнать, какой клип сейчас
// нужнее, нам неоткуда — хост про это не сообщает, а угадывать по времени
// последнего запроса значит отдавать память тому, кто просто громче стучится.
//
// Предел берётся из настроек (cache_memory_mb / AETHER_CACHE_MB). Ноль —
// кэш выключен, а не «без потолка»: один кадр 8K не должен пробивать лимит.
size_t Decoder::Budget() const {
    const size_t kCeiling = (size_t)MemoryCacheLimitBytes();
    if (kCeiling == 0) return 0;

    const int live = videoDecoders_.load(std::memory_order_relaxed);
    const size_t share = kCeiling / (live > 0 ? (size_t)live : 1u);

    return (cacheBudget_ < share) ? cacheBudget_ : share;
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

    const size_t nbytes = (size_t)size;
    const size_t budget = Budget();
    const size_t ceiling = (size_t)MemoryCacheLimitBytes();
    if (nbytes == 0 || budget == 0 || nbytes > budget || nbytes > ceiling) {
        av_frame_free(&copy);
        return;
    }

    auto dropOne = [&](int64_t around) {
        auto first = frameCache_.begin();
        auto last  = std::prev(frameCache_.end());
        auto drop  = (around - first->first) >= (last->first - around) ? first : last;
        const int dropSize = av_image_get_buffer_size((AVPixelFormat)drop->second->format,
                                                      drop->second->width,
                                                      drop->second->height, 1);
        const size_t freed = dropSize > 0 ? (size_t)dropSize : 0;
        if (freed && cacheBytes_ >= freed) cacheBytes_ -= freed;
        else cacheBytes_ = 0;
        if (freed) g_ramCacheBytes.fetch_sub(freed, std::memory_order_relaxed);
        av_frame_free(&drop->second);
        frameCache_.erase(drop);
    };

    while (cacheBytes_ > budget - nbytes && !frameCache_.empty()) {
        dropOne(index);
    }
    while (g_ramCacheBytes.load(std::memory_order_relaxed) > ceiling - nbytes &&
           !frameCache_.empty()) {
        dropOne(index);
    }
    if (cacheBytes_ > budget - nbytes || !ReserveRamCache(nbytes, ceiling)) {
        av_frame_free(&copy);
        return;
    }

    frameCache_[index] = copy;
    cacheBytes_ += nbytes;
}

void Decoder::CloseLocked() {
    // Снимаемся со счёта раньше всего остального: доля памяти освобождается
    // для соседних клипов сразу, а не после закрытия контекстов
    if (countedAsVideo_) {
        videoDecoders_.fetch_sub(1, std::memory_order_relaxed);
        countedAsVideo_ = false;
    }

    ClearCache();
    sourceFpValid_ = false;
    lastPreviewFrame_ = -1;
    CloseAudioLocked();
    if (sws_)      { sws_freeContext(sws_); sws_ = nullptr; }
    if (swsYuv_)   { sws_freeContext(swsYuv_); swsYuv_ = nullptr; }
    if (swsP010_)  { sws_freeContext(swsP010_); swsP010_ = nullptr; }
    if (packet_)   { av_packet_free(&packet_); }
    if (frame_)    { av_frame_free(&frame_); }
    if (swFrame_)  { av_frame_free(&swFrame_); }
    if (lastFrame_){ av_frame_free(&lastFrame_); }
    if (aheadFrame_){ av_frame_free(&aheadFrame_); }
    if (codec_)    { avcodec_free_context(&codec_); }
    if (fmt_)      { avformat_close_input(&fmt_); }
    path_.clear();

    videoStream_      = -1;
    codecId_          = 0;
    lastDecodedFrame_ = -1;
    eofReached_       = false;
    lastFrameTimeSec_ = 0.0;
    startTimeSec_     = 0.0;
    streamColourspace_ = AVCOL_SPC_UNSPECIFIED;
    streamColourRange_ = AVCOL_RANGE_UNSPECIFIED;
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

    int steps = 0;
    while (true) {
        if (++steps > kMaxDemuxSteps) {
            SetError("too many packets without reaching the requested frame");
            return false;
        }

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
// Какой матрицей переводить яркость и цветность в RGB.
//
// Без этого swscale молча берёт BT.601 — ту, что верна для стандартного
// разрешения прошлого века. Почти всё HD снято в BT.709, и разница видна на
// глаз: однотонный кадр E04030 приезжал как D02F30 вместо DC3E2C, то есть
// на полтора десятка единиц мимо по красному и зелёному.
//
// Когда в файле не сказано ничего, решаем по высоте кадра — так же поступают
// проигрыватели: до 576 строк это материал стандартного разрешения и BT.601,
// выше — BT.709.
namespace {
int SwsColourspaceFor(int colourspace, int height) {
    switch (colourspace) {
        case AVCOL_SPC_BT709:       return SWS_CS_ITU709;
        case AVCOL_SPC_FCC:         return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:     return SWS_CS_ITU624;
        case AVCOL_SPC_SMPTE170M:   return SWS_CS_SMPTE170M;
        case AVCOL_SPC_SMPTE240M:   return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:   return SWS_CS_BT2020;
        default: break;
    }
    return height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}
} // namespace

// Задаётся на каждый кадр, а не при создании пересчётчика, и это осознанно:
// sws_getCachedContext умеет пересоздать контекст незаметно для нас, и любая
// попытка сэкономить здесь означала бы кадр, тихо посчитанный не той матрицей.
// Замер цены — в README.
void Decoder::ApplyColourspace(const AVFrame* src) {
    // По очереди: что сказал кадр, что сказал контейнер, догадка по высоте.
    // Средняя ступень нужна не для красоты: SVT-AV1 не пишет цветовое
    // описание в поток, и на его файлах кадр всегда говорит «не указано» —
    // именно на таком файле первая попытка этой починки ничего не изменила.
    int colourspace = src->colorspace;
    if (colourspace == AVCOL_SPC_UNSPECIFIED) colourspace = streamColourspace_;

    int range = src->color_range;
    if (range == AVCOL_RANGE_UNSPECIFIED) range = streamColourRange_;

    const int csp = SwsColourspaceFor(colourspace, src->height);

    // Полный размах бывает у записей с экрана и у всего, что пришло из
    // JPEG-мира; принять его за урезанный — значит задрать контраст.
    const int srcRange = (range == AVCOL_RANGE_JPEG) ? 1 : 0;

    const int* inv = sws_getCoefficients(csp);
    const int* fwd = sws_getCoefficients(SWS_CS_DEFAULT);

    // Выход у нас RGB, а он всегда полного размаха. Отказ не беда: так
    // отвечают преобразования, которым матрица не нужна вовсе — например
    // когда источник уже в RGB.
    sws_setColorspaceDetails(sws_, inv, srcRange, fwd, 1, 0, 1 << 16, 1 << 16);
}

bool Decoder::ConvertToBGRA(AVFrame* src, uint8_t* dst, int dstStride, int dstW, int dstH,
                            FrameFormat format) {
    AVFrame* srcFrame = ToSystemMemory(src);
    if (!srcFrame) return false;

    if (!UsableFrameSize(dstW, dstH)) {
        SetError("host frame size is not usable");
        return false;
    }

    const auto tConv = std::chrono::steady_clock::now();
    //
    // SWS_BICUBIC, а не BILINEAR: пока масштабирования не было, флаг ни на что
    // не влиял, а при уменьшении разница видна.
    const AVPixelFormat dstFmt = (format == FrameFormat::BGRA16)
                               ? AV_PIX_FMT_BGRA64LE
                               : AV_PIX_FMT_BGRA;

    // Способ интерполяции по тому качеству, которое просит хост.
    //
    // SWS_BICUBIC там, где картинку будут смотреть; SWS_FAST_BILINEAR там, где
    // человек таскает ползунок и Premiere сам попросил уменьшенный кадр.
    //
    // Замер, 1440p, 120 кадров, четыре прогона в обоих порядках, взято лучшее:
    //
    //     размер выхода      bicubic    fast_bilinear
    //     1280x720           326.3 мс      227.8 мс     -30.2%
    //      640x360           269.8 мс      211.6 мс     -21.6%
    //     2560x1440 (как в файле)
    //                        214.6 мс      215.0 мс      +0.2%
    //
    // Последняя строка тут важнее первых двух: при совпадении размеров
    // масштабировать нечего, и флаг не значит НИЧЕГО. То есть качество
    // финального кадра этой правкой не задето вовсе — платит ровно тот
    // случай, ради которого хост поле inQuality и присылает.
    const int wantFlags = (scaling_ == Scaling::Fast) ? SWS_FAST_BILINEAR : SWS_BICUBIC;

    // Пересчётчик кэшируется по всем своим параметрам, включая формат выхода:
    // иначе переключение между 8 и 16 битами молча продолжило бы писать в старом.
    //
    // Флаги в этот список входят отдельной строкой, и это не перестраховка:
    // sws_getCachedContext сверяет только размеры и форматы, а флаги
    // игнорирует. Не сноси мы контекст сами — смена качества не делала бы
    // ничего вовсе, и проверить это было бы нечем.
    if (swsDstFmt_ != dstFmt || swsFlags_ != wantFlags) {
        sws_freeContext(sws_);
        sws_ = nullptr;
        swsDstFmt_ = dstFmt;
        swsFlags_  = wantFlags;
    }
    sws_ = sws_getCachedContext(sws_,
                                srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
                                dstW, dstH, dstFmt,
                                wantFlags, nullptr, nullptr, nullptr);
    if (!sws_) {
        SetError("cannot create colour converter");
        return false;
    }

    ApplyColourspace(srcFrame);

    uint8_t* dstData[4] = { dst, nullptr, nullptr, nullptr };
    int dstLinesize[4]  = { dstStride, 0, 0, 0 };

    sws_scale(sws_, srcFrame->data, srcFrame->linesize, 0, srcFrame->height, dstData, dstLinesize);

    if (format == FrameFormat::BGRA16) {
        // swscale отдаёт полный шестнадцатибитный размах, а Adobe ждёт белое
        // на 32768. (v + 1) >> 1 переводит точно по краям: 65535 -> 32768, 0 -> 0.
        // SSE2: восемь uint16 за такт. На 1440p это ~1 мс против скалярного прохода.
        const int components = dstW * 4;
        const __m128i one = _mm_set1_epi16(1);
        for (int y = 0; y < dstH; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(dst + (ptrdiff_t)y * dstStride);
            int i = 0;
            for (; i + 8 <= components; i += 8) {
                __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
                v = _mm_srli_epi16(_mm_add_epi16(v, one), 1);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(row + i), v);
            }
            for (; i < components; ++i) {
                row[i] = (uint16_t)((row[i] + 1) >> 1);
            }
        }
    }

    stats_.convertMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tConv).count();
    ++stats_.frames;
    return true;
}

namespace {

// Построчное копирование с учётом знака шага.
//
// Отрицательный шаг означает, что буфер идёт снизу вверх, и dst указывает
// на первую строку в порядке записи, а не на верхнюю строку картинки.
// Умножение на ptrdiff_t обязательно: при int-арифметике кадр 4K
// с отрицательным шагом переполняется на середине.
void CopyPlane(uint8_t* dst, int dstStride,
               const uint8_t* src, int srcStride,
               int widthBytes, int height) {
    for (int y = 0; y < height; ++y) {
        memcpy(dst + (ptrdiff_t)y * dstStride,
               src + (ptrdiff_t)y * srcStride,
               (size_t)widthBytes);
    }
}

} // namespace

// Кадр в обычной памяти и заведомо пригодный к работе.
//
// Обе половины этой функции были написаны дважды — в пересчёте в BGRA и
// в выдаче плоскостями, — и с появлением десятибитного пути стали бы писаться
// трижды. Расходятся такие двойники молча (это в проекте уже случалось
// с записью кадра в буфер хоста), поэтому лучше одна функция.
AVFrame* Decoder::ToSystemMemory(AVFrame* src) {
    AVFrame* srcFrame = src;

    // Если декодировала видеокарта, кадр лежит в её памяти — забираем в обычную
    if (src->hw_frames_ctx) {
        const auto t0 = std::chrono::steady_clock::now();
        av_frame_unref(swFrame_);
        if (av_hwframe_transfer_data(swFrame_, src, 0) < 0) {
            SetError("cannot transfer frame from GPU memory");
            return nullptr;
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
        !UsableFrameSize(srcFrame->width, srcFrame->height) ||
        srcFrame->format < 0 ||
        !av_pix_fmt_desc_get((AVPixelFormat)srcFrame->format)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "decoded frame is unusable: %dx%d, pixel format %d",
                 srcFrame->width, srcFrame->height, srcFrame->format);
        SetError(msg);
        return nullptr;
    }
    return srcFrame;
}

bool Decoder::CopyToYUV420(AVFrame* src,
                           uint8_t* dstY, int strideY,
                           uint8_t* dstU, int strideU,
                           uint8_t* dstV, int strideV,
                           int dstW, int dstH) {
    AVFrame* srcFrame = ToSystemMemory(src);
    if (!srcFrame) return false;
    if (!UsableFrameSize(dstW, dstH)) {
        SetError("host frame size is not usable");
        return false;
    }

    const auto tConv = std::chrono::steady_clock::now();

    // Размеры плоскостей цветности округляются ВВЕРХ: у кадра нечётной высоты
    // строк цветности всё равно на одну больше, чем даёт деление нацело,
    // и без округления последняя осталась бы незаполненной полосой.
    const int chromaW = (dstW + 1) / 2;
    const int chromaH = (dstH + 1) / 2;

    // Ради чего всё затевалось: если декодер уже отдал ровно то, что просит
    // хост, работы нет вовсе — только перенос строк. Ни матрицы, ни
    // интерполяции, ни свёртки в RGB.
    if (srcFrame->format == AV_PIX_FMT_YUV420P &&
        srcFrame->width == dstW && srcFrame->height == dstH) {
        CopyPlane(dstY, strideY, srcFrame->data[0], srcFrame->linesize[0], dstW, dstH);
        CopyPlane(dstU, strideU, srcFrame->data[1], srcFrame->linesize[1], chromaW, chromaH);
        CopyPlane(dstV, strideV, srcFrame->data[2], srcFrame->linesize[2], chromaW, chromaH);
    } else {
        // Остальные случаи: кадр с видеокарты приезжает NV12 (цветность
        // вперемешку), а при пониженном качестве воспроизведения хост просит
        // кадр поменьше. И то и другое умеет swscale — но это по-прежнему
        // работа внутри YUV, без перевода в RGB, то есть заметно дешевле
        // прежнего пути.
        // Тот же выбор по качеству, что и у пути в BGRA, и та же оговорка про
        // sws_getCachedContext: флаги он не сверяет, сносить контекст надо самим
        const int wantFlags = (scaling_ == Scaling::Fast) ? SWS_FAST_BILINEAR
                                                          : SWS_BILINEAR;
        if (swsYuvFlags_ != wantFlags) {
            sws_freeContext(swsYuv_);
            swsYuv_ = nullptr;
            swsYuvFlags_ = wantFlags;
        }

        swsYuv_ = sws_getCachedContext(swsYuv_,
                                       srcFrame->width, srcFrame->height,
                                       (AVPixelFormat)srcFrame->format,
                                       dstW, dstH, AV_PIX_FMT_YUV420P,
                                       wantFlags, nullptr, nullptr, nullptr);
        if (!swsYuv_) {
            SetError("cannot create plane converter");
            return false;
        }

        uint8_t* dstData[4]    = { dstY, dstU, dstV, nullptr };
        int      dstLinesize[4] = { strideY, strideU, strideV, 0 };
        sws_scale(swsYuv_, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
                  dstData, dstLinesize);
    }

    stats_.convertMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tConv).count();
    ++stats_.frames;
    return true;
}

bool Decoder::CopyToP010(AVFrame* src,
                         uint8_t* dstY,  int strideY,
                         uint8_t* dstUV, int strideUV,
                         int dstW, int dstH) {
    AVFrame* srcFrame = ToSystemMemory(src);
    if (!srcFrame) return false;
    if (!UsableFrameSize(dstW, dstH)) {
        SetError("host frame size is not usable");
        return false;
    }

    const auto tConv = std::chrono::steady_clock::now();

    // Цветность вдвое меньше по обеим сторонам, с округлением ВВЕРХ: у кадра
    // нечётной высоты строк цветности на одну больше, чем даёт деление нацело.
    // В P010 одна «точка» цветности — это пара uint16, то есть четыре байта.
    const int chromaH = (dstH + 1) / 2;
    const int chromaWidthBytes = ((dstW + 1) / 2) * 2 * (int)sizeof(uint16_t);

    // Ради чего всё. Аппаратные декодеры отдают десять бит СРАЗУ в P010 —
    // ровно в той раскладке, которую ждёт хост. Тогда работы нет вообще:
    // ни матрицы, ни интерполяции, ни свёртки, только перенос строк.
    if (srcFrame->format == AV_PIX_FMT_P010LE &&
        srcFrame->width == dstW && srcFrame->height == dstH) {
        CopyPlane(dstY, strideY, srcFrame->data[0], srcFrame->linesize[0],
                  dstW * (int)sizeof(uint16_t), dstH);
        CopyPlane(dstUV, strideUV, srcFrame->data[1], srcFrame->linesize[1],
                  chromaWidthBytes, chromaH);
    } else {
        // Программный декодер отдаёт yuv420p10le (три плоскости, десять бит
        // в МЛАДШИХ разрядах), а хосту нужен P010 (две плоскости, десять бит
        // в старших). Сдвиг и слияние плоскостей делает swscale — по замеру
        // это 0.42 мс на 1440p против 14.77 у пути через BGRA64.
        const int wantFlags = (scaling_ == Scaling::Fast) ? SWS_FAST_BILINEAR
                                                          : SWS_BILINEAR;
        if (swsP010Flags_ != wantFlags) {
            sws_freeContext(swsP010_);
            swsP010_ = nullptr;
            swsP010Flags_ = wantFlags;
        }

        swsP010_ = sws_getCachedContext(swsP010_,
                                        srcFrame->width, srcFrame->height,
                                        (AVPixelFormat)srcFrame->format,
                                        dstW, dstH, AV_PIX_FMT_P010LE,
                                        wantFlags, nullptr, nullptr, nullptr);
        if (!swsP010_) {
            SetError("cannot create 10-bit plane converter");
            return false;
        }

        uint8_t* dstData[4]     = { dstY, dstUV, nullptr, nullptr };
        int      dstLinesize[4] = { strideY, strideUV, 0, 0 };
        sws_scale(swsP010_, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
                  dstData, dstLinesize);
    }

    stats_.convertMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tConv).count();
    ++stats_.frames;
    return true;
}

void Decoder::SetScaling(Scaling how) {
    std::lock_guard<std::mutex> lock(mutex_);
    scaling_ = how;
}

void Decoder::LimitCacheToFrames(int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames <= 0 || info_.width <= 0 || info_.height <= 0) return;

    const size_t bytesPerFrame = (size_t)info_.width * info_.height * 3 / 2;  // NV12
    const size_t wanted = bytesPerFrame * (size_t)frames;
    if (wanted < cacheBudget_) cacheBudget_ = wanted;
}

bool Decoder::PrefetchFrame(int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmt_ || !info_.hasVideo || !codec_) return false;
    if (frameIndex < 0) frameIndex = 0;

    // Уже лежит — работы нет
    if (frameCache_.count(frameIndex)) return true;

    // Кэш заполняется только при отматывании назад, иначе он был бы пустой
    // тратой памяти на обычном чтении вперёд. Здесь наоборот: смысл всей
    // затеи в том, чтобы кадр дождался запроса, поэтому включаем его явно.
    cacheFill_ = true;
    const bool ok = DecodeUntil(frameIndex);
    cacheFill_ = false;
    return ok;
}

AVFrame* Decoder::AcquireFrameLocked(int64_t frameIndex) {
    if (frameIndex < 0) frameIndex = 0;

    // Отматывание назад — единственный случай, когда кэш окупается: при чтении
    // вперёд кадры и так идут подряд, а кэш только тратил бы память
    const bool backward = (lastRequested_ >= 0 && frameIndex < lastRequested_);
    lastRequested_ = frameIndex;

    // Число живых декодеров могло вырасти с прошлого запроса, а значит доля
    // этого клипа уменьшилась. Подрезаем старый кэш до новой доли до hit:
    // иначе клип, к которому только читают готовые кадры, держал бы прежний
    // большой бюджет бесконечно.
    const size_t budget = Budget();
    while (cacheBytes_ > budget && !frameCache_.empty()) {
        auto first = frameCache_.begin();
        auto last = std::prev(frameCache_.end());
        auto drop = (frameIndex - first->first) >= (last->first - frameIndex)
                  ? first : last;
        const int bytes = av_image_get_buffer_size(
            (AVPixelFormat)drop->second->format,
            drop->second->width, drop->second->height, 1);
        const size_t freed = bytes > 0 ? (size_t)bytes : 0;
        if (freed && cacheBytes_ >= freed) cacheBytes_ -= freed;
        else cacheBytes_ = 0;
        if (freed) g_ramCacheBytes.fetch_sub(freed, std::memory_order_relaxed);
        av_frame_free(&drop->second);
        frameCache_.erase(drop);
    }

    auto cached = frameCache_.find(frameIndex);
    if (cached != frameCache_.end()) {
        lastFrameTimeSec_ = FrameTimeSec(cached->second);
        return cached->second;
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
    if (!ok) return nullptr;
    lastFrameTimeSec_ = FrameTimeSec(frame_);
    return frame_;
}

bool Decoder::GetFrameBGRA(int64_t frameIndex, uint8_t* dst, int dstStride,
                           int dstWidth, int dstHeight, FrameFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmt_ || !dst) {
        SetError("file is not open");
        return false;
    }
    if (!info_.hasVideo || !codec_) {
        SetError("this file has no video");
        return false;
    }

    // Ноль означает «как в файле» — удобно для проверочных программ
    if (dstWidth  <= 0) dstWidth  = info_.width;
    if (dstHeight <= 0) dstHeight = info_.height;
    if (!UsableFrameSize(dstWidth, dstHeight)) {
        SetError("host frame size is not usable");
        return false;
    }

    const Settings set = CurrentSettings();
    const bool reduced = dstWidth < info_.width || dstHeight < info_.height;
    const bool payloadOk = reduced && dstWidth > 0 && dstHeight > 0 &&
        (uint64_t)dstWidth <= (kPreviewCacheMaxPayload / 4ull) / (uint64_t)dstHeight;
    const bool baseEligible = set.previewCache &&
                              format == FrameFormat::BGRA8 &&
                              scaling_ == Scaling::Fast &&
                              payloadOk &&
                              sourceFpValid_;
    const bool sequential =
        baseEligible &&
        lastPreviewFrame_ >= 0 &&
        frameIndex == lastPreviewFrame_ + 1 &&
        dstWidth == lastPreviewW_ &&
        dstHeight == lastPreviewH_;
    if (baseEligible) {
        lastPreviewFrame_ = frameIndex;
        lastPreviewW_ = dstWidth;
        lastPreviewH_ = dstHeight;
    }
    const bool eligible = baseEligible && !sequential;

    if (eligible) {
        try {
            PreviewKeyInput keyIn;
            keyIn.source.hash = sourceFp_;
            keyIn.source.valid = true;
            keyIn.videoStream = videoStream_;
            keyIn.frameIndex = frameIndex;
            keyIn.width = dstWidth;
            keyIn.height = dstHeight;
            keyIn.fastScaling = true;
            keyIn.hardware = info_.hardwareDecode;
            keyIn.decoderName = info_.decoderName;
            keyIn.ffmpegVersion = av_version_info();
            keyIn.aetherVersion = AETHER_VERSION_STR;
            const auto key = MakePreviewKey(keyIn);

            const auto tRead = std::chrono::steady_clock::now();
            if (PreviewCache::Instance().TryRead(key, dstWidth, dstHeight, dst, dstStride)) {
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tRead).count();
                ++stats_.previewCacheHits;
                stats_.previewCacheReadMs += ms;
                ++stats_.frames;
                return true;
            }
        } catch (...) {
            // Any cache failure is a miss. Decode the frame normally.
        }
        ++stats_.previewCacheMisses;
    }

    AVFrame* src = AcquireFrameLocked(frameIndex);
    if (!src) return false;
    if (!ConvertToBGRA(src, dst, dstStride, dstWidth, dstHeight, format)) return false;

    if (eligible) {
        try {
            PreviewKeyInput keyIn;
            keyIn.source.hash = sourceFp_;
            keyIn.source.valid = true;
            keyIn.videoStream = videoStream_;
            keyIn.frameIndex = frameIndex;
            keyIn.width = dstWidth;
            keyIn.height = dstHeight;
            keyIn.fastScaling = true;
            keyIn.hardware = info_.hardwareDecode;
            keyIn.decoderName = info_.decoderName;
            keyIn.ffmpegVersion = av_version_info();
            keyIn.aetherVersion = AETHER_VERSION_STR;
            if (PreviewCache::Instance().QueueWrite(MakePreviewKey(keyIn),
                                                    dstWidth, dstHeight, dst, dstStride)) {
                ++stats_.previewCacheWritesQueued;
            } else {
                ++stats_.previewCacheWritesDropped;
            }
        } catch (...) {
            ++stats_.previewCacheWritesDropped;
        }
    }
    return true;
}

bool Decoder::CanDeliverYUV420() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!info_.hasVideo) return false;

    // Прозрачность в YUV хранить негде — такие файлы идут в BGRA целиком
    if (info_.hasAlpha) return false;

    // Десять бит у Premiere только двухплоскостные (10u_as16u), а доступ
    // к плоскостям в SDK описан для восьмибитных. Оставлено на потом:
    // сперва надо убедиться, что хост берёт хотя бы восьмибитный.
    if (info_.bitDepth > 8) return false;

    // BT.2020 в восьми битах у Premiere нет вовсе — ни одной константы.
    // Файл редкий, но молча отдать его как BT.709 значило бы соврать о цвете.
    if (info_.colourMatrix != AVCOL_SPC_BT709 &&
        info_.colourMatrix != AVCOL_SPC_BT470BG &&
        info_.colourMatrix != AVCOL_SPC_SMPTE170M) {
        return false;
    }
    return true;
}

bool Decoder::GetFrameYUV420(int64_t frameIndex,
                             uint8_t* dstY, int strideY,
                             uint8_t* dstU, int strideU,
                             uint8_t* dstV, int strideV,
                             int dstWidth, int dstHeight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmt_ || !dstY || !dstU || !dstV) {
        SetError("file is not open");
        return false;
    }
    if (!info_.hasVideo || !codec_) {
        SetError("this file has no video");
        return false;
    }

    if (dstWidth  <= 0) dstWidth  = info_.width;
    if (dstHeight <= 0) dstHeight = info_.height;
    if (!UsableFrameSize(dstWidth, dstHeight)) {
        SetError("host frame size is not usable");
        return false;
    }

    AVFrame* src = AcquireFrameLocked(frameIndex);
    if (!src) return false;
    return CopyToYUV420(src, dstY, strideY, dstU, strideU, dstV, strideV,
                        dstWidth, dstHeight);
}

// Десять бит родными двумя плоскостями.
//
// Условия жёстче, чем у восьмибитного пути, и каждое по своей причине:
//
//   * ровно десять бит. Двенадцатибитных констант у Premiere нет вовсе,
//     а отдать двенадцать под видом десяти значит потерять два разряда молча;
//   * без прозрачности — в YUV её негде хранить, как и в восьми битах;
//   * матрица BT.709 или BT.2020. Других десятибитных вариантов у Premiere
//     нет, а назвать BT.601 семьсот девятой значит соврать о цвете.
//
// Кривая переноса тут НЕ ограничение: у BT.2020 Premiere различает обычную,
// PQ и HLG отдельными константами, и все три мы умеем назвать.
bool Decoder::CanDeliverP010() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!info_.hasVideo) return false;
    if (info_.hasAlpha)  return false;
    if (info_.bitDepth != 10) return false;

    return info_.colourMatrix == AVCOL_SPC_BT709 ||
           info_.colourMatrix == AVCOL_SPC_BT2020_NCL ||
           info_.colourMatrix == AVCOL_SPC_BT2020_CL;
}

bool Decoder::GetFrameP010(int64_t frameIndex,
                           uint8_t* dstY,  int strideY,
                           uint8_t* dstUV, int strideUV,
                           int dstWidth, int dstHeight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmt_ || !dstY || !dstUV) {
        SetError("file is not open");
        return false;
    }
    if (!info_.hasVideo || !codec_) {
        SetError("this file has no video");
        return false;
    }

    if (dstWidth  <= 0) dstWidth  = info_.width;
    if (dstHeight <= 0) dstHeight = info_.height;
    if (!UsableFrameSize(dstWidth, dstHeight)) {
        SetError("host frame size is not usable");
        return false;
    }

    AVFrame* src = AcquireFrameLocked(frameIndex);
    if (!src) return false;
    return CopyToP010(src, dstY, strideY, dstUV, strideUV, dstWidth, dstHeight);
}

// ---------------------------------------------------------------------------
// Звук
// ---------------------------------------------------------------------------

void Decoder::CloseAudioDecoderLocked() {
    if (swr_)         { swr_free(&swr_); }
    if (audioPacket_) { av_packet_free(&audioPacket_); }
    if (audioFrame_)  { av_frame_free(&audioFrame_); }
    if (audioCodec_)  { avcodec_free_context(&audioCodec_); }

    audioStreamIndex_ = -1;
    audioOrdinal_     = -1;
    pending_.clear();
    pendingOffset_ = 0;
    pendingCount_  = 0;
    audioCursor_   = -1;
    audioExpectSample_ = -1;
}

void Decoder::CloseAudioLocked() {
    CloseAudioDecoderLocked();
    if (audioFmt_)    { avformat_close_input(&audioFmt_); }
}

bool Decoder::HasAudio() const
{
    std::lock_guard<std::mutex> lock(audioMutex_);
    return audioCodec_ != nullptr;
}

int Decoder::OpenAudioTrack() const
{
    std::lock_guard<std::mutex> lock(audioMutex_);
    return audioOrdinal_;
}

bool Decoder::OpenAudio(int ordinal) {
    // Оба замка: читаем путь из состояния видео, а меняем состояние звука
    std::scoped_lock lock(mutex_, audioMutex_);

    if (!fmt_ || path_.empty()) {
        SetAudioError("file is not open");
        return false;
    }

    if (audioOrdinal_ == ordinal && audioCodec_)
        return true;

    // Декодер сбрасываем, контейнер — нет.
    //
    // Раньше на каждую дорожку файл открывался заново: CloseAudio закрывал
    // audioFmt_, OpenContainer открывал тот же путь, пока fmt_ его ещё держал.
    // На CI это умирало нарушением доступа (0xC0000005) сразу после успешного
    // чтения дорожки 0 — до первой строки про дорожку 1. Два файла из набора:
    // audio_only.mka (два FLAC, без видео) и audio_tracks_differ.mp4 (стерео
    // и моно). Тот же av1_8bit.mp4 с двумя одинаковыми AAC проходил.
    // Локально не воспроизводится; в августе 2026 это уже краснело три
    // прогона подряд и тогда «лечилось» пином образа и сборки ffmpeg.
    // Пин на месте, падение вернулось.
    //
    // Premiere и так перебирает дорожки на одном клипе. Держать демультиплексор
    // и менять только декодер — то, что хост делает по смыслу, плюс не
    // закрывать-открывать тот же файл на Windows Server, где это падает.
    CloseAudioDecoderLocked();

    int err = 0;
    if (!audioFmt_) {
        // Свой разбор контейнера для звука. Общий с видео не годится: чтение
        // пакетов двигает одну общую позицию, и перемотка видео сбивала бы звук.
        err = OpenContainer(&audioFmt_, path_.c_str());
        if (err < 0) {
            SetAudioError("cannot open file for audio", err);
            return false;
        }
    } else {
        av_seek_frame(audioFmt_, -1, 0, AVSEEK_FLAG_BACKWARD);
    }

    // Найти N-ю по счёту звуковую дорожку. Вынесено, потому что зовётся
    // дважды: до полного разбора и, если тот понадобился, после.
    auto findOrdinal = [this](int nth) -> int {
        int seen = 0;
        for (unsigned i = 0; i < audioFmt_->nb_streams; ++i) {
            if (audioFmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            if (seen++ == nth) return (int)i;
        }
        return -1;
    };

    // Полный разбор запускаем ТОЛЬКО если заголовка не хватило.
    //
    // Этот же файл уже открыт под видео, и там avformat_find_stream_info
    // отработал полностью. Второй раз он нужен ради того, чего нет
    // в заголовке, — а у MP4, MOV и Matroska кодек, число каналов и частота
    // в заголовке есть. Проверяем это на самой дорожке, а не предполагаем
    // по имени контейнера: не хватило — честно разбираем, как разбирали.
    //
    // Сколько это стоит на самом деле — замерено, потому что первая оценка
    // была сильно завышена. «До пяти секунд» это ПРЕДЕЛ ffmpeg, а не цена:
    //
    //     файл                       open_input   find_stream_info
    //     запись 42 ГБ, .mov            48.5 мс           2.6 мс
    //     запись 16 ГБ, .mov            27.0 мс           1.8 мс
    //     запись  3 ГБ, .mp4            20.2 мс           3.0 мс
    //     проверочные файлы          0.2-0.5 мс       0.2-1.2 мс
    //
    // То есть выигрыш — единицы миллисекунд на открытие, и на записи
    // с четырьмя дорожками это около двух десятков. Дорого стоит сам
    // open_input, и с ним сделать нечего.
    //
    // ⚠ И вот что замер показал попутно, а это уже не про скорость.
    // Длительность дорожки в заголовке и после разбора СОВПАДАЕТ НЕ ВСЕГДА:
    // на записи ACBlackFlag заголовок говорит 14319616, разбор уточняет до
    // 14320800 — разница около 25 мс звука. Поверь мы заголовку, дорожка
    // приехала бы к Premiere короче, чем она есть, то есть с обрезанным
    // хвостом. Поэтому длину ниже берём НЕ отсюда, а из видеоконтекста,
    // который разобран полностью.
    audioStreamIndex_ = findOrdinal(ordinal);
    bool headerIsEnough = false;
    if (audioStreamIndex_ >= 0) {
        const AVCodecParameters* cp = audioFmt_->streams[audioStreamIndex_]->codecpar;
        headerIsEnough = cp->codec_id != AV_CODEC_ID_NONE &&
                         cp->sample_rate > 0 &&
                         cp->ch_layout.nb_channels > 0;
    }

    if (!headerIsEnough) {
        if ((err = avformat_find_stream_info(audioFmt_, nullptr)) < 0) {
            SetAudioError("cannot read audio stream info", err);
            CloseAudioLocked();
            return false;
        }
        audioStreamIndex_ = findOrdinal(ordinal);
    }

    if (audioStreamIndex_ < 0) {
        SetAudioError("no audio track with that index");
        CloseAudioDecoderLocked();
        return false;
    }

    // Тот же поток, но в полностью разобранном контексте видео. Порядок
    // потоков в одном и том же файле у одного и того же демультиплексора
    // один и тот же, поэтому номер подходит обоим; проверка на длину — на
    // случай, если это когда-нибудь перестанет быть правдой.
    const AVStream* analysed =
        ((unsigned)audioStreamIndex_ < fmt_->nb_streams &&
         fmt_->streams[audioStreamIndex_]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            ? fmt_->streams[audioStreamIndex_]
            : nullptr;

    AVStream* st = audioFmt_->streams[audioStreamIndex_];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        SetAudioError("no decoder for this audio track");
        CloseAudioDecoderLocked();
        return false;
    }

    audioCodec_ = avcodec_alloc_context3(dec);
    if (!audioCodec_ ||
        avcodec_parameters_to_context(audioCodec_, st->codecpar) < 0 ||
        avcodec_open2(audioCodec_, dec, nullptr) < 0) {
        SetAudioError("cannot open audio decoder");
        CloseAudioDecoderLocked();
        return false;
    }

    const int channels = audioCodec_->ch_layout.nb_channels;
    const int rate     = audioCodec_->sample_rate;
    if (channels <= 0 || channels > kMaxAudioChannels || rate <= 0) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "audio layout is not usable (%d channels at %d Hz; more than %d channels is refused)",
                 channels, rate, kMaxAudioChannels);
        SetAudioError(msg);
        CloseAudioDecoderLocked();
        return false;
    }

    info_.audioChannels    = channels;
    info_.audioSampleRate  = rate;

    // Длину берём из разобранного контекста, а из своего — только если того
    // почему-то нет. Разница между ними невелика (около 25 мс на проверенной
    // записи), но она вся в конце дорожки: короче объявишь — короче и приедет.
    const AVStream* forDuration = analysed ? analysed : st;
    info_.audioSampleCount = forDuration->duration != AV_NOPTS_VALUE
        ? (int64_t)(forDuration->duration * av_q2d(forDuration->time_base)
                    * info_.audioSampleRate + 0.5)
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
        SetAudioError("cannot set up audio conversion", err);
        CloseAudioDecoderLocked();
        return false;
    }

    audioFrame_  = av_frame_alloc();
    audioPacket_ = av_packet_alloc();
    if (!audioFrame_ || !audioPacket_) {
        SetAudioError("out of memory for audio buffers");
        CloseAudioDecoderLocked();
        return false;
    }

    pending_.assign(info_.audioChannels, std::vector<float>());
    audioCursor_ = -1;
    audioOrdinal_ = ordinal;   // запоминаем ПОСЛЕ успеха: на отказе он остаётся -1
    return true;
}

// Декодировать очередной кадр звука в очередь готовых отсчётов
bool Decoder::DecodeMoreAudio() {
    int steps = 0;
    while (true) {
        if (++steps > kMaxDemuxSteps) {
            SetAudioError("too many packets without filling the audio request");
            return false;
        }

        int err = avcodec_receive_frame(audioCodec_, audioFrame_);

        if (err == 0) {
            const int n = audioFrame_->nb_samples;

            // Число дорожек берём по МЕНЬШЕМУ из двух: сколько каналов
            // объявлено и сколько буферов на самом деле есть.
            //
            // Раньше индекс брался только из info_.audioChannels, и если бы
            // они разошлись — pending_[c] уходил бы за пределы вектора. Такое
            // расхождение мы считаем невозможным, но «невозможное» здесь
            // означает падение всего Premiere с нарушением доступа, а не
            // ошибку импорта. Цена проверки — одно сравнение на кадр.
            const int channels = std::min<int>({ info_.audioChannels,
                                                 (int)pending_.size(),
                                                 kMaxAudioChannels });
            if (channels <= 0) {
                SetAudioError("audio buffers are not ready");
                return false;
            }

            // Сколько отсчётов конвертер реально запишет. При той же частоте
            // это обычно n, но фильтр может выдать больше — и тогда resize(n)
            // это запись мимо буфера, а падение приходит позже, на free.
            int outCount = swr_get_out_samples(swr_, n);
            if (outCount < n) outCount = n;
            if (outCount <= 0 || outCount > (1 << 20)) {
                SetAudioError("audio conversion buffer size is not usable");
                return false;
            }
            for (int c = 0; c < channels; ++c) pending_[c].resize((size_t)outCount);

            float* out[kMaxAudioChannels] = {};
            for (int c = 0; c < channels; ++c) out[c] = pending_[c].data();

            const int got = swr_convert(swr_, reinterpret_cast<uint8_t**>(out), outCount,
                                        const_cast<const uint8_t**>(audioFrame_->data), n);
            if (got < 0) {
                SetAudioError("audio conversion error", got);
                return false;
            }
            if (got > (int)pending_[0].size()) {
                SetAudioError("audio conversion wrote past the buffer");
                return false;
            }

            // Позицию берём из метки времени: так не накапливается ошибка
            // после перемотки и не важен порядок пакетов.
            //
            // Отсчёт номер ноль — это общий ноль клипа, тот же, от которого
            // считаются кадры. Без вычитания startTimeSec_ файл, начинающийся
            // не с нуля, отдавал одну тишину: запрошенный отсчёт 0 оказывался
            // за сотни тысяч отсчётов до всего, что есть в файле.
            //
            // Мелкие налегания/дыры из‑за миллисекундной сетки Matroska на
            // AAC сшивает GetAudio по audioExpectSample_ — здесь трогать
            // нельзя: преролл после перемотки иначе сдвигает всю дорожку.
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
            SetAudioError("audio decode error", err);
            return false;
        }

        av_packet_unref(audioPacket_);
        int rerr = av_read_frame(audioFmt_, audioPacket_);
        if (rerr == AVERROR_EOF) {
            avcodec_send_packet(audioCodec_, nullptr);  // слить остаток
            continue;
        }
        if (rerr < 0) {
            SetAudioError("audio read error", rerr);
            return false;
        }
        if (audioPacket_->stream_index != audioStreamIndex_) continue;

        if (avcodec_send_packet(audioCodec_, audioPacket_) < 0) {
            SetAudioError("audio decoder rejected packet");
            return false;
        }
    }
}

bool Decoder::GetAudio(int64_t startSample, int32_t sampleCount, float* const* dst,
                       int dstChannels) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (!audioCodec_ || !dst) {
        SetAudioError("audio is not open");
        return false;
    }
    if (startSample < 0) startSample = 0;

    // Столько каналов, сколько объявлено хосту: буферы под них выделил он,
    // и меньше писать нельзя — незаполненный канал останется мусором.
    int channels = info_.audioChannels;
    if (channels <= 0 || channels > kMaxAudioChannels || sampleCount <= 0) {
        SetAudioError("audio request makes no sense");
        return false;
    }

    // Больше, чем выделил вызывающий, — тоже нельзя, и это не симметричная
    // придирка: лишний канал пишется В ЧУЖУЮ ПАМЯТЬ. Расхождение само по себе
    // означает поломку выше, поэтому о нём говорим вслух, а не молча
    // подрезаем: тихая подрезка выглядела бы как пропавший канал, и искали
    // бы её в декодере, где её нет.
    if (dstChannels > 0 && dstChannels < channels) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "caller has %d channel buffers but the track has %d - writing %d",
                 dstChannels, channels, dstChannels);
        SetAudioError(msg);
        channels = dstChannels;
    }

    // Тишина по умолчанию: если файл кончился раньше запроса, Premiere получит
    // нули, а не остатки прошлого куска
    for (int c = 0; c < channels; ++c) {
        std::fill_n(dst[c], sampleCount, 0.0f);
    }

    // А вот ЧИТАТЬ можно только оттуда, где есть буферы. Разойтись эти два
    // числа не должны, но если разойдутся — пусть будет тишина, а не падение.
    const int readable = std::min<int>({ channels, (int)pending_.size(),
                                         kMaxAudioChannels });

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
            SetAudioError("audio seek failed");
            return false;
        }
        avcodec_flush_buffers(audioCodec_);
        pendingCount_  = 0;
        pendingOffset_ = 0;
        audioCursor_   = -1;
        audioExpectSample_ = -1;
    }

    int64_t written = 0;
    while (written < sampleCount) {
        if (pendingCount_ <= 0) {
            if (!DecodeMoreAudio()) break;   // файл кончился — остаток останется тишиной
        }

        int64_t chunkStart = audioCursor_ + pendingOffset_;
        const int64_t want = startSample + written;

        // Стык AAC в Matroska: метка кадра чуть раньше или позже ожидаемой
        // из‑за округления до 1 мс. Двигаем начало кадра к ожиданию — только
        // если уже отдавали звук подряд. После перемотки expect = -1, и
        // прероллу верим по меткам, иначе сдвинется вся дорожка.
        if (audioExpectSample_ >= 0 && pendingOffset_ == 0 &&
            want == audioExpectSample_) {
            const int64_t delta = chunkStart - audioExpectSample_;
            const int64_t slop = std::max<int64_t>(info_.audioSampleRate / 500, 48);
            if (delta > 0 && delta <= slop)
                chunkStart = audioExpectSample_;
        }

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
            audioExpectSample_ = startSample + written;
            continue;
        }

        const int64_t skip = want - chunkStart;
        int64_t take = std::min<int64_t>(pendingCount_ - skip, sampleCount - written);

        // Сколько отсчётов ФИЗИЧЕСКИ лежит в буфере за вычетом уже отданных.
        //
        // Выше проверки гарантируют 0 <= skip < pendingCount_, а pendingCount_
        // не может превысить размер буфера — но гарантируют они это через
        // цепочку допущений о том, что вернул swr_convert и что стоит в метках
        // времени. Отрицательный или слишком большой take превращает memcpy
        // в запись мимо памяти: внутри Premiere это не ошибка импорта,
        // а закрывшийся Premiere.
        const int64_t have = pending_.empty()
                           ? 0
                           : (int64_t)pending_[0].size() - (pendingOffset_ + skip);
        if (take > have) take = have;

        if (take <= 0) {
            // Брать нечего — дальше по этому куску идти некуда
            pendingCount_ = 0;
            continue;
        }

        for (int c = 0; c < readable; ++c) {
            memcpy(dst[c] + written,
                   pending_[c].data() + pendingOffset_ + skip,
                   (size_t)take * sizeof(float));
        }

        written        += take;
        pendingOffset_ += skip + take;
        pendingCount_  -= skip + take;
        audioExpectSample_ = startSample + written;
    }

    return true;
}

} // namespace av1imp
