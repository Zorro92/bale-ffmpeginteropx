#pragma once
// Bale: optional priority boost for decoder worker threads.
// dav1d and swscale create their workers with default priority and expose no hook.
// While a boost window is open, every thread created in the process gets the boost
// in DLL_THREAD_ATTACH (see DllMain in FFmpegMediaSource.cpp). Windows are opened
// only around avcodec_open2 (libdav1d) and sws_init_context, which spawn the workers.
#include <atomic>

namespace BaleThreadBoost
{
    inline std::atomic<int> depth{ 0 };
    inline std::atomic<int> priority{ 0 };

    // 0 = off, 1 = THREAD_PRIORITY_ABOVE_NORMAL, 2 = THREAD_PRIORITY_HIGHEST
    inline int ToWin32(int level)
    {
        return level >= 2 ? THREAD_PRIORITY_HIGHEST : level == 1 ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL;
    }

    inline void OnThreadAttach()
    {
        if (depth.load(std::memory_order_acquire) > 0)
        {
            SetThreadPriority(GetCurrentThread(), priority.load(std::memory_order_relaxed));
        }
    }

    struct Window
    {
        bool active;
        explicit Window(int level) : active(level > 0)
        {
            if (active)
            {
                priority.store(ToWin32(level), std::memory_order_relaxed);
                depth.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        ~Window()
        {
            if (active)
            {
                depth.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
    };
}
