#pragma once
#include "pch.h"
#include "UncompressedVideoSampleProvider.h"
#include "TexturePool.h"
#include "VideoFrameProcessor.h"
#include "BaleMemoryLedger.h"
#include <d3d10.h>
#include "DirectXInteropHelper.h"
#include "FFmpegReader.h"
#include "FFmpegUtils.h"
#include "D3D11DecoderPoolSize.h"
#include "D3D12FrameInterop.h"

extern "C"
{
#include <libavutil/hwcontext_d3d11va.h>
}

using namespace winrt::Windows::Media::Core;

class D3D11VideoSampleProvider : public UncompressedVideoSampleProvider, public std::enable_shared_from_this<D3D11VideoSampleProvider>
{
private:
    const AVCodec* hwCodec;
    const AVCodec* swCodec;
public:

    D3D11VideoSampleProvider(
        std::shared_ptr<FFmpegReader> reader,
        AVFormatContext* avFormatCtx,
        AVCodecContext* avCodecCtx,
        winrt::FFmpegInteropX::MediaSourceConfig const& config,
        int streamIndex,
        HardwareDecoderStatus hardwareDecoderStatus,
        bool applyHdrColorInfo,
        std::unique_ptr<VideoFrameProcessor> processor = nullptr,
        int targetOutputWidth = 0,
        int targetOutputHeight = 0,
        int outputDepth = 0)
        : UncompressedVideoSampleProvider(reader, avFormatCtx, avCodecCtx, config, streamIndex, hardwareDecoderStatus, applyHdrColorInfo)
    {
        frameProcessor = std::move(processor);
        // P010 whenever the processor can write it; NV12 for a processor without P010 output, or
        // dithered NV12 when 8-bit output was requested (outputDepth 8, see SelectOutputFormat).
        outputP010 = frameProcessor && VideoFrameProcessor::SelectOutputFormat(*frameProcessor, outputDepth) == DXGI_FORMAT_P010;
        processorWidth = avCodecCtx->width;
        processorHeight = avCodecCtx->height;
        processedWidth = processorWidth;
        processedHeight = processorHeight;
        SelectProcessedSize(targetOutputWidth, targetOutputHeight);
        decoder = DecoderEngine::FFmpegD3D11HardwareDecoder;
        hwCodec = avCodecCtx->codec;

        if (avCodecCtx->codec_id == AVCodecID::AV_CODEC_ID_AV1)
        {
            swCodec = avcodec_find_decoder_by_name("libdav1d");
        }
        else
        {
            swCodec = NULL;
        }
    }

public:

    virtual ~D3D11VideoSampleProvider()
    {
        ReleaseTrackedSamples();
        // Bale diagnostics: pairs with "created" to show every stream's GPU resources were released.
        av_log(nullptr, AV_LOG_INFO, "Bale video: d3d11 provider destroyed (tracked=%d output created=%u reused=%u)\n",
            static_cast<int>(trackedSamples.size()), processedTexturePool ? processedTexturePool->CreatedCount() : 0u,
            processedTexturePool ? processedTexturePool->ReusedCount() : 0u);
        if (d3d12Interop)
        {
            av_log(nullptr, AV_LOG_INFO, "Bale video: d3d12 frames destroyed (opened=%d)\n", static_cast<int>(d3d12Interop->TextureCount()));
        }
    }

    virtual void Flush(bool flushBuffers) override
    {
        UncompressedVideoSampleProvider::Flush(flushBuffers);
        ReturnTrackedSamples();
    }


    // Size the decoder's frame pool from the stream (D3D11DecoderPoolSize) when the codec context is
    // recreated for a new device, as the media source does at open.
    bool PoolFromStream = false;

