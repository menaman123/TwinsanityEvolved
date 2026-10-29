// te_env.cpp -- memory binding, retail image loading, code-pointer patching (the runner's side of the shared-memory design).
#include "te_env.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "engine/cdvd/iso_fs.h"
#include "engine/ee/mem/main_memory.h"
#if defined(TE_NATIVE)
#include "te_host.h"     // GuestRanges: the valid ranges of a game pointer (defined here, set in BindGuestBlock)
#include "te_native.h"
#endif

#if !defined(TE_NATIVE)
extern "C" wasm_rt_memory_t* w2c_env_memory(struct w2c_env* env) { return &env->memory; }
extern "C" struct w2c_game* te_env_instance(struct w2c_env* env) { return env->instance; }
extern "C" wasm_rt_memory_t* te_env_memory(struct w2c_env* env) { return &env->memory; }
#endif

namespace te {

#if defined(__vita__)
// te_host.h's GuestMemory() / GuestMemorySize() on the Vita (inline reads of these)
uint8_t* g_guest_memory_base = nullptr;
std::size_t g_guest_memory_size = 0;
#endif

#if defined(TE_NATIVE)
// te_host.h's valid ranges of a game pointer: the eboot's data and code segments (the linker's bounds: C++ data and the
// guest block in .bss; code, read-only data) and the game thread's stack (set on that thread, runner/te_game.cpp)
GuestRanges g_guest_ranges;
}  // namespace te
// the default linker script's bounds (CMakeLists.txt's vita_link.ld): the code segment from its start to the end of the
// exception index (.text, .rodata, .ARM.exidx), the data segment from its first section to the end of .bss
extern "C" char __executable_start[], __exidx_end[], __preinit_array_start[], _end[];
namespace te {
// The native lane (te_native.h): the guest block is the game's retail address space; te_host.h's GuestMemory() names it
// (the diagnostics that hash or dump the 32 MB read it there) while a guest address is the native pointer itself.
void BindGuestBlock(w2c_env& env) {
    te::native::BindGuestBlock(env);
    g_guest_memory_base = env.memory.data;
    g_guest_memory_size = size_t(env.memory.size);
    const uint32_t text = uint32_t(reinterpret_cast<uintptr_t>(__executable_start));
    const uint32_t data = uint32_t(reinterpret_cast<uintptr_t>(__preinit_array_start));
    g_guest_ranges.text_base = text;
    g_guest_ranges.text_span = uint32_t(reinterpret_cast<uintptr_t>(__exidx_end)) - text;
    g_guest_ranges.data_base = data;
    g_guest_ranges.data_span = uint32_t(reinterpret_cast<uintptr_t>(_end)) - data;
}
#else
void BindMainMemory(w2c_env& env, MainMemory& ram) {
    static_assert(MainMemory::kSize == 32u * 1024u * 1024u, "the game module is linked for a 32 MB linear memory");
    env.main_memory = &ram;
    env.memory.data = ram.Data();
    env.memory.data_end = ram.Data() + ram.Size();
    env.memory.page_size = 65536;
    env.memory.pages = MainMemory::kSize / 65536;      // 512
    env.memory.max_pages = env.memory.pages;           // fixed: memory.grow fails, the module never calls it
    env.memory.size = MainMemory::kSize;
#if defined(__vita__)
    g_guest_memory_base = env.memory.data;
    g_guest_memory_size = env.memory.size;
#endif
}
#endif

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
}  // namespace

