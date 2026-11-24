#pragma once
#include "pch.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <mutex>
#include "GpuVideoDecoder.h"
#include "BaleMemoryLedger.h"

extern "C"
{
#include <libavutil/pixdesc.h>
}

// The record of rav1g's timing run (av1_perf_frame, for rav1g_set_perf_callback), copied verbatim
// from Zorro92/rav1g libav1/dx/av1_perf.h at tag v0.7.0-alpha.1 (commit 9f834c7). The record is not
// covered by rav1g's interface version: copy it again, unchanged, from the rav1g release Bale ships,
// and check its size below against rav1g's (crates/rav1g-sys/src/perf.rs). Included before rav1g.h,
// which then takes the full definition instead of declaring the struct.
#include "av1_perf.h"
static_assert(sizeof(void*) != 8 || sizeof(av1_perf_frame) == 640, "av1_perf_frame differs from rav1g v0.7.0-alpha.1's");

// rav1g's C interface, copied verbatim from Zorro92/rav1g crates/rav1g/include/rav1g.h (tag
// v0.7.0-alpha.1, commit 9f834c7, interface version 2). rav1g owns the header: copy it again, unchanged, whenever rav1g
// changes it, and raise the expected version below with it.
#include "rav1g.h"
static_assert(RAV1G_ABI_VERSION == 2, "BaleRav1g.h is written against rav1g interface version 2");

// Bale glue for rav1g (GPU AV1 decoding in D3D12 compute shaders): loading rav1g.dll from the app
// package, the process-wide device, and (below) the GpuVideoDecoder implementation. Nothing outside
// this header names rav1g.

// rav1g.dll's exports, resolved once per process. The library is loaded with LoadPackagedLibrary and
// GetProcAddress only (no import library); a library without every export below, or of another
// interface version, is refused.
struct BaleRav1gExports
{
    decltype(&rav1g_abi_version) abiVersion = nullptr;
    decltype(&rav1g_version) version = nullptr;
    decltype(&rav1g_peek_stream_info) peekStreamInfo = nullptr;
    decltype(&rav1g_device_create) deviceCreate = nullptr;
    decltype(&rav1g_device_d3d12_device) deviceD3D12Device = nullptr;
    decltype(&rav1g_settings_default) settingsDefault = nullptr;
    decltype(&rav1g_decoder_create) decoderCreate = nullptr;
    decltype(&rav1g_decoder_decode) decoderDecode = nullptr;
    decltype(&rav1g_decoder_drain) decoderDrain = nullptr;
    decltype(&rav1g_decoder_configuration) decoderConfiguration = nullptr;
    decltype(&rav1g_decoder_output_fence) decoderOutputFence = nullptr;
    decltype(&rav1g_decoder_destroy) decoderDestroy = nullptr;
    // Optional (diagnostics only): null when the library does not export it.
    decltype(&rav1g_set_perf_callback) setPerfCallback = nullptr;
    HRESULT status = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    const char* failure = "library not loaded";

    static const BaleRav1gExports& Get()
    {
        // One load attempt per process; a usable module stays loaded for the process lifetime.
        static const BaleRav1gExports exports = []
        {
            BaleRav1gExports api;
            auto module = LoadPackagedLibrary(L"rav1g.dll", 0);
            if (!module)
            {
                auto error = GetLastError();
                api.status = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
                api.failure = "library missing";
                return api;
            }
            api.abiVersion = Resolve<decltype(api.abiVersion)>(module, "rav1g_abi_version");
            api.version = Resolve<decltype(api.version)>(module, "rav1g_version");
            api.peekStreamInfo = Resolve<decltype(api.peekStreamInfo)>(module, "rav1g_peek_stream_info");
            api.deviceCreate = Resolve<decltype(api.deviceCreate)>(module, "rav1g_device_create");
            api.deviceD3D12Device = Resolve<decltype(api.deviceD3D12Device)>(module, "rav1g_device_d3d12_device");
            api.settingsDefault = Resolve<decltype(api.settingsDefault)>(module, "rav1g_settings_default");
            api.decoderCreate = Resolve<decltype(api.decoderCreate)>(module, "rav1g_decoder_create");
            api.decoderDecode = Resolve<decltype(api.decoderDecode)>(module, "rav1g_decoder_decode");
            api.decoderDrain = Resolve<decltype(api.decoderDrain)>(module, "rav1g_decoder_drain");
            api.decoderConfiguration = Resolve<decltype(api.decoderConfiguration)>(module, "rav1g_decoder_configuration");
            api.decoderOutputFence = Resolve<decltype(api.decoderOutputFence)>(module, "rav1g_decoder_output_fence");
            api.decoderDestroy = Resolve<decltype(api.decoderDestroy)>(module, "rav1g_decoder_destroy");
            api.setPerfCallback = Resolve<decltype(api.setPerfCallback)>(module, "rav1g_set_perf_callback");
            if (!api.abiVersion || !api.version || !api.peekStreamInfo || !api.deviceCreate || !api.deviceD3D12Device ||
                !api.settingsDefault || !api.decoderCreate || !api.decoderDecode || !api.decoderDrain ||
                !api.decoderConfiguration || !api.decoderOutputFence || !api.decoderDestroy)
            {
                FreeLibrary(module);
                BaleRav1gExports missing;
                missing.status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
                missing.failure = "export missing";
                return missing;
            }
            // Compared before anything else of the library is used (rav1g docs/integration.md).
            auto abi = api.abiVersion();
            if (abi != RAV1G_ABI_VERSION)
            {
                FreeLibrary(module);
                av_log(nullptr, AV_LOG_WARNING, "Bale rav1g: interface version %u, expected %u\n", abi, static_cast<unsigned>(RAV1G_ABI_VERSION));
                BaleRav1gExports mismatch;
                mismatch.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                mismatch.failure = "interface version mismatch";
                return mismatch;
            }
            // rav1g_version is the library's fixed build description, meant for a log.
            auto text = api.version();
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: loaded %s (interface %u)\n", text ? text : "(no version)", abi);
            api.status = S_OK;
            api.failure = nullptr;
            return api;
        }();
        return exports;
    }

private:
    template <typename T>
    static T Resolve(HMODULE module, const char* name)
    {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }
};

