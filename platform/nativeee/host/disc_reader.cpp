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
constexpr size_t kEntryReserve = 6u << 20;     // of the extents' budget, kept for the archive entries (an RM2: up to ~5 MB)
constexpr size_t kConsumedSlack = 64 * 1024;
constexpr size_t kLinksPieceMax = 512 * 1024;  // a linked chunk's pieces (they run with a minute to spare)   // an archive entry's extent is read through once a read ends this near its end

// The queue's ranks (a job goes behind every job of its rank or a lower one)
enum Rank : uint8_t {
    kRankWaited = 0,   // a client's read, and an extent's piece a client's read waits on
    kRankLater = 1,    // a big read's later pieces (round robin among them)
    kRankAhead = 2,    // the spans' read-aheads and the archive entries' windows
    kRankLinks = 3,    // the linked chunks
};

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

// TE_DISC_AHEAD_MB: the extents' budget (the Vita's default 16 MB: its newlib heap has ~105 MB free in play and reaches
// ~144 MB of its 224 at the heaviest levels -- device logs' "[te.gfx] heap:" lines; 0 = no extents: the reads still land as
// the card delivers them, never waited for); elsewhere 32 MB
size_t BudgetWanted() {
    const char* v = std::getenv("TE_DISC_AHEAD_MB");
#if defined(__vita__)
    const long mb = v ? std::atol(v) : 16;
#else
    const long mb = v ? std::atol(v) : 32;
#endif
    return mb > 0 ? size_t(std::min(mb, 256L)) << 20 : 0;
}

// TE_DISC_PIECE_KB: an extent's pread (default 1 MB)
size_t PieceWanted() {
    const char* v = std::getenv("TE_DISC_PIECE_KB");
    const long kb = v ? std::atol(v) : 1024;
    return size_t(std::max(64L, std::min(kb, 8192L))) * 1024;
}

// TE_DISC_SLOW_MS=MS[,PER_READ]: a slower card on the reader thread -- MS ms for every 128 KB a read brings, PER_READ ms
// more for every read (the PS Vita's card in play: ~53 ms a 128 KB, 90-240 ms a 215 KB read)
struct Slow { double per_128k = 0, per_read = 0; bool on = false; };
const Slow& SlowCard() {
    static const Slow s = [] {
        Slow r;
        if (const char* v = std::getenv("TE_DISC_SLOW_MS")) {
            r.per_128k = std::atof(v);
            if (const char* comma = std::strchr(v, ',')) r.per_read = std::atof(comma + 1);
            r.on = r.per_128k > 0 || r.per_read > 0;
        }
        return r;
    }();
    return s;
}

// A read of the card on the reader thread (the slow card's time first)
size_t ReadCard(const IsoFs* iso, uint64_t offset, size_t n, uint8_t* dst) {
    const Slow& slow = SlowCard();
    if (slow.on) {
        const double ms = slow.per_read + slow.per_128k * double(n) / 131072.0;
        std::this_thread::sleep_for(std::chrono::microseconds(int64_t(ms * 1000.0)));
    }
    return iso->ReadBytes(offset, n, dst);
}

bool BenchWanted() {
    const char* v = std::getenv("TE_DISC_BENCH");
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

// The census's client of a job, by its site name
enum Client { kStream, kMusic, kMovie, kOther, kLoadPoll };
Client ClientOf(const Job* job) {
    const char* s = job->site;
    if (!std::strcmp(s, "stream") || !std::strcmp(s, "sound bank")) return kStream;
    if (!std::strcmp(s, "music window")) return kMusic;
    if (!std::strcmp(s, "movie slice")) return kMovie;
    return kOther;
}
}  // namespace

