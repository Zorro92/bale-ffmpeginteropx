#pragma once
// Bale: small fork/join pool for the planar staging fill (FillPlanar). The caller runs slice 0 and
// workers run slices 1..n-1. Workers start on first use and are joined by the destructor.
// Run is called only from the serialized frame producer, so it is never reentered.
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class BaleCopyPool
{
public:
    BaleCopyPool(int threads, int win32Priority) : threads(threads < 1 ? 1 : threads), win32Priority(win32Priority) {}

    ~BaleCopyPool()
    {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        start.notify_all();
        for (auto& worker : workers) worker.join();
    }

    BaleCopyPool(const BaleCopyPool&) = delete;
    BaleCopyPool& operator=(const BaleCopyPool&) = delete;

    int Threads()
    {
        if (threads > 1 && workers.empty()) StartWorkers();
        return threads;
    }

    // Runs job(slice) for every slice in [0, Threads()) and returns when all have finished.
    // The job must not throw.
    void Run(const std::function<void(int)>& job)
    {
        if (Threads() == 1)
        {
            job(0);
            return;
        }
        {
            std::lock_guard lock(mutex);
            current = &job;
            pending = threads - 1;
            generation++;
        }
        start.notify_all();
        job(0);
        std::unique_lock lock(mutex);
        done.wait(lock, [this] { return pending == 0; });
        current = nullptr;
    }

private:
    void StartWorkers()
    {
        try
        {
            for (int slice = 1; slice < threads; slice++)
            {
                workers.emplace_back([this, slice] { Worker(slice); });
            }
        }
        catch (...)
        {
            // Fewer threads only: keep the slices that have a worker.
        }
        threads = static_cast<int>(workers.size()) + 1;
    }

    void Worker(int slice)
    {
        if (win32Priority != THREAD_PRIORITY_NORMAL) SetThreadPriority(GetCurrentThread(), win32Priority);
        uint64_t seen = 0;
        std::unique_lock lock(mutex);
        while (true)
        {
            start.wait(lock, [&] { return stop || generation != seen; });
            if (stop) return;
            seen = generation;
            auto job = current;
            lock.unlock();
            (*job)(slice);
            lock.lock();
            if (--pending == 0) done.notify_one();
        }
    }

    int threads;
    int win32Priority;
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable start;
    std::condition_variable done;
    const std::function<void(int)>* current = nullptr;
    uint64_t generation = 0;
    int pending = 0;
    bool stop = false;
};
