// render/backends/gl/gl_uniform_cache.h -- GlUniformCache: per-location value shadow
// for the PS2 program's scalar uniforms.
//
// The draw path sets ~30 scalar uniforms per draw and consecutive draws share
// almost all of them; each setter records the last value sent per location and
// skips the glUniform* call when unchanged. Uniform values are per-program state,
// so the shadow is cleared on every relink. A negative location is a silent no-op,
// as glUniform* defines it. The shadow is a small open-addressed table hashed by a
// multiply (a location can be a large number -- vitaGL's are addresses -- and a
// hashed container's modulo is a software divide on the Vita's CPU).
//
// THE VITA (deferred, the default there; TE_GFX_UNIFORM_SYNC=0 sends at once as elsewhere): vitaGL's glUniform*
// writes the program that OWNS the location, bound or not, and every call marks the bound program's whole default
// uniform buffers for upload at the next draw. The setters only record the value under the full PS2 program's location
// (the logical uniform: the locations the draw path uses); a program receives the values it does not hold yet right
// before a draw with it bound (Sync): program 0 = the full PS2 program at those locations, program 1 = the mirrored one
// (SetMirror: the fast PS2 program) at its paired locations. So each value is sent once, to the program that draws with
// it, instead of to both programs at every change.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace ps2r {
namespace render {

class GlUniformCache {
public:
    GlUniformCache();
    void Clear();
    void Set1i(int loc, int v);
    void Set1f(int loc, float v);
    void Set2f(int loc, float a, float b);
    void Set2i(int loc, int a, int b);
    void Set3f(int loc, float a, float b, float c);
    void Set4f(int loc, float a, float b, float c, float d);
    // a vec4 array of `count` (at most kArrayFloats / 4) from `v`
    void Set4fv(int loc, int count, const float* v);
    uint64_t Skips() const { return skips_; }
    // the values that changed (each a glUniform* where the cache sends at once); equal counts bracket a stretch in which
    // no uniform value moved
    uint64_t Mutations() const { return mutations_; }
    // the glUniform* calls actually made (the mirror's included; the Vita's deferred ones when Sync sends them)
    uint64_t Sent() const { return sent_; }
    // bumped by Clear (a relink: every location's value is the program's default again) -- a caller that remembers what
    // it set compares this to know its memory still holds
    uint32_t Generation() const { return generation_; }
    // A second program whose uniforms mirror this one's (the Vita's fast PS2 program): every value set at location L of
    // the first is the value of its pair's location too (pairs: first-program location, second-program location; empty:
    // none). Sent at once to both, or (deferred) by Sync(1).
    void SetMirror(const std::vector<std::pair<int, int>>& pairs);
    // the last value Set1i recorded for `loc` is `v` (false when unknown: never set, cleared, or the cache bypassed)
    bool Holds1i(int loc, int v) const;
    // A watched location (e.g. the uniforms a program variant's choice reads): WatchedMutations counts the changes of
    // the watched values only. Watches are dropped by Clear (re-arm when Generation changed).
    void Watch(int loc);
    uint64_t WatchedMutations() const { return watched_mutations_; }
    // Deferred mode (the Vita): the values program `program` (0 full, 1 mirrored) does not hold yet, sent now -- call it
    // with that program bound, right before its draw. A no-op when the cache sends at once.
    void Sync(int program);
    bool Deferred() const { return deferred_; }
    // TE_GFX_UNIFORM_CHECK=1: the syncs checked and the uniforms found not to hold their recorded value
    uint64_t CheckedSyncs() const { return check_draws_; }
    uint64_t CheckMismatches() const { return check_mismatches_; }
    // The Vita: deferred sends on / off at run time (off: every program first receives what it lacks; then every change
    // goes to both at once, as before). Elsewhere a no-op.
    void SetDeferred(bool on);

    // What the cache holds for a location: the value last set (kind 0: nothing since the last Clear)
    struct Shadow {
        uint8_t kind = 0;   // 1=1i 2=1f 3=2f 4=3f 5=2i 6=4f
        int i[2] = {0, 0};
        float f[4] = {0.f, 0.f, 0.f, 0.f};
    };
    static constexpr int kArrayFloats = 32;
    struct ArrayShadow {
        int loc = -1;
        int count = 0;   // vec4s held (0: never set)
        float v[kArrayFloats] = {};
        uint8_t dirty = 0;   // deferred: bit p = program p does not hold the value yet
    };
    // A stable view of what the cache holds for `loc` (claimed now when new: kind 0 until a Set*), valid until the next
    // Clear (Generation() moves); nullptr when `loc` cannot be tracked (negative, the table full). A
    // program the cache does not sync (the Vita's lean PS2 programs, GlShaderLibrary) reads the full program's values here
    // and sends what changed to its own locations at the draw that uses it.
    const Shadow* Track(int loc);
    // the same for a vec4-array location (Set4fv); nullptr when no array slot is left
    const ArrayShadow* TrackArray(int loc);

private:
    struct Entry {
        int loc = -1;   // -1: free
        Shadow shadow;
        uint8_t dirty = 0;      // deferred: bit p = program p does not hold the value yet
        bool watched = false;
    };
    // TE_GFX_UNIFORM_CHECK=1 (a diagnosis): what each program was last sent per entry, checked against the recorded value
    // at every Sync -- a program drawing with a value it was never sent is counted and logged
    struct SentRecord {
        Shadow s[2];
        bool valid[2] = {false, false};
    };
    static constexpr int kBits = 9;
    static constexpr int kSize = 1 << kBits;
    static constexpr int kPrograms = 2;
    // the location's entry index (claimed when new); -1 when the table is full (the value is then sent at once)
    int SlotIndex(int loc);
    Shadow& Slot(int loc);
    // a changed value of entry `idx`: sent at once, or marked for every program (deferred)
    void Changed(int idx, int loc);
    void Send(const Shadow& s, int loc);
    Entry table_[kSize];
    SentRecord* sent_shadow_ = nullptr;   // [kSize] when checking
    struct ArraySent { float v[2][32]; int count[2] = {0, 0}; };
    ArraySent array_sent_[4];
    bool check_ = false;
    uint64_t check_draws_ = 0, check_mismatches_ = 0;
    void NoteSent(int idx, int program);
    void NoteArraySent(int a, int program);
    void Check(int program);
    int used_ = 0;
    Shadow overflow_;
    ArrayShadow arrays_[4];
    std::vector<uint16_t> pending_[kPrograms];   // deferred: the entries whose dirty bit p was set since program p's Sync
    uint64_t skips_ = 0;
    uint64_t mutations_ = 0;
    uint64_t sent_ = 0;
    uint64_t watched_mutations_ = 0;
    uint32_t generation_ = 1;
    // the mirror: a small open-addressed table location -> the second program's location (kMirrorSize slots)
    static constexpr int kMirrorBits = 7;
    static constexpr int kMirrorSize = 1 << kMirrorBits;
    struct MirrorEntry { int loc = -1; int to = -1; };
    MirrorEntry mirror_[kMirrorSize];
    bool mirror_on_ = false;
    int MirrorOf(int loc) const;
    bool deferred_ = false;   // the Vita's deferred sends (TE_GFX_UNIFORM_SYNC=0: off)
};

}  // namespace render
}  // namespace ps2r