// The process's one rav1g device: a D3D12 device, its direct queue and rav1g's compute pipeline
// states. Created on the first rav1g stream of the process and never destroyed or reset (rav1g keeps
// nothing of a stream in it; each decoder has its own buffers, threads and output fence).
//
// rav1g creates the D3D12 device and queue itself, on the first hardware adapter: this library has no
// D3D12 import (the project files are fixed) and a UWP app cannot load d3d12.dll by name. The adapter
// is compared with the Media Foundation D3D11 device's (IDXGIDevice::GetAdapter) when a stream gets
// that device; a stream on another adapter fails. A console has one adapter.
struct BaleRav1gDevice
{
    Rav1gDevice* device = nullptr;
    winrt::com_ptr<ID3D12Device> d3d12;
    LUID adapter{};
    HRESULT status = E_FAIL;

    // pipelineThreads: threads that compile rav1g's pipeline states (~99, about 6.5 s on a Series X
    // with 6) while the call returns. The first caller decides; the device is created once per
    // process. Prewarming (bale_rav1g_prewarm) asks for 6; a first stream without it uses 2.
    static const BaleRav1gDevice& Get(uint32_t pipelineThreads = 2)
    {
        static const BaleRav1gDevice shared = [pipelineThreads]
        {
            BaleRav1gDevice result;
            const auto& api = BaleRav1gExports::Get();
            if (FAILED(api.status))
            {
                result.status = api.status;
                return result;
            }
            // The decoder's first creation waits for whatever compile work is left. With a null device
            // rav1g uses the first hardware adapter, the only one (and MF's) on Xbox; every stream
            // still compares the adapter with MF's.
            result.device = api.deviceCreate(nullptr, nullptr, pipelineThreads);
            if (!result.device)
            {
                av_log(nullptr, AV_LOG_WARNING, "Bale rav1g: device creation failed\n");
                return result;
            }
            result.d3d12.copy_from(static_cast<ID3D12Device*>(api.deviceD3D12Device(result.device)));
            if (!result.d3d12)
            {
                result.status = E_NOINTERFACE;
                return result;
            }
            result.adapter = result.d3d12->GetAdapterLuid();
            result.status = S_OK;
            // The process-wide device is never destroyed, so it is counted once and never subtracted.
            BaleMemAccount(BaleMemCategory::Rav1gPrewarmResident, 0, 1);
            return result;
        }();
        return shared;
    }
};

// bale_rav1g_perf: rav1g's timing run (rav1g_set_perf_callback), summarised for the provider's
// delivery line. Diagnostic only. The callback is process-wide: it is installed while at least one
// stream asks for it and removed when the last such stream ends; with several such streams at once
// the summary mixes their frames. Records arrive on the decoders' completion threads.
class BaleRav1gPerf
{
public:
    struct Window
    {
        uint32_t records = 0;
        double decodeSumMs = 0;  // decode thread: frame setup start to submit done (AV1_PERF_CPU_FRAME)
        double decodeMaxMs = 0;
        uint32_t gpuRecords = 0; // records with GPU timestamps
        double gpuSumMs = 0;     // first to last GPU timestamp of the command list
        double gpuMaxMs = 0;
        uint32_t shown = 0;
        uint32_t showable = 0;
        uint32_t showExisting = 0;
        // Decode-thread waits and the tile decode's wall time (cpu_ms), sum and max per window.
        enum Wait { Output, Slot, FrameBuffer, Image, Entropy, WaitCount };
        uint32_t displayRecords = 0; // records with the display pass (AV1_PERF_GPU_DISPLAY)
        double displaySumMs = 0;
        double displayMaxMs = 0;
        double waitSumMs[WaitCount]{};
        double waitMaxMs[WaitCount]{};
    };

