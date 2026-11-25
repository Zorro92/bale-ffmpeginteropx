#include "pch.h"
#include "FFmpegMediaSource.h"
#include "LanguageTagConverter.h"
#include "winrt/impl/Windows.Storage.Streams.0.h"
#include "FFmpegMediaSource.g.cpp"
#include "D3D11VideoSampleProvider.h"
#include "H264AVCSampleProvider.h"
#include "UncompressedAudioSampleProvider.h"
#include "UncompressedVideoSampleProvider.h"
#include "HEVCSampleProvider.h"
#include "NALPacketSampleProvider.h"
#include "CodecChecker.h"
#include "MediaSampleProvider.h"
#include "SubtitleProviderSsaAss.h"
#include "SubtitleProviderBitmap.h"
#include "FFmpegReader.h"
#include "PlatformInfo.h"
#include "BaleThreadBoost.h"
#include "BaleToneMap.h"
#include "GpuDecoderSampleProvider.h"
#include "BaleRav1g.h"
#include "D3D11DecoderPoolSize.h"
// Bale: Media Foundation error codes for streams zero-copy refuses at open.
#include <mferror.h>
#include <atomic>
#include <chrono>
#include <thread>

#ifdef Win32
#include <winrt/Microsoft.Graphics.Display.h>
#endif

void free_buffer(void* lpVoid);

// Bale: thread-attach hook for BaleThreadBoost (no other DllMain exists in this module).
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_THREAD_ATTACH)
    {
        BaleThreadBoost::OnThreadAttach();
    }
    return TRUE;
}

namespace winrt::FFmpegInteropX::implementation
{
    using namespace Windows::Foundation;
    using namespace Windows::Storage::Streams;
    using namespace Windows::Media::Playback;

    // Static functions passed to FFmpeg
    static int FileStreamRead(void* ptr, uint8_t* buf, int bufSize);
    static int InputStreamRead(void* ptr, uint8_t* buf, int bufSize);
    static int64_t FileStreamSeek(void* ptr, int64_t pos, int whence);
    static int IsShuttingDown(void* ptr);

    // Flag for ffmpeg global setup
    static bool isRegistered = false;
    std::mutex isRegisteredMutex;

    FFmpegMediaSource::FFmpegMediaSource(winrt::com_ptr<MediaSourceConfig> const& interopConfig,
         uint64_t windowId, bool useHdr)
        : config(interopConfig)
        , isFirstSeek(true)
        , windowId(windowId)
        , useHdr(useHdr)
    {
        avDict = NULL;
        avHardwareContext = NULL;
        avHardwareContextDefault = NULL;
        device = NULL;
        deviceContext = NULL;
        deviceHandle = NULL;
        deviceManager = NULL;

        if (!isRegistered)
        {
            std::lock_guard lock(isRegisteredMutex);
            if (!isRegistered)
            {
                LanguageTagConverter::Initialize();
                isRegistered = true;
            }
        }
        audioStrInfos = winrt::single_threaded_observable_vector<winrt::FFmpegInteropX::AudioStreamInfo>();
        subtitleStrInfos = winrt::single_threaded_observable_vector<winrt::FFmpegInteropX::SubtitleStreamInfo>();
        videoStrInfos = winrt::single_threaded_observable_vector<winrt::FFmpegInteropX::VideoStreamInfo>();
        if (!config->IsExternalSubtitleParser && !config->IsFrameGrabber)
        {
            metadata = std::shared_ptr<MediaMetadata>(new MediaMetadata());
        }
    }

    FFmpegMediaSource::~FFmpegMediaSource()
    {
        Close();
    }

    winrt::com_ptr<FFmpegMediaSource> FFmpegMediaSource::CreateFromStream(
        IRandomAccessStream const& stream,
        winrt::com_ptr<MediaSourceConfig> const& config,
        uint64_t windowId,
        bool useHdr)
    {
        auto interopMSS = winrt::make_self<FFmpegMediaSource>(config, windowId, useHdr);
        auto hr = interopMSS->CreateMediaStreamSource(stream);
        if (!SUCCEEDED(hr))
        {
            throw_hresult(hr);
        }
        return interopMSS;
    }

    winrt::com_ptr<FFmpegMediaSource> FFmpegMediaSource::CreateFromInputStream(
        IInputStream const& stream,
        winrt::com_ptr<MediaSourceConfig> const& config,
        uint64_t windowId,
        bool useHdr)
    {
        auto interopMSS = winrt::make_self<FFmpegMediaSource>(config, windowId, useHdr);
        auto hr = interopMSS->CreateMediaStreamSource(stream);
        if (!SUCCEEDED(hr))
        {
            throw_hresult(hr);
        }
        return interopMSS;
    }

    winrt::com_ptr<FFmpegMediaSource> FFmpegMediaSource::CreateFromUri(
        hstring const& uri,
        winrt::com_ptr<MediaSourceConfig> const& config,
        uint64_t windowId,
        bool useHdr)
    {
        auto interopMSS = winrt::make_self<FFmpegMediaSource>(config, windowId, useHdr);
        auto hr = interopMSS->CreateMediaStreamSource(uri);
        if (!SUCCEEDED(hr))
        {
            throw_hresult(hr);
        }
        return interopMSS;
    }

    HRESULT FFmpegMediaSource::CreateMediaStreamSource(IRandomAccessStream const& stream)
    {
        HRESULT hr = S_OK;
        if (!stream)
        {
            hr = E_INVALIDARG;
        }

        if (SUCCEEDED(hr))
        {
            // Convert asynchronous IRandomAccessStream to synchronous IStream. This API requires shcore.h and shcore.lib
            //
            //
            hr = CreateStreamOverRandomAccessStream(reinterpret_cast<::IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&fileStreamData));

        }

