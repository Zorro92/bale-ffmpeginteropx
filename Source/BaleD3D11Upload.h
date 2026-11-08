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

    BaleD3D11Upload(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context)
        : device(device), context(context), texturePool(device, 0)
    {
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
            if (FAILED(hr)) return hr;
        }
        nextStaging = (nextStaging + 1) % stagingTextures.size();
        target.context = context;
        target.texture = staging;
        // WRITE (not DISCARD) on a small staging ring: Map waits if the previous GPU copy still uses it.
        return context->Map(staging.get(), 0, D3D11_MAP_WRITE, 0, &target.mapped);
    }

    MediaStreamSample CreateSample(Mapping& source, TimeSpan position, IDirect3DSurface* surface)
    {
        source.Unmap();
        winrt::com_ptr<ID3D11Texture2D> texture;
        {
            std::lock_guard lock(samplesMutex);
            winrt::check_hresult(texturePool.GetUploadTexture(stagingDesc.Width, stagingDesc.Height, stagingDesc.Format, texture));
        }
        context->CopyResource(texture.get(), source.texture.get());
        context->Flush();
        winrt::check_hresult(device->GetDeviceRemovedReason());

        auto dxgiSurface = texture.as<IDXGISurface>();
        auto directSurface = DirectXInteropHelper::GetSurface(dxgiSurface);
        // Create while the decoded AVFrame is still alive, so even a surface/sample failure can
        // re-run the ordinary CPU output step for this same frame, without retaining/copying it.
        auto sample = MediaStreamSample::CreateFromDirect3D11Surface(directSurface, position);
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
            failure.store(winrt::to_hresult());
        }
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    TexturePool texturePool;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> stagingTextures;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    size_t nextStaging = 0;
    std::map<winrt::Windows::Foundation::IUnknown, winrt::event_token> trackedSamples;
    std::mutex samplesMutex;
    std::atomic<HRESULT> failure{ S_OK };
};
