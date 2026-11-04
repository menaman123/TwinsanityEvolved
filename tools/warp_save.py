#!/usr/bin/env python3
"""warp_save.py -- the authoring "debug warp" (assume-playable level coverage, S-UV): a copy of a memory-card folder whose
slot-1 save (Bank0.bin) starts at another chunk. LOAD GAME on the copied card then loads that chunk as the saved level
(GameController::RequestSavedLevel: progress.startChunk = the save's chunk; the game's own progression is untouched -- the
save is only a way to START a pad log at a level's entry; the recorded pad log is the fixture, never the card).

    tools/warp_save.py --card build/padlog_smoke/mc_hub --chunk 'levels\\earth\\cavern\\cavent' --out build/warp_mc [--place N]

The save's layout (SaveController::Write after the 8-byte SaveDate): summary + options (8), timePlayed (4), the chunk as a
length-prefixed string, place (u16, 0xFFFF = no checkpoint: the chunk's start), the screen offset and volumes, the 0xF000
data block; the bank ends with the tag (1) and a checksum over the file that the game accepts when it is ZERO
(SaveFile::EndRead: `checksum == 0 || Checksum(...) == checksum`), so the trailer is rewritten as tag 1 / checksum 0.
The folder's other files (icon.sys, Crash.ico, BESLES-52568, Bank1..3) are copied as they are. No game data is produced:
the card is derived at run time from the card the hub pad log writes."""
import argparse
import os
import shutil
import struct
import sys


CHARACTERS = {"crash": 0, "cortex": 1, "tallcrash": 2, "nina": 3, "none": 6}


