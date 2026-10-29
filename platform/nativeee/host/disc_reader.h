// disc_reader.h -- the disc image read OFF the game thread: one reader thread ("te-io") that the stream service, the music
// windows and the movie player hand their reads to (the Vita's memory card: a 128 KB read is milliseconds of the game
// thread blocked in sceIoPread, a music window of 2-6 MB a few hundred).
//
// WHAT THE GAME SEES DOES NOT CHANGE: a client submits a read when it learns what it will need and Waits for the bytes at
// the point it used to read them (the stream service: at the drive model's landing -- poll / wait / boundary, computed from
// the boundary count as before; the music windows: as the voice reaches each block; the movie: before the slice is fed).
// Only WHEN the bytes come off the card changes. A wait for bytes not yet read blocks (bounded: a job still queued after
// kWaitFallbackMs is read on the waiting thread) and is counted and logged ("[te.io] wait ..." + the window census).
//
// Read-ahead: a small read (<= kReadAheadMax) is read with the kReadAhead bytes after it, kept in a span cache (a few
// spans, LRU); a later read inside a cached span completes at Submit with no thread hop (the drive-paced resource reads
// walk the archive in order: 6-16 KB pieces several a frame, 128 KB blocks every 3-4 frames), and a hit near a span's end
// queues the next span. The image is read-only: cached bytes are never stale.
// Big reads go in kChunk pieces, the queue round-robin between chunks (a 6 MB music window never holds a small read up).
//
// On by default on the Vita (TE_DISC_ASYNC=0 reads on the calling thread as before); elsewhere off (TE_DISC_ASYNC=1 on).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class IsoFs;

namespace te::io {

class Reader;

// One read of the disc image. Owned by the submitter; the reader thread has it between Submit and Done. A job released
// while the reader still has it is deleted by the reader when it is done with it (Release).
class Job {
public:
    Job() = default;
    virtual ~Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    uint64_t offset = 0;          // absolute byte offset in the image (an extent's byte + the position in the file)
    size_t size = 0;              // bytes wanted
    std::vector<uint8_t> bytes;   // a plain job's result: what IsoFs::ReadBytes returned (got bytes)
    size_t got = 0;               // bytes read (== size unless the image ended or the card failed)
    const char* site = "read";    // the census's / the wait log's name for the client
    bool read_ahead = true;       // a plain job may be read with the bytes after it (the span cache)
    size_t first_piece = 0;       // a chunked job's first piece (0: the reader's chunk) -- small, so its first bytes come soon

protected:
    // A progressive job's consumer (the music windows): the reader hands each chunk over as it lands, in order, on the
    // reader thread (pos = bytes from the job's offset); `bytes` stays empty. OnDone after the last one.
    virtual bool Progressive() const { return false; }
    virtual void OnChunk(size_t pos, const uint8_t* data, size_t n) { (void)pos; (void)data; (void)n; }
    virtual void OnDone() {}

private:
    friend class Reader;
    enum class State : uint8_t { Idle, Queued, Running, Done };
    State state_ = State::Idle;   // under the reader's lock
    bool abandoned_ = false;      // released while queued / running: the reader deletes it
    bool prefetch_ = false;       // the reader's own read-ahead (its bytes go to the span cache only)
    size_t next_ = 0;             // bytes read so far (the chunks)
    const IsoFs* iso_ = nullptr;  // the disc it reads (Submit's)
};

// Queue the job (or read it here when the reader is off / the bytes are in the span cache). `iso` = the disc to read.
void Submit(Job* job, const IsoFs* iso);
bool Done(const Job* job);
// Block until the job is done. `why`: the wait log's name for the moment ("stream landing", "music block", ...).
void Wait(Job* job, const char* why);
// The submitter lets go of the job (deleted now when the reader does not have it, else by the reader when done).
void Release(Job* job);
// Whether reads go to the reader thread (decided once: TE_DISC_ASYNC, the platform's default).
bool Async();
// The reader thread and its locks, made once; the stream service / movie player call it on first use, the disc at its
// open (before the graphics device exists: te_sync.h).
void Start();

struct JobReleaser {
    void operator()(Job* job) const { Release(job); }
};
template <typename T>
using Handle = std::unique_ptr<T, JobReleaser>;

// The census (counters since start) and one log line per pace window (te_prof.cpp CpuCensus on the Vita).
struct Census {
    uint64_t submits = 0, cache_hits = 0, inline_reads = 0, preads = 0, pread_bytes = 0, prefetches = 0;
    uint64_t waits = 0, waits_over_2ms = 0, fallbacks = 0;
    double wait_ms = 0, wait_max_ms = 0, pread_ms = 0;
    uint32_t queue_max = 0;
    // TE_DISC_URGENT: reads put ahead of queued read-aheads / later pieces, and how many they passed; TE_DISC_EARLY: reads
    // done before their read-ahead was read, reads found in a span when the reader reached them
    uint64_t put_ahead = 0, passed = 0, early_done = 0, dequeue_hits = 0;
};
Census Count();
void LogWindow(uint64_t from, uint64_t to);

}  // namespace te::io
