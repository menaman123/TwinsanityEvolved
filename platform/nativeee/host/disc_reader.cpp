// disc_reader.cpp -- see disc_reader.h.
#include "disc_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <thread>

#include "engine/cdvd/iso_fs.h"
#include "engine/platform/host/platform.h"
#include "te_load_census.h"
#include "te_prof.h"
#include "te_sync.h"

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

namespace te::io {

namespace {
constexpr size_t kChunk = 256 * 1024;          // a big read's piece (one pread; the queue turns round between pieces)
constexpr size_t kReadAheadMax = 160 * 1024;   // reads up to this size bring the bytes after them into the span cache
constexpr size_t kReadAhead = 256 * 1024;      // ... this many
constexpr int kSpans = 6;                      // the span cache (LRU): ~2.5 MB at most
constexpr unsigned kWaitSliceUs = 100000;      // a waiter re-checks its job (and the fallback) this often
constexpr double kWaitFallbackMs = 3000.0;     // a job still QUEUED after this long is read on the waiting thread
constexpr double kLogWaitMs = 2.0;             // waits longer than this are logged (the first kLogWaits of them)
constexpr int kLogWaits = 40;

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// TE_DISC_URGENT=0: the queue in submission order as before (default: a read a client will wait for goes ahead of the
// reader's own read-aheads and of the big reads' later pieces); TE_DISC_EARLY=0: a read with its read-ahead is one pread and
// done at its end as before (default: the read's own bytes first -- the job done, its waiter released -- then the
// read-ahead into the same span). Neither changes a byte anyone reads: only when the reads come off the card.
bool KnobOn(const char* name) {
    const char* v = std::getenv(name);
    return !(v && v[0] == '0');
}

bool AsyncWanted() {
    const char* v = std::getenv("TE_DISC_ASYNC");
#if defined(__vita__)
    return !(v && v[0] == '0');
#else
    return v && v[0] == '1';
#endif
}

struct Span {
    const IsoFs* iso = nullptr;
    uint64_t begin = 0, end = 0;
    std::vector<uint8_t> bytes;
    uint64_t used = 0;   // the LRU clock at its last use
};
}  // namespace

class Reader {
public:
    static Reader& Get() {
        // Static storage, never destroyed: the reader thread may still wait on the condition while the process exits, and
        // on the Vita the lock's work area must not live in the heap (te_sync.h)
        alignas(Reader) static unsigned char storage[sizeof(Reader)];
        static Reader* reader = new (storage) Reader;
        return *reader;
    }

    void Start() {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (started_) return;
        started_ = true;
        if (!async_) return;
        try {
            std::thread([this] { Run(); }).detach();
        } catch (...) {
            async_ = false;
            std::fprintf(stderr, "[te.io] the disc reader thread did not start: reads stay on the calling threads\n");
            return;
        }
        std::atexit([] { te::io::LogWindow(0, 0); });   // the run's census at exit
        std::fprintf(stderr, "[te.io] disc reads off the game thread: reader thread%s, read-ahead %u KB after reads up to %u KB, "
                     "%d spans (TE_DISC_ASYNC=0: on the calling threads)\n",
#if defined(__vita__)
                     (" at priority " + std::to_string(Priority())).c_str(),
#else
                     "",
#endif
                     unsigned(kReadAhead / 1024), unsigned(kReadAheadMax / 1024), kSpans);
    }
    bool Async() const { return async_; }

