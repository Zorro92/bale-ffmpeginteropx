#pragma once
#include "pch.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
extern "C"
{
#include <libavutil/film_grain_params.h>
}

// Grain ABI 1, mirrored from bale-gpu/src/grain/params.rs. No WinRT/public ABI changes.
struct BaleGrainParams
{
    uint32_t struct_size;
    uint32_t seed;
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t matrix_identity;
    uint8_t num_y_points;
    uint8_t chroma_scaling_from_luma;
    uint8_t num_uv_points[2];
    uint8_t scaling_shift;
    uint8_t ar_coeff_lag;
    uint8_t ar_coeff_shift;
    uint8_t grain_scale_shift;
    uint8_t overlap_flag;
    uint8_t clip_to_restricted_range;
    uint8_t y_points[14][2];
    uint8_t uv_points[2][10][2];
    int8_t ar_coeffs_y[24];
    int8_t ar_coeffs_uv[2][25];
    uint8_t reserved0[2];
    int16_t uv_mult[2];
    int16_t uv_luma_mult[2];
    int16_t uv_offset[2];
};
static_assert(sizeof(BaleGrainParams) == 184);
static_assert(alignof(BaleGrainParams) == 4);
static_assert(offsetof(BaleGrainParams, struct_size) == 0);
static_assert(offsetof(BaleGrainParams, seed) == 4);
static_assert(offsetof(BaleGrainParams, width) == 8);
static_assert(offsetof(BaleGrainParams, height) == 12);
static_assert(offsetof(BaleGrainParams, bit_depth) == 16);
static_assert(offsetof(BaleGrainParams, matrix_identity) == 17);
static_assert(offsetof(BaleGrainParams, num_y_points) == 18);
static_assert(offsetof(BaleGrainParams, chroma_scaling_from_luma) == 19);
static_assert(offsetof(BaleGrainParams, num_uv_points) == 20);
static_assert(offsetof(BaleGrainParams, scaling_shift) == 22);
static_assert(offsetof(BaleGrainParams, ar_coeff_lag) == 23);
static_assert(offsetof(BaleGrainParams, ar_coeff_shift) == 24);
static_assert(offsetof(BaleGrainParams, grain_scale_shift) == 25);
static_assert(offsetof(BaleGrainParams, overlap_flag) == 26);
static_assert(offsetof(BaleGrainParams, clip_to_restricted_range) == 27);
static_assert(offsetof(BaleGrainParams, y_points) == 28);
static_assert(offsetof(BaleGrainParams, uv_points) == 56);
static_assert(offsetof(BaleGrainParams, ar_coeffs_y) == 96);
static_assert(offsetof(BaleGrainParams, ar_coeffs_uv) == 120);
static_assert(offsetof(BaleGrainParams, reserved0) == 170);
static_assert(offsetof(BaleGrainParams, uv_mult) == 172);
static_assert(offsetof(BaleGrainParams, uv_luma_mult) == 176);
static_assert(offsetof(BaleGrainParams, uv_offset) == 180);

struct BaleGrainExports
{
    uint32_t (__cdecl* abiVersion)() = nullptr;
    HRESULT (__cdecl* validate)(const BaleGrainParams*) = nullptr;
    HRESULT (__cdecl* cpuApply)(const BaleGrainParams*, uint32_t, uint8_t*, ptrdiff_t, uint8_t*, ptrdiff_t, uint32_t) = nullptr;
    HRESULT (__cdecl* gpuCreate)(void*, void*, void**) = nullptr;
    HRESULT (__cdecl* gpuApply)(void*, const BaleGrainParams*, void*, void*) = nullptr;
    void (__cdecl* gpuDestroy)(void*) = nullptr;

