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
#include "NativeBufferFactory.h"
#include <mfapi.h>
#include <d3d10.h>
#include <unordered_set>
#include <cmath>
#include "VideoFilterFactory.h"
#include "VideoColorAttributes.h"

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

using namespace NativeBuffer;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;

namespace
{
    // Bale zero-copy: ids of the live bale_zc buffers get_zero_copy_buffer handed to dav1d. A
    // decoded picture whose buffer id is missing came from dav1d's own pool (alloc timeout, seek
    // burst, I400) and is dropped, never passed to bale_zc. Process-global and never destroyed:
    // buffer releases run on any thread, also after every provider is gone. Ids are never reused.
    class BaleZcLiveIds
    {
    public:
        bool Add(void* id) noexcept
        {
            AcquireSRWLockExclusive(&lock);
            bool added = false;
            try
            {
                added = ids.insert(id).second;
            }
            catch (...)
            {
            }
            ReleaseSRWLockExclusive(&lock);
            return added;
        }

        void Remove(void* id) noexcept
        {
            AcquireSRWLockExclusive(&lock);
            ids.erase(id);
            ReleaseSRWLockExclusive(&lock);
        }

        bool Contains(void* id) noexcept
        {
            AcquireSRWLockShared(&lock);
            bool found = ids.find(id) != ids.end();
            ReleaseSRWLockShared(&lock);
            return found;
        }

    private:
        SRWLOCK lock = SRWLOCK_INIT;
        std::unordered_set<void*> ids;
    };

    BaleZcLiveIds& LiveZeroCopyIds()
    {
        static BaleZcLiveIds* ids = new BaleZcLiveIds();
        return *ids;
    }
}

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

