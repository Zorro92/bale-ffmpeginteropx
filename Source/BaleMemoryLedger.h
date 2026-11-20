#pragma once
#include "pch.h"
#include <d3d11.h>
#include <atomic>
#include <cstdint>

extern "C"
{
#include <libavutil/log.h>
}

// Bale's GPU memory ledger, an optional and additive ABI 1 of bale_gpu.dll. Bale's memory view shows
// each component's GPU memory next to the Xbox graphics total, from these two exports:
//   uint32_t bale_mem_abi_version(void);                                     // must return 1
//   void bale_mem_account(uint32_t category, int64_t delta_bytes, int32_t delta_count);  // thread-safe
// Without both exports, or with another version, every function below does nothing. The categories,
// the size rule and the private-data ticket are described in bale/README.md. Header-only, so the seam
// stays in one place.
struct BaleMemoryLedgerExports
{
    uint32_t (__cdecl* abiVersion)() = nullptr;
    void (__cdecl* account)(uint32_t category, int64_t deltaBytes, int32_t deltaCount) = nullptr;

    static const BaleMemoryLedgerExports& Get()
    {
        // One load attempt per process; a usable module stays loaded for the life of the process.
        static const BaleMemoryLedgerExports exports = []
        {
            BaleMemoryLedgerExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module) return api;
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_mem_abi_version"));
            api.account = reinterpret_cast<decltype(api.account)>(GetProcAddress(module, "bale_mem_account"));
            if (!api.abiVersion || !api.account || api.abiVersion() != 1)
            {
                FreeLibrary(module);
                return BaleMemoryLedgerExports{};
            }
            av_log(nullptr, AV_LOG_INFO, "Bale memory ledger: active\n");
            return api;
        }();
        return exports;
    }
};

// Adds to a category's logical bytes and texture count. Any thread: D3D11 releases a ticket, and so calls
// this, on whichever thread drops the texture's last reference.
inline void BaleMemAccount(uint32_t category, int64_t bytes, int32_t count) noexcept
{
    const auto& api = BaleMemoryLedgerExports::Get();
    if (api.account) api.account(category, bytes, count);
}

// Ledger categories, with bale_gpu's numbers. Each counted texture belongs to exactly one.
namespace BaleMemCategory
{
    inline constexpr uint32_t DecoderFrames = 16;        // d3d11va decoder texture array (one per decoder pool)
    inline constexpr uint32_t ToneMapOutput = 17;        // output of the tone-map or downscale stage
    inline constexpr uint32_t Dav1dUpload = 18;          // software AV1 upload textures and staging ring
    inline constexpr uint32_t Rav1gOutputImages = 19;    // rav1g decoder output images
    inline constexpr uint32_t Rav1gPrewarmResident = 20; // the process-wide rav1g device (a count, no bytes)
    inline constexpr uint32_t OtherTextures = 21;        // copy and output textures of the other paths
}

// The logical size of a texture: width x height x bytes per pixel x array slices. Driver padding is not
// counted, and mips are ignored (every texture here has one).
inline uint64_t BaleMemTextureBytes(const D3D11_TEXTURE2D_DESC& desc)
{
    // Twice the bytes per pixel, so that NV12's 1.5 stays an integer.
    uint64_t twice = 8;
    switch (desc.Format)
    {
    case DXGI_FORMAT_NV12: twice = 3; break;
    case DXGI_FORMAT_P010: twice = 6; break;
    case DXGI_FORMAT_R8_UNORM: twice = 2; break;
    case DXGI_FORMAT_R8G8_UNORM: twice = 4; break;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT: twice = 4; break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: twice = 16; break;
    default: break; // Four bytes: B8G8R8A8, R8G8B8A8, R10G10B10A2 and the other 32-bit formats.
    }
    return static_cast<uint64_t>(desc.Width) * desc.Height * desc.ArraySize * twice / 2;
}

// The private-data key of a texture's ticket: a GUID made for this fork, not a Windows-defined one.
inline constexpr GUID BaleMemoryTicketGuid{ 0x1adda65e, 0xd9c2, 0x4083, { 0xac, 0x76, 0xd1, 0xb9, 0xa4, 0xa0, 0x97, 0x5e } };

// The texture's private data. D3D11 holds it for the texture's lifetime and releases it when the texture
// is destroyed (or its private data is replaced), on whichever thread that happens. So the destructor is
// the moment the memory goes, and the allocation is added once, when the ticket is made.
class BaleMemoryTicket final : public ::IUnknown
{
public:
    BaleMemoryTicket(uint32_t ledgerCategory, int64_t ledgerBytes, int32_t ledgerCount) noexcept
        : category(ledgerCategory), bytes(ledgerBytes), count(ledgerCount)
    {
        BaleMemAccount(ledgerCategory, ledgerBytes, ledgerCount);
    }
    ~BaleMemoryTicket() { BaleMemAccount(category, -bytes, -count); }
    BaleMemoryTicket(const BaleMemoryTicket&) = delete;
    BaleMemoryTicket& operator=(const BaleMemoryTicket&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(riid, __uuidof(::IUnknown))) return E_NOINTERFACE;
        *object = static_cast<::IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return ++references;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        auto remaining = --references;
        if (remaining == 0) delete this;
        return remaining;
    }

private:
    const uint32_t category;
    const int64_t bytes;
    const int32_t count;
    std::atomic<ULONG> references{ 1 };
};

// Counts a texture's logical size in `category` until the texture is destroyed. Idempotent: a texture
// that already carries a ticket is not counted again, so callers may report every reuse. A negative
// count means the texture's array slices. Never throws; on any failure the texture is left uncounted.
inline void BaleMemTrackTexture(ID3D11Texture2D* texture, uint32_t category, int32_t count = -1) noexcept
{
    try
    {
        if (!texture || !BaleMemoryLedgerExports::Get().account) return;
        UINT size = 0;
        if (SUCCEEDED(texture->GetPrivateData(BaleMemoryTicketGuid, &size, nullptr)) && size != 0) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        auto bytes = static_cast<int64_t>(BaleMemTextureBytes(desc));
        auto slices = count < 0 ? static_cast<int32_t>(desc.ArraySize) : count;
        // Making the ticket adds the allocation. SetPrivateDataInterface takes its own reference when it
        // succeeds; the release below then leaves the texture holding the only one. When it fails, the
        // release destroys the ticket, which subtracts the allocation again.
        auto ticket = new BaleMemoryTicket(category, bytes, slices);
        texture->SetPrivateDataInterface(BaleMemoryTicketGuid, ticket);
        ticket->Release();
    }
    catch (...)
    {
    }
}
