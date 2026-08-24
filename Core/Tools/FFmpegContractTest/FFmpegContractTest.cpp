extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstdint>

static_assert(UINTPTR_MAX == UINT64_MAX, "The native FFmpeg contract must be compiled for x64.");

int main()
{
    const char *version = av_version_info();
    if (version == nullptr || version[0] == '\0') {
        std::fputs("FFmpeg version information is unavailable.\n", stderr);
        return 1;
    }

    if (avcodec_version() == 0 || avformat_version() == 0 || avutil_version() == 0 || swresample_version() == 0
        || swscale_version() == 0) {
        std::fputs("One or more required FFmpeg libraries failed to report a version.\n", stderr);
        return 1;
    }

    AVFormatContext *format_context = avformat_alloc_context();
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwrContext *resampler = swr_alloc();
    SwsContext *scaler = sws_alloc_context();
    [[maybe_unused]] const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_NONE);

    if (format_context == nullptr || packet == nullptr || frame == nullptr || resampler == nullptr || scaler == nullptr) {
        std::fputs("One or more representative FFmpeg allocations failed.\n", stderr);
        avformat_free_context(format_context);
        av_packet_free(&packet);
        av_frame_free(&frame);
        swr_free(&resampler);
        sws_freeContext(scaler);
        return 1;
    }

    avformat_free_context(format_context);
    av_packet_free(&packet);
    av_frame_free(&frame);
    swr_free(&resampler);
    sws_freeContext(scaler);

    std::printf("FFmpeg SDK contract passed (%s).\n", version);
    return 0;
}