    static const BaleGrainExports& Get()
    {
        // Thread-safe C++ static initialization: exactly one load attempt per process. Keep a
        // successful module loaded for the process lifetime, including all stream destructors.
        static const BaleGrainExports exports = []
        {
            BaleGrainExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module) return api;
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_grain_abi_version"));
            api.validate = reinterpret_cast<decltype(api.validate)>(GetProcAddress(module, "bale_grain_validate"));
            api.cpuApply = reinterpret_cast<decltype(api.cpuApply)>(GetProcAddress(module, "bale_grain_cpu_apply"));
            api.gpuCreate = reinterpret_cast<decltype(api.gpuCreate)>(GetProcAddress(module, "bale_grain_gpu_create"));
            api.gpuApply = reinterpret_cast<decltype(api.gpuApply)>(GetProcAddress(module, "bale_grain_gpu_apply"));
            api.gpuDestroy = reinterpret_cast<decltype(api.gpuDestroy)>(GetProcAddress(module, "bale_grain_gpu_destroy"));
            if (!api.abiVersion || !api.validate || !api.cpuApply || !api.gpuCreate || !api.gpuApply || !api.gpuDestroy || api.abiVersion() != 1)
            {
                FreeLibrary(module);
                return BaleGrainExports{};
            }
            return api;
        }();
        return exports;
    }
};

// Owned by one software-video stream. Decode-ahead/MSS production and seek/close teardown
// are serialized by FFmpegMediaSource's AheadPause/AheadShutdown and main mutex. Ownership
// can move between threads; Processed callbacks never access grain state or the GPU handle.
class BaleFilmGrain
{
public:
    int mode = 0; // 0 dav1d at open / no grain after GPU loss, 1 Rust CPU, 2 GPU; never reset on seek.
    bool above1080p = false; // Initial codec size class; runtime GPU fallback must not overload 4K decode.

    static int BitDepth(AVPixelFormat format)
    {
        switch (format)
        {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_NV12: return 8;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_P010LE: return 10;
        default: return 0;
        }
    }

    static bool Compatible(AVPixelFormat input, AVPixelFormat output)
    {
        return (BitDepth(input) == 8 && output == AV_PIX_FMT_NV12) ||
            (BitDepth(input) == 10 && output == AV_PIX_FMT_P010LE);
    }

    // Must run before avcodec_open2: libdav1d reads apply_grain only during initialization.
    static int SelectMode(int requested, bool exportOnly, bool dav1d, bool hardware,
        AVPixelFormat input, AVPixelFormat output, int uploadVariant)
    {
        int selected = 0;
        const char* reason = nullptr;
        if (exportOnly)
            reason = requested ? "export-only override; mode ignored" : "export-only override";
        else if (requested)
        {
            if (!dav1d || hardware) reason = "decoder is not software libdav1d";
            else if (!BitDepth(input)) reason = "input is not 4:2:0 8/10-bit";
            else if (!Compatible(input, output)) reason = "output is not matching NV12/P010";
            else if (!BaleGrainExports::Get().validate) reason = "grain DLL unavailable or incompatible";
            else
            {
                selected = requested;
                // After patch 0008 only variants 1 and 6 (0016) have D3D11_BIND_RENDER_TARGET.
                if (selected == 2 && uploadVariant != 1 && uploadVariant != 6)
                {
                    // Keep dav1d's faster synthesis while export can still be left off.
                    selected = 0;
                    reason = "GPU requires render-target upload variant 1; keeping dav1d";
                }
            }
        }
        const char* name = selected == 2 ? "gpu" : selected == 1 ? "cpu" : "dav1d";
        if (reason) av_log(nullptr, AV_LOG_INFO, "Bale film grain: mode=%s (%s)\n", name, reason);
        else av_log(nullptr, AV_LOG_INFO, "Bale film grain: mode=%s\n", name);
        return selected;
    }

