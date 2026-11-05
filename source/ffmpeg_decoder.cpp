#include "ffmpeg_decoder.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

namespace ntv {

struct FfmpegDecoder::Impl {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int stream_index = -1;
    bool at_eof = false;
    std::vector<int16_t> pending;
    size_t pending_offset = 0;

    ~Impl() {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }
};

FfmpegDecoder::FfmpegDecoder() : impl_(new Impl()) {}

FfmpegDecoder::~FfmpegDecoder() {
    delete impl_;
}

bool FfmpegDecoder::open(const std::vector<unsigned char> &data, const std::string &ext_hint) {
    Impl &d = *impl_;

    // A custom AVIO over the in-memory buffer: no temp files, no seek support,
    // which is fine because we decode linearly.
    auto *ctx = avformat_alloc_context();
    if (!ctx) return false;

    struct MemIo {
        const unsigned char *data;
        size_t size;
        size_t pos;
    };
    auto *mem = new MemIo{data.data(), data.size(), 0};

    auto read_packet = [](void *opaque, uint8_t *buf, int buf_size) -> int {
        auto *m = static_cast<MemIo *>(opaque);
        if (m->pos >= m->size) return AVERROR_EOF;
        size_t n = m->size - m->pos;
        if (n > (size_t)buf_size) n = (size_t)buf_size;
        memcpy(buf, m->data + m->pos, n);
        m->pos += n;
        return (int)n;
    };

    unsigned char *iobuf = static_cast<unsigned char *>(av_malloc(32768));
    if (!iobuf) return false;

    AVIOContext *avio =
        avio_alloc_context(iobuf, 32768, 0, mem, read_packet, nullptr, nullptr);
    if (!avio) {
        av_free(iobuf);
        return false;
    }
    ctx->pb = avio;

    (void)ext_hint;
    if (avformat_open_input(&ctx, nullptr, nullptr, nullptr) != 0) {
        avio_context_free(&avio);
        return false;
    }
    d.fmt = ctx;

    if (avformat_find_stream_info(d.fmt, nullptr) < 0) return false;

    d.stream_index = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d.stream_index < 0) return false;

    AVStream *stream = d.fmt->streams[d.stream_index];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return false;
    const char *codec_name = codec->name ? codec->name : "unknown";

    d.codec = avcodec_alloc_context3(codec);
    if (!d.codec) return false;
    if (avcodec_parameters_to_context(d.codec, stream->codecpar) < 0) return false;
    if (avcodec_open2(d.codec, codec, nullptr) < 0) return false;

    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (!d.packet || !d.frame) return false;

    rate_ = d.codec->sample_rate > 0 ? d.codec->sample_rate : 44100;
    channels_ = d.codec->ch_layout.nb_channels > 0 ? d.codec->ch_layout.nb_channels : 2;

    if (stream->duration > 0) {
        total_frames_ = av_rescale_q(stream->duration, stream->time_base,
                                     AVRational{1, rate_});
    }

    logf("[ffmpeg] codec=%s rate=%d ch=%d total=%llu", codec_name, rate_, channels_,
         (unsigned long long)total_frames_);
    return true;
}

bool FfmpegDecoder::seek(double seconds) {
    Impl &d = *impl_;
    if (!d.fmt || d.stream_index < 0) return false;
    AVStream *stream = d.fmt->streams[d.stream_index];
    int64_t ts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
                              stream->time_base);
    if (av_seek_frame(d.fmt, d.stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(d.codec);
    d.at_eof = false;
    return true;
}

int FfmpegDecoder::read(int16_t *out, int frames) {
    Impl &d = *impl_;
    int written = 0;

    while (written < frames) {
        // Drain whatever we already converted.
        if (d.pending_offset < d.pending.size()) {
            size_t avail = (d.pending.size() - d.pending_offset) / channels_;
            int take = std::min((size_t)(frames - written), avail);
            memcpy(out + (size_t)written * channels_,
                   d.pending.data() + d.pending_offset, (size_t)take * channels_ * sizeof(int16_t));
            d.pending_offset += (size_t)take * channels_;
            written += take;
            continue;
        }

        if (d.at_eof) break;

        int ret = avcodec_receive_frame(d.codec, d.frame);
        if (ret == AVERROR(EAGAIN)) {
            int got = av_read_frame(d.fmt, d.packet);
            if (got < 0) {
                avcodec_send_packet(d.codec, nullptr);
                d.at_eof = true;
                continue;
            }
            if (d.packet->stream_index == d.stream_index) {
                avcodec_send_packet(d.codec, d.packet);
            }
            av_packet_unref(d.packet);
            continue;
        }
        if (ret == AVERROR_EOF) {
            d.at_eof = true;
            break;
        }
        if (ret < 0) break;

        // Convert planar/float samples to interleaved s16.
        int sample_fmt = d.frame->format;
        int ch = d.frame->ch_layout.nb_channels > 0 ? d.frame->ch_layout.nb_channels : 2;
        int samples = d.frame->nb_samples;
        d.pending.assign((size_t)samples * ch, 0);

        if (sample_fmt == AV_SAMPLE_FMT_FLTP && d.frame->data[0]) {
            for (int c = 0; c < ch && c < 8; ++c) {
                const float *src = reinterpret_cast<const float *>(d.frame->data[c]);
                if (!src) continue;
                for (int i = 0; i < samples; ++i) {
                    float v = src[i];
                    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
                    d.pending[(size_t)i * ch + c] = (int16_t)(v * 32767.0f);
                }
            }
        } else if (sample_fmt == AV_SAMPLE_FMT_S16P && d.frame->data[0]) {
            for (int c = 0; c < ch && c < 8; ++c) {
                const int16_t *src = reinterpret_cast<const int16_t *>(d.frame->data[c]);
                if (!src) continue;
                for (int i = 0; i < samples; ++i) {
                    d.pending[(size_t)i * ch + c] = src[i];
                }
            }
        } else if (sample_fmt == AV_SAMPLE_FMT_S16 && d.frame->data[0]) {
            memcpy(d.pending.data(), d.frame->data[0],
                   (size_t)samples * ch * sizeof(int16_t));
        }
        d.pending_offset = 0;
        av_frame_unref(d.frame);
    }

    return written;
}

} // namespace ntv
