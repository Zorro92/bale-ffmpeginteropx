//*****************************************************************************
//
//	Copyright 2015 Microsoft Corporation
//
//	Licensed under the Apache License, Version 2.0 (the "License");
//	you may not use this file except in compliance with the License.
//	You may obtain a copy of the License at
//
//	http ://www.apache.org/licenses/LICENSE-2.0
//
//	Unless required by applicable law or agreed to in writing, software
//	distributed under the License is distributed on an "AS IS" BASIS,
//	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//	See the License for the specific language governing permissions and
//	limitations under the License.
//
//*****************************************************************************

#include "pch.h"
#include "UncompressedVideoSampleProvider.h"
#include "BaleThreadBoost.h"
#include "NativeBufferFactory.h"
#include <mfapi.h>
#include <d3d10.h>
#include "VideoFilterFactory.h"

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
}

using namespace NativeBuffer;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;

UncompressedVideoSampleProvider::UncompressedVideoSampleProvider(
    std::shared_ptr<FFmpegReader> reader,
    AVFormatContext* avFormatCtx,
    AVCodecContext* avCodecCtx,
    MediaSourceConfig const& config,
    int streamIndex,
    HardwareDecoderStatus hardwareDecoderStatus,
    bool applyHdrColorInfo)
    : UncompressedSampleProvider(reader, avFormatCtx, avCodecCtx, config, streamIndex, hardwareDecoderStatus)
{
    m_pSwsCtx = NULL;
    this->applyHdrColorInfo = applyHdrColorInfo;
}

AVPixelFormat UncompressedVideoSampleProvider::SelectOutputPixelFormat(AVPixelFormat input, bool direct, MediaSourceConfig const& config)
{
    if (config.as<implementation::MediaSourceConfig>()->IsFrameGrabber) return AV_PIX_FMT_BGRA;
    if (config.Video().VideoOutputAllowIyuv() && (input == AV_PIX_FMT_YUV420P || input == AV_PIX_FMT_YUVJ420P) && direct) return input;
    if (input == AV_PIX_FMT_YUV420P10LE && config.Video().VideoOutputAllow10bit()) return AV_PIX_FMT_P010LE;
    if (config.Video().VideoOutputAllowNv12()) return AV_PIX_FMT_NV12;
    if (config.Video().VideoOutputAllowIyuv()) return input == AV_PIX_FMT_YUVJ420P ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    if (config.Video().VideoOutputAllowBgra8()) return AV_PIX_FMT_BGRA;
    return AV_PIX_FMT_NV12; // Upstream fallback when no format is allowed.
}

void UncompressedVideoSampleProvider::SelectOutputFormat()
{
    m_OutputPixelFormat = SelectOutputPixelFormat(m_pAvCodecCtx->pix_fmt,
        (m_pAvCodecCtx->codec->capabilities & AV_CODEC_CAP_DR1) && decoder != DecoderEngine::FFmpegD3D11HardwareDecoder, m_config);
    switch (m_OutputPixelFormat)
    {
    case AV_PIX_FMT_BGRA: outputMediaSubtype = MediaEncodingSubtypes::Bgra8(); break;
    case AV_PIX_FMT_P010LE: outputMediaSubtype = winrt::to_hstring(MFVideoFormat_P010); break;
    case AV_PIX_FMT_NV12: outputMediaSubtype = MediaEncodingSubtypes::Nv12(); break;
    default: outputMediaSubtype = MediaEncodingSubtypes::Iyuv(); break;
    }

    outputWidth = outputFrameWidth = m_pAvCodecCtx->width;
    outputHeight = outputFrameHeight = m_pAvCodecCtx->height;

    if (m_OutputPixelFormat != AV_PIX_FMT_BGRA)
    {
        // only BGRA supports unaligned image sizes, all others require 4 pixel alignment
        outputFrameWidth = ((outputWidth - 1) / 4 * 4) + 4;
        outputFrameHeight = ((outputHeight - 1) / 4 * 4) + 4;
    }

    if (m_pAvCodecCtx->pix_fmt == m_OutputPixelFormat && m_pAvCodecCtx->codec->capabilities & AV_CODEC_CAP_DR1)
    {
        // This codec supports direct buffer decoding.

        m_pAvCodecCtx->get_buffer2 = get_buffer2;
        m_pAvCodecCtx->opaque = (void*)this;
    }
    else if (ZeroCopy && decoder == DecoderEngine::FFmpegSoftwareDecoder &&
        std::string(m_pAvCodecCtx->codec->name) == "libdav1d" &&
        m_pAvCodecCtx->pix_fmt == AV_PIX_FMT_YUV420P10LE && m_OutputPixelFormat == AV_PIX_FMT_P010LE)
    {
        // Bale zero-copy: the patched libdav1d (native p1) asks a replaced get_buffer2 for each
        // picture, per picture, so installing it before the first decode is enough. Until
        // SetUploadDevice creates the handle the hook declines and dav1d uses its own pool.
        m_pAvCodecCtx->get_buffer2 = get_zero_copy_buffer;
        m_pAvCodecCtx->opaque = (void*)this;
        zeroCopyHooked = true;
    }
}

