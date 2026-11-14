#pragma once
#include "VideoFrameProcessor.h"
#include <cstddef>

extern "C"
{
#include <libavutil/pixdesc.h>
}

// BEGIN mirrored from bale-gpu include/bale_tonemap.h ABI 1; replace verbatim when that header lands
extern "C"
{
uint32_t bale_tonemap_abi_version(void);
typedef struct BaleToneMapConfig {
    uint32_t width, height;
    uint32_t curve;
    float source_peak_nits;
    float target_white_nits;
    float desat;
    uint32_t reserved[4];
} BaleToneMapConfig;
typedef struct BaleToneMapFrameInfo {
    uint8_t transfer, primaries, matrix, full_range, chroma_location;
    uint8_t has_mastering, has_content_light, reserved0;
    float mastering_primaries[3][2], mastering_white[2];
    float mastering_min_nits, mastering_max_nits;
    uint32_t max_cll, max_fall;
    uint32_t reserved[4];
} BaleToneMapFrameInfo;
HRESULT bale_tonemap_create(ID3D11Device* device, const BaleToneMapConfig* config, void** handle);
HRESULT bale_tonemap_apply(void* handle, ID3D11DeviceContext* context, ID3D11Texture2D* input, uint32_t input_subresource, ID3D11Texture2D* output, const BaleToneMapFrameInfo* frame);
void bale_tonemap_destroy(void* handle);
}
// END ABI mirror
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
        av_log(nullptr, AV_LOG_INFO, "Bale tonemap: request ignored; not a D3D11 10-bit PQ stream.\n");
    }

    // Called only for a stream that will use D3D11VideoSampleProvider, before its descriptor exists.
    static std::unique_ptr<VideoFrameProcessor> Create(int curve, const AVCodecParameters* parameters)
    {
        if (curve == 0) return nullptr;
        auto description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        bool tenBit = description && description->nb_components > 0
            ? description->comp[0].depth == 10 : parameters->bits_per_raw_sample == 10;
        if (!tenBit || parameters->color_trc != AVCOL_TRC_SMPTEST2084)
        {
            LogIneligible();
            return nullptr;
        }
        // Do not inspect exports here: an eligible stream always declares processed SDR output.
        // Missing ABI/create/self-test failures latch on the first frame, with the MSS device ready.
        return std::make_unique<BaleToneMap>(curve);
    }

    HRESULT Initialize(ID3D11Device* device, UINT width, UINT height) override
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
        auto hr = api.create(device, &config, &handle);
        if (hr != S_OK || !handle)
        {
            Destroy();
            return LogFailure(FAILED(hr) ? hr : E_UNEXPECTED, "create or self-test failed");
        }
        if (!activeLogged)
        {
            activeLogged = true;
            av_log(nullptr, AV_LOG_INFO, "Bale tonemap active: curve=%d %dx%d\n", curve, static_cast<int>(width), static_cast<int>(height));
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
        return hr == S_OK ? S_OK : LogFailure(FAILED(hr) ? hr : E_UNEXPECTED, "apply failed");
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
        }
    }

    void* handle = nullptr;
    int curve;
    bool activeLogged = false;
    bool failureLogged = false;
};
