// platform/nativeee (host side) -- the lock and condition the platform's own worker threads share with the game thread
// (the disc reader, the movie decoder: disc_reader.h, movie_player.cpp).
//
// On the Vita they are the kernel's lightweight mutex and condition with their work areas INSIDE the object, and the
// objects live in static storage made before the graphics device exists: vitaGL maps the whole newlib heap into the GPU's
// view, and Vita3K's tracking of the GPU's pages made a heap page under a pthread mutex unreadable mid-game (the build
// pool's lock faulted at the hub's loading -- graphics.cpp PoolMutex, the same pattern). Elsewhere the standard ones.
// Waits never spin: the kernel parks the thread.
#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

namespace te::sync {

#if defined(__vita__)
class Mutex {
public:
    explicit Mutex(const char* name = "te-sync") { sceKernelCreateLwMutex(&work_, name, 0, 0, nullptr); }
    ~Mutex() { sceKernelDeleteLwMutex(&work_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock() { sceKernelLockLwMutex(&work_, 1, nullptr); }
    void unlock() { sceKernelUnlockLwMutex(&work_, 1); }
    bool try_lock() { return sceKernelTryLockLwMutex(&work_, 1) == 0; }
    SceKernelLwMutexWork* Work() { return &work_; }
private:
    alignas(8) SceKernelLwMutexWork work_;
};
class Condition {
public:
    explicit Condition(Mutex& m, const char* name = "te-sync-cond") { sceKernelCreateLwCond(&work_, name, 0, m.Work(), nullptr); }
    ~Condition() { sceKernelDeleteLwCond(&work_); }
    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;
    void wait(std::unique_lock<Mutex>&) { sceKernelWaitLwCond(&work_, nullptr); }
    // false when the time ran out (the caller re-tests its predicate either way)
    bool wait_for_us(std::unique_lock<Mutex>&, unsigned us) {
        SceUInt32 t = us;
        return sceKernelWaitLwCond(&work_, &t) >= 0;
    }
    void notify_one() { sceKernelSignalLwCond(&work_); }
    void notify_all() { sceKernelSignalLwCondAll(&work_); }
private:
    alignas(8) SceKernelLwCondWork work_;
};
#else
class Mutex : public std::mutex {
public:
    explicit Mutex(const char* = nullptr) {}
};
class Condition {
public:
    explicit Condition(Mutex&, const char* = nullptr) {}
    void wait(std::unique_lock<Mutex>& lock) { cv_.wait(lock); }
    bool wait_for_us(std::unique_lock<Mutex>& lock, unsigned us) {
        return cv_.wait_for(lock, std::chrono::microseconds(us)) == std::cv_status::no_timeout;
    }
    void notify_one() { cv_.notify_one(); }
    void notify_all() { cv_.notify_all(); }
private:
    std::condition_variable_any cv_;
};
#endif

}  // namespace te::sync
