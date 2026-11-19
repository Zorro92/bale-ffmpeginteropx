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

#pragma once
#include "UncompressedSampleProvider.h"
#include "BaleD3D11Upload.h"
#include "VideoColorAttributes.h"
#include <atomic>
using namespace winrt::Windows::Media::Core;


class UncompressedVideoSampleProvider : public UncompressedSampleProvider
{
public:
    virtual ~UncompressedVideoSampleProvider();
    virtual void Flush(bool flushBuffers) override
    {
        hasFirstInterlacedFrame = false;
        UncompressedSampleProvider::Flush(flushBuffers);
        uploadSample = nullptr;
        if (upload) upload->Flush();
    }

    UncompressedVideoSampleProvider(
        std::shared_ptr<FFmpegReader> reader,
        AVFormatContext* avFormatCtx,
        AVCodecContext* avCodecCtx,
        MediaSourceConfig const& config,
        int streamIndex,
        HardwareDecoderStatus hardwareDecoderStatus,
        bool applyHdrColorInfo);
    winrt::Windows::Media::Core::IMediaStreamDescriptor CreateStreamDescriptor() override;
    virtual HRESULT CreateBufferFromFrame(winrt::Windows::Storage::Streams::IBuffer* pBuffer, winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface* surface, AVFrame* avFrame, int64_t& framePts, int64_t& frameDuration) override;
    HRESULT CreateNextSampleBuffer(IBuffer* buffer, int64_t& pts, int64_t& duration, IDirect3DSurface* surface) override
    {
        // Bale: a failed zero-copy stream decodes nothing more (no sample, no retries).
        auto fatalHr = FatalError();
        if (FAILED(fatalHr)) return fatalHr;
        auto hr = UncompressedSampleProvider::CreateNextSampleBuffer(buffer, pts, duration, surface);
        // End-of-stream releases grain resources without dropping MF's outstanding samples.
        if (hr != S_OK && upload) upload->EndGrain();
        return hr;
    }
    virtual HRESULT SetSampleProperties(winrt::Windows::Media::Core::MediaStreamSample  const& sample) override;
    void ReadFrameProperties(AVFrame* avFrame, int64_t& framePts);
    void CheckFrameSize(AVFrame* avFrame);
    AVPixelFormat GetOutputPixelFormat() { return m_OutputPixelFormat; }
    // Shared with the pre-open grain decision; no duplicate output-format policy.
    // eightBitZeroCopy (EightBitZeroCopy below) sends 8-bit 4:2:0 to P010 for the zero-copy direct write.
    static AVPixelFormat SelectOutputPixelFormat(AVPixelFormat input, bool direct, MediaSourceConfig const& config, bool eightBitZeroCopy = false);
    static bool ConversionNeedsTenBitOutput(AVPixelFormat input);
    // Bale: 8-bit libdav1d pictures take zero-copy only as a direct write (bale_zc_repack_into_bits).
    // They then output P010 (samples << 8); otherwise NV12 as before.
    static bool EightBitZeroCopy(AVPixelFormat input, bool dav1d, bool hardware, int zeroCopy, MediaSourceConfig const& config);
    // Bale: whether a libdav1d stream with this input/output format can be a zero-copy stream.
    static bool ZeroCopyEligible(AVPixelFormat input, AVPixelFormat output, bool eightBitZeroCopy);
    // Bale: latched zero-copy failure codes (see FatalError). A write bale_gpu declined, and
    // more than 8 consecutive pictures outside the zero-copy buffers.
    static constexpr HRESULT ZeroCopyDeclined = static_cast<HRESULT>(0x80042B02L);
    static constexpr HRESULT ZeroCopyMissed = static_cast<HRESULT>(0x80042B03L);
    HRESULT FatalError() const override { return fatal.load(); }
    BaleFilmGrain FilmGrain;
    int TargetWidth = 0;
    int TargetHeight = 0;
    // Bale: 1 = libdav1d pictures decode into bale_gpu zero-copy buffers (FFmpeg native patch p1),
    // written on the GPU straight into the upload texture. Needs the D3D11 upload; the app
    // registers d3d12.dll with bale_gpu first. There is no CPU output path: anything zero-copy
    // cannot take fails the stream (FatalError) or, for a picture outside its buffers, is dropped.
    int ZeroCopy = 0;
    // Bale: EightBitZeroCopy's pre-open result for this stream; SelectOutputFormat must agree with it.
    bool ZeroCopyEightBit = false;
    // Bale: 1 = GPU film grain on the staged D3D11 upload (FilmGrain.mode 3, decided before the
    // decoder opened; dav1d exports the grain). The CPU converts each frame into a STAGING texture and
    // the D3D11 grain pass draws it into the MF texture; no D3D12, no zero-copy. Failures after the
    // open fail the stream (FatalError), since dav1d cannot apply the grain itself any more.
    int StagedGrain = 0;
    BYTE* TargetBuffer = NULL;

    // Bale: independent of SetHardwareDevice/AVHWDeviceContext; never changes the decoder engine.
    // S_FALSE for a stream that is neither zero-copy nor staged grain (no uploader); failures latch
    // FatalError.
    HRESULT SetUploadDevice(winrt::com_ptr<ID3D11Device> newDevice, winrt::com_ptr<ID3D11DeviceContext> newContext, int variant);
    void DisableUpload(HRESULT hr);
    MediaStreamSample CreateSurfaceSample(IDirect3DSurface const& surface, TimeSpan position) override
    {
        if (uploadSample)
        {
            auto sample = uploadSample;
            uploadSample = nullptr;
            return sample;
        }
        return MediaSampleProvider::CreateSurfaceSample(surface, position);
    }

