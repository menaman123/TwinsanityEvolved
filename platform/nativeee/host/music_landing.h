// music_landing.h -- a music window's tracks made from its bytes as they land in pieces, front to back (the audio owner's
// music reads off the game thread: audio_service.cpp MusicRead over te::io). The result is title::MusicAsset::Read's for
// the whole window, byte for byte: the tracks allocated whole at the first piece (a mono window's VAGp/MSVp header decides
// where its track starts and the rate it keys on at), the pairs de-interleaved as Read does (the last pair's `final`
// bytes), and Read's loop-marker edits (its Own: every block's flag byte 0, the first block's loop start, the last block's
// loop end, + repeat when looping) made block by block as each lands. Each track's landed mark is published to
// title::SampleLanding, which holds a voice's Decode back from a block not yet in place.
// Accepted(): whether the window's SIZES alone decide that Read accepts it (then the stream is Prepared before a byte is
// read); otherwise the caller reads it whole as before. Pieces must come in order (the disc reader's do).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/sound/vag_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

namespace te::audio {

class MusicLanding {
public:
    explicit MusicLanding(const title::MusicAsset::Layout& layout) : layout_(layout) {}
    ~MusicLanding() { EndSlots(); }
    MusicLanding(const MusicLanding&) = delete;
    MusicLanding& operator=(const MusicLanding&) = delete;

    static bool Accepted(const title::MusicAsset::Layout& layout, uint64_t bytes) {
        constexpr uint64_t kBlock = VagDecoder::kBlockBytes;
        // mono: accepted with or without the 48-byte header iff more than 48 bytes of whole blocks (a header decides
        // only windows of 16..48 bytes, which go the whole way)
        if (layout.tracks == 1) return bytes > 48 && bytes % kBlock == 0 && bytes <= UINT32_MAX;
        if (layout.tracks != 2) return false;
        const uint64_t block = layout.block_bytes, last = layout.final_bytes ? layout.final_bytes : layout.block_bytes;
        return block != 0 && block % kBlock == 0 && last != 0 && last <= block && last % kBlock == 0 &&
               bytes % (block * 2) == 0 && bytes / (block * 2) >= 1 && bytes <= UINT32_MAX;
    }

    // Every track's first block in place (or the window finished): `data` may go to the voices
    bool Ready() const { return ready_.load(std::memory_order_acquire); }
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    // MusicAsset::Read's result (written at the first piece, before Ready); the bytes behind the marks still landing
    title::MusicAsset::Data data;

    // The piece [pos, pos + n) of the window of `size` bytes
    void Land(size_t size, size_t pos, const uint8_t* bytes, size_t n) {
        if (!allocated_) {
            if (pos != 0 || n < 48) return;   // the first piece always holds the header but on a failing card
            Allocate(size, bytes, n);
        }
        const size_t end = pos + n;
        if (layout_.tracks == 1) {
            const size_t from = std::max(pos, header_);
            if (from < end) Copy(0, from - header_, bytes + (from - pos), end - from);
        } else {
            const size_t block = layout_.block_bytes, last = layout_.final_bytes ? layout_.final_bytes : layout_.block_bytes;
            const size_t pairs = size / (block * 2);
            for (size_t b = pos / block; b * block < end; ++b) {
                const size_t pair = b / 2, side = b % 2;
                if (pair >= pairs) break;
                const size_t count = pair + 1 == pairs ? last : block, start = b * block;
                const size_t lo = std::max(pos, start), hi = std::min(end, start + count);
                if (lo < hi) Copy(side, pair * block + (lo - start), bytes + (lo - pos), hi - lo);
            }
        }
        bool first_blocks = true;
        for (uint32_t side = 0; side < layout_.tracks; ++side) {
            Mark(side);
            if (flagged_[side] < std::min<size_t>(VagDecoder::kBlockBytes, total_[side])) first_blocks = false;
        }
        if (first_blocks && guarded_) ready_.store(true, std::memory_order_release);
    }

    // The window's end: `got` of its `size` bytes came. True when the card fell short (what did not land stays silent).
    bool Finish(size_t size, size_t got) {
        const bool short_read = got < size || !allocated_;
        if (short_read) {
            failed_.store(true, std::memory_order_release);
            for (uint32_t side = 0; allocated_ && side < layout_.tracks; ++side) {
                copied_[side] = total_[side];
                Mark(side);
            }
        }
        EndSlots();
        ready_.store(true, std::memory_order_release);
        return short_read;
    }

private:
    static uint32_t Big32(const uint8_t* b) {
        return static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 | static_cast<uint32_t>(b[2]) << 8 | b[3];
    }
    void Allocate(size_t size, const uint8_t* first, size_t n) {
        allocated_ = true;
        data.count = layout_.tracks;
        data.source_bytes = size;
        if (layout_.tracks == 1) {
            header_ = title::MusicAsset::HeaderBytes(first, n);
            data.header_bytes = static_cast<uint32_t>(header_);
            if (header_) data.source_rate = Big32(first + 16);
            total_[0] = size - header_;
        } else {
            const size_t block = layout_.block_bytes, last = layout_.final_bytes ? layout_.final_bytes : layout_.block_bytes;
            total_[0] = total_[1] = (size / (block * 2) - 1) * block + last;
        }
        for (uint32_t side = 0; side < layout_.tracks; ++side) {
            auto track = std::make_shared<std::vector<uint8_t>>(total_[side]);
            tracks_[side] = track.get();
            data.tracks[side] = std::move(track);
            slots_[side] = title::SampleLanding::Begin(tracks_[side], 0);
            if (slots_[side] < 0) guarded_ = false;   // no slot free: Ready once the window is whole
        }
    }
    void Copy(size_t side, size_t at, const uint8_t* src, size_t n) {
        if (at != copied_[side] || at + n > total_[side]) return;   // in order, inside the track (so by construction)
        std::memcpy(tracks_[side]->data() + at, src, n);
        copied_[side] = at + n;
    }
    // The whole blocks copied so far get Read's marker edits, then the track's landed mark moves past them
    void Mark(size_t side) {
        constexpr size_t kBlock = VagDecoder::kBlockBytes;
        std::vector<uint8_t>& track = *tracks_[side];
        const size_t blocks = total_[side] / kBlock;
        while (flagged_[side] + kBlock <= copied_[side]) {
            const size_t index = flagged_[side] / kBlock;
            uint8_t flags = index == 0 ? VagDecoder::kFlagLoopStart : 0;
            if (index + 1 == blocks) flags |= VagDecoder::kFlagLoopEnd | (layout_.looping ? VagDecoder::kFlagLoopRepeat : 0);
            track[flagged_[side] + 1] = flags;
            flagged_[side] += kBlock;
        }
        title::SampleLanding::Advance(slots_[side], static_cast<uint32_t>(flagged_[side]));
    }
    void EndSlots() {
        for (int& slot : slots_) {
            title::SampleLanding::End(slot);
            slot = -1;
        }
    }

    title::MusicAsset::Layout layout_;
    std::array<std::vector<uint8_t>*, 2> tracks_{};
    std::array<size_t, 2> copied_{}, flagged_{}, total_{};
    std::array<int, 2> slots_{{-1, -1}};
    std::atomic<bool> ready_{false}, failed_{false};
    bool allocated_ = false, guarded_ = true;
    size_t header_ = 0;
};

}  // namespace te::audio
