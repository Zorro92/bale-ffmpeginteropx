#pragma once
#include "pch.h"
#include <algorithm>
#include <cstdint>

extern "C"
{
#include <libavutil/mastering_display_metadata.h>
}

// Source colour values use the AV*/H.273 code points, not Media Foundation enums.
struct VideoFrameColorInfo
{
    uint8_t transfer = AVCOL_TRC_UNSPECIFIED;
    uint8_t primaries = AVCOL_PRI_UNSPECIFIED;
    uint8_t matrix = AVCOL_SPC_UNSPECIFIED;
    uint8_t range = AVCOL_RANGE_UNSPECIFIED;
    uint8_t chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
    bool hasMasteringPrimaries = false;
    bool hasMasteringLuminance = false;
    bool hasContentLight = false;
    float masteringPrimaries[3][2]{};
    float masteringWhite[2]{};
    float masteringMinNits = 0;
    float masteringMaxNits = 0;
    uint32_t maxCLL = 0;
    uint32_t maxFALL = 0;

    static VideoFrameColorInfo FromFrame(const AVFrame* frame, const AVCodecParameters* codecParameters)
    {
        VideoFrameColorInfo info;
        // A decoded frame's colour fields come from the bitstream (e.g. HEVC VUI) and may be
        // unspecified when only the container describes the colour; fall back to the stream values.
        info.transfer = static_cast<uint8_t>(frame->color_trc != AVCOL_TRC_UNSPECIFIED ? frame->color_trc : codecParameters->color_trc);
        info.primaries = static_cast<uint8_t>(frame->color_primaries != AVCOL_PRI_UNSPECIFIED ? frame->color_primaries : codecParameters->color_primaries);
        info.matrix = static_cast<uint8_t>(frame->colorspace != AVCOL_SPC_UNSPECIFIED ? frame->colorspace : codecParameters->color_space);
        info.range = static_cast<uint8_t>(frame->color_range != AVCOL_RANGE_UNSPECIFIED ? frame->color_range : codecParameters->color_range);
        info.chromaLocation = static_cast<uint8_t>(frame->chroma_location);

        const AVMasteringDisplayMetadata* mastering = nullptr;
        auto frameMastering = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if (frameMastering && frameMastering->data && frameMastering->size >= sizeof(AVMasteringDisplayMetadata))
        {
            mastering = reinterpret_cast<const AVMasteringDisplayMetadata*>(frameMastering->data);
        }
        else
        {
            auto streamMastering = av_packet_side_data_get(codecParameters->coded_side_data,
                codecParameters->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
            if (streamMastering && streamMastering->data && streamMastering->size >= sizeof(AVMasteringDisplayMetadata))
            {
                mastering = reinterpret_cast<const AVMasteringDisplayMetadata*>(streamMastering->data);
            }
        }
        if (mastering)
        {
            info.hasMasteringPrimaries = mastering->has_primaries != 0;
            info.hasMasteringLuminance = mastering->has_luminance != 0;
            if (info.hasMasteringPrimaries)
            {
                for (int i = 0; i < 3; i++)
                {
                    for (int j = 0; j < 2; j++)
                    {
                        info.masteringPrimaries[i][j] = static_cast<float>(av_q2d(mastering->display_primaries[i][j]));
                    }
                }
                for (int i = 0; i < 2; i++)
                {
                    info.masteringWhite[i] = static_cast<float>(av_q2d(mastering->white_point[i]));
                }
            }
            if (info.hasMasteringLuminance)
            {
                info.masteringMinNits = static_cast<float>(av_q2d(mastering->min_luminance));
                info.masteringMaxNits = static_cast<float>(av_q2d(mastering->max_luminance));
            }
        }

        const AVContentLightMetadata* light = nullptr;
        auto frameLight = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        if (frameLight && frameLight->data && frameLight->size >= sizeof(AVContentLightMetadata))
        {
            light = reinterpret_cast<const AVContentLightMetadata*>(frameLight->data);
        }
        else
        {
            auto streamLight = av_packet_side_data_get(codecParameters->coded_side_data,
                codecParameters->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
            if (streamLight && streamLight->data && streamLight->size >= sizeof(AVContentLightMetadata))
            {
                light = reinterpret_cast<const AVContentLightMetadata*>(streamLight->data);
            }
        }
        if (light)
        {
            info.hasContentLight = true;
            info.maxCLL = light->MaxCLL;
            info.maxFALL = light->MaxFALL;
        }
        return info;
    }
};

// Internal processing contract: P010 input to NV12, limited-range BT.709 output at a fixed size.
// The caller protects the immediate context, retains input until submission, and Flushes afterwards.
// Initialize is called again on a device change; implementations must release their previous state.
class VideoFrameProcessor
{
public:
    virtual ~VideoFrameProcessor() = default;
    // outputWidth/Height: the processed frame size; sourceWidth/Height: the visible input size, larger
    // than the output only when SupportsDownscale() (the processor then downscales).
    virtual HRESULT Initialize(ID3D11Device* device, UINT outputWidth, UINT outputHeight, UINT sourceWidth, UINT sourceHeight) = 0;
    virtual bool SupportsDownscale() const { return false; }
    // Input texture formats Process can read. P010 is the baseline.
    virtual bool SupportsInputFormat(DXGI_FORMAT format) const { return format == DXGI_FORMAT_P010; }
    virtual HRESULT Process(ID3D11DeviceContext* context, ID3D11Texture2D* input, UINT inputSubresource,
        ID3D11Texture2D* output, const VideoFrameColorInfo& colorInfo) = 0;
    // Includes failures in the surrounding output stage (allocation, surface creation, fallback).
    virtual void NotifyError(HRESULT hr) noexcept { UNREFERENCED_PARAMETER(hr); }
    // Output texture formats Process can write. NV12 is the baseline every processor supports.
    virtual bool SupportsOutputFormat(DXGI_FORMAT format) const { return format == DXGI_FORMAT_NV12; }
    // Whether Process dithers when it writes this format (an 8-bit output of higher-precision
    // results without banding). Default: no.
    virtual bool DithersOutputFormat(DXGI_FORMAT format) const { UNREFERENCED_PARAMETER(format); return false; }

    // The processed output format: P010 whenever the processor can write it, so processed video keeps
    // 10 bits, NV12 otherwise. With an 8-bit output depth requested (an 8-bit display), NV12 when the
    // processor dithers it, so the 10-to-8 step happens once, dithered, in the processor's own pass.
    static DXGI_FORMAT SelectOutputFormat(const VideoFrameProcessor& processor, int requestedDepth)
    {
        if (requestedDepth == 8 && processor.DithersOutputFormat(DXGI_FORMAT_NV12)) return DXGI_FORMAT_NV12;
        return processor.SupportsOutputFormat(DXGI_FORMAT_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }
    // Adds processor-specific attributes to the processed stream's media type (after the colour
    // description). Default: none.
    virtual void DescribeOutput(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const
    {
        UNREFERENCED_PARAMETER(properties);
    }

    // The processed size for a visible source of sourceWidth x sourceHeight and a target size (0 =
    // none): the source fitted inside the target, aspect kept, even sizes, at most 4x smaller per
    // axis; only when the processor downscales. Otherwise width/height stay the source size.
    static void SelectOutputSize(const VideoFrameProcessor& processor, int sourceWidth, int sourceHeight,
        int targetWidth, int targetHeight, int& width, int& height)
    {
        width = sourceWidth;
        height = sourceHeight;
        if (targetWidth <= 0 || targetHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return;
        if (sourceWidth <= targetWidth && sourceHeight <= targetHeight) return;
        if (!processor.SupportsDownscale())
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale tonemap: downscale to %dx%d not supported; keeping %dx%d\n",
                targetWidth, targetHeight, sourceWidth, sourceHeight);
            return;
        }
        double scale = (std::min)(static_cast<double>(targetWidth) / sourceWidth, static_cast<double>(targetHeight) / sourceHeight);
        scale = (std::max)(scale, 0.25);
        int fitWidth = (std::max)(2, static_cast<int>(sourceWidth * scale) & ~1);
        int fitHeight = (std::max)(2, static_cast<int>(sourceHeight * scale) & ~1);
        // Keep within the processor's 4x limit after rounding down to even sizes.
        while (fitWidth * 4 < sourceWidth) fitWidth += 2;
        while (fitHeight * 4 < sourceHeight) fitHeight += 2;
        width = fitWidth;
        height = fitHeight;
        av_log(nullptr, AV_LOG_INFO, "Bale tonemap downscale %dx%d -> %dx%d (target %dx%d)\n",
            sourceWidth, sourceHeight, width, height, targetWidth, targetHeight);
    }
};
