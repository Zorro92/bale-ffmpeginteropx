#pragma once
#include "pch.h"
#include "MediaSampleProvider.h"
#include "StreamBuffer.h"
#include "GpuVideoDecoder.h"
#include "GpuDecoderImagePool.h"
#include "TexturePool.h"
#include "BaleMemoryLedger.h"
#include "VideoFrameProcessor.h"
#include "VideoColorAttributes.h"
#include "DirectXInteropHelper.h"
#include <d3d10.h>
#include <d3d11_4.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include <cmath>

// Video decoded on the GPU by a GpuVideoDecoder into textures shared with the renderer's D3D11 device.
// Packets go straight from the demuxer to the decoder (no FFmpeg codec is opened); every frame is
// handed to Media Foundation as a D3D11 texture: the decoder's own texture (direct output), or the
// output of a VideoFrameProcessor that reads it (processed output, e.g. tone mapping or downscaling).
// The media type and size are fixed when the stream is created; a frame that type cannot carry
// fails the stream.
//
// Threads: the provider's feeder thread reads packets and calls the decoder (Create, Decode, Drain at
// the end of the stream) so that a target number of access units stays in flight (fed and not yet
// delivered). A decoder that parses on the calling thread then parses the next units while the GPU
// decodes earlier ones and the sample thread delivers. CreateNextSampleBuffer/SetSampleProperties
// run on the sample thread (the MediaStreamSource request thread, or the media source's decode-ahead
// worker): it waits for a decoded frame, waits for its fence on the GPU (ID3D11DeviceContext4::Wait)
// and copies or processes it under the device's multithread protection. The decoder calls
// AcquireImage on the feeder thread or its own threads and FrameReady on its completion thread;
// FrameReady only queues. Processed returns textures on MF's thread.
//
// The feeder shares the demuxer with the media source's threads (FFmpegReader serialises packet
// reads), and is paused (PauseFeeder/ResumeFeeder) whenever the media source pauses its decode-ahead
// worker: around seeks, flushes, stream switches and Starting requests. Flush, ResumeAfterCancel
// and the stream's enable state change only while it is paused or idle. Locks: framesMutex guards
// the feeder state, the frame queue and the timing table; it is never held while calling the
// decoder, and it may be held while taking the image pool's lock (never the reverse). The decoder is
// never called with samplesMutex or the device's multithread protection held.
class GpuDecoderSampleProvider : public MediaSampleProvider, public GpuDecoderHost, public std::enable_shared_from_this<GpuDecoderSampleProvider>
{
public:
    // Output images per stream, created on demand. The decoder holds up to its pipeline slots + 2 (6 for
    // rav1g's 4 slots above 2560x1440); the rest wait in the ready queue or are being copied, and go
    // back right after. The pool never grows past this: the decoder waits for an image instead.
    static constexpr uint32_t OutputImages = 10;
    // Access units in flight (fed to the decoder and not yet delivered) unless SetDecodeInflight says
    // otherwise. Each needs an output image, so the target stays below OutputImages.
    // 4: smooth 1080p 23.976 on a Series X (3 left little margin, 2 fell short).
    static constexpr uint32_t DefaultInflight = 4;

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
        int targetOutputHeight = 0,
        int outputDepth = 0)
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
            // Processed output keeps 10 bits whenever the processor can write P010, unless 8-bit output
            // was requested and the processor dithers NV12 (see SelectOutputFormat).
            outputP010 = VideoFrameProcessor::SelectOutputFormat(*frameProcessor, outputDepth) == DXGI_FORMAT_P010;
            VideoFrameProcessor::SelectOutputSize(*frameProcessor, sourceWidth, sourceHeight, targetOutputWidth, targetOutputHeight,
                processedWidth, processedHeight);
        }
    }

    virtual ~GpuDecoderSampleProvider()
    {
        StopFeeder();
        pool->Shutdown();
        // Hands the frames in flight back (FrameReady/ImageUnused) before the images go away.
        gpuDecoder->Destroy();
        ReturnQueuedFrames();
        ReleaseTrackedSamples();
        ReleasePending();
        LogDeliveryStats();
        uint32_t created = 0, peak = 0, blocked = 0;
        pool->Statistics(created, peak, blocked);
        av_log(nullptr, AV_LOG_INFO, "%s: destroyed (frames=%u discarded=%u images created=%u peak in use=%u blocked=%u output created=%u)\n",
            logName, framesOut, framesDiscarded, created, peak, blocked, processedTexturePool ? processedTexturePool->CreatedCount() : 0u);
        LogMemory("destroy");
        pool->Clear();
    }

    // For the media source's cancel, which runs without its lock (see FFmpegMediaSource::OnStarting).
    std::shared_ptr<GpuDecoderImagePool> OutputPool() const { return pool; }

    HRESULT FatalError() const override { return fatal.load(); }

    // Units the feeder keeps in flight (1 to OutputImages - 1; larger values are capped). Before the
    // first sample.
    void SetDecodeInflight(uint32_t units)
    {
        std::lock_guard lock(framesMutex);
        inflightTarget = (std::max)(1u, (std::min)(units, OutputImages - 1));
    }

    // At most this many player-side textures exist at once (0 = no limit, the default). When all of them
    // are with the player, the next frame waits up to OutputTextureWaitMs for one to come back and
    // only then grows the pool, so a limit below what the player holds costs a short wait, never a
    // stall. Values below 4 act as 4. Before the first sample.
    void SetOutputTextureLimit(uint32_t textures)
    {
        std::lock_guard lock(samplesMutex);
        outputTextureLimit = textures == 0 ? 0 : (std::max)(4u, textures);
    }

    // Win32 priority of the feeder thread. Before the first sample.
    void SetFeederThreadPriority(int priority)
    {
        feederPriority = priority;
    }

    // Stops the feeder between two steps (it may finish the packet read or decode call it is in) and
    // keeps it stopped until the matching ResumeFeeder. Nests. Never called on the feeder thread.
    void PauseFeeder()
    {
        std::unique_lock lock(framesMutex);
        feederPauses++;
        framesChanged.wait(lock, [this] { return !feederBusy; });
    }

    void ResumeFeeder()
    {
        {
            std::lock_guard lock(framesMutex);
            if (feederPauses > 0) feederPauses--;
        }
        framesChanged.notify_all();
    }

    // Ends the feeder for good (the media source closes, or the provider is destroyed): fails waits
    // for output images and joins the thread. The demuxer must still be open.
    void StopFeeder()
    {
        {
            std::lock_guard lock(framesMutex);
            feederStop = true;
        }
        framesChanged.notify_all();
        pool->Shutdown();
        if (feeder.joinable()) feeder.join();
    }

    void EnableStream() override
    {
        MediaSampleProvider::EnableStream();
        SetFeedEnabled(true);
    }

    void DisableStream() override
    {
        // A disabled stream gets no packets: the feeder must not wait for one.
        MediaSampleProvider::DisableStream();
        SetFeedEnabled(false);
    }

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

    // Called by the media source, with its lock and the feeder paused, after a Starting request that
    // began with pool->RequestCancel(): a decode that the cancel failed leaves the decoder without
    // references, so it is drained and waits for a key frame. True when a sample request ended in
    // the cancel.
    bool ResumeAfterCancel()
    {
        FeederPause pause(this);
        if (pool->CancelHits() != handledCancelHits)
        {
            {
                std::lock_guard lock(framesMutex);
                generation++;
            }
            DrainDecoder();
            awaitKeyFrame = true;
        }
        pool->ClearCancel();
        {
            std::lock_guard lock(framesMutex);
            feedCancelled = false;
        }
        framesChanged.notify_all();
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

        // Frames decoded before this point are never sent; decoding resumes at a key frame. The media
        // source has paused the feeder already (this pause only nests); it resumes after the flush.
        FeederPause pause(this);
        {
            std::lock_guard lock(framesMutex);
            lastDecodeEnd = {};
            generation++;
            feedEnded = false;
            feedCancelled = false;
        }
        LogDeliveryStats();
        stats = DeliveryStats{};
        listedSinceStart = 0;
        listedPts.clear();
        DrainDecoder();
        ReturnTrackedSamples();
        ReleasePending();
        awaitKeyFrame = true;
        inputEnded = false;
    }

    HRESULT CreateNextSampleBuffer(IBuffer* pBuffer, int64_t& samplePts, int64_t& sampleDuration, IDirect3DSurface* surface) override
    {
        UNREFERENCED_PARAMETER(pBuffer);
        auto fatalHr = FatalError();
        if (FAILED(fatalHr)) return fatalHr;
        if (!rendererDevice) return Fail(E_NOINTERFACE, "no renderer device");

        GpuDecodedFrame frame;
        FrameTiming timing;
        uint32_t inFlight = 0;
        size_t queued = 0;
        double waitMs = 0;
        {
            std::unique_lock lock(framesMutex);
            if (!feeder.joinable() && !feederStop)
            {
                try
                {
                    feeder = std::thread([this] { FeedLoop(); });
                }
                catch (...)
                {
                    return Fail(E_OUTOFMEMORY, "feeder thread");
                }
            }
            std::chrono::steady_clock::time_point waitStart{};
            while (true)
            {
                fatalHr = FatalError();
                if (FAILED(fatalHr)) return fatalHr;
                queued = readyFrames.size();
                if (TakeFrame(frame, timing)) break;
                // Every frame in flight is queued when the feeder's final drain returns.
                if (feedEnded) return S_FALSE;
                if (pool->CancelRequested()) return CancelRequest();
                if (waitStart == std::chrono::steady_clock::time_point{}) waitStart = std::chrono::steady_clock::now();
                // Woken when a frame is queued or the feeder stops; the timeout re-checks for a cancel,
                // which another thread requests without this lock.
                framesChanged.wait_for(lock, CancelCheckInterval);
            }
            if (waitStart != std::chrono::steady_clock::time_point{})
            {
                waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
            }
            outputting = true;
            inFlight = InFlight();
        }

        auto outputStart = std::chrono::steady_clock::now();
        auto hr = OutputFrame(frame, timing, surface, samplePts, sampleDuration);
        auto outputMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - outputStart).count();
        {
            std::lock_guard lock(framesMutex);
            outputting = false;
        }
        // The feeder may feed again.
        framesChanged.notify_all();
        if (SUCCEEDED(hr)) RecordDelivery(timing, inFlight, queued, waitMs, outputMs);
        return hr;
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
            if (unitsInDecoder > 0) unitsInDecoder--;
        }
        catch (...)
        {
            pool->Release(static_cast<GpuDecoderImagePool::Image*>(image), 0);
            Fail(E_OUTOFMEMORY, "frame queue");
        }
        framesChanged.notify_all();
    }

    int64_t MemoryAvailable() noexcept override
    {
        try
        {
            using winrt::Windows::System::MemoryManager;
            auto limit = MemoryManager::AppMemoryUsageLimit();
            auto usage = MemoryManager::AppMemoryUsage();
            // Textures that do not exist yet are this stream's to allocate, not the decoder's: the
            // decoder's own image pool, and the player-side pool its output is copied or processed
            // into (sized for the player's usual buffer; it can grow beyond it).
            auto pending = pool->PendingBytes() + PendingPlayerPoolBytes();
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
    // Player-side textures expected in use at once (the player's buffer and renderer queue).
    static constexpr uint32_t ExpectedPlayerTextures = 8;
    std::atomic<uint32_t> playerTexturesCreated{ 0 };

    uint64_t PlayerTextureBytes() noexcept
    {
        uint64_t width = frameProcessor ? static_cast<uint64_t>(processedWidth) : static_cast<uint64_t>((sourceWidth + 1) & ~1);
        uint64_t height = frameProcessor ? static_cast<uint64_t>(processedHeight) : static_cast<uint64_t>((sourceHeight + 1) & ~1);
        // P010: 2 bytes per luma sample plus half as many chroma bytes = 3 bytes a pixel; NV12: 1.5.
        return OutputFormat() == DXGI_FORMAT_P010 ? width * height * 3 : width * height * 3 / 2;
    }

    // Where this stream's GPU memory goes: the decoder's own buffers, its output images and the
    // player-side textures handed to Media Foundation (direct copies or stage output). Not counted:
    // the process-wide decoder device and the stage's internal resources; app is the whole process.
    void LogMemory(const char* when) noexcept
    {
        try
        {
            uint32_t images = 0;
            uint64_t imageBytes = 0;
            pool->Footprint(images, imageBytes);
            uint32_t player = 0, limit = 0, overruns = 0;
            {
                std::lock_guard lock(samplesMutex);
                // Textures that exist now: with the player plus kept for reuse.
                player = OutputTexturesHeld() + (processedTexturePool ? static_cast<uint32_t>(processedTexturePool->PooledTextureCount()) : 0u);
                limit = outputTextureLimit;
                overruns = outputTextureOverruns;
            }
            uint64_t playerBytes = player * PlayerTextureBytes();
            uint64_t decoderBytes = gpuDecoder ? gpuDecoder->DeviceBytes() : 0;
            uint64_t app = 0;
            try { app = winrt::Windows::System::MemoryManager::AppMemoryUsage(); } catch (...) {}
            constexpr double MiB = 1024.0 * 1024.0;
            av_log(nullptr, AV_LOG_INFO,
                "%s: memory (%s) decoder=%.1f MiB images=%u x %.1f = %.1f MiB player=%u x %.1f = %.1f MiB (limit=%u overruns=%u) stream total=%.1f MiB app=%.1f MiB\n",
                logName, when, decoderBytes / MiB, images, images ? imageBytes / MiB / images : 0.0, imageBytes / MiB,
                player, PlayerTextureBytes() / MiB, playerBytes / MiB, limit, overruns, (decoderBytes + imageBytes + playerBytes) / MiB, app / MiB);
        }
        catch (...)
        {
        }
    }

    uint64_t PendingPlayerPoolBytes() noexcept
    {
        uint64_t bytesPerTexture = PlayerTextureBytes();
        // Called on decoder threads: read the count without the samples lock.
        uint32_t created = playerTexturesCreated.load();
        return created >= ExpectedPlayerTextures ? 0 : (ExpectedPlayerTextures - created) * bytesPerTexture;
    }

    struct FrameTiming
    {
        uint64_t userData = UINT64_MAX;
        int64_t pts = 0;
        int64_t duration = 0;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point decodeStart{};
    };

    // Delivery diagnostics (Bale reads them from the log): decode-to-deliver latency and the
    // timestamp steps between delivered samples, summarised every StatsInterval samples; the first
    // samples after an open or seek are also listed one by one.
    static constexpr uint32_t StatsInterval = 240;
    static constexpr uint32_t ListedSamples = 24;
    // How often a sample thread waiting for a frame checks for a cancel requested without framesMutex.
    static constexpr std::chrono::milliseconds CancelCheckInterval{ 20 };
    // Sample thread.
    struct DeliveryStats
    {
        uint32_t samples = 0;
        double latencySumMs = 0;
        double latencyMaxMs = 0;
        int64_t lastPts = INT64_MIN;
        int64_t deltaMin = INT64_MAX;
        int64_t deltaMax = INT64_MIN;
        uint32_t backwards = 0;
        uint32_t gaps = 0;
        size_t queueMax = 0;
        std::chrono::steady_clock::time_point windowStart{};
        // Units in flight when a frame was taken for delivery (it included).
        uint64_t inFlightSum = 0;
        uint32_t inFlightMax = 0;
        // Time the sample thread waited for a decoded frame (0 when one was queued).
        double waitSumMs = 0;
        double waitMaxMs = 0;
        // OutputFrame: the GPU fence wait, the copy or processing, the release signal and the flush.
        double outputSumMs = 0;
        double outputMaxMs = 0;
    };
    // Time spent inside the decoder's decode call, on the feeder thread (with one frame in parse it
    // includes the CPU parse of the unit). Under framesMutex.
    struct DecodeCallStats
    {
        uint32_t calls = 0;
        double sumMs = 0;
        double maxMs = 0;
        // Feeder thread, between the end of one decode call and the start of the next: the packet read
        // (demux, on the same thread), then any wait for the units in flight to drop below the target.
        uint32_t reads = 0;
        double readSumMs = 0;
        double readMaxMs = 0;
        uint32_t gaps = 0;
        double gapSumMs = 0;
        double gapMaxMs = 0;
    };
    DeliveryStats stats;
    DecodeCallStats decodeStats;
    // End of the last decode call (framesMutex); cleared by Flush so a seek's pause isn't a gap.
    std::chrono::steady_clock::time_point lastDecodeEnd{};
    uint32_t windowBlocked = 0; // The pool's blocked count at the start of the window.
    uint32_t listedSinceStart = 0;
    std::string listedPts;

    void RecordDelivery(const FrameTiming& timing, uint32_t inFlight, size_t queued, double waitMs, double outputMs) noexcept
    {
        try
        {
            auto now = std::chrono::steady_clock::now();
            auto latencyMs = std::chrono::duration<double, std::milli>(now - timing.decodeStart).count();
            if (stats.samples == 0) stats.windowStart = now;
            // In sample time (100 ns), as the player receives it: the packets carry stream time-base
            // units, which GetNextSample converts.
            int64_t samplePts = ConvertPosition(timing.pts).count();
            int64_t sampleDuration = ConvertDuration(timing.duration).count();
            stats.samples++;
            stats.latencySumMs += latencyMs;
            stats.latencyMaxMs = (std::max)(stats.latencyMaxMs, latencyMs);
            if (stats.lastPts != INT64_MIN)
            {
                auto delta = samplePts - stats.lastPts;
                if (delta < 0) stats.backwards++;
                // A gap: more than 1.5 sample durations (or 1.5x the smallest step seen when the
                // container gives no duration).
                auto expected = sampleDuration > 0 ? sampleDuration : (stats.deltaMin != INT64_MAX ? stats.deltaMin : 0);
                if (expected > 0 && delta > expected * 3 / 2) stats.gaps++;
                stats.deltaMin = (std::min)(stats.deltaMin, delta);
                stats.deltaMax = (std::max)(stats.deltaMax, delta);
            }
            stats.lastPts = samplePts;
            stats.queueMax = (std::max)(stats.queueMax, queued);
            stats.inFlightSum += inFlight;
            stats.inFlightMax = (std::max)(stats.inFlightMax, inFlight);
            stats.waitSumMs += waitMs;
            stats.waitMaxMs = (std::max)(stats.waitMaxMs, waitMs);
            stats.outputSumMs += outputMs;
            stats.outputMaxMs = (std::max)(stats.outputMaxMs, outputMs);
            if (listedSinceStart < ListedSamples)
            {
                listedSinceStart++;
                listedPts += std::to_string(samplePts / 10000);
                listedPts += listedSinceStart < ListedSamples ? "," : "";
                if (listedSinceStart == ListedSamples)
                {
                    av_log(nullptr, AV_LOG_INFO, "%s: first samples pts ms=%s\n", logName, listedPts.c_str());
                    listedPts.clear();
                }
            }
            if (stats.samples >= StatsInterval) LogDeliveryStats();
        }
        catch (...)
        {
        }
    }

    // Sample thread, or a thread that runs while the sample thread is idle (Flush, the destructor).
    void LogDeliveryStats() noexcept
    {
        DecodeCallStats decode;
        uint32_t target = 0;
        {
            std::lock_guard lock(framesMutex);
            decode = decodeStats;
            decodeStats = DecodeCallStats{};
            target = inflightTarget;
        }
        uint32_t created = 0, peak = 0, blocked = 0;
        pool->Statistics(created, peak, blocked);
        auto blockedDelta = blocked - windowBlocked;
        windowBlocked = blocked;
        uint32_t releaseWaits = 0;
        double releaseWaitSumMs = 0, releaseWaitMaxMs = 0;
        pool->TakeReleaseWaits(releaseWaits, releaseWaitSumMs, releaseWaitMaxMs);
        if (stats.samples == 0) return;
        std::string decoderText;
        try
        {
            gpuDecoder->AppendStatistics(decoderText);
        }
        catch (...)
        {
            decoderText.clear();
        }
        auto elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - stats.windowStart).count();
        av_log(nullptr, AV_LOG_INFO,
            "%s: delivery samples=%u rate=%.2f/s latency avg=%.1f max=%.1f ms decode call avg=%.1f max=%.1f ms read avg=%.1f max=%.1f ms gap avg=%.1f max=%.1f ms pts step min=%.1f max=%.1f ms backwards=%u gaps=%u queue max=%u"
            " inflight avg=%.1f max=%u target=%u wait avg=%.1f max=%.1f ms output avg=%.1f max=%.1f ms pool blocked=%u"
            " image release wait n=%u avg=%.1f max=%.1f ms%s\n",
            logName, stats.samples, elapsedS > 0 ? stats.samples / elapsedS : 0.0, stats.latencySumMs / stats.samples, stats.latencyMaxMs,
            decode.calls ? decode.sumMs / decode.calls : 0.0, decode.maxMs,
            decode.reads ? decode.readSumMs / decode.reads : 0.0, decode.readMaxMs,
            decode.gaps ? decode.gapSumMs / decode.gaps : 0.0, decode.gapMaxMs,
            stats.deltaMin == INT64_MAX ? 0.0 : stats.deltaMin / 10000.0, stats.deltaMax == INT64_MIN ? 0.0 : stats.deltaMax / 10000.0,
            stats.backwards, stats.gaps, static_cast<unsigned>(stats.queueMax),
            static_cast<double>(stats.inFlightSum) / stats.samples, stats.inFlightMax, target,
            stats.waitSumMs / stats.samples, stats.waitMaxMs, stats.outputSumMs / stats.samples, stats.outputMaxMs,
            blockedDelta, releaseWaits, releaseWaits ? releaseWaitSumMs / releaseWaits : 0.0, releaseWaitMaxMs, decoderText.c_str());
        LogMemory("delivery");
        auto lastPts = stats.lastPts;
        stats = DeliveryStats{};
        stats.lastPts = lastPts;
    }

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
        // A waiting sample thread returns the failure (its wait times out if this wake-up is lost).
        framesChanged.notify_all();
        return fatal.load();
    }

    // A cancel (stop or seek) ended this request; the stream continues after the Starting request.
    HRESULT CancelRequest()
    {
        cancelledRequest = true;
        return E_ABORT;
    }

    // Units in flight: in the decoder, decoded and queued, and the one being delivered. framesMutex.
    uint32_t InFlight() const
    {
        return unitsInDecoder + static_cast<uint32_t>(readyFrames.size()) + (outputting ? 1u : 0u);
    }

    // framesMutex held.
    bool CanFeed() const
    {
        return feedEnabled && !feedEnded && !feedCancelled && SUCCEEDED(fatal.load()) && InFlight() < inflightTarget;
    }

    void SetFeedEnabled(bool enabled)
    {
        {
            std::lock_guard lock(framesMutex);
            feedEnabled = enabled;
        }
        framesChanged.notify_all();
    }

    struct FeederPause
    {
        explicit FeederPause(GpuDecoderSampleProvider* owner) : owner(owner) { owner->PauseFeeder(); }
        ~FeederPause() { owner->ResumeFeeder(); }
        FeederPause(const FeederPause&) = delete;
        FeederPause& operator=(const FeederPause&) = delete;
        GpuDecoderSampleProvider* owner;
    };

    // The feeder thread: one step (a packet, or the final drain) at a time while CanFeed.
    void FeedLoop()
    {
        if (feederPriority != THREAD_PRIORITY_NORMAL)
        {
            SetThreadPriority(GetCurrentThread(), feederPriority);
        }
        bool apartment = false;
        try
        {
            // The decoder's memory callback reads Windows.System.MemoryManager on this thread.
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
        }
        catch (...)
        {
        }
        {
            std::unique_lock lock(framesMutex);
            while (true)
            {
                feederBusy = false;
                // PauseFeeder and a waiting sample thread (end of stream, failure) look again.
                framesChanged.notify_all();
                framesChanged.wait(lock, [this] { return feederStop || (feederPauses == 0 && CanFeed()); });
                if (feederStop) break;
                feederBusy = true;
                lock.unlock();
                FeedStep();
                lock.lock();
            }
        }
        if (apartment)
        {
            winrt::uninit_apartment();
        }
    }

    // Feeder thread, no lock held. A cancelled step stops the feeder until ResumeAfterCancel or Flush;
    // a failed one fails the stream.
    void FeedStep() noexcept
    {
        HRESULT hr = S_OK;
        const char* failure = "decode failed";
        try
        {
            if (inputEnded)
            {
                // Every frame in flight is queued when Drain returns.
                failure = "drain failed";
                hr = gpuDecoder->IsCreated() ? gpuDecoder->Drain() : S_OK;
                if (SUCCEEDED(hr))
                {
                    std::lock_guard lock(framesMutex);
                    unitsInDecoder = 0;
                    feedEnded = true;
                    return;
                }
                if (pool->CancelRequested()) hr = E_ABORT;
            }
            else
            {
                AVPacket* packet = nullptr;
                LONGLONG pts = 0;
                LONGLONG duration = 0;
                // Interruptible: a seek or stop disables the stream (whose packets are then dropped) or
                // pauses the feeder, and must not wait for a read that would run to the end of the input.
                auto readStart = std::chrono::steady_clock::now();
                hr = GetNextPacket(&packet, pts, duration, [this]
                {
                    std::lock_guard lock(framesMutex);
                    return feederStop || feederPauses > 0 || !feedEnabled;
                });
                if (hr == S_OK)
                {
                    auto readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
                    std::lock_guard lock(framesMutex);
                    decodeStats.reads++;
                    decodeStats.readSumMs += readMs;
                    decodeStats.readMaxMs = (std::max)(decodeStats.readMaxMs, readMs);
                }
                if (hr == E_PENDING)
                {
                    // Nothing was taken; the feeder waits until it may feed again.
                    return;
                }
                if (hr == S_FALSE)
                {
                    inputEnded = true;
                    return;
                }
                if (FAILED(hr))
                {
                    failure = "packet read failed";
                }
                else
                {
                    try
                    {
                        hr = FeedPacket(packet, pts, duration);
                    }
                    catch (...)
                    {
                        hr = winrt::to_hresult();
                    }
                    av_packet_free(&packet);
                }
            }
        }
        catch (...)
        {
            hr = winrt::to_hresult();
        }
        if (hr == E_ABORT)
        {
            std::lock_guard lock(framesMutex);
            feedCancelled = true;
        }
        else if (FAILED(hr))
        {
            Fail(hr, failure);
        }
    }

    // Feeder thread.
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
        uint64_t userData = 0;
        auto decodeStart = std::chrono::steady_clock::now();
        {
            // Counted before the call: the decoder may hand the frame over before Decode returns.
            std::lock_guard lock(framesMutex);
            if (lastDecodeEnd != std::chrono::steady_clock::time_point{})
            {
                auto gapMs = std::chrono::duration<double, std::milli>(decodeStart - lastDecodeEnd).count();
                decodeStats.gaps++;
                decodeStats.gapSumMs += gapMs;
                decodeStats.gapMaxMs = (std::max)(decodeStats.gapMaxMs, gapMs);
            }
            userData = nextUserData++;
            timings[userData % timings.size()] = FrameTiming{ userData, pts, duration, generation, decodeStart };
            unitsInDecoder++;
        }
        auto hr = gpuDecoder->Decode(packet->data, static_cast<size_t>(packet->size), userData);
        {
            auto decodeEnd = std::chrono::steady_clock::now();
            auto decodeMs = std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();
            std::lock_guard lock(framesMutex);
            lastDecodeEnd = decodeEnd;
            decodeStats.calls++;
            decodeStats.sumMs += decodeMs;
            decodeStats.maxMs = (std::max)(decodeStats.maxMs, decodeMs);
        }
        if (hr == GpuVideoDecoder::ReconfigureRequired)
        {
            // A new sequence the decoder was not created for: its frames in flight are queued by the
            // drain, then a decoder for the new sequence takes the access unit again.
            hr = gpuDecoder->Drain();
            {
                // Nothing of this unit was decoded; it is the only one left in the decoder.
                std::lock_guard lock(framesMutex);
                unitsInDecoder = 1;
            }
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

    // framesMutex held.
    bool TakeFrame(GpuDecodedFrame& result, FrameTiming& resultTiming)
    {
        bool discarded = false;
        while (!readyFrames.empty())
        {
            auto frame = std::move(readyFrames.front());
            readyFrames.pop_front();
            auto& timing = timings[frame.userData % timings.size()];
            if (timing.userData == frame.userData && timing.generation == generation)
            {
                result = std::move(frame);
                resultTiming = timing;
                if (discarded) framesChanged.notify_all();
                return true;
            }
            // A frame of before a seek: back to the pool unsent.
            pool->Release(static_cast<GpuDecoderImagePool::Image*>(frame.image), 0);
            framesDiscarded++;
            discarded = true;
        }
        if (discarded) framesChanged.notify_all();
        return false;
    }

    void ReturnQueuedFrames()
    {
        {
            std::lock_guard lock(framesMutex);
            for (auto& frame : readyFrames)
            {
                pool->Release(static_cast<GpuDecoderImagePool::Image*>(frame.image), 0);
                framesDiscarded++;
            }
            readyFrames.clear();
        }
        framesChanged.notify_all();
    }

    // Hands every frame in flight back (they are never sent) and leaves the decoder ready for a key
    // frame. Errors of frames still in parse are part of this, not stream failures: drain until clean.
    // The feeder is paused or idle.
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
        {
            std::lock_guard lock(framesMutex);
            unitsInDecoder = 0;
        }
        ReturnQueuedFrames();
        handledCancelHits = pool->CancelHits();
    }

    HRESULT OutputFrame(GpuDecodedFrame& frame, const FrameTiming& timing, IDirect3DSurface* surface, int64_t& framePts, int64_t& frameDuration)
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

            // The player always gets a texture of its own pool, never a decoder image: the player keeps
            // samples until it has rendered them (and fills its buffer before it starts), so handing it
            // decoder images would let it drain the decoder's fixed pool and deadlock the decoder.
            D3D11_TEXTURE2D_DESC imageDesc{};
            image->texture->GetDesc(&imageDesc);
            UINT outputWidth = frameProcessor ? static_cast<UINT>(processedWidth) : imageDesc.Width;
            UINT outputHeight = frameProcessor ? static_cast<UINT>(processedHeight) : imageDesc.Height;
            DXGI_FORMAT outputFormat = frameProcessor ? OutputFormat() : imageDesc.Format;
            winrt::com_ptr<ID3D11Texture2D> output;
            {
                std::unique_lock lock(samplesMutex);
                if (!processedTexturePool) processedTexturePool = std::make_shared<TexturePool>(rendererDevice, 5);
                if (outputTextureLimit > 0 && processedTexturePool->PooledTextureCount() == 0 && OutputTexturesHeld() >= outputTextureLimit)
                {
                    auto returned = outputTextureReturned.wait_for(lock, std::chrono::milliseconds(OutputTextureWaitMs),
                        [this] { return processedTexturePool->PooledTextureCount() > 0 || OutputTexturesHeld() < outputTextureLimit; });
                    if (!returned) outputTextureOverruns++;
                }
                winrt::check_hresult(processedTexturePool->GetTexture(outputWidth, outputHeight, outputFormat,
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, output));
                playerTexturesCreated.store(processedTexturePool->CreatedCount());
            }
            BaleMemTrackTexture(output.get(), frameProcessor ? BaleMemCategory::ToneMapOutput : BaleMemCategory::OtherTextures);

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
                }
                else
                {
                    rendererContext->CopyResource(output.get(), image->texture.get());
                }
                releaseValue = ++releaseFenceValue;
                winrt::check_hresult(rendererContext4->Signal(releaseFence.get(), releaseValue));
                rendererContext->Flush();
                auto removed = rendererDevice->GetDeviceRemovedReason();
                if (FAILED(removed))
                {
                    av_log(nullptr, AV_LOG_ERROR, "%s: device removed (hr=0x%08X)\n", logName, static_cast<unsigned>(removed));
                    winrt::throw_hresult(removed);
                }
            }

            framePts = timing.pts;
            frameDuration = timing.duration;
            sampleColor = frameProcessor ? color : frame.color;
            sampleColor.chromaLocation = color.chromaLocation;
            // The processor or the copy has read the image (fenced); the sample carries the output.
            pool->Release(image, releaseValue);
            image = nullptr;
            pendingOutput = output;
            *surface = DirectXInteropHelper::GetSurface(output.as<IDXGISurface>());
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
            // Over the limit (after an overrun): let the texture go instead of keeping it.
            bool keep = outputTextureLimit == 0 || (outputPool && OutputTexturesHeld() + outputPool->PooledTextureCount() < outputTextureLimit);
            if (keep && outputPool && outputPool == processedTexturePool) outputPool->ReturnTexture(tracked.output);
            tracked.output = nullptr;
            outputTextureReturned.notify_all();
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

    // Player-side textures out of the pool: with samples the player has, or about to be. Caller holds
    // samplesMutex.
    uint32_t OutputTexturesHeld() const
    {
        uint32_t held = pendingOutput ? 1u : 0u;
        for (auto& entry : trackedSamples)
        {
            if (entry.second.output) held++;
        }
        return held;
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
    static constexpr uint32_t OutputTextureWaitMs = 150;
    uint32_t outputTextureLimit = 0;                  // guarded by samplesMutex
    uint32_t outputTextureOverruns = 0;               // guarded by samplesMutex
    std::condition_variable outputTextureReturned;    // with samplesMutex
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
    // Feeder state (also changed by Flush/ResumeAfterCancel while the feeder is paused).
    uint32_t packetsWithoutSequence = 0;
    bool awaitKeyFrame = true;
    bool inputEnded = false;
    int feederPriority = THREAD_PRIORITY_NORMAL;
    std::thread feeder;
    // Sample thread state (also changed by Flush/ResumeAfterCancel while the sample thread is idle).
    uint32_t handledCancelHits = 0;
    bool cancelledRequest = false;
    unsigned framesOut = 0;
    unsigned framesDiscarded = 0;
    GpuDecoderImagePool::Image* pendingImage = nullptr;
    winrt::com_ptr<ID3D11Texture2D> pendingOutput;

    // Under framesMutex: the frame queue, the timing table, the feeder's control and its counts.
    std::mutex framesMutex;
    std::condition_variable framesChanged;
    std::deque<GpuDecodedFrame> readyFrames;
    std::array<FrameTiming, 64> timings{};
    uint64_t nextUserData = 0;
    uint64_t generation = 0;
    uint32_t inflightTarget = DefaultInflight;
    uint32_t unitsInDecoder = 0;   // Fed and not handed back by FrameReady (0 after a drain).
    uint32_t feederPauses = 0;
    bool outputting = false;        // The sample thread delivers a frame it took from the queue.
    bool feederBusy = false;        // The feeder runs a step (packet read, decode, drain).
    bool feederStop = false;
    bool feedEnabled = false;       // The stream is enabled (DisableStream: no packets come).
    bool feedEnded = false;         // End of stream: the decoder is drained, the queue holds the rest.
    bool feedCancelled = false;     // A step ended in a cancel; the feeder waits for ResumeAfterCancel.

    std::mutex samplesMutex;
    std::map<winrt::Windows::Foundation::IUnknown, TrackedSample> trackedSamples;
};
