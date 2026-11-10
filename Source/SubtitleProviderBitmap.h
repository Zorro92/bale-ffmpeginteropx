#pragma once

#include "SubtitleProvider.h"
#include "BaleSubtitleSink.h"
#include <winrt/FFmpegInteropX.h>

using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Core;

class SubtitleProviderBitmap : public SubtitleProvider
{

public:
    SubtitleProviderBitmap(std::shared_ptr<FFmpegReader> reader,
        AVFormatContext* avFormatCtx,
        AVCodecContext* avCodecCtx,
        MediaSourceConfig const& config,
        int index,
        uint64_t sinkToken = 0)
        : SubtitleProvider(reader, avFormatCtx, avCodecCtx, config, index, TimedMetadataKind::ImageSubtitle),
        baleSinkToken(sinkToken)
    {
    }

    // Bale: a sink stream decodes every packet and hands the rects to bale_gpu's overlay instead of
    // creating ImageCues. Subtitle streams are never discarded, so this runs whatever the
    // TimedMetadataTrack presentation mode is; the overlay owns timing (clears end the previous event).
    virtual void QueuePacket(AVPacket* packet) override
    {
        if (!baleSinkToken)
        {
            SubtitleProvider::QueuePacket(packet);
            return;
        }
        try
        {
            PushToBaleSink(packet);
        }
        catch (...)
        {
        }
        av_packet_free(&packet);
    }

    virtual HRESULT Initialize() override
    {
        auto hr = SubtitleProvider::Initialize();

        if (SUCCEEDED(hr))
        {
            SubtitleTrack.CueEntered(weak_handler(this, &SubtitleProviderBitmap::OnCueEntered));
            SubtitleTrack.CueExited(weak_handler(this, &SubtitleProviderBitmap::OnCueExited));
        }

        return S_OK;
    }

    virtual void NotifyVideoFrameSize(int width, int height, double aspectRatio) override
    {
        videoWidth = width;
        videoHeight = height;
        if (isnormal(aspectRatio) && aspectRatio > 0)
        {
            videoAspectRatio = aspectRatio;
        }
        else
        {
            videoAspectRatio = (double)width / height;
        }
    }

    virtual IMediaCue CreateCue(AVPacket* packet, TimeSpan* position, TimeSpan* duration) override
    {
        AVSubtitle* subtitle = (AVSubtitle*)av_mallocz(sizeof(AVSubtitle));
        if (!subtitle)
        {
            return nullptr;
        }

        int gotSubtitle = 0;
        auto result = avcodec_decode_subtitle2(m_pAvCodecCtx, subtitle, &gotSubtitle, packet);
        if (result > 0 && gotSubtitle)
        {
            if (subtitle->start_display_time > 0)
            {
                *position = TimeSpan{ position->count() + (long long)10000 * subtitle->start_display_time };
            }
            *duration = TimeSpan{ (long long)10000 * subtitle->end_display_time };

            if (subtitle->num_rects <= 0)
            {
                // return empty cue to force clear previous
                return nullptr;
            }
            else
            {
                int width, height, offsetX, offsetY;
                TimedTextSize cueSize;
                TimedTextPoint cuePosition;
                if (subtitle->num_rects > 0 && CheckSize(subtitle, width, height, offsetX, offsetY, cueSize, cuePosition))
                {
                    auto id = winrt::to_hstring(nextId++);
                    map[id] = subtitle;

                    ImageCue cue;
                    cue.Id(id);
                    return cue;
                }
                else
                {
                    avsubtitle_free(subtitle);
                    av_freep(subtitle);
                    if (subtitle->num_rects > 0)
                    {
                        OutputDebugString(L"Error: Invalid subtitle size received.");
                    }
                }
            }
        }
        else if (result <= 0)
        {
            avsubtitle_free(subtitle);
            av_freep(subtitle);
            OutputDebugString(L"Failed to decode subtitle.");
        }
        return nullptr;
    }

public:

    void Flush(bool flushBuffers) override
    {
        SubtitleProvider::Flush(flushBuffers);

        if (baleSinkToken && flushBuffers)
        {
            BaleSubtitleSinkExports::Get().flush(baleSinkToken);
        }

        if (!m_config.as<implementation::MediaSourceConfig>()->IsExternalSubtitleParser && flushBuffers)
        {
            for (const auto& entry : map)
            {
                auto subtitle = entry.second;
                avsubtitle_free(subtitle);
                av_freep(subtitle);
            }
            map.clear();
        }
    }

private:

