// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ffmpeg_stub.cpp -- iOS-only stand-in for the real FFmpeg static libraries.
//
// externals/ffmpeg-core/CMakeLists.txt downloads a prebuilt matching `APPLE`,
// which doesn't distinguish macOS from iOS -- the only prebuilt it ever fetches
// is macOS-targeted, and its .a files contain macOS object code that cannot be
// linked into an iOS binary (`ld: building for iOS, but linking in object file
// ... built for macOS`). A genuine iOS cross-compile or iOS-targeted prebuilt
// isn't available from that fetch source. Real FFmpeg-for-iOS support is real,
// separate future work (a cross-compile of FFmpeg itself, or sourcing a
// genuine iOS-built release) -- this file is a stopgap, not that fix.
//
// This provides real, correctly-typed (checked against FFmpeg's actual headers
// below, not hand-transcribed) definitions for exactly the FFmpeg symbols the
// codebase currently references (see src/core/libraries/ajm/ajm_mp3.cpp,
// src/core/libraries/avplayer/*, src/core/libraries/videodec/*), each
// returning FFmpeg's own documented failure sentinel for that function
// (nullptr from allocators/openers, a negative AVERROR from status-returning
// calls). The calling code already has to handle real FFmpeg failures during
// normal operation (a codec can fail to open, a stream can fail to parse), so
// it routes through existing, real error-handling paths -- this does not add
// a new "iOS-only" failure mode for callers to separately account for. Net
// effect: MP3/video-cutscene/audio-codec decode fails cleanly and audibly
// (LOG_ERROR from the existing call sites) instead of the app failing to link
// at all.
//
// TODO(iOS FFmpeg): replace this file by either cross-compiling real FFmpeg
// for arm64-apple-ios, or sourcing a genuine iOS-targeted prebuilt, then
// delete this file and its CMake wiring (see externals/CMakeLists.txt and the
// FFmpeg::ffmpeg target's iOS branch in the top-level CMakeLists.txt).

#include <cstddef>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" {

// ---- libavutil ----

AVDictionaryEntry* av_dict_get(const AVDictionary*, const char*, const AVDictionaryEntry*, int) {
    return nullptr;
}

AVFrame* av_frame_alloc(void) {
    return nullptr;
}

void av_frame_free(AVFrame** frame) {
    if (frame) {
        *frame = nullptr;
    }
}

int av_frame_get_buffer(AVFrame*, int) {
    return AVERROR(ENOMEM);
}

void* av_malloc(size_t) {
    return nullptr;
}

int av_strerror(int errnum, char* errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size > 0) {
        errbuf[0] = '\0';
    }
    return errnum;
}

// ---- libavcodec ----

AVPacket* av_packet_alloc(void) {
    return nullptr;
}

void av_packet_free(AVPacket** pkt) {
    if (pkt) {
        *pkt = nullptr;
    }
}

void av_parser_close(AVCodecParserContext*) {}

AVCodecParserContext* av_parser_init(int) {
    return nullptr;
}

int av_parser_parse2(AVCodecParserContext*, AVCodecContext*, uint8_t**, int*, const uint8_t*,
                     int, int64_t, int64_t, int64_t) {
    return AVERROR(ENOSYS);
}

AVCodecContext* avcodec_alloc_context3(const AVCodec*) {
    return nullptr;
}

const AVCodec* avcodec_find_decoder(enum AVCodecID) {
    return nullptr;
}

void avcodec_flush_buffers(AVCodecContext*) {}

void avcodec_free_context(AVCodecContext** avctx) {
    if (avctx) {
        *avctx = nullptr;
    }
}

int avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**) {
    return AVERROR(ENOSYS);
}

int avcodec_parameters_to_context(AVCodecContext*, const AVCodecParameters*) {
    return AVERROR(ENOSYS);
}

int avcodec_receive_frame(AVCodecContext*, AVFrame*) {
    return AVERROR(ENOSYS);
}

int avcodec_send_packet(AVCodecContext*, const AVPacket*) {
    return AVERROR(ENOSYS);
}

// ---- libavformat ----

AVFormatContext* avformat_alloc_context(void) {
    return nullptr;
}

void avformat_close_input(AVFormatContext** ctx) {
    if (ctx) {
        *ctx = nullptr;
    }
}

int avformat_find_stream_info(AVFormatContext*, AVDictionary**) {
    return AVERROR(ENOSYS);
}

int avformat_open_input(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**) {
    return AVERROR(ENOSYS);
}

int av_read_frame(AVFormatContext*, AVPacket*) {
    return AVERROR_EOF;
}

int64_t av_rescale_q(int64_t, AVRational, AVRational) {
    return 0;
}

int avformat_seek_file(AVFormatContext*, int, int64_t, int64_t, int64_t, int) {
    return AVERROR(ENOSYS);
}

AVIOContext* avio_alloc_context(unsigned char*, int, int,
                                void*, int (*)(void*, uint8_t*, int),
                                int (*)(void*, const uint8_t*, int),
                                int64_t (*)(void*, int64_t, int)) {
    return nullptr;
}

void avio_context_free(AVIOContext** ctx) {
    if (ctx) {
        *ctx = nullptr;
    }
}

int64_t avio_seek(AVIOContext*, int64_t, int) {
    return AVERROR(ENOSYS);
}

// ---- libswresample ----

int swr_alloc_set_opts2(SwrContext**, const AVChannelLayout*, enum AVSampleFormat, int,
                        const AVChannelLayout*, enum AVSampleFormat, int, int, void*) {
    return AVERROR(ENOSYS);
}

int swr_convert_frame(SwrContext*, AVFrame*, const AVFrame*) {
    return AVERROR(ENOSYS);
}

void swr_free(SwrContext** s) {
    if (s) {
        *s = nullptr;
    }
}

int swr_init(SwrContext*) {
    return AVERROR(ENOSYS);
}

// ---- libswscale ----

void sws_freeContext(SwsContext*) {}

struct SwsContext* sws_getContext(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat,
                                  int, SwsFilter*, SwsFilter*, const double*) {
    return nullptr;
}

int sws_scale(SwsContext*, const uint8_t* const*, const int*, int, int, uint8_t* const*,
             const int*) {
    return AVERROR(ENOSYS);
}

} // extern "C"