        unsigned char* fileStreamBuffer = NULL;
        if (SUCCEEDED(hr))
        {
            // Setup FFmpeg custom IO to access file as stream. This is necessary when accessing any file outside of app installation directory and appdata folder.
            // Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
            fileStreamBuffer = (unsigned char*)av_malloc(config->General().FileStreamReadSize());
            if (fileStreamBuffer == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            avIOCtx = avio_alloc_context(fileStreamBuffer, config->General().FileStreamReadSize(), 0, (void*)winrt::get_abi(this), FileStreamRead, 0, FileStreamSeek);
            if (avIOCtx == nullptr)
            {
                av_free(fileStreamBuffer);
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            avFormatCtx = avformat_alloc_context();
            if (avFormatCtx == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            // Populate AVDictionary avDict based on PropertySet ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
            hr = ParseOptions(config->FFmpegOptions());
        }


        if (SUCCEEDED(hr))
        {
            // Populate AVDictionary avDict based on additional ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
            hr = ParseOptions(config->AdditionalFFmpegSubtitleOptions);
        }

        if (SUCCEEDED(hr))
        {
            // Register callback for fast dispose
            avFormatCtx->interrupt_callback.callback = IsShuttingDown;
            avFormatCtx->interrupt_callback.opaque = this;
        }

        if (SUCCEEDED(hr))
        {
            avFormatCtx->pb = avIOCtx;
            avFormatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
            // Open media file using custom IO setup above instead of using file name. Opening a file using file name will invoke fopen C API call that only have
            // access within the app installation directory and appdata folder. Custom IO allows access to file selected using FilePicker dialog.
            if (avformat_open_input(&avFormatCtx, "", NULL, &avDict) < 0)
            {
                hr = E_FAIL; // Error opening file
            }

            // avDict is not NULL only when there is an issue with the given ffmpegOptions such as invalid key, value type etc. Iterate through it to see which one is causing the issue.
            if (avDict != nullptr)
            {
                DebugMessage(L"Invalid FFmpeg option(s)");
                av_dict_free(&avDict);

                avDict = nullptr;
            }
        }

        if (SUCCEEDED(hr))
        {
            hr = InitFFmpegContext();
        }

        return hr;
    }

    HRESULT FFmpegMediaSource::CreateMediaStreamSource(IInputStream const& stream)
    {
        HRESULT hr = S_OK;
        if (!stream)
        {
            hr = E_INVALIDARG;
        }

        inputStream = stream;

        unsigned char* fileStreamBuffer = NULL;
        if (SUCCEEDED(hr))
        {
            // Setup FFmpeg custom IO to access file as stream. This is necessary when accessing any file outside of app installation directory and appdata folder.
            // Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
            fileStreamBuffer = (unsigned char*)av_malloc(config->General().FileStreamReadSize());
            if (fileStreamBuffer == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            avIOCtx = avio_alloc_context(fileStreamBuffer, config->General().FileStreamReadSize(), 0, (void*)winrt::get_abi(this), InputStreamRead, 0, 0);
            if (avIOCtx == nullptr)
            {
                av_free(fileStreamBuffer);
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            avFormatCtx = avformat_alloc_context();
            if (avFormatCtx == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            // Populate AVDictionary avDict based on PropertySet ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
            hr = ParseOptions(config->FFmpegOptions());
        }


        if (SUCCEEDED(hr))
        {
            // Populate AVDictionary avDict based on additional ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
            hr = ParseOptions(config->AdditionalFFmpegSubtitleOptions);
        }

        if (SUCCEEDED(hr))
        {
            // Register callback for fast dispose
            avFormatCtx->interrupt_callback.callback = IsShuttingDown;
            avFormatCtx->interrupt_callback.opaque = this;
        }

        if (SUCCEEDED(hr))
        {
            avFormatCtx->pb = avIOCtx;
            avFormatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
            // Open media file using custom IO setup above instead of using file name. Opening a file using file name will invoke fopen C API call that only have
            // access within the app installation directory and appdata folder. Custom IO allows access to file selected using FilePicker dialog.
            if (avformat_open_input(&avFormatCtx, "", NULL, &avDict) < 0)
            {
                hr = E_FAIL; // Error opening file
            }

            // avDict is not NULL only when there is an issue with the given ffmpegOptions such as invalid key, value type etc. Iterate through it to see which one is causing the issue.
            if (avDict != nullptr)
            {
                DebugMessage(L"Invalid FFmpeg option(s)");
                av_dict_free(&avDict);

                avDict = nullptr;
            }
        }

        if (SUCCEEDED(hr))
        {
            hr = InitFFmpegContext();
        }

        return hr;
    }

    HRESULT FFmpegMediaSource::CreateMediaStreamSource(hstring const& uri)
    {
        HRESULT hr = S_OK;
        if (uri.empty())
        {
            hr = E_INVALIDARG;
        }

        if (SUCCEEDED(hr))
        {
            avFormatCtx = avformat_alloc_context();
            if (avFormatCtx == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            // Populate AVDictionary avDict based on PropertySet ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
            hr = ParseOptions(config->FFmpegOptions());
        }

        if (SUCCEEDED(hr))
        {
            // Register callback for fast dispose
            avFormatCtx->interrupt_callback.callback = IsShuttingDown;
            avFormatCtx->interrupt_callback.opaque = this;
        }

        if (SUCCEEDED(hr))
        {
            auto charStr = StringUtils::PlatformStringToUtf8String(uri);

            // Open media in the given URI using the specified options
            if (avformat_open_input(&avFormatCtx, charStr.c_str(), NULL, &avDict) < 0)
            {
                hr = E_FAIL; // Error opening file
            }

            // avDict is not NULL only when there is an issue with the given ffmpegOptions such as invalid key, value type etc. Iterate through it to see which one is causing the issue.
            if (avDict != nullptr)
            {
                DebugMessage(L"Invalid FFmpeg option(s)");
                av_dict_free(&avDict);
                avDict = nullptr;
            }
        }

        if (SUCCEEDED(hr))
        {
            hr = InitFFmpegContext();
        }

        return hr;
    }

    void FFmpegMediaSource::OnPresentationModeChanged(MediaPlaybackTimedMetadataTrackList const& sender, TimedMetadataPresentationModeChangedEventArgs const& args)
    {
        UNREFERENCED_PARAMETER(sender);
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        int index = 0;
        for (auto& stream : subtitleStreamProviders)
        {
            if (stream->SubtitleTrack == args.Track())
            {
                auto mode = args.NewPresentationMode();
                if (mode == TimedMetadataTrackPresentationMode::Disabled || mode == TimedMetadataTrackPresentationMode::Hidden)
                {
                    stream->DisableStream();
                }
                else
                {
                    stream->EnableStream();
                }
            }
            index++;
        }
    }

    void FFmpegMediaSource::OnAudioTracksChanged(MediaPlaybackItem const& sender, IVectorChangedEventArgs const& args)
    {
        UNREFERENCED_PARAMETER(args);
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        if (sender.AudioTracks().Size() == AudioStreams().Size())
        {
            for (unsigned int i = 0; i < AudioStreams().Size(); i++)
            {
                auto track = sender.AudioTracks().GetAt(i);
                auto info = AudioStreams().GetAt(i);
                if (!info.Name().empty())
                {
                    track.Label(info.Name());
                }
                else if (!info.Language().empty())
                {
                    track.Label(info.Language());
                }
            }
        }
    }

    void FFmpegMediaSource::InitializePlaybackItem(MediaPlaybackItem const& playbackItem)
    {
        audioTracksChangedToken = playbackItem.AudioTracksChanged({ get_weak(), &FFmpegInteropX::implementation::FFmpegMediaSource::OnAudioTracksChanged });
        subtitlePresentationModeChangedToken = playbackItem.TimedMetadataTracks().PresentationModeChanged({ get_weak(), &FFmpegInteropX::implementation::FFmpegMediaSource::OnPresentationModeChanged });

        if (config->Subtitles().AutoSelectForcedSubtitles())
        {
            int index = 0;
            for (auto stream : subtitleStreamInfos)
            {
                if (stream.IsForced())
                {
                    playbackItem.TimedMetadataTracks().SetPresentationMode(index, TimedMetadataTrackPresentationMode::PlatformPresented);
                    break;
                }

                index++;
            }
        }

        for (auto& stream : subtitleStreamProviders)
        {
            stream->PlaybackItemWeak = playbackItem;
        }

        playbackItemWeak = playbackItem;
    }

    IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> FFmpegMediaSource::CreateFromStreamInternalAsync(
        IRandomAccessStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
    {
        winrt::apartment_context caller; // Capture calling context.
        auto configImpl = config.as<winrt::FFmpegInteropX::implementation::MediaSourceConfig>();
        bool useHdr = false;
        CheckUseHdr(configImpl, IsOnUIThread(), useHdr, windowId);
        co_await winrt::resume_background();
        auto result = CreateFromStream(stream, configImpl, windowId, useHdr);
        co_await caller;
        co_return result.as<FFmpegInteropX::FFmpegMediaSource>();
    }

    IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> FFmpegMediaSource::CreateFromInputStreamInternalAsync(
        IInputStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
    {
        winrt::apartment_context caller; // Capture calling context.
        auto configImpl = config.as<winrt::FFmpegInteropX::implementation::MediaSourceConfig>();
        bool useHdr = false;
        CheckUseHdr(configImpl, IsOnUIThread(), useHdr, windowId);
        co_await winrt::resume_background();
        auto result = CreateFromInputStream(stream, configImpl, windowId, useHdr);
        co_await caller;
        co_return result.as<FFmpegInteropX::FFmpegMediaSource>();
    }

    IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> FFmpegMediaSource::CreateFromUriInternalAsync(
        hstring uri, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
    {
        winrt::apartment_context caller; // Capture calling context.
        auto configImpl = config.as<winrt::FFmpegInteropX::implementation::MediaSourceConfig>();
        bool useHdr = false;
        CheckUseHdr(configImpl, IsOnUIThread(), useHdr, windowId);
        co_await winrt::resume_background();
        auto result = CreateFromUri(uri, configImpl, windowId, useHdr);
        co_await caller;
        co_return result.as<FFmpegInteropX::FFmpegMediaSource>();
    }

    bool FFmpegMediaSource::IsOnUIThread()
    {
#ifdef Win32
        if (PlatformInfo::IsWinUI())
        {
            return DispatcherQueue::GetForCurrentThread() != nullptr;
        }
        return winrt::Windows::System::DispatcherQueue::GetForCurrentThread() != nullptr;
#else
        return DispatcherQueue::GetForCurrentThread() != nullptr;
#endif
    }

    static int is_hwaccel_pix_fmt(enum AVPixelFormat pix_fmt)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);
        return desc->flags & AV_PIX_FMT_FLAG_HWACCEL;
    }

    static AVPixelFormat get_format(struct AVCodecContext* s, const enum AVPixelFormat* fmt)
    {
        AVPixelFormat result_sw = (AVPixelFormat)-1;
        AVPixelFormat result_hw = (AVPixelFormat)-1;
        AVPixelFormat format;
        int index = 0;
        do
        {
            format = fmt[index++];

            //		
            if (format != -1)
            {
                if (s->hw_device_ctx && format == AV_PIX_FMT_D3D11)
                {
                    // we only support D3D11 HW format (not D3D11_VLD)
                    result_hw = format;
                }
                else if (result_sw == -1 && !is_hwaccel_pix_fmt(format))
                {
                    // take first non hw accelerated format
                    result_sw = format;
                }
                else if (format == AV_PIX_FMT_NV12 && result_sw != AV_PIX_FMT_YUVA420P)
                {
                    // switch SW format to NV12 if available, unless this is an alpha channel file
                    result_sw = format;
                }
            }
        } while (format != -1);


        if (result_hw != -1)
        {
            return result_hw;
        }
        else
        {
            return result_sw;
        }
    }

    HRESULT FFmpegMediaSource::InitFFmpegContext()
    {
        HRESULT hr = S_OK;

        if (SUCCEEDED(hr))
        {
            if (avformat_find_stream_info(avFormatCtx, NULL) < 0)
            {
                hr = E_FAIL; // Error finding info
            }
        }

        if (SUCCEEDED(hr))
        {
            m_pReader = std::shared_ptr<FFmpegReader>(new FFmpegReader(avFormatCtx, &sampleProviders, config.as<FFmpegInteropX::MediaSourceConfig>()));
            if (m_pReader == nullptr)
            {
                hr = E_OUTOFMEMORY;
            }
        }

        // do not use start time for pure subtitle files
        if (config->IsExternalSubtitleParser && avFormatCtx->nb_streams == 1)
        {
            avFormatCtx->start_time = AV_NOPTS_VALUE;
        }

        const AVCodec* avVideoCodec;
        auto videoStreamIndex = av_find_best_stream(avFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &avVideoCodec, 0);
        auto audioStreamIndex = av_find_best_stream(avFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        auto subtitleStreamIndex = av_find_best_stream(avFormatCtx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);

        attachedFileHelper = shared_ptr<AttachedFileHelper>(new AttachedFileHelper(config.as<winrt::FFmpegInteropX::MediaSourceConfig>()));

        // first parse attached files, so they are available for subtitle streams during initialize
        if (config->Subtitles().UseEmbeddedSubtitleFonts())
        {
            for (unsigned int index = 0; index < avFormatCtx->nb_streams; index++)
            {
                auto avStream = avFormatCtx->streams[index];
                if (avStream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                {
                    auto fileName = av_dict_get(avStream->metadata, "filename", NULL, 0);
                    auto mimetype = av_dict_get(avStream->metadata, "mimetype", NULL, 0);
                    if (fileName && avStream->codecpar->extradata && avStream->codecpar->extradata_size > 0)
                    {
                        auto name = StringUtils::Utf8ToPlatformString(fileName->value);
                        auto mime = mimetype ? StringUtils::Utf8ToPlatformString(mimetype->value) : L"";

                        auto file = std::shared_ptr<AttachedFile>(new AttachedFile(name, mime, avStream));
                        attachedFileHelper->AddAttachedFile(file);
                    }
                }
            }
        }

        bool videoRefused = false;
        for (int index = 0; index < (int)avFormatCtx->nb_streams; index++)
        {
            auto avStream = avFormatCtx->streams[index];
            avStream->discard = AVDISCARD_ALL; // all streams are disabled until we enable them

            std::shared_ptr<MediaSampleProvider> stream = nullptr;

            if (avStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !config->IsFrameGrabber && !config->IsExternalSubtitleParser)
            {
                stream = CreateAudioStream(avStream, index);
                if (stream)
                {
                    if (index == audioStreamIndex)
                    {
                        stream->AudioInfo().as<implementation::AudioStreamInfo>()->SetDefault();
                        currentAudioStream = stream;
                        audioStrInfos.InsertAt(0, stream->AudioInfo());
                        audioStreams.insert(audioStreams.begin(), stream);
                    }
                    else
                    {
                        audioStrInfos.Append(stream->AudioInfo());
                        audioStreams.push_back(stream);
                    }
                }
            }
            else if (avStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && avStream->disposition == AV_DISPOSITION_ATTACHED_PIC && !thumbnailData && !config->IsExternalSubtitleParser)
            {
                thumbnailData = ExtractThumbnail(avStream);
            }
            else if (avStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !config->IsExternalSubtitleParser)
            {
                stream = CreateVideoStream(avStream, index);
                // Bale: a refused default video stream fails the open, so the app replans.
                if (!stream && baleVideoRefused)
                {
                    videoRefused = true;
                    if (index == videoStreamIndex) hr = MF_E_UNSUPPORTED_FORMAT;
                }
                if (stream)
                {
                    if (index == videoStreamIndex)
                    {
                        stream->VideoInfo().as<implementation::VideoStreamInfo>()->isDefault = true;
                        currentVideoStream = stream;
                        videoStreams.insert(videoStreams.begin(), stream);
                        videoStrInfos.InsertAt(0, stream->VideoInfo());
                    }
                    else
                    {
                        videoStreams.push_back(stream);
                        videoStrInfos.Append(stream->VideoInfo());
                    }
                }
            }
            else if (avStream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream = CreateSubtitleSampleProvider(avStream, index);
                if (stream)
                {
                    if (index == subtitleStreamIndex)
                    {
                        stream->SubtitleInfo().as<implementation::SubtitleStreamInfo>()->SetDefault();
                        subtitleStrInfos.InsertAt(0, stream->SubtitleInfo());
                        subtitleStreamProviders.insert(subtitleStreamProviders.begin(), (std::reinterpret_pointer_cast<SubtitleProvider>(stream)));
                    }
                    else
                    {
                        subtitleStrInfos.Append(stream->SubtitleInfo());
                        subtitleStreamProviders.push_back((std::reinterpret_pointer_cast<SubtitleProvider>(stream)));
                    }

                    // enable all subtitle streams for external subtitle parsing
                    if (config->IsExternalSubtitleParser)
                    {
                        (std::reinterpret_pointer_cast<SubtitleProvider>(stream))->EnableStream();
                    }
                }
            }

            sampleProviders.push_back(stream);
        }

        if (!currentAudioStream && audioStreams.size() > 0)
        {
            currentAudioStream = audioStreams[0];
        }
        if (!currentVideoStream && videoStreams.size() > 0)
        {
            currentVideoStream = videoStreams[0];
        }
        // Bale: never open audio-only because every video stream was refused.
        if (videoRefused && videoStreams.empty() && SUCCEEDED(hr))
        {
            hr = MF_E_UNSUPPORTED_FORMAT;
        }

        if (currentVideoStream)
        {
            currentVideoStream->EnableStream();
            currentVideoStreamInfo = currentVideoStream->VideoInfo();
        }

        if (currentAudioStream)
        {
            currentAudioStream->EnableStream();
            currentAudioStreamInfo = currentAudioStream->AudioInfo();
        }

        audioStreamInfos = audioStrInfos.GetView();
        subtitleStreamInfos = subtitleStrInfos.GetView();
        videoStreamInfos = videoStrInfos.GetView();



        if (currentVideoStream)
        {
            auto videoDescriptor = (currentVideoStream->StreamDescriptor().as<VideoStreamDescriptor>());
            auto encodingProperties = videoDescriptor.EncodingProperties();
            auto pixelAspect = (double)encodingProperties.PixelAspectRatio().Numerator() / encodingProperties.PixelAspectRatio().Denominator();
            auto videoAspect = ((double)encodingProperties.Width() / encodingProperties.Height()) / pixelAspect;
            for (auto& stream : subtitleStreamProviders)
            {
                stream->NotifyVideoFrameSize(encodingProperties.Width(), encodingProperties.Height(), videoAspect);
            }
        }

        if (SUCCEEDED(hr))
        {
            // Convert media duration from AV_TIME_BASE to TimeSpan unit
            mediaDuration = TimeSpan(LONGLONG(avFormatCtx->duration * 10000000 / double(AV_TIME_BASE)));

            auto title = av_dict_get(avFormatCtx->metadata, "title", NULL, 0);
            auto titleStr = title ? StringUtils::Utf8ToPlatformString(title->value) : L"";
            auto codecStr = StringUtils::Utf8ToPlatformString(avFormatCtx->iformat->name);
            formatInfo = winrt::FFmpegInteropX::FormatInfo(titleStr, codecStr, mediaDuration, avFormatCtx->bit_rate);

            auto chapters = winrt::single_threaded_observable_vector<winrt::FFmpegInteropX::ChapterInfo>();
            if (avFormatCtx->chapters && avFormatCtx->nb_chapters > 1)
            {
                for (size_t i = 0; i < avFormatCtx->nb_chapters; i++)
                {
                    auto chapter = avFormatCtx->chapters[i];
                    auto entry = av_dict_get(chapter->metadata, "title", NULL, AV_DICT_IGNORE_SUFFIX);
                    if (entry)
                    {
                        auto chapterTitle = StringUtils::Utf8ToPlatformString(entry->value);
                        TimeSpan start{ (long long)((chapter->start / (double)chapter->time_base.den) * chapter->time_base.num * 10000000) };
                        TimeSpan duration{ (long long)(((chapter->end - chapter->start) / (double)chapter->time_base.den) * chapter->time_base.num * 10000000) };

                        // compensate for start time offset
                        if (avFormatCtx->start_time != AV_NOPTS_VALUE)
                        {
                            start = TimeSpan(start.count() - (avFormatCtx->start_time * 10));
                        }

                        // cut off negative start times
                        if (start.count() < 0)
                        {
                            duration = TimeSpan(duration.count() + start.count());
                            start = TimeSpan(0);
                        }

                        if (duration.count() > 0)
                        {
                            auto chapInfo = winrt::FFmpegInteropX::ChapterInfo(chapterTitle, start, duration);
                            chapters.Append(chapInfo);
                        }
                    }
                }
            }
            chapterInfos = chapters.GetView();
        }

        if (!config->Video().FFmpegVideoFilters().empty())
        {
            SetFFmpegVideoFilters(config->Video().FFmpegVideoFilters());
        }

        if (!config->Audio().FFmpegAudioFilters().empty())
        {
            SetFFmpegAudioFilters(config->Audio().FFmpegAudioFilters());
        }

        return hr;
    }

    MediaStreamSource FFmpegMediaSource::CreateMediaStreamSource()
    {
        MediaStreamSource mss = nullptr;

        if (currentVideoStream && currentAudioStream)
        {
            mss = MediaStreamSource(currentVideoStream->StreamDescriptor(), currentAudioStream->StreamDescriptor());
        }
        else if (currentAudioStream)
        {
            mss = MediaStreamSource(currentAudioStream->StreamDescriptor());
        }
        else if (currentVideoStream)
        {
            mss = MediaStreamSource(currentVideoStream->StreamDescriptor());
        }
        else if (subtitleStreamProviders.size() == 0 || !config->IsExternalSubtitleParser)
        {
            //only fail if there are no media streams (audio, video, or subtitle)
            throw_hresult(E_FAIL);
        }

        for (auto& stream : audioStreams)
        {
            if (stream != currentAudioStream)
            {
                mss.AddStreamDescriptor(stream->StreamDescriptor());
            }
        }

        for (auto& stream : videoStreams)
        {
            if (stream != currentVideoStream)
            {
                mss.AddStreamDescriptor(stream->StreamDescriptor());
            }
        }

        mss.BufferTime(TimeSpan{ 0 });

        //support playback rate
        auto maxSupportedPlaybackRate = config->General().MaxSupportedPlaybackRate();
        if (maxSupportedPlaybackRate > 1.0)
        {
            mss.MaxSupportedPlaybackRate(winrt::box_value(maxSupportedPlaybackRate).as<Windows::Foundation::IReference<double>>());
        }

        if (mediaDuration.count() > 0)
        {
            mss.Duration(mediaDuration);
            mss.CanSeek(true);
        }

        mssWeak = mss;

        // using strong reference here would create circle references, since we store MSS and MediaPlaybackItem here.
        startingRequestedToken = mss.Starting({ get_weak(), &FFmpegMediaSource::OnStarting });
        sampleRequestedToken = mss.SampleRequested({ get_weak(), &FFmpegMediaSource::OnSampleRequested });
        switchStreamRequestedToken = mss.SwitchStreamsRequested({ get_weak(), &FFmpegMediaSource::OnSwitchStreamsRequested });




        return mss;
    }


    std::shared_ptr<SubtitleProvider> FFmpegMediaSource::CreateSubtitleSampleProvider(AVStream* avStream, int index)
    {
        HRESULT hr = S_OK;
        std::shared_ptr<SubtitleProvider> avSubsStream = nullptr;
        auto avSubsCodec = avcodec_find_decoder(avStream->codecpar->codec_id);
        if (avSubsCodec)
        {
            // allocate a new decoding context
            auto avSubsCodecCtx = avcodec_alloc_context3(avSubsCodec);
            if (!avSubsCodecCtx)
            {
                DebugMessage(L"Could not allocate a decoding context\n");
                hr = E_OUTOFMEMORY;
            }

            // Apply subtitle characte encoding for external subs
            if (config->IsExternalSubtitleParser)
            {
                auto subtitleEncoding = config->Subtitles().ExternalSubtitleEncoding();
                if (!subtitleEncoding && (streamEncoding == TextEncodingDetect::ANSI || streamEncoding == TextEncodingDetect::ASCII))
                {
                    subtitleEncoding = config->Subtitles().ExternalSubtitleAnsiEncoding();
                }

                auto useUtf8 =
                    (!subtitleEncoding && (streamEncoding == TextEncodingDetect::UTF8_BOM || streamEncoding == TextEncodingDetect::UTF8_NOBOM)) ||
                    (subtitleEncoding && subtitleEncoding.WindowsCodePage() == 65001);
                if (useUtf8)
                {
                    // special case for utf8
                    if (av_opt_set(avSubsCodecCtx, "sub_charenc", "utf8", AV_OPT_SEARCH_CHILDREN) < 0)
                    {
                        DebugMessage(L"Could not set sub_charenc on subtitle provider\n");
                    }
                    if (av_opt_set_int(avSubsCodecCtx, "sub_charenc_mode", FF_SUB_CHARENC_MODE_IGNORE, AV_OPT_SEARCH_CHILDREN) < 0)
                    {
                        DebugMessage(L"Could not set sub_charenc_mode on subtitle provider\n");
                    }
                }
                else if (subtitleEncoding)
                {
                    // other encodings need conversion
                    hstring key = subtitleEncoding.Name();
                    std::string keyA = StringUtils::PlatformStringToUtf8String(key);
                    const char* keyChar = keyA.c_str();

                    if (av_opt_set(avSubsCodecCtx, "sub_charenc", keyChar, AV_OPT_SEARCH_CHILDREN) < 0)
                    {
                        DebugMessage(L"Could not set sub_charenc on subtitle provider\n");
                    }
                    if (av_opt_set_int(avSubsCodecCtx, "sub_charenc_mode", FF_SUB_CHARENC_MODE_AUTOMATIC, AV_OPT_SEARCH_CHILDREN) < 0)
                    {
                        DebugMessage(L"Could not set sub_charenc_mode on subtitle provider\n");
                    }
                }
                else
                {
                    // no encoding detected or set: leave ffmpeg default behavior.
                }
            }

            if (SUCCEEDED(hr))
            {
                // initialize the stream parameters with demuxer information
                if (avcodec_parameters_to_context(avSubsCodecCtx, avStream->codecpar) < 0)
                {
                    hr = E_FAIL;
                }

                if (SUCCEEDED(hr))
                {
                    if (avcodec_open2(avSubsCodecCtx, avSubsCodec, NULL) < 0)
                    {
                        hr = E_FAIL;
                    }
                    else
                    {
                        if ((avSubsCodecCtx->codec_descriptor->props & AV_CODEC_PROP_TEXT_SUB) == AV_CODEC_PROP_TEXT_SUB)
                        {
                            avSubsStream = std::shared_ptr<SubtitleProvider>(new SubtitleProviderSsaAss(m_pReader, avFormatCtx, avSubsCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, attachedFileHelper));
                        }
                        else if ((avSubsCodecCtx->codec_descriptor->props & AV_CODEC_PROP_BITMAP_SUB) == AV_CODEC_PROP_BITMAP_SUB)
                        {
                            // Bale: the chosen bitmap stream feeds bale_gpu's subtitle overlay instead of ImageCues.
                            uint64_t sinkToken = 0;
                            if (baleSubtitleSink > 0 && index == baleSubtitleStream && !config->IsExternalSubtitleParser)
                            {
                                if (BaleSubtitleSinkExports::Get().Available())
                                {
                                    sinkToken = (uint64_t)baleSubtitleSink;
                                    av_log(nullptr, AV_LOG_INFO, "Bale subtitle sink: stream %d (%s)\n", index, avSubsCodec->name);
                                }
                                else
                                {
                                    av_log(nullptr, AV_LOG_WARNING, "Bale subtitle sink: bale_gpu.dll has no bitmap ABI 1; stream %d stays a cue track.\n", index);
                                }
                            }
                            avSubsStream = std::shared_ptr<SubtitleProvider>(new SubtitleProviderBitmap(m_pReader, avFormatCtx, avSubsCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, sinkToken));
                        }
                        else
                        {
                            hr = E_FAIL;
                        }
                    }
                }
            }

            if (SUCCEEDED(hr))
            {
                avSubsStream->SetStreamDelay(config->Subtitles().DefaultSubtitleDelay());
                hr = avSubsStream->Initialize();
            }

            if (FAILED(hr))
            {
                avSubsStream = nullptr;
            }

            // free codec context if failed
            if (!avSubsStream && avSubsCodecCtx)
            {
                avcodec_free_context(&avSubsCodecCtx);
            }
        }
        else
        {
            DebugMessage(L"Could not find decoder\n");
        }

        return avSubsStream;
    }


    std::shared_ptr<MediaSampleProvider> FFmpegMediaSource::CreateAudioStream(AVStream* avStream, int index)
    {
        HRESULT hr = S_OK;
        std::shared_ptr<MediaSampleProvider> audioStream = nullptr;
        auto avAudioCodec = avcodec_find_decoder(avStream->codecpar->codec_id);
        if (avAudioCodec)
        {
            // allocate a new decoding context
            auto avAudioCodecCtx = avcodec_alloc_context3(avAudioCodec);
            if (!avAudioCodecCtx)
            {
                DebugMessage(L"Could not allocate a decoding context\n");
                hr = E_OUTOFMEMORY;
            }

            if (SUCCEEDED(hr))
            {
                // initialize the stream parameters with demuxer information
                if (avcodec_parameters_to_context(avAudioCodecCtx, avStream->codecpar) < 0)
                {
                    hr = E_FAIL;
                }

                if (SUCCEEDED(hr))
                {
                    if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_S16P)
                    {
                        avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_S16;
                    }
                    else if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_S32P)
                    {
                        avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_S32;
                    }
                    else if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP)
                    {
                        avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_FLT;
                    }

                    // enable multi threading
                    unsigned threads = std::thread::hardware_concurrency();
                    if (threads > 0)
                    {
                        avAudioCodecCtx->thread_count = config->Audio().MaxDecoderThreads() == 0 ? threads : min((int)threads, config->Audio().MaxDecoderThreads());
                        avAudioCodecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
                    }

                    if (avcodec_open2(avAudioCodecCtx, avAudioCodec, NULL) < 0)
                    {
                        hr = E_FAIL;
                    }
                    else if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_NONE)
                    {
                        hr = E_FAIL;
                    }
                    else
                    {
                        // Detect audio format and create audio stream descriptor accordingly
                        audioStream = CreateAudioSampleProvider(avStream, avAudioCodecCtx, index);
                    }
                }
            }

            // free codec context if failed
            if (!audioStream && avAudioCodecCtx)
            {
                avcodec_free_context(&avAudioCodecCtx);
            }
        }
        else
        {
            DebugMessage(L"Could not find decoder\n");
        }

        return audioStream;
    }


    std::shared_ptr<MediaSampleProvider> FFmpegMediaSource::CreateVideoStream(AVStream* avStream, int index)
    {
        HRESULT hr = S_OK;
        std::shared_ptr<MediaSampleProvider> result = nullptr;

        int filmGrainMode = 0;
        bool eightBitZeroCopy = false;
        bool zeroCopyStream = false;
        baleVideoRefused = false;
        // Bale: opt-in GPU AV1 decoding. bale_zero_copy, bale_d3d11_upload and bale_film_grain_mode do
        // not apply to it (the decoder applies film grain); when it cannot take the stream, the
        // paths below do, with those keys.
        if (baleAv1Decoder > 0 && avStream->codecpar->codec_id == AV_CODEC_ID_AV1 && !config->IsFrameGrabber)
        {
            result = CreateGpuDecoderStream(avStream, index);
            if (result)
            {
                return result;
            }
            if (baleAv1FallbackMaxHeight > 0 && avStream->codecpar->height > baleAv1FallbackMaxHeight)
            {
                av_log(nullptr, AV_LOG_WARNING, "Bale AV1: refusing video stream (GPU decoder declined, height %d above the fallback limit %d).\n",
                    avStream->codecpar->height, baleAv1FallbackMaxHeight);
                baleVideoRefused = true;
                return nullptr;
            }
        }
        // Find the video stream and its decoder
        auto avVideoCodec = avcodec_find_decoder(avStream->codecpar->codec_id);

        if (avVideoCodec)
        {
            auto tryAv1hw = avVideoCodec->id == AVCodecID::AV_CODEC_ID_AV1 && std::string(avVideoCodec->name) != "av1" && config->Video().VideoDecoderMode() == VideoDecoderMode::Automatic;
            auto libdav1d = tryAv1hw ? avVideoCodec : NULL;
            if (tryAv1hw)
            {
                avVideoCodec = avcodec_find_decoder_by_name("av1");
                if (!avVideoCodec)
                {
                    avVideoCodec = libdav1d;
                }
            }

            // allocate a new decoding context
            auto avVideoCodecCtx = avcodec_alloc_context3(avVideoCodec);
            if (!avVideoCodecCtx)
            {
                DebugMessage(L"Could not allocate a decoding context\n");
                hr = E_OUTOFMEMORY;
            }

            // create and assign HW device context, if supported and requested
            if (SUCCEEDED(hr) && config->Video().VideoDecoderMode() == VideoDecoderMode::Automatic)
            {
                int i = 0;
                while (SUCCEEDED(hr))
                {
                    auto hwConfig = avcodec_get_hw_config(avVideoCodec, i++);
                    if (hwConfig)
                    {
                        if (hwConfig->pix_fmt == AV_PIX_FMT_D3D11 && hwConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                        {
                            AVBufferRef* hwContext = NULL;
                            if (!avHardwareContext)
                            {
                                avHardwareContext = av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA);
                            }

                            if (!avHardwareContext)
                            {
                                hr = E_FAIL;
                            }

                            if (avVideoCodecCtx->codec_id == AV_CODEC_ID_VC1 || avVideoCodecCtx->codec_id == AV_CODEC_ID_WMV3)
                            {
                                // workaround for VC1 and WMV3: use default device context, later replace with actual MSS device context
                                if (!avHardwareContextDefault)
                                {
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = av_hwdevice_ctx_create(&avHardwareContextDefault, AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0);
                                    }

                                    if (SUCCEEDED(hr))
                                    {
                                        hr = av_hwdevice_ctx_init(avHardwareContextDefault);
                                    }

                                    if (FAILED(hr) && avHardwareContextDefault)
                                    {
                                        av_buffer_unref(&avHardwareContextDefault);
                                    }
                                }

                                if (SUCCEEDED(hr))
                                {
                                    hwContext = avHardwareContextDefault;
                                }
                            }
                            else
                            {
                                if (SUCCEEDED(hr))
                                {
                                    hwContext = avHardwareContext;
                                }
                            }

                            if (hwContext)
                            {
                                avVideoCodecCtx->hw_device_ctx = av_buffer_ref(hwContext);
                            }
                            else
                            {
                                hr = E_OUTOFMEMORY;
                            }
                            break;
                        }
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if (SUCCEEDED(hr))
            {
                // Bale: the d3d11va frame pool sized from the stream (D3D11DecoderPoolSize.h), opt-in.
                avVideoCodecCtx->get_format = baleD3d11vaPoolFromStream > 0 ? &D3D11DecoderPoolSize::GetFormat : &get_format;

                // initialize the stream parameters with demuxer information
                hr = avcodec_parameters_to_context(avVideoCodecCtx, avStream->codecpar);
            }

            if (SUCCEEDED(hr))
            {
                // enable multi threading only for SW decoders
                if (!avVideoCodecCtx->hw_device_ctx)
                {
                    unsigned threads = std::thread::hardware_concurrency();
                    avVideoCodecCtx->thread_count = config->Video().MaxDecoderThreads() == 0 ? threads : min((int)threads, config->Video().MaxDecoderThreads());
                    avVideoCodecCtx->thread_type = config->IsFrameGrabber ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
                }

                // Bale: optional dav1d frame delay (decoder default is about sqrt(threads)).
                AVDictionary* codecOptions = NULL;
                if (baleDav1dMaxFrameDelay > 0 && std::string(avVideoCodec->name) == "libdav1d")
                {
                    av_dict_set_int(&codecOptions, "max_frame_delay", baleDav1dMaxFrameDelay, 0);
                }
                // Bale: select before open, since libdav1d reads apply_grain at initialization.
                bool dav1d = std::string(avVideoCodec->name) == "libdav1d";
                bool hardware = avVideoCodecCtx->hw_device_ctx != nullptr;
                // Software libdav1d with the D3D11 upload and zero-copy requested: zero-copy only, no
                // CPU output path. Frame grabbers and upload-off keep the upstream path.
                bool zeroCopyRoute = dav1d && !hardware && baleD3d11Upload > 0 && baleZeroCopy > 0 && !config->IsFrameGrabber;
                // Same inputs as the provider's ZeroCopy (set in CreateVideoSampleProvider).
                eightBitZeroCopy = UncompressedVideoSampleProvider::EightBitZeroCopy(avVideoCodecCtx->pix_fmt, dav1d,
                    hardware, zeroCopyRoute ? 1 : 0, config.as<winrt::FFmpegInteropX::MediaSourceConfig>());
                auto outputFormat = UncompressedVideoSampleProvider::SelectOutputPixelFormat(avVideoCodecCtx->pix_fmt,
                    (avVideoCodec->capabilities & AV_CODEC_CAP_DR1) && !hardware,
                    config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), eightBitZeroCopy);
                zeroCopyStream = zeroCopyRoute &&
                    UncompressedVideoSampleProvider::ZeroCopyEligible(avVideoCodecCtx->pix_fmt, outputFormat, eightBitZeroCopy);
                // Mode 3: GPU grain on the staged D3D11 upload, for streams that are not zero-copy; on
                // the zero-copy route it means GPU grain there (2).
                bool stagedGrainRoute = baleFilmGrainMode == 3 && baleDav1dExportFilmGrain == 0 && !zeroCopyRoute &&
                    dav1d && !hardware && !config->IsFrameGrabber;
                filmGrainMode = stagedGrainRoute
                    ? BaleFilmGrain::SelectD3D11Mode(avVideoCodecCtx->pix_fmt, outputFormat)
                    : BaleFilmGrain::SelectMode((std::min)(baleFilmGrainMode, 2), baleDav1dExportFilmGrain > 0,
                        dav1d, hardware, avVideoCodecCtx->pix_fmt, outputFormat, zeroCopyStream, eightBitZeroCopy);
                // Bale: refuse the stream at open (no provider, so the app replans to a server
                // transcode) when zero-copy cannot take it or requested GPU grain is unavailable.
                if (zeroCopyRoute)
                {
                    const char* refusal = nullptr;
                    if (!BaleZeroCopyExports::Get().create) refusal = "bale_gpu zero-copy ABI 1 not found";
                    else if (!zeroCopyStream) refusal = "pixel format not zero-copy eligible";
                    else if (baleFilmGrainMode > 0 && baleDav1dExportFilmGrain == 0 && filmGrainMode != 2) refusal = "GPU film grain unavailable";
                    if (refusal)
                    {
                        av_log(nullptr, AV_LOG_WARNING, "Bale zero-copy: refusing video stream (%s, format %d).\n",
                            refusal, static_cast<int>(avVideoCodecCtx->pix_fmt));
                        baleVideoRefused = true;
                        hr = E_FAIL;
                    }
                }
                // Export-only is independent and always wins. Otherwise keep dav1d grain unless
                // Rust's loader and both input/output formats have passed the pre-open checks.
                if (SUCCEEDED(hr) && dav1d && (baleDav1dExportFilmGrain > 0 || filmGrainMode > 0))
                {
                    avVideoCodecCtx->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
                }
                if (SUCCEEDED(hr))
                {
                    // Bale: dav1d spawns its workers inside avcodec_open2.
                    BaleThreadBoost::Window boost(std::string(avVideoCodec->name) == "libdav1d" ? baleDecodeThreadPriority : 0);
                    hr = avcodec_open2(avVideoCodecCtx, avVideoCodec, &codecOptions);
                }
                av_dict_free(&codecOptions);
            }

            if (SUCCEEDED(hr))
            {
                // Detect video format and create video stream descriptor accordingly
                result = CreateVideoSampleProvider(avStream, avVideoCodecCtx, index, filmGrainMode, eightBitZeroCopy, zeroCopyStream);
            }

            // free codec context if failed
            if (!result && avVideoCodecCtx)
            {
                avcodec_free_context(&avVideoCodecCtx);
            }
        }

        return result;
    }



    // Bale: an AV1 stream on GpuDecoderSampleProvider with rav1g, or null (logged as a fallback to
    // dav1d) when rav1g cannot take it. The codec context is only a description: no FFmpeg decoder
    // is opened.
    std::shared_ptr<MediaSampleProvider> FFmpegMediaSource::CreateGpuDecoderStream(AVStream* avStream, int index)
    {
        int bitDepth = 0;
        auto rav1gDecoder = BaleRav1gDecoder::TryOpen(avStream->codecpar, bitDepth);
        if (rav1gDecoder)
        {
            rav1gDecoder->parseFrames = static_cast<uint32_t>(baleRav1gParse);
            if (baleRav1gPerf > 0) rav1gDecoder->EnablePerfStatistics(baleRav1gPerf >= 2);
        }
        std::unique_ptr<GpuVideoDecoder> gpuDecoder = std::move(rav1gDecoder);
        if (!gpuDecoder)
        {
            return nullptr;
        }

        auto avCodecCtx = avcodec_alloc_context3(avcodec_find_decoder(avStream->codecpar->codec_id));
        if (!avCodecCtx || avcodec_parameters_to_context(avCodecCtx, avStream->codecpar) < 0)
        {
            avcodec_free_context(&avCodecCtx);
            BaleRav1gDecoder::Fallback("codec context", E_OUTOFMEMORY);
            return nullptr;
        }

        // The stage (tone map/downscale) as on the D3D11 path: HDR with bale_tonemap, SDR larger than
        // the target size. Its eligibility needs the decoder's depth, which the container may omit.
        std::unique_ptr<VideoFrameProcessor> processor;
        auto parameters = avcodec_parameters_alloc();
        if (parameters && avcodec_parameters_copy(parameters, avStream->codecpar) >= 0)
        {
            parameters->format = bitDepth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
            int curve = baleToneMap;
            bool larger = baleToneMapOutputWidth > 0 && baleToneMapOutputHeight > 0 &&
                (parameters->width > baleToneMapOutputWidth || parameters->height > baleToneMapOutputHeight);
            if (curve == 0 && larger && !BaleToneMap::IsSupportedSource(parameters))
            {
                // SDR larger than the display is downscaled by the stage, which applies no curve to it.
                curve = 1;
            }
            processor = BaleToneMap::Create(curve, parameters, baleToneMapOutputWidth, baleToneMapOutputHeight, baleToneMapWhiteNits);
        }
        avcodec_parameters_free(&parameters);

        auto provider = std::make_shared<GpuDecoderSampleProvider>(m_pReader, avFormatCtx, avCodecCtx,
            config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, useHdr, std::move(gpuDecoder), bitDepth,
            std::move(processor), baleToneMapOutputWidth, baleToneMapOutputHeight, baleToneMapOutputDepth);
        if (baleGpuOutputImages > 0) provider->SetOutputImages(static_cast<uint32_t>(baleGpuOutputImages));
        if (baleGpuDecodeInflight > 0) provider->SetDecodeInflight(static_cast<uint32_t>(baleGpuDecodeInflight));
        if (baleGpuOutputTextures > 0) provider->SetOutputTextureLimit(static_cast<uint32_t>(baleGpuOutputTextures));
        if (baleDecodeThreadPriority > 0) provider->SetFeederThreadPriority(BaleThreadBoost::ToWin32(baleDecodeThreadPriority));
        if (baleRav1gPerf >= 2) provider->SetPerFrameLog(true);
        // Bale: rav1g's display pass (interface version 2) does the stage's work for BT.2390 (curve 1);
        // Hable (2) keeps the stage.
        if (baleRav1gDisplay > 0 && baleToneMap != 2 &&
            provider->UseDecoderDisplay(baleToneMapWhiteNits > 0 ? static_cast<float>((std::max)(80, (std::min)(baleToneMapWhiteNits, 400))) : 100.0f,
                baleRav1gDisplayFixedPeak > 0, baleRav1gDisplayBilinearChroma > 0))
        {
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: display pass requested\n");
        }
        auto hr = provider->Initialize();
        if (FAILED(hr))
        {
            BaleRav1gDecoder::Fallback("stream descriptor", hr);
            return nullptr;
        }
        gpuDecoderPools.push_back(provider->OutputPool());
        return provider;
    }

    void FFmpegMediaSource::CancelGpuDecoderWaits()
    {
        for (auto& pool : gpuDecoderPools)
        {
            pool->RequestCancel();
        }
    }

    // Called with `mutex` held, from AheadPause/AheadResume: GPU decoder streams read packets on their
    // own feeder threads, which pause whenever the decode-ahead worker does.
    void FFmpegMediaSource::PauseGpuDecoderFeeders(bool pause)
    {
        for (auto& stream : videoStreams)
        {
            if (auto gpuStream = std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream))
            {
                if (pause)
                {
                    gpuStream->PauseFeeder();
                }
                else
                {
                    gpuStream->ResumeFeeder();
                }
            }
        }
    }

    void FFmpegMediaSource::StopGpuDecoderFeeders()
    {
        for (auto& stream : videoStreams)
        {
            if (auto gpuStream = std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream)) gpuStream->StopFeeder();
        }
    }

    bool FFmpegMediaSource::HasGpuDecoderStream()
    {
        for (auto& stream : videoStreams)
        {
            if (std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream)) return true;
        }
        return false;
    }

    void FFmpegMediaSource::InitializeGpuDecoderDevice(MediaStreamSource const& sender)
    {
        HRESULT hr = S_OK;
        com_ptr<ID3D11Device> rendererDevice;
        com_ptr<ID3D11DeviceContext> rendererContext;
        com_ptr<ID3D11VideoDevice> videoDevice;
        try
        {
            winrt::check_hresult(DirectXInteropHelper::GetDeviceManagerFromStreamSource(sender, gpuDecoderDeviceManager));
            if (!gpuDecoderDeviceManager) winrt::throw_hresult(E_NOINTERFACE);
            winrt::check_hresult(DirectXInteropHelper::GetDeviceFromStreamSource(gpuDecoderDeviceManager,
                rendererDevice, rendererContext, videoDevice, &gpuDecoderDeviceHandle));
        }
        catch (...)
        {
            hr = winrt::to_hresult();
        }

        for (auto& stream : videoStreams)
        {
            auto gpuStream = std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream);
            if (!gpuStream) continue;
            if (FAILED(hr))
            {
                gpuStream->DeviceLost(hr);
            }
            else
            {
                gpuStream->SetRendererDevice(rendererDevice, rendererContext);
            }
        }
        if (FAILED(hr))
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale video: GPU decoder device unavailable (hr=0x%08X)\n", static_cast<unsigned>(hr));
            CloseGpuDecoderDevice();
        }
    }

    void FFmpegMediaSource::CloseGpuDecoderDevice()
    {
        if (gpuDecoderDeviceHandle && gpuDecoderDeviceManager)
        {
            gpuDecoderDeviceManager->CloseDeviceHandle(gpuDecoderDeviceHandle);
        }
        gpuDecoderDeviceHandle = nullptr;
        gpuDecoderDeviceManager = nullptr;
    }

    void FFmpegMediaSource::SetSubtitleDelay(TimeSpan const& delay)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        try
        {
            for (auto& subtitleStream : subtitleStreamProviders)
            {
                subtitleStream->SetStreamDelay(delay);
            }
        }
        catch (...)
        {
        }
    }

