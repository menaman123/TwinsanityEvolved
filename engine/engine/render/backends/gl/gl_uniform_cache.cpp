// render/backends/gl/gl_uniform_cache.cpp -- GlUniformCache implementation.
#include "engine/render/backends/gl/gl_uniform_cache.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ps2r {
namespace render {

GlUniformCache::GlUniformCache() {
#if PS2R_HOST_VITA
    const char* e = std::getenv("TE_GFX_UNIFORM_SYNC");
    deferred_ = !(e && e[0] == '0');
#endif
    const char* c = std::getenv("TE_GFX_UNIFORM_CHECK");
    check_ = c && c[0] == '1';
    if (check_) sent_shadow_ = new SentRecord[kSize];
}

// (the check's bookkeeping: entry idx's recorded value went to `program`)
void GlUniformCache::NoteSent(int idx, int program) {
    if (!check_ || idx < 0 || program < 0 || program > 1) return;
    sent_shadow_[idx].s[program] = table_[idx].shadow;
    sent_shadow_[idx].valid[program] = true;
}

void GlUniformCache::NoteArraySent(int a, int program) {
    if (!check_ || program < 0 || program > 1) return;
    std::memcpy(array_sent_[a].v[program], arrays_[a].v, sizeof arrays_[a].v);
    array_sent_[a].count[program] = arrays_[a].count;
}

// Every value recorded for a location `program` has must be the one it was last sent
void GlUniformCache::Check(int program) {
    if (!check_) return;
    ++check_draws_;
    for (int i = 0; i < kSize; ++i) {
        const Entry& e = table_[i];
        if (e.loc < 0 || e.shadow.kind == 0) continue;
        if (program == 1 && MirrorOf(e.loc) < 0) continue;
        const SentRecord& sn = sent_shadow_[i];
        const bool ok = sn.valid[program] && sn.s[program].kind == e.shadow.kind &&
                        std::memcmp(sn.s[program].i, e.shadow.i, sizeof e.shadow.i) == 0 &&
                        std::memcmp(sn.s[program].f, e.shadow.f, sizeof e.shadow.f) == 0;
        if (!ok) {
            if (++check_mismatches_ <= 12)
                std::fprintf(stderr, "[gl] uniform check: program %d draws with location %d (kind %d) not holding its value "
                             "(sent %s)\n", program, e.loc, int(e.shadow.kind), sn.valid[program] ? "another" : "never");
        }
    }
    for (int a = 0; a < 4; ++a) {
        const ArrayShadow& x = arrays_[a];
        if (x.loc < 0) continue;
        if (program == 1 && MirrorOf(x.loc) < 0) continue;
        const ArraySent& sn = array_sent_[a];
        if (sn.count[program] != x.count || std::memcmp(sn.v[program], x.v, size_t(x.count) * 4 * sizeof(float)) != 0) {
            if (++check_mismatches_ <= 12)
                std::fprintf(stderr, "[gl] uniform check: program %d draws with array %d not holding its value\n", program, x.loc);
        }
    }
}

void GlUniformCache::SetMirror(const std::vector<std::pair<int, int>>& pairs) {
    for (MirrorEntry& e : mirror_) e = MirrorEntry{};
    const bool was_on = mirror_on_;
    mirror_on_ = false;
    int used = 0;
    for (const auto& p : pairs) {
        if (p.first < 0 || p.second < 0 || used >= kMirrorSize * 3 / 4) continue;
        uint32_t at = (static_cast<uint32_t>(p.first) * 0x9E3779B1u) >> (32 - kMirrorBits);
        while (mirror_[at].loc >= 0 && mirror_[at].loc != p.first) at = (at + 1) & (kMirrorSize - 1);
        if (mirror_[at].loc < 0) used++;
        mirror_[at].loc = p.first;
        mirror_[at].to = p.second;
        mirror_on_ = true;
    }
    if (deferred_ && mirror_on_ && !was_on) {
        // the second program holds none of the values recorded so far
        pending_[1].clear();
        for (int i = 0; i < kSize; ++i) {
            if (table_[i].loc < 0) continue;
            table_[i].dirty |= 2;
            pending_[1].push_back(uint16_t(i));
        }
        for (ArrayShadow& a : arrays_)
            if (a.loc >= 0) a.dirty |= 2;
    }
}

int GlUniformCache::MirrorOf(int loc) const {
    if (!mirror_on_ || loc < 0) return -1;
    uint32_t at = (static_cast<uint32_t>(loc) * 0x9E3779B1u) >> (32 - kMirrorBits);
    for (int probes = 0; probes < kMirrorSize; ++probes) {
        const MirrorEntry& e = mirror_[at];
        if (e.loc == loc) return e.to;
        if (e.loc < 0) return -1;
        at = (at + 1) & (kMirrorSize - 1);
    }
    return -1;
}

bool GlUniformCache::Holds1i(int loc, int v) const {
    if (loc < 0) return false;
    uint32_t at = (static_cast<uint32_t>(loc) * 0x9E3779B1u) >> (32 - kBits);
    for (int probes = 0; probes < kSize; ++probes) {
        const Entry& e = table_[at];
        if (e.loc == loc) return e.shadow.kind == 1 && e.shadow.i[0] == v;
        if (e.loc < 0) return false;
        at = (at + 1) & (kSize - 1);
    }
    return false;
}

const GlUniformCache::Shadow* GlUniformCache::Track(int loc) {
    if (loc < 0) return nullptr;
    const int idx = SlotIndex(loc);
    return idx < 0 ? nullptr : &table_[idx].shadow;
}

const GlUniformCache::ArrayShadow* GlUniformCache::TrackArray(int loc) {
    if (loc < 0) return nullptr;
    ArrayShadow* slot = nullptr;
    for (ArrayShadow& a : arrays_) {
        if (a.loc == loc) return &a;
        if (a.loc < 0 && !slot) slot = &a;
    }
    if (!slot) return nullptr;
    slot->loc = loc;
    slot->count = 0;
    return slot;
}

void GlUniformCache::Clear() {
    generation_++;
    for (Entry& e : table_) e = Entry{};
    used_ = 0;
    for (ArrayShadow& a : arrays_) a = ArrayShadow{};
    for (auto& p : pending_) p.clear();
    if (sent_shadow_) for (int i = 0; i < kSize; ++i) sent_shadow_[i] = SentRecord{};
    for (ArraySent& a : array_sent_) a = ArraySent{};
}

void GlUniformCache::Watch(int loc) {
    if (loc < 0) return;
    const int idx = SlotIndex(loc);
    if (idx >= 0) table_[idx].watched = true;
}

void GlUniformCache::Send(const Shadow& s, int loc) {
    if (loc < 0) return;
    ++sent_;
    switch (s.kind) {
        case 1: glUniform1i(loc, s.i[0]); break;
        case 2: glUniform1f(loc, s.f[0]); break;
        case 3: glUniform2f(loc, s.f[0], s.f[1]); break;
        case 4: glUniform3f(loc, s.f[0], s.f[1], s.f[2]); break;
        case 5: glUniform2i(loc, s.i[0], s.i[1]); break;
        case 6: glUniform4f(loc, s.f[0], s.f[1], s.f[2], s.f[3]); break;
        default: --sent_; break;
    }
}

void GlUniformCache::Changed(int idx, int loc) {
    Entry& e = table_[idx];
    if (e.watched) ++watched_mutations_;
    if (!deferred_) {
        Send(e.shadow, loc);
        NoteSent(idx, 0);
        if (mirror_on_) {
            Send(e.shadow, MirrorOf(loc));
            NoteSent(idx, 1);
        }
        return;
    }
    for (int p = 0; p < kPrograms; ++p) {
        if (p == 1 && !mirror_on_) continue;
        const uint8_t bit = uint8_t(1u << p);
        if (e.dirty & bit) continue;
        e.dirty |= bit;
        pending_[p].push_back(uint16_t(idx));
    }
}

void GlUniformCache::Sync(int program) {
    if (program < 0 || program >= kPrograms) return;
    if (program == 1 && !mirror_on_) return;
    if (!deferred_) {
        Check(program);
        return;
    }
    const uint8_t bit = uint8_t(1u << program);
    for (uint16_t idx : pending_[program]) {
        Entry& e = table_[idx];
        if (!(e.dirty & bit)) continue;
        e.dirty &= uint8_t(~bit);
        Send(e.shadow, program == 0 ? e.loc : MirrorOf(e.loc));
        NoteSent(int(idx), program);
    }
    pending_[program].clear();
    for (int k = 0; k < 4; ++k) {
        ArrayShadow& a = arrays_[k];
        if (a.loc < 0 || !(a.dirty & bit)) continue;
        a.dirty &= uint8_t(~bit);
        const int to = program == 0 ? a.loc : MirrorOf(a.loc);
        if (to < 0) continue;
        ++sent_;
        glUniform4fv(to, a.count, a.v);
        NoteArraySent(k, program);
    }
    Check(program);
}

void GlUniformCache::SetDeferred(bool on) {
#if PS2R_HOST_VITA
    if (deferred_ && !on) {
        Sync(0);
        Sync(1);
    }
    deferred_ = on;
#else
    (void)on;
#endif
}

void GlUniformCache::Set4fv(int loc, int count, const float* v) {
    if (loc < 0 || count <= 0) return;
    const size_t bytes = size_t(count) * 4 * sizeof(float);
    ArrayShadow* slot = nullptr;
    for (ArrayShadow& a : arrays_) {
        if (a.loc == loc) { slot = &a; break; }
        if (a.loc < 0 && !slot) slot = &a;
    }
    if (slot && slot->loc == loc && slot->count == count && std::memcmp(slot->v, v, bytes) == 0) { skips_++; return; }
    const bool kept = slot && count * 4 <= kArrayFloats;
    if (kept) {
        slot->loc = loc;
        slot->count = count;
        std::memcpy(slot->v, v, bytes);
    }
    ++mutations_;
    if (deferred_ && kept) {
        slot->dirty = mirror_on_ ? 3 : 1;
        return;
    }
    ++sent_;
    glUniform4fv(loc, count, v);
    if (kept) NoteArraySent(int(slot - arrays_), 0);
    if (mirror_on_) {
        const int to = MirrorOf(loc);
        if (to >= 0) ++sent_;
        glUniform4fv(to, count, v);
        if (kept) NoteArraySent(int(slot - arrays_), 1);
    }
    if (kept) slot->dirty = 0;
}

int GlUniformCache::SlotIndex(int loc) {
    uint32_t at = (static_cast<uint32_t>(loc) * 0x9E3779B1u) >> (32 - kBits);
    for (;;) {
        Entry& e = table_[at];
        if (e.loc == loc) return int(at);
        if (e.loc < 0) {
            if (used_ >= kSize * 3 / 4) return -1;
            e = Entry{};
            e.loc = loc;
            used_++;
            return int(at);
        }
        at = (at + 1) & (kSize - 1);
    }
}

GlUniformCache::Shadow& GlUniformCache::Slot(int loc) {
    const int idx = SlotIndex(loc);
    if (idx < 0) { overflow_ = Shadow{}; return overflow_; }
    return table_[idx].shadow;
}

// The setters: the value recorded; unchanged -> nothing; changed -> Changed (sent at once, or marked for the programs).
// A location the table cannot hold (full, or the cache bypassed) is sent at once to both programs, as before.
#define TE_UNIFORM_SET(KIND, SAME, STORE, CALL)                                   \
    if (loc < 0) return;                                                          \
    const int idx = SlotIndex(loc);                                               \
    if (idx < 0) {                                                                \
        ++mutations_;                                                             \
        ++sent_;                                                                  \
        CALL(loc);                                                                \
        if (mirror_on_) { const int to = MirrorOf(loc); if (to >= 0) ++sent_; CALL(to); } \
        return;                                                                   \
    }                                                                             \
    Shadow& s = table_[idx].shadow;                                               \
    if (s.kind == KIND && (SAME)) { skips_++; return; }                           \
    s.kind = KIND;                                                                \
    STORE;                                                                        \
    ++mutations_;                                                                 \
    Changed(idx, loc);

void GlUniformCache::Set1i(int loc, int v) {
#define TE_CALL(L) glUniform1i(L, v)
    TE_UNIFORM_SET(1, s.i[0] == v, s.i[0] = v, TE_CALL)
#undef TE_CALL
}

void GlUniformCache::Set1f(int loc, float v) {
#define TE_CALL(L) glUniform1f(L, v)
    TE_UNIFORM_SET(2, s.f[0] == v, s.f[0] = v, TE_CALL)
#undef TE_CALL
}

void GlUniformCache::Set2f(int loc, float a, float b) {
#define TE_CALL(L) glUniform2f(L, a, b)
    TE_UNIFORM_SET(3, s.f[0] == a && s.f[1] == b, (s.f[0] = a, s.f[1] = b), TE_CALL)
#undef TE_CALL
}

void GlUniformCache::Set2i(int loc, int a, int b) {
#define TE_CALL(L) glUniform2i(L, a, b)
    TE_UNIFORM_SET(5, s.i[0] == a && s.i[1] == b, (s.i[0] = a, s.i[1] = b), TE_CALL)
#undef TE_CALL
}

void GlUniformCache::Set3f(int loc, float a, float b, float c) {
#define TE_CALL(L) glUniform3f(L, a, b, c)
    TE_UNIFORM_SET(4, s.f[0] == a && s.f[1] == b && s.f[2] == c, (s.f[0] = a, s.f[1] = b, s.f[2] = c), TE_CALL)
#undef TE_CALL
}

void GlUniformCache::Set4f(int loc, float a, float b, float c, float d) {
#define TE_CALL(L) glUniform4f(L, a, b, c, d)
    TE_UNIFORM_SET(6, s.f[0] == a && s.f[1] == b && s.f[2] == c && s.f[3] == d,
                   (s.f[0] = a, s.f[1] = b, s.f[2] = c, s.f[3] = d), TE_CALL)
#undef TE_CALL
}
#undef TE_UNIFORM_SET

}  // namespace render
}  // namespace ps2r