    void Submit(Job* job, const IsoFs* iso) {
        job->iso_ = iso;
        job->next_ = 0;
        job->got = 0;
        job->bytes.clear();
        if (!started_) Start();
        if (!async_ || !iso) {
            {
                std::lock_guard<sync::Mutex> lock(mutex_);
                ++census_.submits;
                ++census_.inline_reads;
            }
            RunInline(job);
            job->state_ = Job::State::Done;   // nobody else has seen it
            return;
        }
        std::unique_lock<sync::Mutex> lock(mutex_);
        ++census_.submits;
        if (!job->Progressive() && job->read_ahead && FromCache(job)) {
            job->state_ = Job::State::Done;
            ++census_.cache_hits;
            return;
        }
        job->state_ = Job::State::Queued;
        if (urgent_) {
            // ahead of the reader's own read-aheads and of the big reads' later pieces (their waiters, if any, have their
            // first bytes or none), behind the other reads clients wait for
            auto at = std::find_if(queue_.begin(), queue_.end(), [](const Job* q) { return q->prefetch_ || q->next_ > 0; });
            const size_t passed = size_t(queue_.end() - at);
            if (passed) {
                ++census_.put_ahead;
                census_.passed += passed;
            }
            queue_.insert(at, job);
        } else {
            queue_.push_back(job);
        }
        census_.queue_max = std::max<uint32_t>(census_.queue_max, uint32_t(queue_.size()));
        wake_.notify_one();
    }

    bool Done(const Job* job) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        return job->state_ == Job::State::Done || job->state_ == Job::State::Idle;
    }

    void Wait(Job* job, const char* why) {
        std::unique_lock<sync::Mutex> lock(mutex_);
        if (job->state_ == Job::State::Done || job->state_ == Job::State::Idle) return;
        const double t0 = NowMs();
        bool fell_back = false;
        {
            TE_PROF_SCOPE("disc: waiting for the reader thread");
            while (job->state_ != Job::State::Done) {
                done_.wait_for_us(lock, kWaitSliceUs);
                if (job->state_ == Job::State::Queued && NowMs() - t0 > kWaitFallbackMs) {
                    // The reader has not reached it (stuck behind a failing card?): read it here
                    queue_.erase(std::remove(queue_.begin(), queue_.end(), job), queue_.end());
                    job->state_ = Job::State::Running;
                    lock.unlock();
                    if (job->next_ == 0) {
                        RunInline(job);
                    } else {
                        std::vector<uint8_t> scratch;
                        while (!Step(job, scratch)) {}
                    }
                    lock.lock();
                    job->state_ = Job::State::Done;
                    ++census_.fallbacks;
                    fell_back = true;
                }
            }
        }
        const double ms = NowMs() - t0;
        te::load::Note(te::load::kDiscWait, ms);   // (the game thread's waits only: the load census)
        ++census_.waits;
        census_.wait_ms += ms;
        census_.wait_max_ms = std::max(census_.wait_max_ms, ms);
        if (ms > kLogWaitMs) ++census_.waits_over_2ms;
        const bool log = (ms > kLogWaitMs || fell_back) && logged_waits_ < kLogWaits;
        if (log) ++logged_waits_;
        lock.unlock();
        if (log)
            std::fprintf(stderr, "[te.io] waited %.1f ms at %s for a %s read (%u bytes at +0x%llX)%s%s\n", ms, why, job->site, unsigned(job->size),
                         (unsigned long long)job->offset, fell_back ? " -- the reader had not started it: read on this thread" : "",
                         logged_waits_ == kLogWaits ? " (further waits only in the census)" : "");
    }

    void Release(Job* job) {
        if (!job) return;
        {
            std::lock_guard<sync::Mutex> lock(mutex_);
            if (job->state_ == Job::State::Queued) {
                queue_.erase(std::remove(queue_.begin(), queue_.end(), job), queue_.end());
            } else if (job->state_ == Job::State::Running) {
                job->abandoned_ = true;   // the reader deletes it
                return;
            }
        }
        delete job;
    }