// True when a software-decoded input carries more than 8 bits per component (10/12-bit, any
// chroma layout or colour model). Its conversion outputs P010 so it never drops to 8 bits; 8-bit
// sources keep 8-bit NV12 (Bale's rule: keep the source's bit depth, never 10 to 8).
// Hardware frame formats are not conversions here.
bool UncompressedVideoSampleProvider::ConversionNeedsTenBitOutput(AVPixelFormat input)
{
    if (input == AV_PIX_FMT_NONE || input == AV_PIX_FMT_P010LE) return false;
    auto description = av_pix_fmt_desc_get(input);
    if (!description || (description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return false;
    int depth = 0;
    for (int i = 0; i < description->nb_components; i++)
    {
        depth = (std::max)(depth, description->comp[i].depth);
    }
    return depth > 8;
}

AVPixelFormat UncompressedVideoSampleProvider::SelectOutputPixelFormat(AVPixelFormat input, bool direct, MediaSourceConfig const& config, bool eightBitZeroCopy)
{
    if (config.as<implementation::MediaSourceConfig>()->IsFrameGrabber) return AV_PIX_FMT_BGRA;
    if (eightBitZeroCopy && input == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_P010LE;
    if (config.Video().VideoOutputAllowIyuv() && (input == AV_PIX_FMT_YUV420P || input == AV_PIX_FMT_YUVJ420P) && direct) return input;
    if (input == AV_PIX_FMT_YUV420P10LE && config.Video().VideoOutputAllow10bit()) return AV_PIX_FMT_P010LE;
    if (config.Video().VideoOutputAllow10bit() && ConversionNeedsTenBitOutput(input)) return AV_PIX_FMT_P010LE;
    if (config.Video().VideoOutputAllowNv12()) return AV_PIX_FMT_NV12;
    if (config.Video().VideoOutputAllowIyuv()) return input == AV_PIX_FMT_YUVJ420P ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    if (config.Video().VideoOutputAllowBgra8()) return AV_PIX_FMT_BGRA;
    return AV_PIX_FMT_NV12; // Upstream fallback when no format is allowed.
}

bool UncompressedVideoSampleProvider::EightBitZeroCopy(AVPixelFormat input, bool dav1d, bool hardware, int zeroCopy, MediaSourceConfig const& config)
{
    // YUVJ is not taken: the shift keeps sample values, it never converts range.
    return input == AV_PIX_FMT_YUV420P && dav1d && !hardware && zeroCopy > 0 &&
        !config.as<implementation::MediaSourceConfig>()->IsFrameGrabber && config.Video().VideoOutputAllow10bit() &&
        BaleZeroCopyExports::Get().repackIntoBits != nullptr;
}

bool UncompressedVideoSampleProvider::ZeroCopyEligible(AVPixelFormat input, AVPixelFormat output, bool eightBitZeroCopy)
{
    return output == AV_PIX_FMT_P010LE &&
        (input == AV_PIX_FMT_YUV420P10LE || (eightBitZeroCopy && input == AV_PIX_FMT_YUV420P));
}

void UncompressedVideoSampleProvider::SelectOutputFormat()
{
    m_OutputPixelFormat = HasProcessedOutput() ? ProcessedOutputPixelFormat() : SelectOutputPixelFormat(m_pAvCodecCtx->pix_fmt,
        (m_pAvCodecCtx->codec->capabilities & AV_CODEC_CAP_DR1) && decoder != DecoderEngine::FFmpegD3D11HardwareDecoder, m_config,
        ZeroCopyEightBit);
    switch (m_OutputPixelFormat)
    {
    case AV_PIX_FMT_BGRA: outputMediaSubtype = MediaEncodingSubtypes::Bgra8(); break;
    case AV_PIX_FMT_P010LE: outputMediaSubtype = winrt::to_hstring(MFVideoFormat_P010); break;
    case AV_PIX_FMT_NV12: outputMediaSubtype = MediaEncodingSubtypes::Nv12(); break;
    default: outputMediaSubtype = MediaEncodingSubtypes::Iyuv(); break;
    }

    outputWidth = outputFrameWidth = m_pAvCodecCtx->width;
    outputHeight = outputFrameHeight = m_pAvCodecCtx->height;
    if (HasProcessedOutput())
    {
        ProcessedOutputSize(outputWidth, outputHeight);
        outputFrameWidth = outputWidth;
        outputFrameHeight = outputHeight;
    }

    if (m_OutputPixelFormat != AV_PIX_FMT_BGRA && !HasProcessedOutput())
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
        ZeroCopyEligible(m_pAvCodecCtx->pix_fmt, m_OutputPixelFormat, ZeroCopyEightBit))
    {
        // Bale zero-copy: the patched libdav1d (native p1) asks a replaced get_buffer2 for each
        // picture, per picture, so installing it before the first decode is enough. SetUploadDevice
        // creates the handle in OnStarting, before the first decode; without it the hook declines.
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

    SetCommonVideoEncodingProperties(videoProperties, false);

    MFVideoArea area;
    area.Area.cx = outputWidth;
    area.Area.cy = outputHeight;
    area.OffsetX.fract = 0;
    area.OffsetX.value = 0;
    area.OffsetY.fract = 0;
    area.OffsetY.value = 0;
    properties.Insert(MF_MT_MINIMUM_DISPLAY_APERTURE, PropertyValue::CreateUInt8Array(winrt::array_view((uint8_t*)&area, sizeof(MFVideoArea))));

    hasHdrMetadata = VideoColorAttributes::Apply(m_pAvStream->codecpar, applyHdrColorInfo, properties);
    isHdrActive = hasHdrMetadata && applyHdrColorInfo;

    if (HasProcessedOutput())
    {
        // The processing contract fixes the output colour space for the lifetime of the stream.
        properties.Insert(MF_MT_VIDEO_PRIMARIES, PropertyValue::CreateUInt32(MFVideoPrimaries_BT709));
        properties.Insert(MF_MT_TRANSFER_FUNCTION, PropertyValue::CreateUInt32(MFVideoTransFunc_709));
        properties.Insert(MF_MT_YUV_MATRIX, PropertyValue::CreateUInt32(MFVideoTransferMatrix_BT709));
        properties.Insert(MF_MT_VIDEO_NOMINAL_RANGE, PropertyValue::CreateUInt32(MFNominalRange_16_235));
        for (auto key : { MF_MT_MAX_LUMINANCE_LEVEL, MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL,
            MF_MT_MIN_MASTERING_LUMINANCE, MF_MT_MAX_MASTERING_LUMINANCE, MF_MT_CUSTOM_VIDEO_PRIMARIES })
        {
            if (properties.HasKey(key)) properties.Remove(key);
        }
        isHdrActive = false;
        AddProcessedOutputAttributes(properties);
    }

    videoProperties.Properties().Insert(MF_MT_INTERLACE_MODE, winrt::box_value((UINT32)_MFVideoInterlaceMode::MFVideoInterlace_MixedInterlaceOrProgressive));

    return VideoStreamDescriptor(videoProperties);
}

HRESULT UncompressedVideoSampleProvider::InitializeScalerIfRequired(AVFrame* avFrame)
{
    HRESULT hr = S_OK;

    // use bilinear for downscaling, bicubic for upscaling.
    // don't use point scaling since format conversions might involve scaling even if resolution is same (chroma subsampling)
    int scaler = avFrame->width < outputWidth ? SWS_BICUBIC : SWS_BILINEAR;

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

// Bale: shared renderer context calls must be protected, even on the decode-ahead worker.
// Only zero-copy and staged grain streams get an uploader; any setup failure fails the stream.
HRESULT UncompressedVideoSampleProvider::SetUploadDevice(winrt::com_ptr<ID3D11Device> newDevice, winrt::com_ptr<ID3D11DeviceContext> newContext, int variant)
{
    if (!ZeroCopy && !StagedGrain && !softwareStage) return S_FALSE;
    auto fatalHr = FatalError();
    if (FAILED(fatalHr)) return fatalHr;
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
        if (ZeroCopy)
        {
            upload = std::make_shared<BaleD3D11Upload>(newDevice, newContext, BaleD3D11Upload::Pool::ZeroCopy);
            winrt::check_hresult(CreateZeroCopy(newDevice, newContext));
        }
        else
        {
            // Staged grain or the software stage: plain render-target pool; the grain handle (and its
            // self-test) now when grain is on.
            upload = std::make_shared<BaleD3D11Upload>(newDevice, newContext, BaleD3D11Upload::Pool::Plain);
            if (StagedGrain) upload->PrepareGrain(FilmGrain);
        }
        return S_OK;
    }
    catch (...)
    {
        return Fail(winrt::to_hresult(), ZeroCopy ? "upload device or zero-copy setup failed" : "upload device or D3D11 grain/stage setup failed");
    }
}

void UncompressedVideoSampleProvider::DisableUpload(HRESULT hr)
{
    // Zero-copy and staged grain have no CPU output path: losing the upload device fails the stream.
    // Other streams never get an uploader.
    if (ZeroCopy || StagedGrain || softwareStage) Fail(hr, "upload device lost or unavailable");
}

// Bale: latches the first failure. Afterwards CreateBufferFromFrame/CreateNextSampleBuffer return
// it without decoding, and FFmpegMediaSource raises the MediaStreamSource error once.
HRESULT UncompressedVideoSampleProvider::Fail(HRESULT hr, const char* reason)
{
    if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
    HRESULT expected = S_OK;
    if (fatal.compare_exchange_strong(expected, hr))
    {
        av_log(nullptr, AV_LOG_ERROR, "Bale zero-copy failed: %s (hr=0x%08X); stopping the stream.\n",
            reason, static_cast<unsigned>(hr));
    }
    uploadSample = nullptr;
    // Before the uploader (and its D3D11 context use) goes away.
    DisableZeroCopy(hr);
    upload.reset();
    return fatal.load();
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
    if (stageScaler)
    {
        sws_freeContext(stageScaler);
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

    // Bale: a failed zero-copy stream decodes nothing more.
    auto fatalHr = FatalError();
    if (FAILED(fatalHr)) return fatalHr;

    HRESULT hr = S_OK;
    // Bale: the software stage's output size is the media type's for the stream's lifetime; CheckFrameSize
    // would take a frame of the decoded size as a size change.
    if (softwareStage) return SoftwareStageFrame(avFrame, surface, framePts);
    CheckFrameSize(avFrame);

    // Bale: zero-copy and staged grain streams have no CPU output path; every other stream is upstream.
    if (ZeroCopy) return ZeroCopyFrame(avFrame, surface, framePts);
    if (StagedGrain) return StagedGrainFrame(avFrame, surface, framePts);

    if (outputDirectBuffer)
    {
        // Using direct buffer: just create a buffer reference to hand out to MSS pipeline
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
    else if (avFrame->format == m_OutputPixelFormat && avFrame->width == outputWidth && avFrame->height == outputHeight)
    {
        // Same format and size but non-contiguous buffer: Copy to contiguous buffer
        int linesize[4];
        UINT8* data[4];
        AVBufferRef* buffer;

        hr = FillLinesAndBuffer(linesize, data, &buffer, outputFrameWidth, outputFrameHeight, false);

        if (SUCCEEDED(hr))
        {
            av_image_copy(data, linesize, (const UINT8**)avFrame->data, avFrame->linesize, m_OutputPixelFormat, outputWidth, outputHeight);
            *pBuffer = NativeBufferFactory::CreateNativeBuffer(buffer->data, (UINT32)buffer->size, free_buffer, buffer);
        }
    }
    else
    {
        // Different format or size: Use scaler
        hr = InitializeScalerIfRequired(avFrame);

        if (SUCCEEDED(hr))
        {
            int linesize[4];
            UINT8* data[4];
            AVBufferRef* buffer;

            hr = FillLinesAndBuffer(linesize, data, &buffer, outputFrameWidth, outputFrameHeight, false);

            if (SUCCEEDED(hr))
            {
                // Convert to output format using FFmpeg software scaler
                if (sws_scale(m_pSwsCtx, (const UINT8**)(avFrame->data), avFrame->linesize, 0, avFrame->height, data, linesize) > 0)
                {
                    *pBuffer = NativeBufferFactory::CreateNativeBuffer(buffer->data, (UINT32)buffer->size, free_buffer, buffer);
                }
                else
                {
                    free_buffer(buffer);
                    hr = E_FAIL;
                }
            }
        }
    }

    // Don't set a timestamp on S_FALSE
    if (hr == S_OK)
    {
        ReadFrameProperties(avFrame, framePts);
    }

    return hr;
}

// Bale: every zero-copy frame goes out as a GPU sample, is dropped (not a zero-copy picture), or
// fails the stream. There is no CPU, staging or ungrained fallback.
HRESULT UncompressedVideoSampleProvider::ZeroCopyFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts)
{
    if (!upload || !zeroCopy.load(std::memory_order_acquire))
    {
        *surface = nullptr;
        return Fail(E_UNEXPECTED, "frame decoded without the zero-copy handle");
    }
    BaleGrainParams grainParams{};
    bool hasGrain = FilmGrain.Prepare(avFrame, m_OutputPixelFormat, outputWidth, outputHeight, grainParams);
    HRESULT hr = S_OK;
    try
    {
        hr = TryZeroCopy(avFrame, surface, framePts, hasGrain ? &grainParams : nullptr);
    }
    catch (...)
    {
        hr = winrt::to_hresult();
        if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
    }
    if (hr == BaleFrameDropped) return DropFrame(avFrame);
    if (FAILED(hr))
    {
        *surface = nullptr;
        return Fail(hr, hr == ZeroCopyDeclined ? "zero-copy write declined" :
            hr == E_INVALIDARG ? "layout refused" : "GPU write or sample failed");
    }
    zeroCopyMissRun = 0;
    return S_OK;
}

// Bale: staged GPU grain. swscale converts the frame into a mapped STAGING texture (NV12 for 8-bit,
// P010 for 10-bit, the declared output); the D3D11 grain pass draws it into a pooled texture with the
// frame's grain, or a copy moves it there when the frame has none. Any failure fails the stream.
HRESULT UncompressedVideoSampleProvider::StagedGrainFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts)
{
    if (!upload)
    {
        *surface = nullptr;
        return Fail(E_UNEXPECTED, "frame decoded without the D3D11 grain upload");
    }
    BaleGrainParams grainParams{};
    bool hasGrain = FilmGrain.Prepare(avFrame, m_OutputPixelFormat, outputWidth, outputHeight, grainParams);
    try
    {
        DXGI_FORMAT format = m_OutputPixelFormat == AV_PIX_FMT_P010LE ? DXGI_FORMAT_P010
            : m_OutputPixelFormat == AV_PIX_FMT_NV12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_UNKNOWN;
        if (format == DXGI_FORMAT_UNKNOWN) winrt::throw_hresult(E_INVALIDARG);
        winrt::check_hresult(InitializeScalerIfRequired(avFrame));
        auto& timing = upload->timing;
        auto start = BaleUploadTiming::Now();
        if (timing.lastEnd) timing.Add(BaleUploadTiming::Gap, start - timing.lastEnd);
        BaleD3D11Upload::Mapping staging;
        winrt::check_hresult(upload->Map(outputFrameWidth, outputFrameHeight, format, staging));
        // A mapped planar texture: the chroma plane follows the luma plane's rows at the same pitch.
        auto base = static_cast<uint8_t*>(staging.mapped.pData);
        auto pitch = static_cast<int>(staging.mapped.RowPitch);
        uint8_t* data[4] = { base, base + static_cast<size_t>(pitch) * outputFrameHeight, nullptr, nullptr };
        int linesize[4] = { pitch, pitch, 0, 0 };
        auto fillStart = BaleUploadTiming::Now();
        if (sws_scale(m_pSwsCtx, (const UINT8**)(avFrame->data), avFrame->linesize, 0, avFrame->height, data, linesize) <= 0)
        {
            winrt::throw_hresult(E_FAIL);
        }
        timing.Add(BaleUploadTiming::Fill, BaleUploadTiming::Now() - fillStart);
        ReadFrameProperties(avFrame, framePts);
        uploadSample = upload->CreateStagedSample(staging, ConvertPosition(framePts), surface, FilmGrain, hasGrain ? &grainParams : nullptr);
        timing.EndFrame(0);
    }
    catch (...)
    {
        HRESULT hr = winrt::to_hresult();
        *surface = nullptr;
        return Fail(SUCCEEDED(hr) ? E_UNEXPECTED : hr, "D3D11 grain upload failed");
    }
    stagedGrainFrames++;
    if (hasGrain) stagedGrainAppliedFrames++;
    if (!stagedGrainLogged)
    {
        stagedGrainLogged = true;
        av_log(nullptr, AV_LOG_INFO, "Bale film grain: D3D11 staged upload active: %s %dx%d.\n",
            m_OutputPixelFormat == AV_PIX_FMT_P010LE ? "P010" : "NV12", outputFrameWidth, outputFrameHeight);
    }
    if (stagedGrainFrames % 240 == 0)
    {
        av_log(nullptr, AV_LOG_INFO, "Bale film grain: D3D11 staged frames=%u grained=%u.\n", stagedGrainFrames, stagedGrainAppliedFrames);
    }
    return S_OK;
}

bool UncompressedVideoSampleProvider::SetSoftwareStage(std::unique_ptr<VideoFrameProcessor> processor, int targetWidth, int targetHeight, int outputDepth)
{
    if (!processor || ZeroCopy || m_config.as<implementation::MediaSourceConfig>()->IsFrameGrabber) return false;
    int width = m_pAvCodecCtx->width;
    int height = m_pAvCodecCtx->height;
    auto description = av_pix_fmt_desc_get(m_pAvCodecCtx->pix_fmt);
    int depth = 0;
    if (description)
    {
        for (int i = 0; i < description->nb_components; i++) depth = (std::max)(depth, description->comp[i].depth);
    }
    // Source depth: 8-bit stays NV12 when the stage reads it; deeper sources (and 8-bit otherwise) P010.
    auto input = depth > 0 && depth <= 8 && processor->SupportsInputFormat(DXGI_FORMAT_NV12) ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || depth <= 0 || !processor->SupportsInputFormat(input))
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale software stage: declined for %dx%d depth=%d\n", width, height, depth);
        return false;
    }
    stageSourceWidth = width;
    stageSourceHeight = height;
    stageInputFormat = input;
    stageInputPixelFormat = input == DXGI_FORMAT_NV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
    stageOutputP010 = VideoFrameProcessor::SelectOutputFormat(*processor, outputDepth) == DXGI_FORMAT_P010;
    stageOutputWidth = width;
    stageOutputHeight = height;
    VideoFrameProcessor::SelectOutputSize(*processor, width, height, targetWidth, targetHeight, stageOutputWidth, stageOutputHeight);
    softwareStage = std::move(processor);
    av_log(nullptr, AV_LOG_INFO, "Bale software stage: %dx%d %s -> %dx%d %s\n", width, height,
        input == DXGI_FORMAT_NV12 ? "NV12" : "P010", stageOutputWidth, stageOutputHeight, stageOutputP010 ? "P010" : "NV12");
    return true;
}

// Bale: the software stage. swscale converts the frame into a mapped STAGING texture of the stage's
// input format at the decoded size; the upload copies it (or applies GPU grain) into a shader-readable
// texture, and the stage writes the processed output into a pooled texture. Any failure fails the stream.
HRESULT UncompressedVideoSampleProvider::SoftwareStageFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts)
{
    if (!upload)
    {
        *surface = nullptr;
        return Fail(E_UNEXPECTED, "frame decoded without the software stage's upload device");
    }
    if (avFrame->width != stageSourceWidth || avFrame->height != stageSourceHeight)
    {
        *surface = nullptr;
        av_log(nullptr, AV_LOG_ERROR, "Bale software stage: frame %dx%d, stream %dx%d\n", avFrame->width, avFrame->height,
            stageSourceWidth, stageSourceHeight);
        return Fail(E_INVALIDARG, "software stage: the frame size changed");
    }
    BaleGrainParams grainParams{};
    bool hasGrain = StagedGrain && FilmGrain.Prepare(avFrame, stageInputPixelFormat, stageSourceWidth, stageSourceHeight, grainParams);
    try
    {
        stageScaler = sws_getCachedContext(stageScaler, avFrame->width, avFrame->height, static_cast<AVPixelFormat>(avFrame->format),
            stageSourceWidth, stageSourceHeight, stageInputPixelFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!stageScaler) winrt::throw_hresult(E_OUTOFMEMORY);
        auto& timing = upload->timing;
        auto start = BaleUploadTiming::Now();
        if (timing.lastEnd) timing.Add(BaleUploadTiming::Gap, start - timing.lastEnd);
        BaleD3D11Upload::Mapping staging;
        winrt::check_hresult(upload->MapStaging(stageSourceWidth, stageSourceHeight, stageInputFormat, staging));
        // A mapped planar texture: the chroma plane follows the luma plane's rows at the same pitch.
        auto base = static_cast<uint8_t*>(staging.mapped.pData);
        auto pitch = static_cast<int>(staging.mapped.RowPitch);
        uint8_t* data[4] = { base, base + static_cast<size_t>(pitch) * stageSourceHeight, nullptr, nullptr };
        int linesize[4] = { pitch, pitch, 0, 0 };
        auto fillStart = BaleUploadTiming::Now();
        if (sws_scale(stageScaler, (const UINT8**)(avFrame->data), avFrame->linesize, 0, avFrame->height, data, linesize) <= 0)
        {
            winrt::throw_hresult(E_FAIL);
        }
        timing.Add(BaleUploadTiming::Fill, BaleUploadTiming::Now() - fillStart);
        upload->SetOutput(stageOutputWidth, stageOutputHeight, stageOutputP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12);
        ReadFrameProperties(avFrame, framePts);
        auto color = VideoFrameColorInfo::FromFrame(avFrame, m_pAvStream->codecpar);
        uploadSample = upload->CreateProcessedSample(staging, ConvertPosition(framePts), surface, FilmGrain,
            hasGrain ? &grainParams : nullptr, *softwareStage, color, stageSourceWidth, stageSourceHeight);
        timing.EndFrame(0);
    }
    catch (...)
    {
        HRESULT hr = winrt::to_hresult();
        *surface = nullptr;
        if (softwareStage) softwareStage->NotifyError(SUCCEEDED(hr) ? E_UNEXPECTED : hr);
        return Fail(SUCCEEDED(hr) ? E_UNEXPECTED : hr, "software stage failed");
    }
    if (!stageLogged)
    {
        stageLogged = true;
        av_log(nullptr, AV_LOG_INFO, "Bale software stage active: %dx%d -> %dx%d\n", stageSourceWidth, stageSourceHeight,
            stageOutputWidth, stageOutputHeight);
    }
    if (++stageFrames % 240 == 0)
    {
        av_log(nullptr, AV_LOG_INFO, "Bale software stage: frames=%u\n", stageFrames);
    }
    return S_OK;
}