    virtual HRESULT CreateBufferFromFrame(IBuffer* pBuffer, IDirect3DSurface* surface, AVFrame* avFrame, int64_t& framePts, int64_t& frameDuration) override
    {
        auto fatalHr = FatalError();
        if (FAILED(fatalHr)) return fatalHr;
        if (!poolLogged && avFrame->format == AV_PIX_FMT_D3D11 && avFrame->hw_frames_ctx)
        {
            poolLogged = true;
            auto frames = reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
            av_log(nullptr, AV_LOG_INFO, "Bale video: d3d11 provider created; decoder surfaces=%d %dx%d processed=%d\n",
                frames->initial_pool_size, frames->width, frames->height, frameProcessor ? 1 : 0);
        }
        if (avFrame->format == AV_PIX_FMT_D3D11)
        {
            // The decoder's texture array; counted once, so every later frame of the pool is a no-op.
            BaleMemTrackTexture(reinterpret_cast<ID3D11Texture2D*>(avFrame->data[0]), BaleMemCategory::DecoderFrames);
        }
        if (avFrame->format == AV_PIX_FMT_D3D12) return CreateBufferFromD3D12Frame(pBuffer, surface, avFrame, framePts, frameDuration);
        return CreateBufferFromHardwareTexture(pBuffer, surface, avFrame, framePts, frameDuration,
            avFrame->format == AV_PIX_FMT_D3D11 ? reinterpret_cast<ID3D11Texture2D*>(avFrame->data[0]) : nullptr,
            static_cast<UINT>(reinterpret_cast<uintptr_t>(avFrame->data[1])));
    }

    // A d3d12va frame, read on the D3D11 device through its shared texture (D3D12FrameInterop); the
    // stage or the copy below then reads it exactly as a d3d11va frame.
    HRESULT CreateBufferFromD3D12Frame(IBuffer* pBuffer, IDirect3DSurface* surface, AVFrame* avFrame, int64_t& framePts, int64_t& frameDuration)
    {
        if (!poolLogged && avFrame->hw_frames_ctx)
        {
            poolLogged = true;
            auto frames = reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
            av_log(nullptr, AV_LOG_INFO, "Bale video: d3d12 provider created; decoder surfaces=%d %dx%d processed=%d\n",
                frames->initial_pool_size, frames->width, frames->height, frameProcessor ? 1 : 0);
        }
        winrt::com_ptr<ID3D11Texture2D> input;
        bool opened = false;
        HRESULT hr = d3d12Interop ? d3d12Interop->Acquire(avFrame, input, opened) : E_NOT_VALID_STATE;
        if (FAILED(hr))
        {
            if (!d3d12FailureLogged)
            {
                d3d12FailureLogged = true;
                av_log(nullptr, AV_LOG_ERROR, "Bale video: d3d12 frame not readable on d3d11 (hr=0x%08X)\n", static_cast<unsigned>(hr));
            }
            return frameProcessor ? FailProcessing(hr) : hr;
        }
        if (opened)
        {
            // One shared texture per decoder frame; counted when it is first opened here.
            BaleMemTrackTexture(input.get(), BaleMemCategory::DecoderFrames);
        }
        hr = CreateBufferFromHardwareTexture(pBuffer, surface, avFrame, framePts, frameDuration, input.get(), 0);
        auto releaseHr = d3d12Interop->Release(avFrame);
        if (SUCCEEDED(hr) && FAILED(releaseHr))
        {
            hr = frameProcessor ? FailProcessing(releaseHr) : releaseHr;
        }
        return hr;
    }

