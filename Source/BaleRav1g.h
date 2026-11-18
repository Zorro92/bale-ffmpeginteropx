#pragma once
#include "pch.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

// rav1g's C interface, copied verbatim from Zorro92/rav1g crates/rav1g/include/rav1g.h (commit
// eebfbd7, interface version 1). rav1g owns the header: copy it again, unchanged, whenever rav1g
// changes it, and raise the expected version below with it.
#include "rav1g.h"
static_assert(RAV1G_ABI_VERSION == 1, "BaleRav1g.h is written against rav1g interface version 1");

// Bale glue for rav1g (GPU AV1 decoding in D3D12 compute shaders): loading rav1g.dll from the app
// package and the process-wide device. Nothing outside
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

    static const BaleRav1gDevice& Get()
    {
        static const BaleRav1gDevice shared = []
        {
            BaleRav1gDevice result;
            const auto& api = BaleRav1gExports::Get();
            if (FAILED(api.status))
            {
                result.status = api.status;
                return result;
            }
            // Two threads compile the pipeline states while playback opens; the first decoder
            // creation waits for whatever is left.
            result.device = api.deviceCreate(nullptr, nullptr, 2);
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
            return result;
        }();
        return shared;
    }
};