    void FFmpegMediaSource::SetFFmpegAudioFilters(hstring const& audioFilters)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            OutputDebugString(L"\n SetFFmpegAudioFilters failed");
            return;
        }
        for (auto audioStream : audioStreams)
        {
            audioStream->SetFFmpegFilters(audioFilters);
        }
    }

    void FFmpegMediaSource::SetFFmpegAudioFilters(hstring const& audioFilters, winrt::FFmpegInteropX::AudioStreamInfo const& audioStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        for (auto internalAudioStream : audioStreams)
        {
            if (internalAudioStream->AudioInfo() == audioStream)
            {
                internalAudioStream->SetFFmpegFilters(audioFilters);
                break;
            }
        }
    }

    void FFmpegMediaSource::SetFFmpegVideoFilters(hstring const& videoFilters)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        AheadPauseScope aheadPause(this, false);

        for (auto videoStream : videoStreams)
        {
            videoStream->SetFFmpegFilters(videoFilters);
        }
    }

    void FFmpegMediaSource::SetFFmpegVideoFilters(hstring const& videoFilters, winrt::FFmpegInteropX::VideoStreamInfo const& videoStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        AheadPauseScope aheadPause(this, false);

        for (auto internalvideoStream : videoStreams)
        {
            if (internalvideoStream->VideoInfo() == videoStream)
            {
                internalvideoStream->SetFFmpegFilters(videoFilters);
                break;
            }
        }
    }

    void FFmpegMediaSource::ClearFFmpegAudioFilters()
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        for (auto audioStream : audioStreams)
        {
            audioStream->ClearFFmpegFilters();
        }
    }

    void FFmpegMediaSource::ClearFFmpegAudioFilters(winrt::FFmpegInteropX::AudioStreamInfo const& audioStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        for (auto internaAudioStream : audioStreams)
        {
            if (internaAudioStream->AudioInfo() == audioStream)
            {
                internaAudioStream->ClearFFmpegFilters();
                break;
            }
        }
    }

    void FFmpegMediaSource::ClearFFmpegVideoFilters()
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        AheadPauseScope aheadPause(this, false);
        for (auto videoStream : videoStreams)
        {
            videoStream->ClearFFmpegFilters();
        }
    }

    void FFmpegMediaSource::ClearFFmpegVideoFilters(winrt::FFmpegInteropX::VideoStreamInfo const& videoStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        AheadPauseScope aheadPause(this, false);

        for (auto internaVideoStream : videoStreams)
        {
            if (internaVideoStream->VideoInfo() == videoStream)
            {
                internaVideoStream->ClearFFmpegFilters();
                break;
            }
        }
    }

    hstring FFmpegMediaSource::GetFFmpegAudioFilters(winrt::FFmpegInteropX::AudioStreamInfo const& audioStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return hstring{};
        }

        for (int i = 0; i < audioStreams.size(); i++)
        {
            if (audioStreams.at(i)->AudioInfo() == audioStream)
            {
                return audioStreams.at(i)->GetFFmpegFilters();
            }
        }
        return hstring{};
    }

    hstring FFmpegMediaSource::GetFFmpegVideoFilters(winrt::FFmpegInteropX::VideoStreamInfo const& videoStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return hstring{};
        }
        for (int i = 0; i < videoStreams.size(); i++)
        {
            if (videoStreams.at(i)->VideoInfo() == videoStream)
            {
                return videoStreams.at(i)->GetFFmpegFilters();
            }
        }

        return hstring{};
    }

    ///<summary>Sends a command to audio filters on all enabled audio streams.</summary>
    ///<returns>The result from the filter command. If multiple audio streams are enabled, the result from the first stream that returns a non-error result will be returned.</returns>
    FFmpegInteropX::FilterCommandResult FFmpegMediaSource::SendFFmpegAudioFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return FFmpegInteropX::FilterCommandResult(false, L"Closed");
        }
        bool hasResult = false;
        auto result = FFmpegInteropX::FilterCommandResult(false, audioStreams.size() > 0 ? L"No enabled streams" : L"No streams");
        for (int i = 0; i < audioStreams.size(); i++)
        {
            if (audioStreams.at(i)->IsEnabled())
            {
                auto res = audioStreams.at(i)->SendFFmpegFilterCommand(target, command, arguments);
                if (!hasResult || (res.Succeeded() && !result.Succeeded()))
                {
                    result = res;
                    hasResult = true;
                }
            }
        }
        return result;
    }

    ///<summary>Sends a command to audio filters on the specified audio stream.</summary>
    ///<returns>The result from the filter command.</returns>
    FFmpegInteropX::FilterCommandResult FFmpegMediaSource::SendFFmpegAudioFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments, winrt::FFmpegInteropX::AudioStreamInfo const& audioStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return FFmpegInteropX::FilterCommandResult(false, L"Closed");
        }
        for (int i = 0; i < audioStreams.size(); i++)
        {
            if (audioStreams.at(i)->AudioInfo() == audioStream)
            {
                if (audioStreams.at(i)->IsEnabled())
                {
                    return audioStreams.at(i)->SendFFmpegFilterCommand(target, command, arguments);
                }
                else
                {
                    return FFmpegInteropX::FilterCommandResult(false, L"Stream disabled");
                }
            }
        }
        return FFmpegInteropX::FilterCommandResult(false, L"Stream not found");
    }

    ///<summary>Sends a command to video filters on all enabled video streams.</summary>
    ///<returns>The result from the filter command. If multiple video streams are enabled, the result from the first stream that returns a non-error result will be returned.</returns>
    FFmpegInteropX::FilterCommandResult FFmpegMediaSource::SendFFmpegVideoFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return FFmpegInteropX::FilterCommandResult(false, L"Closed");
        }
        AheadPauseScope aheadPause(this, false);
        bool hasResult = false;
        auto result = FFmpegInteropX::FilterCommandResult(false, videoStreams.size() > 0 ? L"No enabled streams" : L"No streams");
        for (int i = 0; i < videoStreams.size(); i++)
        {
            if (videoStreams.at(i)->IsEnabled())
            {
                auto res = videoStreams.at(i)->SendFFmpegFilterCommand(target, command, arguments);
                if (!hasResult || (res.Succeeded() && !result.Succeeded()))
                {
                    result = res;
                    hasResult = true;
                }
            }
        }
        return result;
    }

    ///<summary>Sends a command to video filters on the specified video stream.</summary>
    ///<returns>The result from the filter command.</returns>
    FFmpegInteropX::FilterCommandResult FFmpegMediaSource::SendFFmpegVideoFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments, winrt::FFmpegInteropX::VideoStreamInfo const& videoStream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return FFmpegInteropX::FilterCommandResult(false, L"Closed");
        }
        AheadPauseScope aheadPause(this, false);
        for (int i = 0; i < videoStreams.size(); i++)
        {
            if (videoStreams.at(i)->VideoInfo() == videoStream)
            {
                if (videoStreams.at(i)->IsEnabled())
                {
                    return videoStreams.at(i)->SendFFmpegFilterCommand(target, command, arguments);
                }
                else
                {
                    return FFmpegInteropX::FilterCommandResult(false, L"Stream disabled");
                }
            }
        }
        return FFmpegInteropX::FilterCommandResult(false, L"Stream not found");
    }


    FFmpegInteropX::MediaThumbnailData FFmpegMediaSource::ExtractThumbnail()
    {
        std::lock_guard lock(mutex);
        return thumbnailData;
    }

    FFmpegInteropX::MediaThumbnailData FFmpegMediaSource::ExtractThumbnail(AVStream* imageStream)
    {
        if (isClosed || !imageStream->attached_pic.data || !imageStream->attached_pic.buf || !imageStream->attached_pic.size)
        {
            return nullptr;
        }

        hstring extension = L".jpg";
        switch (imageStream->codecpar->codec_id)
        {
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_MJPEGB:
        case AV_CODEC_ID_JPEG2000:
        case AV_CODEC_ID_JPEGLS: extension = L".jpg"; break;
        case AV_CODEC_ID_PNG: extension = L".png"; break;
        case AV_CODEC_ID_BMP: extension = L".bmp"; break;
        }

        auto bufferRef = av_buffer_ref(imageStream->attached_pic.buf);
        if (bufferRef)
        {
            auto buffer = NativeBuffer::NativeBufferFactory::CreateNativeBuffer(bufferRef->data, (UINT32)imageStream->attached_pic.size, free_buffer, bufferRef);
            return MediaThumbnailData(buffer, extension);
        }

        return nullptr;
    }

    Windows::Media::Core::MediaStreamSource FFmpegMediaSource::GetMediaStreamSource()
    {
        std::lock_guard lock(mutex);
        if (this->config->IsFrameGrabber) throw_hresult(E_UNEXPECTED);
        return mssWeak.get();
    }

    MediaSource FFmpegMediaSource::CreateMediaSource()
    {
        for (auto& stream : sampleProviders)
        {
            if (stream)
            {
                stream->NotifyCreateSource();
            }
        }

        if (this->config->IsFrameGrabber) throw_hresult(E_UNEXPECTED);

        auto mss = CreateMediaStreamSource();

        MediaSource source = MediaSource::CreateFromMediaStreamSource(mss);
        for (auto& stream : subtitleStreamProviders)
        {
            source.ExternalTimedMetadataTracks().Append(stream->SubtitleTrack);
        }
        for (auto subtitleInfo : SubtitleStreams())
        {
            if (subtitleInfo.IsExternal()) {
                source.ExternalTimedMetadataTracks().Append(subtitleInfo.SubtitleTrack());
            }
        }

        if (chapterInfos.Size() > 0)
        {
            auto track = TimedMetadataTrack(L"Chapters", L"", TimedMetadataKind::Chapter);
            for (auto chapter : chapterInfos)
            {
                auto cue = ChapterCue();
                cue.Title(chapter.Title());
                cue.StartTime(chapter.StartTime());
                cue.Duration(chapter.Duration());
                track.AddCue(cue);
            }
            source.ExternalTimedMetadataTracks().Append(track);
        }

        return source;
    }

    MediaPlaybackItem FFmpegMediaSource::CreateMediaPlaybackItem()
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            throw_hresult(RO_E_CLOSED);
        }

        if (this->config->IsFrameGrabber || playbackItemWeak.get() != nullptr) throw_hresult(E_UNEXPECTED);
        auto playbackItem = MediaPlaybackItem(CreateMediaSource());
        InitializePlaybackItem(playbackItem);
        return playbackItem;
    }

    MediaPlaybackItem FFmpegMediaSource::CreateMediaPlaybackItem(TimeSpan const& startTime)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            throw_hresult(RO_E_CLOSED);
        }

        if (this->config->IsFrameGrabber || playbackItemWeak.get() != nullptr) throw_hresult(E_UNEXPECTED);
        auto playbackItem = MediaPlaybackItem(CreateMediaSource(), startTime);
        InitializePlaybackItem(playbackItem);
        return playbackItem;
    }

    MediaPlaybackItem FFmpegMediaSource::CreateMediaPlaybackItem(TimeSpan const& startTime, TimeSpan const& durationLimit)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            throw_hresult(RO_E_CLOSED);
        }

        if (this->config->IsFrameGrabber || playbackItemWeak.get() != nullptr) throw_hresult(E_UNEXPECTED);
        auto playbackItem = MediaPlaybackItem(CreateMediaSource(), startTime, durationLimit);
        InitializePlaybackItem(playbackItem);
        return playbackItem;
    }

    IAsyncAction FFmpegMediaSource::OpenWithMediaPlayerAsync(MediaPlayer mediaPlayer)
    {
        auto playbackItem = playbackItemWeak.get();
        if (!playbackItem)
        {
            playbackItem = CreateMediaPlaybackItem();
        }

        // Does not seem to work on Windows 10?!
        //auto mediaSource = playbackItem.Source();
        //if (!mediaSource.IsOpen())
        //{
        //    co_await mediaSource.OpenAsync();
        //}

        task_completion_event<bool> tce;

        auto openedToken = mediaPlayer.MediaOpened([tce](MediaPlayer const&, IInspectable const&) { tce.set(true); });
        auto failedToken = mediaPlayer.MediaFailed([tce](MediaPlayer const&, MediaPlayerFailedEventArgs const&) { tce.set(false); });

        mediaPlayer.Source(playbackItem);
        auto currentPlaybackItemWeak = this->playbackItemWeak;

        auto result = co_await task<bool>(tce);

        mediaPlayer.MediaOpened(openedToken);
        mediaPlayer.MediaFailed(failedToken);

        auto source = mediaPlayer.Source();
        if (!result || source != playbackItem)
        {
            // we were disposed already
            playbackItem.Source().Close();
        }
        else
        {
            // register for soruce changed event
            auto tokenPtr = new event_token[1]();
            tokenPtr[0] = mediaPlayer.SourceChanged([tokenPtr, currentPlaybackItemWeak](MediaPlayer const& mediaPlayer, IInspectable const&)
                {
                    auto playbackItem = currentPlaybackItemWeak.get();
                    if (!playbackItem)
                    {
                        // we were disposed already
                        mediaPlayer.SourceChanged(tokenPtr[0]);
                        delete[] tokenPtr;
                    }
                    else
                    {
                        auto source = mediaPlayer.Source();
                        if (source != playbackItem)
                        {
                            // source has changed. close now.
                            playbackItem.Source().Close();
                            mediaPlayer.SourceChanged(tokenPtr[0]);
                            delete[] tokenPtr;
                        }
                    }
                });
        }
    }

    IAsyncOperation<winrt::FFmpegInteropX::FFmpegMediaSource> FFmpegMediaSource::ReadExternalSubtitleStreamAsync(IRandomAccessStream stream,
        hstring streamName,
        winrt::FFmpegInteropX::MediaSourceConfig const& config,
        VideoStreamDescriptor videoDescriptor,
        uint64_t windowId,
        bool useHdr)
    {
        winrt::apartment_context caller; // Capture calling context.
        co_await winrt::resume_background();

        auto cancellation = co_await get_cancellation_token();
        auto subConfig(winrt::make_self<MediaSourceConfig>());
        subConfig->IsExternalSubtitleParser = true;
        subConfig->Subtitles().DefaultStreamName(streamName);
        subConfig->Subtitles().DefaultSubtitleDelay(config.Subtitles().DefaultSubtitleDelay());
        subConfig->Subtitles().ExternalSubtitleEncoding(config.Subtitles().ExternalSubtitleEncoding());
        subConfig->Subtitles().ExternalSubtitleAnsiEncoding(config.Subtitles().ExternalSubtitleAnsiEncoding());
        subConfig->Subtitles().OverrideSubtitleStyles(config.Subtitles().OverrideSubtitleStyles());
        subConfig->Subtitles().SubtitleRegion(config.Subtitles().SubtitleRegion());
        subConfig->Subtitles().SubtitleStyle(config.Subtitles().SubtitleStyle());
        subConfig->Subtitles().AutoSelectForcedSubtitles(false);
        subConfig->Subtitles().MinimumSubtitleDuration(config.Subtitles().MinimumSubtitleDuration());
        subConfig->Subtitles().AdditionalSubtitleDuration(config.Subtitles().AdditionalSubtitleDuration());
        subConfig->Subtitles().PreventModifiedSubtitleDurationOverlap(config.Subtitles().PreventModifiedSubtitleDurationOverlap());

        if (videoDescriptor)
        {
            subConfig->AdditionalFFmpegSubtitleOptions = PropertySet();

            subConfig->AdditionalFFmpegSubtitleOptions.Insert(L"subfps",
                winrt::box_value(winrt::to_hstring(videoDescriptor.EncodingProperties().FrameRate().Numerator()) + L"/" + winrt::to_hstring(videoDescriptor.EncodingProperties().FrameRate().Denominator())));
        }
        auto externalSubsParser = FFmpegMediaSource::CreateFromStream(stream, subConfig, windowId, useHdr);

        if (externalSubsParser->SubtitleStreams().Size() > 0)
        {
            if (videoDescriptor)
            {
                auto encodingProperties = videoDescriptor.EncodingProperties();
                auto pixelAspect = (double)encodingProperties.PixelAspectRatio().Numerator() / encodingProperties.PixelAspectRatio().Denominator();
                auto videoAspect = ((double)encodingProperties.Width() / encodingProperties.Height()) / pixelAspect;
                for (auto& subtitleStream : externalSubsParser->subtitleStreamProviders)
                {
                    subtitleStream->NotifyVideoFrameSize(encodingProperties.Width(), encodingProperties.Height(), videoAspect);
                }
            }

            int readResult = 0;
            while ((readResult = externalSubsParser->m_pReader->ReadPacket()) >= 0)
            {
                //Concurrency::interruption_point();
                if (cancellation()) co_return nullptr;
            }
        }

        co_await caller;
        co_return externalSubsParser.as<winrt::FFmpegInteropX::FFmpegMediaSource>();
    }

    IAsyncOperation<Collections::IVectorView<FFmpegInteropX::SubtitleStreamInfo>> FFmpegMediaSource::AddExternalSubtitleAsync(IRandomAccessStream stream, hstring streamName)
    {
        auto strong = get_strong();

        winrt::apartment_context caller; // Capture calling context.
        co_await winrt::resume_background();
        auto videoDescriptor = currentVideoStream ? (currentVideoStream->StreamDescriptor()).as<VideoStreamDescriptor>() : nullptr;

        auto externalSubsParser = (co_await ReadExternalSubtitleStreamAsync(stream, streamName, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), videoDescriptor, windowId, useHdr)).as<winrt::FFmpegInteropX::implementation::FFmpegMediaSource>();

        Collections::IVectorView<FFmpegInteropX::SubtitleStreamInfo> result;
        {
            std::lock_guard lock(mutex);
            if (isClosed)
            {
                throw_hresult(RO_E_CLOSED);
            }
            if (config->Subtitles().DefaultSubtitleDelay().count() != externalSubsParser->config->Subtitles().DefaultSubtitleDelay().count())
            {
                //this should never happen?
                externalSubsParser->SetSubtitleDelay(config->Subtitles().DefaultSubtitleDelay());
            }

            int subtitleTracksCount = 0;

            for (auto& externalSubtitle : externalSubsParser->subtitleStreamProviders)
            {
                if (externalSubtitle->SubtitleTrack.Cues().Size() > 0)
                {
                    // detach stream
                    externalSubtitle->Detach();

                    // find and add stream info

                    subtitleStrInfos.Append(externalSubtitle->SubtitleInfo());
                    // add stream
                    if (!isClosed)
                    {
                        subtitleStreamProviders.push_back(externalSubtitle);
                    }
                    if (auto playbackItem = playbackItemWeak.get())
                    {
                        playbackItem.Source().ExternalTimedMetadataTracks().Append(externalSubtitle->SubtitleTrack);
                    }
                    subtitleTracksCount++;
                }
            }

            if (subtitleTracksCount == 0)
            {
                throw_hresult(E_INVALIDARG);
            }

            subtitleStreamInfos = subtitleStrInfos.GetView();
            result = externalSubsParser->SubtitleStreams();
        }

        co_await caller;
        co_return result;
    }

    IAsyncOperation<Collections::IVectorView<FFmpegInteropX::SubtitleStreamInfo>> FFmpegMediaSource::AddExternalSubtitleAsync(IRandomAccessStream stream)
    {
        return AddExternalSubtitleAsync(stream, config->Subtitles().DefaultExternalSubtitleStreamName());
    }

    void FFmpegMediaSource::StartBuffering()
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        m_pReader->Start();
    }

    FFmpegInteropX::MediaSourceConfig FFmpegMediaSource::Configuration()
    {
        return config.as<winrt::FFmpegInteropX::MediaSourceConfig>();
    }

    Collections::IMapView<hstring, Collections::IVectorView<hstring>> FFmpegMediaSource::MetadataTags()
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return metadata->MetadataTags();
        }

        metadata->LoadMetadataTags(avFormatCtx);
        return metadata->MetadataTags();
    }

    TimeSpan FFmpegMediaSource::Duration()
    {
        return mediaDuration;
    }

    FFmpegInteropX::VideoStreamInfo FFmpegMediaSource::CurrentVideoStream()
    {
        std::lock_guard lock(mutex);
        return currentVideoStreamInfo;
    }

    FFmpegInteropX::AudioStreamInfo FFmpegMediaSource::CurrentAudioStream()
    {
        std::lock_guard lock(mutex);
        return currentAudioStreamInfo;
    }

    Collections::IVectorView<FFmpegInteropX::VideoStreamInfo> FFmpegMediaSource::VideoStreams()
    {
        return videoStreamInfos;
    }

    Collections::IVectorView<FFmpegInteropX::AudioStreamInfo> FFmpegMediaSource::AudioStreams()
    {
        return audioStreamInfos;
    }

    Collections::IVectorView<FFmpegInteropX::SubtitleStreamInfo> FFmpegMediaSource::SubtitleStreams()
    {
        return subtitleStreamInfos;
    }

    Collections::IVectorView<FFmpegInteropX::ChapterInfo> FFmpegMediaSource::ChapterInfos()
    {
        return chapterInfos;
    }

    FFmpegInteropX::FormatInfo FFmpegMediaSource::FormatInfo()
    {
        return formatInfo;
    }

    bool FFmpegMediaSource::HasThumbnail()
    {
        return thumbnailData != nullptr;
    }

    MediaPlaybackItem FFmpegMediaSource::PlaybackItem()
    {
        return playbackItemWeak.get();
    }

    void FFmpegMediaSource::SetStreamDelay(FFmpegInteropX::IStreamInfo const& stream, TimeSpan const& delay)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        AheadPauseScope aheadPause(this, false);
        for (auto provider : sampleProviders)
        {
            if (provider->StreamInfo() == stream)
            {
                provider->SetStreamDelay(delay);
                return;
            }
        }
    }

    TimeSpan FFmpegMediaSource::GetStreamDelay(FFmpegInteropX::IStreamInfo const& stream)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return TimeSpan{ 0L };
        }
        for (auto provider : sampleProviders)
        {
            if (provider->StreamInfo() == stream)
            {
                return provider->GetStreamDelay();
            }
        }

        return TimeSpan{ 0L };
    }

    MediaPlaybackSession FFmpegMediaSource::PlaybackSession()
    {
        return sessionWeak.get();
    }

    void FFmpegMediaSource::PlaybackSession(MediaPlaybackSession const& value)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        if (auto strong = sessionWeak.get())
        {
            strong.PositionChanged(sessionPositionEvent);
        }
        sessionWeak = value;
        if (value)
        {
            sessionPositionEvent = value.PositionChanged({ get_weak(), &FFmpegInteropX::implementation::FFmpegMediaSource::OnPositionChanged });
        }
    }

    void FFmpegMediaSource::Close()
    {
        Close(false);
    }

    void FFmpegMediaSource::Close(bool onMediaSourceClosed)
    {
        isShuttingDown = true;
        // Bale: a GPU decoder waiting for an output image may hold `mutex` on the sample thread.
        CancelGpuDecoderWaits();
        std::lock_guard lock(mutex);


        if (onMediaSourceClosed)
        {
            if (config->General().KeepMetadataOnMediaSourceClosed())
            {
                // Save metadata before closing
                if (avFormatCtx)
                {
                    metadata->LoadMetadataTags(avFormatCtx);
                }
            }
            else
            {
                metadata->Clear();
                thumbnailData = nullptr;
            }
        }

        if (auto mss = mssWeak.get())
        {
            mss.Starting(startingRequestedToken);
            mss.SampleRequested(sampleRequestedToken);
            mss.SwitchStreamsRequested(switchStreamRequestedToken);
            mss.Closed(closeToken);
            mssWeak = nullptr;
        }

        if (auto playbackItem = playbackItemWeak.get())
        {
            playbackItem.AudioTracksChanged(audioTracksChangedToken);
            playbackItem.TimedMetadataTracks().PresentationModeChanged(subtitlePresentationModeChangedToken);
            playbackItemWeak = nullptr;
        }

        PlaybackSession(nullptr);

        // Bale: the decode-ahead worker and the GPU decoder feeders use the streams and reader cleared
        // below.
        AheadShutdown();
        StopGpuDecoderFeeders();

        // Clear our data
        currentAudioStream = nullptr;
        currentVideoStream = nullptr;

        if (m_pReader != nullptr)
        {
            m_pReader->Stop();
            m_pReader->Flush();
            m_pReader = nullptr;
        }

        subtitleStreamProviders.clear();
        sampleProviders.clear();
        audioStreams.clear();
        videoStreams.clear();

        if (avFormatCtx)
        {
            avformat_close_input(&avFormatCtx);
        }

        if (avIOCtx)
        {
            avIOCtx->opaque = NULL;
            avio_closep(&avIOCtx);
        }

        if (avDict)
        {
            av_dict_free(&avDict);
        }

        if (avHardwareContext)
        {
            av_buffer_unref(&avHardwareContext);
        }
        if (avHardwareContextDefault)
        {
            av_buffer_unref(&avHardwareContextDefault);
        }

        if (deviceHandle && deviceManager)
        {
            deviceManager->CloseDeviceHandle(deviceHandle);
        }
        if (!isClosed) av_log(nullptr, AV_LOG_INFO, "Bale source: closed\n");

        device = nullptr;
        deviceContext = nullptr;
        deviceManager = nullptr;
        CloseUploadDevice();
        CloseGpuDecoderDevice();

        isClosed = true;
    }


    std::shared_ptr<MediaSampleProvider> FFmpegMediaSource::CreateAudioSampleProvider(AVStream* avStream, AVCodecContext* avAudioCodecCtx, int index)
    {
        UNREFERENCED_PARAMETER(avStream);
        std::shared_ptr<MediaSampleProvider> audioSampleProvider = nullptr;
        if (avAudioCodecCtx->codec_id == AV_CODEC_ID_AAC && config->Audio().SystemDecoderAAC())
        {
            AudioEncodingProperties encodingProperties;
            if (avAudioCodecCtx->extradata_size == 0)
            {
                encodingProperties = AudioEncodingProperties::CreateAacAdts(avAudioCodecCtx->sample_rate, avAudioCodecCtx->ch_layout.nb_channels, (unsigned int)avAudioCodecCtx->bit_rate);
            }
            else
            {
                encodingProperties = AudioEncodingProperties::CreateAac(avAudioCodecCtx->profile == AV_PROFILE_AAC_HE || avAudioCodecCtx->profile == AV_PROFILE_AAC_HE_V2 ? avAudioCodecCtx->sample_rate / 2 : avAudioCodecCtx->sample_rate, avAudioCodecCtx->ch_layout.nb_channels, (unsigned int)avAudioCodecCtx->bit_rate);
            }
            audioSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avAudioCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, encodingProperties, HardwareDecoderStatus::Unknown));
        }
        else if (avAudioCodecCtx->codec_id == AV_CODEC_ID_MP3 && config->Audio().SystemDecoderMP3())
        {
            AudioEncodingProperties encodingProperties = AudioEncodingProperties::CreateMp3(avAudioCodecCtx->sample_rate, avAudioCodecCtx->ch_layout.nb_channels, (unsigned int)avAudioCodecCtx->bit_rate);
            audioSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avAudioCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, encodingProperties, HardwareDecoderStatus::Unknown));
        }
        else if ((avAudioCodecCtx->codec_id == AV_CODEC_ID_AC3 || avAudioCodecCtx->codec_id == AV_CODEC_ID_EAC3) && baleSystemDecoderDolby > 0)
        {
            // Bale: hand the Dolby bitstream to the system decoder untouched, like SystemDecoderAAC.
            // FFmpeg's decoder would output PCM and drop the E-AC-3 JOC (Atmos) object metadata.
            AudioEncodingProperties encodingProperties;
            encodingProperties.Subtype(avAudioCodecCtx->codec_id == AV_CODEC_ID_EAC3 ? MediaEncodingSubtypes::Eac3() : MediaEncodingSubtypes::Ac3());
            encodingProperties.SampleRate(avAudioCodecCtx->sample_rate);
            encodingProperties.ChannelCount(avAudioCodecCtx->ch_layout.nb_channels);
            if (avAudioCodecCtx->bit_rate > 0)
            {
                encodingProperties.Bitrate((unsigned int)avAudioCodecCtx->bit_rate);
            }
            audioSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avAudioCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, encodingProperties, HardwareDecoderStatus::Unknown));
        }
        else
        {
            audioSampleProvider = std::shared_ptr<MediaSampleProvider>(new UncompressedAudioSampleProvider(m_pReader, avFormatCtx, avAudioCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index));
        }

        auto hr = audioSampleProvider->Initialize();
        if (FAILED(hr))
        {
            audioSampleProvider = nullptr;
        }

        return audioSampleProvider;
    }

    bool FFmpegMediaSource::CheckUseHardwareAcceleration(AVCodecContext* avCodecCtx, HardwareAccelerationStatus const& status, HardwareDecoderStatus& hardwareDecoderStatus, int maxProfile, int maxLevel)
    {
        UNREFERENCED_PARAMETER(maxProfile);
        bool result = false;
        if (!config->IsFrameGrabber)
        {
#pragma warning (disable: 4973)

            if (config->Video().VideoDecoderMode() == VideoDecoderMode::AutomaticSystemDecoder)
            {
                result = CodecChecker::CheckUseHardwareAcceleration(status,
                    avCodecCtx->codec_id, avCodecCtx->profile, avCodecCtx->width, avCodecCtx->height);

                // check level, if restricted
                if (result && maxLevel >= 0)
                {
                    result = avCodecCtx->level <= maxLevel;
                }

                hardwareDecoderStatus = result ? HardwareDecoderStatus::Available : HardwareDecoderStatus::NotAvailable;
            }
            else if (config->Video().VideoDecoderMode() == VideoDecoderMode::ForceSystemDecoder)
            {
                result = true;
            }
            else
            {
                result = false;
            }
#pragma warning (default: 4973)

        }

        return result;
    }

    void FFmpegMediaSource::CheckUseHdr(winrt::com_ptr<MediaSourceConfig> const& config, bool checkDisplayInformation, bool& useHdr, uint64_t& windowId)
    {
        switch (config->Video().HdrSupport())
        {
        case HdrSupport::Enabled:
            useHdr = true;
            break;
        case HdrSupport::Automatic:

            if (checkDisplayInformation)
            {
                try
                {
#ifdef Win32
                    if (PlatformInfo::IsWinUI())
                    {
                        useHdr = Win32CheckHdr(windowId);
                    }
                    else
                    {
                        useHdr = UwpCheckUseHdr(windowId);
                    }
#else // UWP
                    useHdr = UwpCheckUseHdr(windowId);
#endif // Win32
                }
                catch (...)
                {
                }
            }
            break;
        default:
            break;
        }
    }