def warp(bank, chunk, place, character=None, areas=None, second=None, pairing=None):
    data = bytearray(bank)
    size = len(data)
    if areas:
        # The save's options (SaveControllerOptions, the word after the summary): the area play is in (bits 0-4), the area the
        # story has got to (5-9) and the last area open (10-14), which SaveController::Restore gives the progress's play state.
        # The scripts read the story's area (StoryAreaCondition 0x27F) to pick what a chunk shows: a cutscene of the story plays
        # only when the story has got to its area. A save made at that point of the story carries these numbers.
        (options,) = struct.unpack_from("<I", data, 12)
        for name, shift in (("area", 0), ("story", 5), ("open", 10)):
            if areas.get(name) is not None:
                options = (options & ~(31 << shift)) | ((areas[name] & 31) << shift)
        struct.pack_into("<I", data, 12, options)
    if character is not None:
        # The summary's pairing / character / second (SaveControllerSummary, the word after the SaveDate): a save written INSIDE a
        # chunk carries the character played there, and GameController::StartingPlay restarts that character's follow camera
        # before any script runs. The hub autosave (the card every warp derives from) holds NoCharacter (6) -- a chunk whose start
        # script does not set the player mode (levels\\earth\\cavern\\escape, entered only from cortthro in the game) then has no
        # player at StartingPlay. --character writes the summary a save made in that chunk would hold: pairing Alone (1),
        # the character, second NoCharacter.
        (summary,) = struct.unpack_from("<I", data, 8)
        summary = (summary & 0x000FFFFF) | (1 << 20) | (character << 24) | (6 << 28)
        struct.pack_into("<I", data, 8, summary)
    if second is not None or pairing is not None:
        # The summary's pairing (bits 20-23) and second character (28-31): a save made with Cortex along (the Academy's corridors,
        # the ice hub after the lab) names him as the second; the scripts that need him (the Cortex follow manager's run) wait
        # for him otherwise
        (summary,) = struct.unpack_from("<I", data, 8)
        if pairing is not None:
            summary = (summary & ~(15 << 20)) | (pairing << 20)
        if second is not None:
            summary = (summary & ~(15 << 28)) | (second << 28)
        struct.pack_into("<I", data, 8, summary)
    off = 8 + 8 + 4   # SaveDate, summary + options, timePlayed
    (old_len,) = struct.unpack_from("<I", data, off)
    if not (0 < old_len < 128):
        raise SystemExit("warp_save: not a save bank (chunk length %d at 0x%x)" % (old_len, off))
    old_tail = off + 4 + old_len
    new = struct.pack("<I", len(chunk)) + chunk.encode("ascii")
    body = data[:off] + new + data[old_tail:]
    body = body[:size - 8]
    if len(body) < size - 8:
        body += b"\xA5" * (size - 8 - len(body))
    if place is not None:
        struct.pack_into("<H", body, off + 4 + len(chunk), place)
    body += struct.pack("<ii", 1, 0)   # the tag and a zero checksum (accepted)
    assert len(body) == size
    return bytes(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", required=True, help="a --mc-dir holding the hub save (mc0/BESLES-52568/Bank0.bin)")
    ap.add_argument("--chunk", required=True, help="the chunk path to start at, e.g. levels\\earth\\cavern\\cavent")
    ap.add_argument("--out", required=True, help="the card copy to write (a --mc-dir)")
    ap.add_argument("--place", type=lambda v: int(v, 0), default=None, help="the checkpoint id (default: keep the save's; 0xFFFF = the chunk's start)")
    ap.add_argument("--character", default=None, help="the character the save's summary names as played (crash, cortex, nina, tallcrash; default: keep the save's -- the hub autosave's is none)")
    ap.add_argument("--story", type=int, default=None, help="the area the story has got to (0-24; default: keep the save's -- the hub autosave's is 0)")
    ap.add_argument("--open", type=int, default=None, help="the last area open (0-24; default: keep the save's)")
    ap.add_argument("--area", type=int, default=None, help="the area play is in (0-24; default: keep the save's; the chunk's start script sets its own)")
    ap.add_argument("--second", default=None, help="the second character the save's summary names (crash, cortex, nina, tallcrash, none; default: keep the save's, or none with --character)")
    ap.add_argument("--pairing", type=int, default=None, help="how the second is paired (1 alone .. 7: progress.h Pairing; default: keep the save's, or 1 with --character)")
    ap.add_argument("--bank", default="mc0/BESLES-52568/Bank0.bin")
    a = ap.parse_args()
    for name in ("story", "open", "area"):
        value = getattr(a, name)
        if value is not None and not 0 <= value <= 24:
            raise SystemExit("warp_save: --%s %d is not an area (0-24)" % (name, value))
    src = os.path.join(a.card, a.bank)
    if not os.path.isfile(src):
        raise SystemExit("warp_save: no save bank at %s (run the hub pad log on that card first)" % src)
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(a.card, a.out)
    bank = open(src, "rb").read()
    character = None
    if a.character is not None:
        if a.character.lower() not in CHARACTERS:
            raise SystemExit("warp_save: unknown character %s (crash, cortex, tallcrash, nina, none)" % a.character)
        character = CHARACTERS[a.character.lower()]
    second = None
    if a.second is not None:
        if a.second.lower() not in CHARACTERS:
            raise SystemExit("warp_save: unknown character %s (crash, cortex, tallcrash, nina, none)" % a.second)
        second = CHARACTERS[a.second.lower()]
    if a.pairing is not None and not 1 <= a.pairing <= 7:
        raise SystemExit("warp_save: --pairing %d is not a pairing (1-7)" % a.pairing)
    areas = {"story": a.story, "open": a.open, "area": a.area}
    out = warp(bank, a.chunk, a.place, character, areas if any(v is not None for v in areas.values()) else None, second, a.pairing)
    open(os.path.join(a.out, a.bank), "wb").write(out)
    print("warp_save: %s -> %s: slot 1 starts at '%s'%s%s%s" % (src, a.out, a.chunk, "" if a.place is None else " place 0x%X" % a.place,
                                                                "" if character is None else " character %s" % a.character.lower(),
                                                                "".join(" %s %d" % (k, v) for k, v in areas.items() if v is not None) +
                                                                ("" if second is None else " second %s" % a.second.lower()) +
                                                                ("" if a.pairing is None else " pairing %d" % a.pairing)))


if __name__ == "__main__":
    main()
