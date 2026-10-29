#pragma once
// The binary patch formats the community ships against a disc image, read and applied by the importer:
//   xdelta3   VCDIFF (RFC 3284) as xdelta3 writes it: the default code table, source and target copy windows, xdelta3's
//             per-window Adler-32 (VCD_ADLER32) and application header. Refused, with a sentence saying so: secondary
//             compression (xdelta3 -S djw / fgk / lzma) in a window, an application-defined code table.
//   PPF       1.0, 2.0 and 3.0 (PlayStation Patch File): in-place byte records; 2.0 / 3.0's validation block (1024 bytes of
//             the image the patch was made for) and 2.0's image size are checked against the disc; 3.0's undo data and the
//             FILE_ID.DIZ trailer are skipped.
//   BPS       beat's patch: the source / target / patch CRC-32s and sizes checked.
// Applying writes the TARGET image to a new file (the importer's temporary copy under <mods>/.import-tmp) from the source
// image (the player's disc), which is never written.
#include <cstdint>
#include <functional>
#include <string>

namespace te::modimport {

enum class PatchFormat { Xdelta, Ppf, Bps };

struct PatchHeader {
    PatchFormat format = PatchFormat::Xdelta;
    std::string label;            // "xdelta3 (VCDIFF)", "PPF 3.0", "BPS"
    std::string description;      // PPF's description, BPS's metadata, xdelta3's application header
    uint64_t source_bytes = 0;    // the image the patch expects (BPS; PPF 2.0), 0 = not recorded
    uint64_t target_bytes = 0;    // the image it makes (BPS), 0 = not recorded (VCDIFF: known once applied)
    bool source_crc_known = false;
    uint32_t source_crc = 0, target_crc = 0;   // BPS
    bool validation_block = false;             // PPF 2.0 / 3.0 carry 1024 bytes of the source image
    uint64_t records = 0;                      // PPF records; VCDIFF windows (once applied)
    std::string secondary;                     // VCDIFF: the secondary compressor the header declares ("" = none)
};

// Progress over the target bytes written (0..1); false = cancel (the apply returns false with *error "stopped").
using PatchProgress = std::function<bool(double fraction)>;

bool ReadPatchHeader(PatchFormat format, const std::string& patch_path, PatchHeader* out, std::string* error);
// Checks what can be checked against the disc without applying (BPS: the source size, PPF: the validation block and the
// image size). True when nothing contradicts; false with *error a sentence for the player.
bool CheckPatchSource(const PatchHeader& header, const std::string& patch_path, const std::string& source_path, std::string* error);
bool ApplyPatch(PatchFormat format, const std::string& patch_path, const std::string& source_path, const std::string& target_path,
                const PatchProgress& progress, PatchHeader* header, std::string* error);

}  // namespace te::modimport