    void PushToBaleSink(AVPacket* packet)
    {
        if (packet->pts == AV_NOPTS_VALUE)
        {
            return;
        }

        AVSubtitle subtitle{};
        int gotSubtitle = 0;
        if (avcodec_decode_subtitle2(m_pAvCodecCtx, &subtitle, &gotSubtitle, packet) < 0 || !gotSubtitle)
        {
            avsubtitle_free(&subtitle);
            return;
        }

        constexpr unsigned int MaxRects = 64;
        BaleSubBitmapRect rects[MaxRects];
        int32_t count = 0;
        for (unsigned int i = 0; i < subtitle.num_rects && count < (int32_t)MaxRects; i++)
        {
            auto rect = subtitle.rects[i];
            if (!rect || rect->type != SUBTITLE_BITMAP || rect->w <= 0 || rect->h <= 0 ||
                !rect->data[0] || !rect->data[1] || rect->nb_colors <= 0 || rect->linesize[0] < rect->w)
            {
                continue;
            }
            rects[count++] = BaleSubBitmapRect{ rect->x, rect->y, rect->w, rect->h, rect->linesize[0],
                rect->nb_colors > 256 ? 256 : rect->nb_colors, rect->data[0], reinterpret_cast<const uint32_t*>(rect->data[1]) };
        }

        // Display times are relative to the packet pts. PGS sends UINT32_MAX (shown until the next
        // display set, which may be a clear); DVB sends its page timeout; 0 falls back to the packet.
        auto positionMs = ConvertPosition(packet->pts).count() / 10000;
        BaleSubBitmapEvent event{};
        event.start_ms = positionMs + subtitle.start_display_time;
        if (subtitle.end_display_time != UINT32_MAX && subtitle.end_display_time > subtitle.start_display_time)
        {
            event.end_ms = positionMs + subtitle.end_display_time;
        }
        else if (subtitle.end_display_time == 0 && packet->duration > 0)
        {
            event.end_ms = event.start_ms + ConvertDuration(packet->duration).count() / 10000;
        }
        else
        {
            event.end_ms = -1;
        }
        event.canvas_w = m_pAvCodecCtx->width;
        event.canvas_h = m_pAvCodecCtx->height;
        event.video_w = videoWidth;
        event.video_h = videoHeight;
        event.rects = count > 0 ? rects : nullptr;
        event.count = count;

        // Copies synchronously; a negative result only means the overlay is gone or not bound yet.
        BaleSubtitleSinkExports::Get().push(baleSinkToken, &event);
        avsubtitle_free(&subtitle);
    }

    void OnCueEntered(TimedMetadataTrack sender, MediaCueEventArgs args)
    {
        UNREFERENCED_PARAMETER(sender);
        std::lock_guard lock(mutex);
        try
        {
            auto cue = args.Cue().try_as<ImageCue>();
            if (cue && !cue.Id().empty())
            {
                auto image = cue.SoftwareBitmap();
                if (!image || image == GetDummyBitmap())
                {
                    auto subtitle = map.find(cue.Id());
                    if (subtitle != map.end())
                    {
                        CreateSubtitleImage(cue, subtitle->second);
                    }
                }
            }
        }
        catch (...)
        {
            OutputDebugString(L"Failed to render image cue.");
        }
    }

    void OnCueExited(TimedMetadataTrack sender, MediaCueEventArgs args)
    {
        UNREFERENCED_PARAMETER(sender);
        std::lock_guard lock(mutex);
        try
        {
            auto cue = args.Cue().try_as<ImageCue>();
            if (cue && !cue.Id().empty() && cue.SoftwareBitmap())
            {
                cue.SoftwareBitmap(GetDummyBitmap());
            }
        }
        catch (...)
        {
            OutputDebugString(L"Failed to clear image cue.");
        }
    }