bool LoadRetailImage(w2c_env& env, const std::vector<uint8_t>& elf, const RetailImage& image, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0) return fail("not an ELF file");
    if (elf[4] != 1 || elf[5] != 1) return fail("not a 32-bit little-endian ELF");
    if (rd16(&elf[18]) != 8) return fail("not a MIPS ELF");
    const uint32_t entry = rd32(&elf[24]);
    if (entry != 0x00100008u) return fail("ELF entry is not 0x00100008 (expected the retail PAL executable)");
    const uint32_t phoff = rd32(&elf[28]);
    const uint16_t phentsize = rd16(&elf[42]), phnum = rd16(&elf[44]);
    if (phoff + size_t(phentsize) * phnum > elf.size()) return fail("program headers out of range");

    uint8_t* mem = env.memory.data;
    bool covered = false;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = &elf[phoff + size_t(i) * phentsize];
        if (rd32(ph) != 1) continue;                                    // PT_LOAD
        const uint32_t p_offset = rd32(ph + 4), p_vaddr = rd32(ph + 8), p_filesz = rd32(ph + 16), p_memsz = rd32(ph + 20);
        const uint32_t seg_lo = p_vaddr, seg_hi_file = p_vaddr + p_filesz, seg_hi_mem = p_vaddr + p_memsz;
        // The part of this segment inside [image.start, image.bss_start) with file bytes
        const uint32_t lo = std::max(seg_lo, image.start);
        const uint32_t hi = std::min(seg_hi_file, image.bss_start);
        if (lo < hi) {
            if (p_offset + (hi - seg_lo) > elf.size()) return fail("PT_LOAD file range out of the ELF");
            std::memcpy(mem + lo, &elf[p_offset + (lo - seg_lo)], hi - lo);
            covered = true;
        }
        // memsz beyond filesz inside the image is bss: already zero
        (void)seg_hi_mem;
    }
    if (!covered) return fail("no PT_LOAD segment covers the retail data image 0x2D9D90..0x30A480");
    // The image's .sbss/.bss must be zero (fresh memory; a second load after a run must clear them)
    std::memset(mem + image.bss_start, 0, image.end - image.bss_start);
    return true;
}

bool ReadElfFromIso(const std::string& iso_path, const std::string& path_in_iso, std::vector<uint8_t>& out, std::string* error) {
    IsoFs iso;
    if (!iso.Open(iso_path, error)) return false;
    IsoFs::Entry e;
    if (!iso.Find(path_in_iso, e)) { if (error) *error = path_in_iso + " not found in " + iso_path; return false; }
    out.resize(e.size);
    const size_t got = iso.ReadBytes(uint64_t(e.lba) * IsoFs::kSectorSize, e.size, out.data());
    if (got != e.size) { if (error) *error = "short read of " + path_in_iso; return false; }
    return true;
}

bool ReadElfFromDisc(const IsoFs& disc, const std::string& path_in_iso, std::vector<uint8_t>& out, std::string* error) {
    IsoFs::Entry e;
    if (!disc.IsOpen() || !disc.Find(path_in_iso, e) || e.directory) { if (error) *error = path_in_iso + " not found on the disc"; return false; }
    out.resize(e.size);
    const size_t got = disc.ReadBytes(uint64_t(e.lba) * IsoFs::kSectorSize, e.size, out.data());
    if (got != e.size) { if (error) *error = "short read of " + path_in_iso; return false; }
    return true;
}

bool ReadRetailWords(const std::vector<uint8_t>& elf, uint32_t vaddr, uint32_t count, std::vector<uint32_t>& out) {
    out.clear();
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0) return false;
    const uint32_t phoff = rd32(&elf[28]);
    const uint16_t phentsize = rd16(&elf[42]), phnum = rd16(&elf[44]);
    if (phoff + size_t(phentsize) * phnum > elf.size()) return false;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = &elf[phoff + size_t(i) * phentsize];
        if (rd32(ph) != 1) continue;   // PT_LOAD
        const uint32_t p_offset = rd32(ph + 4), p_vaddr = rd32(ph + 8), p_filesz = rd32(ph + 16);
        if (vaddr < p_vaddr || uint64_t(vaddr) + uint64_t(count) * 4 > uint64_t(p_vaddr) + p_filesz) continue;
        const size_t at = size_t(p_offset) + (vaddr - p_vaddr);
        if (at + size_t(count) * 4 > elf.size()) return false;
        for (uint32_t k = 0; k < count; ++k) out.push_back(rd32(&elf[at + size_t(k) * 4]));
        return true;
    }
    return false;
}

