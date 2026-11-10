#pragma once
#include "pch.h"
#include <cstddef>
#include <cstdint>

// Bitmap subtitle ABI 1, mirrored from bale-gpu/src/subs (bale_sub_bitmap_*). The app binds its
// overlay to a token; this fork pushes decoded PAL8 rects for that token. No WinRT/public ABI changes.
struct BaleSubBitmapRect
{
    int32_t x, y, w, h;
    int32_t linesize;
    int32_t nb_colors;
    const uint8_t* indices;
    const uint32_t* palette; // AV_PIX_FMT_PAL8 entries: native-endian 0xAARRGGBB, straight alpha
};
static_assert(sizeof(BaleSubBitmapRect) == 40);

struct BaleSubBitmapEvent
{
    int64_t start_ms;
    int64_t end_ms; // < 0 or <= start_ms: open-ended (until the next event)
    int32_t canvas_w, canvas_h;
    int32_t video_w, video_h;
    const BaleSubBitmapRect* rects;
    int32_t count; // 0 = a clear
    int32_t reserved;
};
static_assert(sizeof(BaleSubBitmapEvent) == 48);

struct BaleSubtitleSinkExports
{
    uint32_t (__cdecl* abiVersion)() = nullptr;
    int32_t (__cdecl* push)(uint64_t, const BaleSubBitmapEvent*) = nullptr;
    int32_t (__cdecl* flush)(uint64_t) = nullptr;

    bool Available() const { return push && flush; }

    static const BaleSubtitleSinkExports& Get()
    {
        // One load attempt per process; a successful module stays loaded for the process lifetime.
        static const BaleSubtitleSinkExports exports = []
        {
            BaleSubtitleSinkExports api;
            auto module = LoadPackagedLibrary(L"bale_gpu.dll", 0);
            if (!module) return api;
            api.abiVersion = reinterpret_cast<decltype(api.abiVersion)>(GetProcAddress(module, "bale_sub_bitmap_abi_version"));
            api.push = reinterpret_cast<decltype(api.push)>(GetProcAddress(module, "bale_sub_bitmap_push"));
            api.flush = reinterpret_cast<decltype(api.flush)>(GetProcAddress(module, "bale_sub_bitmap_flush"));
            auto version = api.abiVersion ? api.abiVersion() : 0;
            if (!api.push || !api.flush || version != 1)
            {
                FreeLibrary(module);
                return BaleSubtitleSinkExports{};
            }
            return api;
        }();
        return exports;
    }
};
