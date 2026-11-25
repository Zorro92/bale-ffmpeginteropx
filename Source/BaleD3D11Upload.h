#pragma once
#include "pch.h"
#include <array>
#include <atomic>
#include "TexturePool.h"
#include "BaleFilmGrain.h"
#include "BaleMemoryLedger.h"
#include "DirectXInteropHelper.h"
#include "VideoFrameProcessor.h"

// Bale: bale_gpu.dll zero-copy ABI 1. dav1d decodes into GPU-visible buffers from bale_zc_alloc and
// bale_zc_repack turns one into a P010 texture on the MSS device, with no staging map or CPU fill.
// The optional bale_zc_repack_into writes the pooled texture itself, skipping the landing copy;
// the optional bale_zc_repack_into_bits does the same for 8-bit pictures (P010 samples << 8).
// Layouts mirror the Rust repr(C) structs.
struct BaleZcFrame
{
    uint32_t width;
    uint32_t height;
    uint32_t lumaStride;
    uint32_t chromaStride;
    uint64_t offsets[3]; // Y, U, V plane starts in bytes from the buffer's CPU address.
};
static_assert(sizeof(BaleZcFrame) == 40, "BaleZcFrame must match the zero-copy ABI");

struct BaleZcStats
{
    uint64_t buffersCreated;
    uint64_t buffersLive;
    uint64_t buffersPeak;
    uint64_t buffersPooled;
    uint64_t framesRepacked;
    uint64_t allocFailures;
    uint64_t allocWaits;
    uint64_t landingCopies; // Frames that took an extra GPU copy (the device cannot share UAV P010).
};
static_assert(sizeof(BaleZcStats) == 64, "BaleZcStats must match the zero-copy ABI");

struct BaleZeroCopyExports
{
    uint32_t (__cdecl* abiVersion)() = nullptr;
    HRESULT (__cdecl* create)(void*, void*, void**) = nullptr;
    HRESULT (__cdecl* alloc)(void*, uint64_t, uint8_t**, void**) = nullptr;
    HRESULT (__cdecl* release)(void*) = nullptr;
    HRESULT (__cdecl* repack)(void*, void*, const BaleZcFrame*, void**) = nullptr;
    HRESULT (__cdecl* stats)(void*, BaleZcStats*) = nullptr;
    void (__cdecl* destroy)(void*) = nullptr;
    // Optional (added within ABI 1): null in an older bale_gpu, which then only lands frames.
    HRESULT (__cdecl* repackInto)(void*, void*, const BaleZcFrame*, const BaleGrainParams*, void*) = nullptr;
    HRESULT (__cdecl* repackIntoBits)(void*, void*, const BaleZcFrame*, uint32_t, const BaleGrainParams*, void*) = nullptr;

    static const BaleZeroCopyExports& Get()
    {
        // One load attempt per process; the module stays loaded, so buffer releases that outlive
        // every handle (dav1d references freed with the codec) still reach it.
        static const BaleZeroCopyExports exports = []
        {
            BaleZeroCopyExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module) return api;
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_zc_abi_version"));
            api.create = reinterpret_cast<decltype(api.create)>(GetProcAddress(module, "bale_zc_create"));
            api.alloc = reinterpret_cast<decltype(api.alloc)>(GetProcAddress(module, "bale_zc_alloc"));
            api.release = reinterpret_cast<decltype(api.release)>(GetProcAddress(module, "bale_zc_release"));
            api.repack = reinterpret_cast<decltype(api.repack)>(GetProcAddress(module, "bale_zc_repack"));
            api.stats = reinterpret_cast<decltype(api.stats)>(GetProcAddress(module, "bale_zc_stats"));
            api.destroy = reinterpret_cast<decltype(api.destroy)>(GetProcAddress(module, "bale_zc_destroy"));
            api.repackInto = reinterpret_cast<decltype(api.repackInto)>(GetProcAddress(module, "bale_zc_repack_into"));
            api.repackIntoBits = reinterpret_cast<decltype(api.repackIntoBits)>(GetProcAddress(module, "bale_zc_repack_into_bits"));
            if (!api.abiVersion || !api.create || !api.alloc || !api.release || !api.repack || !api.stats ||
                !api.destroy || api.abiVersion() != 1)
            {
                FreeLibrary(module);
                return BaleZeroCopyExports{};
            }
            return api;
        }();
        return exports;
    }
};

// Bale debug: wall time per upload stage on the producing thread, summed per 240-frame window.
struct BaleUploadTiming
{
    enum Stage { Gap, Map, Fill, Unmap, Gpu, Flush, Sample, Count };
    int64_t sum[Count]{};
    int64_t peak[Count]{};
    unsigned frames = 0;
    int64_t lastEnd = 0;