    bool Prepare(AVFrame* frame, AVPixelFormat output, int width, int height, BaleGrainParams& params)
    {
        // Runtime mode 0 leaves exported frames ungrained; dav1d cannot resume without reopening.
        if (!mode) return false;
        auto side = av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
        if (!side) return false; // Grain is per-frame, never reuse the previous frame's parameters.
        if (side->size < sizeof(AVFilmGrainParamsType)) return Invalid();
        auto source = reinterpret_cast<const AVFilmGrainParams*>(side->data);
        if (source->type != AV_FILM_GRAIN_PARAMS_AV1) return false;
        if (side->size < offsetof(AVFilmGrainParams, codec) + sizeof(AVFilmGrainAOMParams)) return Invalid();
        if (width != frame->width || height != frame->height)
        {
            if (!scalingLogged)
            {
                scalingLogged = true;
                av_log(nullptr, AV_LOG_WARNING, "Bale film grain: scaled output; showing ungrained frame.\n");
            }
            return false;
        }
        if (!Compatible(static_cast<AVPixelFormat>(frame->format), output))
        {
            if (!formatLogged)
            {
                formatLogged = true;
                av_log(nullptr, AV_LOG_WARNING, "Bale film grain: unsupported frame format; showing ungrained frame.\n");
            }
            return false;
        }
        if (!Map(frame, *source, params) || BaleGrainExports::Get().validate(&params) != S_OK) return Invalid();
        return true;
    }

    bool ApplyCpu(const BaleGrainParams& params, AVPixelFormat output, uint8_t** data, const int* pitch)
    {
        auto hr = BaleGrainExports::Get().cpuApply(&params, output == AV_PIX_FMT_NV12 ? 1U : 2U,
            data[0], pitch[0], data[1], pitch[1], 0);
        if (hr == S_OK) return true;
        if (!cpuLogged)
        {
            cpuLogged = true;
            av_log(nullptr, AV_LOG_WARNING, "Bale film grain: CPU apply failed (hr=0x%08X); showing ungrained frame.\n", static_cast<unsigned>(hr));
        }
        // Caller must refill its owned output: a contained Rust panic need not roll back writes.
        return false;
    }

    void GpuFailed(HRESULT hr, bool creating)
    {
        if (mode != 2) return;
        mode = above1080p ? 0 : 1;
        if (!gpuLogged)
        {
            gpuLogged = true;
            if (above1080p)
            {
                if (creating) av_log(nullptr, AV_LOG_WARNING, "Bale film grain: GPU create failed (hr=0x%08X); showing ungrained frames (above 1080p).\n", static_cast<unsigned>(hr));
                else av_log(nullptr, AV_LOG_WARNING, "Bale film grain: GPU apply failed (hr=0x%08X); showing ungrained frames (above 1080p).\n", static_cast<unsigned>(hr));
            }
            else
            {
                if (creating) av_log(nullptr, AV_LOG_WARNING, "Bale film grain: GPU create failed (hr=0x%08X); using CPU grain.\n", static_cast<unsigned>(hr));
                else av_log(nullptr, AV_LOG_WARNING, "Bale film grain: GPU apply failed (hr=0x%08X); using CPU grain.\n", static_cast<unsigned>(hr));
            }
        }
    }

    void UploadDisabled(HRESULT hr)
    {
        if (mode != 2) return;
        mode = above1080p ? 0 : 1;
        if (!uploadLogged)
        {
            uploadLogged = true;
            if (above1080p) av_log(nullptr, AV_LOG_WARNING, "Bale film grain: upload disabled (hr=0x%08X); showing ungrained frames (above 1080p).\n", static_cast<unsigned>(hr));
            else av_log(nullptr, AV_LOG_WARNING, "Bale film grain: upload disabled (hr=0x%08X); using CPU grain.\n", static_cast<unsigned>(hr));
        }
    }

    void NoUpload()
    {
        if (mode != 2) return;
        mode = above1080p ? 0 : 1;
        if (!missingUploadLogged)
        {
            missingUploadLogged = true;
            if (above1080p) av_log(nullptr, AV_LOG_WARNING, "Bale film grain: upload unavailable; showing ungrained frames (above 1080p).\n");
            else av_log(nullptr, AV_LOG_WARNING, "Bale film grain: upload unavailable; using CPU grain.\n");
        }
    }

private:
    bool invalidLogged = false, scalingLogged = false, formatLogged = false;
    bool cpuLogged = false, gpuLogged = false, uploadLogged = false, missingUploadLogged = false;