    void CreateSubtitleImage(ImageCue cue, AVSubtitle* subtitle)
    {
        int width, height, offsetX, offsetY;
        TimedTextSize cueSize;
        TimedTextPoint cuePosition;
        if (subtitle->num_rects > 0 && CheckSize(subtitle, width, height, offsetX, offsetY, cueSize, cuePosition))
        {
            auto bitmap = SoftwareBitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Straight);
            {
                auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                auto reference = buffer.CreateReference();
                BYTE* pixels = reference.data();

                auto plane = buffer.GetPlaneDescription(0);

                for (unsigned int i = 0; i < subtitle->num_rects; i++)
                {
                    auto rect = subtitle->rects[i];

                    for (int y = 0; y < rect->h; y++)
                    {
                        for (int x = 0; x < rect->w; x++)
                        {
                            auto inPointer = rect->data[0] + y * rect->linesize[0] + x;
                            auto color = inPointer[0];
                            if (color < rect->nb_colors)
                            {
                                auto rgba = ((uint32_t*)rect->data[1])[color];
                                auto outPointer = pixels + plane.StartIndex + plane.Stride * ((y + rect->y) - offsetY) + 4 * ((x + rect->x) - offsetX);
                                ((uint32_t*)outPointer)[0] = rgba;
                            }
                            else
                            {
                                OutputDebugString(L"Error: Illegal subtitle color.");
                            }
                        }
                    }
                }
            }

            auto converted = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied);
            cue.SoftwareBitmap(converted);
            cue.Position(cuePosition);
            cue.Extent(cueSize);
        }
        else if (subtitle->num_rects > 0)
        {
            OutputDebugString(L"Error: Invalid subtitle size received.");
        }
    }

    SoftwareBitmap GetDummyBitmap()
    {
        if (!dummyBitmap)
        {
            dummyBitmap = SoftwareBitmap(BitmapPixelFormat::Bgra8, 16, 16, BitmapAlphaMode::Premultiplied);
        }

        return dummyBitmap;
    }

    bool CheckSize(AVSubtitle* subtitle, int& width, int& height, int& offsetX, int& offsetY,TimedTextSize& cueSize,TimedTextPoint& cuePosition)
    {
        if (!GetInitialSize())
        {
            return false;
        }

        // get actual extent of subtitle rects
        int minX = subtitleWidth, minY = subtitleHeight, maxW = 0, maxH = 0;
        for (unsigned int i = 0; i < subtitle->num_rects; i++)
        {
            auto rect = subtitle->rects[i];
            minX = min(minX, rect->x);
            minY = min(minY, rect->y);
            maxW = max(maxW, rect->x + rect->w);
            maxH = max(maxH, rect->y + rect->h);
        }

        // sanity check
        if (minX < 0 || minY < 0 || maxW > subtitleWidth || maxH > subtitleHeight)
        {
            return false;
        }

        offsetX = minX;
        offsetY = minY;
        width = maxW - minX;
        height = maxH - minY;

        // try to fit into actual video frame aspect ratio, if aspect of sub is different from video
        int heightOffset = 0;
        int targetHeight = subtitleHeight;
        if (optimalHeight)
        {
            heightOffset = (subtitleHeight - optimalHeight) / 2;
            targetHeight = optimalHeight;

            // if subtitle does not fit into optimal height, fall back to normal height
            if (maxH > optimalHeight + heightOffset || minY < heightOffset)
            {
                optimalHeight = 0;
                heightOffset = 0;
                targetHeight = subtitleHeight;
            }
        }

        cueSize.Unit =TimedTextUnit::Percentage;
        cueSize.Width = (double)width * 100 / subtitleWidth;
        cueSize.Height = (double)height * 100 / targetHeight;

        // for some reason, all bitmap cues are moved down by 5% by uwp. we need to compensate for that.
        cuePosition.Unit =TimedTextUnit::Percentage;
        cuePosition.X = (double)offsetX * 100 / subtitleWidth;
        cuePosition.Y = ((double)(offsetY - heightOffset) * 100 / targetHeight) - 5;

        return true;
    }

    bool GetInitialSize()
    {
        if (!hasSize)
        {
            // initially get size information
            subtitleWidth = m_pAvCodecCtx->width;
            subtitleHeight = m_pAvCodecCtx->height;

            if (subtitleWidth == 0 && subtitleHeight == 0)
            {
                OutputDebugString(L"Warning: No subtitle size received. Assuming equal to video size.\n");
                subtitleWidth = videoWidth;
                subtitleHeight = videoHeight;
            }

            if (subtitleWidth > 0 && subtitleHeight > 0)
            {
                if (subtitleWidth != videoWidth || subtitleHeight != videoHeight || (videoAspectRatio > 0 && videoAspectRatio != 1))
                {
                    auto height = (int)(subtitleWidth / videoAspectRatio);
                    if (height < subtitleHeight)
                    {
                        optimalHeight = height;
                    }
                }

                hasSize = true;
            }
        }
        return hasSize;
    }

private:
    uint64_t baleSinkToken = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    double videoAspectRatio = 0.0;
    bool hasSize = false;
    int subtitleWidth = 0;
    int subtitleHeight = 0;
    int optimalHeight = 0;
    SoftwareBitmap dummyBitmap = { nullptr };
    std::map<winrt::hstring, AVSubtitle*> map;
    int nextId = 0;
};