    // A decoded hardware frame as a D3D11 texture (and array slice), or a software frame (no texture).
    HRESULT CreateBufferFromHardwareTexture(IBuffer* pBuffer, IDirect3DSurface* surface, AVFrame* avFrame, int64_t& framePts,
        int64_t& frameDuration, ID3D11Texture2D* decodedTexture, UINT subresource)
    {
        if (frameProcessor) return CreateProcessedBufferFromFrame(surface, avFrame, framePts, decodedTexture, subresource);

        HRESULT hr = S_OK;
        if (decodedTexture)
        {
            if (!texturePool)
            {
                // init texture pool, fail if we did not get a device ptr
                std::lock_guard lock(samplesMutex);
                if (device && deviceContext)
                {
                    texturePool = std::make_shared<TexturePool>(device, 5);
                }
                else
                {
                    hr = E_FAIL;
                }
            }
            //copy texture data
            if (SUCCEEDED(hr))
            {
                winrt::com_ptr<ID3D11Texture2D> renderTexture;
                //happy path:decoding and rendering on same GPU
                {
                    std::lock_guard lock(samplesMutex);
                    hr = texturePool->GetCopyTexture(decodedTexture, renderTexture);
                }
                if (SUCCEEDED(hr))
                {
                    BaleMemTrackTexture(renderTexture.get(), BaleMemCategory::OtherTextures);

                    // copy the decoded texture to the render texture
                    deviceContext->CopySubresourceRegion(renderTexture.get(), 0, 0, 0, 0, decodedTexture, subresource, NULL);
                    deviceContext->Flush();

                    //create a IDXGISurface from the shared texture
                    auto finalSurface = renderTexture.as<IDXGISurface>();

                    //get the IDirect3DSurface pointer
                    *surface = DirectXInteropHelper::GetSurface(finalSurface);
                }

                if (SUCCEEDED(hr))
                {
                    CheckFrameSize(avFrame);
                    ReadFrameProperties(avFrame, framePts);
                }

                if (decoder != DecoderEngine::FFmpegD3D11HardwareDecoder)
                {
                    decoder = DecoderEngine::FFmpegD3D11HardwareDecoder;
                    VideoInfo().as<implementation::VideoStreamInfo>()->decoderEngine = decoder;
                }
            }
        }
        else
        {
            hr = UncompressedVideoSampleProvider::CreateBufferFromFrame(pBuffer, surface, avFrame, framePts, frameDuration);

            if (decoder != DecoderEngine::FFmpegSoftwareDecoder)
            {
                decoder = DecoderEngine::FFmpegSoftwareDecoder;
                VideoInfo().as<implementation::VideoStreamInfo>()->decoderEngine = decoder;
            }
        }

        return hr;
    }

    virtual HRESULT SetSampleProperties(MediaStreamSample const& sample) override
    {
        try
        {
            if (sample.Direct3D11Surface())
            {
                std::lock_guard lock(samplesMutex);

                // Capture the texture while the sample is open. MF may close its surface before Processed.
                winrt::com_ptr<IDXGISurface> surface;
                auto hr = DirectXInteropHelper::GetDXGISurface(sample.Direct3D11Surface(), surface);
                if (FAILED(hr)) return frameProcessor ? FailProcessing(hr) : hr;
                auto texture = surface.as<ID3D11Texture2D>();

                // AddRef the sample on native interface to prevent it from being collected before Processed is called
                auto sampleNative = sample.as<winrt::Windows::Foundation::IUnknown>();

                // Attach Processed event and store in samples list
                auto token = sample.Processed(weak_handler(this, &D3D11VideoSampleProvider::OnProcessed));
                try
                {
                    trackedSamples.emplace(sampleNative, TrackedSample{ token, texture, frameProcessor ? processedTexturePool : texturePool });
                }
                catch (...)
                {
                    DetachProcessed(sampleNative, token);
                    throw;
                }
            }

            auto hr = UncompressedVideoSampleProvider::SetSampleProperties(sample);
            return FAILED(hr) && frameProcessor ? FailProcessing(hr) : hr;
        }
        catch (...)
        {
            if (frameProcessor) return FailProcessing(winrt::to_hresult());
            throw;
        }
    }

    void OnProcessed(MediaStreamSample const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        try
        {
            std::lock_guard lock(samplesMutex);
            auto sampleNative = sender.as<winrt::Windows::Foundation::IUnknown>();
            auto entry = trackedSamples.find(sampleNative);
            if (entry == trackedSamples.end()) return; // Already released by Flush or device change.

            auto tracked = std::move(entry->second);
            trackedSamples.erase(entry);
            DetachProcessed(sampleNative, tracked.token);
            ReturnTextureToPool(tracked);
        }
        catch (...)
        {
            // Never throw out of a Media Foundation event handler.
            DebugMessage(L"Failed to recycle processed video texture.\n");
        }
    }

    struct TrackedSample
    {
        winrt::event_token token;
        winrt::com_ptr<ID3D11Texture2D> texture;
        std::weak_ptr<TexturePool> pool;
    };

    void DetachProcessed(winrt::Windows::Foundation::IUnknown const& sample, winrt::event_token token) noexcept
    {
        try
        {
            sample.as<MediaStreamSample>().Processed(token);
        }
        catch (...)
        {
            DebugMessage(L"Failed to detach processed video sample.\n");
        }
    }