    Census Count() {
        std::lock_guard<sync::Mutex> lock(mutex_);
        return census_;
    }

private:
    Reader() : async_(AsyncWanted()), urgent_(KnobOn("TE_DISC_URGENT")), early_(KnobOn("TE_DISC_EARLY")) {}

#if defined(__vita__)
    static int Priority() {
        // below the game (96) and the render thread (128: a music window's copy into its tracks never delays a present),
        // above the builders (144); its work is mostly waiting in the kernel, and the drive model gives each read frames of
        // slack. TE_DISC_PRIO=64..191 for an A/B
        static const int p = [] {
            const char* v = std::getenv("TE_DISC_PRIO");
            const int q = v ? std::atoi(v) : 132;
            return q >= 64 && q <= 191 ? q : 132;
        }();
        return p;
    }
#endif

    // The job on the calling thread, whole (the reader off, no disc, or the wait's fallback)
    void RunInline(Job* job) {
        job->next_ = 0;
        const bool cache = job->read_ahead;
        job->read_ahead = false;   // no span: the bytes go straight to the job
        std::vector<uint8_t> scratch;
        while (!Step(job, scratch)) {}
        job->read_ahead = cache;
    }

    // Under the lock: the job's bytes from a cached span (true), the next span queued when the hit is near its end
    bool FromCache(Job* job) {
        for (Span& s : spans_) {
            if (s.iso != job->iso_ || s.bytes.empty() || job->offset < s.begin || job->offset + job->size > s.end) continue;
            const size_t at = size_t(job->offset - s.begin);
            job->bytes.assign(s.bytes.begin() + std::ptrdiff_t(at), s.bytes.begin() + std::ptrdiff_t(at + job->size));
            job->got = job->size;
            s.used = ++clock_;
            // sequential pieces: keep a span ahead of them
            if (s.end - (job->offset + job->size) < kReadAhead / 2 && s.end < job->iso_->ImageBytes()) Prefetch(job->iso_, s.end);
            return true;
        }
        return false;
    }

    // Under the lock: one span's read queued (unless one is cached / queued already at that offset)
    void Prefetch(const IsoFs* iso, uint64_t at) {
        for (const Span& s : spans_)
            if (s.iso == iso && !s.bytes.empty() && at >= s.begin && at < s.end) return;
        for (const Job* q : queue_)
            if (q->prefetch_ && q->iso_ == iso && q->offset == at) return;
        Job* p = new Job;
        p->iso_ = iso;
        p->offset = at;
        p->size = kReadAhead;
        p->site = "read-ahead";
        p->prefetch_ = true;
        p->abandoned_ = true;   // nobody waits for it: the reader keeps its bytes as a span and deletes it
        p->state_ = Job::State::Queued;
        queue_.push_back(p);
        ++census_.prefetches;
        wake_.notify_one();
    }

    // Under the lock: a span into the cache (the least recently used one replaced)
    void CacheSpan(const IsoFs* iso, uint64_t begin, std::vector<uint8_t>&& bytes) {
        Span* victim = &spans_[0];
        for (Span& s : spans_) {
            if (s.bytes.empty()) { victim = &s; break; }
            if (s.used < victim->used) victim = &s;
        }
        victim->iso = iso;
        victim->begin = begin;
        victim->end = begin + bytes.size();
        victim->bytes = std::move(bytes);
        victim->used = ++clock_;
    }