    // Never destroyed: rav1g's threads may still call the callback after it was replaced.
    static BaleRav1gPerf& Get()
    {
        static auto instance = new BaleRav1gPerf();
        return *instance;
    }

    // True when the callback is installed for the caller; a library without the export logs once.
    bool Acquire()
    {
        std::lock_guard lock(installMutex);
        const auto& api = BaleRav1gExports::Get();
        if (FAILED(api.status) || !api.setPerfCallback)
        {
            if (!missingLogged)
            {
                missingLogged = true;
                av_log(nullptr, AV_LOG_WARNING, "Bale rav1g: perf statistics unavailable (no rav1g_set_perf_callback)\n");
            }
            return false;
        }
        if (users++ == 0)
        {
            {
                std::lock_guard dataLock(dataMutex);
                window = Window{};
            }
            api.setPerfCallback(&OnFrame, this);
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: perf statistics on\n");
        }
        return true;
    }

    void Release()
    {
        std::lock_guard lock(installMutex);
        if (users == 0 || --users > 0) return;
        BaleRav1gExports::Get().setPerfCallback(nullptr, nullptr);
        av_log(nullptr, AV_LOG_INFO, "Bale rav1g: perf statistics off\n");
    }

    // bale_rav1g_perf=2: also one log line per record, its pts resolved by this host (the last stream
    // that asked; with several such streams their frames mix). Null unregisters it, if it is that host.
    void SetFrameHost(GpuDecoderHost* newHost, GpuDecoderHost* previous = nullptr)
    {
        std::lock_guard lock(dataMutex);
        if (newHost || frameHost == previous) frameHost = newHost;
    }

    // The records since the last call.
    Window TakeWindow()
    {
        std::lock_guard lock(dataMutex);
        auto result = window;
        window = Window{};
        return result;
    }

private:
    BaleRav1gPerf() = default;

    // Reads only fields of av1_perf_frame as av1_perf.h defines them.
    static void OnFrame(void* context, const av1_perf_frame* frame)
    {
        if (!context || !frame) return;
        auto self = static_cast<BaleRav1gPerf*>(context);
        std::lock_guard lock(self->dataMutex);
        auto& window = self->window;
        window.records++;
        auto decodeMs = frame->cpu_ms[AV1_PERF_CPU_FRAME];
        window.decodeSumMs += decodeMs;
        window.decodeMaxMs = (std::max)(window.decodeMaxMs, decodeMs);
        if (frame->gpu_valid)
        {
            window.gpuRecords++;
            window.gpuSumMs += frame->gpu_span_ms;
            window.gpuMaxMs = (std::max)(window.gpuMaxMs, frame->gpu_span_ms);
        }
        static constexpr int fields[Window::WaitCount] = { AV1_PERF_CPU_WAIT_OUTPUT, AV1_PERF_CPU_WAIT_SLOT,
            AV1_PERF_CPU_WAIT_FB, AV1_PERF_CPU_WAIT_IMAGE, AV1_PERF_CPU_ENTROPY_WALL };
        for (int i = 0; i < Window::WaitCount; i++)
        {
            auto ms = frame->cpu_ms[fields[i]];
            window.waitSumMs[i] += ms;
            window.waitMaxMs[i] = (std::max)(window.waitMaxMs[i], ms);
        }
        if (frame->gpu_valid && frame->gpu_stage_hits[AV1_PERF_GPU_DISPLAY] > 0)
        {
            window.displayRecords++;
            window.displaySumMs += frame->gpu_stage_ms[AV1_PERF_GPU_DISPLAY];
            window.displayMaxMs = (std::max)(window.displayMaxMs, frame->gpu_stage_ms[AV1_PERF_GPU_DISPLAY]);
        }
        if (self->frameHost)
        {
            int64_t pts = -1;
            if (!self->frameHost->UnitTimestamp(frame->user_data, pts)) pts = -1;
            av_log(nullptr, AV_LOG_INFO,
                "Bale rav1g frame u=%llu pts=%lld show=%d sef=%d intra=%d out=%d decode=%.3f entropy=%.3f waitout=%.3f"
                " waitslot=%.3f gpu=%.3f display=%.3f gap=%.3f done=%.3f\n",
                static_cast<unsigned long long>(frame->user_data), static_cast<long long>(pts), frame->show_frame,
                frame->is_sef, frame->intra_frame, frame->output_path, frame->cpu_ms[AV1_PERF_CPU_FRAME],
                frame->cpu_ms[AV1_PERF_CPU_ENTROPY_WALL], frame->cpu_ms[AV1_PERF_CPU_WAIT_OUTPUT],
                frame->cpu_ms[AV1_PERF_CPU_WAIT_SLOT], frame->gpu_valid ? frame->gpu_span_ms : -1.0,
                frame->gpu_valid && frame->gpu_stage_hits[AV1_PERF_GPU_DISPLAY] > 0 ? frame->gpu_stage_ms[AV1_PERF_GPU_DISPLAY] : -1.0,
                frame->gpu_gap_ms,
                frame->complete_ms);
        }
        if (frame->show_frame) window.shown++;
        if (frame->showable_frame) window.showable++;
        if (frame->is_sef) window.showExisting++;
    }

