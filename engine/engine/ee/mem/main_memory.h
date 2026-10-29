#pragma once
// EE main RAM: 32 MB of physical memory. KUSEG (0x0xxxxxxx), KSEG0 (0x8...)
// and KSEG1 (0xA...) all map onto it through `addr & 0x1FFFFFFF`; the RAM
// itself is the low 32 MB of that physical window.
#include <cstddef>
#include <cstdint>
#include <vector>
#if defined(__vita__)
#include <malloc.h>
#include <cstdlib>
#endif

#if defined(__vita__)
// The Vita: the 32 MB on a 4 KB boundary. newlib's malloc aligns to 8 bytes, which can put the game's 16- and 32-byte
// aligned data (the PS2's quadword vectors and matrices) across the Cortex-A9's 32-byte cache lines; on a page every
// guest alignment is the host's. Only the host address changes -- the game sees offsets into the block.
template <class T>
struct MainMemoryPageAllocator {
    using value_type = T;
    MainMemoryPageAllocator() = default;
    template <class U>
    MainMemoryPageAllocator(const MainMemoryPageAllocator<U>&) {}
    T* allocate(size_t n) {
        void* p = memalign(4096, n * sizeof(T));   // newlib's (the Vita's has no posix_memalign body)
        if (!p) std::abort();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { std::free(p); }
    template <class U>
    bool operator==(const MainMemoryPageAllocator<U>&) const { return true; }
    template <class U>
    bool operator!=(const MainMemoryPageAllocator<U>&) const { return false; }
};
#endif

class MainMemory {
public:
    static constexpr uint32_t kSize = 32u * 1024u * 1024u;
    static constexpr uint32_t kPhysMask = 0x1FFFFFFFu;

    MainMemory();

    // Physical address of a KUSEG/KSEG0/KSEG1 virtual address.
    static uint32_t Phys(uint32_t addr) { return addr & kPhysMask; }

    // Host pointer for `size` bytes at physical `phys`, or nullptr when the
    // range is not entirely inside RAM.
    uint8_t* Pointer(uint32_t phys, size_t size);
    const uint8_t* Pointer(uint32_t phys, size_t size) const;

    uint8_t* Data() { return bytes_.data(); }
    const uint8_t* Data() const { return bytes_.data(); }
    size_t Size() const { return bytes_.size(); }

    void Clear();

private:
#if defined(__vita__)
    std::vector<uint8_t, MainMemoryPageAllocator<uint8_t>> bytes_;
#else
    std::vector<uint8_t> bytes_;
#endif
};
