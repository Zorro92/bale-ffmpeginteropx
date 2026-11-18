#pragma once
#include "VideoFrameProcessor.h"
#include <cstddef>

extern "C"
{
#include <libavutil/pixdesc.h>
}

// Bale's tone-map ABI 1, copied verbatim from bale-for-silo native/bale-gpu/include/bale_tonemap.h
// (commit f778498). Keep it after the Windows headers (pch.h): it defines HRESULT only if they didn't.
#include "bale_tonemap.h"
static_assert(sizeof(BaleToneMapConfig) == 40, "BaleToneMapConfig must match ABI 1");
static_assert(sizeof(BaleToneMapFrameInfo) == 72, "BaleToneMapFrameInfo must match ABI 1");
static_assert(offsetof(BaleToneMapFrameInfo, mastering_primaries) == 8);
static_assert(offsetof(BaleToneMapFrameInfo, reserved) == 56);

struct BaleToneMapExports
{
    decltype(&bale_tonemap_abi_version) abiVersion = nullptr;
    decltype(&bale_tonemap_create) create = nullptr;
    decltype(&bale_tonemap_apply) apply = nullptr;
    decltype(&bale_tonemap_destroy) destroy = nullptr;
    // Optional (additive in ABI 1): absent means NV12 output only.
    decltype(&bale_tonemap_output_formats) outputFormats = nullptr;
    HRESULT status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    static const BaleToneMapExports& Get()
    {
        // One load attempt per process; a usable module stays loaded for every handle's lifetime.
        static const BaleToneMapExports exports = []
        {
            BaleToneMapExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module)
            {
                auto error = GetLastError();
                api.status = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
                return api;
            }
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_tonemap_abi_version"));
            api.create = reinterpret_cast<decltype(api.create)>(GetProcAddress(module, "bale_tonemap_create"));
            api.apply = reinterpret_cast<decltype(api.apply)>(GetProcAddress(module, "bale_tonemap_apply"));
            api.destroy = reinterpret_cast<decltype(api.destroy)>(GetProcAddress(module, "bale_tonemap_destroy"));
            api.outputFormats = reinterpret_cast<decltype(api.outputFormats)>(GetProcAddress(module, "bale_tonemap_output_formats"));
            if (!api.abiVersion || !api.create || !api.apply || !api.destroy)
            {
                FreeLibrary(module);
                return BaleToneMapExports{};
            }
            if (api.abiVersion() != 1)
            {
                FreeLibrary(module);
                BaleToneMapExports unavailable;
                unavailable.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                return unavailable;
            }
            api.status = S_OK;
            return api;
        }();
        return exports;
    }
};

// The shader, its ABI and the application's transcode-on-failure policy stay at this seam.
class BaleToneMap final : public VideoFrameProcessor
{
public:
    explicit BaleToneMap(int curve) : curve(curve) {}
    ~BaleToneMap() override { Destroy(); }
    BaleToneMap(const BaleToneMap&) = delete;
    BaleToneMap& operator=(const BaleToneMap&) = delete;

    static void LogIneligible()
    {
        av_log(nullptr, AV_LOG_INFO, "Bale tonemap: request ignored; not a D3D11 PQ, HLG, BT.2020 SDR or downscaled BT.709 SDR stream.\n");
    }

    // Called only for a stream that will use D3D11VideoSampleProvider, before its descriptor exists.
    // PQ (HDR10) is tone mapped. HLG and 10-bit SDR on BT.2020 primaries/matrix are converted to
    // SDR BT.709 too (HLG through its OOTF, SDR by gamut conversion only): Windows shows BT.2020 SDR
    // as BT.709 otherwise (desaturated). The shader reads the transfer per frame, so all of them
    // share one stage and one SDR BT.709 output.
    static bool IsSupportedSource(const AVCodecParameters* parameters)
    {
        if (parameters->color_trc == AVCOL_TRC_SMPTEST2084) return true;
        bool bt2020Transfer = parameters->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
            parameters->color_trc == AVCOL_TRC_BT709 || parameters->color_trc == AVCOL_TRC_SMPTE170M ||
            parameters->color_trc == AVCOL_TRC_BT2020_10 || parameters->color_trc == AVCOL_TRC_BT2020_12;
        return bt2020Transfer && parameters->color_primaries == AVCOL_PRI_BT2020 && parameters->color_space == AVCOL_SPC_BT2020_NCL;
    }