    std::mutex installMutex; // users and the install calls (never held by the callback)
    uint32_t users = 0;
    bool missingLogged = false;
    std::mutex dataMutex;
    Window window;
    GpuDecoderHost* frameHost = nullptr; // dataMutex
};

// GpuVideoDecoder on rav1g, semi-planar output (one NV12 or P010 texture a frame) into the provider's
// images, with a shared output fence the D3D11 side waits on. Settings are rav1g's defaults plus the
// output, the shared fence and the budget callback: low_memory, slots, threads and frames in parse stay
// rav1g's choice (unless bale_rav1g_parse), and of the process-global rav1g_set_* switches only the
// diagnostic perf callback is ever set (bale_rav1g_perf, BaleRav1gPerf).
class BaleRav1gDecoder final : public GpuVideoDecoder
{
public:
    // Direct rav1g output carries this private media type attribute (UINT32 1); Bale's player
    // accepts direct output on this route only with it. Processed output carries the tone-map mark.
    static constexpr winrt::guid OutputMark{ 0x6d553633, 0x9287, 0x4856, { 0xbc, 0x9f, 0x0c, 0x23, 0xaf, 0x00, 0xe1, 0x84 } };

    BaleRav1gDecoder() = default;
    ~BaleRav1gDecoder() override
    {
        Destroy();
        if (perfStatistics) BaleRav1gPerf::Get().Release();
    }
    BaleRav1gDecoder(const BaleRav1gDecoder&) = delete;
    BaleRav1gDecoder& operator=(const BaleRav1gDecoder&) = delete;

    // The open-time check: library, interface version, profile, the container's sequence header and
    // the process device. Null, logged once as a fallback to dav1d, when rav1g cannot take the stream.
    // bitDepth: the depth the stream's frames will have (8 or 10).
    static std::unique_ptr<BaleRav1gDecoder> TryOpen(const AVCodecParameters* parameters, int& bitDepth)
    {
        const auto& api = BaleRav1gExports::Get();
        if (FAILED(api.status)) return Fallback(api.failure, api.status);
        // 4:2:0 and monochrome (Main profile) only; rav1g refuses 4:4:4 and 4:2:2.
        if (parameters->profile != AV_PROFILE_AV1_MAIN && parameters->profile != AV_PROFILE_UNKNOWN)
        {
            return Fallback("profile not Main", parameters->profile);
        }
        auto result = std::make_unique<BaleRav1gDecoder>();
        result->configObus = ConfigObus(parameters);
        int depth = 0;
        if (!result->configObus.empty())
        {
            Rav1gStreamInfo info{};
            auto status = api.peekStreamInfo(result->configObus.data(), result->configObus.size(), &info);
            if (status != RAV1G_STATUS_OK) return Fallback("stream info unsupported", status);
            depth = static_cast<int>(info.bit_depth);
        }
        else
        {
            // No codec configuration (e.g. a raw OBU stream): the depth the demuxer probed.
            auto description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
            depth = description && description->nb_components > 0 ? description->comp[0].depth : parameters->bits_per_raw_sample;
        }
        if (depth != 8 && depth != 10) return Fallback("bit depth unsupported", depth);
        const auto& shared = BaleRav1gDevice::Get();
        if (FAILED(shared.status)) return Fallback("device unavailable", shared.status);
        bitDepth = depth;
        return result;
    }

    static std::nullptr_t Fallback(const char* reason, HRESULT status)
    {
        av_log(nullptr, AV_LOG_WARNING, "Bale rav1g: fallback to dav1d (%s, status=0x%08X)\n", reason, static_cast<unsigned>(status));
        return nullptr;
    }

    const char* LogName() const override { return "Bale rav1g"; }