    void ReturnTextureToPool(TrackedSample const& tracked)
    {
        // A replacement pool must never receive textures from the previous device generation.
        auto pool = tracked.pool.lock();
        if (pool && (pool == texturePool || pool == processedTexturePool))
        {
            pool->ReturnTexture(tracked.texture);
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

    void ReturnTrackedSamples()
    {
        std::lock_guard lock(samplesMutex);
        for (auto& entry : trackedSamples)
        {
            DetachProcessed(entry.first, entry.second.token);
            ReturnTextureToPool(entry.second);
        }
        trackedSamples.clear();
    }

    virtual HRESULT SetHardwareDevice(winrt::com_ptr<ID3D11Device> newDevice,
        winrt::com_ptr<ID3D11DeviceContext> newDeviceContext,
        AVBufferRef* avHardwareContext) override
    {
        HRESULT hr = S_OK;

        bool isCompatible = false;
        hr = CheckHWAccelerationCompatible(newDevice, isCompatible);

        if (SUCCEEDED(hr))
        {
            if (isCompatible)
            {
                bool needsReinit = device.get();

                // must explicitly reset com_ptr before assigning new value
                device = nullptr;
                deviceContext = nullptr;

                device = newDevice;
                deviceContext = newDeviceContext;

                // d3d12va decoding: its frames are read on this D3D11 device through shared textures.
                d3d12Interop.reset();
                if (D3D12FrameInterop::IsD3D12(avHardwareContext))
                {
                    d3d12Interop = std::make_unique<D3D12FrameInterop>();
                    hr = d3d12Interop->Initialize(avHardwareContext, device.get(), deviceContext.get());
                    if (FAILED(hr))
                    {
                        av_log(nullptr, AV_LOG_ERROR, "Bale video: d3d12 interop failed (hr=0x%08X)\n", static_cast<unsigned>(hr));
                        d3d12Interop.reset();
                    }
                }

                if (SUCCEEDED(hr) && needsReinit)
                {
                    hr = UpdateCodecContext(hwCodec, avHardwareContext);
                }
                else if (SUCCEEDED(hr) && (!m_pAvCodecCtx->hw_device_ctx || m_pAvCodecCtx->hw_device_ctx->data != avHardwareContext->data))
                {
                    av_buffer_unref(&m_pAvCodecCtx->hw_device_ctx);
                    m_pAvCodecCtx->hw_device_ctx = av_buffer_ref(avHardwareContext);
                }
            }
            else
            {
                d3d12Interop.reset();
                device = nullptr;
                deviceContext = nullptr;
                av_buffer_unref(&m_pAvCodecCtx->hw_device_ctx);

                if (swCodec)
                {
                    hr = UpdateCodecContext(swCodec, NULL);
                }
                else
                {
                    hr = E_FAIL;
                }
            }
        }

        {
            std::lock_guard lock(samplesMutex);
            texturePool.reset();
            texturePool = nullptr;
            processedTexturePool.reset();
            processorInitialized = false;
        }

        ReleaseTrackedSamples();

        return FAILED(hr) && frameProcessor ? FailProcessing(hr) : hr;
    }

    HRESULT UpdateCodecContext(const AVCodec* codec, AVBufferRef* avHardwareContext)
    {
        auto hr = S_OK;

        D3D11DecoderPoolSize::Forget(m_pAvCodecCtx);
        avcodec_free_context(&m_pAvCodecCtx);

        m_pAvCodecCtx = avcodec_alloc_context3(codec);
        if (!m_pAvCodecCtx)
        {
            hr = E_OUTOFMEMORY;
        }

        if (SUCCEEDED(hr))
        {
            // enable multi threading only for SW decoders
            if (!avHardwareContext)
            {
                unsigned threads = std::thread::hardware_concurrency();
                m_pAvCodecCtx->thread_count = m_config.Video().MaxDecoderThreads() == 0 ? threads : min(threads, m_config.Video().MaxDecoderThreads());
                m_pAvCodecCtx->thread_type = m_config.as<implementation::MediaSourceConfig>()->IsFrameGrabber ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
            }

            // initialize the stream parameters with demuxer information
            hr = avcodec_parameters_to_context(m_pAvCodecCtx, m_pAvStream->codecpar);
        }

        if (SUCCEEDED(hr))
        {
            m_pAvCodecCtx->hw_device_ctx = avHardwareContext ? av_buffer_ref(avHardwareContext) : NULL;
            m_pAvCodecCtx->get_format = PoolFromStream ? &D3D11DecoderPoolSize::GetFormat : &FFmpegUtils::get_format;
            hr = avcodec_open2(m_pAvCodecCtx, codec, NULL);
        }

        if (SUCCEEDED(hr))
        {
            frameProvider->UpdateCodecContext(m_pAvCodecCtx);
        }

        return hr;
    }

    HRESULT CheckHWAccelerationCompatible(winrt::com_ptr<ID3D11Device> newDevice, bool& isCompatible)
    {
        HRESULT hr = S_OK;
        isCompatible = true;

        if (m_pAvCodecCtx->codec->id == AVCodecID::AV_CODEC_ID_AV1)
        {
            isCompatible = m_pAvCodecCtx->profile >= 0 && m_pAvCodecCtx->profile <= 2;
            if (isCompatible)
            {
                const GUID av1Profiles[3]{
                    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0,
                    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1,
                    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE2
                };
                auto requiredProfile = av1Profiles[m_pAvCodecCtx->profile];
                auto videoDevice = newDevice.try_as<ID3D11VideoDevice>();

                // check profile exists
                if (videoDevice)
                {
                    isCompatible = false;
                    GUID profile;
                    int count = videoDevice->GetVideoDecoderProfileCount();
                    for (int i = 0; i < count; i++)
                    {
                        hr = videoDevice->GetVideoDecoderProfile(i, &profile);
                        if (FAILED(hr))
                        {
                            break;
                        }
                        else if (profile == requiredProfile)
                        {
                            isCompatible = true;
                            break;
                        }
                    }
                }

                // check resolution
                if (SUCCEEDED(hr) && isCompatible)
                {
                    D3D11_VIDEO_DECODER_DESC desc;
                    desc.Guid = requiredProfile;
                    desc.OutputFormat = DXGI_FORMAT_NV12;

                    desc.SampleWidth = m_pAvCodecCtx->coded_width;
                    desc.SampleHeight = m_pAvCodecCtx->coded_height;

                    UINT count;
                    hr = videoDevice->GetVideoDecoderConfigCount(&desc, &count);
                    if (SUCCEEDED(hr))
                    {
                        isCompatible = count > 0;
                    }
                }

            }

            if (FAILED(hr))
            {
                isCompatible = false;
            }
        }

        return hr;
    }

    static HRESULT InitializeHardwareDeviceContext(MediaStreamSource sender,
        AVBufferRef* avHardwareContext,
        winrt::com_ptr<ID3D11Device>& outDevice,
        winrt::com_ptr<ID3D11DeviceContext>& outDeviceContext,
        const winrt::com_ptr<IMFDXGIDeviceManager>& deviceManager,
        HANDLE* outDeviceHandle)
    {
        UNREFERENCED_PARAMETER(sender);

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> deviceContext;
        winrt::com_ptr<ID3D11VideoDevice> videoDevice;
        winrt::com_ptr<ID3D11VideoContext> videoContext;

        HRESULT hr = DirectXInteropHelper::GetDeviceFromStreamSource(deviceManager, device, deviceContext, videoDevice, outDeviceHandle);

        if (D3D12FrameInterop::IsD3D12(avHardwareContext))
        {
            // d3d12va: a D3D12 device on the Media Foundation device's adapter; the D3D11 device stays
            // the one samples are rendered on.
            if (SUCCEEDED(hr))
            {
                auto multithread = device.try_as<ID3D10Multithread>();
                if (multithread) multithread->SetMultithreadProtected(TRUE);
                hr = D3D12FrameInterop::CreateDeviceContext(device.get(), avHardwareContext);
            }
            av_log(nullptr, AV_LOG_INFO, "Bale video: d3d12 device (hr=0x%08X)\n", static_cast<unsigned>(hr));
            if (SUCCEEDED(hr))
            {
                outDevice = device;
                outDeviceContext = deviceContext;
            }
            return hr;
        }

        if (SUCCEEDED(hr)) hr = deviceContext->QueryInterface(videoContext.put());

        auto dataBuffer = (AVHWDeviceContext*)avHardwareContext->data;
        auto internalDirectXHwContext = (AVD3D11VADeviceContext*)dataBuffer->hwctx;

        if (SUCCEEDED(hr))
        {
            // give ownership to FFmpeg

            device.copy_to(&internalDirectXHwContext->device);
            deviceContext.copy_to(&internalDirectXHwContext->device_context);
            videoDevice.copy_to(&internalDirectXHwContext->video_device);
            videoContext.copy_to(&internalDirectXHwContext->video_context);
        }
        else
        {
            // release
            device = nullptr;
            deviceContext = nullptr;
            videoDevice = nullptr;
            videoContext = nullptr;
        }

        if (SUCCEEDED(hr))
        {
            // multithread interface seems to be optional
            auto multithread = device.try_as<ID3D10Multithread>();
            if (multithread)
                multithread->SetMultithreadProtected(TRUE);
        }

        if (SUCCEEDED(hr))
        {
            hr = av_hwdevice_ctx_init(avHardwareContext);
        }

        if (SUCCEEDED(hr))
        {
            // addref and hand out pointers
            outDevice = device;
            outDeviceContext = deviceContext;
        }

        return hr;
    }

protected:
    bool HasProcessedOutput() const override { return frameProcessor != nullptr; }
    void ProcessedOutputSize(int& width, int& height) const override
    {
        width = processedWidth;
        height = processedHeight;
    }

    // The size is fixed for the stream's lifetime.
    void SelectProcessedSize(int targetWidth, int targetHeight)
    {
        if (!frameProcessor) return;
        VideoFrameProcessor::SelectOutputSize(*frameProcessor, processorWidth, processorHeight, targetWidth, targetHeight,
            processedWidth, processedHeight);
    }

    void AddProcessedOutputAttributes(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const override
    {
        if (frameProcessor)
        {
            frameProcessor->DescribeOutput(properties);
        }
    }
    AVPixelFormat ProcessedOutputPixelFormat() const override
    {
        return outputP010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    }
    DXGI_FORMAT ProcessedOutputDxgiFormat() const
    {
        return outputP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }

private:
    // Match FFmpeg's lock ordering: its device lock precedes the D3D multithread lock.
    class DeviceContextLock
    {
    public:
        DeviceContextLock(AVD3D11VADeviceContext* hardwareContext, ID3D10Multithread* multithread)
            : hardwareContext(hardwareContext), multithread(multithread)
        {
            if (hardwareContext && hardwareContext->lock && hardwareContext->unlock)
            {
                hardwareContext->lock(hardwareContext->lock_ctx);
                locked = true;
            }
            multithread->Enter();
        }

        ~DeviceContextLock()
        {
            multithread->Leave();
            if (locked) hardwareContext->unlock(hardwareContext->lock_ctx);
        }

        DeviceContextLock(const DeviceContextLock&) = delete;
        DeviceContextLock& operator=(const DeviceContextLock&) = delete;

    private:
        AVD3D11VADeviceContext* hardwareContext;
        ID3D10Multithread* multithread;
        bool locked = false;
    };

    HRESULT FailProcessing(HRESULT hr)
    {
        auto fatalHr = LatchFatalError(hr);
        DebugMessage(L"Video frame processing failed.\n");
        frameProcessor->NotifyError(fatalHr);
        return fatalHr;
    }

    HRESULT CreateProcessedBufferFromFrame(IDirect3DSurface* surface, AVFrame* frame, int64_t& framePts, ID3D11Texture2D* input,
        UINT subresource)
    {
        try
        {
            // The declared output never changes; a fallback or size change cannot bypass processing.
            if ((frame->format != AV_PIX_FMT_D3D11 && frame->format != AV_PIX_FMT_D3D12) || !input || frame->width != processorWidth ||
                frame->height != processorHeight)
            {
                winrt::throw_hresult(E_INVALIDARG);
            }
            if (!device || !deviceContext) winrt::throw_hresult(E_NOINTERFACE);
            if (processorWidth <= 0 || processorHeight <= 0 || (processorWidth & 1) || (processorHeight & 1))
            {
                winrt::throw_hresult(E_INVALIDARG);
            }
            D3D11_TEXTURE2D_DESC desc{};
            input->GetDesc(&desc);
            if (!frameProcessor->SupportsInputFormat(desc.Format) || desc.Width < static_cast<UINT>(processorWidth) ||
                desc.Height < static_cast<UINT>(processorHeight) || subresource >= desc.ArraySize || desc.MipLevels != 1)
            {
                winrt::throw_hresult(E_INVALIDARG);
            }
            winrt::com_ptr<ID3D11Texture2D> output;
            {
                std::lock_guard lock(samplesMutex);
                if (!processedTexturePool) processedTexturePool = std::make_shared<TexturePool>(device, 5);
                winrt::check_hresult(processedTexturePool->GetTexture(processedWidth, processedHeight, ProcessedOutputDxgiFormat(),
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, output));
            }
            BaleMemTrackTexture(output.get(), BaleMemCategory::ToneMapOutput);
            if (!outputFormatLogged)
            {
                outputFormatLogged = true;
                av_log(nullptr, AV_LOG_INFO, "Bale tonemap output format=%s\n", outputP010 ? "P010"
                    : frameProcessor->DithersOutputFormat(DXGI_FORMAT_NV12) ? "NV12 dithered" : "NV12");
            }

            auto multithread = device.try_as<ID3D10Multithread>();
            if (!multithread) multithread = deviceContext.try_as<ID3D10Multithread>();
            if (!multithread || (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED))
            {
                winrt::throw_hresult(E_NOINTERFACE);
            }
            multithread->SetMultithreadProtected(TRUE);
            if (!multithread->GetMultithreadProtected()) winrt::throw_hresult(E_FAIL);
            auto colorInfo = VideoFrameColorInfo::FromFrame(frame, m_pAvStream->codecpar);
            // FFmpeg's D3D11 device lock; a d3d12va device has none on this D3D11 device.
            auto hardwareDevice = m_pAvCodecCtx->hw_device_ctx;
            auto hardwareContext = hardwareDevice &&
                reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data)->type == AV_HWDEVICE_TYPE_D3D11VA
                ? reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data)->hwctx)
                : nullptr;
            {
                // Never hold samplesMutex while entering the shared immediate context.
                DeviceContextLock contextLock(hardwareContext, multithread.get());
                if (!processorInitialized)
                {
                    winrt::check_hresult(frameProcessor->Initialize(device.get(), processedWidth, processedHeight, processorWidth, processorHeight));
                    processorInitialized = true;
                }
                winrt::check_hresult(frameProcessor->Process(deviceContext.get(), input, subresource, output.get(), colorInfo));
                deviceContext->Flush();
                auto removed = device->GetDeviceRemovedReason();
                if (FAILED(removed))
                {
                    // The shared MF device was removed; the stage only noticed it, it did not cause it.
                    av_log(nullptr, AV_LOG_ERROR, "Bale video: device removed (hr=0x%08X)\n", static_cast<unsigned>(removed));
                    winrt::throw_hresult(removed);
                }
            }
            *surface = DirectXInteropHelper::GetSurface(output.as<IDXGISurface>());
            ReadFrameProperties(frame, framePts);
            return S_OK;
        }
        catch (...)
        {
            return FailProcessing(winrt::to_hresult());
        }
    }

    bool outputP010 = false;
    bool outputFormatLogged = false;
    std::unique_ptr<VideoFrameProcessor> frameProcessor;
    bool processorInitialized = false;
    bool poolLogged = false;
    bool d3d12FailureLogged = false;
    std::unique_ptr<D3D12FrameInterop> d3d12Interop;
    int processorWidth = 0;   // visible decoded (source) size
    int processorHeight = 0;
    int processedWidth = 0;   // processed output size (smaller when downscaling)
    int processedHeight = 0;
    std::shared_ptr<TexturePool> processedTexturePool;
    std::shared_ptr<TexturePool> texturePool;
    std::map<winrt::Windows::Foundation::IUnknown, TrackedSample> trackedSamples;
    std::mutex samplesMutex;
};