    static int64_t Now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    static double Ms(int64_t ticks)
    {
        static const double perMs = []
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1000.0;
        }();
        return static_cast<double>(ticks) / perMs;
    }

    void Add(Stage stage, int64_t ticks)
    {
        sum[stage] += ticks;
        if (ticks > peak[stage]) peak[stage] = ticks;
    }

    // Called once per zero-copy frame, after its sample. Logs average/max ms per stage.
    // path: 0 staged (fill = swscale into the staging texture, gpu = grain or copy submit),
    // 2 zero-copy landing (fill = repack submit), 3 zero-copy direct write (gpu = repack submit). Decode-ahead also uses Now/Ms for its own timing.
    void EndFrame(int path)
    {
        lastEnd = Now();
        if (++frames < 240) return;
        auto avg = [&](Stage stage) { return Ms(sum[stage]) / frames; };
        auto top = [&](Stage stage) { return Ms(peak[stage]); };
        av_log(nullptr, AV_LOG_INFO,
            "Bale D3D11 upload timing: frames=%u repack=%d avg/max ms gap=%.2f/%.1f map=%.2f/%.1f fill=%.2f/%.1f "
            "unmap=%.2f/%.1f gpu=%.2f/%.1f flush=%.2f/%.1f sample=%.2f/%.1f.\n",
            frames, path, avg(Gap), top(Gap), avg(Map), top(Map), avg(Fill), top(Fill),
            avg(Unmap), top(Unmap), avg(Gpu), top(Gpu), avg(Flush), top(Flush), avg(Sample), top(Sample));
        for (int i = 0; i < Count; i++) sum[i] = peak[i] = 0;
        frames = 0;
    }
};

// Bale: software AV1 output on the MSS device, in pooled NV12/P010 textures handed to MF.
// Zero-copy: bale_gpu writes (or lands) each dav1d picture into textures that D3D12 can open (UAV +
// NT shared handle). Staged: the CPU writes each converted frame into a STAGING ring texture, and
// a D3D11 copy or the D3D11 grain pass moves it into a plain render-target texture (no D3D12).
// Decode/seek serialize SetOutput/Map/Create*Sample/Flush. Processed can arrive on any MF thread.
class BaleD3D11Upload : public std::enable_shared_from_this<BaleD3D11Upload>
{
public:
    enum class Pool { ZeroCopy, Plain };

    // A mapped staging texture; Create*Sample unmaps it, so does the destructor.
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

    BaleD3D11Upload(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context, Pool pool)
        : device(device), context(context), texturePool(device, 0)
    {
        if (pool == Pool::Plain)
        {
            // The grain pass draws into the target (render target); MF reads it.
            bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            miscFlags = 0;
        }
    }

    ~BaleD3D11Upload()
    {
        Flush();
    }

    // Output size/format for the next sample; a change starts a new generation (Flush).
    void SetOutput(UINT width, UINT height, DXGI_FORMAT format)
    {
        if (width != outputDesc.Width || height != outputDesc.Height || format != outputDesc.Format)
        {
            Flush();
            outputDesc = {};
            outputDesc.Width = width;
            outputDesc.Height = height;
            outputDesc.Format = format;
        }
    }

    // Staged: maps the next texture of a small STAGING ring of the SetOutput size for the CPU to fill.
    // WRITE (not DISCARD): Map waits if the GPU still reads that texture from three frames ago.
    HRESULT Map(UINT width, UINT height, DXGI_FORMAT format, Mapping& target)
    {
        SetOutput(width, height, format);
        return MapStaging(width, height, format, target);
    }