    HRESULT SetDevice(ID3D11Device* device) override
    {
        const auto& shared = BaleRav1gDevice::Get();
        if (FAILED(shared.status)) return shared.status;
        winrt::com_ptr<ID3D11Device> renderer;
        renderer.copy_from(device);
        auto dxgiDevice = renderer.try_as<IDXGIDevice>();
        if (!dxgiDevice) return E_NOINTERFACE;
        winrt::com_ptr<IDXGIAdapter> adapter;
        auto hr = dxgiDevice->GetAdapter(adapter.put());
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(hr)) hr = adapter->GetDesc(&description);
        if (FAILED(hr)) return hr;
        if (description.AdapterLuid.LowPart != shared.adapter.LowPart || description.AdapterLuid.HighPart != shared.adapter.HighPart)
        {
            // The shared textures and fence must be on the D3D12 device's adapter.
            av_log(nullptr, AV_LOG_ERROR, "Bale rav1g: the renderer device is on another adapter\n");
            return DXGI_ERROR_UNSUPPORTED;
        }
        device11 = renderer.try_as<ID3D11Device5>();
        return device11 ? S_OK : E_NOINTERFACE;
    }

    HRESULT OpenTexture(ID3D11Texture2D* texture, winrt::com_ptr<::IUnknown>& decoderView) override
    {
        winrt::com_ptr<ID3D11Texture2D> texture11;
        texture11.copy_from(texture);
        auto resource = texture11.try_as<IDXGIResource1>();
        if (!resource) return E_NOINTERFACE;
        HANDLE handle = nullptr;
        auto hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        if (FAILED(hr)) return hr;
        winrt::com_ptr<ID3D12Resource> texture12;
        hr = BaleRav1gDevice::Get().d3d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), texture12.put_void());
        CloseHandle(handle);
        if (FAILED(hr)) return hr;
        // rav1g takes the ID3D12Resource pointer itself; keep that exact pointer.
        decoderView.copy_from(static_cast<::IUnknown*>(texture12.get()));
        return S_OK;
    }

    HRESULT Create(const uint8_t* data, size_t size, GpuDecoderHost* newHost) override
    {
        if (decoder) return E_NOT_VALID_STATE;
        if (!device11) return E_NOT_VALID_STATE;
        const auto& api = BaleRav1gExports::Get();
        const auto& shared = BaleRav1gDevice::Get();
        host = newHost;
        if (perFrameLog && perfStatistics) BaleRav1gPerf::Get().SetFrameHost(host);

        // The unit's own sequence header wins (a new sequence); without one, the container's.
        Rav1gStreamInfo info{};
        bool ownSequence = Peek(data, size, info);
        if (!ownSequence)
        {
            if (configObus.empty()) return S_FALSE;
            auto unit = WithConfig(data, size);
            if (!Peek(unit.data(), unit.size(), info)) return S_FALSE;
        }
        if (info.bit_depth > 10) return StatusError(RAV1G_STATUS_UNSUPPORTED_BITSTREAM);

        Rav1gSettings settings{};
        api.settingsDefault(&settings);
        settings.width = info.width;
        settings.height = info.height;
        settings.bit_depth = info.bit_depth == 8 ? 8 : 10;
        settings.memory_available = &MemoryAvailable;
        settings.memory_available_context = this;
        settings.output_kind = RAV1G_OUTPUT_SEMI_PLANAR;
        settings.shared_fence = true;
        settings.output.context = this;
        settings.output.get_image = &GetImage;
        settings.output.frame = &Frame;
        settings.output.unused = &Unused;
        settings.output.end_of_frames = nullptr;
        if (displayRequested && !CanWriteDisplayImages())
        {
            // The display pass writes the images with a shader; without that, the stage stays.
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: display pass not used (no unordered-access images); the stage processes the frames\n");
            displayRequested = false;
        }
        if (displayRequested)
        {
            // The stage's work in rav1g's own command list (Bale's configuration: BT.2390, 1000-nit
            // peak, the stage's SDR white, no desaturation); the images come at the display size.
            settings.display.width = display.width;
            settings.display.height = display.height;
            settings.display.transform = RAV1G_DISPLAY_TRANSFORM_AUTO;
            settings.display.tone_curve = RAV1G_TONE_CURVE_BT2390;
            settings.display.source_peak_nits = 1000.0f;
            settings.display.target_white_nits = display.whiteNits;
            settings.display.desaturation = 0.0f;
            settings.display.depth = display.depth;
            settings.display.dither = display.depth == 8 && display.dither ? RAV1G_DISPLAY_DITHER_AUTO : RAV1G_DISPLAY_DITHER_OFF;
            settings.display.assumed_primaries = display.primaries;
            settings.display.assumed_transfer_characteristics = display.transfer;
            settings.display.assumed_matrix_coefficients = display.matrix;
            settings.display.assumed_chroma_sample_position = static_cast<uint8_t>(
                display.chromaLocation == AVCHROMA_LOC_LEFT ? RAV1G_CHROMA_SAMPLE_POSITION_VERTICAL :
                display.chromaLocation == AVCHROMA_LOC_TOPLEFT ? RAV1G_CHROMA_SAMPLE_POSITION_COLOCATED : 0);
        }
        if (parseFrames > 0)
        {
            // Frames in parse and parse threads (rav1g: frames in parse never exceed the threads; the
            // budget guard may still give fewer). 0 keeps rav1g's defaults (1 and 1 up to 1440p).
            settings.frames_in_parse = parseFrames;
            settings.threads = parseFrames;
        }
        auto status = api.decoderCreate(shared.device, &settings, &decoder);
        if (displayRequested && status == RAV1G_STATUS_INCAPABLE)
        {
            // rav1g can't do this configuration (or the device can't write the format): the provider's
            // stage takes the frames as before.
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: display pass declined (incapable); the stage processes the frames\n");
            settings.display = {};
            decoder = nullptr;
            status = api.decoderCreate(shared.device, &settings, &decoder);
        }
        else if (displayRequested && status == RAV1G_STATUS_OK)
        {
            av_log(nullptr, AV_LOG_INFO, "Bale rav1g: display pass %ux%u depth=%u\n", display.width, display.height, display.depth);
        }
        if (status != RAV1G_STATUS_OK || !decoder)
        {
            decoder = nullptr;
            av_log(nullptr, AV_LOG_ERROR, "Bale rav1g: decoder creation failed (status %d)\n", static_cast<int>(status));
            return StatusError(status != RAV1G_STATUS_OK ? status : RAV1G_STATUS_ERROR);
        }

        // Each decoder has its own output fence; open it on the renderer's device.
        HANDLE handle = nullptr;
        auto fence12 = static_cast<ID3D12Fence*>(api.decoderOutputFence(decoder));
        auto hr = fence12 ? shared.d3d12->CreateSharedHandle(fence12, nullptr, GENERIC_ALL, nullptr, &handle) : E_POINTER;
        if (SUCCEEDED(hr))
        {
            hr = device11->OpenSharedFence(handle, __uuidof(ID3D11Fence), fence11.put_void());
            CloseHandle(handle);
        }
        if (FAILED(hr))
        {
            av_log(nullptr, AV_LOG_ERROR, "Bale rav1g: output fence could not be shared (hr=0x%08X)\n", static_cast<unsigned>(hr));
            Destroy();
            return hr;
        }

        Rav1gConfiguration configuration{};
        api.decoderConfiguration(decoder, &configuration);
        deviceBytes = configuration.device_size;
        // A decoder with no device bytes is not counted: Destroy subtracts only when ledgerBytes is set.
        ledgerBytes = deviceBytes;
        if (ledgerBytes) BaleMemAccount(BaleMemCategory::Rav1gDecoderBuffers, static_cast<int64_t>(ledgerBytes), 1);
        av_log(nullptr, AV_LOG_INFO, "Bale rav1g: active %ux%u bits=%u slots=%u parse=%u threads=%u device=%.1f MiB\n",
            info.width, info.height, settings.bit_depth, configuration.frame_slots, configuration.frames_in_parse,
            configuration.threads, configuration.device_size / (1024.0 * 1024.0));
        sendConfig = !ownSequence;
        return S_OK;
    }

    bool IsCreated() const override { return decoder != nullptr; }

    HRESULT Decode(const uint8_t* data, size_t size, uint64_t userData) override
    {
        if (!decoder) return E_NOT_VALID_STATE;
        const auto& api = BaleRav1gExports::Get();
        Rav1gStatus status;
        if (sendConfig)
        {
            // The first unit of a decoder created from the container's sequence header carries it.
            auto unit = WithConfig(data, size);
            status = api.decoderDecode(decoder, unit.data(), unit.size(), userData);
        }
        else
        {
            status = api.decoderDecode(decoder, data, size, userData);
        }
        if (status == RAV1G_STATUS_NEEDS_RECONFIGURE) return ReconfigureRequired;
        sendConfig = false;
        return status == RAV1G_STATUS_OK ? S_OK : StatusError(status);
    }

    HRESULT Drain() override
    {
        if (!decoder) return S_OK;
        auto status = BaleRav1gExports::Get().decoderDrain(decoder);
        return status == RAV1G_STATUS_OK ? S_OK : StatusError(status);
    }

    void Destroy() override
    {
        // Unregistered before the host goes away (the provider destroys the decoder first); Create
        // registers it again after a reconfigure.
        if (perFrameLog) BaleRav1gPerf::Get().SetFrameHost(nullptr, host);
        if (decoder)
        {
            // Waits for the frames in flight and hands them over (Frame/Unused) first.
            BaleRav1gExports::Get().decoderDestroy(decoder);
            decoder = nullptr;
            if (ledgerBytes)
            {
                BaleMemAccount(BaleMemCategory::Rav1gDecoderBuffers, -static_cast<int64_t>(ledgerBytes), -1);
                ledgerBytes = 0;
            }
        }
        fence11 = nullptr;
    }

    void DescribeOutput(winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties) const override
    {
        properties.Insert(OutputMark, winrt::Windows::Foundation::PropertyValue::CreateUInt32(1));
    }

    // bale_rav1g_perf=1: rav1g's per-frame timing on the provider's delivery line. Before the first
    // decode of the stream.
    void EnablePerfStatistics(bool perFrame = false)
    {
        if (!perfStatistics) perfStatistics = BaleRav1gPerf::Get().Acquire();
        perFrameLog = perFrame;
    }

    uint64_t DeviceBytes() const override { return deviceBytes; }

    // Whether the renderer's device creates display-size images as the image pool does, with unordered
    // access (the pool otherwise falls back to images without it, which the display pass can't write).
    bool CanWriteDisplayImages() const
    {
        if (!device11) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = display.width;
        desc.Height = display.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = display.depth == 8 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        winrt::com_ptr<ID3D11Texture2D> texture;
        return SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, texture.put()));
    }

    bool SetDisplayOutput(const GpuDisplayOutput& output) override
    {
        if (!output.width || !output.height || (output.depth != 8 && output.depth != 10)) return false;
        display = output;
        displayRequested = true;
        return true;
    }

    void AppendStatistics(std::string& line) override
    {
        if (!perfStatistics) return;
        auto window = BaleRav1gPerf::Get().TakeWindow();
        char text[512];
        auto avg = [&](int i) { return window.records ? window.waitSumMs[i] / window.records : 0.0; };
        using W = BaleRav1gPerf::Window;
        auto length = snprintf(text, sizeof(text),
            " rav1g perf records=%u decode thread avg=%.1f max=%.1f ms gpu avg=%.1f max=%.1f ms shown=%u showable=%u sef=%u"
            " wait output avg=%.1f max=%.1f slot avg=%.1f max=%.1f fb avg=%.1f max=%.1f image avg=%.1f max=%.1f ms"
            " entropy wall avg=%.1f max=%.1f ms display n=%u avg=%.2f max=%.2f ms",
            window.records, window.records ? window.decodeSumMs / window.records : 0.0, window.decodeMaxMs,
            window.gpuRecords ? window.gpuSumMs / window.gpuRecords : 0.0, window.gpuMaxMs,
            window.shown, window.showable, window.showExisting,
            avg(W::Output), window.waitMaxMs[W::Output], avg(W::Slot), window.waitMaxMs[W::Slot],
            avg(W::FrameBuffer), window.waitMaxMs[W::FrameBuffer], avg(W::Image), window.waitMaxMs[W::Image],
            avg(W::Entropy), window.waitMaxMs[W::Entropy],
            window.displayRecords, window.displayRecords ? window.displaySumMs / window.displayRecords : 0.0, window.displayMaxMs);
        if (length > 0) line.append(text, (std::min)(static_cast<size_t>(length), sizeof(text) - 1));
    }