    virtual void NotifyCreateSource() override
    {
        if (VideoInfo().FramesPerSecondOverride() > 0 && VideoInfo().FramesPerSecond() > 0)
        {
            timeBaseFactor *= VideoInfo().FramesPerSecond() / VideoInfo().FramesPerSecondOverride();
        }

        UncompressedSampleProvider::NotifyCreateSource();
    }

    virtual void InitializeStreamInfo() override
    {
        MediaSampleProvider::InitializeStreamInfo();

        auto videoStreamInfoImpl = videoStreamInfo.as<implementation::VideoStreamInfo>();

        videoStreamInfoImpl->SetHasHdrMetadata(hasHdrMetadata);
        videoStreamInfoImpl->SetIsHdrActive(isHdrActive);
    }

protected:
    virtual bool HasProcessedOutput() const { return false; }
    virtual AVPixelFormat ProcessedOutputPixelFormat() const { return AV_PIX_FMT_NV12; }
    // Processed output size; default: the decoded size (unchanged).
    virtual void ProcessedOutputSize(int& width, int& height) const { UNREFERENCED_PARAMETER(width); UNREFERENCED_PARAMETER(height); }
    virtual void AddProcessedOutputAttributes(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const
    {
        UNREFERENCED_PARAMETER(properties);
    }
    HRESULT LatchFatalError(HRESULT hr)
    {
        if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
        HRESULT expected = S_OK;
        fatal.compare_exchange_strong(expected, hr);
        return fatal.load();
    }

private:
    std::shared_ptr<BaleD3D11Upload> upload;
    MediaStreamSample uploadSample{ nullptr };
    // First fatal output failure, S_OK until then. Written on the serialized producer or
    // in OnStarting/device checks (decode-ahead paused); read by FFmpegMediaSource.
    std::atomic<HRESULT> fatal{ S_OK };
    HRESULT Fail(HRESULT hr, const char* reason);
    void SelectOutputFormat();
    HRESULT InitializeScalerIfRequired(AVFrame* avFrame);
    HRESULT FillLinesAndBuffer(int* linesize, BYTE** data, AVBufferRef** buffer, int width, int height, bool isSourceBuffer);
    AVBufferRef* AllocateBuffer(int requestedSize, AVBufferPool** bufferPool, int* bufferPoolSize);
    static int get_buffer2(AVCodecContext* avCodecContext, AVFrame* frame, int flags);
    // Bale zero-copy. The handle is created by SetUploadDevice and destroyed (exchanged to null) by
    // DisableZeroCopy, which runs only where decoding is paused or serialized: the hook below runs
    // inside avcodec_receive_frame, on the same serialized producer as CreateBufferFromFrame.
    std::atomic<void*> zeroCopy{ nullptr };
    bool zeroCopyHooked = false;
    bool zeroCopyLogged = false;
    bool zeroCopyDeclineLogged = false;
    bool zeroCopyGrainDeclineLogged = false;
    bool zeroCopyDropLogged = false;
    unsigned zeroCopyFrames = 0;
    unsigned zeroCopyDirectFrames = 0; // Written straight into the pooled texture (no landing copy).
    unsigned zeroCopyDirectGrainFrames = 0; // Of those, with GPU grain fused into the D3D12 write.
    unsigned zeroCopyEightBitFrames = 0; // Of those, 8-bit pictures written as P010.
    unsigned zeroCopyDropped = 0; // Pictures outside the zero-copy buffers, dropped (no sample).
    unsigned zeroCopyMissRun = 0; // Consecutive drops; more than 8 fail the stream.
    static int get_zero_copy_buffer(AVCodecContext* avCodecContext, AVFrame* frame, int flags);
    HRESULT CreateZeroCopy(winrt::com_ptr<ID3D11Device> const& device, winrt::com_ptr<ID3D11DeviceContext> const& context);
    HRESULT ZeroCopyFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts);
    HRESULT StagedGrainFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts);
    bool stagedGrainLogged = false;
    unsigned stagedGrainFrames = 0;     // Frames out through the staged upload.
    unsigned stagedGrainAppliedFrames = 0; // Of those, with grain applied.
    HRESULT DropFrame(AVFrame* avFrame);
    HRESULT TryZeroCopy(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts, const BaleGrainParams* params);
    void LogZeroCopyStats(void* handle, const char* when);
    void DisableZeroCopy(HRESULT hr);

    winrt::hstring outputMediaSubtype{};
    int outputWidth = 0;
    int outputHeight = 0;
    int outputFrameHeight = 0;
    int outputFrameWidth = 0;
    bool outputDirectBuffer = 0;
    bool applyHdrColorInfo = false;

    AVBufferPool* sourceBufferPool = NULL;
    int sourceBufferPoolSize = 0;
    AVBufferPool* targetBufferPool = NULL;
    int targetBufferPoolSize = 0;
    AVPixelFormat m_OutputPixelFormat = AVPixelFormat::AV_PIX_FMT_NONE;
    SwsContext* m_pSwsCtx = NULL;
    bool m_interlaced_frame = 0;
    bool m_top_field_first = 0;
    AVChromaLocation m_chroma_location = AVChromaLocation::AVCHROMA_LOC_UNSPECIFIED;
    bool hasFirstInterlacedFrame = 0;
    winrt::Windows::Foundation::IInspectable maxCLL = { nullptr };
    winrt::Windows::Foundation::IInspectable maxFALL = { nullptr };
    winrt::Windows::Foundation::IInspectable minLuminance = { nullptr };
    winrt::Windows::Foundation::IInspectable maxLuminance = { nullptr };
    winrt::Windows::Foundation::IInspectable customPrimaries = { nullptr };
    bool hasHdrMetadata = false;
    bool isHdrActive = false;
};

