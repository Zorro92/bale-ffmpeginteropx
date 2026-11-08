#pragma once
#include "pch.h"
#include <atomic>
#include "TexturePool.h"
#include "BaleFilmGrain.h"
#include "DirectXInteropHelper.h"

// Bale: software decode is unchanged; only the final NV12/P010 output lives on the MSS device.
// Decode/seek serialize Map/CreateSample/Flush. Processed can arrive on any MF thread.
class BaleD3D11Upload : public std::enable_shared_from_this<BaleD3D11Upload>
{
public:
    struct Mapping
    {
        winrt::com_ptr<ID3D11DeviceContext> context;
        winrt::com_ptr<ID3D11Texture2D> texture;
        D3D11_MAPPED_SUBRESOURCE mapped{};

        Mapping() = default;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        ~Mapping() { Unmap(); }

        void Unmap()
        {
            if (mapped.pData)
            {
                context->Unmap(texture.get(), 0);
                mapped.pData = nullptr;
            }
        }
    };

    BaleD3D11Upload(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context, int variant)
        : device(device), context(context), texturePool(device, 0)
    {
        // Keep the staging ring unchanged; only the MF-facing DEFAULT texture varies.
        switch (variant)
        {
        case 2: bindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE; break;
        case 3: bindFlags = D3D11_BIND_DECODER; break;
        case 4: bindFlags = D3D11_BIND_SHADER_RESOURCE; break;
        case 5: bindFlags = D3D11_BIND_SHADER_RESOURCE; miscFlags = D3D11_RESOURCE_MISC_SHARED; break;
        }
    }

    ~BaleD3D11Upload() { Flush(); }

    HRESULT Map(UINT width, UINT height, DXGI_FORMAT format, Mapping& target)
    {
        // Only synchronous D3D/MF failures reach the provider's permanent CPU fallback.
        if (width != stagingDesc.Width || height != stagingDesc.Height || format != stagingDesc.Format)
        {
            Flush();
            stagingDesc = {};
            stagingDesc.Width = width;
            stagingDesc.Height = height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        }

        auto& staging = stagingTextures[nextStaging];
        if (!staging)
        {
            auto hr = device->CreateTexture2D(&stagingDesc, nullptr, staging.put());
            if (FAILED(hr))
            {
                av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload CreateTexture2D STAGING failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
                return hr;
            }
        }
        nextStaging = (nextStaging + 1) % stagingTextures.size();
        target.context = context;
        target.texture = staging;
        // WRITE (not DISCARD) on a small staging ring: Map waits if the previous GPU copy still uses it.
        auto hr = context->Map(staging.get(), 0, D3D11_MAP_WRITE, 0, &target.mapped);
        if (FAILED(hr)) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload Map failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        return hr;
    }

    MediaStreamSample CreateSample(Mapping& source, TimeSpan position, IDirect3DSurface* surface,
        BaleFilmGrain& grain, const BaleGrainParams* params)
    {
        source.Unmap();
        const char* operation = "GetUploadTexture";
        try
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            {
                std::lock_guard lock(samplesMutex);
                winrt::check_hresult(texturePool.GetUploadTexture(stagingDesc.Width, stagingDesc.Height, stagingDesc.Format, bindFlags, miscFlags, texture));
            }
            operation = "CopyResource/GetDeviceRemovedReason";
            // Grain reads clean, unmapped staging and writes only the Bale-owned pooled target.
            // A failed pass may have partially written it: overwrite with the clean plain copy.
            if (!params || grain.mode != 2 || !ApplyGrain(grain, *params, source.texture.get(), texture.get()))
                context->CopyResource(texture.get(), source.texture.get());
            // CopyResource and Flush return void; device removal is the available HRESULT check.
            context->Flush();
            winrt::check_hresult(device->GetDeviceRemovedReason());

            operation = "GetSurface";
            auto dxgiSurface = texture.as<IDXGISurface>();
            auto directSurface = DirectXInteropHelper::GetSurface(dxgiSurface);
            // Create while the decoded AVFrame is still alive, so even a surface/sample failure can
            // re-run the ordinary CPU output step for this same frame, without retaining/copying it.
            operation = "CreateFromDirect3D11Surface";
            auto sample = MediaStreamSample::CreateFromDirect3D11Surface(directSurface, position);
            operation = "track Processed";
            {
                std::lock_guard lock(samplesMutex);
                auto native = sample.as<winrt::Windows::Foundation::IUnknown>();
                auto token = sample.Processed(weak_handler(this, &BaleD3D11Upload::OnProcessed));
                try
                {
                    // MF can close the sample's surface (and timestamp) before Processed fires.
                    trackedSamples.emplace(native, TrackedSample{ token, texture, position });
                }
                catch (...)
                {
                    DetachProcessed(native, token);
                    throw;
                }
                // Count texture samples issued by this uploader, including decoded lookahead.
                if (++issuedSamples == 240)
                {
                    av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload steady: issued=%u processed=%u pooled=%zu.\n",
                        issuedSamples, processedSamples, texturePool.PooledTextureCount());
                }
            }
            *surface = directSurface;
            return sample;
        }
        catch (...)
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload %s failed (hr=0x%08X).\n", operation, static_cast<unsigned>(winrt::to_hresult()));
            throw;
        }
    }

    void EndGrain() { DestroyGrain(); }

    void Flush()
    {
        // Serialized with frame production, not tied to the thread that created the handle.
        DestroyGrain();
        std::lock_guard lock(samplesMutex);
        for (auto& entry : trackedSamples)
        {
            DetachProcessed(entry.first, entry.second.token);
        }
        trackedSamples.clear();
        // Some samples were decoded ahead but never issued; others may still be held by MF.
        // Drop all old references, never recycle their textures into a new seek/size generation.
        // MF's own surface references keep any in-flight textures alive until it is finished.
        texturePool.Clear();
        for (auto& texture : stagingTextures) texture = nullptr;
        stagingDesc = {};
        nextStaging = 0;
    }

