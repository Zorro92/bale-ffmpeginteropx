#pragma once
#include "FFmpegMediaSource.g.h"
#include "pch.h"
#include <thread>
#include <condition_variable>
#include "MediaSampleProvider.h"
#include "SubtitleProvider.h"
#include "AttachedFileHelper.h"
#include "MediaSourceConfig.h"
#include "CodecChecker.h"
#include "MediaMetadata.h"
#include "text_encoding_detect.h"
#include "GpuDecoderImagePool.h"

namespace winrt::FFmpegInteropX::implementation
{
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;
    using namespace winrt::Windows::Media::Core;
    using namespace winrt::Windows::Media::Playback;
    using namespace winrt::Windows::Storage::Streams;

#ifdef Win32
    using namespace winrt::Microsoft::UI::Dispatching;
#else
    using namespace winrt::Windows::System;
#endif

    using namespace TextEncoding;

    struct FFmpegMediaSource : FFmpegMediaSourceT<FFmpegMediaSource>
    {
        virtual ~FFmpegMediaSource();

#ifdef Win32
        ///<summary>Creates a FFmpegMediaSource from a stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromStreamAsync(IRandomAccessStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
        {
            return CreateFromStreamInternalAsync(stream, config, windowId);
        }

        ///<summary>Creates a FFmpegMediaSource from a input stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromInputStreamAsync(IInputStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
        {
            return CreateFromInputStreamInternalAsync(stream, config, windowId);
        }

        ///<summary>Creates a FFmpegMediaSource from a Uri.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromUriAsync(hstring uri, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
        {
            return CreateFromUriInternalAsync(uri, config, windowId);
        }

        ///<summary>Creates a FFmpegMediaSource from a file.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromFileAsync(hstring fileName, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId)
        {
            return CreateFromUriInternalAsync(fileName, config, windowId);
        }
#endif


