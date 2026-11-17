#pragma once
#include <d3d11.h>
#include "DirectXInteropHelper.h"

class TexturePool
{
public:

    TexturePool(winrt::com_ptr<ID3D11Device> device, int initialPoolSize)
    {
        this->device = device;
        this->initialPoolSize = initialPoolSize;
    }

public:
    ~TexturePool()
    {
        Clear();
        device = nullptr;
    }

public:

    void Clear()
    {
        pool.clear();
        ZeroMemory(&desc_shared, sizeof(desc_shared));
    }

    HRESULT Initialize(ID3D11Texture2D* sourceTexture)
    {
        if (pool.size() > 0)
        {
            Clear();
        }

        D3D11_TEXTURE2D_DESC desc;
        sourceTexture->GetDesc(&desc);

        //create a new texture description, with shared flag
        ZeroMemory(&desc_shared, sizeof(desc_shared));
        desc_shared.Width = desc.Width;
        desc_shared.Height = desc.Height;
        desc_shared.MipLevels = desc.MipLevels;
        desc_shared.ArraySize = 1;
        desc_shared.Format = desc.Format;
        desc_shared.SampleDesc.Count = desc.SampleDesc.Count;
        desc_shared.SampleDesc.Quality = desc.SampleDesc.Quality;
        desc_shared.Usage = D3D11_USAGE_DEFAULT;
        desc_shared.CPUAccessFlags = 0;
        desc_shared.MiscFlags = 0;
        desc_shared.BindFlags = desc.BindFlags;

        // pre-allocate pool
        winrt::com_ptr<ID3D11Texture2D> copy_tex;
        HRESULT hr = S_OK;
        for (int i = 0; i < initialPoolSize; i++)
        {
            hr = device->CreateTexture2D(&desc_shared, NULL, copy_tex.put());

            if (SUCCEEDED(hr))
            {
                pool.push_back(copy_tex);
            }
            else
            {
                break;
            }
        }
        return hr;
    }

    HRESULT GetCopyTexture(ID3D11Texture2D* sourceTexture, winrt::com_ptr<ID3D11Texture2D>& result)
    {
        HRESULT hr = S_OK;

        D3D11_TEXTURE2D_DESC desc;
        sourceTexture->GetDesc(&desc);

        if (desc.Width != desc_shared.Width || desc.Height != desc_shared.Height || desc.Format != desc_shared.Format)
        {
            hr = Initialize(sourceTexture);
        }

        if (SUCCEEDED(hr))
        {
            if (pool.size() > 0)
            {
                //use pool texture, if available
                result = pool.back();
                pool.pop_back();
            }
            else
            {
                //otherwise create a new texture
                hr = device->CreateTexture2D(&desc_shared, NULL, result.put());
            }
        }

        return hr;
    }

    // Explicit output resources for a frame-processing stage, separate from the decoder pool.
    HRESULT GetTexture(UINT width, UINT height, DXGI_FORMAT format, UINT bindFlags, winrt::com_ptr<ID3D11Texture2D>& result)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        if (!MatchesDescription(desc))
        {
            Clear();
            desc_shared = desc;
        }
        if (!pool.empty())
        {
            result = pool.back();
            pool.pop_back();
            reusedCount++;
            return S_OK;
        }
        auto hr = device->CreateTexture2D(&desc_shared, nullptr, result.put());
        if (SUCCEEDED(hr)) createdCount++;
        return hr;
    }

    // Diagnostics for GetTexture pools (caller holds its pool mutex).
    uint32_t CreatedCount() const { return createdCount; }
    uint32_t ReusedCount() const { return reusedCount; }

    // Bale: a separate software-upload pool; flags are selected by bale_d3d11_upload.
    HRESULT GetUploadTexture(UINT width, UINT height, DXGI_FORMAT format, UINT bindFlags, UINT miscFlags, winrt::com_ptr<ID3D11Texture2D>& result)
    {
        if (width != desc_shared.Width || height != desc_shared.Height || format != desc_shared.Format ||
            bindFlags != desc_shared.BindFlags || miscFlags != desc_shared.MiscFlags)
        {
            Clear();
            desc_shared.Width = width;
            desc_shared.Height = height;
            desc_shared.MipLevels = 1;
            desc_shared.ArraySize = 1;
            desc_shared.Format = format;
            desc_shared.SampleDesc.Count = 1;
            desc_shared.Usage = D3D11_USAGE_DEFAULT;
            desc_shared.BindFlags = bindFlags;
            desc_shared.MiscFlags = miscFlags;
            av_log(nullptr, AV_LOG_INFO, "Bale D3D11 upload pool desc: %ux%u format=%u bind=0x%X misc=0x%X.\n",
                width, height, static_cast<unsigned>(format), bindFlags, miscFlags);
        }
        if (!pool.empty())
        {
            result = pool.back();
            pool.pop_back();
            return S_OK;
        }
        auto hr = device->CreateTexture2D(&desc_shared, nullptr, result.put());
        if (FAILED(hr)) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload CreateTexture2D DEFAULT failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        return hr;
    }

    // Bale: only return matching upload resources; the hardware-decoder pool remains unchanged.
    void ReturnUploadTexture(winrt::com_ptr<ID3D11Texture2D> texture)
    {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width == desc_shared.Width && desc.Height == desc_shared.Height && desc.Format == desc_shared.Format &&
            desc.MipLevels == desc_shared.MipLevels && desc.ArraySize == desc_shared.ArraySize &&
            desc.SampleDesc.Count == desc_shared.SampleDesc.Count && desc.SampleDesc.Quality == desc_shared.SampleDesc.Quality &&
            desc.Usage == desc_shared.Usage && desc.BindFlags == desc_shared.BindFlags &&
            desc.CPUAccessFlags == desc_shared.CPUAccessFlags && desc.MiscFlags == desc_shared.MiscFlags)
        {
            pool.push_back(std::move(texture));
        }
    }

    size_t PooledTextureCount() const { return pool.size(); } // Caller holds its pool mutex.

    void ReturnTexture(winrt::com_ptr<ID3D11Texture2D> texture)
    {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        if (MatchesDescription(desc))
        {
            pool.push_back(texture);
        }
    }

private:
    bool MatchesDescription(const D3D11_TEXTURE2D_DESC& desc) const
    {
        return desc.Width == desc_shared.Width && desc.Height == desc_shared.Height && desc.Format == desc_shared.Format &&
            desc.MipLevels == desc_shared.MipLevels && desc.ArraySize == desc_shared.ArraySize &&
            desc.SampleDesc.Count == desc_shared.SampleDesc.Count && desc.SampleDesc.Quality == desc_shared.SampleDesc.Quality &&
            desc.Usage == desc_shared.Usage && desc.BindFlags == desc_shared.BindFlags &&
            desc.CPUAccessFlags == desc_shared.CPUAccessFlags && desc.MiscFlags == desc_shared.MiscFlags;
    }

    winrt::com_ptr<ID3D11Device> device;
    std::vector<winrt::com_ptr<ID3D11Texture2D>> pool;
    D3D11_TEXTURE2D_DESC desc_shared{};
    int initialPoolSize = 0;
    uint32_t createdCount = 0;
    uint32_t reusedCount = 0;
};