private:
    void* grainHandle = nullptr; // Only the serialized producer/teardown owner touches this.

    void DestroyGrain()
    {
        if (grainHandle)
        {
            BaleGrainExports::Get().gpuDestroy(grainHandle);
            grainHandle = nullptr;
        }
    }

    bool ApplyGrain(BaleFilmGrain& grain, const BaleGrainParams& params, ID3D11Texture2D* source, ID3D11Texture2D* target)
    {
        const auto& api = BaleGrainExports::Get();
        if (!grainHandle)
        {
            auto hr = api.gpuCreate(device.get(), context.get(), &grainHandle);
            if (hr != S_OK || !grainHandle)
            {
                grain.GpuFailed(hr == S_OK ? E_UNEXPECTED : hr, true);
                DestroyGrain();
                return false;
            }
        }
        auto hr = api.gpuApply(grainHandle, &params, source, target);
        if (hr == S_OK) return true;
        grain.GpuFailed(hr, false);
        DestroyGrain();
        return false;
    }

    struct TrackedSample
    {
        winrt::event_token token;
        winrt::com_ptr<ID3D11Texture2D> texture;
        TimeSpan position;
    };

    void LogProcessedFailure() noexcept
    {
        // Benign event/cleanup problems must not poison subsequent D3D uploads.
        auto hr = winrt::to_hresult();
        if (!processedFailureLogged.exchange(true))
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload OnProcessed failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        }
    }

    void DetachProcessed(winrt::Windows::Foundation::IUnknown const& sample, winrt::event_token token) noexcept
    {
        try
        {
            sample.as<MediaStreamSample>().Processed(token);
        }
        catch (...)
        {
            LogProcessedFailure();
        }
    }

    void OnProcessed(MediaStreamSample const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        try
        {
            std::lock_guard lock(samplesMutex);
            auto native = sender.as<winrt::Windows::Foundation::IUnknown>();
            auto entry = trackedSamples.find(native);
            if (entry == trackedSamples.end()) return; // Already flushed: never recycle across generations.
            auto tracked = std::move(entry->second);
            trackedSamples.erase(entry); // Release tracking even if cleanup or pool allocation throws.
            DetachProcessed(native, tracked.token);
            texturePool.ReturnUploadTexture(std::move(tracked.texture));
            if (++processedSamples <= 3)
            {
                av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload Processed index=%u position_ms=%lld.\n",
                    processedSamples, static_cast<long long>(tracked.position.count() / 10000));
            }
        }
        catch (...)
        {
            // Drop this texture on a cleanup failure, but never throw out of an MF event handler.
            LogProcessedFailure();
        }
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    TexturePool texturePool;
    UINT bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    UINT miscFlags = 0;
    // Protected by samplesMutex; totals survive Flush, so each diagnostic is logged once per uploader.
    unsigned issuedSamples = 0;
    unsigned processedSamples = 0;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> stagingTextures;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    size_t nextStaging = 0;
    std::map<winrt::Windows::Foundation::IUnknown, TrackedSample> trackedSamples;
    std::mutex samplesMutex;
    std::atomic<bool> processedFailureLogged{ false };
};