#if defined(TE_NATIVE)
PatchReport PatchRetailCodePointers(w2c_env& env, const std::vector<uint32_t>& known_function_starts) {
    return te::native::PatchRetailCodePointers(env, known_function_starts);
}
#else
PatchReport PatchRetailCodePointers(w2c_env& env, const std::vector<uint32_t>& known_function_starts) {
    PatchReport r;
    w2c_game* inst = env.instance;
    uint8_t* mem = env.memory.data;
    // The module's exported table: {u32 address, u32 table index} pairs at te_retail_functions
    const uint32_t table_addr = *w2c_game_te_retail_functions(inst);
    const uint32_t count = *(const uint32_t*)(mem + *w2c_game_te_retail_function_count(inst));
    std::unordered_map<uint32_t, uint32_t> by_address;
    by_address.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t addr = rd32(mem + table_addr + i * 8);
        const uint32_t index = rd32(mem + table_addr + i * 8 + 4);
        by_address.emplace(addr, index);
    }
    std::vector<uint32_t> starts = known_function_starts;
    std::sort(starts.begin(), starts.end());
    const RetailImage image;
    // .data .rodata .sdata: 0x2E6F00 .. 0x30A460 (tools/gen_retail_image.py SECTIONS)
    for (uint32_t a = 0x2E6F00; a + 4 <= 0x30A460; a += 4) {
        ++r.words_scanned;
        const uint32_t v = rd32(mem + a);
        if (v < image.code_start || v >= image.code_end) continue;
        auto it = by_address.find(v);
        if (it != by_address.end()) {
            const uint32_t idx = it->second;
            mem[a] = uint8_t(idx); mem[a + 1] = uint8_t(idx >> 8); mem[a + 2] = uint8_t(idx >> 16); mem[a + 3] = uint8_t(idx >> 24);
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
#endif

#if defined(TE_NATIVE)
}  // namespace te
#else
// ---- vtable slot thunks -------------------------------------------------------------------------------------------
// The host table generated after the link (tools/gen_vtable_thunks.py --host-out)
struct te_vt_slot_row { uint32_t vtable; uint16_t slot; uint16_t sig; const char* const* types[4]; uint32_t ntypes; };
extern "C" const struct te_vt_slot_row te_vt_slot_rows[];
extern "C" const uint32_t te_vt_slot_row_count;
extern "C" const uint32_t te_vt_vtable_starts[];
extern "C" const uint32_t te_vt_vtable_start_count;
extern "C" const char* const te_vt_signatures[];
extern "C" const uint32_t te_vt_signature_count_host;
extern "C" const uint32_t te_vt_pure_virtual_address;

struct te_vt_type_name_row { const char* const* id; const char* name; };
extern "C" const struct te_vt_type_name_row te_vt_type_names[];
extern "C" const uint32_t te_vt_type_name_count;

const char* TypeName(const char* id) {
    if (!id) return "(null)";
    for (uint32_t i = 0; i < te_vt_type_name_count; ++i)
        if (*te_vt_type_names[i].id == id || std::memcmp(*te_vt_type_names[i].id, id, 32) == 0) return te_vt_type_names[i].name;
    return "(unknown type)";
}

VtableThunkReport PatchVtableThunks(w2c_env& env, bool verbose) {
    VtableThunkReport r;
    w2c_game* inst = env.instance;
    uint8_t* mem = env.memory.data;
    wasm_rt_funcref_table_t* table = w2c_game_0x5F_indirect_function_table(inst);
    // The module's thunk descriptors: {sig, kind (0 pool / 1 pure), target index, table index}, and the targets array
    const uint32_t desc_addr = *w2c_game_te_vt_thunks(inst);
    const uint32_t desc_count = rd32(mem + *w2c_game_te_vt_thunk_count(inst));
    const uint32_t sig_count = rd32(mem + *w2c_game_te_vt_signature_count(inst));
    const uint32_t targets_addr = *w2c_game_te_vt_targets(inst);
    if (sig_count != te_vt_signature_count_host) {
        std::fprintf(stderr, "[twinsanityevolved] vtable thunks: module has %u signatures, host table %u -- rebuild\n", sig_count, te_vt_signature_count_host);
        return r;
    }
    struct Pool { std::vector<std::pair<uint32_t, uint32_t>> free; uint32_t pure = 0; bool has_pure = false; };   // (target, table index)
    std::vector<Pool> pools(sig_count);
    for (uint32_t i = 0; i < desc_count; ++i) {
        const uint8_t* d = mem + desc_addr + i * 16;
        const uint32_t sig = rd32(d), kind = rd32(d + 4), target = rd32(d + 8), fn = rd32(d + 12);
        if (sig >= sig_count) continue;
        if (kind == 0) pools[sig].free.emplace_back(target, fn);
        else { pools[sig].pure = fn; pools[sig].has_pure = true; }
    }
    for (auto& p : pools) std::reverse(p.free.begin(), p.free.end());   // pop_back hands out instance 0 first
    // The pure-virtual body's table index (patched into the words already): look it up through the words themselves
    const RetailImage image;
    std::vector<uint32_t> starts(te_vt_vtable_starts, te_vt_vtable_starts + te_vt_vtable_start_count);
    std::sort(starts.begin(), starts.end());
    std::unordered_map<uint32_t, bool> done;
    // The retail table maps address -> index; the pure body's index is the entry for its retail address
    uint32_t pure_index = UINT32_MAX;
    {
        const uint32_t table_addr = *w2c_game_te_retail_functions(inst);
        const uint32_t count = rd32(mem + *w2c_game_te_retail_function_count(inst));
        for (uint32_t i = 0; i < count; ++i)
            if (rd32(mem + table_addr + i * 8) == te_vt_pure_virtual_address) { pure_index = rd32(mem + table_addr + i * 8 + 4); break; }
    }
    for (uint32_t i = 0; i < te_vt_slot_row_count; ++i) {
        const te_vt_slot_row& row = te_vt_slot_rows[i];
        const uint32_t word = row.vtable + 8u * row.slot + 4u;
        if (word + 4 > 0x30A460u || row.vtable < 0x2E6F00u) continue;
        if (done.count(word)) { ++r.already; continue; }
        // Past this vtable's end? Another declared vtable starts first, or an all-zero entry (every vtable's entry 0) lies between.
        auto nx = std::upper_bound(starts.begin(), starts.end(), row.vtable);
        bool beyond = nx != starts.end() && *nx <= word - 4;
        for (uint32_t e = row.vtable + 8; !beyond && e + 8 <= word + 4; e += 8)
            if (rd32(mem + e) == 0 && rd32(mem + e + 4) == 0) beyond = true;
        if (beyond) { ++r.beyond_vtable; continue; }
        // The entry must look like a GCC 2.9x one: index 0, a small delta, a function word that was patched to a table index
        const int16_t delta = int16_t(rd16(mem + word - 4)), index = int16_t(rd16(mem + word - 2));
        if (index != 0 || delta < -4096 || delta > 4096) { ++r.beyond_vtable; continue; }
        const uint32_t v = rd32(mem + word);
        if (v >= image.code_start && v < image.code_end) { ++r.unported; continue; }   // still a retail address: no body
        if (v >= table->size || !table->data[v].func) continue;                      // not a function index at all
        ++r.slots_checked;
        done[word] = true;
        bool match = false;
        for (uint32_t t = 0; t < row.ntypes && !match; ++t)
            match = table->data[v].func_type == *row.types[t] || std::memcmp(table->data[v].func_type, *row.types[t], 32) == 0;
        if (match) { ++r.matched; continue; }
        Pool& pool = pools[row.sig];
        uint32_t thunk;
        if (v == pure_index && pool.has_pure) {
            thunk = pool.pure;
            ++r.pure;
        } else {
            if (pool.free.empty()) {
                ++r.pool_exhausted;
                const std::string s = te_vt_signatures[row.sig];
                if (std::find(r.pool_exhausted_signatures.begin(), r.pool_exhausted_signatures.end(), s) == r.pool_exhausted_signatures.end())
                    r.pool_exhausted_signatures.push_back(s);
                continue;
            }
            const auto [target, fn] = pool.free.back();
            pool.free.pop_back();
            const uint32_t t = targets_addr + target * 4;
            mem[t] = uint8_t(v); mem[t + 1] = uint8_t(v >> 8); mem[t + 2] = uint8_t(v >> 16); mem[t + 3] = uint8_t(v >> 24);
            thunk = fn;
            ++r.thunked;
            if (r.thunked_words.size() < 4096) r.thunked_words.emplace_back(word, v);
        }
        mem[word] = uint8_t(thunk); mem[word + 1] = uint8_t(thunk >> 8); mem[word + 2] = uint8_t(thunk >> 16); mem[word + 3] = uint8_t(thunk >> 24);
        if (verbose) std::fprintf(stderr, "  vtable 0x%08X slot %u: body index %u -> thunk %u (%s)%s\n", row.vtable, row.slot, v, thunk,
                                  te_vt_signatures[row.sig], v == pure_index ? " [pure virtual]" : "");
    }
    return r;
}

}  // namespace te

