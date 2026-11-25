#pragma once
#include "pch.h"
#include <d3d11_4.h>
#include <map>
#include <mutex>

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
}

// Reads FFmpeg d3d12va frames on a D3D11 device without a CPU wait.
//
// The D3D12 device is created on the D3D11 device's adapter (feature level 11_0, the highest an Xbox
// app gets; FFmpeg's own device creation asks for 12_0) and its frames are allocated in shared heaps
// (AVD3D12VADeviceContext::heap_flags), so each frame texture opens once on the D3D11 device and is
// read in place there. One shared fence orders the two devices on the GPU:
//
//   Acquire: our D3D12 queue waits for the frame's decode fence and signals the shared fence; the
//            D3D11 context waits for that value before it reads the texture.
//   Release: the D3D11 context signals the shared fence after its reads; our D3D12 queue waits for
//            that and then raises the frame's own fence. FFmpeg waits on the frame's fence before it
//            decodes into the texture again, so a reused texture is never written while D3D11 reads it.
//
// Thread use: Acquire and Release on the sample thread, in pairs, with the frame still referenced.
class D3D12FrameInterop
{
public:
    // A D3D12VA device context on the D3D11 device's adapter, its frames shareable with D3D11. The
    // context owns the D3D12 device (released by its free callback).
    static HRESULT CreateDeviceContext(ID3D11Device* device11, AVBufferRef* hardwareContext)
    {
        if (!device11 || !hardwareContext) return E_POINTER;
        auto deviceContext = reinterpret_cast<AVHWDeviceContext*>(hardwareContext->data);
        if (deviceContext->type != AV_HWDEVICE_TYPE_D3D12VA) return E_INVALIDARG;

        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::com_ptr<IDXGIAdapter> adapter;
        HRESULT hr = device11->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(adapter.put());

        winrt::com_ptr<ID3D12Device> device12;
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device12.put()));
        if (FAILED(hr)) return hr;

        auto context = reinterpret_cast<AVD3D12VADeviceContext*>(deviceContext->hwctx);
        context->heap_flags |= D3D12_HEAP_FLAG_SHARED;
        context->device = device12.detach();
        deviceContext->user_opaque = context->device;
        deviceContext->free = [](AVHWDeviceContext* owner)
        {
            if (owner->user_opaque) static_cast<ID3D12Device*>(owner->user_opaque)->Release();
            owner->user_opaque = nullptr;
        };
        int error = av_hwdevice_ctx_init(hardwareContext);
        return error < 0 ? E_FAIL : S_OK;
    }

    static bool IsD3D12(const AVBufferRef* hardwareContext)
    {
        return hardwareContext && reinterpret_cast<const AVHWDeviceContext*>(hardwareContext->data)->type == AV_HWDEVICE_TYPE_D3D12VA;
    }

    D3D12FrameInterop() = default;
    D3D12FrameInterop(const D3D12FrameInterop&) = delete;
    D3D12FrameInterop& operator=(const D3D12FrameInterop&) = delete;

    ~D3D12FrameInterop()
    {
        WaitIdle();
    }

    HRESULT Initialize(AVBufferRef* hardwareContext, ID3D11Device* device11, ID3D11DeviceContext* deviceContext11)
    {
        if (!IsD3D12(hardwareContext) || !device11 || !deviceContext11) return E_INVALIDARG;
        auto context = reinterpret_cast<AVD3D12VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hardwareContext->data)->hwctx);
        if (!context->device) return E_POINTER;
        device12.copy_from(context->device);

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put()));
        if (SUCCEEDED(hr)) hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence12.put()));

        HANDLE handle = nullptr;
        if (SUCCEEDED(hr)) hr = device12->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, &handle);
        winrt::com_ptr<ID3D11Device5> device5;
        if (SUCCEEDED(hr))
        {
            hr = device11->QueryInterface(IID_PPV_ARGS(device5.put()));
            if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(fence11.put()));
        }
        if (handle) CloseHandle(handle);
        if (SUCCEEDED(hr)) hr = device11->QueryInterface(IID_PPV_ARGS(device1.put()));
        if (SUCCEEDED(hr)) hr = deviceContext11->QueryInterface(IID_PPV_ARGS(context4.put()));
        return hr;
    }

    // The frame's texture on the D3D11 device, readable by work submitted after this call.
    HRESULT Acquire(const AVFrame* frame, winrt::com_ptr<ID3D11Texture2D>& texture, bool& opened)
    {
        opened = false;
        auto source = Frame(frame);
        if (!source) return E_INVALIDARG;
        std::lock_guard lock(mutex);
        if (!queue || !context4) return E_NOT_VALID_STATE;

        // A new frame pool (open, size change): the textures of the old one are no longer FFmpeg's.
        if (frame->hw_frames_ctx->data != framesContext)
        {
            textures.clear();
            framesContext = frame->hw_frames_ctx->data;
        }

        auto entry = textures.find(source->texture);
        if (entry == textures.end())
        {
            HANDLE handle = nullptr;
            HRESULT hr = device12->CreateSharedHandle(source->texture, nullptr, GENERIC_ALL, nullptr, &handle);
            winrt::com_ptr<ID3D11Texture2D> opened11;
            if (SUCCEEDED(hr)) hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(opened11.put()));
            if (handle) CloseHandle(handle);
            if (FAILED(hr)) return hr;
            winrt::com_ptr<ID3D12Resource> keep;
            keep.copy_from(source->texture);
            entry = textures.emplace(source->texture, Shared{ keep, opened11 }).first;
            opened = true;
        }

        HRESULT hr = queue->Wait(source->sync_ctx.fence, source->sync_ctx.fence_value);
        if (SUCCEEDED(hr)) hr = queue->Signal(fence12.get(), ++value);
        if (SUCCEEDED(hr)) hr = context4->Wait(fence11.get(), value);
        if (FAILED(hr)) return hr;
        texture = entry->second.texture11;
        return S_OK;
    }

    // After the last D3D11 read of the frame's texture was submitted; before the frame is unreferenced.
    HRESULT Release(const AVFrame* frame)
    {
        auto source = Frame(frame);
        if (!source) return E_INVALIDARG;
        std::lock_guard lock(mutex);
        if (!queue || !context4) return E_NOT_VALID_STATE;
        HRESULT hr = context4->Signal(fence11.get(), ++value);
        context4->Flush();
        if (SUCCEEDED(hr)) hr = queue->Wait(fence12.get(), value);
        // FFmpeg waits for this value before it writes the texture again (d3d12va end_frame).
        if (SUCCEEDED(hr)) hr = queue->Signal(source->sync_ctx.fence, ++source->sync_ctx.fence_value);
        return hr;
    }

    // Frame textures opened on the D3D11 device.
    size_t TextureCount()
    {
        std::lock_guard lock(mutex);
        return textures.size();
    }