#ifdef Win32

    bool FFmpegMediaSource::Win32CheckHdr(uint64_t& windowId)
    {
        if (windowId)
        {
            Microsoft::UI::WindowId id{ windowId };
            auto displayInfo = Microsoft::Graphics::Display::DisplayInformation::CreateForWindowId(id);
            if (displayInfo)
            {
                auto colorInfo = displayInfo.GetAdvancedColorInfo();
                if (colorInfo.CurrentAdvancedColorKind() == Microsoft::Graphics::Display::DisplayAdvancedColorKind::HighDynamicRange)
                {
                    return true;
                }
            }
        }

        return false;
    }

#endif //Win32


    bool FFmpegMediaSource::UwpCheckUseHdr(uint64_t& windowId)
    {
        UNREFERENCED_PARAMETER(windowId);

        if (PlatformInfo::IsXbox())
        {
            // HdmiDisplayInformation is xbox only, DisplayInformation is non-xbox only, each being null in the other case
            auto hdmiDisplayInfo = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
            if (hdmiDisplayInfo)
            {
                auto currentDisplayMode = hdmiDisplayInfo.GetCurrentDisplayMode();
                if (currentDisplayMode && currentDisplayMode.ColorSpace() == Windows::Graphics::Display::Core::HdmiDisplayColorSpace::BT2020)
                {
                    return true;
                }
            }
        }
        else
        {
            auto displayInfo = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            if (displayInfo)
            {
                auto colorInfo = displayInfo.GetAdvancedColorInfo();
                if (colorInfo.CurrentAdvancedColorKind() == Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange)
                {
                    return true;
                }
            }
        }

        return false;
    }

    std::shared_ptr<MediaSampleProvider> FFmpegMediaSource::CreateVideoSampleProvider(AVStream* avStream, AVCodecContext* avVideoCodecCtx, int index, int filmGrainMode, bool eightBitZeroCopy, bool zeroCopyStream)
    {
        std::shared_ptr<MediaSampleProvider> videoSampleProvider = nullptr;
        winrt::FFmpegInteropX::HardwareDecoderStatus hardwareDecoderStatus = HardwareDecoderStatus::Unknown;
        bool softwareStaged = false;

#pragma warning (disable: 4973)

        if (config->Video().VideoDecoderMode() == VideoDecoderMode::AutomaticSystemDecoder)
        {
            CodecChecker::Initialize();
        }

        if (avVideoCodecCtx->codec_id == AV_CODEC_ID_H264 &&
            (CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationH264(), hardwareDecoderStatus, config->Video().SystemDecoderH264MaxProfile(), config->Video().SystemDecoderH264MaxLevel())))
        {
            auto videoProperties = VideoEncodingProperties::CreateH264();
            SetBaleDecoderProperties(videoProperties);

            // Check for H264 bitstream flavor. H.264 AVC extradata starts with 1 while non AVC one starts with 0
            if (avVideoCodecCtx->extradata != nullptr && avVideoCodecCtx->extradata_size > 0 && avVideoCodecCtx->extradata[0] == 1)
            {
                videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new H264AVCSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
            }
            else
            {
                videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new NALPacketSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
            }
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_HEVC &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationHEVC(), hardwareDecoderStatus, config->Video().SystemDecoderHEVCMaxProfile(), config->Video().SystemDecoderHEVCMaxLevel()))
        {
            auto videoProperties = VideoEncodingProperties::CreateHevc();
            SetBaleDecoderProperties(videoProperties);
            // Check for HEVC bitstream flavor.
            if (avVideoCodecCtx->extradata != nullptr && avVideoCodecCtx->extradata_size > 22 &&
                (avVideoCodecCtx->extradata[0] || avVideoCodecCtx->extradata[1] || avVideoCodecCtx->extradata[2] > 1))
            {
                videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new HEVCSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
            }
            else
            {
                videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new NALPacketSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
            }
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_WMV3 &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationWMV3(), hardwareDecoderStatus, -1, -1) &&
            avVideoCodecCtx->extradata_size > 0)
        {
            auto videoProperties = VideoEncodingProperties();
            SetBaleDecoderProperties(videoProperties);
            videoProperties.Subtype(MediaEncodingSubtypes::Wmv3());

            auto extradata = array_view(avVideoCodecCtx->extradata, avVideoCodecCtx->extradata_size);
            videoProperties.SetFormatUserData(extradata);
            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_VC1 &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationVC1(), hardwareDecoderStatus, -1, -1) &&
            avVideoCodecCtx->extradata_size > 0)
        {
            auto videoProperties = VideoEncodingProperties();
            SetBaleDecoderProperties(videoProperties);
            videoProperties.Subtype(MediaEncodingSubtypes::Wvc1());

            auto extradata = array_view(avVideoCodecCtx->extradata, avVideoCodecCtx->extradata_size);
            videoProperties.SetFormatUserData(extradata);
            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_MPEG2VIDEO &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationMPEG2(), hardwareDecoderStatus, -1, -1))
        {
            auto videoProperties = VideoEncodingProperties();
            SetBaleDecoderProperties(videoProperties);
            videoProperties.Subtype(MediaEncodingSubtypes::Mpeg2());

            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_VP9 &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationVP9(), hardwareDecoderStatus, -1, -1))
        {
            auto videoProperties = VideoEncodingProperties();
            SetBaleDecoderProperties(videoProperties);
            videoProperties.Subtype(MediaEncodingSubtypes::Vp9());

            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
        }
        else if (avVideoCodecCtx->codec_id == AV_CODEC_ID_VP8 &&
            CheckUseHardwareAcceleration(avVideoCodecCtx, CodecChecker::HardwareAccelerationVP8(), hardwareDecoderStatus, -1, -1))

        {
            auto videoProperties = VideoEncodingProperties();
            SetBaleDecoderProperties(videoProperties);
            videoProperties.Subtype(Windows::Media::Core::CodecSubtypes::VideoFormatVP80());

            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(new CompressedSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, videoProperties, hardwareDecoderStatus));
        }
        else if (avVideoCodecCtx->hw_device_ctx)
        {
            hardwareDecoderStatus = HardwareDecoderStatus::Available;
            auto processor = BaleToneMap::Create(baleToneMap, avStream->codecpar, baleToneMapOutputWidth, baleToneMapOutputHeight, baleToneMapWhiteNits);
            auto d3d11Provider = new D3D11VideoSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, hardwareDecoderStatus, useHdr, std::move(processor), baleToneMapOutputWidth, baleToneMapOutputHeight, baleToneMapOutputDepth);
            d3d11Provider->PoolFromStream = baleD3d11vaPoolFromStream > 0;
            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(d3d11Provider);
        }
        else
        {
            if (config->Video().VideoDecoderMode() == VideoDecoderMode::AutomaticSystemDecoder)
            {
                hardwareDecoderStatus = HardwareDecoderStatus::NotAvailable;
            }
            auto uncompressed = new UncompressedVideoSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config.as<winrt::FFmpegInteropX::MediaSourceConfig>(), index, hardwareDecoderStatus, useHdr);
            // Bale: the pre-open decision (CreateVideoStream); never enable zero-copy here alone.
            // Frame grabbers and upload-off never get it.
            uncompressed->ZeroCopy = zeroCopyStream ? 1 : 0;
            uncompressed->ZeroCopyEightBit = eightBitZeroCopy && zeroCopyStream;
            uncompressed->FilmGrain.eightInP010 = uncompressed->ZeroCopyEightBit;
            uncompressed->FilmGrain.mode = filmGrainMode;
            uncompressed->StagedGrain = filmGrainMode == 3 && !zeroCopyStream ? 1 : 0;
            // Bale: the stage for software frames too (tone map, downscale to the target size, dither), as
            // for d3d11va: bale_tonemap, or an SDR source larger than the target. Its input is NV12/P010.
            if (!zeroCopyStream && !config->IsFrameGrabber)
            {
                auto parameters = avcodec_parameters_alloc();
                if (parameters && avcodec_parameters_copy(parameters, avStream->codecpar) >= 0)
                {
                    auto description = av_pix_fmt_desc_get(avVideoCodecCtx->pix_fmt);
                    int depth = description && description->nb_components > 0 ? description->comp[0].depth : 8;
                    parameters->format = depth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
                    int curve = baleToneMap;
                    bool larger = baleToneMapOutputWidth > 0 && baleToneMapOutputHeight > 0 &&
                        (parameters->width > baleToneMapOutputWidth || parameters->height > baleToneMapOutputHeight);
                    if (curve == 0 && larger && !BaleToneMap::IsSupportedSource(parameters)) curve = 1;
                    if (curve > 0)
                    {
                        auto processor = BaleToneMap::Create(curve, parameters, baleToneMapOutputWidth, baleToneMapOutputHeight, baleToneMapWhiteNits);
                        softwareStaged = uncompressed->SetSoftwareStage(std::move(processor), baleToneMapOutputWidth, baleToneMapOutputHeight,
                            baleToneMapOutputDepth);
                    }
                }
                avcodec_parameters_free(&parameters);
            }
            videoSampleProvider = std::shared_ptr<MediaSampleProvider>(uncompressed);
        }