IMediaStreamDescriptor UncompressedVideoSampleProvider::CreateStreamDescriptor()
{
    SelectOutputFormat();

    frameProvider = std::shared_ptr<UncompressedFrameProvider>(new UncompressedFrameProvider(m_pAvFormatCtx, m_pAvCodecCtx, std::shared_ptr< VideoFilterFactory>(new VideoFilterFactory(m_pAvCodecCtx, m_pAvStream))));

    auto videoProperties = VideoEncodingProperties::CreateUncompressed(outputMediaSubtype, outputFrameWidth, outputFrameHeight);
    auto properties = videoProperties.Properties();
    auto codecPar = m_pAvStream->codecpar;

    SetCommonVideoEncodingProperties(videoProperties, false);

    MFVideoArea area;
    area.Area.cx = outputWidth;
    area.Area.cy = outputHeight;
    area.OffsetX.fract = 0;
    area.OffsetX.value = 0;
    area.OffsetY.fract = 0;
    area.OffsetY.value = 0;
    properties.Insert(MF_MT_MINIMUM_DISPLAY_APERTURE, PropertyValue::CreateUInt8Array(winrt::array_view((uint8_t*)&area, sizeof(MFVideoArea))));

    using namespace winrt::Windows::Graphics::Display;

    bool hasHdrVideoPrimaries = false;
    if (codecPar->color_primaries != AVCOL_PRI_UNSPECIFIED)
    {
        MFVideoPrimaries videoPrimaries{ MFVideoPrimaries_Unknown };
        switch (codecPar->color_primaries)
        {
        case AVCOL_PRI_BT709:
            videoPrimaries = MFVideoPrimaries_BT709;
            break;

        case AVCOL_PRI_BT470M:
            videoPrimaries = MFVideoPrimaries_BT470_2_SysM;
            break;

        case AVCOL_PRI_BT470BG:
            videoPrimaries = MFVideoPrimaries_BT470_2_SysBG;
            break;

        case AVCOL_PRI_SMPTE170M:
            videoPrimaries = MFVideoPrimaries_SMPTE170M;
            break;

        case AVCOL_PRI_SMPTE240M:
            videoPrimaries = MFVideoPrimaries_SMPTE240M;
            break;

        case AVCOL_PRI_FILM:
            // FFmpeg uses SMPTE-C / BT.601-NTSC as best approximation.
            videoPrimaries = MFVideoPrimaries_SMPTE_C;
            break;

        case AVCOL_PRI_BT2020:
            videoPrimaries = MFVideoPrimaries_BT2020;
            break;

        case AVCOL_PRI_EBU3213: // EBU Tech. 3213 (PAL/SECAM Variant)
            videoPrimaries = MFVideoPrimaries_EBU3213;
            break;

        case AVCOL_PRI_SMPTE428: // XYZ (DCI-P3 standard in XYZ-Colorspace)
            videoPrimaries = MFVideoPrimaries_XYZ;
            break;

        case AVCOL_PRI_SMPTE431: // DCI-P3 (cinema standard)
        case AVCOL_PRI_SMPTE432: // Display P3 / Apple P3
            videoPrimaries = MFVideoPrimaries_DCI_P3;
            break;

        default:
            break;
        }

        if (videoPrimaries != MFVideoPrimaries_Unknown)
        {
            hasHdrVideoPrimaries = videoPrimaries >= MFVideoPrimaries_BT2020;
            if (!hasHdrVideoPrimaries || applyHdrColorInfo)
            {
                properties.Insert(MF_MT_VIDEO_PRIMARIES, PropertyValue::CreateUInt32(videoPrimaries));
            }
        }
    }

    bool hasHdrVideoTransferFunction = false;
    if (codecPar->color_trc != AVCOL_TRC_UNSPECIFIED)
    {
        MFVideoTransferFunction videoTransferFunc{ MFVideoTransFunc_Unknown };
        switch (codecPar->color_trc)
        {
        case AVCOL_TRC_LINEAR:
            videoTransferFunc = MFVideoTransFunc_10; // Gamma 1.0 (Linear)
            break;

        case AVCOL_TRC_GAMMA22:
            videoTransferFunc = MFVideoTransFunc_22; // Pure Gamma 2.2
            break;

        case AVCOL_TRC_GAMMA28:
            videoTransferFunc = MFVideoTransFunc_28; // Pure Gamma 2.8
            break;

        case AVCOL_TRC_SMPTE240M:
            videoTransferFunc = MFVideoTransFunc_240M;
            break;

        case AVCOL_TRC_LOG:
            videoTransferFunc = MFVideoTransFunc_Log_100;
            break;

        case AVCOL_TRC_LOG_SQRT:
            videoTransferFunc = MFVideoTransFunc_Log_316;
            break;

        case AVCOL_TRC_BT1361_ECG:
            videoTransferFunc = MFVideoTransFunc_BT1361_ECG;
            break;

        case AVCOL_TRC_SMPTE428:
            videoTransferFunc = MFVideoTransFunc_SMPTE428; // DCI-Cinema transfer function
            break;

            // True ITU-R / SDR Transfer Functions ---
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M: // BT.601 uses the same transfer characteristics as BT.709
            videoTransferFunc = MFVideoTransFunc_709;
            break;

            // Standard sRGB (Crucial for web, desktop and screen captures) ---
        case AVCOL_TRC_IEC61966_2_1:
            videoTransferFunc = MFVideoTransFunc_sRGB;
            break;

            // --- HDR STANDARDS ---
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:
            videoTransferFunc = codecPar->color_space == AVCOL_SPC_BT2020_CL || codecPar->color_space == AVCOL_SPC_CHROMA_DERIVED_CL ?
                MFVideoTransFunc_2020_const :
                MFVideoTransFunc_2020;
            break;

        case AVCOL_TRC_SMPTEST2084:
            videoTransferFunc = MFVideoTransFunc_2084; // PQ / HDR10 / Dolby Vision
            break;

        case AVCOL_TRC_ARIB_STD_B67:
            videoTransferFunc = MFVideoTransFunc_HLG; // Hybrid Log-Gamma
            break;

        default:
            break;
        }

        if (videoTransferFunc != MFVideoTransFunc_Unknown)
        {
            hasHdrVideoTransferFunction = videoTransferFunc >= MFVideoTransFunc_2020_const;
            if (!hasHdrVideoTransferFunction || applyHdrColorInfo)
            {
                properties.Insert(MF_MT_TRANSFER_FUNCTION, PropertyValue::CreateUInt32(videoTransferFunc));
            }
        }
    }

    if (codecPar->color_range != AVCOL_RANGE_UNSPECIFIED)
    {
        MFNominalRange nominalRange{ codecPar->color_range == AVCOL_RANGE_JPEG ? MFNominalRange_0_255 : MFNominalRange_16_235 };
        properties.Insert(MF_MT_VIDEO_NOMINAL_RANGE, PropertyValue::CreateUInt32(nominalRange));
    }

    if (codecPar->color_space != AVCOL_SPC_UNSPECIFIED)
    {
        MFVideoTransferMatrix transferMatrix{ MFVideoTransferMatrix_Unknown };
        switch (codecPar->color_space)
        {
        case AVCOL_SPC_BT709:
            transferMatrix = MFVideoTransferMatrix_BT709;
            break;

        case AVCOL_SPC_FCC:
            transferMatrix = MFVideoTransferMatrix_FCC47;
            break;

        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            transferMatrix = MFVideoTransferMatrix_BT601;
            break;

        case AVCOL_SPC_SMPTE240M:
            transferMatrix = MFVideoTransferMatrix_SMPTE240M;
            break;

        case AVCOL_SPC_YCGCO:
        case AVCOL_SPC_YCGCO_RE: // H.273 v3 Reversible Matrix rounded even
        case AVCOL_SPC_YCGCO_RO: // H.273 v3 Reversible Matrix rounded odd
            transferMatrix = MFVideoTransferMatrix_YCgCo;
            break;

        case AVCOL_SPC_SMPTE2085:
            transferMatrix = MFVideoTransferMatrix_SMPTE2085;
            break;

        case AVCOL_SPC_BT2020_NCL:
            transferMatrix = codecPar->color_trc == AVCOL_TRC_BT2020_12 ?
                MFVideoTransferMatrix_BT2020_12 :
                MFVideoTransferMatrix_BT2020_10;
            break;

        case AVCOL_SPC_BT2020_CL:
        case AVCOL_SPC_CHROMA_DERIVED_CL:
            transferMatrix = MFVideoTransferMatrix_Chroma_const;
            break;

        case AVCOL_SPC_CHROMA_DERIVED_NCL:
            transferMatrix = MFVideoTransferMatrix_Chroma;
            break;

        case AVCOL_SPC_ICTCP:
            transferMatrix = MFVideoTransferMatrix_ICtCp;
            break;

        default:
            break;
        }

        if (transferMatrix != MFVideoTransferMatrix_Unknown)
        {
            properties.Insert(MF_MT_YUV_MATRIX, PropertyValue::CreateUInt32(transferMatrix));
        }
    }

    const AVContentLightMetadata* contentLightMetadata{ reinterpret_cast<const AVContentLightMetadata*>(av_packet_side_data_get(m_pAvStream->codecpar->coded_side_data, m_pAvStream->codecpar->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) };
    if (contentLightMetadata != nullptr && applyHdrColorInfo)
    {
        properties.Insert(MF_MT_MAX_LUMINANCE_LEVEL, PropertyValue::CreateUInt32(contentLightMetadata->MaxCLL));
        properties.Insert(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL, PropertyValue::CreateUInt32(contentLightMetadata->MaxFALL));
    }

    const AVMasteringDisplayMetadata* masteringDisplayMetadata{ reinterpret_cast<const AVMasteringDisplayMetadata*>(av_packet_side_data_get(m_pAvStream->codecpar->coded_side_data, m_pAvStream->codecpar->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) };
    if (masteringDisplayMetadata != nullptr && applyHdrColorInfo)
    {
        if (masteringDisplayMetadata->has_luminance)
        {
            constexpr UINT32 MASTERING_DISP_LUMINANCE_SCALE{ 10000 };
            properties.Insert(MF_MT_MIN_MASTERING_LUMINANCE, PropertyValue::CreateUInt32(static_cast<UINT32>(MASTERING_DISP_LUMINANCE_SCALE * av_q2d(masteringDisplayMetadata->min_luminance))));
            properties.Insert(MF_MT_MAX_MASTERING_LUMINANCE, PropertyValue::CreateUInt32(static_cast<UINT32>(av_q2d(masteringDisplayMetadata->max_luminance))));
        }

        if (masteringDisplayMetadata->has_primaries)
        {
            MT_CUSTOM_VIDEO_PRIMARIES customVideoPrimaries
            {
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[1]))
            };
            auto data = winrt::array_view<UINT8>(reinterpret_cast<UINT8*>(&customVideoPrimaries), sizeof(customVideoPrimaries));
            properties.Insert(MF_MT_CUSTOM_VIDEO_PRIMARIES, PropertyValue::CreateUInt8Array(data));
        }
    }

    hasHdrMetadata = (masteringDisplayMetadata != nullptr && (masteringDisplayMetadata->has_primaries || masteringDisplayMetadata->has_luminance)) || contentLightMetadata != nullptr || hasHdrVideoPrimaries || hasHdrVideoTransferFunction;
    isHdrActive = hasHdrMetadata && applyHdrColorInfo;

    videoProperties.Properties().Insert(MF_MT_INTERLACE_MODE, winrt::box_value((UINT32)_MFVideoInterlaceMode::MFVideoInterlace_MixedInterlaceOrProgressive));

    return VideoStreamDescriptor(videoProperties);
}

