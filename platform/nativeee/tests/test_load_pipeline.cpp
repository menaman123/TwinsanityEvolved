// test_load_pipeline.cpp -- the load pipeline's scheduling pieces: what they hand over must be what the
// synchronous paths hand over, whatever the order the threads run in.
//   - the disc reader (disc_reader.h) with the reader thread on (TE_DISC_ASYNC=1) and its urgent-first order and early
//     completion (TE_DISC_URGENT / TE_DISC_EARLY, on by default): every read's bytes are the image's, for small reads with
//     their read-ahead spans, sequential walks served from the spans, big reads in pieces, reads submitted while others are
//     in flight, and reads released before they are done; and the extents (ranges read ahead whole): reads served from them,
//     waiting on their pieces, after they are let go of, the budget's shares;
//   - the builder pool's background queue (graphics_internal.h PostBackground): every job posted runs once, staged posts
//     included once the frame's hand-over (FlushBuilds) hands them over;
//   - PreparedMesh's state machine: the work runs exactly once whichever of TryMake / Ensure gets there first, a cancelled
//     one never runs, Settle waits for one being made.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "disc_reader.h"
#include "engine/cdvd/iso_fs.h"
#include "graphics_internal.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {

std::vector<uint8_t> g_image;   // the big file's bytes
uint64_t g_file_at = 0;         // its byte offset in the image

void TestReaderBytes(const IsoFs& iso) {
    // a sequential walk of small reads (the drive-paced resource reads: read-ahead spans), some of them released before
    // they are waited for, interleaved with big reads (pieces) and random small reads
    std::vector<te::io::Job*> jobs;
    struct Want { te::io::Job* job; uint64_t at; size_t size; };
    std::vector<Want> wants;
    uint32_t rng = 12345;
    auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng; };
    uint64_t walk = 0;
    for (int round = 0; round < 160; ++round) {
        Want w{new te::io::Job, 0, 0};
        const uint32_t kind = next() % 8;
        if (kind < 5) {   // the walk
            w.size = 2048 + next() % (48 * 1024);
            w.at = walk;
            walk += w.size;
        } else if (kind < 7) {   // random small
            w.size = 1 + next() % (160 * 1024);
            w.at = next() % (g_image.size() - w.size);
        } else {   // big
            w.size = 300 * 1024 + next() % (700 * 1024);
            w.at = next() % (g_image.size() - w.size);
        }
        if (w.at + w.size > g_image.size()) { delete w.job; continue; }
        w.job->offset = g_file_at + w.at;
        w.job->size = w.size;
        w.job->site = "test";
        te::io::Submit(w.job, &iso);
        wants.push_back(w);
        // every few reads: wait for the oldest (a landing), release one now and then without waiting
        if (wants.size() > 6) {
            Want old = wants.front();
            wants.erase(wants.begin());
            if (next() % 9 == 0) {
                te::io::Release(old.job);
                continue;
            }
            te::io::Wait(old.job, "the test");
            CHECK(te::io::Done(old.job));
            CHECK_EQ(old.job->got, old.size);
            CHECK(old.job->bytes.size() == old.size && std::memcmp(old.job->bytes.data(), g_image.data() + old.at, old.size) == 0);
            te::io::Release(old.job);
        }
    }
    for (Want& w : wants) {
        te::io::Wait(w.job, "the test");
        CHECK_EQ(w.job->got, w.size);
        CHECK(w.job->bytes.size() == w.size && std::memcmp(w.job->bytes.data(), g_image.data() + w.at, w.size) == 0);
        te::io::Release(w.job);
    }
    const te::io::Census c = te::io::Count();
    CHECK(te::io::Async());
    CHECK(c.cache_hits > 0);     // the walk was served from the spans
    CHECK(c.early_done > 0);     // reads done before their read-ahead
    std::fprintf(stderr, "  reader: %llu submits, %llu from the spans, %llu card reads, %llu early, %llu put ahead (passing %llu), "
                 "%llu found in a span when reached\n", (unsigned long long)c.submits, (unsigned long long)c.cache_hits,
                 (unsigned long long)c.preads, (unsigned long long)c.early_done, (unsigned long long)c.put_ahead,
                 (unsigned long long)c.passed, (unsigned long long)c.dequeue_hits);
}

