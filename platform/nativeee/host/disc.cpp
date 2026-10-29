// platform/nativeee (host side) -- disc.h: Platform::Disc over the engine's IsoFs. The disc image path is the
// manifest (te::Settings::iso_path): opened once, read at RUN time by Files, the stream façade and the runner's ELF
// load. The libcdvd rows' constants are kept (SCECdComplete 2 / SCECdNotReady 6), nothing waits: the image is there
// or it is not.
#include <cstdio>

#include "disc_reader.h"
#include "engine/cdvd/iso_fs.h"
#include "ps2e_seam.h"
#include "te_mods.h"
#include "te_services.h"

namespace te {

// The volume: the image, or the enabled folder mods' disc/ trees composed over it (te_mods.h MakeDiscVolume) -- every
// reader (Files, the stream façade, the movies, the language banks, the runner's executable) reads it through Iso()
Disc::Disc(const Settings& settings) : iso_(mods::MakeDiscVolume()), path_(settings.iso_path) {}
Disc::~Disc() = default;

int32_t Disc::Initialise() {
    if (opened_) return 1;
    if (tried_) return 0;
    tried_ = true;
    if (path_.empty()) { error_ = "no disc image (te::Settings::iso_path empty)"; return 0; }
    opened_ = iso_->Open(path_, &error_);
    if (!opened_ && GetSettings().verbose) std::fprintf(stderr, "[twinsanityevolved] disc: %s\n", error_.c_str());
    // The reader thread (disc_reader.h: the reads off the game thread where te::io::Async()) and its locks, made here at the
    // start -- long before the graphics device exists (te_sync.h)
    if (opened_) te::io::Start();
    return opened_ ? 1 : 0;
}

int32_t Disc::SetMedia(int32_t media) {
    // sceCdMmode: 1 (Cd) / 2 (Dvd) accepted, returns 1; anything else is refused with 0.
    if (media != 1 && media != 2) return 0;
    media_ = media;
    return 1;
}

int32_t Disc::WaitReady() const { return opened_ ? 2 : 6; }

bool Disc::IsOpen() const { return opened_; }

}  // namespace te

extern "C" {

ps2e_s32 ps2e_disc_initialise(void) { return te::Services::Get().disc().Initialise(); }
ps2e_s32 ps2e_disc_set_media(ps2e_s32 media) { return te::Services::Get().disc().SetMedia(media); }
ps2e_s32 ps2e_disc_wait_ready(void) { return te::Services::Get().disc().WaitReady(); }

}  // extern "C"
