#pragma once
#include "pch.h"
#include "MediaSampleProvider.h"
#include "GpuVideoDecoder.h"
#include "GpuDecoderImagePool.h"
#include "TexturePool.h"
#include "VideoFrameProcessor.h"
#include "VideoColorAttributes.h"
#include "DirectXInteropHelper.h"
#include <d3d10.h>
#include <d3d11_4.h>
#include <atomic>
#include <cmath>

// Video decoded on the GPU by a GpuVideoDecoder into textures shared with the renderer's D3D11 device.
// Packets go straight from the demuxer to the decoder (no FFmpeg codec is opened); every frame is
// handed to Media Foundation as a D3D11 texture: the decoder's own texture (direct output), or the
// output of a VideoFrameProcessor that reads it (processed output, e.g. tone mapping or downscaling).
// The media type and size are fixed when the stream is created; a frame that type cannot carry
// fails the stream.
//
// Threads: CreateNextSampleBuffer/SetSampleProperties run on the sample thread (the MediaStreamSource
// request thread, or the media source's decode-ahead worker). The decoder calls AcquireImage on the
// sample thread or its own threads and FrameReady on its completion thread; FrameReady only queues.
// The sample thread waits for each frame's fence on the GPU (ID3D11DeviceContext4::Wait) and runs the
// processor, under the device's multithread protection. Processed returns images on MF's thread.
class GpuDecoderSampleProvider : public MediaSampleProvider, public GpuDecoderHost, public std::enable_shared_from_this<GpuDecoderSampleProvider>
{
public:
    // Output images per stream. The decoder holds up to its pipeline slots + 2; the rest are queued or
    // held by Media Foundation. The pool never grows: the decoder waits for an image instead.
    static constexpr uint32_t OutputImages = 8;

    GpuDecoderSampleProvider(
        std::shared_ptr<FFmpegReader> reader,
        AVFormatContext* avFormatCtx,
        AVCodecContext* avCodecCtx,
        MediaSourceConfig const& config,
        int streamIndex,
        bool applyHdrColorInfo,
        std::unique_ptr<GpuVideoDecoder> gpuDecoder,
        int bitDepth,
        std::unique_ptr<VideoFrameProcessor> processor = nullptr,
        int targetOutputWidth = 0,
        int targetOutputHeight = 0)
        : MediaSampleProvider(reader, avFormatCtx, avCodecCtx, config, streamIndex, HardwareDecoderStatus::Available)
        , gpuDecoder(std::move(gpuDecoder))
        , applyHdrColorInfo(applyHdrColorInfo)
    {
        decoder = DecoderEngine::FFmpegD3D11HardwareDecoder;
        logName = this->gpuDecoder->LogName();
        pool = std::make_shared<GpuDecoderImagePool>(OutputImages, logName);
        decodedFormat = bitDepth > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        sourceWidth = m_pAvStream->codecpar->width;
        sourceHeight = m_pAvStream->codecpar->height;
        pool->SetExpectedImage((sourceWidth + 1) & ~1, (sourceHeight + 1) & ~1, decodedFormat);
        streamColor = StreamColorInfo(m_pAvStream->codecpar);

        frameProcessor = std::move(processor);
        if (frameProcessor && ((sourceWidth & 1) || (sourceHeight & 1) || sourceWidth <= 0 || sourceHeight <= 0 ||
            !frameProcessor->SupportsInputFormat(decodedFormat)))
        {
            av_log(nullptr, AV_LOG_WARNING, "%s: processing declined for %dx%d format=%u\n", logName, sourceWidth, sourceHeight,
                static_cast<unsigned>(decodedFormat));
            frameProcessor = nullptr;
        }
        processedWidth = sourceWidth;
        processedHeight = sourceHeight;
        if (frameProcessor)
        {
            // Processed output keeps 10 bits whenever the processor can write P010.
            outputP010 = frameProcessor->SupportsOutputFormat(DXGI_FORMAT_P010);
            VideoFrameProcessor::SelectOutputSize(*frameProcessor, sourceWidth, sourceHeight, targetOutputWidth, targetOutputHeight,
                processedWidth, processedHeight);
        }
    }