private:
    // A rav1g status as an HRESULT: 0x80042B20 + status.
    static HRESULT StatusError(Rav1gStatus status)
    {
        return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2B20 + static_cast<int>(status));
    }

    static bool Peek(const uint8_t* data, size_t size, Rav1gStreamInfo& info)
    {
        if (!data || !size) return false;
        info = {};
        auto status = BaleRav1gExports::Get().peekStreamInfo(data, size, &info);
        return status == RAV1G_STATUS_OK && info.bit_depth != 0 && info.width != 0 && info.height != 0;
    }

    // The configuration OBUs of the codec configuration (av1C: four bytes, then the OBUs; otherwise
    // the extradata is OBUs already). Matroska requires the sequence header there and lets blocks
    // leave it out.
    static std::vector<uint8_t> ConfigObus(const AVCodecParameters* parameters)
    {
        if (!parameters->extradata || parameters->extradata_size <= 0) return {};
        auto data = parameters->extradata;
        auto size = static_cast<size_t>(parameters->extradata_size);
        if (data[0] & 0x80)
        {
            if (size <= 4) return {};
            return std::vector<uint8_t>(data + 4, data + size);
        }
        return std::vector<uint8_t>(data, data + size);
    }

    // The unit with the configuration OBUs in front, behind a leading temporal delimiter.
    std::vector<uint8_t> WithConfig(const uint8_t* data, size_t size) const
    {
        size_t delimiter = 0;
        if (size >= 2 && ((data[0] >> 3) & 0x0F) == 2 && (data[0] & 0x02))
        {
            // OBU_TEMPORAL_DELIMITER with a size field: header (1 or 2 bytes) and size 0.
            size_t header = (data[0] & 0x04) ? 2 : 1;
            if (size > header && data[header] == 0) delimiter = header + 1;
        }
        std::vector<uint8_t> unit;
        unit.reserve(size + configObus.size());
        unit.insert(unit.end(), data, data + delimiter);
        unit.insert(unit.end(), configObus.begin(), configObus.end());
        unit.insert(unit.end(), data + delimiter, data + size);
        return unit;
    }

    // color: the frame's (the image's) or its source_color (what was decoded, with the display pass).
    static VideoFrameColorInfo ColorInfo(const Rav1gFrame& frame, const Rav1gColorInfo& color)
    {
        // AV1 code points are H.273's, like FFmpeg's; 2 (unspecified) without a colour description.
        VideoFrameColorInfo info;
        info.transfer = color.transfer_characteristics;
        info.primaries = color.primaries;
        info.matrix = color.matrix_coefficients;
        info.range = static_cast<uint8_t>(color.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG);
        info.chromaLocation = static_cast<uint8_t>(
            color.chroma_sample_position == RAV1G_CHROMA_SAMPLE_POSITION_VERTICAL ? AVCHROMA_LOC_LEFT :
            color.chroma_sample_position == RAV1G_CHROMA_SAMPLE_POSITION_COLOCATED ? AVCHROMA_LOC_TOPLEFT :
            AVCHROMA_LOC_UNSPECIFIED);
        if (frame.has_mastering_display)
        {
            // The metadata OBU's fixed point: chromaticity 0.16, max luminance 24.8, min 18.14.
            info.hasMasteringPrimaries = true;
            info.hasMasteringLuminance = true;
            for (int i = 0; i < 3; i++)
            {
                for (int j = 0; j < 2; j++)
                {
                    info.masteringPrimaries[i][j] = frame.mastering_display.primaries[i][j] / 65536.0f;
                }
            }
            info.masteringWhite[0] = frame.mastering_display.white_point[0] / 65536.0f;
            info.masteringWhite[1] = frame.mastering_display.white_point[1] / 65536.0f;
            info.masteringMaxNits = frame.mastering_display.max_luminance / 256.0f;
            info.masteringMinNits = frame.mastering_display.min_luminance / 16384.0f;
        }
        if (frame.has_content_light)
        {
            info.hasContentLight = true;
            info.maxCLL = frame.content_light.max_content_light_level;
            info.maxFALL = frame.content_light.max_frame_average_light_level;
        }
        return info;
    }

    // rav1g's callbacks: get_image/unused on the decode thread or a parse thread, frame on the
    // completion thread.
    static int32_t GetImage(void* context, const Rav1gImageRequest* request, Rav1gImage* image)
    {
        auto self = static_cast<BaleRav1gDecoder*>(context);
        *image = {};
        DXGI_FORMAT format = request->format == RAV1G_IMAGE_FORMAT_P010 ? DXGI_FORMAT_P010 :
            request->format == RAV1G_IMAGE_FORMAT_NV12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_UNKNOWN;
        if (format == DXGI_FORMAT_UNKNOWN || !self->host) return 1;
        void* view = nullptr;
        void* token = nullptr;
        // A non-zero result fails the temporal unit: only for a cancel (stop, seek) or a failure.
        if (!self->host->AcquireImage((request->width + 1) & ~1u, (request->height + 1) & ~1u, format, &view, &token)) return 1;
        image->d3d12_textures[0] = view;
        image->token = token;
        return 0;
    }

    static void Frame(void* context, const Rav1gFrame* frame)
    {
        auto self = static_cast<BaleRav1gDecoder*>(context);
        GpuDecodedFrame decoded;
        decoded.image = frame->image.token;
        decoded.fence = self->fence11;
        decoded.fenceValue = frame->fence_value;
        decoded.width = frame->width;
        decoded.height = frame->height;
        decoded.format = frame->format == RAV1G_IMAGE_FORMAT_P010 ? DXGI_FORMAT_P010 :
            frame->format == RAV1G_IMAGE_FORMAT_NV12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_UNKNOWN;
        decoded.color = ColorInfo(*frame, frame->color);
        decoded.userData = frame->user_data;
        if (frame->display_transform != RAV1G_DISPLAY_TRANSFORM_NONE)
        {
            decoded.displayOutput = true;
            decoded.sourceWidth = frame->source_width;
            decoded.sourceHeight = frame->source_height;
            decoded.sourceColor = ColorInfo(*frame, frame->source_color);
        }
        self->host->FrameReady(std::move(decoded));
    }

    static void Unused(void* context, void* token)
    {
        static_cast<BaleRav1gDecoder*>(context)->host->ImageUnused(token);
    }

    static int64_t MemoryAvailable(void* context)
    {
        auto self = static_cast<BaleRav1gDecoder*>(context);
        return self->host ? self->host->MemoryAvailable() : -1;
    }

public:
    // bale_rav1g_parse: frames in parse and parse threads for this stream's decoder (0 = rav1g's choice).
    uint32_t parseFrames = 0;

private:
    // bale_rav1g_perf: this decoder holds a use of the process-wide perf callback.
    bool perfStatistics = false;
    bool perFrameLog = false;

private:
    std::vector<uint8_t> configObus;
    uint64_t deviceBytes = 0;
    // Bytes reported to the memory ledger for this decoder, 0 when none.
    uint64_t ledgerBytes = 0;
    Rav1gDecoder* decoder = nullptr;
    GpuDecoderHost* host = nullptr;
    winrt::com_ptr<ID3D11Device5> device11;
    winrt::com_ptr<ID3D11Fence> fence11;
    bool sendConfig = false;
    GpuDisplayOutput display;
    bool displayRequested = false;
};
