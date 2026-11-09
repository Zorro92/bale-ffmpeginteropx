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
#include "BaleCopyPool.h"
using namespace winrt::Windows::Media::Core;

#pragma region Desktop Family
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

// {47537213-8cfb-4722-aa34-fbc9e24d77b8}   MF_MT_CUSTOM_VIDEO_PRIMARIES    {BLOB (MT_CUSTOM_VIDEO_PRIMARIES)}
DEFINE_GUID(MF_MT_CUSTOM_VIDEO_PRIMARIES,
    0x47537213, 0x8cfb, 0x4722, 0xaa, 0x34, 0xfb, 0xc9, 0xe2, 0x4d, 0x77, 0xb8);

typedef struct _MT_CUSTOM_VIDEO_PRIMARIES {
    float fRx;
    float fRy;
    float fGx;
    float fGy;
    float fBx;
    float fBy;
    float fWx;
    float fWy;
} MT_CUSTOM_VIDEO_PRIMARIES;

#endif /* WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES) */
#pragma endregion




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
    static AVPixelFormat SelectOutputPixelFormat(AVPixelFormat input, bool direct, MediaSourceConfig const& config);
    BaleFilmGrain FilmGrain;
    int TargetWidth = 0;
    int TargetHeight = 0;
    // Bale: slice threads for the output pixel conversion (1 = upstream single-threaded sws_scale).
    int SwsThreads = 1;
    // Bale: 0..2, BaleThreadBoost level for the sws slice threads.
    int SwsThreadPriority = 0;
    // Bale debug: after this many uploaded frames the next upload fails with E_OUTOFMEMORY (0 = off).
    int UploadFaultFrames = 0;
    // Bale debug: 1 = planar staging fill plus bale_gpu repack (needs upload variant 1).
    int Repack = 0;
    BYTE* TargetBuffer = NULL;

    // Bale: independent of SetHardwareDevice/AVHWDeviceContext; never changes the decoder engine.
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

private:
    std::shared_ptr<BaleD3D11Upload> upload;
    MediaStreamSample uploadSample{ nullptr };
    bool uploadFailed = false;
    bool uploadLogged = false;
    int uploadedFrames = 0;
    bool repackFailed = false;
    bool repackLogged = false;
    bool CanRepack(AVFrame* avFrame, const BaleGrainParams* params) const;
    void FillPlanar(AVFrame* avFrame, BYTE** data, UINT pitch);
    // Bale: FillPlanar row slices; SwsThreads workers, created on the first planar frame.
    std::unique_ptr<BaleCopyPool> copyPool;
    void DisableRepack(HRESULT hr);
    HRESULT ScaleFrame(AVFrame* avFrame, BYTE** data, int* linesize, AVBufferRef* buffer);
    HRESULT CopyFrameToOutput(AVFrame* avFrame, BYTE** data, int* linesize, AVBufferRef* buffer);
    void UploadFrame(AVFrame* avFrame, IDirect3DSurface* surface, int64_t& framePts, const BaleGrainParams* params, bool planar);
    void SelectOutputFormat();
    HRESULT InitializeScalerIfRequired(AVFrame* avFrame);
    HRESULT FillLinesAndBuffer(int* linesize, BYTE** data, AVBufferRef** buffer, int width, int height, bool isSourceBuffer);
    AVBufferRef* AllocateBuffer(int requestedSize, AVBufferPool** bufferPool, int* bufferPoolSize);
    static int get_buffer2(AVCodecContext* avCodecContext, AVFrame* frame, int flags);

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
    // Bale: parameters of an explicitly initialized threaded context (m_swsThreaded).
    bool m_swsThreaded = false;
    int m_swsSrcW = 0, m_swsSrcH = 0, m_swsDstW = 0, m_swsDstH = 0, m_swsFlags = 0;
    AVPixelFormat m_swsSrcFmt = AV_PIX_FMT_NONE, m_swsDstFmt = AV_PIX_FMT_NONE;
    HRESULT InitializeThreadedScaler(AVFrame* avFrame, int scaler);
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

