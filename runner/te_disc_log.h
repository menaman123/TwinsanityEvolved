// runner/te_disc_log.h -- the disc's identity at boot: which program, which region, and whether the image IS a known retail
// dump, judged by content against the tables built into the binary (launcher/discs/*.json: the known dumps, the file
// lists; engine/app/host/disc_identity.h). One line on stderr:
//   "[twinsanityevolved] disc: modified PAL image SLES-52568 (not the retail dump: 2,579,234,816 bytes vs 2,558,263,296;
//    volume CRASH)".
// The size is compared at every boot (decisive when it differs); the whole image is hashed (CRC-32 + SHA-1, seconds) only
// with --verify-disc or TE_DISC_VERIFY=1 -- boot time matters.
#pragma once
#include <string>

#include "app/host/disc_identity.h"

namespace te::disc {

// With a file list of retail provenance (none is built in today): the files up to this size are hashed at boot to name
// the ones a modified disc changes; larger ones are compared by size.
inline constexpr unsigned long long kBootHashBytes = 4ull << 20;

// The disc tables built into the binary (the known dumps, the file lists), registered once; the region maps go to
// `region_map` (disc_regions.h) when given. Logs a table that does not read.
void RegisterBuiltInTables();

// True when the run asks for the whole-image check: TE_DISC_VERIFY=1 (the command line's --verify-disc is passed in).
bool VerifyRequested(bool verify_flag);

// Identifies the image at `iso` and logs the line; false when the disc is refused (the line then says why). `verify` hashes
// the whole image. Reading the disc's directories (and, verifying, every sector) is all it does: no game state is touched.
bool LogIdentity(const std::string& iso, ps2r::app::DiscIdentity* out = nullptr, bool verify = false);

}  // namespace te::disc