        ///<summary>Creates a FFmpegMediaSource from a stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromStreamAsync(IRandomAccessStream stream, FFmpegInteropX::MediaSourceConfig config)
        {
            return CreateFromStreamInternalAsync(stream, config, 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromStreamAsync(IRandomAccessStream stream)
        {
            return CreateFromStreamInternalAsync(stream, FFmpegInteropX::MediaSourceConfig(), 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a input stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromInputStreamAsync(IInputStream stream, FFmpegInteropX::MediaSourceConfig config)
        {
            return CreateFromInputStreamInternalAsync(stream, config, 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a input stream.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromInputStreamAsync(IInputStream stream)
        {
            return CreateFromInputStreamInternalAsync(stream, FFmpegInteropX::MediaSourceConfig(), 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a Uri.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromUriAsync(hstring uri, FFmpegInteropX::MediaSourceConfig config)
        {
            return CreateFromUriInternalAsync(uri, config, 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a Uri.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromUriAsync(hstring uri)
        {
            return CreateFromUriInternalAsync(uri, FFmpegInteropX::MediaSourceConfig(), 0);
        }

#ifndef UWP
        ///<summary>Creates a FFmpegMediaSource from a file.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromFileAsync(hstring fileName, FFmpegInteropX::MediaSourceConfig config)
        {
            return CreateFromUriInternalAsync(fileName, config, 0);
        }

        ///<summary>Creates a FFmpegMediaSource from a file.</summary>
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromFileAsync(hstring fileName)
        {
            return CreateFromUriInternalAsync(fileName, FFmpegInteropX::MediaSourceConfig(), 0);
        }
#endif

        ///<summary>Sets the subtitle delay for all subtitle streams. Use negative values to speed them up, positive values to delay them.</summary>
        void SetSubtitleDelay(TimeSpan const& delay);

        ///<summary>Sets FFmpeg audio filters. This replaces any filters which were already set.</summary>
        void SetFFmpegAudioFilters(hstring const& audioFilters);

        ///<summary>Sets FFmpeg audio filters for audio stream specified by audioStreamIndex. This replaces any filters which were already set.</summary>
        void SetFFmpegAudioFilters(hstring const& audioFilters, winrt::FFmpegInteropX::AudioStreamInfo const& audioStream);

        ///<summary>Sets FFmpeg video filters. This replaces any filters which were already set.</summary>
        ///<remarks>Using FFmpeg video filters will degrade playback performance, since they run on the CPU and not on the GPU.</remarks>
        void SetFFmpegVideoFilters(hstring const& videoFilters);

        ///<summary>Sets FFmpeg video filters for the video stream specified by videoStreamIndex. This replaces any filters which were already set.</summary>
        ///<remarks>Using FFmpeg video filters will degrade playback performance, since they run on the CPU and not on the GPU.</remarks>
        void SetFFmpegVideoFilters(hstring const& videoFilters, winrt::FFmpegInteropX::VideoStreamInfo const& videoStream);

        ///<summary>Disables audio filters.</summary>
        void ClearFFmpegAudioFilters();

        ///<summary>Disables audio filters for the specified audio stream.</summary>
        void ClearFFmpegAudioFilters(winrt::FFmpegInteropX::AudioStreamInfo const& audioStream);

        ///<summary>Clears video filters.</summary>
        void ClearFFmpegVideoFilters();

        ///<summary>Disables audio filters for the specified video stream.</summary>
        void ClearFFmpegVideoFilters(winrt::FFmpegInteropX::VideoStreamInfo const& videoStream);

        ///<summary>Gets audio filters for the specified audio stream.</summary>
        hstring GetFFmpegAudioFilters(winrt::FFmpegInteropX::AudioStreamInfo const& audioStream);

        ///<summary>Gets video filters for the specified video stream.</summary>
        hstring GetFFmpegVideoFilters(winrt::FFmpegInteropX::VideoStreamInfo const& videoStream);

        ///<summary>Sends a command to audio filters on all audio streams.</summary>
        FFmpegInteropX::FilterCommandResult SendFFmpegAudioFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments);

        ///<summary>Sends a command to audio filters on the specified audio stream.</summary>
        FFmpegInteropX::FilterCommandResult SendFFmpegAudioFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments, winrt::FFmpegInteropX::AudioStreamInfo const& audioStream);

        ///<summary>Sends a command to video filters on all video streams.</summary>
        FFmpegInteropX::FilterCommandResult SendFFmpegVideoFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments);

        ///<summary>Sends a command to video filters on the specified video stream.</summary>
        FFmpegInteropX::FilterCommandResult SendFFmpegVideoFilterCommand(winrt::hstring target, winrt::hstring command, winrt::hstring arguments, winrt::FFmpegInteropX::VideoStreamInfo const& videoStream);


        ///<summary>Extracts an embedded thumbnail, if one is available (see HasThumbnail).</summary>
        FFmpegInteropX::MediaThumbnailData ExtractThumbnail();

        ///<summary>Gets the MediaStreamSource. Using the MediaStreamSource will prevent subtitles from working. Please use CreateMediaPlaybackItem instead.</summary>
        MediaStreamSource GetMediaStreamSource();

        ///<summary>Creates a MediaPlaybackItem for playback.</summary>
        MediaPlaybackItem CreateMediaPlaybackItem();

        ///<summary>Creates a MediaPlaybackItem for playback which starts at the specified stream offset.</summary>
        MediaPlaybackItem CreateMediaPlaybackItem(TimeSpan const& startTime);

        ///<summary>Creates a MediaPlaybackItem for playback which starts at the specified stream offset and ends after the specified duration.</summary>
        MediaPlaybackItem CreateMediaPlaybackItem(TimeSpan const& startTime, TimeSpan const& durationLimit);

        ///<summary>Creates a MediaPlaybackItem, assigns it to MediaPlayer.Source and waits for MediaOpened or MediaFailed (throws in that case).</summary>
        ///<remarks>This will also automatically cleanup resources, if MediaPlayer switches to a different file, or it's Source property is assigned null.</remarks>
        IAsyncAction OpenWithMediaPlayerAsync(MediaPlayer mediaPlayer);

        ///<summary>Adds an external subtitle from a stream.</summary>
        ///<param name="stream">The subtitle stream.</param>
        ///<param name="streamName">The name to use for the subtitle.</param>
        IAsyncOperation<IVectorView<FFmpegInteropX::SubtitleStreamInfo>> AddExternalSubtitleAsync(IRandomAccessStream stream, hstring streamName);

        ///<summary>Adds an external subtitle from a stream.</summary>
        ///<param name="stream">The subtitle stream.</param>
        IAsyncOperation<IVectorView<FFmpegInteropX::SubtitleStreamInfo>> AddExternalSubtitleAsync(IRandomAccessStream stream);

        ///<summary>Starts filling the read-ahead buffer, if enabled in the configuration.</summary>
        ///<remarks>Let the stream buffer fill before starting playback.</remarks>
        void StartBuffering();

        ///<summary>Gets the configuration that has been passed when creating the MSS instance.</summary>
        FFmpegInteropX::MediaSourceConfig Configuration();

        ///<summary>Gets the metadata tags available in the file.</summary>
        IMapView<hstring, IVectorView<hstring>> MetadataTags();

        ///<summary>Gets the duration of the stream. Returns zero, if this is streaming media.</summary>
        TimeSpan Duration();

        ///<summary>Gets the current video stream information.</summary>
        FFmpegInteropX::VideoStreamInfo CurrentVideoStream();

        ///<summary>Gets the current audio stream information.</summary>
        FFmpegInteropX::AudioStreamInfo CurrentAudioStream();

        ///<summary>Gets video stream information</summary>
        IVectorView<FFmpegInteropX::VideoStreamInfo> VideoStreams();

        ///<summary>Gets audio stream information.</summary>
        IVectorView<FFmpegInteropX::AudioStreamInfo> AudioStreams();

        ///<summary>Gets subtitle stream information.</summary>
        IVectorView<FFmpegInteropX::SubtitleStreamInfo> SubtitleStreams();

        ///<summary>Gets chapter information.</summary>
        IVectorView<FFmpegInteropX::ChapterInfo> ChapterInfos();

        ///<summary>Gets format information.</summary>
        FFmpegInteropX::FormatInfo FormatInfo();

        ///<summary>Gets a boolean indication if a thumbnail is embedded in the file.</summary>
        bool HasThumbnail();

        ///<summary>Gets the MediaPlaybackItem that was created before by using CreateMediaPlaybackItem.</summary>
        MediaPlaybackItem PlaybackItem();

        ///<summary>Gets or sets the MediaPlaybackSession associated with this FFmpeg source. Used when FastSeek is enabled.</summary>
        ///<remarks>After playback has started, please assign MediaPlayer.PlaybackSession to this .</remarks>
        MediaPlaybackSession PlaybackSession();
        void PlaybackSession(MediaPlaybackSession const& value);

        ///<summary>Sets a presentation timestamp delay for the given stream. Audio, video and subtitle synchronisation can be achieved this way. A positive value will cause samples (or subtitles) to be rendered at a later time. A negative value will make rendering come sooner</summary>
        void SetStreamDelay(winrt::FFmpegInteropX::IStreamInfo const& stream, winrt::Windows::Foundation::TimeSpan const& delay);

        ///<summary>Gets the presentation timestamp delay for the given stream. </summary>
        winrt::Windows::Foundation::TimeSpan GetStreamDelay(winrt::FFmpegInteropX::IStreamInfo const& stream);

        void Close();

        FFmpegMediaSource(winrt::com_ptr<MediaSourceConfig> const& interopConfig, uint64_t windowId, bool useHdr);

    private:

        HRESULT CreateMediaStreamSource(IRandomAccessStream const& stream);
        HRESULT CreateMediaStreamSource(IInputStream const& stream);
        HRESULT CreateMediaStreamSource(hstring const& uri);
        HRESULT InitFFmpegContext();
        MediaStreamSource CreateMediaStreamSource();
        MediaSource CreateMediaSource();
        std::shared_ptr<MediaSampleProvider> CreateAudioStream(AVStream* avStream, int index);
        std::shared_ptr<MediaSampleProvider> CreateVideoStream(AVStream* avStream, int index);
        std::shared_ptr<SubtitleProvider> CreateSubtitleSampleProvider(AVStream* avStream, int index);
        std::shared_ptr<MediaSampleProvider> CreateAudioSampleProvider(AVStream* avStream, AVCodecContext* avCodecCtx, int index);
        std::shared_ptr<MediaSampleProvider> CreateVideoSampleProvider(AVStream* avStream, AVCodecContext* avCodecCtx, int index, int filmGrainMode, bool eightBitZeroCopy, bool zeroCopyStream);
        HRESULT ParseOptions(PropertySet const& ffmpegOptions);
        void MediaStreamSourceClosed(MediaStreamSource const& sender, MediaStreamSourceClosedEventArgs const& args);
        void OnStarting(MediaStreamSource const& sender, MediaStreamSourceStartingEventArgs const& args);
        void OnSampleRequested(MediaStreamSource const& sender, MediaStreamSourceSampleRequestedEventArgs const& args);
        void CheckVideoDeviceChanged(MediaStreamSource const& mss);
        void OnSwitchStreamsRequested(MediaStreamSource  const& sender, MediaStreamSourceSwitchStreamsRequestedEventArgs  const& args);
        void OnAudioTracksChanged(MediaPlaybackItem  const& sender, IVectorChangedEventArgs  const& args);
        void OnPresentationModeChanged(MediaPlaybackTimedMetadataTrackList  const& sender, TimedMetadataPresentationModeChangedEventArgs  const& args);
        void InitializePlaybackItem(MediaPlaybackItem  const& playbackitem);
        bool CheckUseHardwareAcceleration(AVCodecContext* avCodecCtx, HardwareAccelerationStatus const& status, HardwareDecoderStatus& hardwareDecoderStatus, int maxProfile, int maxLevel);
        static void CheckUseHdr(winrt::com_ptr<MediaSourceConfig> const& config, bool checkDisplayInformation, bool& useHdr, uint64_t& windowId);
#ifdef Win32
        static bool Win32CheckHdr(uint64_t& windowId);
#endif
        static bool UwpCheckUseHdr(uint64_t& windowId);
        void CheckExtendDuration(MediaStreamSample sample);
        MediaThumbnailData ExtractThumbnail(AVStream* avStream);

        void Close(bool onMediaSourceClosed);
        static bool IsOnUIThread();

    public://internal:
        static IAsyncOperation<winrt::FFmpegInteropX::FFmpegMediaSource> ReadExternalSubtitleStreamAsync(IRandomAccessStream stream, hstring streamName, winrt::FFmpegInteropX::MediaSourceConfig const& config, VideoStreamDescriptor videoDescriptor, uint64_t windowId, bool useHdr);
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromStreamInternalAsync(IRandomAccessStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId);
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromInputStreamInternalAsync(IInputStream stream, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId);
        static IAsyncOperation<FFmpegInteropX::FFmpegMediaSource> CreateFromUriInternalAsync(hstring uri, FFmpegInteropX::MediaSourceConfig config, uint64_t windowId);
        static winrt::com_ptr<FFmpegMediaSource> CreateFromStream(IRandomAccessStream const& stream, winrt::com_ptr<MediaSourceConfig> const& config, uint64_t windowId, bool useHdr);
        static winrt::com_ptr<FFmpegMediaSource> CreateFromInputStream(IInputStream const& stream, winrt::com_ptr<MediaSourceConfig> const& config, uint64_t windowId, bool useHdr);
        static winrt::com_ptr<FFmpegMediaSource> CreateFromUri(hstring  const& uri, winrt::com_ptr<MediaSourceConfig>  const& config, uint64_t windowId, bool useHdr);
        HRESULT Seek(TimeSpan const& position, TimeSpan& actualPosition, bool allowFastSeek);

        std::shared_ptr<MediaSampleProvider> VideoSampleProvider()
        {
            return currentVideoStream;
        }

        std::shared_ptr<FFmpegReader> m_pReader;
        AVDictionary* avDict = nullptr;
        AVIOContext* avIOCtx = nullptr;
        AVFormatContext* avFormatCtx = nullptr;
        winrt::com_ptr<IStream> fileStreamData = { nullptr };
        IInputStream inputStream = { nullptr };
        TextEncodingDetect::Encoding streamEncoding = TextEncodingDetect::None;
        bool streamEncodingChecked = false;
        winrt::com_ptr<MediaSourceConfig> config = { nullptr };
        bool isShuttingDown = false;

        std::vector<std::shared_ptr<SubtitleProvider>> subtitleStreamProviders;

    private:

        winrt::weak_ref<MediaStreamSource> mssWeak = { nullptr };
        winrt::event_token startingRequestedToken{};
        winrt::event_token sampleRequestedToken{};
        winrt::event_token switchStreamRequestedToken{};
        winrt::event_token closeToken{};

        winrt::weak_ref<MediaPlaybackItem> playbackItemWeak = { nullptr };
        IVector<FFmpegInteropX::AudioStreamInfo> audioStrInfos = { nullptr };
        IVector<FFmpegInteropX::SubtitleStreamInfo> subtitleStrInfos = { nullptr };
        IVector<FFmpegInteropX::VideoStreamInfo> videoStrInfos = { nullptr };

        std::vector<std::shared_ptr<MediaSampleProvider>> sampleProviders;
        std::vector<std::shared_ptr<MediaSampleProvider>> audioStreams;
        std::vector<std::shared_ptr<MediaSampleProvider>> videoStreams;

        std::shared_ptr<MediaSampleProvider> currentVideoStream = { nullptr };
        std::shared_ptr<MediaSampleProvider> currentAudioStream = { nullptr };
        FFmpegInteropX::AudioStreamInfo currentAudioStreamInfo = { nullptr };
        FFmpegInteropX::VideoStreamInfo currentVideoStreamInfo = { nullptr };
        int thumbnailStreamIndex = 0;

        winrt::event_token audioTracksChangedToken{};
        winrt::event_token subtitlePresentationModeChangedToken{};

        IVectorView<FFmpegInteropX::VideoStreamInfo> videoStreamInfos = { nullptr };
        IVectorView<FFmpegInteropX::AudioStreamInfo> audioStreamInfos = { nullptr };
        IVectorView<FFmpegInteropX::SubtitleStreamInfo> subtitleStreamInfos = { nullptr };
        IVectorView<FFmpegInteropX::ChapterInfo> chapterInfos = { nullptr };
        FFmpegInteropX::FormatInfo formatInfo = { nullptr };

        std::shared_ptr<AttachedFileHelper> attachedFileHelper = { nullptr };

        std::shared_ptr<MediaMetadata> metadata = { nullptr };
        MediaThumbnailData thumbnailData = nullptr;

        std::recursive_mutex mutex;

        // Bale tuning, read from FFmpegOptions keys starting with "bale_" (never passed to FFmpeg).
        int baleDecodeAheadFrames = 0;
        int baleDav1dMaxFrameDelay = 0;
        int baleDav1dExportFilmGrain = 0;
        int baleFilmGrainMode = 0;
        // 0 = off (upstream CPU output); any nonzero value = the D3D12-writable upload texture.
        int baleD3d11Upload = 0;
        // Opt-in D3D11 10-bit PQ -> NV12 BT.709: 0 = off, 1 = BT.2390, 2 = Hable.
        int baleToneMap = 0;
        // Bale: target size of the processed output (0 = source size); fit inside, aspect kept.
        int baleToneMapOutputWidth = 0;
        int baleToneMapOutputHeight = 0;
        // Bale: 8 = the stage writes dithered NV12 when it can (an 8-bit display); 0 or 10 = P010 as before.
        int baleToneMapOutputDepth = 0;
        // 1 = software libdav1d decodes into bale_gpu zero-copy buffers (needs the D3D11 upload).
        // Zero-copy only: a stream it cannot take is refused or fails (no CPU fallback).
        int baleZeroCopy = 0;
        int baleDecodeThreadPriority = 0;
        int baleDecoderUseMaxResolution = 0;
        int baleDecoderMaxDpbCount = 0;
        // 1 = propagate container colour/static HDR attributes to system video decoders.
        int baleCompressedColorInfo = 0;
        // Bale: rav1g frames in parse and parse threads (0 = rav1g's defaults).
        int baleRav1gParse = 0;
        // Bale: 1 = rav1g's per-frame timing on the delivery line (diagnostic; process-wide callback).
        int baleRav1gPerf = 0;
        // Bale: access units a GPU decoder stream keeps in flight (0 = the provider's default, 3).
        int baleGpuDecodeInflight = 0;
        // Bale: most player-side textures a GPU decoder stream keeps (0 = no limit; below 4 acts as 4).
        int baleGpuOutputTextures = 0;
        // Bale: fast seek without a MediaPlaybackSession (e.g. IMFMediaEngine), backward-only.
        int baleFastSeekWithoutSession = 0;
        // 1 = AC-3/E-AC-3 packets go undecoded to the system Dolby decoder (keeps E-AC-3 JOC/Atmos).
        int baleSystemDecoderDolby = 0;
        // Bitmap subtitle stream (AVStream index) whose decoded rects go to bale_gpu's overlay
        // bound to bale_subtitle_sink (a token); -1/0 = none, the upstream ImageCue path.
        int baleSubtitleStream = -1;
        int baleSubtitleSink = 0;
        bool TryParseBaleOption(std::string const& key, hstring const& value, HRESULT& hr);
        void SetBaleDecoderProperties(Windows::Media::MediaProperties::VideoEncodingProperties const& videoProperties);

        // Bale decode-ahead: a worker decodes FFmpeg software video samples ahead of the
        // MediaStreamSource requests, so conversion and decode bursts overlap with playback.
        // The worker never takes `mutex`; every path that changes decoder or reader state pauses it.
        std::thread aheadThread;
        std::mutex aheadMutex;
        std::condition_variable aheadCv;
        std::deque<MediaStreamSample> aheadQueue;
        std::shared_ptr<MediaSampleProvider> aheadStream;
        uint64_t aheadGeneration = 0;
        int aheadPauses = 0;
        bool aheadBusy = false;
        bool aheadEnd = false;
        bool aheadStop = false;
        // Bale debug: QPC ticks summed per 240 produced samples, guarded by aheadMutex.
        int64_t aheadWorkTicks = 0, aheadWorkPeak = 0, aheadIdleTicks = 0, aheadStarvedTicks = 0, aheadStarvedPeak = 0;
        unsigned aheadTimedSamples = 0, aheadStarvedCount = 0;
        void AheadLoop();
        void AheadPause(bool clear);
        void AheadResume();
        void AheadShutdown();
        bool UseDecodeAhead();
        MediaStreamSample TakeAheadSample();
        struct AheadPauseScope
        {
            FFmpegMediaSource* owner;
            AheadPauseScope(FFmpegMediaSource* owner, bool clear) : owner(owner) { owner->AheadPause(clear); }
            ~AheadPauseScope() { owner->AheadResume(); }
        };

        winrt::weak_ref<MediaPlaybackSession> sessionWeak = { nullptr };
        winrt::event_token sessionPositionEvent{};

        TimeSpan mediaDuration{};

        AVBufferRef* avHardwareContext = nullptr;
        AVBufferRef* avHardwareContextDefault = nullptr;
        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> deviceContext;
        HANDLE deviceHandle = nullptr;
        com_ptr<IMFDXGIDeviceManager> deviceManager;

        // Bale: upload-only manager/handle, never an FFmpeg hardware device context.
        com_ptr<IMFDXGIDeviceManager> uploadDeviceManager;
        HANDLE uploadDeviceHandle = nullptr;
        unsigned uploadSamplesIssued = 0; // Under mutex; first three texture samples per source only.
        void InitializeUploadDevice(MediaStreamSource const& sender);
        void DisableUploadStreams(HRESULT hr);
        void CloseUploadDevice();
        // Bale: true once the current video stream's zero-copy failure was raised (NotifyError).
        bool baleFatalRaised = false; // Under mutex.
        // Bale: set by CreateVideoStream when it refused the stream (zero-copy, or a GPU decoder
        // fallback above bale_av1_fallback_max_height); a refused default stream fails the open.
        bool baleVideoRefused = false;
        bool RaiseBaleFatal(MediaStreamSource const& sender);
        bool HasZeroCopyStream();
        bool HasStagedGrainStream();

        // Bale: 1 = AV1 decodes on the GPU with rav1g (GpuDecoderSampleProvider); 0 = the FFmpeg paths.
        // Falls back to them at open when rav1g cannot take the stream.
        int baleAv1Decoder = 0;
        // Bale: with the GPU AV1 decoder requested, the tallest source the software fallback may take
        // when that decoder declines at open; a taller one is refused, so the app replans (0 = no limit).
        int baleAv1FallbackMaxHeight = 0;
        // Bale experiment: 1 = probe whether decoder texture arrays take shader views and, if so,
        // make the d3d11va decoder pools shader-readable (BaleD3D11vaShaderResource.h); 0 = as upstream.
        int baleD3d11vaShaderResource = 0;
        std::shared_ptr<MediaSampleProvider> CreateGpuDecoderStream(AVStream* avStream, int index);
        // Image pools of the GPU decoder streams, filled at open and never changed afterwards: a
        // Starting or Close request cancels their waits before it takes `mutex` (a decoder waiting for
        // an image may hold it on the sample thread).
        std::vector<std::shared_ptr<GpuDecoderImagePool>> gpuDecoderPools;
        void CancelGpuDecoderWaits();
        void PauseGpuDecoderFeeders(bool pause);
        void StopGpuDecoderFeeders();
        // Renderer device handle for the GPU decoder streams (not an FFmpeg hardware device).
        com_ptr<IMFDXGIDeviceManager> gpuDecoderDeviceManager;
        HANDLE gpuDecoderDeviceHandle = nullptr;
        void InitializeGpuDecoderDevice(MediaStreamSource const& sender);
        void CloseGpuDecoderDevice();
        bool HasGpuDecoderStream();

        uint64_t windowId;
        bool useHdr;

        bool isFirstSeek;
        bool isFirstSeekAfterStreamSwitch = false;

        int lastDurationExtension = 0;

        bool isClosed = false;

        TimeSpan currentPosition{ 0 };
        TimeSpan lastPosition{ 0 };
        TimeSpan lastSeek{ 0 };

        void OnPositionChanged(MediaPlaybackSession const& sender, IInspectable const& args);
    };
}
namespace winrt::FFmpegInteropX::factory_implementation
{
    struct FFmpegMediaSource : FFmpegMediaSourceT<FFmpegMediaSource, implementation::FFmpegMediaSource>
    {
    };
}
