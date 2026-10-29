// te_native.cpp -- the game's code compiled natively for the PlayStation Vita (CMake TE_VITA_NATIVE): the runner's side of
// the module that is no wasm module any more (see te_native.h for the address model).
#include "te_native.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "te_env.h"

extern "C" {
// tools/gen_retail_image.py --arm: the game's 32 MB retail address space (the retail image + the game's heap)
extern unsigned char te_guest_block[];
// tools/gen_retail_table.py over the native objects: {retail code address, the native function}
struct te_retail_function { uint32_t address; void (*function)(void); };
extern const te_retail_function te_retail_functions[];
extern const uint32_t te_retail_function_count;
// tools/native/gen_retail_data_ptrs.py from tools/native/retail_data_ptrs.tsv: the retail data image's words that hold
// DATA addresses (the reviewed list), in address order
extern const uint32_t te_retail_data_pointer_words[];
extern const uint32_t te_retail_data_pointer_word_count;
}

namespace te::native {

namespace {
uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void Wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
// The retail image's sections (tools/gen_retail_image.py SECTIONS): the words of .data .rodata .sdata hold the pointers
constexpr uint32_t kPointerWordsStart = 0x2E6F00, kPointerWordsEnd = 0x30A460;
constexpr uint32_t kImageStart = 0x2D9D90, kImageEnd = 0x3DB200;
constexpr uint32_t kCodeStart = 0x00100000, kCodeEnd = 0x002D9D90;
}  // namespace

uint8_t* GuestBlock() { return te_guest_block; }

void BindGuestBlock(w2c_env& env) {
    env.memory.data = te_guest_block;
    env.memory.size = kGuestBlockSize;
    std::memset(te_guest_block, 0, kGuestBlockSize);   // .bss: zero already; a second bind (none today) must clear it too
}

DataRelocationReport RelocateRetailDataPointers(w2c_env& env) {
    DataRelocationReport r;
    uint8_t* mem = env.memory.data;
    const uint32_t bias = uint32_t(reinterpret_cast<uintptr_t>(mem));
    for (uint32_t i = 0; i < te_retail_data_pointer_word_count; ++i) {
        const uint32_t a = te_retail_data_pointer_words[i];
        if (a < kPointerWordsStart || a + 4 > kPointerWordsEnd) { ++r.out_of_range; continue; }
        const uint32_t v = Rd32(mem + a);
        // a listed word whose value is no address inside the image: another executable's data (a modded or another
        // release's program) -- left as it is, counted (the start-up line says so)
        if (v < kImageStart || v > kImageEnd) { ++r.not_an_address; if (r.examples.size() < 8) r.examples.push_back(a); continue; }
        Wr32(mem + a, v + bias);
        ++r.relocated;
    }
    // what the list does not name but a scan would call a data address (diagnosis: a different executable's data shows here)
    for (uint32_t a = kPointerWordsStart; a + 4 <= kPointerWordsEnd; a += 4) {
        const uint32_t v = Rd32(mem + a);
        if (v >= kImageStart && v <= kImageEnd) ++r.unlisted_candidates;
    }
    return r;
}

PatchReport PatchRetailCodePointers(w2c_env& env, const std::vector<uint32_t>& known_function_starts) {
    PatchReport r;
    uint8_t* mem = env.memory.data;
    std::unordered_map<uint32_t, uint32_t> by_address;
    by_address.reserve(te_retail_function_count);
    for (uint32_t i = 0; i < te_retail_function_count; ++i)
        by_address.emplace(te_retail_functions[i].address, uint32_t(reinterpret_cast<uintptr_t>(te_retail_functions[i].function)));
    std::vector<uint32_t> starts = known_function_starts;
    std::sort(starts.begin(), starts.end());
    for (uint32_t a = kPointerWordsStart; a + 4 <= kPointerWordsEnd; a += 4) {
        ++r.words_scanned;
        const uint32_t v = Rd32(mem + a);
        if (v < kCodeStart || v >= kCodeEnd) continue;
        auto it = by_address.find(v);
        if (it != by_address.end()) {
            Wr32(mem + a, it->second);   // the native function's address (Thumb bit set): a blx through the word calls it
            ++r.patched;
        } else if (std::binary_search(starts.begin(), starts.end(), v)) {
            ++r.unported_hits;
            if (r.unported.size() < 4096) r.unported.emplace_back(a, v);
        } else {
            ++r.stray_code_range;
        }
    }
    return r;
}

uint32_t RetailFunctionCount() { return te_retail_function_count; }

}  // namespace te::native

// ---- the decomp's calls through retail function pointers (abi.h CallEabi) --------------------------------------------
// The R5900's EABI took integer arguments in a0..a7 and float arguments in f12..f19, each file in its own order; AAPCS-VFP
// (the Vita's hard-float ABI) does the same with r0..r3 + the stack and s0..s15. abi.h packs the arguments that way (up to
// 8 integers, 8 floats; no 64-bit integer argument occurs in a retail slot's signature), so the call is one native call
// with all of them: a callee reads the prefix it declares. The wasm build dispatches by the callee's module type instead
// (tools/gen_eabi_dispatch.py); natively the registers ARE the dispatch.
namespace {
typedef uint64_t (*EabiIntegerFunction)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float,
                                        float, float, float, float, float, float, float);
typedef float (*EabiFloatFunction)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float, float,
                                   float, float, float, float, float, float);
[[noreturn]] void UnportedCall(const void* function, const void* caller) {
    std::fprintf(stderr, "[twinsanityevolved] fatal: a call through a retail pointer to 0x%08X, a retail function with no body in the "
                         "native build (called from %p)\n", unsigned(reinterpret_cast<uintptr_t>(function)), caller);
    std::abort();
}
inline bool RetailCode(const void* function) {
    const uintptr_t f = reinterpret_cast<uintptr_t>(function);
    return f >= 0x00100000u && f < 0x002D9D90u;
}
}  // namespace

extern "C" uint64_t CallEabiTrampoline(const void* function, const uint64_t* i, const float* f) {
    if (__builtin_expect(RetailCode(function) || !function, 0)) UnportedCall(function, __builtin_return_address(0));
    return reinterpret_cast<EabiIntegerFunction>(const_cast<void*>(function))(
        uint32_t(i[0]), uint32_t(i[1]), uint32_t(i[2]), uint32_t(i[3]), uint32_t(i[4]), uint32_t(i[5]), uint32_t(i[6]), uint32_t(i[7]),
        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

extern "C" float CallEabiTrampolineF(const void* function, const uint64_t* i, const float* f) {
    if (__builtin_expect(RetailCode(function) || !function, 0)) UnportedCall(function, __builtin_return_address(0));
    return reinterpret_cast<EabiFloatFunction>(const_cast<void*>(function))(
        uint32_t(i[0]), uint32_t(i[1]), uint32_t(i[2]), uint32_t(i[3]), uint32_t(i[4]), uint32_t(i[5]), uint32_t(i[6]), uint32_t(i[7]),
        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

// A callback the game handed a host body (Movie::BeginPresenting's `present`): natively the function's own address.
extern "C" int te_call_void_callback(uint32_t function) {
    if (!function || RetailCode(reinterpret_cast<const void*>(uintptr_t(function)))) {
        std::fprintf(stderr, "[twinsanityevolved] callback: 0x%08X is no native function: not called\n", function);
        return 0;
    }
    reinterpret_cast<void (*)()>(uintptr_t(function))();
    return 1;
}