// Bale: a picture outside the zero-copy buffers is never staged. It yields no sample; more than
// 8 in a row fail the stream.
HRESULT UncompressedVideoSampleProvider::DropFrame(AVFrame* avFrame)
{
    zeroCopyDropped++;
    if (!zeroCopyDropLogged)
    {
        zeroCopyDropLogged = true;
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy: dropped a frame outside zero-copy buffers (format %d, %dx%d).\n",
            avFrame->format, avFrame->width, avFrame->height);
    }
    if (++zeroCopyMissRun > 8) return Fail(ZeroCopyMissed, "more than 8 consecutive frames outside zero-copy buffers");
    return BaleFrameDropped;
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

    if (HasProcessedOutput())
    {
        // Source HDR metadata belongs to the processor, never to its SDR output samples.
        maxCLL = maxFALL = minLuminance = maxLuminance = customPrimaries = nullptr;
        return;
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
            minLuminance = PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(MASTERING_DISP_LUMINANCE_SCALE * av_q2d(masteringDisplayMetadata->min_luminance))));
            maxLuminance = PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(av_q2d(masteringDisplayMetadata->max_luminance))));
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
// allocator contract). Any refusal (< 0) makes dav1d use its own pool for that picture, which
// CreateBufferFromFrame then drops (DropFrame).
int UncompressedVideoSampleProvider::get_zero_copy_buffer(AVCodecContext* avCodecContext, AVFrame* frame, int flags)
{
    UNREFERENCED_PARAMETER(flags);
    auto provider = reinterpret_cast<UncompressedVideoSampleProvider*>(avCodecContext->opaque);
    auto handle = provider->zeroCopy.load(std::memory_order_acquire);
    bool eightBit = frame->format == AV_PIX_FMT_YUV420P && provider->ZeroCopyEightBit;
    if (!handle || (frame->format != AV_PIX_FMT_YUV420P10LE && !eightBit)) return AVERROR(ENOSYS);
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384) return AVERROR(EINVAL);

    static constexpr size_t align = 64;
    auto alignUp = [](size_t value) { return (value + align - 1) / align * align; };
    size_t width = static_cast<size_t>(frame->width);
    size_t height = static_cast<size_t>(frame->height);
    size_t sample = eightBit ? 1 : 2;
    size_t lumaStride = alignUp(width * sample);
    size_t chromaStride = alignUp((width + 1) / 2 * sample);
    size_t lumaSize = lumaStride * height;
    size_t chromaSize = chromaStride * ((height + 1) / 2);
    size_t offsetU = alignUp(lumaSize + align);
    size_t offsetV = alignUp(offsetU + chromaSize + align);
    size_t bytes = offsetV + chromaSize + align;

    const auto& api = BaleZeroCopyExports::Get();
    uint8_t* data = nullptr;
    void* id = nullptr;
    // Bounded: a full pool waits briefly, then fails and this picture uses dav1d's pool (dropped).
    if (api.alloc(handle, bytes, &data, &id) != S_OK || !data || !id) return AVERROR(ENOMEM);
    if (reinterpret_cast<uintptr_t>(data) % align)
    {
        api.release(id);
        return AVERROR(EINVAL);
    }
    // The buffer's opaque is the bale_zc id; TryZeroCopy hands it to bale_zc_repack. Unref (from any
    // thread, also after the handle is destroyed) returns the memory to its pool.
    auto& live = LiveZeroCopyIds();
    if (!live.Add(id))
    {
        api.release(id);
        return AVERROR(ENOMEM);
    }
    frame->buf[0] = av_buffer_create(data, bytes,
        [](void* opaque, uint8_t*)
        {
            LiveZeroCopyIds().Remove(opaque);
            BaleZeroCopyExports::Get().release(opaque);
        }, id, 0);
    if (!frame->buf[0])
    {
        live.Remove(id);
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

// Runs from SetUploadDevice. Any failure fails the stream (there is no staging upload).
HRESULT UncompressedVideoSampleProvider::CreateZeroCopy(winrt::com_ptr<ID3D11Device> const& device, winrt::com_ptr<ID3D11DeviceContext> const& context)
{
    if (zeroCopy.load()) return S_OK;
    if (!zeroCopyHooked)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy unavailable (decoder buffer hook not installed).\n");
        return E_UNEXPECTED;
    }
    const auto& api = BaleZeroCopyExports::Get();
    if (!api.create)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy unavailable (bale_gpu zero-copy ABI 1 not found).\n");
        return E_NOINTERFACE;
    }
    void* handle = nullptr;
    auto hr = api.create(device.get(), context.get(), &handle);
    if (hr != S_OK || !handle)
    {
        hr = FAILED(hr) ? hr : E_UNEXPECTED;
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy create failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        return hr;
    }
    zeroCopy.store(handle, std::memory_order_release);
    av_log(nullptr, AV_LOG_INFO, "Bale zero-copy created (self-test passed).\n");
    return S_OK;
}

