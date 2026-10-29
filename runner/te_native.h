// te_native.h -- the PlayStation Vita's NATIVE build of the game code (CMake TE_VITA_NATIVE=ON; compiled only there).
//
// The game's translation units are compiled for armv7 by the Vita's GCC (tools/te_cxx_arm.py: the five MIPS-only
// constructs of the decomp rewritten, zero edits to game code) instead of wasm32 + wasm2c, and linked into the eboot as one
// prelinked object whose symbols are local but for its entry and the retail function table (the decomp's own libc / malloc
// / operator new never meet newlib's).
//
// THE ADDRESS MODEL: a guest address IS the native address (the Vita's pointers are 32-bit). The game's retail address
// space is te_guest_block (32 MB of .bss, tools/gen_retail_image.py --arm): the retail data image lives at
// te_guest_block + its retail address (loaded from the disc's executable as before), the game's heap at te_guest_block +
// 0x3F6480 .. + 0x2000000 (platform/nativeee/wasm/memory.cpp) -- the wasm module's offsets, bit for bit. Everything else
// the game points at (its C++ data, its stack: a kernel thread's, which the Vita cannot place) is wherever the native
// toolchain put it. The seam's u32 "guest addresses" are the pointers themselves (the forwarders' reinterpret_cast<u32>, as
// on wasm), so a host body reads any game pointer -- an argument, or one the game stored in a structure -- with
// te::GuestPointer as the identity; host code naming a RETAIL address adds the block's base (te::RetailAddress, te_host.h).
//
// At load the retail image's pointer words are made native: the data addresses it holds (the reviewed list
// tools/native/retail_data_ptrs.tsv) get the block's base added, the retail function addresses become the native functions'
// (the same table the wasm build patches indices from). No vtable thunks and no call-site adapters exist on this lane:
// AAPCS-VFP takes the arguments a body declares as a prefix of what any caller passes (notes: the vtable slot census).
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

struct w2c_env;

namespace te {
struct PatchReport;
}

namespace te::native {

constexpr uint32_t kGuestBlockSize = 0x2000000;

// The block (its address is the bias every retail address gets)
uint8_t* GuestBlock();
// env.memory = the block (zeroed)
void BindGuestBlock(w2c_env& env);

struct DataRelocationReport {
    uint32_t relocated = 0;            // listed words whose retail data address got the block's base added
    uint32_t not_an_address = 0;       // listed words whose value is no address in the image (another executable's data)
    uint32_t out_of_range = 0;         // listed words outside .data/.rodata/.sdata (a broken list)
    uint32_t unlisted_candidates = 0;  // words the list does not name whose value lies in the image (text, packed halves)
    std::vector<uint32_t> examples;    // the first not_an_address words
};
// After the image is loaded (and before the code pointers are patched): the listed data pointers made native.
DataRelocationReport RelocateRetailDataPointers(w2c_env& env);

// The retail CODE addresses in .data/.rodata/.sdata -> the native functions (te_env.h's report)
te::PatchReport PatchRetailCodePointers(w2c_env& env, const std::vector<uint32_t>& known_function_starts);
uint32_t RetailFunctionCount();

}  // namespace te::native