    // bale_gpu's optional capability bits; NV12 output only without the export.
    static uint32_t Capabilities()
    {
        auto& api = BaleToneMapExports::Get();
        return SUCCEEDED(api.status) && api.outputFormats ? api.outputFormats() : BALE_TONEMAP_OUTPUT_NV12;
    }

    // BT.709 SDR needs no tone map; it is processed only to downscale it before the player does
    // (the Xbox player's downscale of 4K SDR leaks driver state per open).
    static bool IsSdr709(const AVCodecParameters* parameters)
    {
        bool sdrTransfer = parameters->color_trc == AVCOL_TRC_BT709 || parameters->color_trc == AVCOL_TRC_SMPTE170M ||
            parameters->color_trc == AVCOL_TRC_BT2020_10 || parameters->color_trc == AVCOL_TRC_BT2020_12;
        return sdrTransfer && parameters->color_primaries == AVCOL_PRI_BT709 && parameters->color_space == AVCOL_SPC_BT709;
    }

    // targetWidth/Height: the requested output size (0 = none); BT.709 SDR is eligible only when the
    // video is larger than it, i.e. when the stage downscales.
    static std::unique_ptr<VideoFrameProcessor> Create(int curve, const AVCodecParameters* parameters, int targetWidth = 0, int targetHeight = 0)
    {
        if (curve == 0) return nullptr;
        auto description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        int depth = description && description->nb_components > 0 ? description->comp[0].depth : parameters->bits_per_raw_sample;
        bool downscale = targetWidth > 0 && targetHeight > 0 && (parameters->width > targetWidth || parameters->height > targetHeight);
        bool supported = IsSupportedSource(parameters);
        uint32_t capabilities = 0;
        bool sdr709 = !supported && downscale && IsSdr709(parameters);
        if (sdr709 || depth == 8)
        {
            // Inspect exports only when the decision needs them, before the stream declares its output.
            capabilities = Capabilities();
        }
        if (sdr709)
        {
            sdr709 = (capabilities & BALE_TONEMAP_SDR709) && (capabilities & BALE_TONEMAP_OUTPUT_DOWNSCALE);
        }
        bool depthOk = depth == 10 || (depth == 8 && (capabilities & BALE_TONEMAP_INPUT_NV12));
        if (!depthOk || !(supported || sdr709))
        {
            LogIneligible();
            return nullptr;
        }
        // Missing ABI/create/self-test failures latch on the first frame, with the MSS device ready.
        return std::make_unique<BaleToneMap>(curve);
    }

    HRESULT Initialize(ID3D11Device* device, UINT width, UINT height, UINT sourceWidth, UINT sourceHeight) override
    {
        Destroy();
        const auto& api = BaleToneMapExports::Get();
        if (FAILED(api.status)) return LogFailure(api.status, "ABI 1 unavailable");
        BaleToneMapConfig config{};
        config.width = width;
        config.height = height;
        config.curve = curve;
        // Silo tonemapx bt2390 peak=100 defaults; curve 2 uses the same luminance/desaturation.
        config.source_peak_nits = 1000.0f;
        config.target_white_nits = 100.0f;
        config.desat = 0.0f;
        if (sourceWidth != width || sourceHeight != height)
        {
            // DOWNSCALE (optional capability): visible source size in reserved[0..1].
            config.reserved[0] = sourceWidth;
            config.reserved[1] = sourceHeight;
        }
        auto hr = api.create(device, &config, &handle);
        if (hr != S_OK || !handle)
        {
            Destroy();
            return LogFailure(FAILED(hr) ? hr : E_UNEXPECTED,
                hr == BALE_TONEMAP_E_SELFTEST ? "self-test did not match the CPU reference" : "create failed");
        }
        if (!activeLogged)
        {
            activeLogged = true;
            av_log(nullptr, AV_LOG_INFO, "Bale tonemap active: curve=%d %dx%d from %dx%d\n", curve, static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(sourceWidth), static_cast<int>(sourceHeight));
        }
        return S_OK;
    }

