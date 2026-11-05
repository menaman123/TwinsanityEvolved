// platform/nativeee/host -- test_requests.cpp: see te_test_requests.h. Platform::System::NextTestRequest answered from the queue a
// test fills; empty unless a test asks, so the game's frames of play are its own.
#include "te_test_requests.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "ps2e_seam.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_services.h"

namespace te::test_requests {

namespace {
// Platform::System::TestRequestKind and TestRequest (game/include/platform/system.h): eight 32-bit words in the game's memory
constexpr unsigned kTeleport = 1, kMessage = 2, kCounter = 3, kAgentCounter = 4, kHitPoints = 5;
constexpr unsigned kCounters = 1000;     // ChunkManager::Counters
constexpr unsigned kAgentCounters = 4;   // Agent::Counters
constexpr unsigned kPlaying = 12;      // GameController::StatePlaying

struct Asked {
    Request request;
    uint64_t boundary;
};
std::deque<Asked> g_queue;
Finder g_finder = nullptr;

bool IsPlayer(const std::string& name) { return name.empty() || name == "player"; }
}  // namespace

bool Parse(const std::string& spec, Request* out, std::string* error) {
    std::istringstream in(spec);
    std::string op;
    Request r;
    auto fail = [&](const char* what) { if (error) *error = std::string(what) + ": '" + spec + "'"; return false; };
    if (!(in >> op)) return fail("an empty test request");
    if (op == "teleport") {
        r.kind = kTeleport;
        if (!(in >> r.x >> r.y >> r.z)) return fail("teleport X Y Z [NAME]");
        in >> r.instance;
        if (r.instance == "player") r.instance.clear();
    } else if (op == "message") {
        r.kind = kMessage;
        long id = -1;
        if (!(in >> r.instance >> id) || id < 0 || id > 0xFFFF) return fail("message NAME ID [FROM] (ID 0..65535)");
        r.value = int32_t(id);
        in >> r.other;
        if (r.instance == "player") r.instance.clear();
        if (r.other == "player") return fail("message FROM is an object's instance, not the player");
    } else if (op == "counter") {
        r.kind = kCounter;
        long v = 0;
        if (!(in >> r.counter >> v) || r.counter >= kCounters) return fail("counter N V [NAME] (N 0..999)");
        r.value = int32_t(v);
        if (in >> r.instance) {
            r.kind = kAgentCounter;
            if (r.instance == "player" || r.counter >= kAgentCounters || v < 0 || v > 255)
                return fail("counter N V NAME: an object's instance, N 0..3, V 0..255");
        }
    } else if (op == "hitpoints") {
        r.kind = kHitPoints;
        long v = -1;
        if (!(in >> v) || v < 0 || v > 255) return fail("hitpoints N [NAME] (N 0..255)");
        r.value = int32_t(v);
        in >> r.instance;
        if (r.instance == "player") r.instance.clear();
    } else {
        return fail("unknown test request (teleport, message, counter, hitpoints)");
    }
    std::string extra;
    if (in >> extra) return fail("more words than the request takes");
    *out = r;
    return true;
}

std::string Spec(const Request& r) {
    char text[256];
    if (r.kind == kTeleport)
        std::snprintf(text, sizeof text, "teleport %.4f %.4f %.4f%s%s", r.x, r.y, r.z, r.instance.empty() ? "" : " ", r.instance.c_str());
    else if (r.kind == kMessage)
        std::snprintf(text, sizeof text, "message %s %d%s%s", IsPlayer(r.instance) ? "player" : r.instance.c_str(), r.value,
                      r.other.empty() ? "" : " ", r.other.c_str());
    else if (r.kind == kHitPoints)
        std::snprintf(text, sizeof text, "hitpoints %d%s%s", r.value, r.instance.empty() ? "" : " ", r.instance.c_str());
    else if (r.kind == kAgentCounter)
        std::snprintf(text, sizeof text, "counter %u %d %s", r.counter, r.value, r.instance.c_str());
    else
        std::snprintf(text, sizeof text, "counter %u %d", r.counter, r.value);
    return text;
}

void Ask(const Request& request, uint64_t boundary) { g_queue.push_back({request, boundary}); }

void SetFinder(Finder finder) { g_finder = finder; }

bool InstallFromSpec(const char* spec, std::string* error) {
    // SPEC@BOUNDARY;SPEC@BOUNDARY...: each queued at the first boundary of play from its boundary on, in the list's order
    auto timed = std::make_shared<std::vector<std::pair<uint64_t, Request>>>();
    std::string all = spec;
    size_t from = 0;
    while (from <= all.size()) {
        size_t end = all.find(';', from);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(from, end - from);
        from = end + 1;
        if (item.find_first_not_of(" \t") == std::string::npos) continue;
        const size_t at = item.rfind('@');
        if (at == std::string::npos) { if (error) *error = "TE_TEST_ACTIONS: '" + item + "' has no @BOUNDARY"; return false; }
        Request r;
        if (!Parse(item.substr(0, at), &r, error)) return false;
        timed->push_back({std::strtoull(item.c_str() + at + 1, nullptr, 10), r});
    }
    std::fprintf(stderr, "[te.test] on: %zu test requests (TE_TEST_ACTIONS)\n", timed->size());
    auto next = std::make_shared<size_t>(0);
    Services::Get().clock().AddBoundaryHook("test requests", [timed, next](uint64_t boundary) {
        unsigned state = 0, last = 0, upcoming = 0;
        if (*next >= timed->size() || !harness::ReadGameState(&state, &last, &upcoming) || state != kPlaying) return;
        while (*next < timed->size() && (*timed)[*next].first <= boundary) {
            Ask((*timed)[*next].second, boundary);
            ++*next;
        }
    });
    return true;
}

}  // namespace te::test_requests

// The next request a test asked (game/include/platform/system.h TestRequest written at `request`): 1, or 0 when none. A named
// instance is looked up now, at the frame that carries it out
extern "C" ps2e_s32 ps2e_system_next_test_request(ps2e_u32 request) {
    using namespace te::test_requests;
    while (!g_queue.empty()) {
        const Asked asked = g_queue.front();
        g_queue.pop_front();
        const Request& r = asked.request;
        std::fprintf(stderr, "[te.test] b %llu: %s\n", (unsigned long long)asked.boundary, Spec(r).c_str());
        uint32_t instance = 0, other = 0;
        if (!IsPlayer(r.instance)) {
            instance = g_finder ? g_finder(r.instance) : 0;
            if (!instance) { std::fprintf(stderr, "[te.test]   no loaded instance of %s: not done\n", r.instance.c_str()); continue; }
        }
        if (!r.other.empty()) {
            other = g_finder ? g_finder(r.other) : 0;
            if (!other) { std::fprintf(stderr, "[te.test]   no loaded instance of %s: not done\n", r.other.c_str()); continue; }
        }
        uint32_t words[8] = {r.kind, instance, other, r.counter, uint32_t(r.value), 0, 0, 0};
        std::memcpy(&words[5], &r.x, 4);
        std::memcpy(&words[6], &r.y, 4);
        std::memcpy(&words[7], &r.z, 4);
        uint8_t* out = te::GuestPointer(request, sizeof words);
        if (out == nullptr) return 0;
        std::memcpy(out, words, sizeof words);
        if (other)
            std::fprintf(stderr, "[te.test]   instance 0x%08X from 0x%08X\n", instance, other);
        else if (instance)
            std::fprintf(stderr, "[te.test]   instance 0x%08X\n", instance);
        return 1;
    }
    return 0;
}