void UncompressedVideoSampleProvider::LogZeroCopyStats(void* handle, const char* when)
{
    BaleZcStats stats{};
    if (BaleZeroCopyExports::Get().stats(handle, &stats) != S_OK) return;
    av_log(nullptr, AV_LOG_INFO, "Bale zero-copy stats (%s): frames=%u repacked=%llu buffers created=%llu live=%llu peak=%llu "
        "pooled=%llu alloc_failures=%llu alloc_waits=%llu landing_copies=%llu direct=%u direct_grain=%u eight_bit=%u dropped=%u.\n", when, zeroCopyFrames,
        static_cast<unsigned long long>(stats.framesRepacked), static_cast<unsigned long long>(stats.buffersCreated),
        static_cast<unsigned long long>(stats.buffersLive), static_cast<unsigned long long>(stats.buffersPeak),
        static_cast<unsigned long long>(stats.buffersPooled), static_cast<unsigned long long>(stats.allocFailures),
        static_cast<unsigned long long>(stats.allocWaits), static_cast<unsigned long long>(stats.landingCopies), zeroCopyDirectFrames,
        zeroCopyDirectGrainFrames, zeroCopyEightBitFrames, zeroCopyDropped);
}

// Permanent for this stream: from Fail (failure) or the destructor (S_OK). Callers are serialized
// with decoding (see zeroCopy). Buffers dav1d still holds stay valid and are released into the
// closed pool later.
void UncompressedVideoSampleProvider::DisableZeroCopy(HRESULT hr)
{
    auto handle = zeroCopy.exchange(nullptr);
    if (!handle) return;
    LogZeroCopyStats(handle, FAILED(hr) ? "disabled" : "closed");
    BaleZeroCopyExports::Get().destroy(handle);
}

