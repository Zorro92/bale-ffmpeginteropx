#pragma once
#include "pch.h"
#include <cstddef>
#include <cstdint>

// Caption packet tap ABI 1, mirrored from bale_gpu (bale_cc_*): every compressed packet of a video
// stream that carries in-band captions (A/53 cc_data in H.264/HEVC SEI, GA94 user data in MPEG-2),
// handed to the app's caption decoder as it is taken for decoding. The app binds its decoder to a
// token (bale_cc_sink); nothing is parsed here. No WinRT/public ABI changes.
//
// Tapped where a provider takes the packet from its buffer (MediaSampleProvider::GetNextPacket), so
// it covers every decode route (system decoder, d3d11va, software, GPU decoder), in decode order,
// and a buffered seek hands the kept packets again. Every flush of the stream (seek, stream switch,
// close) calls bale_cc_flush so the app can drop its reorder buffer.
struct BaleCaptionSinkExports
{
    uint32_t (__cdecl* abiVersion)() = nullptr;
    int32_t (__cdecl* push)(uint64_t token, const uint8_t* data, size_t len, int64_t pts100ns, uint32_t codec, uint32_t nalLengthSize) = nullptr;
    int32_t (__cdecl* flush)(uint64_t token) = nullptr;

    bool Available() const { return push && flush; }

    static const BaleCaptionSinkExports& Get()
    {
        // One load attempt per process; a successful module stays loaded for the process lifetime.
        static const BaleCaptionSinkExports exports = []
        {
            BaleCaptionSinkExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module) return api;
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_cc_abi_version"));
            api.push = reinterpret_cast<decltype(api.push)>(GetProcAddress(module, "bale_cc_push"));
            api.flush = reinterpret_cast<decltype(api.flush)>(GetProcAddress(module, "bale_cc_flush"));
            auto version = api.abiVersion ? api.abiVersion() : 0;
            if (!api.push || !api.flush || version != 1)
            {
                FreeLibrary(module);
                return BaleCaptionSinkExports{};
            }
            return api;
        }();
        return exports;
    }
};

// The tap of one video stream. Off (token 0) costs one compare per packet.
class BaleCaptionTap
{
public:
    // ABI codec values.
    static constexpr uint32_t CodecH264 = 1;
    static constexpr uint32_t CodecHevc = 2;
    static constexpr uint32_t CodecMpeg2 = 3;

    // Binds the stream to the app's token. False when the stream's codec carries no captions this
    // ABI knows, or bale_gpu.dll lacks the exports (logged once); the tap then stays off.
    bool Enable(uint64_t sinkToken, const AVCodecParameters* parameters)
    {
        token = 0;
        if (!sinkToken || !parameters) return false;
        switch (parameters->codec_id)
        {
        case AV_CODEC_ID_H264: codec = CodecH264; break;
        case AV_CODEC_ID_HEVC: codec = CodecHevc; break;
        case AV_CODEC_ID_MPEG2VIDEO: codec = CodecMpeg2; break;
        default: return false;
        }
        if (!BaleCaptionSinkExports::Get().Available())
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                av_log(nullptr, AV_LOG_WARNING, "Bale captions: sink unavailable; tap off\n");
            }
            return false;
        }
        nalLengthSize = NalLengthSize(codec, parameters->extradata, parameters->extradata_size);
        token = sinkToken;
        av_log(nullptr, AV_LOG_INFO, "Bale captions: tap on (codec %u nal_length_size %u)\n", codec, nalLengthSize);
        return true;
    }

    bool Enabled() const noexcept { return token != 0; }

    // pts100ns: the packet's presentation time on the samples' timeline, INT64_MIN for none.
    void Push(const AVPacket* packet, int64_t pts100ns) noexcept
    {
        if (!token || !packet || !packet->data || packet->size <= 0) return;
        BaleCaptionSinkExports::Get().push(token, packet->data, static_cast<size_t>(packet->size), pts100ns, codec, nalLengthSize);
        packets++;
    }

    void Flush() noexcept
    {
        if (!token) return;
        BaleCaptionSinkExports::Get().flush(token);
        flushes++;
    }

    ~BaleCaptionTap()
    {
        if (token)
        {
            av_log(nullptr, AV_LOG_INFO, "Bale captions: tap closed (packets=%llu flushes=%u)\n",
                static_cast<unsigned long long>(packets), flushes);
        }
    }

    // The NAL length field size of length-prefixed (MP4/MKV) H.264 and HEVC, from avcC/hvcC: 1, 2 or
    // 4. 0 for Annex B start codes (MPEG-TS, raw streams) and MPEG-2.
    static uint32_t NalLengthSize(uint32_t codec, const uint8_t* extradata, int size)
    {
        if (!extradata || size <= 0) return 0;
        if (codec == CodecH264)
        {
            // AVCDecoderConfigurationRecord: version 1; lengthSizeMinusOne in byte 4.
            return size >= 7 && extradata[0] == 1 ? (extradata[4] & 3u) + 1 : 0;
        }
        if (codec == CodecHevc)
        {
            // HEVCDecoderConfigurationRecord: lengthSizeMinusOne in byte 21. Annex B extradata starts
            // with a start code (00 00 01 or 00 00 00 01) instead.
            bool startCode = size >= 3 && extradata[0] == 0 && extradata[1] == 0 &&
                (extradata[2] == 1 || (size >= 4 && extradata[2] == 0 && extradata[3] == 1));
            return size >= 23 && !startCode ? (extradata[21] & 3u) + 1 : 0;
        }
        return 0;
    }

private:
    uint64_t token = 0;
    uint32_t codec = 0;
    uint32_t nalLengthSize = 0;
    uint64_t packets = 0;
    uint32_t flushes = 0;
};