    virtual ~GpuDecoderSampleProvider()
    {
        pool->Shutdown();
        // Hands the frames in flight back (FrameReady/ImageUnused) before the images go away.
        gpuDecoder->Destroy();
        ReturnQueuedFrames();
        ReleaseTrackedSamples();
        ReleasePending();
        uint32_t created = 0, peak = 0, blocked = 0;
        pool->Statistics(created, peak, blocked);
        av_log(nullptr, AV_LOG_INFO, "%s: destroyed (frames=%u discarded=%u images created=%u peak in use=%u blocked=%u output created=%u)\n",
            logName, framesOut, framesDiscarded, created, peak, blocked, processedTexturePool ? processedTexturePool->CreatedCount() : 0u);
        pool->Clear();
    }

    // For the media source's cancel, which runs without its lock (see FFmpegMediaSource::OnStarting).
    std::shared_ptr<GpuDecoderImagePool> OutputPool() const { return pool; }

    HRESULT FatalError() const override { return fatal.load(); }

    // The renderer's device (Media Foundation's D3D11 device). Before the first sample.
    HRESULT SetRendererDevice(winrt::com_ptr<ID3D11Device> newDevice, winrt::com_ptr<ID3D11DeviceContext> newContext)
    {
        auto fatalHr = FatalError();
        if (FAILED(fatalHr)) return fatalHr;
        if (rendererDevice)
        {
            // The decoder's images live on the first device; a new device cannot take them.
            return rendererDevice == newDevice ? S_OK : Fail(E_FAIL, "renderer device changed");
        }
        try
        {
            if (!newDevice || !newContext) winrt::throw_hresult(E_NOINTERFACE);
            auto newMultithread = newDevice.try_as<ID3D10Multithread>();
            if (!newMultithread) newMultithread = newContext.try_as<ID3D10Multithread>();
            if (!newMultithread || (newDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)) winrt::throw_hresult(E_NOINTERFACE);
            // SetMultithreadProtected returns the previous value, not a result.
            newMultithread->SetMultithreadProtected(TRUE);
            if (!newMultithread->GetMultithreadProtected()) winrt::throw_hresult(E_FAIL);
            auto device5 = newDevice.as<ID3D11Device5>();
            auto context4 = newContext.as<ID3D11DeviceContext4>();
            winrt::com_ptr<ID3D11Fence> fence;
            winrt::check_hresult(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), fence.put_void()));
            winrt::check_hresult(gpuDecoder->SetDevice(newDevice.get()));
            auto decoderPtr = gpuDecoder.get();
            pool->SetDevice(newDevice, fence, [decoderPtr](ID3D11Texture2D* texture, winrt::com_ptr<::IUnknown>& view)
                {
                    return decoderPtr->OpenTexture(texture, view);
                });
            rendererDevice = newDevice;
            rendererContext = newContext;
            rendererContext4 = context4;
            multithread = newMultithread;
            releaseFence = fence;
            return S_OK;
        }
        catch (...)
        {
            return Fail(winrt::to_hresult(), "renderer device unusable");
        }
    }

    void DeviceLost(HRESULT hr)
    {
        pool->RequestCancel();
        Fail(hr, "renderer device lost");
    }

    // Called by the media source, with its lock, after a Starting request that began with
    // pool->RequestCancel(): a decode that the cancel failed leaves the decoder without references,
    // so it is drained and waits for a key frame. True when a sample request ended in the cancel.
    bool ResumeAfterCancel()
    {
        if (pool->CancelHits() != handledCancelHits)
        {
            generation++;
            DrainDecoder();
            awaitKeyFrame = true;
        }
        pool->ClearCancel();
        if (cancelledRequest)
        {
            // The cancelled request ended the stream (no sample); a Starting request resumes it.
            cancelledRequest = false;
            if (!IsEnabled()) EnableStream();
            return true;
        }
        return false;
    }

    void Flush(bool flushBuffers) override
    {
        // MediaSampleProvider::Flush without avcodec_flush_buffers: the codec context is never opened.
        if (flushBuffers)
        {
            packetBuffer->Flush();
        }
        m_isDiscontinuous = true;
        IsCleanSample = false;

        // Frames decoded before this point are never sent; decoding resumes at a key frame.
        generation++;
        DrainDecoder();
        ReturnTrackedSamples();
        ReleasePending();
        awaitKeyFrame = true;
        inputEnded = false;
        drained = false;
    }

    HRESULT CreateNextSampleBuffer(IBuffer* pBuffer, int64_t& samplePts, int64_t& sampleDuration, IDirect3DSurface* surface) override
    {
        UNREFERENCED_PARAMETER(pBuffer);
        while (true)
        {
            auto fatalHr = FatalError();
            if (FAILED(fatalHr)) return fatalHr;
            if (pool->CancelRequested()) return CancelRequest();

            GpuDecodedFrame frame;
            if (TakeFrame(frame))
            {
                return OutputFrame(frame, surface, samplePts, sampleDuration);
            }
            if (inputEnded)
            {
                if (drained || !gpuDecoder->IsCreated()) return S_FALSE;
                // Every frame in flight is queued when Drain returns.
                drained = true;
                auto hr = gpuDecoder->Drain();
                if (FAILED(hr)) return pool->CancelRequested() ? CancelRequest() : Fail(hr, "drain failed");
                continue;
            }
            if (!rendererDevice) return Fail(E_NOINTERFACE, "no renderer device");

            AVPacket* packet = nullptr;
            LONGLONG pts = 0;
            LONGLONG duration = 0;
            auto hr = GetNextPacket(&packet, pts, duration);
            if (hr == S_FALSE)
            {
                inputEnded = true;
                continue;
            }
            if (FAILED(hr)) return Fail(hr, "packet read failed");
            hr = FeedPacket(packet, pts, duration);
            av_packet_free(&packet);
            if (hr == E_ABORT) return CancelRequest();
            if (FAILED(hr)) return Fail(hr, "decode failed");
        }
    }

    HRESULT SetSampleProperties(MediaStreamSample const& sample) override
    {
        try
        {
            auto sampleNative = sample.as<winrt::Windows::Foundation::IUnknown>();
            auto token = sample.Processed(weak_handler(this, &GpuDecoderSampleProvider::OnProcessed));
            {
                std::lock_guard lock(samplesMutex);
                try
                {
                    trackedSamples.emplace(sampleNative, TrackedSample{ token, pendingImage, pendingOutput, processedTexturePool });
                }
                catch (...)
                {
                    DetachProcessed(sampleNative, token);
                    throw;
                }
                pendingImage = nullptr;
                pendingOutput = nullptr;
            }

            auto properties = sample.ExtendedProperties();
            properties.Insert(MFSampleExtension_Interlaced, winrt::box_value(FALSE));
            switch (sampleColor.chromaLocation)
            {
            case AVCHROMA_LOC_LEFT:
                properties.Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_MPEG2));
                break;
            case AVCHROMA_LOC_CENTER:
                properties.Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_MPEG1));
                break;
            case AVCHROMA_LOC_TOPLEFT:
                properties.Insert(MF_MT_VIDEO_CHROMA_SITING, winrt::box_value((UINT32)MFVideoChromaSubsampling_Cosited));
                break;
            default:
                break;
            }
            if (!frameProcessor)
            {
                // The frame's own HDR metadata, as the FFmpeg-decoded paths attach it; processed output
                // carries none (the processor consumed it).
                AddSampleHdrMetadata(properties);
            }
            return S_OK;
        }
        catch (...)
        {
            ReleasePending();
            return Fail(winrt::to_hresult(), "sample setup failed");
        }
    }

    winrt::Windows::Media::Core::IMediaStreamDescriptor CreateStreamDescriptor() override
    {
        bool p010 = OutputFormat() == DXGI_FORMAT_P010;
        auto subtype = p010 ? winrt::to_hstring(MFVideoFormat_P010) : MediaEncodingSubtypes::Nv12();
        // Direct output is the decoder's texture: the visible size rounded up to even.
        int frameWidth = frameProcessor ? processedWidth : (sourceWidth + 1) & ~1;
        int frameHeight = frameProcessor ? processedHeight : (sourceHeight + 1) & ~1;
        auto videoProperties = VideoEncodingProperties::CreateUncompressed(subtype, frameWidth, frameHeight);
        auto properties = videoProperties.Properties();

        SetCommonVideoEncodingProperties(videoProperties, false);

        MFVideoArea area{};
        area.Area.cx = frameProcessor ? processedWidth : sourceWidth;
        area.Area.cy = frameProcessor ? processedHeight : sourceHeight;
        properties.Insert(MF_MT_MINIMUM_DISPLAY_APERTURE, winrt::Windows::Foundation::PropertyValue::CreateUInt8Array(winrt::array_view((uint8_t*)&area, sizeof(MFVideoArea))));

        hasHdrMetadata = VideoColorAttributes::Apply(m_pAvStream->codecpar, applyHdrColorInfo, properties);
        isHdrActive = hasHdrMetadata && applyHdrColorInfo;

        if (frameProcessor)
        {
            // The processing contract fixes the output colour space for the lifetime of the stream.
            properties.Insert(MF_MT_VIDEO_PRIMARIES, winrt::Windows::Foundation::PropertyValue::CreateUInt32(MFVideoPrimaries_BT709));
            properties.Insert(MF_MT_TRANSFER_FUNCTION, winrt::Windows::Foundation::PropertyValue::CreateUInt32(MFVideoTransFunc_709));
            properties.Insert(MF_MT_YUV_MATRIX, winrt::Windows::Foundation::PropertyValue::CreateUInt32(MFVideoTransferMatrix_BT709));
            properties.Insert(MF_MT_VIDEO_NOMINAL_RANGE, winrt::Windows::Foundation::PropertyValue::CreateUInt32(MFNominalRange_16_235));
            for (auto key : { MF_MT_MAX_LUMINANCE_LEVEL, MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL,
                MF_MT_MIN_MASTERING_LUMINANCE, MF_MT_MAX_MASTERING_LUMINANCE, MF_MT_CUSTOM_VIDEO_PRIMARIES })
            {
                if (properties.HasKey(key)) properties.Remove(key);
            }
            isHdrActive = false;
            frameProcessor->DescribeOutput(properties);
        }
        else
        {
            gpuDecoder->DescribeOutput(properties);
        }

        properties.Insert(MF_MT_INTERLACE_MODE, winrt::box_value((UINT32)_MFVideoInterlaceMode::MFVideoInterlace_MixedInterlaceOrProgressive));
        av_log(nullptr, AV_LOG_INFO, "%s: output %dx%d %s %s hdr=%d\n", logName, frameWidth, frameHeight, p010 ? "P010" : "NV12",
            frameProcessor ? "processed" : "direct", isHdrActive ? 1 : 0);
        return VideoStreamDescriptor(videoProperties);
    }

    void InitializeStreamInfo() override
    {
        MediaSampleProvider::InitializeStreamInfo();
        auto videoStreamInfoImpl = videoStreamInfo.as<implementation::VideoStreamInfo>();
        videoStreamInfoImpl->SetHasHdrMetadata(hasHdrMetadata);
        videoStreamInfoImpl->SetIsHdrActive(isHdrActive);
    }

    void NotifyCreateSource() override
    {
        if (VideoInfo().FramesPerSecondOverride() > 0 && VideoInfo().FramesPerSecond() > 0)
        {
            timeBaseFactor *= VideoInfo().FramesPerSecond() / VideoInfo().FramesPerSecondOverride();
        }
        MediaSampleProvider::NotifyCreateSource();
    }

    // GpuDecoderHost: called on the decoder's threads.
    bool AcquireImage(UINT width, UINT height, DXGI_FORMAT format, void** decoderView, void** token) noexcept override
    {
        GpuDecoderImagePool::Image* image = nullptr;
        auto hr = pool->Acquire(width, height, format, &image);
        if (hr == E_ABORT) return false;
        if (FAILED(hr))
        {
            Fail(hr, "no output image");
            return false;
        }
        *decoderView = image->decoderView.get();
        *token = image;
        return true;
    }

    void ImageUnused(void* token) noexcept override
    {
        pool->Release(static_cast<GpuDecoderImagePool::Image*>(token), 0);
    }

    void FrameReady(GpuDecodedFrame&& frame) noexcept override
    {
        auto image = frame.image;
        try
        {
            std::lock_guard lock(framesMutex);
            readyFrames.push_back(std::move(frame));
        }
        catch (...)
        {
            pool->Release(static_cast<GpuDecoderImagePool::Image*>(image), 0);
            Fail(E_OUTOFMEMORY, "frame queue");
        }
    }

    int64_t MemoryAvailable() noexcept override
    {
        try
        {
            using winrt::Windows::System::MemoryManager;
            auto limit = MemoryManager::AppMemoryUsageLimit();
            auto usage = MemoryManager::AppMemoryUsage();
            // Output images that do not exist yet are this stream's to allocate, not the decoder's.
            auto pending = pool->PendingBytes();
            int64_t available = limit > usage ? static_cast<int64_t>(limit - usage) : 0;
            available -= static_cast<int64_t>(pending);
            return available > 0 ? available : 0;
        }
        catch (...)
        {
            return -1;
        }
    }