    // Staged, with a processing stage: the ring at the decoded size and the stage's input format, which
    // differ from the pooled output's (SetOutput: the stage's output size and format).
    HRESULT MapStaging(UINT width, UINT height, DXGI_FORMAT format, Mapping& target)
    {
        if (width != stagingWidth || height != stagingHeight || format != stagingFormat)
        {
            for (auto& texture : stagingTextures) texture = nullptr;
            stagingWidth = width;
            stagingHeight = height;
            stagingFormat = format;
        }
        auto& staging = stagingTextures[nextStaging];
        if (!staging)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            auto hr = device->CreateTexture2D(&desc, nullptr, staging.put());
            if (FAILED(hr))
            {
                av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload CreateTexture2D STAGING failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
                return hr;
            }
            BaleMemTrackTexture(staging.get(), BaleMemCategory::Dav1dUpload);
        }
        nextStaging = (nextStaging + 1) % stagingTextures.size();
        target.context = context;
        target.texture = staging;
        auto mapStart = BaleUploadTiming::Now();
        auto hr = context->Map(staging.get(), 0, D3D11_MAP_WRITE, 0, &target.mapped);
        timing.Add(BaleUploadTiming::Map, BaleUploadTiming::Now() - mapStart);
        if (FAILED(hr)) av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload Map failed (hr=0x%08X).\n", static_cast<unsigned>(hr));
        return hr;
    }

    // Staged: unmaps `source` and copies it, or applies GPU grain from it, into a pooled texture.
    // A grain failure throws (the stream fails): dav1d no longer applies grain itself.
    MediaStreamSample CreateStagedSample(Mapping& source, TimeSpan position, IDirect3DSurface* surface,
        BaleFilmGrain& grain, const BaleGrainParams* params)
    {
        auto unmapStart = BaleUploadTiming::Now();
        source.Unmap();
        timing.Add(BaleUploadTiming::Unmap, BaleUploadTiming::Now() - unmapStart);
        return Publish(position, surface, params ? "staged grain" : "staged CopyResource", [&](ID3D11Texture2D* texture)
            {
                if (params) ApplyGrain(grain, *params, source.texture.get(), texture);
                else context->CopyResource(texture, source.texture.get());
                return true;
            });
    }

    // Staged, with a processing stage: unmaps `source` (the decoded size, the stage's input format), moves
    // it into a shader-readable texture (with GPU grain when `params`, else a copy) and runs `processor`
    // into a pooled texture of the SetOutput size and format. The processor is initialized on this
    // uploader's device the first time. Any failure throws (the stream fails; never an unprocessed frame).
    MediaStreamSample CreateProcessedSample(Mapping& source, TimeSpan position, IDirect3DSurface* surface, BaleFilmGrain& grain,
        const BaleGrainParams* params, VideoFrameProcessor& processor, const VideoFrameColorInfo& color, UINT sourceWidth, UINT sourceHeight)
    {
        auto unmapStart = BaleUploadTiming::Now();
        source.Unmap();
        timing.Add(BaleUploadTiming::Unmap, BaleUploadTiming::Now() - unmapStart);
        return Publish(position, surface, "staged stage", [&](ID3D11Texture2D* texture)
            {
                D3D11_TEXTURE2D_DESC sourceDesc{};
                source.texture->GetDesc(&sourceDesc);
                D3D11_TEXTURE2D_DESC landingDesc{};
                if (landing) landing->GetDesc(&landingDesc);
                if (!landing || landingDesc.Width != sourceDesc.Width || landingDesc.Height != sourceDesc.Height ||
                    landingDesc.Format != sourceDesc.Format)
                {
                    landing = nullptr;
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = sourceDesc.Width;
                    desc.Height = sourceDesc.Height;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    desc.Format = sourceDesc.Format;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    // The grain pass draws into it; the stage reads it.
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, landing.put()));
                    BaleMemTrackTexture(landing.get(), BaleMemCategory::OtherTextures);
                }
                if (params) ApplyGrain(grain, *params, source.texture.get(), landing.get());
                else context->CopyResource(landing.get(), source.texture.get());
                if (!processorReady)
                {
                    winrt::check_hresult(processor.Initialize(device.get(), outputDesc.Width, outputDesc.Height, sourceWidth, sourceHeight));
                    processorReady = true;
                }
                winrt::check_hresult(processor.Process(context.get(), landing.get(), 0, texture, color));
                return true;
            });
    }

    // Staged: creates the grain handle now (its self-test included), so a failure shows before the
    // first frame. Throws on failure, recorded in `grain`.
    void PrepareGrain(BaleFilmGrain& grain)
    {
        EnsureGrain(grain);
    }

    // Zero-copy: `landing` is bale_zc_repack's borrowed P010 texture (SetOutput size), valid until
    // the next repack. Copied or grained into a pooled texture here, before that next repack.
    MediaStreamSample CreateLandedSample(ID3D11Texture2D* landing, TimeSpan position, IDirect3DSurface* surface,
        BaleFilmGrain& grain, const BaleGrainParams* params)
    {
        return Publish(position, surface, "zero-copy CopyResource", [&](ID3D11Texture2D* texture)
            {
                // GPU grain failure throws (the stream fails); there is no ungrained fallback.
                if (params) ApplyGrain(grain, *params, landing, texture);
                else context->CopyResource(texture, landing);
                return true;
            });
    }

    // Zero-copy direct write: `write` fills a pooled texture of the SetOutput size itself, or
    // returns false having left it untouched; the texture then goes back to the pool and the
    // sample is null. `position` is read only after a successful write, so `write` may set it.
    // Failures after a write throw, like the other paths.
    template <typename Write>
    MediaStreamSample CreateWrittenSample(const TimeSpan& position, IDirect3DSurface* surface, Write&& write)
    {
        return Publish(position, surface, "zero-copy direct write", std::forward<Write>(write));
    }

    void EndGrain() { DestroyGrain(); }

    // Producer-thread only (same serialization as the Create*Sample calls).
    BaleUploadTiming timing;

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
        outputDesc = {};
    }

