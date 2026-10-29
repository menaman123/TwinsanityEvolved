// app/launcher/disc_text.h -- what a disc's card says, in plain language, from its identity: the identity badge (the region
// and what the image is: "PAL · original disc", "PAL · modified disc", "PAL · checking…", "PAL · unrecognised version"),
// the playability badge (shown only for "Not playable in this version"), the verdict in a sentence ("A modified copy of the
// PAL disc: 20 MB larger than the original.") and the details line (serial · size · folder). One place, so the setup's
// cards and the Games page say the same thing -- and the same thing the game's boot line says.
#pragma once
#include <QString>
#include <QStringList>
#include "badge.h"
namespace ps2r::launcher {
struct DiscFound;

class DiscText {
public:
    static QString regionBadge(const DiscFound& d);      // PAL / NTSC-U / NTSC-J / Unknown region
    static QString identityBadge(const DiscFound& d);    // "PAL · modified disc"
    static Badge::Tone identityTone(const DiscFound& d);
    static QString stateBadge(const DiscFound& d, bool playable);   // "Not playable in this version", else "" (hidden)
    static Badge::Tone stateTone(const DiscFound& d, bool playable);
    static QString verdictLine(const DiscFound& d);      // "" for an original disc
    static QString detailsLine(const DiscFound& d);      // SLES-52568 · 2.4 GB · ~/Downloads
    static QString stateLine(const DiscFound& d, bool playable, const QString& notPlayable);   // "" for an original disc
    static QString fileName(const DiscFound& d);
};

}  // namespace ps2r::launcher