// The extents (disc_reader.h: ranges read ahead whole): the bytes the image's whether a read finds its range in, waits on
// it or comes before it; a range held twice is refused; let go of, its reads are plain reads; the budget's shares
void TestExtents(const IsoFs& iso) {
    const te::io::Census c0 = te::io::Count();
    CHECK_EQ(te::io::AheadBudget(), size_t(8u << 20));   // (TE_DISC_AHEAD_MB=8 in main)
    const uint64_t a = g_file_at + 4096, a_end = a + 1536 * 1024;
    // reads submitted right after the read-ahead: they wait on its pieces (or come first); then from memory
    CHECK(te::io::ReadAhead(&iso, a, a_end, te::io::Ahead::Links, "chunk a"));
    CHECK(!te::io::ReadAhead(&iso, a + 1024, a + 2048, te::io::Ahead::Links, "chunk a"));   // held already
    CHECK(te::io::AheadHas(&iso, a + 100, a + 200));
    auto check = [](te::io::Job* job, uint64_t at, size_t size) {
        te::io::Wait(job, "the test");
        CHECK_EQ(job->got, size);
        CHECK(job->bytes.size() == size && std::memcmp(job->bytes.data(), g_image.data() + (at - g_file_at), size) == 0);
        te::io::Release(job);
    };
    std::vector<std::pair<te::io::Job*, uint64_t>> jobs;
    for (uint64_t at = a; at + 131072 <= a_end; at += 150000) {
        auto* job = new te::io::Job;
        job->offset = at;
        job->size = 131072;
        job->site = "stream";
        te::io::Submit(job, &iso);
        jobs.push_back({job, at});
    }
    for (auto& [job, at] : jobs) check(job, at, 131072);
    // all in memory now: a read inside is done in Submit
    for (int k = 0; k < 200 && te::io::Count().ahead_held < 1536 * 1024; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    {
        auto* job = new te::io::Job;
        job->offset = a + 777777;
        job->size = 5000;
        job->site = "stream";
        const uint64_t hits = te::io::Count().ahead_hits;
        te::io::Submit(job, &iso);
        CHECK(te::io::Done(job));
        CHECK_EQ(te::io::Count().ahead_hits, hits + 1);
        check(job, a + 777777, 5000);
    }
    // the budget's shares: the linked chunks up to 8 - 6 MB, the archive entries the rest
    CHECK(!te::io::ReadAhead(&iso, g_file_at + (2u << 20), g_file_at + (2u << 20) + (1u << 19) + 1, te::io::Ahead::Links, "chunk b"));
    CHECK(te::io::ReadAhead(&iso, g_file_at + (2u << 20), g_file_at + (2u << 20) + (900u << 10), te::io::Ahead::Entry, "entry", 4096));
    // let go of: the reads inside are plain reads, their bytes the image's
    te::io::KeepAhead(te::io::Ahead::Links, {});
    CHECK(!te::io::AheadHas(&iso, a + 100, a + 200));
    {
        auto* job = new te::io::Job;
        job->offset = a + 999;
        job->size = 70000;
        job->site = "stream";
        te::io::Submit(job, &iso);
        check(job, a + 999, 70000);
    }
    // the archive entry's extent read through: let go of once a read ends near its end
    const uint64_t e = g_file_at + (2u << 20);
    for (uint64_t at = e; at + 65536 <= e + (900u << 10); at += 65536) {
        auto* job = new te::io::Job;
        job->offset = at;
        job->size = 65536;
        job->site = "stream";
        te::io::Submit(job, &iso);
        check(job, at, 65536);
    }
    CHECK(!te::io::AheadHas(&iso, e, e + 1));
    const te::io::Census c = te::io::Count();
    CHECK_EQ(c.ahead_links - c0.ahead_links, 1u);
    CHECK_EQ(c.ahead_entries - c0.ahead_entries, 1u);
    CHECK_EQ(c.ahead_refused - c0.ahead_refused, 1u);
    CHECK(c.ahead_hits > c0.ahead_hits);
    CHECK_EQ(c.ahead_held, 0u);
    std::fprintf(stderr, "  extents: %llu reads served (%llu waited on a piece), %.1f MB read in them, %llu let go of\n",
                 (unsigned long long)(c.ahead_hits + c.ahead_waited - c0.ahead_hits - c0.ahead_waited),
                 (unsigned long long)(c.ahead_waited - c0.ahead_waited), double(c.ahead_bytes - c0.ahead_bytes) / 1048576.0,
                 (unsigned long long)(c.ahead_dropped - c0.ahead_dropped));
}

void TestBackgroundQueue() {
    std::atomic<int> ran{0};
    std::atomic<int> twice{0};
    constexpr int kJobs = 200;
    std::vector<std::atomic<int>> each(kJobs);
    for (auto& e : each) e.store(0);
    for (int i = 0; i < kJobs; ++i)
        te::gfx::PostBackground([&ran, &each, &twice, i] {
            if (each[size_t(i)].fetch_add(1) != 0) twice.fetch_add(1);
            ran.fetch_add(1);
        });
    te::gfx::DrainBuilds();   // (the frame's builds: none; a drain does not wait for background jobs)
    // the staged ones handed over at a frame's hand-over: here the pool's Flush through FlushBuilds' path
    te::gfx::PostBackground([] {});   // (one more, so the staging batch is not exactly full)
    for (int k = 0; k < 400 && ran.load() < kJobs; ++k) {
        te::gfx::PostBackground([] {});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_EQ(ran.load(), kJobs);
    CHECK_EQ(twice.load(), 0);
}

struct CountingPrep : te::gfx::PreparedMesh {
    std::atomic<int> computed{0};
    int sleep_ms = 0;
};
void Compute(te::gfx::PreparedMesh& base) {
    auto& p = static_cast<CountingPrep&>(base);
    if (p.sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(p.sleep_ms));
    p.computed.fetch_add(1);
}

void TestPreparedMesh() {
    {
        CountingPrep p;
        p.compute = Compute;
        CHECK(p.TryMake());
        CHECK(!p.TryMake());
        CHECK(p.Ensure());
        CHECK_EQ(p.computed.load(), 1);
    }
    {
        CountingPrep p;
        p.compute = Compute;
        p.Settle();   // queued: cancelled
        CHECK(!p.TryMake());
        CHECK(!p.Ensure());
        CHECK_EQ(p.computed.load(), 0);
    }
    for (int round = 0; round < 50; ++round) {
        CountingPrep p;
        p.compute = Compute;
        p.sleep_ms = round % 3;
        std::atomic<int> made{0};
        std::thread a([&] { if (p.TryMake()) made.fetch_add(1); });
        std::thread b([&] { if (p.Ensure()) made.fetch_add(0); });
        a.join();
        b.join();
        p.Settle();   // made: nothing to cancel, returns at once
        CHECK_EQ(p.computed.load(), 1);
        CHECK_EQ(int(p.state.load()), int(te::gfx::PreparedMesh::kMade));
    }
}

}  // namespace

int main() {
    setenv("TE_DISC_ASYNC", "1", 1);   // the reader thread (the Vita's default) before its first use
    setenv("TE_DISC_AHEAD_MB", "8", 1);   // the extents' budget (TestExtents)
    setenv("TE_DISC_SLOW_MS", "1,1", 1);  // a card slower than a computer's disk: the reads overlap the submits
    g_image = te_test::Pattern(3 * 1024 * 1024 + 777, 0x5EED);
    const std::string path = te_test::WriteTestIso({{"CRASH6", "BIG.BD", g_image}});
    CHECK(!path.empty());
    IsoFs iso;
    CHECK(iso.Open(path));
    IsoFs::Entry e;
    CHECK(iso.Find("CRASH6/BIG.BD", e));
    g_file_at = uint64_t(e.lba) * IsoFs::kSectorSize;
    te::io::Start();
    TestReaderBytes(iso);
    TestExtents(iso);
    TestBackgroundQueue();
    TestPreparedMesh();
    return te_test::Finish("test_load_pipeline");
}
