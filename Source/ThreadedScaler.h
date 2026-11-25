#pragma once
#include "pch.h"
#include <algorithm>
#include <thread>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// Pixel format conversion with swscale's slice threads (sws_alloc_context + "threads" + sws_scale_frame),
// into planes the caller owns (a buffer, or a mapped texture). For conversions that are more than a
// plain interleave (4:2:2/4:4:4 to 4:2:0, more than 8 bits to P010), which a single-threaded sws_scale
// does on one core. The destination takes the source's colour description, so the conversion changes
// the format and size only, never the colour space or range.
class ThreadedScaler
{
public:
    ThreadedScaler() = default;
    ThreadedScaler(const ThreadedScaler&) = delete;
    ThreadedScaler& operator=(const ThreadedScaler&) = delete;
    ~ThreadedScaler() { sws_free_context(&context); }

    // Slice threads: one per hardware thread, at most four (the console has four shared cores).
    static int DefaultThreads()
    {
        unsigned hardware = std::thread::hardware_concurrency();
        return static_cast<int>((std::max)(1u, (std::min)(hardware, 4u)));
    }

    // Whether a software frame is worth the threaded path: anything but 8-bit 4:2:0 (and no hardware
    // frames, which swscale can't read).
    static bool Wanted(AVPixelFormat input)
    {
        if (input == AV_PIX_FMT_NONE || input == AV_PIX_FMT_YUV420P || input == AV_PIX_FMT_YUVJ420P) return false;
        auto description = av_pix_fmt_desc_get(input);
        return description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL);
    }

    // Converts `source` into `format` at width x height, written to data/linesize. >= 0 on success.
    int Scale(const AVFrame* source, uint8_t* const data[4], const int linesize[4], AVPixelFormat format, int width, int height,
        int flags = SWS_BILINEAR)
    {
        if (!context || flags != contextFlags)
        {
            sws_free_context(&context);
            context = sws_alloc_context();
            if (!context) return AVERROR(ENOMEM);
            av_opt_set_int(context, "threads", DefaultThreads(), 0);
            av_opt_set_int(context, "sws_flags", flags, 0);
            contextFlags = flags;
        }
        AVFrame* destination = av_frame_alloc();
        if (!destination) return AVERROR(ENOMEM);
        destination->format = format;
        destination->width = width;
        destination->height = height;
        for (int i = 0; i < 4; i++)
        {
            destination->data[i] = data[i];
            destination->linesize[i] = linesize[i];
        }
        destination->colorspace = source->colorspace;
        destination->color_range = source->color_range;
        destination->color_primaries = source->color_primaries;
        destination->color_trc = source->color_trc;
        destination->chroma_location = source->chroma_location;
        int result = sws_scale_frame(context, destination, source);
        // No buffers of its own: freeing leaves the caller's planes alone.
        av_frame_free(&destination);
        return result;
    }

private:
    SwsContext* context = nullptr;
    int contextFlags = 0;
};