private:
    struct FrameTiming
    {
        uint64_t userData = UINT64_MAX;
        int64_t pts = 0;
        int64_t duration = 0;
        uint64_t generation = 0;
    };

    struct TrackedSample
    {
        winrt::event_token token;
        GpuDecoderImagePool::Image* image = nullptr;       // direct output
        winrt::com_ptr<ID3D11Texture2D> output;           // processed output
        std::weak_ptr<TexturePool> outputPool;
    };

    // The device's multithread protection guards the shared immediate context.
    class ContextLock
    {
    public:
        explicit ContextLock(ID3D10Multithread* multithread) : multithread(multithread) { multithread->Enter(); }
        ~ContextLock() { multithread->Leave(); }
        ContextLock(const ContextLock&) = delete;
        ContextLock& operator=(const ContextLock&) = delete;
    private:
        ID3D10Multithread* multithread;
    };

    static VideoFrameColorInfo StreamColorInfo(const AVCodecParameters* parameters)
    {
        // A frame without colour fields or side data takes the stream's values.
        VideoFrameColorInfo info;
        auto frame = av_frame_alloc();
        if (frame)
        {
            info = VideoFrameColorInfo::FromFrame(frame, parameters);
            av_frame_free(&frame);
        }
        info.chromaLocation = static_cast<uint8_t>(parameters->chroma_location);
        return info;
    }

    // The bitstream's description where it has one, the container's otherwise.
    VideoFrameColorInfo MergeStreamColor(const VideoFrameColorInfo& frame) const
    {
        auto info = frame;
        if (info.transfer == AVCOL_TRC_UNSPECIFIED) info.transfer = streamColor.transfer;
        if (info.primaries == AVCOL_PRI_UNSPECIFIED) info.primaries = streamColor.primaries;
        if (info.matrix == AVCOL_SPC_UNSPECIFIED) info.matrix = streamColor.matrix;
        if (info.range == AVCOL_RANGE_UNSPECIFIED) info.range = streamColor.range;
        if (info.chromaLocation == AVCHROMA_LOC_UNSPECIFIED) info.chromaLocation = streamColor.chromaLocation;
        if (!info.hasMasteringPrimaries && !info.hasMasteringLuminance)
        {
            info.hasMasteringPrimaries = streamColor.hasMasteringPrimaries;
            info.hasMasteringLuminance = streamColor.hasMasteringLuminance;
            std::copy(&streamColor.masteringPrimaries[0][0], &streamColor.masteringPrimaries[0][0] + 6, &info.masteringPrimaries[0][0]);
            info.masteringWhite[0] = streamColor.masteringWhite[0];
            info.masteringWhite[1] = streamColor.masteringWhite[1];
            info.masteringMinNits = streamColor.masteringMinNits;
            info.masteringMaxNits = streamColor.masteringMaxNits;
        }
        if (!info.hasContentLight)
        {
            info.hasContentLight = streamColor.hasContentLight;
            info.maxCLL = streamColor.maxCLL;
            info.maxFALL = streamColor.maxFALL;
        }
        return info;
    }

    DXGI_FORMAT OutputFormat() const
    {
        if (frameProcessor) return outputP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        return decodedFormat;
    }

    HRESULT Fail(HRESULT hr, const char* reason) noexcept
    {
        if (SUCCEEDED(hr) || hr == E_ABORT) hr = E_UNEXPECTED;
        HRESULT expected = S_OK;
        if (fatal.compare_exchange_strong(expected, hr))
        {
            av_log(nullptr, AV_LOG_ERROR, "%s: stream failed (%s, hr=0x%08X)\n", logName, reason, static_cast<unsigned>(hr));
        }
        return fatal.load();
    }

    // A cancel (stop or seek) ended this request; the stream continues after the Starting request.
    HRESULT CancelRequest()
    {
        cancelledRequest = true;
        return E_ABORT;
    }

    HRESULT FeedPacket(AVPacket* packet, int64_t pts, int64_t duration)
    {
        if (awaitKeyFrame && !(packet->flags & AV_PKT_FLAG_KEY))
        {
            // Frames before the first key frame (after open, a seek or a cancel) have no references.
            return S_OK;
        }
        if (!gpuDecoder->IsCreated())
        {
            auto hr = gpuDecoder->Create(packet->data, static_cast<size_t>(packet->size), this);
            if (hr == S_FALSE)
            {
                // No sequence header yet; a stream that never sends one cannot be decoded.
                return ++packetsWithoutSequence > 300 ? E_INVALIDARG : S_OK;
            }
            if (FAILED(hr)) return hr;
        }
        awaitKeyFrame = false;

        auto cancelHits = pool->CancelHits();
        auto userData = nextUserData++;
        timings[userData % timings.size()] = FrameTiming{ userData, pts, duration, generation };
        auto hr = gpuDecoder->Decode(packet->data, static_cast<size_t>(packet->size), userData);
        if (hr == GpuVideoDecoder::ReconfigureRequired)
        {
            // A new sequence the decoder was not created for: its frames in flight are queued by the
            // drain, then a decoder for the new sequence takes the access unit again.
            hr = gpuDecoder->Drain();
            gpuDecoder->Destroy();
            if (SUCCEEDED(hr)) hr = gpuDecoder->Create(packet->data, static_cast<size_t>(packet->size), this);
            if (hr == S_FALSE) hr = E_INVALIDARG;
            if (SUCCEEDED(hr)) hr = gpuDecoder->Decode(packet->data, static_cast<size_t>(packet->size), userData);
            if (hr == GpuVideoDecoder::ReconfigureRequired) hr = E_UNEXPECTED;
        }
        if (FAILED(hr) && (pool->CancelHits() != cancelHits || pool->CancelRequested()))
        {
            awaitKeyFrame = true;
            return E_ABORT;
        }
        return hr;
    }

    bool TakeFrame(GpuDecodedFrame& result)
    {
        std::lock_guard lock(framesMutex);
        while (!readyFrames.empty())
        {
            auto frame = std::move(readyFrames.front());
            readyFrames.pop_front();
            auto& timing = timings[frame.userData % timings.size()];
            if (timing.userData == frame.userData && timing.generation == generation)
            {
                result = std::move(frame);
                return true;
            }
            // A frame of before a seek: back to the pool unsent.
            pool->Release(static_cast<GpuDecoderImagePool::Image*>(frame.image), 0);
            framesDiscarded++;
        }
        return false;
    }

    void ReturnQueuedFrames()
    {
        std::lock_guard lock(framesMutex);
        for (auto& frame : readyFrames)
        {
            pool->Release(static_cast<GpuDecoderImagePool::Image*>(frame.image), 0);
            framesDiscarded++;
        }
        readyFrames.clear();
    }

    // Hands every frame in flight back (they are never sent) and leaves the decoder ready for a key
    // frame. Errors of frames still in parse are part of this, not stream failures: drain until clean.
    void DrainDecoder()
    {
        if (gpuDecoder->IsCreated())
        {
            pool->SetFlushing(true);
            for (int i = 0; i < 8 && gpuDecoder->Drain() != S_OK; i++)
            {
            }
            pool->SetFlushing(false);
        }
        ReturnQueuedFrames();
        handledCancelHits = pool->CancelHits();
    }

    HRESULT OutputFrame(GpuDecodedFrame& frame, IDirect3DSurface* surface, int64_t& framePts, int64_t& frameDuration)
    {
        auto image = static_cast<GpuDecoderImagePool::Image*>(frame.image);
        ReleasePending();
        try
        {
            if (!image || !rendererContext4 || !frame.fence) winrt::throw_hresult(E_UNEXPECTED);
            bool fits = frameProcessor
                ? frameProcessor->SupportsInputFormat(frame.format) && frame.width == static_cast<UINT>(sourceWidth) && frame.height == static_cast<UINT>(sourceHeight)
                : frame.format == decodedFormat && frame.width == static_cast<UINT>(sourceWidth) && frame.height == static_cast<UINT>(sourceHeight);
            if (!fits)
            {
                // The declared media type never changes; this frame cannot be carried.
                av_log(nullptr, AV_LOG_ERROR, "%s: frame %ux%u format=%u does not match the stream's %dx%d format=%u\n", logName,
                    frame.width, frame.height, static_cast<unsigned>(frame.format), sourceWidth, sourceHeight, static_cast<unsigned>(decodedFormat));
                winrt::throw_hresult(E_INVALIDARG);
            }
            auto color = MergeStreamColor(frame.color);

            winrt::com_ptr<ID3D11Texture2D> output;
            if (frameProcessor)
            {
                std::lock_guard lock(samplesMutex);
                if (!processedTexturePool) processedTexturePool = std::make_shared<TexturePool>(rendererDevice, 5);
                winrt::check_hresult(processedTexturePool->GetTexture(processedWidth, processedHeight, OutputFormat(),
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, output));
            }

            uint64_t releaseValue = 0;
            {
                // Never hold samplesMutex while entering the shared immediate context.
                ContextLock contextLock(multithread.get());
                // GPU-side: D3D11 work after this reads the frame only once the decoder has written it.
                winrt::check_hresult(rendererContext4->Wait(frame.fence.get(), frame.fenceValue));
                if (frameProcessor)
                {
                    if (!processorInitialized)
                    {
                        winrt::check_hresult(frameProcessor->Initialize(rendererDevice.get(), processedWidth, processedHeight, sourceWidth, sourceHeight));
                        processorInitialized = true;
                    }
                    winrt::check_hresult(frameProcessor->Process(rendererContext.get(), image->texture.get(), 0, output.get(), color));
                    releaseValue = ++releaseFenceValue;
                    winrt::check_hresult(rendererContext4->Signal(releaseFence.get(), releaseValue));
                }
                rendererContext->Flush();
                auto removed = rendererDevice->GetDeviceRemovedReason();
                if (FAILED(removed))
                {
                    av_log(nullptr, AV_LOG_ERROR, "%s: device removed (hr=0x%08X)\n", logName, static_cast<unsigned>(removed));
                    winrt::throw_hresult(removed);
                }
            }

            auto& timing = timings[frame.userData % timings.size()];
            framePts = timing.pts;
            frameDuration = timing.duration;
            sampleColor = frameProcessor ? color : frame.color;
            sampleColor.chromaLocation = color.chromaLocation;
            if (frameProcessor)
            {
                // The processor has read the image (fenced); the sample carries the processed texture.
                pool->Release(image, releaseValue);
                image = nullptr;
                pendingOutput = output;
                *surface = DirectXInteropHelper::GetSurface(output.as<IDXGISurface>());
            }
            else
            {
                pendingImage = image;
                image = nullptr;
                *surface = DirectXInteropHelper::GetSurface(pendingImage->texture.as<IDXGISurface>());
            }
            framesOut++;
            return S_OK;
        }
        catch (...)
        {
            auto hr = winrt::to_hresult();
            if (image) pool->Release(image, 0);
            ReleasePending();
            if (frameProcessor) frameProcessor->NotifyError(hr);
            return Fail(hr, "frame output failed");
        }
    }

    void AddSampleHdrMetadata(MediaStreamSamplePropertySet const& properties) const
    {
        if (sampleColor.hasContentLight)
        {
            properties.Insert(MF_MT_MAX_LUMINANCE_LEVEL, winrt::Windows::Foundation::PropertyValue::CreateUInt32(sampleColor.maxCLL));
            properties.Insert(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL, winrt::Windows::Foundation::PropertyValue::CreateUInt32(sampleColor.maxFALL));
        }
        if (sampleColor.hasMasteringLuminance)
        {
            constexpr double MASTERING_DISP_LUMINANCE_SCALE{ 10000 };
            properties.Insert(MF_MT_MIN_MASTERING_LUMINANCE, winrt::Windows::Foundation::PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(MASTERING_DISP_LUMINANCE_SCALE * sampleColor.masteringMinNits))));
            properties.Insert(MF_MT_MAX_MASTERING_LUMINANCE, winrt::Windows::Foundation::PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(sampleColor.masteringMaxNits))));
        }
        if (sampleColor.hasMasteringPrimaries)
        {
            MT_CUSTOM_VIDEO_PRIMARIES primaries
            {
                sampleColor.masteringPrimaries[0][0], sampleColor.masteringPrimaries[0][1],
                sampleColor.masteringPrimaries[1][0], sampleColor.masteringPrimaries[1][1],
                sampleColor.masteringPrimaries[2][0], sampleColor.masteringPrimaries[2][1],
                sampleColor.masteringWhite[0], sampleColor.masteringWhite[1]
            };
            auto data = winrt::array_view<UINT8>(reinterpret_cast<UINT8*>(&primaries), sizeof(primaries));
            properties.Insert(MF_MT_CUSTOM_VIDEO_PRIMARIES, winrt::Windows::Foundation::PropertyValue::CreateUInt8Array(data));
        }
    }

    // After the last D3D11 use of a direct-output image: a fence value the decoder's next write waits for.
    uint64_t SignalRelease() noexcept
    {
        try
        {
            if (!multithread || !rendererContext4 || !releaseFence) return 0;
            ContextLock contextLock(multithread.get());
            auto value = ++releaseFenceValue;
            if (FAILED(rendererContext4->Signal(releaseFence.get(), value))) return 0;
            rendererContext->Flush();
            return value;
        }
        catch (...)
        {
            return 0;
        }
    }

    void ReturnTracked(TrackedSample& tracked)
    {
        if (tracked.image)
        {
            pool->Release(tracked.image, SignalRelease());
            tracked.image = nullptr;
        }
        if (tracked.output)
        {
            std::lock_guard lock(samplesMutex);
            // A replacement pool must never receive textures of an earlier one.
            auto outputPool = tracked.outputPool.lock();
            if (outputPool && outputPool == processedTexturePool) outputPool->ReturnTexture(tracked.output);
            tracked.output = nullptr;
        }
    }

    void OnProcessed(MediaStreamSample const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        try
        {
            TrackedSample tracked;
            {
                std::lock_guard lock(samplesMutex);
                auto sampleNative = sender.as<winrt::Windows::Foundation::IUnknown>();
                auto entry = trackedSamples.find(sampleNative);
                if (entry == trackedSamples.end()) return; // Already returned by Flush.
                tracked = std::move(entry->second);
                trackedSamples.erase(entry);
                DetachProcessed(sampleNative, tracked.token);
            }
            ReturnTracked(tracked);
        }
        catch (...)
        {
            // Never throw out of a Media Foundation event handler.
            DebugMessage(L"Failed to recycle a GPU decoder texture.\n");
        }
    }

    void DetachProcessed(winrt::Windows::Foundation::IUnknown const& sample, winrt::event_token token) noexcept
    {
        try
        {
            sample.as<MediaStreamSample>().Processed(token);
        }
        catch (...)
        {
            DebugMessage(L"Failed to detach a processed video sample.\n");
        }
    }

    // Takes every texture back from samples Media Foundation still has, as the D3D11 provider does on
    // a flush (samples dropped before delivery never raise Processed).
    void ReturnTrackedSamples()
    {
        std::vector<TrackedSample> returned;
        {
            std::lock_guard lock(samplesMutex);
            for (auto& entry : trackedSamples)
            {
                DetachProcessed(entry.first, entry.second.token);
                returned.push_back(std::move(entry.second));
            }
            trackedSamples.clear();
        }
        for (auto& tracked : returned)
        {
            ReturnTracked(tracked);
        }
    }

    void ReleaseTrackedSamples()
    {
        std::lock_guard lock(samplesMutex);
        for (auto& entry : trackedSamples)
        {
            DetachProcessed(entry.first, entry.second.token);
        }
        trackedSamples.clear();
    }

    // Output of CreateNextSampleBuffer that never reached SetSampleProperties.
    void ReleasePending()
    {
        if (pendingImage)
        {
            pool->Release(pendingImage, SignalRelease());
            pendingImage = nullptr;
        }
        pendingOutput = nullptr;
    }

    std::unique_ptr<GpuVideoDecoder> gpuDecoder;
    const char* logName = "";
    std::shared_ptr<GpuDecoderImagePool> pool;
    std::unique_ptr<VideoFrameProcessor> frameProcessor;
    std::shared_ptr<TexturePool> processedTexturePool;
    bool processorInitialized = false;
    bool outputP010 = false;
    bool applyHdrColorInfo = false;
    bool hasHdrMetadata = false;
    bool isHdrActive = false;
    DXGI_FORMAT decodedFormat = DXGI_FORMAT_NV12;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int processedWidth = 0;
    int processedHeight = 0;
    VideoFrameColorInfo streamColor;
    VideoFrameColorInfo sampleColor;

    winrt::com_ptr<ID3D11Device> rendererDevice;
    winrt::com_ptr<ID3D11DeviceContext> rendererContext;
    winrt::com_ptr<ID3D11DeviceContext4> rendererContext4;
    winrt::com_ptr<ID3D10Multithread> multithread;
    winrt::com_ptr<ID3D11Fence> releaseFence;
    uint64_t releaseFenceValue = 0; // Under the multithread lock.

    std::atomic<HRESULT> fatal{ S_OK };
    // Sample thread state (also changed by Flush/ResumeAfterCancel while the sample thread is idle).
    std::array<FrameTiming, 64> timings{};
    uint64_t nextUserData = 0;
    uint64_t generation = 0;
    uint32_t handledCancelHits = 0;
    uint32_t packetsWithoutSequence = 0;
    bool awaitKeyFrame = true;
    bool inputEnded = false;
    bool drained = false;
    bool cancelledRequest = false;
    unsigned framesOut = 0;
    unsigned framesDiscarded = 0;
    GpuDecoderImagePool::Image* pendingImage = nullptr;
    winrt::com_ptr<ID3D11Texture2D> pendingOutput;

    std::mutex framesMutex;
    std::deque<GpuDecodedFrame> readyFrames;

    std::mutex samplesMutex;
    std::map<winrt::Windows::Foundation::IUnknown, TrackedSample> trackedSamples;
};