HRESULT UncompressedVideoSampleProvider::InitializeScalerIfRequired(AVFrame* avFrame)
{
    HRESULT hr = S_OK;

    // use bilinear for downscaling, bicubic for upscaling.
    // don't use point scaling since format conversions might involve scaling even if resolution is same (chroma subsampling)
    int scaler = avFrame->width < outputWidth ? SWS_BICUBIC : SWS_BILINEAR;

    if (SwsThreads > 1)
    {
        return InitializeThreadedScaler(avFrame, scaler);
    }

    // Setup software scaler to convert frame to output pixel type and size
    m_pSwsCtx = sws_getCachedContext(
        m_pSwsCtx,
        avFrame->width,
        avFrame->height,
        (AVPixelFormat)avFrame->format,
        outputWidth,
        outputHeight,
        m_OutputPixelFormat,
        scaler,
        NULL,
        NULL,
        NULL);

    if (m_pSwsCtx == nullptr)
    {
        hr = E_OUTOFMEMORY;
    }

    return hr;
}

// Bale: sws_getCachedContext cannot request slice threads, and FFmpeg 8 runs the legacy sws_scale
// on one slice context only. A context initialized with explicit options (threads > 1) runs
// sws_scale_frame through the same legacy scaler, split across slice threads.
HRESULT UncompressedVideoSampleProvider::InitializeThreadedScaler(AVFrame* avFrame, int scaler)
{
    if (m_pSwsCtx && m_swsThreaded &&
        m_swsSrcW == avFrame->width && m_swsSrcH == avFrame->height && m_swsSrcFmt == (AVPixelFormat)avFrame->format &&
        m_swsDstW == outputWidth && m_swsDstH == outputHeight && m_swsDstFmt == m_OutputPixelFormat && m_swsFlags == scaler)
    {
        return S_OK;
    }

    if (m_pSwsCtx)
    {
        sws_freeContext(m_pSwsCtx);
        m_pSwsCtx = NULL;
    }
    m_swsThreaded = false;

    SwsContext* ctx = sws_alloc_context();
    if (!ctx)
    {
        return E_OUTOFMEMORY;
    }

    int swsInit;
    {
        // Bale: sws creates its slice threads inside sws_init_context.
        BaleThreadBoost::Window boost(SwsThreadPriority);
        swsInit = av_opt_set_int(ctx, "srcw", avFrame->width, 0) < 0 ||
        av_opt_set_int(ctx, "srch", avFrame->height, 0) < 0 ||
        av_opt_set_int(ctx, "src_format", avFrame->format, 0) < 0 ||
        av_opt_set_int(ctx, "dstw", outputWidth, 0) < 0 ||
        av_opt_set_int(ctx, "dsth", outputHeight, 0) < 0 ||
        av_opt_set_int(ctx, "dst_format", m_OutputPixelFormat, 0) < 0 ||
        av_opt_set_int(ctx, "sws_flags", scaler, 0) < 0 ||
        av_opt_set_int(ctx, "threads", SwsThreads, 0) < 0 ||
        sws_init_context(ctx, NULL, NULL) < 0;
    }
    if (swsInit)
    {
        sws_freeContext(ctx);
        return E_FAIL;
    }

    m_pSwsCtx = ctx;
    m_swsThreaded = true;
    m_swsSrcW = avFrame->width;
    m_swsSrcH = avFrame->height;
    m_swsSrcFmt = (AVPixelFormat)avFrame->format;
    m_swsDstW = outputWidth;
    m_swsDstH = outputHeight;
    m_swsDstFmt = m_OutputPixelFormat;
    m_swsFlags = scaler;
    return S_OK;
}

// Bale: shared renderer context calls must be protected, even on the decode-ahead worker.
HRESULT UncompressedVideoSampleProvider::SetUploadDevice(winrt::com_ptr<ID3D11Device> newDevice, winrt::com_ptr<ID3D11DeviceContext> newContext, int variant)
{
    if (uploadFailed) return S_FALSE;
    try
    {
        if (!newDevice || !newContext) winrt::throw_hresult(E_NOINTERFACE);
        auto flags = newDevice->GetCreationFlags();
        auto multithread = newDevice.try_as<ID3D10Multithread>();
        if (!multithread) multithread = newContext.try_as<ID3D10Multithread>();
        // SetMultithreadProtected returns the PREVIOUS value, not a success/failure BOOL.
        if (multithread && !(flags & D3D11_CREATE_DEVICE_SINGLETHREADED)) multithread->SetMultithreadProtected(TRUE);
        bool protectedContext = multithread && multithread->GetMultithreadProtected();
        av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload device acquired: flags=0x%X feature_level=0x%X multithread_protected=%u variant=%d.\n",
            flags, static_cast<unsigned>(newDevice->GetFeatureLevel()), static_cast<unsigned>(protectedContext), variant);
        if (!multithread || (flags & D3D11_CREATE_DEVICE_SINGLETHREADED)) winrt::throw_hresult(E_NOINTERFACE);
        if (!protectedContext) winrt::throw_hresult(E_FAIL);
        av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload output declared: %dx%d subtype=%s.\n",
            outputFrameWidth, outputFrameHeight, winrt::to_string(outputMediaSubtype).c_str());
        upload = std::make_shared<BaleD3D11Upload>(newDevice, newContext, variant);
        CreateZeroCopy(newDevice, newContext);
        return S_OK;
    }
    catch (...)
    {
        auto hr = winrt::to_hresult();
        DisableUpload(hr);
        return hr;
    }
}

