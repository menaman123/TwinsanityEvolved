// te_link_ahead.h -- the chunks a level's links lead to, read ahead from the card before the game asks for them (the PS Vita).
//
// THE GAME STREAMS ITS LEVELS a chunk at a time (game/src/game/chunkloading.cpp): every frame the chunk loading manager
// follows the links of the chunk the player is in (its focus); a linked chunk the player comes near (a link's hulls) is
// wanted, loaded a second later -- its SM2 and RM2 read from the archive (CRASH6\CRASH.BD) in 128 KB blocks every few frames
// at the drive's pace -- and dropped 4 s after it is no longer wanted. On a PS2 the DVD delivered them at 2.2 MB/s with its
// own seeks. The PS Vita's memory card is slower in play (1.6-3.1 MB/s in reads of ~215 KB in the device logs: the bus
// chase's roofcor2, 6.5 MB, read while the player runs at it), so these reads come late.
//
// THE READ-AHEAD: a boundary hook reads the manager (G_ChunkLoadingManager_: its focus loader, the focus's links and their
// loaders' states -- read-only, the game's own structures) and has the reader thread read every linked chunk the game does
// not have yet into memory, whole, in big pieces at the back of its queue (te::io::ReadAhead, Ahead::Links): from the frame the
// player enters a chunk, the chunks it leads to come off the card -- a minute ahead of the chase's roofcor2 -- and the game's
// reads of them are served from memory. A chunk the game has loaded, or no link leads to any more, is let go of. Bounded by
// the reader's budget (te::io::AheadBudget); a chunk that does not fit is read as before. Nothing the game reads changes:
// the bytes are the image's; only when they come off the card.
//
// THE ARCHIVE INDEX (CRASH6\CRASH.BH, read through the disc's volume at first use: names, offsets and sizes in CRASH.BD) is
// also the stream service's: a read of the archive outside the read-ahead has the rest of its entry read ahead whole
// (stream_service.cpp, Ahead::Entry).
//
// THE CENSUS: one line per chunk read ahead ("[te.io] reading ahead the linked chunk ..."), the reader's window lines.
//
// TE_CHUNK_LOG=1 (on in the PS Vita's built-in diagnosis set): a line at each change of a loaded or loading chunk -- its SM2's
// and RM2's states, the link depths it is wanted at, its chunk's state (hidden until its files are all in), its scenery in or
// not, and the drawing's answer (not reached / reached with nothing drawn / drawn): "[te.io] chunk loaders @B: '...' SM2
// loaded, RM2 loading; depth 1, keep depth 1; hidden, scenery in, drawn". What a picture shows of the next chunk through a
// doorway is what these say: not loaded yet, culled, or drawn.
#pragma once
#include <cstdint>
#include <string>

namespace te::archive {

struct Entry {
    uint64_t image = 0;   // its first byte in the image
    uint32_t size = 0;
};
// The entry of a name (as the archive names it, any case: "levels\school\rooftop\roofcor2.sm2"); false when the disc has no
// such archive or entry
bool Find(const std::string& name, Entry& out);
// The entry an image byte lies in (false: not in the archive's data, or between entries)
bool EntryAt(uint64_t image_byte, Entry& out);

}  // namespace te::archive

namespace te::link_ahead {

// The boundary hook (installed once by the runner; nothing happens without the reader thread and a budget)
void Install();

}  // namespace te::link_ahead
