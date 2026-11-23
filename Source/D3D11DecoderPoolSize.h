#pragma once
#include "pch.h"
#include "FFmpegUtils.h"
#include <algorithm>
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
// H.264 and HEVC (the most pictures either standard lets a stream keep) and plus 8 for VP9. At 4K
// 10-bit that is 20 x 23.7 MiB = 478 MiB. What a stream needs, current picture included:
// - HEVC: sps_max_dec_pic_buffering_minus1 + 1 from its SPS (films typically 5 or 6);
// - H.264: max_dec_frame_buffering + 1 from its SPS (VUI bitstream restriction), or, without it, the
//   level's MaxDpbMbs over the picture size (at most 16), and never fewer than max_num_ref_frames;
// - VP9: its 8 reference slots + 1, fixed by the format.
// With this get_format the pool gets that many plus a margin, never more than FFmpeg would allocate:
// 3 for HEVC and H.264 (frames FFmpeg holds back for output order, and the provider's brief use while
// it copies or processes each frame), 2 for VP9, which never holds frames back for output. Anything it
// cannot read (no SPS in the codec configuration, a parse error, another codec) keeps FFmpeg's pool.
//
// Limit: an SPS that arrives later in the stream with a larger buffer and no format change keeps the
// pool sized for the first one; decoding then runs out of surfaces ("Static surface pool size
// exceeded"). The margin covers normal streams; the option is opt-in for that reason.
class D3D11DecoderPoolSize
{
public:
    static constexpr int Margin = 3;
    static constexpr int Vp9Margin = 2;

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
        int size = buffer + (context->codec_id == AV_CODEC_ID_VP9 ? Vp9Margin : Margin);
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

    // The pictures the stream's decoder holds, current picture included, from its codec configuration
    // (fixed for VP9); 0 when unknown.
    static int DecodedPictureBuffer(const AVCodecContext* context)
    {
        if (context->codec_id == AV_CODEC_ID_VP9) return 9; // 8 reference slots + the current frame
        bool hevc = context->codec_id == AV_CODEC_ID_HEVC;
        if ((!hevc && context->codec_id != AV_CODEC_ID_H264) || !context->extradata || context->extradata_size < 4) return 0;
        int result = 0;
        auto visit = [&](const uint8_t* nal, size_t size)
        {
            int buffer = 0;
            if (hevc && size >= 3 && ((nal[0] >> 1) & 0x3f) == 33) buffer = HevcSpsBuffer(nal + 2, size - 2);
            else if (!hevc && size >= 2 && (nal[0] & 0x1f) == 7) buffer = H264SpsBuffer(nal + 1, size - 1);
            if (buffer > result) result = buffer;
        };
        if (hevc) ForEachHevcNal(context->extradata, context->extradata_size, visit);
        else ForEachH264Nal(context->extradata, context->extradata_size, visit);
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
        ForEachAnnexBNal(data, size, f);
    }

    // avcC (configurationVersion 1): its SPSs; otherwise Annex B start codes.
    template <typename F>
    static void ForEachH264Nal(const uint8_t* data, int size, F&& f)
    {
        if (data[0] == 1)
        {
            if (size < 6) return;
            int count = data[5] & 0x1f;
            int offset = 6;
            for (int n = 0; n < count && offset + 2 <= size; n++)
            {
                int length = (data[offset] << 8) | data[offset + 1];
                offset += 2;
                if (length <= 0 || offset + length > size) return;
                f(data + offset, static_cast<size_t>(length));
                offset += length;
            }
            return;
        }
        ForEachAnnexBNal(data, size, f);
    }

