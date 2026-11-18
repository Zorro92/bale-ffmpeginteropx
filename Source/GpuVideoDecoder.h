#pragma once
#include "pch.h"
#include "VideoFrameProcessor.h"
#include <d3d11_4.h>
#include <string>

// A video decoder that decodes on the GPU into textures the renderer's D3D11 device can read, for
// GpuDecoderSampleProvider. The decoder takes its output textures from the provider (GpuDecoderHost)
// and hands every decoded frame back with a fence value to wait for in D3D11.

// One decoded picture, queued from the decoder's thread to the provider's sample thread.
struct GpuDecodedFrame
{
    // The output image the decoder wrote (GpuDecoderHost::AcquireImage's token).
    void* image = nullptr;
    // The texture holds the frame once this fence reaches fenceValue (wait on the D3D11 queue).
    winrt::com_ptr<ID3D11Fence> fence;
    uint64_t fenceValue = 0;
    // The visible frame size; the texture may be up to one sample larger in each direction.
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // The bitstream's colour description; unspecified fields and absent metadata when it has none.
    VideoFrameColorInfo color;
    // What the provider passed to Decode with the access unit of this frame.
    uint64_t userData = 0;
};

// What the decoder needs from its host. Called on the decoder's threads; never throws.
class GpuDecoderHost
{
public:
    virtual ~GpuDecoderHost() = default;
    // A free output texture of this size and format, as a view on the decoder's device (decoderView)
    // and an opaque token. May block until one is free. False cancels the request.
    virtual bool AcquireImage(UINT width, UINT height, DXGI_FORMAT format, void** decoderView, void** token) noexcept = 0;
    // An image the decoder took and wrote no frame into.
    virtual void ImageUnused(void* token) noexcept = 0;
    // A frame is ready; called in output order.
    virtual void FrameReady(GpuDecodedFrame&& frame) noexcept = 0;
    // Bytes the decoder may still allocate, or a negative value when unknown.
    virtual int64_t MemoryAvailable() noexcept = 0;
};

class GpuVideoDecoder
{
public:
    // Decode: the access unit starts a sequence the decoder was not created for. Nothing of it was
    // decoded: drain, destroy, Create with this access unit and decode it again.
    static constexpr HRESULT ReconfigureRequired = static_cast<HRESULT>(0x00042B10L);

    virtual ~GpuVideoDecoder() = default;
    // Prefix of this decoder's log lines.
    virtual const char* LogName() const = 0;
    // Binds the renderer's device (it must be on the decoder's adapter). Once per stream.
    virtual HRESULT SetDevice(ID3D11Device* device) = 0;
    // A view of a renderer texture on the decoder's device, for an output image.
    virtual HRESULT OpenTexture(ID3D11Texture2D* texture, winrt::com_ptr<::IUnknown>& decoderView) = 0;
    // Creates the decoder for the sequence of this access unit. S_FALSE: it has no sequence header;
    // try a later one.
    virtual HRESULT Create(const uint8_t* data, size_t size, GpuDecoderHost* host) = 0;
    virtual bool IsCreated() const = 0;
    virtual HRESULT Decode(const uint8_t* data, size_t size, uint64_t userData) = 0;
    // Hands every frame in flight to the host. The decoder stays usable.
    virtual HRESULT Drain() = 0;
    virtual void Destroy() = 0;
    // Text the decoder adds to the provider's periodic statistics line, for the time since the last
    // call (e.g. the decoder's own per-frame timing). Called on the provider's sample thread, also
    // while another thread decodes: it must not touch the decoding state. Default: nothing.
    virtual void AppendStatistics(std::string& line)
    {
        UNREFERENCED_PARAMETER(line);
    }
    // Media type attributes of the decoder's direct output (not of processed output). Default: none.
    virtual void DescribeOutput(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const
    {
        UNREFERENCED_PARAMETER(properties);
    }
};