// A range of the image read ahead whole (disc_reader.h: extents)
struct Extent {
    const IsoFs* iso = nullptr;
    uint64_t begin = 0, end = 0;          // [begin, end) of the image
    std::unique_ptr<uint8_t[]> bytes;     // end - begin, made at once (the reader writes into it off the lock: never moved)
    uint64_t filled = 0;                  // [begin, begin + filled) read
    size_t first = 0;                     // its first piece (0: the reader's piece)
    Ahead why = Ahead::Links;
    std::string tag;
    Job* piece = nullptr;                 // its next piece, queued or being read
    bool running = false;                 // that piece is being read
    bool dropped = false;                 // let go of while its piece was being read: the reader deletes it after
    bool ended = false;                   // the card gave fewer bytes than asked: no more pieces
    bool hit = false;                     // a read was served from it
    bool consumed = false;                // an archive entry's: the reads came to its end
    std::vector<Job*> waiters;            // reads inside it past `filled`: done when the fill passes their end
    uint64_t used = 0;                    // the LRU clock at its last use
    size_t Size() const { return size_t(end - begin); }
};

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
        const Slow& slow = SlowCard();
        char slowed[160] = "";
        if (slow.on)
            std::snprintf(slowed, sizeof slowed, "; the card slowed (TE_DISC_SLOW_MS): %.1f ms for every 128 KB + %.1f ms a read",
                          slow.per_128k, slow.per_read);
        std::fprintf(stderr, "[te.io] disc reads off the game thread: reader thread%s, read-ahead %u KB after reads up to %u KB, "
                     "%d spans; ranges read ahead whole in %u KB pieces within a budget of %u MB (TE_DISC_AHEAD_MB)%s "
                     "(TE_DISC_ASYNC=0: on the calling threads)\n",
#if defined(__vita__)
                     (" at priority " + std::to_string(Priority())).c_str(),
#else
                     "",
#endif
                     unsigned(kReadAhead / 1024), unsigned(kReadAheadMax / 1024), kSpans, unsigned(piece_ / 1024),
                     unsigned(budget_ >> 20), slowed);
    }
    bool Async() const { return async_; }
    size_t Budget() const { return async_ ? budget_ : 0; }

    void Submit(Job* job, const IsoFs* iso) {
        job->iso_ = iso;
        job->next_ = 0;
        job->got = 0;
        job->bytes.clear();
        job->waits_on_ = nullptr;
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
        if (!job->Progressive()) {
            if (Extent* e = ExtentOf(job)) {
                if (Serve(e, job)) {
                    job->state_ = Job::State::Done;
                    ++census_.ahead_hits;
                    return;
                }
                if (Attach(e, job)) return;
            }
            if (job->read_ahead && FromCache(job)) {
                job->state_ = Job::State::Done;
                ++census_.cache_hits;
                return;
            }
        }
        job->state_ = Job::State::Queued;
        job->rank_ = kRankWaited;
        Insert(job, true);
        wake_.notify_one();
    }

    bool Done(const Job* job) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        return job->state_ == Job::State::Done || job->state_ == Job::State::Idle;
    }

    // Until done, or (limit_ms >= 0) until limit_ms have passed: true when done
    bool Wait(Job* job, const char* why, double limit_ms = -1) {
        std::unique_lock<sync::Mutex> lock(mutex_);
        if (job->state_ == Job::State::Done || job->state_ == Job::State::Idle) return true;
        if (limit_ms == 0) return false;
        const double t0 = NowMs();
        bool fell_back = false;
        {
            TE_PROF_SCOPE("disc: waiting for the reader thread");
            while (job->state_ != Job::State::Done) {
                if (limit_ms > 0) {
                    const double left = limit_ms - (NowMs() - t0);
                    if (left <= 0) break;
                    done_.wait_for_us(lock, unsigned(std::min<double>(kWaitSliceUs, left * 1000.0)) + 1);
                    continue;
                }
                done_.wait_for_us(lock, kWaitSliceUs);
                if (job->state_ == Job::State::Queued && NowMs() - t0 > kWaitFallbackMs) {
                    // The reader has not reached it (stuck behind a failing card?): read it here
                    Unqueue(job);
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
        const bool done = job->state_ == Job::State::Done;
        const double ms = NowMs() - t0;
        te::load::Note(te::load::kDiscWait, ms);   // (the game thread's waits only: the load census)
        ++census_.waits;
        census_.wait_ms += ms;
        census_.wait_max_ms = std::max(census_.wait_max_ms, ms);
        switch (limit_ms > 0 ? kLoadPoll : ClientOf(job)) {
        case kLoadPoll: ++census_.waits_load; census_.wait_ms_load += ms; break;
        case kStream: ++census_.waits_stream; census_.wait_ms_stream += ms; break;
        case kMusic: ++census_.waits_music; census_.wait_ms_music += ms; break;
        case kMovie: ++census_.waits_movie; census_.wait_ms_movie += ms; break;
        default: ++census_.waits_other; census_.wait_ms_other += ms; break;
        }
        if (ms > kLogWaitMs) ++census_.waits_over_2ms;
        const bool log = (ms > kLogWaitMs || fell_back) && logged_waits_ < kLogWaits;
        if (log) ++logged_waits_;
        lock.unlock();
        if (log)
            std::fprintf(stderr, "[te.io] waited %.1f ms at %s for a %s read (%u bytes at +0x%llX)%s%s%s\n", ms, why, job->site, unsigned(job->size),
                         (unsigned long long)job->offset, fell_back ? " -- the reader had not started it: read on this thread" : "",
                         done ? "" : " -- not in yet: the frame went on",
                         logged_waits_ == kLogWaits ? " (further waits only in the census)" : "");
        return done;
    }

    void Release(Job* job) {
        if (!job) return;
        {
            std::lock_guard<sync::Mutex> lock(mutex_);
            if (job->state_ == Job::State::Queued) {
                Unqueue(job);
            } else if (job->state_ == Job::State::Running) {
                job->abandoned_ = true;   // the reader deletes it
                return;
            }
        }
        delete job;
    }

    Census Count() {
        std::lock_guard<sync::Mutex> lock(mutex_);
        Census c = census_;
        c.ahead_held = ahead_held_;
        c.ahead_links_held = 0;
        for (const Extent* e : extents_) if (e->why == Ahead::Links) c.ahead_links_held += e->Size();
        return c;
    }

    // ----- extents ----------------------------------------------------------------------------------------------------
    bool ReadAhead(const IsoFs* iso, uint64_t begin, uint64_t end, Ahead why, const std::string& tag, size_t first) {
        if (!iso) return false;
        if (!started_) Start();
        if (!async_ || !budget_) return false;
        end = std::min<uint64_t>(end, iso->ImageBytes());
        if (end <= begin) return false;
        std::unique_lock<sync::Mutex> lock(mutex_);
        for (const Extent* e : extents_)
            if (e->iso == iso && e->begin <= begin && end <= e->end) return false;   // held already
        const size_t n = size_t(end - begin);
        if (!MakeRoom(n, why)) {
            ++census_.ahead_refused;
            return false;
        }
        auto* e = new Extent;
        e->bytes.reset(new (std::nothrow) uint8_t[n]);
        if (!e->bytes) {
            delete e;
            ++census_.ahead_refused;
            return false;
        }
        e->iso = iso;
        e->begin = begin;
        e->end = end;
        e->first = first;
        e->why = why;
        e->tag = tag;
        e->used = ++clock_;
        extents_.push_back(e);
        ahead_held_ += n;
        census_.ahead_held_max = std::max<uint64_t>(census_.ahead_held_max, ahead_held_);
        ++(why == Ahead::Links ? census_.ahead_links : census_.ahead_entries);
        QueuePiece(e);
        wake_.notify_one();
        return true;
    }

    void PauseLinks(bool paused) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (paused == links_paused_) return;
        links_paused_ = paused;
        if (!paused) wake_.notify_one();
    }

    void Idle() {
        std::unique_lock<sync::Mutex> lock(mutex_);
        while (!extents_.empty()) Drop(extents_.back());
        while (!queue_.empty() || busy_) done_.wait_for_us(lock, kWaitSliceUs);
        for (Span& span : spans_) span = Span{};   // (a disc made later may sit at the same address)
    }

    bool AheadHas(const IsoFs* iso, uint64_t begin, uint64_t end) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        for (const Extent* e : extents_)
            if (e->iso == iso && e->begin <= begin && end <= e->end) return true;
        return false;
    }

    void KeepAhead(Ahead why, const std::vector<std::string>& keep) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        for (size_t k = 0; k < extents_.size();) {
            Extent* e = extents_[k];
            if (e->why == why && std::find(keep.begin(), keep.end(), e->tag) == keep.end()) {
                Drop(e);   // (removes it from extents_)
                continue;
            }
            ++k;
        }
    }