private:
    static AVD3D12VAFrame* Frame(const AVFrame* frame)
    {
        if (!frame || frame->format != AV_PIX_FMT_D3D12 || !frame->hw_frames_ctx) return nullptr;
        auto source = reinterpret_cast<AVD3D12VAFrame*>(frame->data[0]);
        return source && source->texture && source->sync_ctx.fence ? source : nullptr;
    }

    void WaitIdle() noexcept
    {
        if (!queue || !fence12) return;
        auto target = ++value;
        if (FAILED(queue->Signal(fence12.get(), target))) return;
        if (fence12->GetCompletedValue() >= target) return;
        auto event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!event) return;
        if (SUCCEEDED(fence12->SetEventOnCompletion(target, event))) WaitForSingleObjectEx(event, 5000, FALSE);
        CloseHandle(event);
    }

    struct Shared
    {
        winrt::com_ptr<ID3D12Resource> texture12; // keeps the key's address unique while cached
        winrt::com_ptr<ID3D11Texture2D> texture11;
    };

    std::mutex mutex;
    winrt::com_ptr<ID3D12Device> device12;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12Fence> fence12;
    winrt::com_ptr<ID3D11Fence> fence11;
    winrt::com_ptr<ID3D11Device1> device1;
    winrt::com_ptr<ID3D11DeviceContext4> context4;
    uint64_t value = 0;
    const void* framesContext = nullptr;
    std::map<ID3D12Resource*, Shared> textures;
};