void UncompressedVideoSampleProvider::DisableUpload(HRESULT hr)
{
    if (!uploadFailed)
    {
        uploadFailed = true;
        av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload disabled (hr=0x%08X); using CPU buffers.\n", static_cast<unsigned>(hr));
    }
    FilmGrain.UploadDisabled(hr);
    uploadSample = nullptr;
    // Zero-copy only feeds the upload: before the uploader (and its D3D11 context use) goes away.
    DisableZeroCopy(hr);
    upload.reset();
}

void UncompressedVideoSampleProvider::DisableRepack(HRESULT hr)
{
    if (!repackFailed)
    {
        repackFailed = true;
        av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 repack disabled (hr=0x%08X); using swscale.\n", static_cast<unsigned>(hr));
    }
    if (upload) upload->DestroyRepack();
}

HRESULT UncompressedVideoSampleProvider::ScaleFrame(AVFrame* avFrame, BYTE** data, int* linesize, AVBufferRef* buffer)
{
    bool converted = false;
    if (m_swsThreaded)
    {
        // Wrap the destination (CPU pool or mapped staging memory) so sws does not allocate.
        AVFrame* dst = av_frame_alloc();
        if (dst)
        {
            dst->buf[0] = av_buffer_ref(buffer);
            if (dst->buf[0])
            {
                for (int i = 0; i < 4; i++)
                {
                    dst->data[i] = data[i];
                    dst->linesize[i] = linesize[i];
                }
                dst->width = outputWidth;
                dst->height = outputHeight;
                dst->format = m_OutputPixelFormat;
                converted = sws_scale_frame(m_pSwsCtx, dst, avFrame) >= 0;
            }
            av_frame_free(&dst);
        }
    }
    else
    {
        converted = sws_scale(m_pSwsCtx, (const UINT8**)(avFrame->data), avFrame->linesize, 0, avFrame->height, data, linesize) > 0;
    }
    return converted ? S_OK : E_FAIL;
}

HRESULT UncompressedVideoSampleProvider::CopyFrameToOutput(AVFrame* avFrame, BYTE** data, int* linesize, AVBufferRef* buffer)
{
    if (avFrame->format == m_OutputPixelFormat && avFrame->width == outputWidth && avFrame->height == outputHeight)
    {
        av_image_copy(data, linesize, (const UINT8**)avFrame->data, avFrame->linesize, m_OutputPixelFormat, outputWidth, outputHeight);
        return S_OK;
    }
    auto hr = InitializeScalerIfRequired(avFrame);
    if (FAILED(hr)) return hr;
    AVBufferRef* mapping = nullptr;
    if (!buffer)
    {
        // Only mapped NV12/P010 staging uses this no-op owner. sws_scale_frame and its
        // workers finish synchronously before it is released, and before staging is unmapped.
        auto size = static_cast<size_t>(linesize[0]) * outputFrameHeight * 3 / 2;
        mapping = av_buffer_create(data[0], size, [](void*, uint8_t*) {}, nullptr, 0);
        if (!mapping) return E_OUTOFMEMORY;
        buffer = mapping;
    }
    hr = ScaleFrame(avFrame, data, linesize, buffer);
    av_buffer_unref(&mapping);
    return hr;
}

bool UncompressedVideoSampleProvider::CanRepack(AVFrame* avFrame, const BaleGrainParams* params) const
{
    if (!Repack || repackFailed || !upload || !upload->CanRepack()) return false;
    // CPU grain runs on the converted NV12/P010 layout in mapped staging.
    if (params && FilmGrain.mode == 1) return false;
    // YUVJ is excluded: swscale converts its full range, the repack never changes sample values.
    auto input = static_cast<AVPixelFormat>(avFrame->format);
    bool formats = (input == AV_PIX_FMT_YUV420P && m_OutputPixelFormat == AV_PIX_FMT_NV12) ||
        (input == AV_PIX_FMT_YUV420P10LE && m_OutputPixelFormat == AV_PIX_FMT_P010LE);
    // Width % 4: the V half of each chroma row must start on a whole two-sample texel.
    return formats && avFrame->width == outputWidth && avFrame->height == outputHeight &&
        outputFrameWidth % 4 == 0 && outputFrameHeight % 2 == 0;
}

// Unconverted samples for the GPU repack: Y rows as decoded; each chroma row holds that row's
// U samples from byte 0 and its V samples from half the texture row (outputFrameWidth / 2 samples).
void UncompressedVideoSampleProvider::FillPlanar(AVFrame* avFrame, BYTE** data, UINT pitch)
{
    int bytes = m_OutputPixelFormat == AV_PIX_FMT_P010LE ? 2 : 1;
    int chromaWidth = (outputWidth + 1) / 2;
    int chromaHeight = (outputHeight + 1) / 2;
    int linesize = static_cast<int>(pitch);
    BYTE* vData = data[1] + static_cast<size_t>(outputFrameWidth / 2) * bytes;
    // One thread fills a 4K frame in ~25-40 ms on the console (measured with 0013), most of a 24 fps
    // frame interval. Split rows across the swscale thread count; each slice owns disjoint rows.
    if (!copyPool) copyPool = std::make_unique<BaleCopyPool>(SwsThreads, BaleThreadBoost::ToWin32(SwsThreadPriority));
    int slices = copyPool->Threads();
    copyPool->Run([&](int slice)
        {
            auto copyRows = [&](BYTE* dst, const uint8_t* src, int srcLinesize, int rowBytes, int height)
                {
                    int begin = height * slice / slices;
                    int end = height * (slice + 1) / slices;
                    if (end <= begin) return;
                    av_image_copy_plane(dst + static_cast<ptrdiff_t>(begin) * linesize, linesize,
                        src + static_cast<ptrdiff_t>(begin) * srcLinesize, srcLinesize, rowBytes, end - begin);
                };
            copyRows(data[0], avFrame->data[0], avFrame->linesize[0], outputWidth * bytes, outputHeight);
            copyRows(data[1], avFrame->data[1], avFrame->linesize[1], chromaWidth * bytes, chromaHeight);
            copyRows(vData, avFrame->data[2], avFrame->linesize[2], chromaWidth * bytes, chromaHeight);
        });
}

void UncompressedVideoSampleProvider::UploadFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts, const BaleGrainParams* params, bool planar)
{
    auto format = m_OutputPixelFormat == AV_PIX_FMT_NV12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;
    BaleD3D11Upload::Mapping target;
    auto& timing = upload->timing;
    if (timing.lastEnd) timing.Add(BaleUploadTiming::Gap, BaleUploadTiming::Now() - timing.lastEnd);
    winrt::check_hresult(upload->Map(outputFrameWidth, outputFrameHeight, format, target));
    auto fillStart = BaleUploadTiming::Now();
    auto pitch = target.mapped.RowPitch;
    auto rowBytes = static_cast<UINT>(outputFrameWidth) * (format == DXGI_FORMAT_P010 ? 2U : 1U);
    if (!target.mapped.pData || pitch < rowBytes || pitch > INT_MAX) winrt::throw_hresult(E_FAIL);
    BYTE* data[4]{ static_cast<BYTE*>(target.mapped.pData), nullptr, nullptr, nullptr };
    // NV12/P010 are planar staging resources: UV starts after RowPitch * TEXTURE height, not image height.
    data[1] = data[0] + static_cast<size_t>(pitch) * outputFrameHeight;
    int linesize[4]{ static_cast<int>(pitch), static_cast<int>(pitch), 0, 0 };
    if (planar)
    {
        FillPlanar(avFrame, data, pitch);
    }
    else
    {
        winrt::check_hresult(CopyFrameToOutput(avFrame, data, linesize, nullptr));
        if (params && FilmGrain.mode == 1 && !FilmGrain.ApplyCpu(*params, m_OutputPixelFormat, data, linesize))
        {
            // Refill clean output even if a contained Rust panic partially modified staging.
            winrt::check_hresult(CopyFrameToOutput(avFrame, data, linesize, nullptr));
        }
    }
    if (UploadFaultFrames > 0 && uploadedFrames >= UploadFaultFrames)
    {
        // Same point as a real pool CreateTexture2D failure: staging is mapped and filled.
        av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload fault injected after %d frames.\n", uploadedFrames);
        winrt::throw_hresult(E_OUTOFMEMORY);
    }
    ReadFrameProperties(avFrame, framePts);
    timing.Add(BaleUploadTiming::Fill, BaleUploadTiming::Now() - fillStart);
    uploadSample = upload->CreateSample(target, ConvertPosition(framePts), surface, FilmGrain, params, planar);
    timing.EndFrame(planar ? 1 : 0);
    uploadedFrames++;
    if (!uploadLogged)
    {
        uploadLogged = true;
        av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload active: %s %dx%d.\n", format == DXGI_FORMAT_NV12 ? "NV12" : "P010", outputFrameWidth, outputFrameHeight);
    }
}

