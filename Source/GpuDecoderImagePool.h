#pragma once
#include "pch.h"
#include <d3d11_4.h>
#include <atomic>
#include <condition_variable>
#include <functional>

// A fixed number of output textures for a GPU decoder, each shared with the decoder's device. The pool
// never grows: when every image is in use, Acquire waits until one comes back (a decoder that waits
// for its output instead of allocating more keeps memory bounded, e.g. during seeks). Images match
// each request's size and format; images of an older size are released when they come back.
//
// Thread use: Acquire on the decoder's threads, Release on the sample thread or a Media Foundation
// thread, RequestCancel from any thread (also without the media source's lock). A cancel ends waits
// for a free image; an Acquire that finds one free still gets it, so a cancel that comes while the
// pool has room (e.g. every Starting request) does not fail the decoder's work. A flush or a
// shutdown fails every Acquire. A released image may
// still be read by D3D11 work already submitted: the releaser signals the pool's D3D11 fence and
// Acquire waits on the CPU until the fence has passed that value before the decoder writes again.
class GpuDecoderImagePool
{
public:
    struct Image
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<::IUnknown> decoderView;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t releaseValue = 0;
        bool inUse = false;
    };

    using OpenView = std::function<HRESULT(ID3D11Texture2D*, winrt::com_ptr<::IUnknown>&)>;

    GpuDecoderImagePool(uint32_t capacity, const char* logName) : capacity(capacity), logName(logName) {}

    GpuDecoderImagePool(const GpuDecoderImagePool&) = delete;
    GpuDecoderImagePool& operator=(const GpuDecoderImagePool&) = delete;

    void SetDevice(winrt::com_ptr<ID3D11Device> newDevice, winrt::com_ptr<ID3D11Fence> newReleaseFence, OpenView newOpenView)
    {
        std::lock_guard lock(mutex);
        images.clear();
        device = std::move(newDevice);
        releaseFence = std::move(newReleaseFence);
        openView = std::move(newOpenView);
    }

    // The size and format the stream is expected to need, for PendingBytes before the first request.
    void SetExpectedImage(UINT width, UINT height, DXGI_FORMAT format)
    {
        std::lock_guard lock(mutex);
        requestWidth = width;
        requestHeight = height;
        requestFormat = format;
    }

    // Bytes the pool will still allocate before it is full.
    uint64_t PendingBytes()
    {
        std::lock_guard lock(mutex);
        auto missing = capacity > images.size() ? capacity - images.size() : 0;
        return missing * ImageBytes(requestWidth, requestHeight, requestFormat);
    }

    static uint64_t ImageBytes(UINT width, UINT height, DXGI_FORMAT format)
    {
        uint64_t samples = static_cast<uint64_t>(width) * height * 3 / 2;
        return format == DXGI_FORMAT_P010 ? samples * 2 : samples;
    }

    // S_OK with an image; E_ABORT when cancelled (stop, seek, flush, shutdown); another failure when
    // no image could be created or the previous reader did not finish.
    HRESULT Acquire(UINT width, UINT height, DXGI_FORMAT format, Image** result)
    {
        *result = nullptr;
        Image* image = nullptr;
        {
            std::unique_lock lock(mutex);
            requestWidth = width;
            requestHeight = height;
            requestFormat = format;
            bool counted = false;
            while (!image)
            {
                if (shutdown || flushing)
                {
                    cancelHits++;
                    return E_ABORT;
                }
                Image* stale = nullptr;
                for (auto& entry : images)
                {
                    if (entry->inUse) continue;
                    if (Matches(*entry, width, height, format))
                    {
                        image = entry.get();
                        break;
                    }
                    stale = entry.get();
                }
                if (image) break;
                if (stale)
                {
                    // An image of an older size; make room for one of the requested size.
                    Erase(stale);
                }
                if (images.size() < capacity)
                {
                    auto hr = CreateImage(width, height, format, image);
                    if (FAILED(hr)) return hr;
                    break;
                }
                if (cancelRequested)
                {
                    // Only a wait is cancelled.
                    cancelHits++;
                    return E_ABORT;
                }
                if (!counted)
                {
                    counted = true;
                    blockedCount++;
                }
                changed.wait(lock);
            }
            image->inUse = true;
            inUse++;
            if (inUse > peakInUse) peakInUse = inUse;
        }

        auto hr = WaitForRelease(*image);
        if (FAILED(hr))
        {
            Release(image, 0);
            return hr;
        }
        *result = image;
        return S_OK;
    }

    // fenceValue: the pool fence value signalled after the last D3D11 use of the image, 0 for none.
    void Release(Image* image, uint64_t fenceValue)
    {
        if (!image) return;
        std::lock_guard lock(mutex);
        if (!Contains(image) || !image->inUse) return;
        image->inUse = false;
        if (fenceValue) image->releaseValue = fenceValue;
        inUse--;
        if (!Matches(*image, requestWidth, requestHeight, requestFormat))
        {
            Erase(image);
        }
        changed.notify_all();
    }

    // Ends any Acquire wait and fails new ones that would wait, until ClearCancel. Any thread.
    void RequestCancel()
    {
        std::lock_guard lock(mutex);
        cancelRequested = true;
        changed.notify_all();
    }

    void ClearCancel()
    {
        std::lock_guard lock(mutex);
        cancelRequested = false;
    }

    bool CancelRequested()
    {
        std::lock_guard lock(mutex);
        return cancelRequested || shutdown;
    }

    // Acquire fails while the owner drains the decoder (frames of before a seek need no image).
    void SetFlushing(bool value)
    {
        std::lock_guard lock(mutex);
        flushing = value;
        changed.notify_all();
    }

    // Requests that ended without an image because of a cancelled wait, a flush or a shutdown.
    uint32_t CancelHits()
    {
        std::lock_guard lock(mutex);
        return cancelHits;
    }

    // Fails every Acquire from now on, then (Clear) drops the images. Clear only after the decoder
    // that used them has been destroyed.
    void Shutdown()
    {
        std::lock_guard lock(mutex);
        shutdown = true;
        changed.notify_all();
    }

    void Clear()
    {
        std::lock_guard lock(mutex);
        images.clear();
        inUse = 0;
        device = nullptr;
        releaseFence = nullptr;
        openView = nullptr;
    }

    // Images that exist now and their total size.
    void Footprint(uint32_t& count, uint64_t& bytes)
    {
        std::lock_guard lock(mutex);
        count = static_cast<uint32_t>(images.size());
        bytes = 0;
        for (auto& image : images) bytes += ImageBytes(image.width, image.height, image.format);
    }

    void Statistics(uint32_t& created, uint32_t& peak, uint32_t& blocked)
    {
        std::lock_guard lock(mutex);
        created = createdCount;
        peak = peakInUse;
        blocked = blockedCount;
    }

