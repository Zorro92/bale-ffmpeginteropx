#pragma once
#include "pch.h"
#include "FFmpegUtils.h"
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
}

// Sizes the d3d11va decoder's frame pool from the stream instead of the worst case.
//
// FFmpeg allocates the pool once, as one texture array that cannot grow, with 4 surfaces plus 16 for
// H.264 and HEVC (the most pictures either standard lets a stream keep). At 4K 10-bit that is
// 20 x 23.7 MiB = 478 MiB. An HEVC stream states what it needs in its SPS
// (sps_max_dec_pic_buffering_minus1 + 1, the decoded picture buffer including the current picture);
// films typically need 5 or 6. With this get_format the pool gets that many plus Margin surfaces
// (frames FFmpeg holds for output and the provider's brief use while it copies or processes each
// frame), never more than FFmpeg would allocate. Anything it cannot read (no HEVC SPS in the codec
// configuration, a parse error, another codec) keeps FFmpeg's own pool.
//
// Limit: an SPS that arrives later in the stream with a larger buffer and no format change keeps the
// pool sized for the first one; decoding then runs out of surfaces ("Static surface pool size
// exceeded"). The margin covers normal streams; the option is opt-in for that reason.
class D3D11DecoderPoolSize
{
public:
    static constexpr int Margin = 3;

    // Use instead of FFmpegUtils::get_format.
    static AVPixelFormat GetFormat(AVCodecContext* context, const AVPixelFormat* formats)
    {
        auto result = FFmpegUtils::get_format(context, formats);
        if (result != AV_PIX_FMT_D3D11 || !context->hw_device_ctx) return result;
        int buffer = DecodedPictureBuffer(context);
        if (buffer <= 0)
        {
            av_log(nullptr, AV_LOG_INFO, "D3D11 decoder pool: stream buffer unknown (codec %d); FFmpeg's default\n",
                static_cast<int>(context->codec_id));
            return result;
        }
        AVBufferRef* frames = nullptr;
        if (avcodec_get_hw_frames_parameters(context, context->hw_device_ctx, AV_PIX_FMT_D3D11, &frames) < 0 || !frames)
        {
            return result;
        }
        auto framesContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
        int defaultSize = framesContext->initial_pool_size;
        int size = buffer + Margin;
        if (defaultSize > 0 && size >= defaultSize)
        {
            av_buffer_unref(&frames);
            return result;
        }
        framesContext->initial_pool_size = size;
        int error = av_hwframe_ctx_init(frames);
        if (error < 0)
        {
            av_log(nullptr, AV_LOG_WARNING, "D3D11 decoder pool: init failed (%d); FFmpeg's default\n", error);
            av_buffer_unref(&frames);
            return result;
        }
        av_buffer_unref(&context->hw_frames_ctx);
        context->hw_frames_ctx = frames;
        av_log(nullptr, AV_LOG_INFO, "D3D11 decoder pool: %d surfaces %dx%d (stream buffer %d, FFmpeg default %d)\n",
            size, framesContext->width, framesContext->height, buffer, defaultSize);
        return result;
    }

