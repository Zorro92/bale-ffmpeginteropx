#pragma once
#include "pch.h"
#include <atomic>
#include "TexturePool.h"
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
        case 4: bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; break;
        case 5: miscFlags = D3D11_RESOURCE_MISC_SHARED; break;
        }
    }

    ~BaleD3D11Upload() { Flush(); }

    HRESULT Map(UINT width, UINT height, DXGI_FORMAT format, Mapping& target)
    {
        auto hr = failure.load();
        if (FAILED(hr)) return hr;
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
        hr = context->Map(staging.get(), 0, D3D11_MAP_WRITE, 0, &target.mapped);
        if (FAILED(hr)) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload Map failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        return hr;
    }

    MediaStreamSample CreateSample(Mapping& source, TimeSpan position, IDirect3DSurface* surface)
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
            // CopyResource and Flush return void; device removal is the available HRESULT check.
            context->CopyResource(texture.get(), source.texture.get());
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
                    trackedSamples.emplace(native, token);
                }
                catch (...)
                {
                    sample.Processed(token);
                    throw;
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

    void Flush()
    {
        std::lock_guard lock(samplesMutex);
        for (auto& entry : trackedSamples)
        {
            entry.first.as<MediaStreamSample>().Processed(entry.second);
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
    void OnProcessed(MediaStreamSample const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        try
        {
            std::lock_guard lock(samplesMutex);
            if (processedSamples < 3)
            {
                av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload Processed index=%u position_ms=%lld.\n",
                    ++processedSamples, static_cast<long long>(sender.Timestamp().count() / 10000));
            }
            auto native = sender.as<winrt::Windows::Foundation::IUnknown>();
            auto entry = trackedSamples.find(native);
            if (entry != trackedSamples.end())
            {
                sender.Processed(entry->second);
                trackedSamples.erase(entry);
                winrt::com_ptr<IDXGISurface> surface;
                winrt::check_hresult(DirectXInteropHelper::GetDXGISurface(sender.Direct3D11Surface(), surface));
                texturePool.ReturnTexture(surface.as<ID3D11Texture2D>());
            }
        }
        catch (...)
        {
            // Discard that texture and let the decode thread log/disable upload on the next frame.
            // Never throw out of an MF event handler.
            auto hr = winrt::to_hresult();
            failure.store(hr);
            av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload OnProcessed failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        }
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    TexturePool texturePool;
    UINT bindFlags = D3D11_BIND_SHADER_RESOURCE;
    UINT miscFlags = 0;
    unsigned processedSamples = 0; // Protected by samplesMutex; bounded diagnostics, not per-frame logging.
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> stagingTextures;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    size_t nextStaging = 0;
    std::map<winrt::Windows::Foundation::IUnknown, winrt::event_token> trackedSamples;
    std::mutex samplesMutex;
    std::atomic<HRESULT> failure{ S_OK };
};