    bool Invalid()
    {
        if (!invalidLogged)
        {
            invalidLogged = true;
            av_log(nullptr, AV_LOG_WARNING, "Bale film grain: invalid parameters; showing ungrained frame.\n");
        }
        return false;
    }

    static bool Range(int value, int low, int high) { return value >= low && value <= high; }

    static bool Map(const AVFrame* frame, const AVFilmGrainParams& source, BaleGrainParams& params)
    {
        const auto& aom = source.codec.aom;
        // Check before narrowing or indexing. Rust validation remains authoritative afterwards.
        if (frame->width <= 0 || frame->height <= 0 ||
            !Range(aom.num_y_points, 0, 14) || !Range(aom.num_uv_points[0], 0, 10) || !Range(aom.num_uv_points[1], 0, 10) ||
            !Range(aom.chroma_scaling_from_luma, 0, 1) || !Range(aom.scaling_shift, 8, 11) ||
            !Range(aom.ar_coeff_lag, 0, 3) || !Range(aom.ar_coeff_shift, 6, 9) ||
            !Range(aom.grain_scale_shift, 0, 3) || !Range(aom.overlap_flag, 0, 1) || !Range(aom.limit_output_range, 0, 1)) return false;
        for (int plane = 0; plane < 2; plane++)
        {
            if (!Range(aom.uv_mult[plane], -128, 127) || !Range(aom.uv_mult_luma[plane], -128, 127) ||
                !Range(aom.uv_offset[plane], -256, 255)) return false;
        }
        params = {}; // All unused points, coefficients and reserved0 must be zero.
        params.struct_size = sizeof(BaleGrainParams);
        params.seed = static_cast<uint32_t>(source.seed & 0xffff);
        params.width = frame->width;
        params.height = frame->height;
        params.bit_depth = static_cast<uint8_t>(BitDepth(static_cast<AVPixelFormat>(frame->format)));
        params.matrix_identity = frame->colorspace == AVCOL_SPC_RGB;
        params.num_y_points = static_cast<uint8_t>(aom.num_y_points);
        params.chroma_scaling_from_luma = static_cast<uint8_t>(aom.chroma_scaling_from_luma);
        params.scaling_shift = static_cast<uint8_t>(aom.scaling_shift);
        params.ar_coeff_lag = static_cast<uint8_t>(aom.ar_coeff_lag);
        params.ar_coeff_shift = static_cast<uint8_t>(aom.ar_coeff_shift);
        params.grain_scale_shift = static_cast<uint8_t>(aom.grain_scale_shift);
        params.overlap_flag = static_cast<uint8_t>(aom.overlap_flag);
        params.clip_to_restricted_range = static_cast<uint8_t>(aom.limit_output_range);
        std::memcpy(params.y_points, aom.y_points, aom.num_y_points * sizeof(params.y_points[0]));
        int yCoeffs = 2 * aom.ar_coeff_lag * (aom.ar_coeff_lag + 1);
        if (aom.num_y_points) std::memcpy(params.ar_coeffs_y, aom.ar_coeffs_y, yCoeffs);
        for (int plane = 0; plane < 2; plane++)
        {
            params.num_uv_points[plane] = static_cast<uint8_t>(aom.num_uv_points[plane]);
            std::memcpy(params.uv_points[plane], aom.uv_points[plane], aom.num_uv_points[plane] * sizeof(params.uv_points[plane][0]));
            if (aom.num_uv_points[plane] || aom.chroma_scaling_from_luma)
                std::memcpy(params.ar_coeffs_uv[plane], aom.ar_coeffs_uv[plane], yCoeffs + (aom.num_y_points != 0));
            params.uv_mult[plane] = static_cast<int16_t>(aom.uv_mult[plane]);
            params.uv_luma_mult[plane] = static_cast<int16_t>(aom.uv_mult_luma[plane]);
            params.uv_offset[plane] = static_cast<int16_t>(aom.uv_offset[plane]);
        }
        return true;
    }
};