UncompressedVideoSampleProvider::~UncompressedVideoSampleProvider()
{
    // The base destructor frees the codec later: stop the hook from seeing this provider, then
    // destroy the handle. Pictures dav1d still holds are released into the closed pool.
    if (zeroCopyHooked && m_pAvCodecCtx && m_pAvCodecCtx->get_buffer2 == get_zero_copy_buffer)
    {
        m_pAvCodecCtx->get_buffer2 = avcodec_default_get_buffer2;
        m_pAvCodecCtx->opaque = nullptr;
    }
    DisableZeroCopy(S_OK);

    if (m_pSwsCtx)
    {
        sws_freeContext(m_pSwsCtx);
    }

    if (sourceBufferPool)
    {
        av_buffer_pool_uninit(&sourceBufferPool);
    }

    if (targetBufferPool)
    {
        av_buffer_pool_uninit(&targetBufferPool);
    }
}

HRESULT UncompressedVideoSampleProvider::CreateBufferFromFrame(IBuffer* pBuffer, IDirect3DSurface* surface, AVFrame* avFrame, int64_t& framePts, int64_t& frameDuration)
{
    UNREFERENCED_PARAMETER(frameDuration);

    HRESULT hr = S_OK;
    CheckFrameSize(avFrame);
    BaleGrainParams grainParams{};
    bool hasGrain = FilmGrain.Prepare(avFrame, m_OutputPixelFormat, outputWidth, outputHeight, grainParams);

    if (upload && (m_OutputPixelFormat == AV_PIX_FMT_NV12 || m_OutputPixelFormat == AV_PIX_FMT_P010LE))
    {
        try
        {
            auto params = hasGrain ? &grainParams : nullptr;
            if (TryZeroCopy(avFrame, surface, framePts, params)) return S_OK;
            if (CanRepack(avFrame, params))
            {
                try
                {
                    UploadFrame(avFrame, surface, framePts, params, true);
                    if (!repackLogged)
                    {
                        repackLogged = true;
                        av_log(nullptr, AV_LOG_INFO, "Bale D3D11 repack active: %dx%d.\n", outputWidth, outputHeight);
                    }
                    return S_OK;
                }
                catch (...)
                {
                    // Permanent for this stream; retry this same frame once through swscale.
                    DisableRepack(winrt::to_hresult());
                    *surface = nullptr;
                }
            }
            else if (Repack && !repackLogged && !repackFailed)
            {
                repackLogged = true;
                av_log(nullptr, AV_LOG_INFO, "Bale D3D11 repack not applicable (input format %d, %dx%d to %dx%d); using swscale.\n",
                    avFrame->format, avFrame->width, avFrame->height, outputWidth, outputHeight);
            }
            UploadFrame(avFrame, surface, framePts, params, false);
            return S_OK;
        }
        catch (...)
        {
            DisableUpload(winrt::to_hresult());
            *surface = nullptr;
            // Retry this very frame through the unchanged CPU path, permanently for this stream.
        }
    }

    FilmGrain.NoUpload();
    // Prepare ran before upload could fail: mode 0 must also skip CPU grain on this very frame.
    hasGrain = hasGrain && FilmGrain.mode != 0;
    if (outputDirectBuffer && !hasGrain)
    {
        // Never grain a decoder AVFrame: dav1d may retain it as a clean reference frame.
        // Direct references are safe only when no grain will be applied to this frame.
        auto bufferRef = av_buffer_ref(avFrame->buf[0]);
        if (bufferRef)
        {
            *pBuffer = NativeBufferFactory::CreateNativeBuffer(bufferRef->data, (UINT32)bufferRef->size, free_buffer, bufferRef);
        }
        else
        {
            hr = E_FAIL;
        }
    }
    else
    {
        // Copy/repack into a Bale-owned buffer, including upload-off and mid-stream disable.
        int linesize[4];
        UINT8* data[4];
        AVBufferRef* buffer;
        hr = FillLinesAndBuffer(linesize, data, &buffer, outputFrameWidth, outputFrameHeight, false);
        if (SUCCEEDED(hr))
        {
            hr = CopyFrameToOutput(avFrame, data, linesize, buffer);
            if (SUCCEEDED(hr) && hasGrain && !FilmGrain.ApplyCpu(grainParams, m_OutputPixelFormat, data, linesize))
            {
                hr = CopyFrameToOutput(avFrame, data, linesize, buffer);
            }
            if (SUCCEEDED(hr))
                *pBuffer = NativeBufferFactory::CreateNativeBuffer(buffer->data, (UINT32)buffer->size, free_buffer, buffer);
            else
                free_buffer(buffer);
        }
    }

    // Don't set a timestamp on S_FALSE
    if (hr == S_OK)
    {
        ReadFrameProperties(avFrame, framePts);
    }

    return hr;
}


