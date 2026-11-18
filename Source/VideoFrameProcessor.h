#pragma once
#include "pch.h"
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
    virtual HRESULT Process(ID3D11DeviceContext* context, ID3D11Texture2D* input, UINT inputSubresource,
        ID3D11Texture2D* output, const VideoFrameColorInfo& colorInfo) = 0;
    // Includes failures in the surrounding output stage (allocation, surface creation, fallback).
    virtual void NotifyError(HRESULT hr) noexcept { UNREFERENCED_PARAMETER(hr); }
    // Output texture formats Process can write. NV12 is the baseline every processor supports.
    virtual bool SupportsOutputFormat(DXGI_FORMAT format) const { return format == DXGI_FORMAT_NV12; }
    // Adds processor-specific attributes to the processed stream's media type (after the colour
    // description). Default: none.
    virtual void DescribeOutput(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const
    {
        UNREFERENCED_PARAMETER(properties);
    }
};
