// platform/nativeee (wasm side) -- Platform::Memory over the 32 MB linear memory == NativeEE MainMemory.
//
// Memory map of the game module (wasm address == PS2 guest address):
//   0x00000000 .. 0x00040000   the wasm shadow stack (wasm-ld --stack-first, -z stack-size): the PS2 kernel's range, which
//                              no game code addresses; the PS2 main thread's stack was the scratchpad
//   0x00040000 .. global-base  free (the rest of the PS2 kernel's and the retail CODE's range: no code lives in memory here)
//   global-base .. 0x002D9D90  the C++'s own .rodata/.data/.bss, linked to END at the retail image (tools/link_wasm.py's
//                              two passes): the image's addresses and the data end never move with the build
//   0x002D9D90 .. 0x003DB200   the RETAIL IMAGE (.vutext .data .rodata .sdata .sbss .bss) -- a zero .bss segment, the
//                              module's LAST data (== __data_end == __heap_base), which the runner fills from the disc's
//                              ELF at RUN time (tools/gen_retail_image.py)
//   0x003DB200 .. 0x003F6480   the C library's break range: dlmalloc (game/src/platform/ps2/malloc.cpp, kept) over
//                              RetailLibc::Sbrk, whose break word g_HeapEnd (retail D_002EABE4) is bumped to __heap_base
//                              at entry (wasm/entry.cpp); nothing is allocated here before the pools
//   0x003F6480 .. 0x02000000   the game's pools, from FixedPoolStart (below): the heap pool (PoolSpace - DiskPoolSize, a
//                              third of it the small pages) then the disk pool. The PS2's pools started at its _end
//                              (0x3DB200) + HeapReserve, so the game's heap is a few hundred KB short of the PS2's; the
//                              renderer's 0x540000-byte DMA block the PS2 took from it is not taken here.
#include "platform/memory.h"
#include "debug.h"
#include "retail/libc.h"
#include <kernel.h>

extern "C" unsigned char __heap_base[];

#if defined(TE_NATIVE)
// The Vita's native build (runner/te_native.h): the game's retail address space is te_guest_block (retail address X at
// te_guest_block + X), so the fixed addresses below are the block's -- the same offsets, in native memory
extern "C" unsigned char te_guest_block[];
#define TE_RETAIL_ADDRESS(x) (reinterpret_cast<u32>(te_guest_block) + (x))
#else
#define TE_RETAIL_ADDRESS(x) (x)
#endif

namespace
{
constexpr u32 MemorySize = 0x2000000;
// What the retail game left the C library's heap before its pools (memory.cpp of the PS2 arm): kept for the same pool size
constexpr u32 HeapReserve = 0x2800;
// The pools start HERE, not at the break the C++'s data leaves, so the game's heap layout does not change with the build
// The game's replay DOES depend on it (a 16-byte shift of the break desyncs jungle_bungle_walk.padlog at boundary 7030;
// the PS2's layout never moved). 0x3F6480 is the break of the frozen 1952747 (its __heap_base; the pools' first word D_002EA828 holds it),
// so every pad log and judge recorded on it replays bit for bit. Since the heap layout was frozen the C++'s data sits BELOW the retail image
// (tools/link_wasm.py) and __heap_base is the image's end 0x3DB200 for good; the bytes 0x3DB200..0x3F6490 are the C library's
// (dlmalloc over Sbrk: nothing is allocated there before the pools), then the pools.
constexpr u32 FixedPoolStartRetail = 0x3F6480;

inline u32 MemoryEnd()
{
    return TE_RETAIL_ADDRESS(MemorySize);
}

inline u32 FixedPoolStart()
{
    return TE_RETAIL_ADDRESS(FixedPoolStartRetail);
}

u32 PoolStart()
{
    u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
    return now < FixedPoolStart() ? FixedPoolStart() : now;
}
}

extern "C" void* EndOfHeap(void)
{
    return reinterpret_cast<void*>(MemoryEnd());
}

u32 Platform::Memory::PoolSpace()
{
    return MemoryEnd() - (PoolStart() + HeapReserve);
}

void* Platform::Memory::AllocatePool(u32 size)
{
    static bool moved;
    if (!moved)
    {
        moved = true;
        u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
        if (now < FixedPoolStart())
        {
            RetailLibc::Sbrk(static_cast<s32>(FixedPoolStart() - now));
        }
    }

    return RetailLibc::Malloc(size);
}

// Host memory is coherent: the engine reads the same bytes the game wrote. Nothing to do.
void Platform::Memory::Synchronise()
{
}

void Platform::Memory::BeforeDeviceWrite(void*, u32)
{
}

void Platform::Memory::WriteBackCache()
{
}