void UncompressedVideoSampleProvider::ReadFrameProperties(AVFrame* avFrame, int64_t& framePts)
{
    // Try to get the best effort timestamp for the frame.
    if (avFrame->best_effort_timestamp != AV_NOPTS_VALUE)
        framePts = avFrame->best_effort_timestamp;
    m_interlaced_frame = avFrame->flags & AV_FRAME_FLAG_INTERLACED;
    m_top_field_first = avFrame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
    m_chroma_location = avFrame->chroma_location;
    if (m_config.as<implementation::MediaSourceConfig>()->IsFrameGrabber && !IsCleanSample)
    {
        if (m_interlaced_frame)
        {
            // for interlaced content we need to decode two frames to get clean image
            if (!hasFirstInterlacedFrame)
            {
                hasFirstInterlacedFrame = true;
            }
            else
            {
                IsCleanSample = true;
            }
        }
        else
        {
            // for progressive video, we need a key frame or b frame
            IsCleanSample = avFrame->flags & AV_FRAME_FLAG_KEY || avFrame->pict_type == AV_PICTURE_TYPE_B;
        }
    }

    // metadata for jpeg and png is only loaded on frame decode. check for orientation now and apply.
    if (avFrame->metadata && m_pAvCodecCtx->frame_num == 1)
    {
        auto entry = av_dict_get(avFrame->metadata, "Orientation", NULL, 0);
        if (entry)
        {
            auto value = atoi(entry->value);
            UINT32 rotationAngle = 0;
            switch (value)
            {
            case 8:
                rotationAngle = 270;
                break;
            case 3:
                rotationAngle = 180;
                break;
            case 6:
                rotationAngle = 90;
                break;
            }
            if (rotationAngle)
            {
                auto videoProperties = this->StreamDescriptor().as<VideoStreamDescriptor>().EncodingProperties();
                //Platform::Guid MF_MT_VIDEO_ROTATION(0xC380465D, 0x2271, 0x428C, 0x9B, 0x83, 0xEC, 0xEA, 0x3B, 0x4A, 0x85, 0xC1);
                videoProperties.Properties().Insert(MF_MT_VIDEO_ROTATION, winrt::box_value((UINT32)rotationAngle));
            }
        }
    }

    auto sideData = av_frame_get_side_data(avFrame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sideData)
    {
        auto contentLightMetadata = reinterpret_cast<AVContentLightMetadata*>(sideData->data);
        maxCLL = PropertyValue::CreateUInt32(contentLightMetadata->MaxCLL);
        maxFALL = PropertyValue::CreateUInt32(contentLightMetadata->MaxFALL);
    }
    else
    {
        maxCLL = maxFALL = nullptr;
    }

    sideData = av_frame_get_side_data(avFrame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sideData)
    {
        auto masteringDisplayMetadata = reinterpret_cast<AVMasteringDisplayMetadata*>(sideData->data);
        if (masteringDisplayMetadata->has_luminance)
        {
            constexpr UINT32 MASTERING_DISP_LUMINANCE_SCALE{ 10000 };
            minLuminance = PropertyValue::CreateUInt32(static_cast<UINT32>(MASTERING_DISP_LUMINANCE_SCALE * av_q2d(masteringDisplayMetadata->min_luminance)));
            maxLuminance = PropertyValue::CreateUInt32(static_cast<UINT32>(av_q2d(masteringDisplayMetadata->max_luminance)));
        }
        else
        {
            minLuminance = maxLuminance = nullptr;
        }

        if (masteringDisplayMetadata->has_primaries)
        {
            MT_CUSTOM_VIDEO_PRIMARIES customVideoPrimaries
            {
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][1])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[0])),
                static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[1]))
            };
            auto data = winrt::array_view<UINT8>(reinterpret_cast<UINT8*>(&customVideoPrimaries), sizeof(customVideoPrimaries));
            customPrimaries = PropertyValue::CreateUInt8Array(data);
        }
        else
        {
            customPrimaries = nullptr;
        }
    }
    else
    {
        minLuminance = maxLuminance = customPrimaries = nullptr;
    }
}

HRESULT UncompressedVideoSampleProvider::SetSampleProperties(MediaStreamSample  const& sample)
{
    if (m_interlaced_frame)
    {
        sample.ExtendedProperties().Insert(MFSampleExtension_Interlaced, winrt::box_value(TRUE));
        sample.ExtendedProperties().Insert(MFSampleExtension_BottomFieldFirst, m_top_field_first ? winrt::box_value(FALSE) : winrt::box_value(TRUE));
        sample.ExtendedProperties().Insert(MFSampleExtension_RepeatFirstField, winrt::box_value(FALSE));
    }
    else
    {
        sample.ExtendedProperties().Insert(MFSampleExtension_Interlaced, winrt::box_value(FALSE));
    }

    switch (m_chroma_location)
    {
    case AVCHROMA_LOC_LEFT:
        sample.ExtendedProperties().Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_MPEG2));
        break;
    case AVCHROMA_LOC_CENTER:
        sample.ExtendedProperties().Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_MPEG1));
        break;
    case AVCHROMA_LOC_TOPLEFT:
        if (m_interlaced_frame)
        {
            sample.ExtendedProperties().Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_DV_PAL));
        }
        else
        {
            sample.ExtendedProperties().Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_Cosited));
        }
        break;
    default:
        break;
    }

    if (maxCLL && maxFALL)
    {
        sample.ExtendedProperties().Insert(MF_MT_MAX_LUMINANCE_LEVEL, maxCLL);
        sample.ExtendedProperties().Insert(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL, maxFALL);
    }

    if (minLuminance && maxLuminance)
    {
        sample.ExtendedProperties().Insert(MF_MT_MIN_MASTERING_LUMINANCE, minLuminance);
        sample.ExtendedProperties().Insert(MF_MT_MAX_MASTERING_LUMINANCE, maxLuminance);
    }

    if (customPrimaries)
    {
        sample.ExtendedProperties().Insert(MF_MT_CUSTOM_VIDEO_PRIMARIES, customPrimaries);
    }

    return S_OK;
}

HRESULT UncompressedVideoSampleProvider::FillLinesAndBuffer(int* linesize, BYTE** data, AVBufferRef** buffer, int width, int height, bool isSourceBuffer)
{
    // this method more or less follows the ffmpeg av_image_alloc() implementation

    ptrdiff_t linesizes1[4];
    size_t sizes[4];
    int totalSize = 0;

    if (av_image_check_size(width, height, 0, NULL) < 0)
    {
        return E_FAIL;
    }

    if (av_image_fill_linesizes(linesize, m_OutputPixelFormat, width) < 0)
    {
        return E_FAIL;
    }

    for (int i = 0; i < 4; i++) {
        linesizes1[i] = linesize[i];
    }

    if (av_image_fill_plane_sizes(sizes, m_OutputPixelFormat, height, linesizes1) < 0)
    {
        return E_FAIL;
    }

    for (int i = 0; i < 4; i++) {
        if (sizes[i] > INT_MAX - totalSize)
            return AVERROR(EINVAL);
        totalSize += (int)sizes[i];
    }

    // allocate buffer
    buffer[0] = AllocateBuffer(totalSize, isSourceBuffer ? &sourceBufferPool : &targetBufferPool, isSourceBuffer ? &sourceBufferPoolSize : &targetBufferPoolSize);
    if (!buffer[0])
    {
        return E_OUTOFMEMORY;
    }

    // fill pointers
    if (av_image_fill_pointers(data, m_OutputPixelFormat, height, buffer[0]->data, linesize) < 0)
    {
        return E_FAIL;
    }

    return S_OK;
}

AVBufferRef* UncompressedVideoSampleProvider::AllocateBuffer(int requestedSize, AVBufferPool** bufferPool, int* bufferPoolSize)
{
    if (m_config.as<implementation::MediaSourceConfig>()->IsFrameGrabber && TargetBuffer)
    {
        auto bufferRef = av_buffer_create(TargetBuffer, requestedSize, [](void*, BYTE*) {}, NULL, 0);
        return bufferRef;
    }

    if (*bufferPool && *bufferPoolSize != requestedSize)
    {
        av_buffer_pool_uninit(bufferPool);
    }

    if (!*bufferPool)
    {
        *bufferPool = av_buffer_pool_init(requestedSize, NULL);
        if (!*bufferPool)
        {
            return NULL;
        }
        *bufferPoolSize = requestedSize;
    }

    auto buffer = av_buffer_pool_get(*bufferPool);
    if (!buffer)
    {
        return NULL;
    }

    return buffer;
}

int UncompressedVideoSampleProvider::get_buffer2(AVCodecContext* avCodecContext, AVFrame* frame, int flags)
{
    auto provider = reinterpret_cast<UncompressedVideoSampleProvider*>(avCodecContext->opaque);

    if (frame->format == AV_PIX_FMT_D3D11 || frame->format == AV_PIX_FMT_D3D11VA_VLD)
    {
        // custom buffer allocation not supported for HW formats. switch to default function.
        avCodecContext->get_buffer2 = avcodec_default_get_buffer2;
        return avcodec_default_get_buffer2(avCodecContext, frame, flags);
    }
    else
    {
        return provider->FillLinesAndBuffer(frame->linesize, frame->data, frame->buf, frame->width, frame->height, true);
    }
}