    HRESULT Process(ID3D11DeviceContext* context, ID3D11Texture2D* input, UINT inputSubresource,
        ID3D11Texture2D* output, const VideoFrameColorInfo& colorInfo) override
    {
        if (!handle) return LogFailure(E_UNEXPECTED, "processor not initialized");
        BaleToneMapFrameInfo frame{};
        frame.transfer = colorInfo.transfer;
        frame.primaries = colorInfo.primaries;
        frame.matrix = colorInfo.matrix;
        frame.full_range = colorInfo.range == AVCOL_RANGE_JPEG;
        frame.chroma_location = colorInfo.chromaLocation;
        frame.has_mastering = colorInfo.hasMasteringPrimaries || colorInfo.hasMasteringLuminance;
        frame.has_content_light = colorInfo.hasContentLight;
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                frame.mastering_primaries[i][j] = colorInfo.masteringPrimaries[i][j];
            }
        }
        for (int i = 0; i < 2; i++) frame.mastering_white[i] = colorInfo.masteringWhite[i];
        frame.mastering_min_nits = colorInfo.masteringMinNits;
        frame.mastering_max_nits = colorInfo.masteringMaxNits;
        frame.max_cll = colorInfo.maxCLL;
        frame.max_fall = colorInfo.maxFALL;
        auto hr = BaleToneMapExports::Get().apply(handle, context, input, inputSubresource, output, &frame);
        if (hr == S_OK) return S_OK;
        if (!failureLogged)
        {
            // What bale_gpu was given, to tell a rejected colour description from a device failure.
            av_log(nullptr, AV_LOG_ERROR, "Bale tonemap: apply frame transfer=%d primaries=%d matrix=%d full_range=%d chroma=%d\n",
                frame.transfer, frame.primaries, frame.matrix, frame.full_range, frame.chroma_location);
        }
        return LogFailure(FAILED(hr) ? hr : E_UNEXPECTED, "apply failed");
    }

    // Bale's player accepts 10-bit SDR output only when it carries this mark: a declined stage can
    // pass P010 through with BT.709 container tags, so BT.709 alone proves nothing.
    static constexpr winrt::guid ToneMappedOutputMark{ 0xb41e6a52, 0x3c9d, 0x4f0b, { 0x8e, 0x7a, 0x5d, 0x2c, 0x1f, 0x9a, 0x0b, 0x37 } };

    void DescribeOutput(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const override
    {
        properties.Insert(ToneMappedOutputMark, winrt::Windows::Foundation::PropertyValue::CreateUInt32(1));
    }

    bool SupportsInputFormat(DXGI_FORMAT format) const override
    {
        return format == DXGI_FORMAT_P010 || (format == DXGI_FORMAT_NV12 && (Capabilities() & BALE_TONEMAP_INPUT_NV12));
    }

    bool SupportsDownscale() const override
    {
        auto& api = BaleToneMapExports::Get();
        return SUCCEEDED(api.status) && api.outputFormats && (api.outputFormats() & BALE_TONEMAP_OUTPUT_DOWNSCALE) != 0;
    }

    bool SupportsOutputFormat(DXGI_FORMAT format) const override
    {
        if (format == DXGI_FORMAT_NV12) return true;
        auto& api = BaleToneMapExports::Get();
        return format == DXGI_FORMAT_P010 && SUCCEEDED(api.status) && api.outputFormats &&
            (api.outputFormats() & BALE_TONEMAP_OUTPUT_P010) != 0;
    }

    void NotifyError(HRESULT hr) noexcept override
    {
        LogFailure(hr, "frame processing failed");
    }

private:
    HRESULT LogFailure(HRESULT hr, const char* reason) noexcept
    {
        if (!failureLogged)
        {
            failureLogged = true;
            av_log(nullptr, AV_LOG_ERROR, "Bale tonemap: %s (hr=0x%08X); stopping the stream.\n", reason, static_cast<unsigned>(hr));
        }
        return hr;
    }

    void Destroy()
    {
        if (handle)
        {
            BaleToneMapExports::Get().destroy(handle);
            handle = nullptr;
            av_log(nullptr, AV_LOG_INFO, "Bale tonemap destroyed\n");
        }
    }

    void* handle = nullptr;
    int curve;
    bool activeLogged = false;
    bool failureLogged = false;
};