// The call-site type adapters (tools/gen_eabi_dispatch.py, wired by tools/w2c_diag.py) report every adapted call
// here: a mismatch that no vtable slot thunk caught (a slot the decomp calls through a `void*` object, or one two
// callers declare differently). Counted, and the first distinct (expected type, callee) pairs are logged with the
// callee's retail address so the residue stays visible in every run log.
extern w2c_env* te_diag_env;
namespace {
struct AdaptSite { const char* expected; uint32_t index; uint64_t calls; };
std::vector<AdaptSite> g_adapt_sites;
uint64_t g_adapt_calls = 0;
constexpr size_t kAdaptLogMax = 64;
}  // namespace

extern "C" void te_adapt_note(const wasm_rt_funcref_table_t* table, const char* expected_type, uint32_t index) {
    ++g_adapt_calls;
    for (AdaptSite& s : g_adapt_sites)
        if (s.index == index && (s.expected == expected_type || std::memcmp(s.expected, expected_type, 32) == 0)) { ++s.calls; return; }
    g_adapt_sites.push_back({expected_type, index, 1});
    if (g_adapt_sites.size() > kAdaptLogMax) return;
    const char* got = index < table->size && table->data[index].func ? table->data[index].func_type : nullptr;
    // the retail address of the callee, when the retail table knows it (the module's exported {address, index} pairs)
    uint32_t retail = 0;
    if (te_diag_env && te_diag_env->instance) {
        const uint8_t* mem = te_diag_env->memory.data;
        const uint32_t table_addr = *w2c_game_te_retail_functions(te_diag_env->instance);
        const uint32_t count = te::rd32(mem + *w2c_game_te_retail_function_count(te_diag_env->instance));
        for (uint32_t i = 0; i < count; ++i)
            if (te::rd32(mem + table_addr + i * 8 + 4) == index) { retail = te::rd32(mem + table_addr + i * 8); break; }
    }
    std::fprintf(stderr, "[twinsanityevolved] call_indirect ADAPTED: table index %u (retail 0x%08X%s) has type %s, the caller expects %s\n",
                 index, retail, retail ? "" : " = not a retail function (C++-only or a thunk)",
                 got ? te::TypeName(got) : "(no function)", te::TypeName(expected_type));
}