// Bale zero-copy: only the native p1 libdav1d allocator calls this (stock libdav1d never calls
// get_buffer2), on the thread inside avcodec_receive_frame. One bale_zc buffer holds the three
// planes with 64-byte aligned starts/strides and 64 bytes of padding after each plane (dav1d's
// allocator contract). Any refusal (< 0) makes dav1d use its own pool for that picture.
int UncompressedVideoSampleProvider::get_zero_copy_buffer(AVCodecContext* avCodecContext, AVFrame* frame, int flags)
{
    UNREFERENCED_PARAMETER(flags);
    auto provider = reinterpret_cast<UncompressedVideoSampleProvider*>(avCodecContext->opaque);
    auto handle = provider->zeroCopy.load(std::memory_order_acquire);
    if (!handle || frame->format != AV_PIX_FMT_YUV420P10LE) return AVERROR(ENOSYS);
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384) return AVERROR(EINVAL);

    static constexpr size_t align = 64;
    auto alignUp = [](size_t value) { return (value + align - 1) / align * align; };
    size_t width = static_cast<size_t>(frame->width);
    size_t height = static_cast<size_t>(frame->height);
    size_t lumaStride = alignUp(width * 2);
    size_t chromaStride = alignUp((width + 1) / 2 * 2);
    size_t lumaSize = lumaStride * height;
    size_t chromaSize = chromaStride * ((height + 1) / 2);
    size_t offsetU = alignUp(lumaSize + align);
    size_t offsetV = alignUp(offsetU + chromaSize + align);
    size_t bytes = offsetV + chromaSize + align;

    const auto& api = BaleZeroCopyExports::Get();
    uint8_t* data = nullptr;
    void* id = nullptr;
    // Bounded: a full pool waits briefly, then fails and this picture uses dav1d's pool.
    if (api.alloc(handle, bytes, &data, &id) != S_OK || !data || !id) return AVERROR(ENOMEM);
    if (reinterpret_cast<uintptr_t>(data) % align)
    {
        api.release(id);
        return AVERROR(EINVAL);
    }
    // The buffer's opaque is the bale_zc id; TryZeroCopy hands it to bale_zc_repack. Unref (from any
    // thread, also after the handle is destroyed) returns the memory to its pool.
    frame->buf[0] = av_buffer_create(data, bytes,
        [](void* opaque, uint8_t*) { BaleZeroCopyExports::Get().release(opaque); }, id, 0);
    if (!frame->buf[0])
    {
        api.release(id);
        return AVERROR(ENOMEM);
    }
    frame->data[0] = data;
    frame->data[1] = data + offsetU;
    frame->data[2] = data + offsetV;
    frame->linesize[0] = static_cast<int>(lumaStride);
    frame->linesize[1] = static_cast<int>(chromaStride);
    frame->linesize[2] = static_cast<int>(chromaStride);
    return 0;
}

// Runs once, from SetUploadDevice. Failure only logs: every frame keeps the staging path.
void UncompressedVideoSampleProvider::CreateZeroCopy(winrt::com_ptr<ID3D11Device> const& device, winrt::com_ptr<ID3D11DeviceContext> const& context)
{
    if (!zeroCopyHooked || zeroCopy.load()) return;
    const auto& api = BaleZeroCopyExports::Get();
    if (!api.create)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy unavailable (bale_gpu zero-copy ABI 1 not found); using staging upload.\n");
        return;
    }
    void* handle = nullptr;
    auto hr = api.create(device.get(), context.get(), &handle);
    if (hr != S_OK || !handle)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy create failed (hr=0x%08X); using staging upload.\n",
            static_cast<unsigned>(hr == S_OK ? E_UNEXPECTED : hr));
        return;
    }
    zeroCopy.store(handle, std::memory_order_release);
    av_log(nullptr, AV_LOG_INFO, "Bale zero-copy created (self-test passed).\n");
}

void UncompressedVideoSampleProvider::LogZeroCopyStats(void* handle, const char* when)
{
    BaleZcStats stats{};
    if (BaleZeroCopyExports::Get().stats(handle, &stats) != S_OK) return;
    av_log(nullptr, AV_LOG_INFO, "Bale zero-copy stats (%s): frames=%u repacked=%llu buffers created=%llu live=%llu peak=%llu "
        "pooled=%llu alloc_failures=%llu alloc_waits=%llu landing_copies=%llu direct=%u.\n", when, zeroCopyFrames,
        static_cast<unsigned long long>(stats.framesRepacked), static_cast<unsigned long long>(stats.buffersCreated),
        static_cast<unsigned long long>(stats.buffersLive), static_cast<unsigned long long>(stats.buffersPeak),
        static_cast<unsigned long long>(stats.buffersPooled), static_cast<unsigned long long>(stats.allocFailures),
        static_cast<unsigned long long>(stats.allocWaits), static_cast<unsigned long long>(stats.landingCopies), zeroCopyDirectFrames);
}

// Permanent for this stream. Callers are serialized with decoding (see zeroCopy). Buffers dav1d
// still holds stay valid and are released into the closed pool later.
void UncompressedVideoSampleProvider::DisableZeroCopy(HRESULT hr)
{
    auto handle = zeroCopy.exchange(nullptr);
    if (!handle) return;
    LogZeroCopyStats(handle, FAILED(hr) ? "disabled" : "closed");
    if (FAILED(hr))
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy disabled (hr=0x%08X); using staging upload.\n", static_cast<unsigned>(hr));
    }
    BaleZeroCopyExports::Get().destroy(handle);
}