    // One piece of the job, off the lock (`scratch`: the calling thread's buffer for a progressive job's piece). True when
    // the job is complete.
    bool Step(Job* job, std::vector<uint8_t>& scratch) {
        const IsoFs* iso = job->iso_;
        if (!iso) { job->got = 0; job->bytes.clear(); return true; }
        const double t0 = NowMs();
        size_t got = 0, want = 0;
        bool finished = false;
        if (job->prefetch_ || (job->next_ == 0 && job->read_ahead && !job->Progressive() && job->size <= kReadAheadMax)) {
            // one read of the job and the bytes after it: the job's part copied out, the whole kept as a span
            want = job->prefetch_ ? job->size : job->size + kReadAhead;
            std::vector<uint8_t> span(want);
            got = iso->ReadBytes(job->offset, want, span.data());
            span.resize(got);
            if (!job->prefetch_) {
                const size_t n = std::min(got, job->size);
                job->bytes.assign(span.begin(), span.begin() + std::ptrdiff_t(n));
                job->got = n;
            }
            if (got) {
                std::lock_guard<sync::Mutex> lock(mutex_);
                CacheSpan(iso, job->offset, std::move(span));
            }
            finished = true;
        } else {
            if (job->next_ == 0 && !job->Progressive()) job->bytes.resize(job->size);
            const size_t piece = job->next_ == 0 && job->first_piece ? std::min(job->first_piece, kChunk) : kChunk;
            want = std::min(piece, job->size - job->next_);
            uint8_t* dst;
            if (job->Progressive()) {
                if (scratch.size() < kChunk) scratch.resize(kChunk);
                dst = scratch.data();
            } else {
                dst = job->bytes.data() + job->next_;
            }
            got = want ? iso->ReadBytes(job->offset + job->next_, want, dst) : 0;
            if (job->Progressive() && got) job->OnChunk(job->next_, dst, got);
            job->next_ += got;
            if (got < want || job->next_ >= job->size) {
                job->got = job->next_;
                if (!job->Progressive()) job->bytes.resize(job->got);
                else job->OnDone();
                finished = true;
            }
        }
        const double ms = NowMs() - t0;
        std::lock_guard<sync::Mutex> lock(mutex_);
        ++census_.preads;
        census_.pread_bytes += got;
        census_.pread_ms += ms;
        return finished;
    }

    // A read with its read-ahead (Step's first branch) in two preads: the job's bytes -- the job done, its waiter released --
    // then the read-ahead after them, the whole kept as one span as before. Called without the lock; returns holding it.
    void RunEarly(Job* job, std::unique_lock<sync::Mutex>& lock) {
        const IsoFs* iso = job->iso_;
        const uint64_t at = job->offset;
        const size_t size = job->size;
        std::vector<uint8_t> span(size + kReadAhead);
        double t0 = NowMs();
        const size_t got = iso ? iso->ReadBytes(at, size, span.data()) : 0;
        double ms = NowMs() - t0;
        lock.lock();
        ++census_.preads;
        census_.pread_bytes += got;
        census_.pread_ms += ms;
        ++census_.early_done;
        job->bytes.assign(span.begin(), span.begin() + std::ptrdiff_t(std::min(got, size)));
        job->got = std::min(got, size);
        job->state_ = Job::State::Done;
        if (job->abandoned_) {
            lock.unlock();
            delete job;
            lock.lock();
        }
        done_.notify_all();
        job = nullptr;   // (the submitter may release it from here)
        lock.unlock();
        size_t ahead = 0;
        if (iso && got == size && at + size < iso->ImageBytes()) {
            t0 = NowMs();
            ahead = iso->ReadBytes(at + size, kReadAhead, span.data() + size);
            ms = NowMs() - t0;
        } else {
            ms = 0;
        }
        span.resize(got + ahead);
        lock.lock();
        if (ahead) {
            ++census_.preads;
            census_.pread_bytes += ahead;
            census_.pread_ms += ms;
        }
        if (!span.empty()) CacheSpan(iso, at, std::move(span));
    }