// An indirect call that cannot be adapted: the index is out of the table (an unpatched retail code address: the body
// was not ported -- see the runner's unported list) or the entry is empty. Printed before wasm2c's trap.
extern "C" void te_call_indirect_bad(const wasm_rt_funcref_table_t* table, const char* expected_type, uint32_t index) {
    const te::RetailImage image;
    const bool retail_addr = index >= image.code_start && index < image.code_end;
    std::fprintf(stderr, "[twinsanityevolved] call_indirect BAD INDEX: %u (0x%08X)%s, table size %u, the caller expects %s\n",
                 index, index, retail_addr ? " = an UNPORTED retail function address (no body in the module)" : "",
                 table->size, expected_type ? te::TypeName(expected_type) : "(null type)");
}

namespace te {
AdaptReport AdaptedCalls() {
    AdaptReport r;
    r.calls = g_adapt_calls;
    r.sites = uint32_t(g_adapt_sites.size());
    return r;
}
}  // namespace te

w2c_env* te_diag_env = nullptr;

// A callback the game handed the host as a function pointer (Movie::BeginPresenting's `present`) arrives as a wasm
// TABLE INDEX. The host calls it back into the module through the table, after checking the entry's type is the
// `void()` the header declares (any other type is logged and skipped -- never a trap from inside a host body). The
// module is re-entered from the import that is running: wasm2c's functions are plain C, so the call nests on the
// host stack exactly as a PS2 interrupt handler nested on the game's.
extern "C" int te_call_void_callback(uint32_t index) {
    if (!te_diag_env || !te_diag_env->instance) return 0;
    wasm_rt_funcref_table_t* table = w2c_game_0x5F_indirect_function_table(te_diag_env->instance);
    if (index >= table->size || !table->data[index].func) {
        std::fprintf(stderr, "[twinsanityevolved] callback: bad table index %u (table size %u)\n", index, table->size);
        return 0;
    }
    const wasm_rt_funcref_t ref = table->data[index];
    if (std::strcmp(te::TypeName(ref.func_type), "() -> nil") != 0) {
        std::fprintf(stderr, "[twinsanityevolved] callback: table index %u has type %s, not () -> nil: not called\n", index,
                     te::TypeName(ref.func_type));
        return 0;
    }
    reinterpret_cast<void (*)(void*)>(ref.func)(ref.module_instance);
    return 1;
}
#endif   // !TE_NATIVE (the module's tables, thunks, adapters and callbacks: te_native.cpp on the native lane)
