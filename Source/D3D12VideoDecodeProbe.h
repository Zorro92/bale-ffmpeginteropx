#pragma once
#include "pch.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
}

// Diagnostic: whether a stream could be decoded through D3D12 video (FFmpeg's d3d12va hwaccel), asked
// before any D3D12 decode path exists. It logs whether the decoder has a d3d12va hwaccel, whether
// FFmpeg can create its D3D12 device, whether that device exposes D3D12 video decoding, and whether
// it supports this stream's profile, format and size. Nothing is decoded and nothing stays
// allocated: the device is released before Run returns.
class D3D12VideoDecodeProbe
{
public:
    static void Run(const AVCodec* codec, const AVCodecParameters* parameters, AVRational frameRate)
    {
        bool hwaccel = false;
        for (int i = 0; codec; i++)
        {
            auto hwConfig = avcodec_get_hw_config(codec, i);
            if (!hwConfig) break;
            if (hwConfig->pix_fmt == AV_PIX_FMT_D3D12 && hwConfig->device_type == AV_HWDEVICE_TYPE_D3D12VA &&
                (hwConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            {
                hwaccel = true;
                break;
            }
        }

        GUID profile{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int depth = BitDepth(parameters);
        bool mapped = DecodeProfile(parameters, depth, profile, format);

        // The caller creates the device (feature level 11_0) and hands it to FFmpeg, as a decode route
        // would: FFmpeg's own av_hwdevice_ctx_create asks for feature level 12_0, which the console's
        // app partition does not create (AVERROR_UNKNOWN there; rav1g also creates its device at 11_0).
        winrt::com_ptr<ID3D12Device> device;
        HRESULT deviceHr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
        unsigned maxLevel = 0;
        HRESULT videoHr = E_NOT_SET;
        UINT profiles = 0;
        D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support{};
        HRESULT supportHr = E_NOT_SET;
        int ffmpegInit = AVERROR(ENOSYS);
        if (SUCCEEDED(deviceHr))
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_12_1, static_cast<D3D_FEATURE_LEVEL>(0xc200) };
            D3D12_FEATURE_DATA_FEATURE_LEVELS levelData{ ARRAYSIZE(levels), levels };
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levelData, sizeof(levelData))))
            {
                maxLevel = static_cast<unsigned>(levelData.MaxSupportedFeatureLevel);
            }

            winrt::com_ptr<ID3D12VideoDevice> videoDevice;
            videoHr = device->QueryInterface(IID_PPV_ARGS(videoDevice.put()));
            if (SUCCEEDED(videoHr))
            {
                D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count{};
                if (SUCCEEDED(videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof(count))))
                {
                    profiles = count.ProfileCount;
                }
                if (mapped)
                {
                    support.Configuration.DecodeProfile = profile;
                    support.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
                    support.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
                    support.Width = static_cast<UINT>(parameters->width);
                    support.Height = static_cast<UINT>(parameters->height);
                    support.DecodeFormat = format;
                    support.FrameRate = { static_cast<UINT>((std::max)(frameRate.num, 0)), static_cast<UINT>((std::max)(frameRate.den, 1)) };
                    supportHr = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support));
                }
            }

            // FFmpeg's d3d12va device context on the caller's device. FFmpeg releases only the video
            // device it queries itself; the device stays owned here.
            AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
            if (deviceRef)
            {
                auto context = reinterpret_cast<AVD3D12VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(deviceRef->data)->hwctx);
                context->device = device.get();
                ffmpegInit = av_hwdevice_ctx_init(deviceRef);
                av_buffer_unref(&deviceRef);
            }
            else
            {
                ffmpegInit = AVERROR(ENOMEM);
            }
        }

        bool supported = SUCCEEDED(supportHr) && (support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
        av_log(nullptr, AV_LOG_INFO,
            "Bale d3d12va probe: %s (codec %d profile %d %dx%d %d-bit): hwaccel=%d device=0x%08X level=0x%X video=0x%08X "
            "profiles=%u check=0x%08X flags=0x%X config=0x%X tier=%d ffmpeg=%d\n",
            supported ? "supported" : (mapped ? "not supported" : "no D3D12 profile for this stream"),
            static_cast<int>(parameters->codec_id), parameters->profile, parameters->width, parameters->height, depth,
            hwaccel ? 1 : 0, static_cast<unsigned>(deviceHr), maxLevel, static_cast<unsigned>(videoHr), profiles,
            static_cast<unsigned>(supportHr), static_cast<unsigned>(support.SupportFlags),
            static_cast<unsigned>(support.ConfigurationFlags), static_cast<int>(support.DecodeTier), ffmpegInit);
    }

private:
    static int BitDepth(const AVCodecParameters* parameters)
    {
        auto description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        if (description) return description->comp[0].depth;
        return parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample : 8;
    }

    // The D3D12 decode profile and output format FFmpeg's d3d12va would use for 4:2:0 streams.
    static bool DecodeProfile(const AVCodecParameters* parameters, int depth, GUID& profile, DXGI_FORMAT& format)
    {
        bool tenBit = depth > 8;
        format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        if (depth > 10) return false;
        switch (parameters->codec_id)
        {
        case AV_CODEC_ID_H264:
            profile = D3D12_VIDEO_DECODE_PROFILE_H264;
            return !tenBit;
        case AV_CODEC_ID_HEVC:
            profile = tenBit ? D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10 : D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
            return parameters->profile == AV_PROFILE_UNKNOWN || parameters->profile == AV_PROFILE_HEVC_MAIN ||
                parameters->profile == AV_PROFILE_HEVC_MAIN_10;
        case AV_CODEC_ID_VP9:
            profile = tenBit ? D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2 : D3D12_VIDEO_DECODE_PROFILE_VP9;
            return true;
        case AV_CODEC_ID_AV1:
            profile = D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
            return parameters->profile == AV_PROFILE_UNKNOWN || parameters->profile == AV_PROFILE_AV1_MAIN;
        case AV_CODEC_ID_MPEG2VIDEO:
            profile = D3D12_VIDEO_DECODE_PROFILE_MPEG2;
            return !tenBit;
        default:
            return false;
        }
    }
};
