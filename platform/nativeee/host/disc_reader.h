// disc_reader.h -- the disc image read OFF the game thread: one reader thread ("te-io") that the stream service, the music
// windows and the movie player hand their reads to (the Vita's memory card: a 128 KB read is milliseconds of the game
// thread blocked in sceIoPread, a music window of 2-6 MB a few hundred).
//
// WHAT THE GAME SEES DOES NOT CHANGE: a client submits a read when it learns what it will need and takes the bytes at the
// point it used to read them (the stream service: at the drive model's landing -- poll / wait / boundary, computed from the
// boundary count as before; the music windows: as the voice reaches each block; the movie: before the slice is fed). Only
// WHEN the bytes come off the card changes. A Wait for bytes not yet read blocks (bounded: a job still queued after
// kWaitFallbackMs is read on the waiting thread) and is counted by the client's site ("[te.io] waited ..." + the window
// census); in play the stream service does not Wait at a landing whose bytes are not in (stream_service.h): it asks Done.
//
// Read-ahead, three kinds, all of them bytes of the read-only image (never stale):
//   * EXTENTS (ReadAhead): a range read ahead whole, in kPiece preads (1 MB: the card's cost is per read as much as per byte),
//     at the back of the queue -- the chunks the current chunk links to (te_link_ahead.h: a minute before the game wants
//     them) and the rest of an archive entry the game has started reading (the stream service: its 128 KB blocks every few
//     frames become hits). A read inside an extent is done at Submit when its bytes are in; when they are about to be (the
//     extent's next pieces) it waits on the extent -- its piece put first -- instead of reading them a second time.
//     Bounded by a budget (TE_DISC_AHEAD_MB; the Vita's 16 MB of its ~105 MB of free heap); an extent that does not fit is
//     not made (the old reads then).
//   * SPANS: a small read (<= kReadAheadMax) outside the extents is read with the kReadAhead bytes after it, kept in a span
//     cache (a few spans, LRU); a later read inside a cached span completes at Submit with no thread hop, and a hit near a
//     span's end queues the next span.
// Big reads go in kChunk pieces, the queue round-robin between them (a 6 MB music window never holds a small read up).
// The queue's order: the reads a client waits for (and the pieces such reads wait on), the big reads' later pieces, the
// archive entries' pieces and the spans' read-aheads, the linked chunks' pieces last.
//
// On by default on the Vita (TE_DISC_ASYNC=0 reads on the calling thread as before); elsewhere off (TE_DISC_ASYNC=1 on).
// TE_DISC_SLOW_MS=MS[,PER_READ] makes the reader thread a slower card (MS ms for every 128 KB it reads, PER_READ ms more for
// every read: the device's card in Vita3K or on a computer); TE_DISC_BENCH=1 prices the card at start-up (the Vita's default).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IsoFs;

namespace te::io {

class Reader;
struct Extent;

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
    uint8_t rank_ = 0;            // its place in the queue (Reader::Rank)
    size_t next_ = 0;             // bytes read so far (the chunks)
    const IsoFs* iso_ = nullptr;  // the disc it reads (Submit's)
    Extent* fill_ = nullptr;      // a piece of this extent (the reader's own job)
    Extent* waits_on_ = nullptr;  // queued on this extent's pieces (not in the queue)
};

// Queue the job (or read it here when the reader is off / the bytes are in the span cache or an extent). `iso` = the disc.
void Submit(Job* job, const IsoFs* iso);
bool Done(const Job* job);
// Block until the job is done. `why`: the wait log's name for the moment ("stream landing", "music block", ...).
void Wait(Job* job, const char* why);
// Block until the job is done or `ms` have passed (counted as a wait); true when it is done
bool WaitFor(Job* job, double ms, const char* why);
// The submitter lets go of the job (deleted now when the reader does not have it, else by the reader when done).
void Release(Job* job);
// Whether reads go to the reader thread (decided once: TE_DISC_ASYNC, the platform's default).
bool Async();
// The reader thread and its locks, made once; the stream service / movie player call it on first use, the disc at its
// open (before the graphics device exists: te_sync.h).
void Start();

// ----- extents ----------------------------------------------------------------------------------------------------------
// Entry: the rest of an archive entry a read has started (let go of once the reads come to its end); Links: a chunk the
// current chunk links to (te_link_ahead.h: let go of by KeepAhead once the game has it or no link reaches it)
enum class Ahead : uint8_t { Entry = 1, Links = 2 };
// Read [begin, end) of the image ahead, whole (`first`: the first piece's size, 0 = the reader's piece -- an entry's first
// piece is the read that asked for it). False when the reader is off, the range is held already, or it does not fit the
// budget (the linked chunks may not take its last 6 MB: the archive entries' share).
bool ReadAhead(const IsoFs* iso, uint64_t begin, uint64_t end, Ahead why, const std::string& tag, size_t first = 0);
// Whether [begin, end) lies in an extent (read or being read)
bool AheadHas(const IsoFs* iso, uint64_t begin, uint64_t end);
// Let go of the extents of `why` whose tag is not in `keep` (their waiters go to the queue as plain reads)
void KeepAhead(Ahead why, const std::vector<std::string>& keep);
// The extents' budget in bytes (0: none are made: the reader off, or TE_DISC_AHEAD_MB=0)
size_t AheadBudget();
// While paused the linked chunks' pieces wait (a load's reads have the card to themselves)
void PauseLinks(bool paused);
// Block until the reader has nothing queued or being read, its extents and spans let go of (tests: before the disc goes)
void Idle();

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
    // the waits by client: the stream service's landings (stream reads and sound banks), the music windows, the movie
    // thread's slices, the rest (save states); and the bounded ones (WaitFor: a load's polls, within the frame's time)
    uint64_t waits_stream = 0, waits_music = 0, waits_movie = 0, waits_other = 0, waits_load = 0;
    double wait_ms_stream = 0, wait_ms_music = 0, wait_ms_movie = 0, wait_ms_other = 0, wait_ms_load = 0;
    // the extents: made by kind, refused by the budget, their bytes read, the reads served from them (at Submit, when the
    // reader reached them, by a piece they waited on), let go of, held now (bytes, peak)
    uint64_t ahead_links = 0, ahead_entries = 0, ahead_refused = 0, ahead_bytes = 0, ahead_hits = 0, ahead_waited = 0,
             ahead_dropped = 0, ahead_dropped_unread = 0;
    uint64_t ahead_held = 0, ahead_held_max = 0, ahead_links_held = 0;
};
Census Count();
void LogWindow(uint64_t from, uint64_t to);

}  // namespace te::io