    // The stream's decoded picture buffer from its codec configuration, 0 when unknown.
    static int DecodedPictureBuffer(const AVCodecContext* context)
    {
        if (context->codec_id != AV_CODEC_ID_HEVC || !context->extradata || context->extradata_size < 4) return 0;
        int result = 0;
        ForEachHevcNal(context->extradata, context->extradata_size, [&](const uint8_t* nal, size_t size)
        {
            if (size < 3 || ((nal[0] >> 1) & 0x3f) != 33) return; // SPS
            int buffer = HevcSpsBuffer(nal + 2, size - 2);
            if (buffer > result) result = buffer;
        });
        return result;
    }

private:
    // hvcC (configurationVersion 1) or Annex B start codes.
    template <typename F>
    static void ForEachHevcNal(const uint8_t* data, int size, F&& f)
    {
        if (data[0] == 1 && size >= 23)
        {
            int offset = 22;
            int arrays = data[offset++];
            for (int a = 0; a < arrays && offset + 3 <= size; a++)
            {
                offset++; // array_completeness, reserved, NAL_unit_type
                int count = (data[offset] << 8) | data[offset + 1];
                offset += 2;
                for (int n = 0; n < count && offset + 2 <= size; n++)
                {
                    int length = (data[offset] << 8) | data[offset + 1];
                    offset += 2;
                    if (length <= 0 || offset + length > size) return;
                    f(data + offset, static_cast<size_t>(length));
                    offset += length;
                }
            }
            return;
        }
        // Annex B: NAL units between 00 00 01 start codes.
        int start = -1;
        for (int i = 0; i + 2 < size; i++)
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            {
                if (start >= 0) f(data + start, static_cast<size_t>(TrimZeros(data, start, i)));
                start = i + 3;
                i += 2;
            }
        }
        if (start >= 0 && start < size) f(data + start, static_cast<size_t>(size - start));
    }

    static int TrimZeros(const uint8_t* data, int start, int end)
    {
        while (end > start && data[end - 1] == 0) end--;
        return end - start;
    }

    class BitReader
    {
    public:
        BitReader(const uint8_t* data, size_t size)
        {
            // Without emulation prevention bytes (00 00 03).
            bytes.reserve(size);
            int zeros = 0;
            for (size_t i = 0; i < size; i++)
            {
                if (zeros >= 2 && data[i] == 3)
                {
                    zeros = 0;
                    continue;
                }
                zeros = data[i] == 0 ? zeros + 1 : 0;
                bytes.push_back(data[i]);
            }
        }

        bool Failed() const { return failed; }

        uint32_t Bits(int count)
        {
            uint32_t value = 0;
            for (int i = 0; i < count; i++)
            {
                if (position >= bytes.size() * 8)
                {
                    failed = true;
                    return 0;
                }
                value = (value << 1) | ((bytes[position / 8] >> (7 - position % 8)) & 1);
                position++;
            }
            return value;
        }

        void Skip(int count)
        {
            for (int i = 0; i < count && !failed; i++) Bits(1);
        }

        // ue(v)
        uint32_t Golomb()
        {
            int zeros = 0;
            while (!failed && Bits(1) == 0)
            {
                if (++zeros > 31)
                {
                    failed = true;
                    return 0;
                }
            }
            if (failed) return 0;
            return (zeros ? Bits(zeros) : 0) + (1u << zeros) - 1;
        }

    private:
        std::vector<uint8_t> bytes;
        size_t position = 0;
        bool failed = false;
    };

    // H.265 7.3.2.2.1, up to sps_max_dec_pic_buffering_minus1; the largest over the sub-layers, + 1.
    static int HevcSpsBuffer(const uint8_t* rbsp, size_t size)
    {
        BitReader bits(rbsp, size);
        bits.Skip(4); // sps_video_parameter_set_id
        int maxSubLayersMinus1 = static_cast<int>(bits.Bits(3));
        bits.Skip(1); // sps_temporal_id_nesting_flag
        // profile_tier_level(1, maxSubLayersMinus1): the general part is 96 bits.
        bits.Skip(96);
        bool profilePresent[8]{};
        bool levelPresent[8]{};
        for (int i = 0; i < maxSubLayersMinus1; i++)
        {
            profilePresent[i] = bits.Bits(1) != 0;
            levelPresent[i] = bits.Bits(1) != 0;
        }
        if (maxSubLayersMinus1 > 0)
        {
            for (int i = maxSubLayersMinus1; i < 8; i++) bits.Skip(2);
        }
        for (int i = 0; i < maxSubLayersMinus1; i++)
        {
            if (profilePresent[i]) bits.Skip(88);
            if (levelPresent[i]) bits.Skip(8);
        }
        bits.Golomb(); // sps_seq_parameter_set_id
        if (bits.Golomb() == 3) bits.Skip(1); // chroma_format_idc, separate_colour_plane_flag
        bits.Golomb(); // pic_width_in_luma_samples
        bits.Golomb(); // pic_height_in_luma_samples
        if (bits.Bits(1)) // conformance_window_flag
        {
            for (int i = 0; i < 4; i++) bits.Golomb();
        }
        bits.Golomb(); // bit_depth_luma_minus8
        bits.Golomb(); // bit_depth_chroma_minus8
        bits.Golomb(); // log2_max_pic_order_cnt_lsb_minus4
        bool orderingPresent = bits.Bits(1) != 0;
        uint32_t buffer = 0;
        for (int i = orderingPresent ? 0 : maxSubLayersMinus1; i <= maxSubLayersMinus1; i++)
        {
            uint32_t decPicBufferingMinus1 = bits.Golomb();
            bits.Golomb(); // sps_max_num_reorder_pics
            bits.Golomb(); // sps_max_latency_increase_plus1
            if (decPicBufferingMinus1 + 1 > buffer) buffer = decPicBufferingMinus1 + 1;
        }
        if (bits.Failed() || buffer < 1 || buffer > 16) return 0;
        return static_cast<int>(buffer);
    }
};