    template <typename F>
    static void ForEachAnnexBNal(const uint8_t* data, int size, F&& f)
    {
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

        // se(v)
        int32_t SignedGolomb()
        {
            uint32_t value = Golomb();
            return (value & 1) ? static_cast<int32_t>((value + 1) / 2) : -static_cast<int32_t>(value / 2);
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

    // H.264 7.3.2.1.1 and E.1.1, up to max_dec_frame_buffering; + 1 for the current picture.
    static int H264SpsBuffer(const uint8_t* rbsp, size_t size)
    {
        BitReader bits(rbsp, size);
        uint32_t profile = bits.Bits(8);
        uint32_t constraints = bits.Bits(8);
        uint32_t level = bits.Bits(8);
        bits.Golomb(); // seq_parameter_set_id
        switch (profile)
        {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128:
        case 138: case 139: case 134: case 135:
        {
            uint32_t chroma = bits.Golomb();
            if (chroma == 3) bits.Skip(1); // separate_colour_plane_flag
            bits.Golomb(); // bit_depth_luma_minus8
            bits.Golomb(); // bit_depth_chroma_minus8
            bits.Skip(1); // qpprime_y_zero_transform_bypass_flag
            if (bits.Bits(1)) // seq_scaling_matrix_present_flag
            {
                int lists = chroma == 3 ? 12 : 8;
                for (int i = 0; i < lists && !bits.Failed(); i++)
                {
                    if (!bits.Bits(1)) continue;
                    int count = i < 6 ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < count && !bits.Failed(); j++)
                    {
                        if (next != 0) next = (last + bits.SignedGolomb() + 256) % 256;
                        last = next == 0 ? last : next;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        bits.Golomb(); // log2_max_frame_num_minus4
        uint32_t pocType = bits.Golomb();
        if (pocType == 0)
        {
            bits.Golomb(); // log2_max_pic_order_cnt_lsb_minus4
        }
        else if (pocType == 1)
        {
            bits.Skip(1); // delta_pic_order_always_zero_flag
            bits.SignedGolomb(); // offset_for_non_ref_pic
            bits.SignedGolomb(); // offset_for_top_to_bottom_field
            uint32_t cycle = bits.Golomb();
            if (cycle > 255) return 0;
            for (uint32_t i = 0; i < cycle; i++) bits.SignedGolomb();
        }
        uint32_t refFrames = bits.Golomb(); // max_num_ref_frames
        bits.Skip(1); // gaps_in_frame_num_value_allowed_flag
        uint32_t widthInMbs = bits.Golomb() + 1;
        uint32_t heightInMapUnits = bits.Golomb() + 1;
        uint32_t frameMbsOnly = bits.Bits(1);
        if (!frameMbsOnly) bits.Skip(1); // mb_adaptive_frame_field_flag
        bits.Skip(1); // direct_8x8_inference_flag
        if (bits.Bits(1)) // frame_cropping_flag
        {
            for (int i = 0; i < 4; i++) bits.Golomb();
        }
        if (bits.Failed()) return 0;
        int buffer = -1;
        if (bits.Bits(1)) buffer = H264VuiBuffer(bits); // vui_parameters_present_flag
        if (bits.Failed()) buffer = -1;
        if (buffer < 0)
        {
            // A.3.1: MaxDpbFrames = Min(MaxDpbMbs / (PicWidthInMbs * FrameHeightInMbs), 16).
            uint64_t frameMbs = static_cast<uint64_t>(widthInMbs) * heightInMapUnits * (frameMbsOnly ? 1 : 2);
            uint32_t maxDpbMbs = H264MaxDpbMbs(level, profile, constraints);
            if (!maxDpbMbs || !frameMbs) return 0;
            buffer = static_cast<int>((std::min)(maxDpbMbs / frameMbs, static_cast<uint64_t>(16)));
        }
        buffer = (std::max)(buffer, static_cast<int>(refFrames));
        if (buffer < 1 || buffer > 16) return 0;
        return buffer + 1;
    }

    // max_dec_frame_buffering, or -1 when the VUI has no bitstream restriction.
    static int H264VuiBuffer(BitReader& bits)
    {
        if (bits.Bits(1)) // aspect_ratio_info_present_flag
        {
            if (bits.Bits(8) == 255) bits.Skip(32); // Extended_SAR: sar_width, sar_height
        }
        if (bits.Bits(1)) bits.Skip(1); // overscan_info_present_flag, overscan_appropriate_flag
        if (bits.Bits(1)) // video_signal_type_present_flag
        {
            bits.Skip(4); // video_format, video_full_range_flag
            if (bits.Bits(1)) bits.Skip(24); // colour_description_present_flag
        }
        if (bits.Bits(1)) // chroma_loc_info_present_flag
        {
            bits.Golomb();
            bits.Golomb();
        }
        if (bits.Bits(1)) bits.Skip(65); // timing_info_present_flag: num_units_in_tick, time_scale, fixed_frame_rate_flag
        bool nalHrd = bits.Bits(1) != 0;
        if (nalHrd) SkipHrd(bits);
        bool vclHrd = bits.Bits(1) != 0;
        if (vclHrd) SkipHrd(bits);
        if (nalHrd || vclHrd) bits.Skip(1); // low_delay_hrd_flag
        bits.Skip(1); // pic_struct_present_flag
        if (!bits.Bits(1)) return -1; // bitstream_restriction_flag
        bits.Skip(1); // motion_vectors_over_pic_boundaries_flag
        bits.Golomb(); // max_bytes_per_pic_denom
        bits.Golomb(); // max_bits_per_mb_denom
        bits.Golomb(); // log2_max_mv_length_horizontal
        bits.Golomb(); // log2_max_mv_length_vertical
        bits.Golomb(); // max_num_reorder_frames
        uint32_t buffering = bits.Golomb(); // max_dec_frame_buffering
        return buffering > 16 ? -1 : static_cast<int>(buffering);
    }

    // E.1.2
    static void SkipHrd(BitReader& bits)
    {
        uint32_t count = bits.Golomb() + 1; // cpb_cnt_minus1
        if (count > 32) count = 32;
        bits.Skip(8); // bit_rate_scale, cpb_size_scale
        for (uint32_t i = 0; i < count && !bits.Failed(); i++)
        {
            bits.Golomb(); // bit_rate_value_minus1
            bits.Golomb(); // cpb_size_value_minus1
            bits.Skip(1); // cbr_flag
        }
        bits.Skip(20); // four delay and offset lengths, 5 bits each
    }

    // Table A-1 MaxDpbMbs by level_idc (level 1b: 11 with constraint_set3 in Baseline, Main, Extended).
    static uint32_t H264MaxDpbMbs(uint32_t level, uint32_t profile, uint32_t constraints)
    {
        if (level == 11 && (constraints & 0x10) && (profile == 66 || profile == 77 || profile == 88)) return 396;
        switch (level)
        {
        case 9: case 10: return 396;
        case 11: return 900;
        case 12: case 13: case 20: return 2376;
        case 21: return 4752;
        case 22: case 30: return 8100;
        case 31: return 18000;
        case 32: return 20480;
        case 40: case 41: return 32768;
        case 42: return 34816;
        case 50: return 110400;
        case 51: case 52: return 184320;
        case 60: case 61: case 62: return 696320;
        default: return 0;
        }
    }
};