private:
    static bool Matches(const Image& image, UINT width, UINT height, DXGI_FORMAT format)
    {
        return image.width == width && image.height == height && image.format == format;
    }

    bool Contains(const Image* image) const
    {
        for (auto& entry : images)
        {
            if (entry.get() == image) return true;
        }
        return false;
    }

    void Erase(const Image* image)
    {
        for (auto it = images.begin(); it != images.end(); ++it)
        {
            if (it->get() == image)
            {
                images.erase(it);
                return;
            }
        }
    }

    // Caller holds the mutex.
    HRESULT CreateImage(UINT width, UINT height, DXGI_FORMAT format, Image*& result)
    {
        if (!device || !openView) return E_NOT_VALID_STATE;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // Shader resource for a processing stage, render target as the hardware decoder's textures
        // have, unordered access so the decoder's shaders can write the texture directly.
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0);
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        auto image = std::make_unique<Image>();
        auto hr = device->CreateTexture2D(&desc, nullptr, image->texture.put());
        if (FAILED(hr) && unorderedAccess)
        {
            // The decoder then writes through a copy (rav1g: its staged path).
            unorderedAccess = false;
            av_log(nullptr, AV_LOG_WARNING, "%s: output textures without unordered access (hr=0x%08X)\n", logName, static_cast<unsigned>(hr));
            desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
            hr = device->CreateTexture2D(&desc, nullptr, image->texture.put());
        }
        if (SUCCEEDED(hr)) hr = openView(image->texture.get(), image->decoderView);
        if (FAILED(hr))
        {
            av_log(nullptr, AV_LOG_ERROR, "%s: output image %ux%u format=%u failed (hr=0x%08X)\n", logName, width, height,
                static_cast<unsigned>(format), static_cast<unsigned>(hr));
            return hr;
        }
        image->width = width;
        image->height = height;
        image->format = format;
        result = image.get();
        images.push_back(std::move(image));
        createdCount++;
        return S_OK;
    }

    // The previous reader's D3D11 work must be done before the decoder writes the image again.
    HRESULT WaitForRelease(const Image& image)
    {
        winrt::com_ptr<ID3D11Fence> fence;
        {
            std::lock_guard lock(mutex);
            fence = releaseFence;
        }
        if (!image.releaseValue || !fence || fence->GetCompletedValue() >= image.releaseValue) return S_OK;
        HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!event) return HRESULT_FROM_WIN32(GetLastError());
        auto hr = fence->SetEventOnCompletion(image.releaseValue, event);
        if (SUCCEEDED(hr) && WaitForSingleObjectEx(event, 5000, FALSE) != WAIT_OBJECT_0)
        {
            hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        CloseHandle(event);
        return hr;
    }

    const uint32_t capacity;
    const char* logName;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::unique_ptr<Image>> images;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11Fence> releaseFence;
    OpenView openView;
    UINT requestWidth = 0;
    UINT requestHeight = 0;
    DXGI_FORMAT requestFormat = DXGI_FORMAT_UNKNOWN;
    bool unorderedAccess = true;
    bool cancelRequested = false;
    bool flushing = false;
    bool shutdown = false;
    uint32_t inUse = 0;
    uint32_t peakInUse = 0;
    uint32_t createdCount = 0;
    uint32_t blockedCount = 0;
    uint32_t cancelHits = 0;
};