private:
    // Fills a pooled texture of the SetOutput size with `draw`, then flushes and wraps it in a
    // tracked MF sample. Any failure throws (the provider latches it and fails the stream). A draw that
    // returns false left the texture untouched: it goes back to the pool and the sample is null.
    // `position` is read only after `draw` succeeded.
    template <typename Draw>
    MediaStreamSample Publish(const TimeSpan& position, IDirect3DSurface* surface, const char* drawOperation, Draw&& draw)
    {
        auto stageStart = BaleUploadTiming::Now();
        auto stage = [&](BaleUploadTiming::Stage done)
        {
            auto now = BaleUploadTiming::Now();
            timing.Add(done, now - stageStart);
            stageStart = now;
        };
        const char* operation = "GetUploadTexture";
        try
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            {
                std::lock_guard lock(samplesMutex);
                winrt::check_hresult(texturePool.GetUploadTexture(outputDesc.Width, outputDesc.Height, outputDesc.Format, bindFlags, miscFlags, texture));
            }
            BaleMemTrackTexture(texture.get(), BaleMemCategory::Dav1dUpload);
            operation = drawOperation;
            if (!draw(texture.get()))
            {
                std::lock_guard lock(samplesMutex);
                texturePool.ReturnUploadTexture(std::move(texture));
                return MediaStreamSample{ nullptr };
            }
            stage(BaleUploadTiming::Gpu);
            operation = "CopyResource/GetDeviceRemovedReason";
            // CopyResource and Flush return void; device removal is the available HRESULT check.
            context->Flush();
            winrt::check_hresult(device->GetDeviceRemovedReason());
            stage(BaleUploadTiming::Flush);

            operation = "GetSurface";
            auto dxgiSurface = texture.as<IDXGISurface>();
            auto directSurface = DirectXInteropHelper::GetSurface(dxgiSurface);
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
            stage(BaleUploadTiming::Sample);
            *surface = directSurface;
            return sample;
        }
        catch (...)
        {
            av_log(nullptr, AV_LOG_WARNING, "Bale D3D11 upload %s failed (hr=0x%08X).\n", operation, static_cast<unsigned>(winrt::to_hresult()));
            throw;
        }
    }

    void* grainHandle = nullptr; // Only the serialized producer/teardown owner touches this.
    void DestroyGrain()
    {
        if (grainHandle)
        {
            BaleGrainExports::Get().gpuDestroy(grainHandle);
            grainHandle = nullptr;
        }
    }

    void EnsureGrain(BaleFilmGrain& grain)
    {
        if (grainHandle) return;
        auto hr = BaleGrainExports::Get().gpuCreate(device.get(), context.get(), &grainHandle);
        if (hr != S_OK || !grainHandle)
        {
            grain.GpuFailed(hr == S_OK ? E_UNEXPECTED : hr, true);
            DestroyGrain();
            winrt::throw_hresult(FAILED(hr) ? hr : E_FAIL);
        }
    }

    // Throws on failure after recording it in `grain` (the stream fails, no ungrained frame).
    void ApplyGrain(BaleFilmGrain& grain, const BaleGrainParams& params, ID3D11Texture2D* source, ID3D11Texture2D* target)
    {
        const auto& api = BaleGrainExports::Get();
        EnsureGrain(grain);
        auto hr = api.gpuApply(grainHandle, &params, source, target);
        if (hr == S_OK) return;
        grain.GpuFailed(hr, false);
        DestroyGrain();
        winrt::throw_hresult(FAILED(hr) ? hr : E_FAIL);
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
    UINT bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    UINT miscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    // Protected by samplesMutex; totals survive Flush, so each diagnostic is logged once per uploader.
    unsigned issuedSamples = 0;
    unsigned processedSamples = 0;
    D3D11_TEXTURE2D_DESC outputDesc{}; // Pooled output size/format; only Width/Height/Format are used.
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> stagingTextures; // Staged path; producer-only.
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    winrt::com_ptr<ID3D11Texture2D> landing; // Staged with a stage: the stage's shader-readable input.
    bool processorReady = false;             // The stage was initialized on this device.
    size_t nextStaging = 0;
    std::map<winrt::Windows::Foundation::IUnknown, TrackedSample> trackedSamples;
    std::mutex samplesMutex;
    std::atomic<bool> processedFailureLogged{ false };
};