#pragma warning (default: 4973)

        if (baleToneMap > 0 && videoSampleProvider->Decoder() != DecoderEngine::FFmpegD3D11HardwareDecoder && !softwareStaged)
        {
            BaleToneMap::LogIneligible();
        }
        std::shared_ptr<CompressedSampleProvider> compressed;
        if (baleCompressedColorInfo > 0)
        {
            compressed = std::dynamic_pointer_cast<CompressedSampleProvider>(videoSampleProvider);
            if (compressed)
            {
                compressed->EnableColorAttributes(useHdr);
            }
        }
        auto hr = videoSampleProvider->Initialize();

        if (FAILED(hr))
        {
            videoSampleProvider = nullptr;
        }
        else if (compressed)
        {
            // av_log, like the other Bale lines: the app captures only FFmpeg log lines starting with "Bale ".
            av_log(nullptr, AV_LOG_INFO, "Bale compressed colour info: transfer=%d primaries=%d matrix=%d range=%d hdr=%d\n",
                static_cast<int>(avStream->codecpar->color_trc), static_cast<int>(avStream->codecpar->color_primaries),
                static_cast<int>(avStream->codecpar->color_space), static_cast<int>(avStream->codecpar->color_range), useHdr ? 1 : 0);
        }

        return videoSampleProvider;
    }

    // Bale: only compressed/system-decoder providers call this; FFmpeg-decoded surfaces stay unchanged.
    void FFmpegMediaSource::SetBaleDecoderProperties(VideoEncodingProperties const& videoProperties)
    {
        // GUIDs from Windows SDK mfapi.h (local constants avoid its WINVER declaration guards):
        // https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/mfapi.h
        // MF_MT_DECODER_USE_MAX_RESOLUTION {UINT32 (BOOL)}
        constexpr winrt::guid decoderUseMaxResolution{ 0x4c547c24, 0xaf9a, 0x4f38, { 0x96, 0xad, 0x97, 0x87, 0x73, 0xcf, 0x53, 0xe7 } };
        // MF_MT_DECODER_MAX_DPB_COUNT {UINT32}
        constexpr winrt::guid decoderMaxDpbCount{ 0x67be144c, 0x88b7, 0x4ca9, { 0x96, 0x28, 0xc8, 0x08, 0xd5, 0x26, 0x22, 0x17 } };
        if (baleDecoderUseMaxResolution > 0)
        {
            videoProperties.Properties().Insert(decoderUseMaxResolution, PropertyValue::CreateUInt32(1));
        }
        if (baleDecoderMaxDpbCount > 0)
        {
            videoProperties.Properties().Insert(decoderMaxDpbCount, PropertyValue::CreateUInt32(static_cast<uint32_t>(baleDecoderMaxDpbCount)));
        }
    }

    HRESULT FFmpegMediaSource::ParseOptions(PropertySet const& ffmpegOptions)
    {
        HRESULT hr = S_OK;

        // Convert FFmpeg options given in PropertySet to AVDictionary. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
        if (ffmpegOptions != nullptr)
        {
            auto options = ffmpegOptions.First();

            while (options.HasCurrent())
            {
                auto key = StringUtils::PlatformStringToUtf8String(options.Current().Key());
                hstring value = StringUtils::ToString(options.Current().Value());
                if (TryParseBaleOption(key, value, hr))
                {
                    if (FAILED(hr))
                    {
                        break;
                    }
                }
                else if (!value.empty())
                {
                    // Add key and value pair entry
                    auto valCstr = StringUtils::PlatformStringToUtf8String(value);
                    if (av_dict_set(&avDict, key.c_str(), valCstr.c_str(), 0) < 0)
                    {
                        hr = E_INVALIDARG;
                        break;
                    }
                }
                options.MoveNext();
            }
        }

        return hr;
    }

    // Bale: "bale_" keys tune this fork and are never handed to FFmpeg as format options.
    // Returns false for any other key. Out-of-range or non-numeric values fail with E_INVALIDARG.
    bool FFmpegMediaSource::TryParseBaleOption(std::string const& key, hstring const& value, HRESULT& hr)
    {
        if (key.rfind("bale_", 0) != 0)
        {
            return false;
        }

        int* target = nullptr;
        int minimum = 0;
        int maximum = 0;
        int retired = 0;
        if (key == "bale_decode_ahead_frames") { target = &baleDecodeAheadFrames; minimum = 0; maximum = 64; }
        else if (key == "bale_dav1d_max_frame_delay") { target = &baleDav1dMaxFrameDelay; minimum = 0; maximum = 64; }
        else if (key == "bale_dav1d_export_film_grain") { target = &baleDav1dExportFilmGrain; minimum = 0; maximum = 1; }
        else if (key == "bale_film_grain_mode") { target = &baleFilmGrainMode; minimum = 0; maximum = 3; }
        else if (key == "bale_d3d11_upload") { target = &baleD3d11Upload; minimum = 0; maximum = 6; }
        else if (key == "bale_tonemap") { target = &baleToneMap; minimum = 0; maximum = 2; }
        // Retired debug keys: still accepted (old ranges) so older apps that send them keep working,
        // but ignored.
        else if (key == "bale_tonemap_output") { target = &retired; minimum = 0; maximum = 15; }
        else if (key == "bale_tonemap_format") { target = &retired; minimum = 0; maximum = 3; }
        else if (key == "bale_tonemap_output_width") { target = &baleToneMapOutputWidth; minimum = 0; maximum = 8192; }
        else if (key == "bale_tonemap_output_height") { target = &baleToneMapOutputHeight; minimum = 0; maximum = 8192; }
        else if (key == "bale_tonemap_white_nits") { target = &baleToneMapWhiteNits; minimum = 0; maximum = 400; }
        else if (key == "bale_tonemap_output_depth") { target = &baleToneMapOutputDepth; minimum = 0; maximum = 10; }
        else if (key == "bale_zero_copy") { target = &baleZeroCopy; minimum = 0; maximum = 1; }
        else if (key == "bale_decode_thread_priority") { target = &baleDecodeThreadPriority; minimum = 0; maximum = 2; }
        else if (key == "bale_decoder_use_max_resolution") { target = &baleDecoderUseMaxResolution; minimum = 0; maximum = 1; }
        else if (key == "bale_decoder_max_dpb_count") { target = &baleDecoderMaxDpbCount; minimum = 0; maximum = 16; }
        else if (key == "bale_compressed_color_info") { target = &baleCompressedColorInfo; minimum = 0; maximum = 1; }
        else if (key == "bale_rav1g_parse") { target = &baleRav1gParse; minimum = 0; maximum = 8; }
        else if (key == "bale_rav1g_display") { target = &baleRav1gDisplay; minimum = 0; maximum = 1; }
        else if (key == "bale_rav1g_display_fixed_peak") { target = &baleRav1gDisplayFixedPeak; minimum = 0; maximum = 1; }
        else if (key == "bale_rav1g_display_bilinear_chroma") { target = &baleRav1gDisplayBilinearChroma; minimum = 0; maximum = 1; }
        else if (key == "bale_rav1g_perf") { target = &baleRav1gPerf; minimum = 0; maximum = 2; }
        else if (key == "bale_gpu_decode_inflight") { target = &baleGpuDecodeInflight; minimum = 1; maximum = 8; }
        else if (key == "bale_gpu_output_images") { target = &baleGpuOutputImages; minimum = 0; maximum = 16; }
        else if (key == "bale_gpu_output_textures") { target = &baleGpuOutputTextures; minimum = 0; maximum = 64; }
        else if (key == "bale_fast_seek_without_session") { target = &baleFastSeekWithoutSession; minimum = 0; maximum = 1; }
        else if (key == "bale_system_decoder_dolby") { target = &baleSystemDecoderDolby; minimum = 0; maximum = 1; }
        else if (key == "bale_subtitle_stream") { target = &baleSubtitleStream; minimum = 0; maximum = 65535; }
        else if (key == "bale_subtitle_sink") { target = &baleSubtitleSink; minimum = 1; maximum = 999999999; }
        else if (key == "bale_av1_decoder") { target = &baleAv1Decoder; minimum = 0; maximum = 1; }
        else if (key == "bale_d3d11va_pool_from_stream") { target = &baleD3d11vaPoolFromStream; minimum = 0; maximum = 1; }
        // Retired experiment (bale.63-75): shader-readable decoder pools; accepted and ignored.
        else if (key == "bale_d3d11va_shader_resource") { target = &retired; minimum = 0; maximum = 1; }
        else if (key == "bale_av1_fallback_max_height") { target = &baleAv1FallbackMaxHeight; minimum = 0; maximum = 8192; }
        else
        {
            hr = E_INVALIDARG;
            return true;
        }

        auto text = StringUtils::PlatformStringToUtf8String(value);
        if (text.empty() || text.size() > 9)
        {
            hr = E_INVALIDARG;
            return true;
        }
        int parsed = 0;
        for (auto c : text)
        {
            if (c < '0' || c > '9')
            {
                hr = E_INVALIDARG;
                return true;
            }
            parsed = parsed * 10 + (c - '0');
        }
        if (parsed < minimum || parsed > maximum)
        {
            hr = E_INVALIDARG;
            return true;
        }

        *target = parsed;
        if (target == &retired)
        {
            av_log(nullptr, AV_LOG_INFO, "Bale option %s retired; ignored\n", key.c_str());
        }
        return true;
    }

    bool FFmpegMediaSource::UseDecodeAhead()
    {
        // GPU decoder streams too: their feeder thread keeps the decoder busy, and the worker turns
        // decoded frames into finished samples (fence wait, copy or processing) ahead of MF's
        // requests, so the request thread, which holds `mutex`, seldom waits for the GPU.
        return baleDecodeAheadFrames > 0 && currentVideoStream &&
            (currentVideoStream->Decoder() == DecoderEngine::FFmpegSoftwareDecoder ||
                std::dynamic_pointer_cast<GpuDecoderSampleProvider>(currentVideoStream)) &&
            !config->IsFrameGrabber;
    }

    void FFmpegMediaSource::AheadLoop()
    {
        if (baleDecodeThreadPriority > 0)
        {
            SetThreadPriority(GetCurrentThread(), BaleThreadBoost::ToWin32(baleDecodeThreadPriority));
        }
        bool apartment = false;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
        }
        catch (...)
        {
        }
        {
            std::unique_lock lock(aheadMutex);
            while (true)
            {
                auto idleStart = BaleUploadTiming::Now();
                aheadCv.wait(lock, [this]
                    {
                        return aheadStop || (aheadPauses == 0 && aheadStream && !aheadEnd && (int)aheadQueue.size() < baleDecodeAheadFrames);
                    });
                aheadIdleTicks += BaleUploadTiming::Now() - idleStart;
                if (aheadStop)
                {
                    break;
                }

                auto stream = aheadStream;
                auto generation = aheadGeneration;
                aheadBusy = true;
                lock.unlock();

                MediaStreamSample sample{ nullptr };
                auto workStart = BaleUploadTiming::Now();
                try
                {
                    sample = stream->GetNextSample();
                }
                catch (...)
                {
                    sample = nullptr;
                }
                auto work = BaleUploadTiming::Now() - workStart;

                lock.lock();
                aheadBusy = false;
                aheadWorkTicks += work;
                if (work > aheadWorkPeak) aheadWorkPeak = work;
                if (++aheadTimedSamples == 240)
                {
                    // Starved = the MF request thread found the queue empty and waited for the worker.
                    av_log(nullptr, AV_LOG_INFO,
                        "Bale decode-ahead timing: samples=%u queued=%zu work avg/max ms=%.2f/%.1f idle avg ms=%.2f starved=%u total/max ms=%.1f/%.1f.\n",
                        aheadTimedSamples, aheadQueue.size(), BaleUploadTiming::Ms(aheadWorkTicks) / aheadTimedSamples,
                        BaleUploadTiming::Ms(aheadWorkPeak), BaleUploadTiming::Ms(aheadIdleTicks) / aheadTimedSamples,
                        aheadStarvedCount, BaleUploadTiming::Ms(aheadStarvedTicks), BaleUploadTiming::Ms(aheadStarvedPeak));
                    aheadWorkTicks = aheadWorkPeak = aheadIdleTicks = aheadStarvedTicks = aheadStarvedPeak = 0;
                    aheadTimedSamples = aheadStarvedCount = 0;
                }
                if (generation == aheadGeneration && stream == aheadStream)
                {
                    if (sample)
                    {
                        aheadQueue.push_back(sample);
                    }
                    else
                    {
                        aheadEnd = true;
                    }
                }
                aheadCv.notify_all();
            }
        }
        if (apartment)
        {
            winrt::uninit_apartment();
        }
    }

    // Called with `mutex` held. Stops the worker between samples; clear also drops decoded samples
    // (their decoder state is about to be discarded by a seek, flush or stream switch).
    void FFmpegMediaSource::AheadPause(bool clear)
    {
        std::unique_lock lock(aheadMutex);
        aheadPauses++;
        if (clear)
        {
            aheadGeneration++;
            aheadQueue.clear();
            aheadEnd = false;
        }
        aheadCv.wait(lock, [this] { return !aheadBusy; });
        lock.unlock();
        // After the worker: a sample request it is in may wait for a frame the feeder still decodes.
        PauseGpuDecoderFeeders(true);
    }

    void FFmpegMediaSource::AheadResume()
    {
        PauseGpuDecoderFeeders(false);
        std::lock_guard lock(aheadMutex);
        aheadPauses--;
        aheadCv.notify_all();
    }

    void FFmpegMediaSource::AheadShutdown()
    {
        {
            std::lock_guard lock(aheadMutex);
            aheadStop = true;
            aheadCv.notify_all();
        }
        if (aheadThread.joinable())
        {
            aheadThread.join();
        }
        std::lock_guard lock(aheadMutex);
        aheadQueue.clear();
        aheadStream = nullptr;
        aheadEnd = false;
        aheadGeneration++;
    }

    // Called with `mutex` held, from OnSampleRequested for the current FFmpeg software video stream.
    MediaStreamSample FFmpegMediaSource::TakeAheadSample()
    {
        std::unique_lock lock(aheadMutex);
        if (aheadStop)
        {
            return nullptr;
        }
        if (aheadStream != currentVideoStream)
        {
            aheadGeneration++;
            aheadQueue.clear();
            aheadEnd = false;
            aheadCv.wait(lock, [this] { return !aheadBusy; });
            aheadStream = currentVideoStream;
        }
        if (!aheadThread.joinable())
        {
            aheadThread = std::thread([this] { AheadLoop(); });
        }
        aheadCv.notify_all();
        if (aheadQueue.empty() && !aheadEnd)
        {
            auto starvedStart = BaleUploadTiming::Now();
            aheadCv.wait(lock, [this] { return !aheadQueue.empty() || aheadEnd || aheadStop; });
            auto starved = BaleUploadTiming::Now() - starvedStart;
            aheadStarvedCount++;
            aheadStarvedTicks += starved;
            if (starved > aheadStarvedPeak) aheadStarvedPeak = starved;
        }
        aheadCv.wait(lock, [this] { return !aheadQueue.empty() || aheadEnd || aheadStop; });
        if (aheadQueue.empty())
        {
            return nullptr;
        }
        auto sample = aheadQueue.front();
        aheadQueue.pop_front();
        aheadCv.notify_all();
        return sample;
    }

    void FFmpegMediaSource::MediaStreamSourceClosed(MediaStreamSource const&, MediaStreamSourceClosedEventArgs const&)
    {
        Close(true);
    }

    void FFmpegMediaSource::CloseUploadDevice()
    {
        if (uploadDeviceHandle && uploadDeviceManager)
        {
            auto hr = uploadDeviceManager->CloseDeviceHandle(uploadDeviceHandle);
            if (FAILED(hr)) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload CloseDeviceHandle failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        }
        uploadDeviceHandle = nullptr;
        uploadDeviceManager = nullptr;
    }

    void FFmpegMediaSource::DisableUploadStreams(HRESULT hr)
    {
        for (auto& stream : videoStreams)
        {
            if (stream->Decoder() == DecoderEngine::FFmpegSoftwareDecoder)
            {
                auto uncompressed = std::dynamic_pointer_cast<UncompressedVideoSampleProvider>(stream);
                if (uncompressed) uncompressed->DisableUpload(hr);
            }
        }
    }

    bool FFmpegMediaSource::HasZeroCopyStream()
    {
        for (auto& stream : videoStreams)
        {
            if (stream->Decoder() == DecoderEngine::FFmpegSoftwareDecoder)
            {
                auto uncompressed = std::dynamic_pointer_cast<UncompressedVideoSampleProvider>(stream);
                if (uncompressed && uncompressed->ZeroCopy > 0) return true;
            }
        }
        return false;
    }

    bool FFmpegMediaSource::HasStagedGrainStream()
    {
        for (auto& stream : videoStreams)
        {
            if (stream->Decoder() == DecoderEngine::FFmpegSoftwareDecoder)
            {
                auto uncompressed = std::dynamic_pointer_cast<UncompressedVideoSampleProvider>(stream);
                if (uncompressed && (uncompressed->StagedGrain > 0 || uncompressed->HasSoftwareStage())) return true;
            }
        }
        return false;
    }

    // Called with `mutex` held. Raises the current video stream's latched fatal failure once.
    // Returns true when that stream has failed; the caller then delivers no sample.
    bool FFmpegMediaSource::RaiseBaleFatal(MediaStreamSource const& sender)
    {
        if (baleFatalRaised) return true;
        if (!currentVideoStream) return false;
        HRESULT hr = currentVideoStream->FatalError();
        if (SUCCEEDED(hr)) return false;

        baleFatalRaised = true;
        av_log(nullptr, AV_LOG_ERROR, "Bale video: stream failed (hr=0x%08X); raising MediaStreamSource error.\n", static_cast<unsigned>(hr));
        try
        {
            sender.NotifyError(hr == E_INVALIDARG || hr == UncompressedVideoSampleProvider::ZeroCopyMissed
                ? MediaStreamSourceErrorStatus::UnsupportedMediaFormat
                : MediaStreamSourceErrorStatus::DecodeError);
        }
        catch (...)
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale video: NotifyError failed (hr=0x%08X).\n", static_cast<unsigned>(winrt::to_hresult()));
        }
        return true;
    }

    void FFmpegMediaSource::InitializeUploadDevice(MediaStreamSource const& sender)
    {
        HRESULT hr = S_OK;
        com_ptr<ID3D11Device> uploadDevice;
        com_ptr<ID3D11DeviceContext> uploadContext;
        com_ptr<ID3D11VideoDevice> videoDevice;
        try
        {
            winrt::check_hresult(DirectXInteropHelper::GetDeviceManagerFromStreamSource(sender, uploadDeviceManager));
            if (!uploadDeviceManager) winrt::throw_hresult(E_NOINTERFACE);
            winrt::check_hresult(DirectXInteropHelper::GetDeviceFromStreamSource(uploadDeviceManager,
                uploadDevice, uploadContext, videoDevice, &uploadDeviceHandle));
        }
        catch (...)
        {
            hr = winrt::to_hresult();
        }

        if (FAILED(hr))
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload acquire device failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
            DisableUploadStreams(hr);
            // OpenDeviceHandle can succeed even if GetVideoService fails; always close that handle.
            CloseUploadDevice();
            return;
        }

        bool active = false;
        for (auto& stream : videoStreams)
        {
            if (stream->Decoder() == DecoderEngine::FFmpegSoftwareDecoder)
            {
                auto uncompressed = std::dynamic_pointer_cast<UncompressedVideoSampleProvider>(stream);
                if (uncompressed && uncompressed->SetUploadDevice(uploadDevice, uploadContext, baleD3d11Upload) == S_OK) active = true;
            }
        }
        if (!active) CloseUploadDevice();
    }

    void FFmpegMediaSource::OnStarting(MediaStreamSource const& sender, MediaStreamSourceStartingEventArgs const& args)
    {
        // Bale: a GPU decoder waiting for an output image (e.g. while paused) may hold `mutex` on the
        // sample thread; end that wait first. The streams resume below.
        CancelGpuDecoderWaits();
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }
        MediaStreamSourceStartingRequest request = args.Request();
        AheadPauseScope aheadPause(this, false);

        try
        {
            if (isFirstSeek && avHardwareContext)
            {
                HRESULT hr = DirectXInteropHelper::GetDeviceManagerFromStreamSource(sender, deviceManager);
                if (SUCCEEDED(hr))
                    hr = D3D11VideoSampleProvider::InitializeHardwareDeviceContext(sender, avHardwareContext, device, deviceContext, deviceManager, &deviceHandle);

                if (SUCCEEDED(hr))
                {
                    // assign device and context
                    for (auto& stream : videoStreams)
                    {
                        // set device pointers to stream
                        hr = stream->SetHardwareDevice(device, deviceContext, avHardwareContext);

                        if (!SUCCEEDED(hr))
                        {
                            break;
                        }
                    }
                }
                else
                {
                    // unref all hw device contexts
                    for (auto& stream : videoStreams)
                    {
                        stream->FreeHardwareDevice();
                    }
                    av_buffer_unref(&avHardwareContext);
                    device = nullptr;
                    deviceContext = nullptr;
                }
            }

            // Bale: zero-copy streams need their upload device and bale_zc handle before any decode,
            // including when another stream opened a hardware context; so do staged grain streams
            // (their grain handle), which need no bale_d3d11_upload.
            if (isFirstSeek && !config->IsFrameGrabber &&
                ((baleD3d11Upload > 0 && HasZeroCopyStream()) || HasStagedGrainStream()))
            {
                InitializeUploadDevice(sender);
            }

            // Bale: GPU decoder streams need the renderer's device before their first decode.
            if (isFirstSeek && !config->IsFrameGrabber && HasGpuDecoderStream())
            {
                InitializeGpuDecoderDevice(sender);
            }

            // Perform seek operation when MediaStreamSource received seek event from MediaElement
            if (request.StartPosition() && request.StartPosition().Value().count() <= mediaDuration.count() && (!isFirstSeek || request.StartPosition().Value().count() > 0))
            {
                if (currentVideoStream && !currentVideoStream->IsEnabled())
                {
                    currentVideoStream->EnableStream();
                }

                if (currentAudioStream && !currentAudioStream->IsEnabled())
                {
                    currentAudioStream->EnableStream();
                }

                TimeSpan actualPosition = request.StartPosition().Value();
                auto hr = Seek(request.StartPosition().Value(), actualPosition, true);
                if (SUCCEEDED(hr))
                {
                    request.SetActualStartPosition(actualPosition);
                }
            }
        }
        catch (...)
        {
            DebugMessage(L"Exception in OnStarting()!");
        }

        // Bale: end the cancel; a stream whose sample request it ended drops the worker's end mark.
        bool cancelledRequest = false;
        for (auto& stream : videoStreams)
        {
            if (auto gpuStream = std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream))
            {
                cancelledRequest = gpuStream->ResumeAfterCancel() || cancelledRequest;
            }
        }
        if (cancelledRequest)
        {
            AheadPauseScope clearAhead(this, true);
        }

        isFirstSeek = false;
        isFirstSeekAfterStreamSwitch = false;
    }

    void FFmpegMediaSource::OnSampleRequested(winrt::Windows::Media::Core::MediaStreamSource const& sender, winrt::Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs const& args)
    {
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        try
        {
            // Bale: after a fatal video failure the source is in error; decode and deliver nothing.
            if (RaiseBaleFatal(sender))
            {
                return;
            }
            if (config->General().ReadAheadBufferEnabled())
            {
                m_pReader->Start();
            }
            if (currentAudioStream && args.Request().StreamDescriptor() == currentAudioStream->StreamDescriptor())
            {
                auto sample = currentAudioStream->GetNextSample();
                CheckExtendDuration(sample);
                args.Request().Sample(sample);
            }
            else if (currentVideoStream && args.Request().StreamDescriptor() == currentVideoStream->StreamDescriptor())
            {
                CheckVideoDeviceChanged(sender);
                if (RaiseBaleFatal(sender))
                {
                    return;
                }
                auto sample = UseDecodeAhead() ? TakeAheadSample() : currentVideoStream->GetNextSample();
                if (RaiseBaleFatal(sender))
                {
                    return;
                }
                CheckExtendDuration(sample);
                args.Request().Sample(sample);
                // Count actual MSS handoffs, not samples merely prepared by decode-ahead.
                if (baleD3d11Upload > 0 && uploadSamplesIssued < 3 && sample)
                {
                    try
                    {
                        if (auto surface = sample.Direct3D11Surface())
                        {
                            auto desc = surface.Description();
                            av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload sample issued index=%u position_ms=%lld texture=%dx%d.\n",
                                ++uploadSamplesIssued, static_cast<long long>(sample.Timestamp().count() / 10000), desc.Width, desc.Height);
                        }
                    }
                    catch (...)
                    {
                        av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload sample diagnostics failed (hr=0x%08X).\n", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
            }
            else
            {
                args.Request().Sample(nullptr);
            }
        }
        catch (...)
        {
            if (baleD3d11Upload > 0) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload OnSampleRequested failed (hr=0x%08X).\n", static_cast<unsigned>(winrt::to_hresult()));
            DebugMessage(L"Exception in OnSampleRequested()!");
        }
    }

    void FFmpegMediaSource::CheckExtendDuration(MediaStreamSample sample)
    {
        if (sample && config->General().AutoExtendDuration())
        {
            auto sampleEnd = sample.Timestamp() + sample.Duration();
            if (TimeSpan::zero() < mediaDuration && (mediaDuration < sampleEnd ||
                (lastDurationExtension && (mediaDuration - sampleEnd).count() < 5000000)))
            {
                auto extension = min(lastDurationExtension + 1, 5);

                mediaDuration += TimeSpan{ extension * 10000000 };
                if (auto mss = mssWeak.get())
                {
                    mss.Duration(mediaDuration);
                }

                lastDurationExtension = extension;
            }
        }
    }

    void FFmpegMediaSource::CheckVideoDeviceChanged(MediaStreamSource const& mss)
    {
        if (gpuDecoderDeviceManager)
        {
            auto gpuHr = gpuDecoderDeviceManager->TestDevice(gpuDecoderDeviceHandle);
            if (FAILED(gpuHr))
            {
                // The decoder's images live on the lost device: the stream fails (no recreate).
                CancelGpuDecoderWaits();
                AheadPauseScope aheadPause(this, true);
                for (auto& stream : videoStreams)
                {
                    if (auto gpuStream = std::dynamic_pointer_cast<GpuDecoderSampleProvider>(stream)) gpuStream->DeviceLost(gpuHr);
                }
                CloseGpuDecoderDevice();
            }
        }
        if (uploadDeviceManager)
        {
            auto uploadHr = uploadDeviceManager->TestDevice(uploadDeviceHandle);
            if (FAILED(uploadHr))
            {
                // Do not deliver queued textures from a removed/reset device. No upload retries.
                AheadPauseScope aheadPause(this, true);
                DisableUploadStreams(uploadHr);
                CloseUploadDevice();
            }
        }

        bool hasDeviceChanged = false;
        HRESULT hr = S_OK;
        if (deviceManager)
        {
            hr = deviceManager->TestDevice(deviceHandle);
            hasDeviceChanged = hr == MF_E_DXGI_NEW_VIDEO_DEVICE;
        }

        if (hasDeviceChanged && avHardwareContext)
        {
            hr = S_OK;
            av_buffer_unref(&avHardwareContext);
            device = nullptr;
            deviceContext = nullptr;

            if (deviceHandle && deviceManager)
                deviceManager->CloseDeviceHandle(deviceHandle);

            avHardwareContext = av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA);

            if (!avHardwareContext)
            {
                hr = E_OUTOFMEMORY;
            }

            if (SUCCEEDED(hr))
            {
                hr = D3D11VideoSampleProvider::InitializeHardwareDeviceContext(mss, avHardwareContext, device, deviceContext, deviceManager, &deviceHandle);
            }

            if (SUCCEEDED(hr))
            {
                // assign device and context
                for (auto& stream : videoStreams)
                {
                    // set device pointers to stream
                    hr = stream->SetHardwareDevice(device, deviceContext, avHardwareContext);

                    if (!SUCCEEDED(hr))
                    {
                        break;
                    }
                }
            }

            if (SUCCEEDED(hr))
            {
                // We assume can seek if duration > 0.
                if (mediaDuration.count() > 0)
                {
                    // seek to last keyframe position
                    TimeSpan lastVideoTimestamp = currentVideoStream->LastSampleTimestamp;
                    TimeSpan actualPosition;
                    Seek(lastVideoTimestamp, actualPosition, false);

                    // decode video until we are at target position
                    while (true)
                    {
                        auto sample = currentVideoStream->GetNextSample();
                        if (!sample || sample.Timestamp() >= lastVideoTimestamp)
                        {
                            break;
                        }
                    }

                    // decode audio until we are at target position
                    if (currentAudioStream)
                    {
                        TimeSpan lastAudioTimestamp = currentAudioStream->LastSampleTimestamp;
                        while (true)
                        {
                            auto sample = currentAudioStream->GetNextSample();
                            if (!sample || sample.Timestamp() >= lastAudioTimestamp)
                            {
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    void FFmpegMediaSource::OnSwitchStreamsRequested(MediaStreamSource const& sender, MediaStreamSourceSwitchStreamsRequestedEventArgs const& args)
    {
        UNREFERENCED_PARAMETER(sender);
        std::lock_guard lock(mutex);
        if (isClosed)
        {
            return;
        }

        AheadPauseScope aheadPause(this, true);

        try
        {
            m_pReader->Stop();
            m_pReader->Flush();

            if (currentAudioStream && args.Request().OldStreamDescriptor() == currentAudioStream->StreamDescriptor())
            {
                currentAudioStream->DisableStream();
                currentAudioStream = nullptr;
                currentAudioStreamInfo = nullptr;
            }
            if (currentVideoStream && args.Request().OldStreamDescriptor() == currentVideoStream->StreamDescriptor())
            {
                currentVideoStream->DisableStream();
                currentVideoStream = nullptr;
                currentAudioStreamInfo = nullptr;
            }

            for (auto& stream : audioStreams)
            {
                if (stream->StreamDescriptor() == args.Request().NewStreamDescriptor())
                {
                    currentAudioStream = stream;
                    currentAudioStream->EnableStream();
                }
            }
            for (auto& stream : videoStreams)
            {
                if (stream->StreamDescriptor() == args.Request().NewStreamDescriptor())
                {
                    currentVideoStream = stream;
                    currentVideoStream->EnableStream();

                    currentVideoStreamInfo = currentVideoStream->VideoInfo();
                }
            }

            isFirstSeekAfterStreamSwitch = config->General().FastSeekSmartStreamSwitching();
        }
        catch (...)
        {
            DebugMessage(L"Exception in OnSwitchStreamsRequested()!");
        }
    }

    HRESULT FFmpegMediaSource::Seek(const TimeSpan& position, TimeSpan& actualPosition, bool allowFastSeek)
    {
        DebugMessage(L"Seek\n");

        auto diffCurrent = position - currentPosition;
        auto diffLast = position - lastPosition;
        // Bale: without a MediaPlaybackSession, currentPosition/lastPosition never update, so the
        // stream-switch heuristic is skipped and fast seeks are backward-only (bale_fast_seek_without_session).
        bool hasSession = PlaybackSession() != nullptr;
        bool sessionlessFastSeek = !hasSession && baleFastSeekWithoutSession > 0;
        bool isSeekBeforeStreamSwitch = hasSession && allowFastSeek && config->General().FastSeekSmartStreamSwitching() && !isFirstSeekAfterStreamSwitch && diffCurrent.count() > 0 && diffCurrent.count() < 5000000 && diffLast.count() > 0 && diffLast.count() < 10000000;

        bool fastSeek = allowFastSeek && config->General().FastSeek() && currentVideoStream && (hasSession || sessionlessFastSeek) && !isFirstSeekAfterStreamSwitch;
        if (isSeekBeforeStreamSwitch)
        {
            return S_OK;
        }
        else if (position == currentPosition && position == lastPosition && position == lastSeek && !isFirstSeekAfterStreamSwitch && position.count() > 0)
        {
            DebugMessage(L"Skipping double seek request.\n");
            return S_OK;
        }
        else
        {
            lastSeek = position;
            {
                // Bale: decoded-ahead samples predate the seek. Callers hold `mutex`; pause clears
                // the queue and waits for the worker to finish its current sample.
                AheadPauseScope aheadPause(this, true);
                auto hr = m_pReader->Seek(position, actualPosition, lastPosition, fastSeek, currentVideoStream, currentAudioStream, fastSeek && sessionlessFastSeek);
                if (fastSeek && sessionlessFastSeek && SUCCEEDED(hr))
                {
                    av_log(nullptr, AV_LOG_INFO, "Bale seek: sessionless fast seek target=%lld landed=%lld ms\n",
                        static_cast<long long>(position.count() / 10000), static_cast<long long>(actualPosition.count() / 10000));
                }
                return hr;
            }
        }
    }

    void FFmpegMediaSource::OnPositionChanged(MediaPlaybackSession const& sender, IInspectable const& args)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(args);
        std::lock_guard lock(mutex);
        if (isClosed || isShuttingDown)
        {
            return;
        }

        try
        {
            lastPosition = currentPosition;
            currentPosition = sender.Position();
        }
        catch (...)
        {
            // The session may already be torn down (e.g. the player was disposed); never throw out of
            // a Media Foundation event handler.
        }
    }

    // Static functions passed to FFmpeg
    static int FileStreamRead(void* ptr, uint8_t* buf, int bufSize)
    {
        FFmpegMediaSource* mss = reinterpret_cast<FFmpegMediaSource*>(ptr);
        ULONG bytesRead = 0;
        HRESULT hr = mss->fileStreamData->Read(buf, bufSize, &bytesRead);

        if (FAILED(hr))
        {
            return -1;
        }

        // If we succeed but don't have any bytes, assume end of file
        if (bytesRead == 0)
        {
            return AVERROR_EOF;  // Let FFmpeg know that we have reached eof
        }

        // Check encoding in case of external subtitle parser
        if (!mss->streamEncodingChecked && mss->config->IsExternalSubtitleParser && !mss->config->Subtitles().ExternalSubtitleEncoding())
        {
            // Make sure we have at least 4 bytes for BOM check
            bool isEof = false;
            while (bytesRead < (ULONG)min(bufSize, 4) && !isEof)
            {
                ULONG read = 0;
                hr = mss->fileStreamData->Read(buf + bytesRead, bufSize - bytesRead, &read);
                if (FAILED(hr))
                {
                    return -1;
                }
                else if (read == 0)
                {
                    isEof = true;
                }

                bytesRead += read;
            }

            // first check BOM
            auto encoding = TextEncodingDetect::CheckBOM(buf, bytesRead);
            if (encoding == TextEncodingDetect::None)
            {
                // if no BOM is present, make sure we read the first chunk for full probing
                while (bytesRead < (ULONG)bufSize && !isEof)
                {
                    ULONG read = 0;
                    hr = mss->fileStreamData->Read(buf + bytesRead, bufSize - bytesRead, &read);
                    if (FAILED(hr))
                    {
                        return -1;
                    }
                    else if (read == 0)
                    {
                        isEof = true;
                    }

                    bytesRead += read;
                }

                TextEncodingDetect detect;
                encoding = detect.DetectEncoding(buf, bytesRead);
            }

            mss->streamEncoding = encoding;
            mss->streamEncodingChecked = true;
        }

        return bytesRead;
    }

    // Static function to seek in file stream. Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
    static int64_t FileStreamSeek(void* ptr, int64_t pos, int whence)
    {
        FFmpegMediaSource* mss = reinterpret_cast<FFmpegMediaSource*>(ptr);
        if (whence == AVSEEK_SIZE)
        {
            // get stream size
            STATSTG status;
            if (FAILED(mss->fileStreamData->Stat(&status, STATFLAG_NONAME)))
            {
                return -1;
            }
            return status.cbSize.QuadPart;
        }
        else
        {
            LARGE_INTEGER in;
            in.QuadPart = pos;
            ULARGE_INTEGER out = { 0 };

            if (FAILED(mss->fileStreamData->Seek(in, whence, &out)))
            {
                return -1;
            }

            return out.QuadPart; // Return the new position:
        }
    }


    // Static functions passed to FFmpeg
    static int InputStreamRead(void* ptr, uint8_t* buf, int bufSize)
    {
        FFmpegMediaSource* mss = reinterpret_cast<FFmpegMediaSource*>(ptr);
        ULONG bytesRead = 0;
        auto buffer = NativeBuffer::NativeBufferFactory::CreateNativeBuffer(buf, bufSize);
        auto readFunction = mss->inputStream.ReadAsync(buffer, bufSize, InputStreamOptions::None);
        auto resultBuffer = readFunction.get();

        // The buffer returned by ReadAsync may not be the same as the one we passed in. In that case, we need to copy the data to our original buffer.
        if (resultBuffer != buffer)
        {
            auto resultData = resultBuffer.data();
            auto resultSize = resultBuffer.Length();
            if (resultSize > (ULONG)bufSize)
            {
                return -1;
            }
            memcpy(buf, resultData, resultSize);
        }
        bytesRead = resultBuffer.Length();

        // If we succeed but don't have any bytes, assume end of file
        if (bytesRead == 0)
        {
            return AVERROR_EOF;  // Let FFmpeg know that we have reached eof
        }

        // Check encoding in case of external subtitle parser
        if (!mss->streamEncodingChecked && mss->config->IsExternalSubtitleParser && !mss->config->Subtitles().ExternalSubtitleEncoding())
        {
            // first check BOM
            auto encoding = TextEncodingDetect::CheckBOM(buf, bytesRead);
            if (encoding == TextEncodingDetect::None)
            {
                TextEncodingDetect detect;
                encoding = detect.DetectEncoding(buf, bytesRead);
            }

            mss->streamEncoding = encoding;
            mss->streamEncodingChecked = true;
        }

        return bytesRead;
    }


    static int IsShuttingDown(void* ptr)
    {
        FFmpegMediaSource* mss = reinterpret_cast<FFmpegMediaSource*>(ptr);
        if (mss->isShuttingDown)
        {
            return 1;
        }
        return 0;
    }
}

// Bale: plain C export (no WinRT API change) so the app can create rav1g's process-wide device and
// compile its pipeline states before the first AV1 stream opens. Returns at once; idempotent.
// 0: started now, 1: already started earlier. The adapter is rav1g's choice (the first hardware
// adapter, the only one on Xbox); a stream whose MF device is on another adapter fails as before.
extern "C" __declspec(dllexport) int bale_rav1g_prewarm(void)
{
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return 1;
    try
    {
        std::thread([]
        {
            auto start = std::chrono::steady_clock::now();
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: prewarm started\n");
            const auto& device = BaleRav1gDevice::Get(6);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (SUCCEEDED(device.status))
            {
                av_log(nullptr, AV_LOG_INFO, "Bale rav1g: prewarm ready ms=%lld\n", static_cast<long long>(ms));
            }
            else
            {
                av_log(nullptr, AV_LOG_WARNING, "Bale rav1g: prewarm failed (status=0x%08X) ms=%lld\n",
                    static_cast<unsigned>(device.status), static_cast<long long>(ms));
            }
        }).detach();
    }
    catch (...)
    {
        started = false;
        return -1;
    }
    return 0;
}