// True when this frame went out as a zero-copy sample. False leaves the frame to the staging paths:
// not a zero-copy picture (dav1d's own pool), a layout the repack refuses, or a disabled handle.
// Upload failures after a successful repack throw, like the staging paths.
bool UncompressedVideoSampleProvider::TryZeroCopy(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts, const BaleGrainParams* params)
{
    auto handle = zeroCopy.load(std::memory_order_acquire);
    // CPU grain needs the converted frame in mapped staging.
    if (!handle || (params && FilmGrain.mode == 1)) return false;
    if (avFrame->format != AV_PIX_FMT_YUV420P10LE || m_OutputPixelFormat != AV_PIX_FMT_P010LE ||
        avFrame->width != outputWidth || avFrame->height != outputHeight) return false;
    auto buffer = avFrame->buf[0];
    if (!buffer || avFrame->buf[1] || avFrame->linesize[0] <= 0 || avFrame->linesize[1] <= 0 ||
        avFrame->linesize[1] != avFrame->linesize[2]) return false;
    // Whole 4-aligned texture rows: the repack reads the decoder's row padding past the picture,
    // and dav1d's pictures are 128-aligned, so those rows are inside the buffer (fits() checks).
    BaleZcFrame frame{};
    frame.width = static_cast<uint32_t>(outputFrameWidth);
    frame.height = static_cast<uint32_t>(outputFrameHeight);
    frame.lumaStride = static_cast<uint32_t>(avFrame->linesize[0]);
    frame.chromaStride = static_cast<uint32_t>(avFrame->linesize[1]);
    for (int i = 0; i < 3; i++)
    {
        if (!avFrame->data[i] || avFrame->data[i] < buffer->data) return false;
        frame.offsets[i] = static_cast<uint64_t>(avFrame->data[i] - buffer->data);
    }

    const auto& api = BaleZeroCopyExports::Get();
    auto& timing = upload->timing;
    upload->SetOutput(outputFrameWidth, outputFrameHeight, DXGI_FORMAT_P010);
    if (UploadFaultFrames > 0 && uploadedFrames >= UploadFaultFrames)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload fault injected after %d frames.\n", uploadedFrames);
        winrt::throw_hresult(E_OUTOFMEMORY);
    }
    auto repackStart = BaleUploadTiming::Now();
    if (timing.lastEnd) timing.Add(BaleUploadTiming::Gap, repackStart - timing.lastEnd);
    auto picture = av_buffer_get_opaque(buffer);
    HRESULT hr = S_FALSE;

    // Direct write into the pooled texture: no landing texture, no D3D11 copy. GPU grain still
    // needs the clean landing frame as its input.
    if (api.repackInto && upload->CanWriteDirect() && !(params && FilmGrain.mode == 2))
    {
        TimeSpan position{};
        uploadSample = upload->CreateWrittenSample(position, surface, [&](ID3D11Texture2D* texture)
            {
                hr = api.repackInto(handle, picture, &frame, nullptr, texture);
                if (hr != S_OK) return false;
                // Once per frame (frame-grabber state), and only for a frame that goes out here.
                ReadFrameProperties(avFrame, framePts);
                position = ConvertPosition(framePts);
                return true;
            });
        if (hr == S_OK)
        {
            timing.EndFrame(3);
            uploadedFrames++;
            zeroCopyDirectFrames++;
            if (++zeroCopyFrames == 240) LogZeroCopyStats(handle, "steady");
            if (!zeroCopyLogged)
            {
                zeroCopyLogged = true;
                av_log(nullptr, AV_LOG_INFO, "Bale zero-copy active (direct write): P010 %dx%d.\n", outputFrameWidth, outputFrameHeight);
            }
            return true;
        }
        if (hr == S_FALSE && !zeroCopyDeclineLogged)
        {
            zeroCopyDeclineLogged = true;
            av_log(nullptr, AV_LOG_INFO, "Bale zero-copy direct write declined; landing frames instead.\n");
        }
    }

    void* landing = nullptr;
    if (hr == S_FALSE) hr = api.repack(handle, picture, &frame, &landing);
    if (hr == S_FALSE) return false;
    if (hr == E_INVALIDARG)
    {
        if (!zeroCopyLayoutLogged)
        {
            zeroCopyLayoutLogged = true;
            av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy layout refused (%dx%d, strides %d/%d); using staging upload for such frames.\n",
                outputFrameWidth, outputFrameHeight, avFrame->linesize[0], avFrame->linesize[1]);
        }
        return false;
    }
    if (hr != S_OK || !landing)
    {
        DisableZeroCopy(hr == S_OK ? E_UNEXPECTED : hr);
        return false;
    }
    timing.Add(BaleUploadTiming::Fill, BaleUploadTiming::Now() - repackStart);

    // Borrowed from the handle: hold a reference while MF/D3D11 calls run below.
    winrt::com_ptr<ID3D11Texture2D> texture;
    texture.copy_from(static_cast<ID3D11Texture2D*>(landing));
    ReadFrameProperties(avFrame, framePts);
    uploadSample = upload->CreateLandedSample(texture.get(), ConvertPosition(framePts), surface, FilmGrain, params);
    timing.EndFrame(2);
    uploadedFrames++;
    if (++zeroCopyFrames == 240) LogZeroCopyStats(handle, "steady");
    if (!zeroCopyLogged)
    {
        zeroCopyLogged = true;
        av_log(nullptr, AV_LOG_INFO, "Bale zero-copy active: P010 %dx%d.\n", outputFrameWidth, outputFrameHeight);
    }
    return true;
}


void UncompressedVideoSampleProvider::CheckFrameSize(AVFrame* avFrame)
{
    outputDirectBuffer = true;
    int frameWidth = avFrame->width;
    int frameHeight = avFrame->height;

    // check if format has contiguous buffer for direct buffer approach
    if (avFrame->format == m_OutputPixelFormat && avFrame->buf && avFrame->buf[0] && avFrame->data && avFrame->linesize)
    {
        auto firstBufferStart = avFrame->buf[0]->data;
        auto firstBufferEnd = firstBufferStart + avFrame->buf[0]->size;
        for (int i = 0; i < 4; i++)
        {
            if (avFrame->linesize[i] && !(avFrame->data[i] >= firstBufferStart && avFrame->data[i] <= firstBufferEnd))
            {
                outputDirectBuffer = false;
            }
        }
    }
    else
    {
        outputDirectBuffer = false;
    }

    // if format has more than one plane, calculate true contiguous frame height
    if (outputDirectBuffer && avFrame->linesize[1])
    {
        auto firstPlaneSize = avFrame->data[1] - avFrame->data[0];
        double calculatedFrameHeight = (double)firstPlaneSize / avFrame->linesize[0];
        if (calculatedFrameHeight == (int)calculatedFrameHeight)
        {
            frameHeight = (int)calculatedFrameHeight;
        }
        else
        {
            outputDirectBuffer = false;
        }
    }

    if (m_OutputPixelFormat != AV_PIX_FMT_BGRA)
    {
        // only BGRA supports unaligned image sizes, all others require 4 pixel alignment
        int frameWidthAligned = ((frameWidth - 1) / 4 * 4) + 4;
        int frameHeightAligned = ((frameHeight - 1) / 4 * 4) + 4;

        if (frameWidth != frameWidthAligned)
        {
            frameWidth = frameWidthAligned;
            outputDirectBuffer = false;
        }
        if (frameHeight != frameHeightAligned)
        {
            frameHeight = frameHeightAligned;
            outputDirectBuffer = false;
        }
    }

    bool hasFormatChanged = avFrame->width != outputWidth || avFrame->height != outputHeight ||
        frameWidth != outputFrameWidth || frameHeight != outputFrameHeight;

    bool isFrameGrabberOverride = TargetWidth > 0 && TargetHeight > 0;

    if (isFrameGrabberOverride)
    {
        outputWidth = outputFrameWidth = TargetWidth;
        outputHeight = outputFrameHeight = TargetHeight;
        outputDirectBuffer = false;
    }
    else if (hasFormatChanged)
    {
        OutputDebugStringW(L"Video Frame Size changed!\r\n");

        auto streamDescriptor = VideoDescriptor();
        if (streamDescriptor) {
            OutputDebugStringW(L"Perform dynamic format change.\r\n");
            auto encProp = streamDescriptor.EncodingProperties();
            MediaPropertySet encodingProperties{ encProp.Properties() };

            // dynamic output size switching
            outputWidth = avFrame->width;
            outputHeight = avFrame->height;
            outputFrameWidth = frameWidth;
            outputFrameHeight = frameHeight;

            encProp.Width(outputFrameWidth);
            encProp.Height(outputFrameHeight);

            MFVideoArea area;
            area.Area.cx = outputWidth;
            area.Area.cy = outputHeight;
            area.OffsetX.fract = 0;
            area.OffsetX.value = 0;
            area.OffsetY.fract = 0;
            area.OffsetY.value = 0;
            encodingProperties.Insert(MF_MT_MINIMUM_DISPLAY_APERTURE, PropertyValue::CreateUInt8Array(winrt::array_view((uint8_t*)&area, sizeof(MFVideoArea))));
        }
    }
}
