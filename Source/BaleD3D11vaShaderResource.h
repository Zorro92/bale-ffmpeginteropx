#pragma once
#include "pch.h"
#include <d3d11.h>

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/log.h>
}

// Bale experiment (bale_d3d11va_shader_resource=1): can a stage shader read the d3d11va decoder's
// frames in place? FFmpeg decodes into one texture array per pool, and a shader resource view of a
// decoder-bound array is commonly taken to be invalid, so bale_gpu's tone map copies each frame into
// a scratch texture first (one full-frame GPU copy per frame; GPU copies are ~2.4x slower while the
// console is idle). This probe asks the device directly: a small two-slice NV12 and P010 array with
// DECODER | SHADER_RESOURCE, and plane views of slice 1 (R8/R8G8, R16/R16G16, as the stage makes).
// Only when every step succeeds does it add SHADER_RESOURCE to the device's bind flags, which FFmpeg
// applies to every decoder pool made from that device afterwards. Results are logged either way.
class BaleD3D11vaShaderResource
{
public:
    // Call after the device context is initialised and before the first decode on it.
    static void Probe(ID3D11Device* device, AVBufferRef* hardwareContext) noexcept
    {
        try
        {
            if (!device || !hardwareContext) return;
            HRESULT nv12 = ProbeFormat(device, DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM);
            HRESULT p010 = ProbeFormat(device, DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM);
            bool readable = SUCCEEDED(nv12) && SUCCEEDED(p010);
            if (readable)
            {
                auto context = reinterpret_cast<AVHWDeviceContext*>(hardwareContext->data);
                auto d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(context->hwctx);
                d3d11->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            }
            av_log(nullptr, AV_LOG_INFO, "Bale d3d11va shader resource probe: NV12 hr=0x%08X P010 hr=0x%08X; decoder pools %s\n",
                static_cast<unsigned>(nv12), static_cast<unsigned>(p010), readable ? "get SHADER_RESOURCE" : "unchanged");
        }
        catch (...)
        {
        }
    }

private:
    static HRESULT ProbeFormat(ID3D11Device* device, DXGI_FORMAT format, DXGI_FORMAT lumaView, DXGI_FORMAT chromaView)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 2;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> texture;
        auto hr = device->CreateTexture2D(&desc, nullptr, texture.put());
        if (FAILED(hr)) return hr;
        for (auto viewFormat : { lumaView, chromaView })
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = viewFormat;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MostDetailedMip = 0;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.FirstArraySlice = 1;
            view.Texture2DArray.ArraySize = 1;
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            hr = device->CreateShaderResourceView(texture.get(), &view, srv.put());
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }
};