    void Run() {
#if defined(__vita__)
        sceKernelChangeThreadPriority(0, Priority());
#endif
        te::prof::BindThread(6);   // TE_PROF's "io" slot; the CPU census's te-io
        std::unique_lock<sync::Mutex> lock(mutex_);
        for (;;) {
            {
                TE_PROF_SCOPE("io idle");
                while (queue_.empty()) wake_.wait(lock);
            }
            Job* job = queue_.front();
            queue_.pop_front();
            const bool plain_small = !job->prefetch_ && job->next_ == 0 && job->read_ahead && !job->Progressive() && job->size <= kReadAheadMax;
            if (plain_small && FromCache(job)) {
                // its bytes landed in a span since it was queued (the read-ahead of the read before it): no card read
                ++census_.dequeue_hits;
                job->state_ = Job::State::Done;
                if (job->abandoned_) {
                    lock.unlock();
                    delete job;
                    lock.lock();
                }
                done_.notify_all();
                continue;
            }
            job->state_ = Job::State::Running;
            if (early_ && plain_small) {
                // the job's own bytes first, the job done; then its read-ahead into the same span (RunEarly takes the lock back)
                lock.unlock();
                TE_PROF_SCOPE("io read (the card)");
                RunEarly(job, lock);
                continue;
            }
            lock.unlock();
            bool finished;
            {
                TE_PROF_SCOPE("io read (the card)");
                finished = Step(job, scratch_);
            }
            lock.lock();
            if (job->abandoned_ && !job->prefetch_) {
                // released while it ran: nobody wants the rest
                lock.unlock();
                delete job;
                lock.lock();
                continue;
            }
            if (!finished) {
                job->state_ = Job::State::Queued;   // round robin: the next job's piece first
                queue_.push_back(job);
                continue;
            }
            job->state_ = Job::State::Done;
            if (job->abandoned_) {
                lock.unlock();
                delete job;
                lock.lock();
            }
            done_.notify_all();
        }
    }

    sync::Mutex mutex_{"te-io"};
    sync::Condition wake_{mutex_, "te-io-wake"};
    sync::Condition done_{mutex_, "te-io-done"};
    std::deque<Job*> queue_;
    Span spans_[kSpans];
    uint64_t clock_ = 0;
    std::vector<uint8_t> scratch_;   // the reader thread's own (progressive jobs' pieces)
    Census census_;
    int logged_waits_ = 0;
    bool async_ = false;
    bool started_ = false;
    bool urgent_ = true, early_ = true;
};

void Submit(Job* job, const IsoFs* iso) { if (job) Reader::Get().Submit(job, iso); }
bool Done(const Job* job) { return !job || Reader::Get().Done(job); }
void Wait(Job* job, const char* why) { if (job) Reader::Get().Wait(job, why); }
void Release(Job* job) { Reader::Get().Release(job); }
bool Async() { return Reader::Get().Async(); }
void Start() { Reader::Get().Start(); }
Census Count() { return Reader::Get().Count(); }

void LogWindow(uint64_t from, uint64_t to) {
    static Census last;
    const Census now = Count();
    if (!Async() && now.submits == last.submits) return;
    std::fprintf(stderr, "[te.io] presents %llu-%llu: %llu reads submitted (%llu from the read-ahead cache, %llu read on the calling "
                 "thread), %llu card reads %.1f MB in %.0f ms on the reader thread, %llu read-aheads; the callers waited %llu times "
                 "%.1f ms (max %.1f ms, %llu over 2 ms, %llu fallbacks); queue max %u; put ahead of read-aheads / later pieces %llu "
                 "(passing %llu), done before their read-ahead %llu, found in a span when reached %llu\n",
                 (unsigned long long)from, (unsigned long long)to, (unsigned long long)(now.submits - last.submits),
                 (unsigned long long)(now.cache_hits - last.cache_hits), (unsigned long long)(now.inline_reads - last.inline_reads),
                 (unsigned long long)(now.preads - last.preads), double(now.pread_bytes - last.pread_bytes) / 1048576.0,
                 now.pread_ms - last.pread_ms, (unsigned long long)(now.prefetches - last.prefetches),
                 (unsigned long long)(now.waits - last.waits), now.wait_ms - last.wait_ms, now.wait_max_ms,
                 (unsigned long long)(now.waits_over_2ms - last.waits_over_2ms), (unsigned long long)(now.fallbacks - last.fallbacks),
                 now.queue_max, (unsigned long long)(now.put_ahead - last.put_ahead), (unsigned long long)(now.passed - last.passed),
                 (unsigned long long)(now.early_done - last.early_done), (unsigned long long)(now.dequeue_hits - last.dequeue_hits));
    last = now;
}

}  // namespace te::io