// S_OK: the frame went out as a zero-copy sample. BaleFrameDropped: not a zero-copy picture
// (dav1d's own pool). Any failure fails the stream: a declined write (ZeroCopyDeclined), a refused
// layout (E_INVALIDARG) or a bale_zc error; D3D11/MF failures after a write throw.
HRESULT UncompressedVideoSampleProvider::TryZeroCopy(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts, const BaleGrainParams* params)
{
    auto handle = zeroCopy.load(std::memory_order_acquire);
    if (!handle) return E_UNEXPECTED;
    auto buffer = avFrame->buf[0];
    if (!buffer || !LiveZeroCopyIds().Contains(av_buffer_get_opaque(buffer))) return BaleFrameDropped;
    auto refused = [&]
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy layout refused (format %d, %dx%d to %dx%d, strides %d/%d).\n",
            avFrame->format, avFrame->width, avFrame->height, outputFrameWidth, outputFrameHeight,
            avFrame->linesize[0], avFrame->linesize[1]);
        return E_INVALIDARG;
    };
    // 8-bit pictures are written as P010 (samples << 8) by the direct write only; never landed.
    bool eightBit = avFrame->format == AV_PIX_FMT_YUV420P && ZeroCopyEightBit;
    if ((avFrame->format != AV_PIX_FMT_YUV420P10LE && !eightBit) || m_OutputPixelFormat != AV_PIX_FMT_P010LE ||
        avFrame->width != outputWidth || avFrame->height != outputHeight) return refused();
    if (avFrame->buf[1] || avFrame->linesize[0] <= 0 || avFrame->linesize[1] <= 0 ||
        avFrame->linesize[1] != avFrame->linesize[2]) return refused();
    // Whole 4-aligned texture rows: the repack reads the decoder's row padding past the picture,
    // and dav1d's pictures are 128-aligned, so those rows are inside the buffer (fits() checks).
    BaleZcFrame frame{};
    frame.width = static_cast<uint32_t>(outputFrameWidth);
    frame.height = static_cast<uint32_t>(outputFrameHeight);
    frame.lumaStride = static_cast<uint32_t>(avFrame->linesize[0]);
    frame.chromaStride = static_cast<uint32_t>(avFrame->linesize[1]);
    for (int i = 0; i < 3; i++)
    {
        if (!avFrame->data[i] || avFrame->data[i] < buffer->data) return refused();
        frame.offsets[i] = static_cast<uint64_t>(avFrame->data[i] - buffer->data);
    }

    const auto& api = BaleZeroCopyExports::Get();
    auto& timing = upload->timing;
    upload->SetOutput(outputFrameWidth, outputFrameHeight, DXGI_FORMAT_P010);
    auto repackStart = BaleUploadTiming::Now();
    if (timing.lastEnd) timing.Add(BaleUploadTiming::Gap, repackStart - timing.lastEnd);
    auto picture = av_buffer_get_opaque(buffer);
    HRESULT hr = S_FALSE;

    // Direct write into the pooled texture: no landing texture, no D3D11 copy. GPU grain is
    // applied in the same D3D12 dispatch; a bale_gpu that cannot (older, or its grain self-test
    // failed) declines, and a 10-bit frame lands with the D3D11 grain pass instead. 8-bit pictures
    // use bale_zc_repack_into_bits and never land.
    if (eightBit ? api.repackIntoBits != nullptr : api.repackInto != nullptr)
    {
        TimeSpan position{};
        uploadSample = upload->CreateWrittenSample(position, surface, [&](ID3D11Texture2D* texture)
            {
                hr = eightBit ? api.repackIntoBits(handle, picture, &frame, 8, params, texture)
                    : api.repackInto(handle, picture, &frame, params, texture);
                if (hr != S_OK) return false;
                // Once per frame (frame-grabber state), and only for a frame that goes out here.
                ReadFrameProperties(avFrame, framePts);
                position = ConvertPosition(framePts);
                return true;
            });
        if (hr == S_OK)
        {
            timing.EndFrame(3);
            zeroCopyDirectFrames++;
            if (params) zeroCopyDirectGrainFrames++;
            if (eightBit) zeroCopyEightBitFrames++;
            if (++zeroCopyFrames == 240) LogZeroCopyStats(handle, "steady");
            if (!zeroCopyLogged)
            {
                zeroCopyLogged = true;
                av_log(nullptr, AV_LOG_INFO, "Bale zero-copy active (direct write%s): P010 %dx%d.\n", eightBit ? ", 8-bit" : "",
                    outputFrameWidth, outputFrameHeight);
            }
            return S_OK;
        }
        // Once per kind: a declined 10-bit frame lands below.
        auto& declineLogged = params ? zeroCopyGrainDeclineLogged : zeroCopyDeclineLogged;
        if (hr == S_FALSE && !eightBit && !declineLogged)
        {
            declineLogged = true;
            av_log(nullptr, AV_LOG_INFO, "Bale zero-copy direct write declined%s; landing frames instead.\n",
                params ? " with grain" : "");
        }
    }

    // bale_zc_repack lands 10-bit pictures only: a declined (or unavailable) 8-bit write fails.
    if (eightBit && hr == S_FALSE) return ZeroCopyDeclined;
    void* landing = nullptr;
    if (hr == S_FALSE) hr = api.repack(handle, picture, &frame, &landing);
    // The buffer is live (checked above), so a declined landing fails too.
    if (hr == S_FALSE) return ZeroCopyDeclined;
    if (hr == E_INVALIDARG) return refused();
    if (FAILED(hr)) return hr;
    if (hr != S_OK || !landing) return E_UNEXPECTED;
    timing.Add(BaleUploadTiming::Fill, BaleUploadTiming::Now() - repackStart);

    // Borrowed from the handle: hold a reference while MF/D3D11 calls run below.
    winrt::com_ptr<ID3D11Texture2D> texture;
    texture.copy_from(static_cast<ID3D11Texture2D*>(landing));
    ReadFrameProperties(avFrame, framePts);
    uploadSample = upload->CreateLandedSample(texture.get(), ConvertPosition(framePts), surface, FilmGrain, params);
    timing.EndFrame(2);
    if (++zeroCopyFrames == 240) LogZeroCopyStats(handle, "steady");
    if (!zeroCopyLogged)
    {
        zeroCopyLogged = true;
        av_log(nullptr, AV_LOG_INFO, "Bale zero-copy active: P010 %dx%d.\n", outputFrameWidth, outputFrameHeight);
    }
    return S_OK;
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
