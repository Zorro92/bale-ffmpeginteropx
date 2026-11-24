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
    // The decoder wrote the display output (GpuVideoDecoder::SetDisplayOutput) instead of the frame:
    // the texture is width x height at the display size, BT.709 SDR; sourceWidth/sourceHeight and
    // sourceColor describe what was decoded. False: the texture is the frame (source* unused).
    bool displayOutput = false;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    VideoFrameColorInfo sourceColor;
};

// The processed output a decoder may write itself, at the end of its own GPU work, in place of the
// provider's processing stage: the frame fitted into width x height and tone-mapped or gamut-converted
// to BT.709 SDR (the stage's BT.2390 curve, 1000-nit source peak, whiteNits SDR white, no desaturation).
struct GpuDisplayOutput
{
    UINT width = 0;
    UINT height = 0;
    UINT depth = 10;       // 10: P010, 8: NV12
    bool dither = false;   // 8-bit codes ordered-dithered (else rounded)
    float whiteNits = 100.0f; // the SDR white the tone curve maps to
    // The container's H.273 code points for what the bitstream leaves unspecified, 0 for none.
    uint8_t primaries = 0;
    uint8_t transfer = 0;
    uint8_t matrix = 0;
    uint8_t chromaLocation = 0; // AVChromaLocation of the container, 0 for none
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
    // Diagnostics: the sample timestamp (100 ns) of the access unit fed with this user data, while the
    // host still remembers it. Default: unknown.
    virtual bool UnitTimestamp(uint64_t userData, int64_t& timestamp) noexcept
    {
        UNREFERENCED_PARAMETER(userData);
        UNREFERENCED_PARAMETER(timestamp);
        return false;
    }
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
    // GPU memory the decoder allocated for its own work (not the output images); 0 if unknown.
    virtual uint64_t DeviceBytes() const { return 0; }

    // Asks the decoder to write the processed output itself (before Create). True when it will try; it
    // may still decline at Create, and then its frames carry displayOutput = false (the provider runs
    // its stage). Default: not supported.
    virtual bool SetDisplayOutput(const GpuDisplayOutput& output)
    {
        UNREFERENCED_PARAMETER(output);
        return false;
    }

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