private:
    Reader()
        : async_(AsyncWanted()), urgent_(KnobOn("TE_DISC_URGENT")), early_(KnobOn("TE_DISC_EARLY")), budget_(BudgetWanted()),
          piece_(PieceWanted()) {}

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

    // Under the lock: the job into the queue at its rank (behind every job of its rank or a lower one; in submission order
    // when TE_DISC_URGENT=0); `count`: the census's put-ahead figures for a client's read
    void Insert(Job* job, bool count) {
        if (!urgent_) {
            queue_.push_back(job);
        } else {
            auto at = std::find_if(queue_.begin(), queue_.end(), [job](const Job* q) { return q->rank_ > job->rank_; });
            const size_t passed = size_t(queue_.end() - at);
            if (count && passed) {
                ++census_.put_ahead;
                census_.passed += passed;
            }
            queue_.insert(at, job);
        }
        census_.queue_max = std::max<uint32_t>(census_.queue_max, uint32_t(queue_.size()));
    }

    // Under the lock: a queued job out of the queue or off the extent it waits on
    void Unqueue(Job* job) {
        if (Extent* e = job->waits_on_) {
            e->waiters.erase(std::remove(e->waiters.begin(), e->waiters.end(), job), e->waiters.end());
            job->waits_on_ = nullptr;
            return;
        }
        queue_.erase(std::remove(queue_.begin(), queue_.end(), job), queue_.end());
    }

    // The job on the calling thread, whole (the reader off, no disc, or the wait's fallback)
    void RunInline(Job* job) {
        job->next_ = 0;
        const bool cache = job->read_ahead;
        job->read_ahead = false;   // no span: the bytes go straight to the job
        std::vector<uint8_t> scratch;
        while (!Step(job, scratch)) {}
        job->read_ahead = cache;
    }

    // Under the lock: the extent the job's range lies in (nullptr: none)
    Extent* ExtentOf(const Job* job) {
        for (Extent* e : extents_)
            if (e->iso == job->iso_ && !e->dropped && e->begin <= job->offset && job->offset + job->size <= e->end) return e;
        return nullptr;
    }

    // Under the lock: the job's bytes from the extent when they are in (true); the extent used (an entry's windows the reads
    // have passed let go of, the next window queued when the reads near this one's end)
    bool Serve(Extent* e, Job* job) {
        if (job->offset + job->size > e->begin + e->filled) return false;
        const uint8_t* src = e->bytes.get() + (job->offset - e->begin);
        job->bytes.assign(src, src + job->size);
        job->got = job->size;
        Used(e, job);
        if (e->consumed && e->waiters.empty() && !e->running) Drop(e);
        return true;
    }

    // Under the lock: the extent used; an archive entry's extent the reads have come to the end of is let go of (`job`
    // served: now; waiting: once its piece has served it)
    void Used(Extent* e, const Job* job) {
        e->used = ++clock_;
        e->hit = true;
        if (e->why == Ahead::Entry && job->offset + job->size + kConsumedSlack >= e->end) e->consumed = true;
    }

    // Under the lock: the job waits on the extent when its bytes are among the next pieces (its piece then put first)
    bool Attach(Extent* e, Job* job) {
        if (e->ended || job->offset >= e->begin + e->filled + 2 * piece_) return false;
        job->state_ = Job::State::Queued;
        job->waits_on_ = e;
        e->waiters.push_back(job);
        Used(e, job);
        if (e->piece && !e->running && e->piece->rank_ != kRankWaited) {
            queue_.erase(std::remove(queue_.begin(), queue_.end(), e->piece), queue_.end());
            e->piece->rank_ = kRankWaited;
            Insert(e->piece, false);
        }
        ++census_.ahead_waited;
        wake_.notify_one();
        return true;
    }

    // Under the lock: the extent's next piece queued (its rank: a client's read waits on it, else its kind's)
    void QueuePiece(Extent* e) {
        if (e->piece || e->ended || e->filled >= e->Size()) return;
        Job* p = new Job;
        p->iso_ = e->iso;
        p->offset = e->begin + e->filled;
        // (the linked chunks' pieces at most 512 KB: a client's read that comes while one is read waits less for the card)
        const size_t piece = e->why == Ahead::Links ? std::min<size_t>(piece_, kLinksPieceMax) : piece_;
        const size_t want = e->filled == 0 && e->first ? e->first : piece;
        p->size = std::min<size_t>(want, e->Size() - size_t(e->filled));
        p->site = "read-ahead";
        p->prefetch_ = true;
        p->fill_ = e;
        p->state_ = Job::State::Queued;
        p->rank_ = !e->waiters.empty() ? kRankWaited : e->why == Ahead::Links ? kRankLinks : kRankAhead;
        e->piece = p;
        Insert(p, false);
    }

    // Under the lock: room for n more bytes of extents of `why` -- the archive entries' windows nobody waits on let go of,
    // the least recently used first; the linked chunks may not take the last kEntryReserve
    bool MakeRoom(size_t n, Ahead why, const Extent* keep = nullptr) {
        const size_t cap = why == Ahead::Links ? (budget_ > kEntryReserve ? budget_ - kEntryReserve : 0) : budget_;
        auto links_held = [this] {
            size_t held = 0;
            for (const Extent* e : extents_) if (e->why == Ahead::Links) held += e->Size();
            return held;
        };
        if (why == Ahead::Links && links_held() + n > cap) return false;
        while (ahead_held_ + n > budget_) {
            Extent* victim = nullptr;
            for (Extent* e : extents_)
                if (e != keep && e->why == Ahead::Entry && e->waiters.empty() && !e->running && (!victim || e->used < victim->used)) victim = e;
            if (!victim) return false;
            Drop(victim);
        }
        return true;
    }

    // Under the lock: the extent let go of -- out of the list, its queued piece with it, its waiters to the queue as plain
    // reads; deleted now, or by the reader after the piece it is reading
    void Drop(Extent* e) {
        extents_.erase(std::remove(extents_.begin(), extents_.end(), e), extents_.end());
        ahead_held_ -= e->Size();
        ++census_.ahead_dropped;
        if (!e->hit) ++census_.ahead_dropped_unread;
        for (Job* w : e->waiters) {
            w->waits_on_ = nullptr;
            w->rank_ = kRankWaited;
            Insert(w, false);
        }
        e->waiters.clear();
        if (!queue_.empty()) wake_.notify_one();
        if (e->running) {
            e->dropped = true;
            return;
        }
        if (e->piece) {
            queue_.erase(std::remove(queue_.begin(), queue_.end(), e->piece), queue_.end());
            delete e->piece;
        }
        delete e;
    }

    // Off the lock: the extent's piece read into it; returns holding the lock (the waiters done, the next piece queued)
    void RunPiece(Job* p, std::unique_lock<sync::Mutex>& lock) {
        Extent* e = p->fill_;
        const uint64_t at = p->offset;
        const size_t want = p->size;
        e->running = true;
        uint8_t* dst = e->bytes.get() + (at - e->begin);
        lock.unlock();
        const double t0 = NowMs();
        const size_t got = ReadCard(e->iso, at, want, dst);
        const double ms = NowMs() - t0;
        lock.lock();
        ++census_.preads;
        census_.pread_bytes += got;
        census_.pread_ms += ms;
        census_.ahead_bytes += got;
        e->running = false;
        e->piece = nullptr;
        delete p;
        if (e->dropped) {
            delete e;   // (its waiters went to the queue at the drop)
            return;
        }
        e->filled += got;
        if (got < want) e->ended = true;
        // the waiters whose bytes are in
        bool any = false;
        for (size_t k = 0; k < e->waiters.size();) {
            Job* w = e->waiters[k];
            if (w->offset + w->size <= e->begin + e->filled) {
                const uint8_t* src = e->bytes.get() + (w->offset - e->begin);
                w->bytes.assign(src, src + w->size);
                w->got = w->size;
                w->waits_on_ = nullptr;
                w->state_ = Job::State::Done;
                e->waiters.erase(e->waiters.begin() + std::ptrdiff_t(k));
                if (e->why == Ahead::Entry && w->offset + w->size + kConsumedSlack >= e->end) e->consumed = true;
                if (w->abandoned_) delete w;
                any = true;
                continue;
            }
            if (e->ended) {
                // the card ended short of it: a plain read
                w->waits_on_ = nullptr;
                w->rank_ = kRankWaited;
                e->waiters.erase(e->waiters.begin() + std::ptrdiff_t(k));
                Insert(w, false);
                continue;
            }
            ++k;
        }
        if (any) done_.notify_all();
        if (e->consumed && e->waiters.empty()) {
            Drop(e);
            return;
        }
        QueuePiece(e);
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
            if (q->prefetch_ && !q->fill_ && q->iso_ == iso && q->offset == at) return;
        Job* p = new Job;
        p->iso_ = iso;
        p->offset = at;
        p->size = kReadAhead;
        p->site = "read-ahead";
        p->prefetch_ = true;
        p->abandoned_ = true;   // nobody waits for it: the reader keeps its bytes as a span and deletes it
        p->state_ = Job::State::Queued;
        p->rank_ = kRankAhead;
        Insert(p, false);
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
    // the job is complete. On the reader thread the card's reads go through ReadCard (the slow card's knob)
    bool Step(Job* job, std::vector<uint8_t>& scratch) {
        const IsoFs* iso = job->iso_;
        if (!iso) { job->got = 0; job->bytes.clear(); return true; }
        const bool reader = std::this_thread::get_id() == reader_id_;
        auto read = [&](uint64_t at, size_t n, uint8_t* dst) { return reader ? ReadCard(iso, at, n, dst) : iso->ReadBytes(at, n, dst); };
        const double t0 = NowMs();
        size_t got = 0, want = 0;
        bool finished = false;
        if (job->prefetch_ || (job->next_ == 0 && job->read_ahead && !job->Progressive() && job->size <= kReadAheadMax)) {
            // one read of the job and the bytes after it: the job's part copied out, the whole kept as a span
            want = job->prefetch_ ? job->size : job->size + kReadAhead;
            std::vector<uint8_t> span(want);
            got = read(job->offset, want, span.data());
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
            got = want ? read(job->offset + job->next_, want, dst) : 0;
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
        const size_t got = iso ? ReadCard(iso, at, size, span.data()) : 0;
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
            ahead = ReadCard(iso, at + size, kReadAhead, span.data() + size);
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

    // The card priced once at start-up (TE_DISC_BENCH): reads of 64 KB, 256 KB and 1 MB at places across the image (so the
    // card's own cache does not answer them); off the lock, before the first job
    static void Bench(const IsoFs* iso) {
        const uint64_t image = iso->ImageBytes();
        if (image < (64u << 20)) return;
        struct Size { size_t bytes; int reads; double ms = 0; size_t got = 0; } sizes[] = {{64u << 10, 6}, {256u << 10, 3}, {1u << 20, 2}};
        std::vector<uint8_t> buffer(1u << 20);
        const double t0 = NowMs();
        int place = 0;
        for (Size& s : sizes) {
            for (int k = 0; k < s.reads; ++k, ++place) {
                // the 11 reads at 11 places 1/12 of the image apart, sector-aligned
                const uint64_t at = (image / 12 * uint64_t(place + 1)) & ~uint64_t(2047);
                const double r0 = NowMs();
                s.got += ReadCard(iso, at, s.bytes, buffer.data());
                s.ms += NowMs() - r0;
            }
        }
        const double total = NowMs() - t0;
        auto rate = [](const Size& s) { return s.ms > 0 ? double(s.got) / 1048576.0 / (s.ms / 1000.0) : 0.0; };
        std::fprintf(stderr, "[te.io] card: 64 KB reads %.1f MB/s (%.1f ms a read), 256 KB reads %.1f MB/s (%.1f ms), 1 MB reads %.1f MB/s "
                     "(%.1f ms); %.0f ms once at start-up (TE_DISC_BENCH=0 skips it)\n",
                     rate(sizes[0]), sizes[0].ms / sizes[0].reads, rate(sizes[1]), sizes[1].ms / sizes[1].reads, rate(sizes[2]),
                     sizes[2].ms / sizes[2].reads, total);
    }

    void Run() {
#if defined(__vita__)
        sceKernelChangeThreadPriority(0, Priority());
#endif
        te::prof::BindThread(6);   // TE_PROF's "io" slot; the CPU census's te-io
        std::unique_lock<sync::Mutex> lock(mutex_);
        reader_id_ = std::this_thread::get_id();
        bool benched = !BenchWanted();
        for (;;) {
            busy_ = false;
            done_.notify_all();   // (Idle)
            {
                TE_PROF_SCOPE("io idle");
                while (queue_.empty() || (links_paused_ && queue_.front()->rank_ == kRankLinks)) wake_.wait(lock);
            }
            busy_ = true;
            if (!benched) {
                benched = true;
                const IsoFs* iso = queue_.front()->iso_;
                lock.unlock();
                if (iso) Bench(iso);
                lock.lock();
                continue;
            }
            Job* job = queue_.front();
            queue_.pop_front();
            if (job->fill_) {
                job->state_ = Job::State::Running;
                TE_PROF_SCOPE("io read (the card)");
                RunPiece(job, lock);
                continue;
            }
            const bool plain = !job->prefetch_ && job->next_ == 0 && !job->Progressive();
            if (plain) {
                // its bytes may have come into an extent or a span since it was queued (a read-ahead before it)
                if (Extent* e = ExtentOf(job)) {
                    if (Serve(e, job)) {
                        ++census_.ahead_hits;
                        Finish(job, lock);
                        continue;
                    }
                    if (Attach(e, job)) continue;
                }
            }
            const bool plain_small = plain && job->read_ahead && job->size <= kReadAheadMax;
            if (plain_small && FromCache(job)) {
                ++census_.dequeue_hits;
                Finish(job, lock);
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
                job->rank_ = kRankLater;
                Insert(job, false);
                continue;
            }
            Finish(job, lock);
        }
    }

    // Under the lock: the job done (deleted when nobody wants it), its waiter woken
    void Finish(Job* job, std::unique_lock<sync::Mutex>& lock) {
        job->state_ = Job::State::Done;
        if (job->abandoned_) {
            lock.unlock();
            delete job;
            lock.lock();
        }
        done_.notify_all();
    }

    sync::Mutex mutex_{"te-io"};
    sync::Condition wake_{mutex_, "te-io-wake"};
    sync::Condition done_{mutex_, "te-io-done"};
    std::deque<Job*> queue_;
    Span spans_[kSpans];
    std::vector<Extent*> extents_;
    size_t ahead_held_ = 0;
    uint64_t clock_ = 0;
    std::vector<uint8_t> scratch_;   // the reader thread's own (progressive jobs' pieces)
    std::thread::id reader_id_;
    Census census_;
    int logged_waits_ = 0;
    bool async_ = false;
    bool started_ = false;
    bool urgent_ = true, early_ = true;
    bool links_paused_ = false;
    bool busy_ = false;   // the reader thread has a job (or the bench) in hand
    size_t budget_ = 0, piece_ = 1u << 20;
};

void Submit(Job* job, const IsoFs* iso) { if (job) Reader::Get().Submit(job, iso); }
bool Done(const Job* job) { return !job || Reader::Get().Done(job); }
void Wait(Job* job, const char* why) { if (job) Reader::Get().Wait(job, why); }
bool WaitFor(Job* job, double ms, const char* why) { return !job || Reader::Get().Wait(job, why, std::max(0.0, ms)); }
void Release(Job* job) { Reader::Get().Release(job); }
bool Async() { return Reader::Get().Async(); }
void Start() { Reader::Get().Start(); }
Census Count() { return Reader::Get().Count(); }
bool ReadAhead(const IsoFs* iso, uint64_t begin, uint64_t end, Ahead why, const std::string& tag, size_t first) {
    return Reader::Get().ReadAhead(iso, begin, end, why, tag, first);
}
bool AheadHas(const IsoFs* iso, uint64_t begin, uint64_t end) { return Reader::Get().AheadHas(iso, begin, end); }
void KeepAhead(Ahead why, const std::vector<std::string>& keep) { Reader::Get().KeepAhead(why, keep); }
size_t AheadBudget() { return Reader::Get().Budget(); }
void PauseLinks(bool paused) { Reader::Get().PauseLinks(paused); }
void Idle() { if (Async()) Reader::Get().Idle(); }

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
    const double mb = 1048576.0;
    std::fprintf(stderr, "[te.io] presents %llu-%llu: the game waited for the card %llu times, %.1f ms (stream landings %llu, %.1f ms; "
                 "music windows %llu, %.1f ms; other %llu, %.1f ms; and behind the loading picture, within the frames' time, %llu polls "
                 "%.1f ms), the movie thread %llu times %.1f ms; read ahead: %llu linked chunks "
                 "and %llu archive-entry windows made (%llu refused by the budget), %.1f MB read in them, %llu reads served from them "
                 "(%llu of those waited on a piece), %llu let go of (%llu unread); held now %.1f MB (the linked chunks %.1f MB) of the "
                 "%.0f MB budget, at most %.1f MB\n",
                 (unsigned long long)from, (unsigned long long)to,
                 (unsigned long long)(now.waits_stream + now.waits_music + now.waits_other - last.waits_stream - last.waits_music - last.waits_other),
                 now.wait_ms_stream + now.wait_ms_music + now.wait_ms_other - last.wait_ms_stream - last.wait_ms_music - last.wait_ms_other,
                 (unsigned long long)(now.waits_stream - last.waits_stream), now.wait_ms_stream - last.wait_ms_stream,
                 (unsigned long long)(now.waits_music - last.waits_music), now.wait_ms_music - last.wait_ms_music,
                 (unsigned long long)(now.waits_other - last.waits_other), now.wait_ms_other - last.wait_ms_other,
                 (unsigned long long)(now.waits_load - last.waits_load), now.wait_ms_load - last.wait_ms_load,
                 (unsigned long long)(now.waits_movie - last.waits_movie), now.wait_ms_movie - last.wait_ms_movie,
                 (unsigned long long)(now.ahead_links - last.ahead_links), (unsigned long long)(now.ahead_entries - last.ahead_entries),
                 (unsigned long long)(now.ahead_refused - last.ahead_refused), double(now.ahead_bytes - last.ahead_bytes) / mb,
                 (unsigned long long)(now.ahead_hits + now.ahead_waited - last.ahead_hits - last.ahead_waited),
                 (unsigned long long)(now.ahead_waited - last.ahead_waited), (unsigned long long)(now.ahead_dropped - last.ahead_dropped),
                 (unsigned long long)(now.ahead_dropped_unread - last.ahead_dropped_unread), double(now.ahead_held) / mb,
                 double(now.ahead_links_held) / mb, double(AheadBudget()) / mb, double(now.ahead_held_max) / mb);
    last = now;
}

}  // namespace te::io
