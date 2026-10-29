#!/usr/bin/env python3
"""diff.py -- the oracle diff: the port's harness leg against the recomp oracle's leg on the same pad log.

The check ("frames/audio hashes within tolerance of the oracle on the same padlogs, every divergence
attributed: port bug / oracle bug / acceptable") made mechanical. The oracle is a TOOL for development and a check,
not a byte-identity gate: the two legs run different discs (PAL port, NTSC oracle), different renderers and different
movie configurations, so the frame hashes never match bit for bit; the tool says so, then falls back to the perceptual
descriptor (region luminance, mean colour, clear share, dHash) and reports what differs, where, by how much.

  diff.py --padlog fixtures/padlogs/hub_entry.padlog --out /abs/OUT [--iso PAL.iso] [--oracle-iso NTSC.iso]
          [--port-bin build/twinsanityevolved] [--oracle-bin int_work/twinsanityevolved/oracle/nativeee.ff178f8e]
          [--port-mc CARD_DIR] [--oracle-mc CARD_DIR] [--port-args "..."] [--oracle-args "..."] [--skip-oracle-presses N]
          [--oracle-padlog X.oracle.padlog] [--frames N] [--oracle-frames M] [--stride 100]
          [--reuse-port LEG_DIR] [--reuse-oracle LEG_DIR --oracle-frames-dir DIR] [--attributions TSV]
          [--no-flip-oracle] [--tol-lum 10 --tol-rgb 12 --tol-dhash 16 --tol-clear 0.15 --tol-timing 6] [--force]

Legs. The port runs `--harness-out` (platform/nativeee/host/te_harness.cpp): frames.fhash / audio.ahash / run.meta
through the engine's own writers, plus state.trace (GameController::State per change) and frames.pdesc (a perceptual
descriptor per presented frame). The oracle runs `--harness-out --dump-frames` (its PPMs are bottom-up; flipped here) with
the transposed pad log (fixtures/padlogs/<stem>.oracle.padlog: `A-B:BUTTON`, frame clock, every press at the port's
offset from START). One game process at a time (pgrep'd); the oracle's dumps are thinned to --stride while it runs.

Alignment ("game-state trace anchors"). The oracle does not expose the game state word (--watch-word is a diagnostics-
build knob), so the anchors are the events both legs show: (1) the pad presses (the k-th press of each pad log), (2) the
frame-hash plateaus (runs of >= 20 identical presented frames: the static card check, the menu pages, the black of a
load) matched in order by length; each matched plateau gives a start and an end anchor. The port->oracle offset is
piecewise constant between anchors; the port's state.trace names the segments. alignment.tsv lists every anchor pair
and every segment with the anchor it took; timeline rows flag a port state change whose oracle image event (the nearest
hash change) is further than --tol-timing frames from the mapped frame.

Comparison. Per segment: exact hash equality (counted, never gated), the perceptual descriptor on every oracle frame
dump (stride) against the port's frames.pdesc row at the mapped key, the audio lane per 100 ms window (silent /
non-silent pattern with +-1 window slack, exact equality counted), and the census (run.meta rows both legs carry). Every
row over tolerance is one divergence with a stable id (`<State>@<port_start>:<metric>`) and an EMPTY attribution slot;
--attributions TSV (`id<TAB>attribution<TAB>note`, attribution in {port bug, oracle bug, acceptable}) fills it and the
report fails (exit 3) while any divergence is unattributed. Outputs under --out: alignment.tsv, divergences.tsv,
census.tsv, DIVERGENCES.md, pairs/<state>_<port>_<oracle>.png (side by side, when the port dumped frames).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdesc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# The frozen oracle: the engine's `nativeee` title binary built from NativeEE ff178f8e (the T0-inside-the-boundary fix: no
# 1174-unit leap frames, oracle_clock_judge GREEN; + the --dump-frames readbacks on the GS consumer thread, so --load-state +
# --dump-frames legs run) with its `title/` tables beside it. The earlier 2b14dc5b recomp build (int_work/m_series/cutover/
# allon_noexc/bin/) leaps twice every 512 boundaries, and both it and 11c3a55e SIGSEGV under --load-state + --dump-frames.
# the reference binary and the two disc images: arguments, or these environment variables
DEFAULT_ORACLE = os.environ.get("TE_ORACLE_BIN", "")
DEFAULT_PAL = os.environ.get("TE_PAL_ISO", "")
DEFAULT_NTSC = os.environ.get("TE_NTSC_ISO", "")
ATTRIBUTIONS = ("port bug", "oracle bug", "acceptable")
PLATEAU_MIN = 20
WINDOW_TICKS = 6  # the engine's AudioHasher: 100 ms = 6 ticks at 60 Hz (both legs run 60 Hz)


# ----------------------------------------------------------------------------- readers
def read_lines(path):
    with open(path) as f:
        return [l.rstrip("\n") for l in f]


def read_fhash(path):
    rows = []
    for l in read_lines(path):
        l = l.strip()
        if not l or l.startswith("#"):
            continue
        k, h = l.split(",")[:2]
        rows.append((int(k), int(h, 16)))
    return rows


def read_ahash(path):
    out = {}
    for l in read_lines(path):
        l = l.strip()
        if not l or l.startswith("#"):
            continue
        k, h = l.split(",")[:2]
        out[int(k)] = int(h, 16)
    return out


def read_meta(path):
    out = {}
    for l in read_lines(path):
        if "=" in l and not l.startswith("#"):
            k, v = l.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def meta_field(value, name):
    """`a=1,b=2` -> the value of b; None when absent."""
    for part in value.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            if k == name:
                return v
    return None


def read_pdesc(path):
    out = {}
    for l in read_lines(path):
        row = pdesc.parse_row(l)
        if row:
            out[row["key"]] = row
    return out


def read_state_trace(path):
    out = []
    for l in read_lines(path):
        l = l.strip()
        if not l or l.startswith("#"):
            continue
        b, s, name = l.split(",")[:3]
        out.append((int(b), int(s), name))
    return out


def parse_port_padlog(path):
    """-> (presses [(boundary, name)], end boundary or None). A press = a sample whose buttons word is non-zero."""
    presses, end = [], None
    for l in read_lines(path):
        m = re.match(r"^#\s*end at boundary\s+(\d+)", l)
        if m:
            end = int(m.group(1))
        s = l.split("#")[0].split()
        if len(s) >= 2 and s[0].isdigit():
            if int(s[1], 16) != 0:
                name = l.split("#", 1)[1].strip() if "#" in l else s[1]
                presses.append((int(s[0]), name))
    return presses, end


def parse_oracle_padlog(path):
    presses = []
    for l in read_lines(path):
        l = l.split("#")[0].strip()
        m = re.match(r"^(\d+)(?:-(\d+))?:(\S+)", l)
        if m:
            presses.append((int(m.group(1)), m.group(3)))
    return presses


# ----------------------------------------------------------------------------- legs
def game_processes():
    try:
        out = subprocess.run(["pgrep", "-fl", "ps2recomp_release|twinsanityevolved|nativeee"], capture_output=True, text=True).stdout
    except OSError:
        return []
    found = []
    for l in out.splitlines():
        parts = l.split(None, 2)
        if len(parts) < 2:
            continue
        exe = os.path.basename(parts[1])
        if exe.startswith("ps2recomp_release") or exe == "twinsanityevolved" or exe.startswith("nativeee"):
            found.append(l)
    return found


def free_gb(path):
    return shutil.disk_usage(path).free / float(1 << 30)


def guard(out, force):
    procs = game_processes()
    if procs and not force:
        sys.exit("diff.py: another game process is running (one at a time): " + "; ".join(procs))
    min_free = float(os.environ.get("TE_MIN_FREE_GB", "15"))
    if free_gb(out) < min_free:
        sys.exit("diff.py: %.1f GB free under %s < TE_MIN_FREE_GB=%.0f -- prune first" % (free_gb(out), out, min_free))


class Thinner(threading.Thread):
    """Deletes the oracle's per-frame dumps the comparison does not keep (f<N>.ppm with N % stride != 0, every
    f<N>_<variant>.ppm and every per-present p<N>.ppm) while the oracle runs: 5000 frames x 3 x 700 KB would not fit."""

    def __init__(self, directory, stride):
        super().__init__(daemon=True)
        self.directory, self.stride, self.stop = directory, stride, threading.Event()
        self.deleted = 0

    def sweep(self):
        try:
            names = os.listdir(self.directory)
        except OSError:
            return
        for n in names:
            m = re.match(r"^([fp])(\d+)(_.*)?\.ppm$", n)
            if not m:
                continue
            # keep only f<key>.ppm at the stride: the variants (f<key>_drawn_*.ppm) and the per-present p<N>.ppm go
            if m.group(1) == "p" or m.group(3) or int(m.group(2)) % self.stride != 0:
                try:
                    os.remove(os.path.join(self.directory, n))
                    self.deleted += 1
                except OSError:
                    pass

    def run(self):
        while not self.stop.is_set():
            self.sweep()
            self.stop.wait(0.5)
        self.sweep()


def run_logged(cmd, log_path, env=None):
    with open(log_path, "w") as log:
        log.write("$ " + " ".join(cmd) + "\n")
        log.flush()
        t0 = time.time()
        p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        log.write("\n[diff.py] exit %d after %.1f s\n" % (p.returncode, time.time() - t0))
    return p.returncode


def seed_card(src, mc):
    """A prepared memory-card directory (the hub save, or tools/warp_save.py's warped card) copied into the leg's --mc-dir:
    a pad log that starts with LOAD GAME (load_game, the m3 Cavern warp logs) needs it on both legs."""
    if not src:
        return
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        dst = os.path.join(mc, rel) if rel != "." else mc
        os.makedirs(dst, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(dst, f))


def run_port(a, out):
    port = os.path.join(out, "port")
    leg, frames, mc = os.path.join(port, "leg"), os.path.join(port, "frames"), os.path.join(port, "mc")
    for d in (frames, mc):
        os.makedirs(d, exist_ok=True)
    seed_card(a.port_mc, mc)
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DUMP_DIR=frames, TE_GFX_DUMP_EVERY=str(a.stride))
    cmd = [a.port_bin, "--iso", a.iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
           "--play-pad", a.padlog, "--stop-at", str(a.frames), "--mc-dir", mc, "--harness-out", leg] + (a.port_args or "").split()
    rc = run_logged(cmd, os.path.join(port, "run.log"), env)
    print("[diff.py] port leg: exit %d -> %s" % (rc, leg))
    return leg, frames


def run_oracle(a, out):
    oracle = os.path.join(out, "oracle")
    leg, frames, mc = os.path.join(oracle, "leg"), os.path.join(oracle, "frames"), os.path.join(oracle, "mc")
    for d in (frames, mc):
        os.makedirs(d, exist_ok=True)
    seed_card(a.oracle_mc, mc)
    cmd = [a.oracle_bin, "--profile", "deterministic", "--headless", "--frames", str(a.oracle_frames), "--inputs", a.oracle_padlog,
           "--iso", a.oracle_iso, "--set", "paths.mc_dir=" + mc, "--harness-out", leg, "--dump-frames", frames, "--log-level", "warn"] \
        + (a.oracle_args or "").split()
    thinner = Thinner(frames, a.stride)
    thinner.start()
    rc = run_logged(cmd, os.path.join(oracle, "run.log"))
    thinner.stop.set()
    thinner.join()
    print("[diff.py] oracle leg: exit %d -> %s (%d dumps thinned)" % (rc, leg, thinner.deleted))
    return leg, frames


# ----------------------------------------------------------------------------- oracle frames -> pdesc
def flip_rows(w, h, px, channels=3):
    stride = w * channels
    return b"".join(px[(h - 1 - y) * stride:(h - y) * stride] for y in range(h))


def oracle_pdesc(frames_dir, flip, cache_path):
    rows = read_pdesc(cache_path) if os.path.exists(cache_path) else {}
    names = sorted((pdesc.frame_key(n), n) for n in os.listdir(frames_dir) if re.match(r"^f\d+\.ppm$", n))
    fresh = []
    for key, n in names:
        if key in rows:
            continue
        w, h, px = pdesc.read_ppm(os.path.join(frames_dir, n))
        if flip:
            px = flip_rows(w, h, px)
        line = "%d,%s" % (key, pdesc.describe(w, h, px, 3))
        fresh.append(line)
        rows[key] = pdesc.parse_row(line)
    if fresh:
        with open(cache_path, "a") as f:
            f.write("# oracle frame dumps described by tools/oracle/pdesc.py (rows flipped=%d)\n" % int(flip))
            f.write("\n".join(fresh) + "\n")
    return rows


# ----------------------------------------------------------------------------- alignment
def plateaus(rows, minlen=PLATEAU_MIN):
    """[(start_key, end_key, hash)] runs of >= minlen identical consecutive hashes (the leg's keys are consecutive)."""
    out = []
    if not rows:
        return out
    start, cur, prev = rows[0][0], rows[0][1], rows[0][0]
    for k, h in rows[1:]:
        if h != cur:
            if prev - start + 1 >= minlen:
                out.append((start, prev, cur))
            start, cur = k, h
        prev = k
    if prev - start + 1 >= minlen:
        out.append((start, prev, cur))
    return out


def match_presses(port_presses, oracle_presses):
    pairs, j = [], 0
    for b, name in port_presses:
        while j < len(oracle_presses) and oracle_presses[j][1] != name:
            j += 1
        if j >= len(oracle_presses):
            break
        pairs.append((b, oracle_presses[j][0], "press " + name))
        j += 1
    return pairs


def match_plateaus(port_pl, oracle_pl, press_pairs, max_jump=150):
    """Greedy in-order matching by length (+-max(15, 20 %)) with the implied offset within max_jump of the running offset."""
    pairs = []
    offset = None  # unconstrained until an anchor precedes the plateau (the boot movies shift the oracle before START)
    j = 0
    for ps, pe, _ in port_pl:
        plen = pe - ps + 1
        # the running offset follows the presses: the last press pair before this plateau
        for pb, ob, _ in press_pairs:
            if pb <= ps:
                offset = ob - pb
        k = j
        while k < len(oracle_pl):
            os_, oe, _ = oracle_pl[k]
            olen = oe - os_ + 1
            if abs(olen - plen) <= max(15, 0.2 * plen) and (offset is None or abs((os_ - ps) - offset) <= max_jump):
                pairs.append((ps, os_, "plateau start (%d/%d frames)" % (plen, olen)))
                pairs.append((pe + 1, oe + 1, "plateau end (%d/%d frames)" % (plen, olen)))
                offset = oe + 1 - (pe + 1)
                j = k + 1
                break
            k += 1
    return pairs


LOOKAHEAD = 3  # the state word changes the boundary before the image does: an anchor up to 3 frames ahead already applies


def anchor_for(anchors, b):
    """anchors sorted by port frame: the last anchor at or before b + LOOKAHEAD (the first anchor before any)."""
    best = anchors[0]
    for an in anchors:
        if an[0] <= b + LOOKAHEAD:
            best = an
        else:
            break
    return best


def offset_at(anchors, b):
    an = anchor_for(anchors, b)
    return an[1] - an[0]


MOVIE_STATES = ("VivendiLogo", "TravellersTalesLogo", "Movie")


def segment_anchor(anchors, b, e):
    """The anchors that pin a state segment [b, e]: (internal anchors, the anchor the segment's offset comes from).
    An anchor inside the segment (+-LOOKAHEAD) is the truth for it (the state's own timing); without one, the NEXT
    anchor (a load's end IS the next segment's start: the loading screen is pinned by when the title appears); failing
    that, the last anchor before."""
    internal = [an for an in anchors if b - LOOKAHEAD <= an[0] <= e + LOOKAHEAD]
    if internal:
        return internal, internal[0]
    nxt = [an for an in anchors if an[0] > e + LOOKAHEAD]
    if nxt:
        return [], nxt[0]
    return [], [an for an in anchors if an[0] < b - LOOKAHEAD][-1]


def frame_offset(internal, fallback, k):
    off = fallback[1] - fallback[0]
    if internal:
        off = internal[0][1] - internal[0][0]
        for p, o, _ in internal:
            if p <= k + LOOKAHEAD:
                off = o - p
    return off


def nearest_hash_change(rows_by_key, key, window=120):
    """The nearest key in [key-window, key+window] whose hash differs from its predecessor's (an image event); None."""
    best = None
    for k in range(key - window, key + window + 1):
        if k in rows_by_key and (k - 1) in rows_by_key and rows_by_key[k] != rows_by_key[k - 1]:
            if best is None or abs(k - key) < abs(best - key):
                best = k
    return best


# ----------------------------------------------------------------------------- audio
def silence_hash(ahash):
    """The engine's AudioHasher hashes a silent window to one constant: the most frequent value of the lane."""
    if not ahash:
        return None
    counts = {}
    for h in ahash.values():
        counts[h] = counts.get(h, 0) + 1
    return max(counts.items(), key=lambda t: t[1])[0]


def first_nonsilent(ahash, silence):
    for w in sorted(ahash):
        if ahash[w] != silence:
            return w
    return None


def selftest():
    rows = [(k, 1 if 10 <= k < 40 else (2 if 60 <= k < 90 else k * 7919)) for k in range(1, 121)]
    pl = plateaus(rows)
    assert pl == [(10, 39, 1), (60, 89, 2)], pl
    o_rows = [(k, 1 if 110 <= k < 141 else (2 if 160 <= k < 192 else k * 104729)) for k in range(1, 300)]
    o_pl = plateaus(o_rows)
    press = match_presses([(50, "START"), (95, "CROSS")], [(150, "START"), (195, "CROSS"), (250, "UP")])
    assert press == [(50, 150, "press START"), (95, 195, "press CROSS")], press
    pairs = match_plateaus(pl, o_pl, press)
    assert [(p, o) for p, o, _ in pairs] == [(10, 110), (40, 141), (60, 160), (90, 192)], pairs
    anchors = sorted(press + pairs)
    internal, an = segment_anchor(anchors, 1, 5)        # before every anchor: the next one (a load's end)
    assert not internal and an[0] == 10, an
    internal, an = segment_anchor(anchors, 95, 120)     # none inside (press 95 is at the edge: inside); shrink
    internal, an = segment_anchor(anchors, 99, 120)     # none inside, none after: the last before
    assert not internal and an[0] == 95, an
    internal, an = segment_anchor(anchors, 45, 100)     # inside: piecewise, the first internal before it
    assert internal[0][0] == 50 and frame_offset(internal, an, 45) == 100 and frame_offset(internal, an, 91) == 102 and frame_offset(internal, an, 92) == 100, internal
    assert offset_at(anchors, 1) == 100 and offset_at(anchors, 39) == 101 and offset_at(anchors, 200) == 100
    assert silence_hash({0: 5, 1: 5, 2: 7}) == 5 and first_nonsilent({0: 5, 1: 5, 2: 7}, 5) == 2
    assert hamming(0b1011, 0b0001) == 2
    print("diff.py selftest OK")
    return 0


# ----------------------------------------------------------------------------- comparison
def hamming(a, b):
    return bin(a ^ b).count("1")


def perceptual_delta(p, o):
    return {"lum_sky": o["sky"] - p["sky"], "lum_mid": o["mid"] - p["mid"], "lum_ground": o["ground"] - p["ground"],
            "mean_r": o["r"] - p["r"], "mean_g": o["g"] - p["g"], "mean_b": o["b"] - p["b"],
            "clear_share": o["clear_share"] - p["clear_share"], "dhash": hamming(p["dhash"], o["dhash"])}


def over_tolerance(d, a):
    bad = []
    for m in ("lum_sky", "lum_mid", "lum_ground"):
        if abs(d[m]) > a.tol_lum:
            bad.append(m)
    for m in ("mean_r", "mean_g", "mean_b"):
        if abs(d[m]) > a.tol_rgb:
            bad.append(m)
    if abs(d["clear_share"]) > a.tol_clear:
        bad.append("clear_share")
    if d["dhash"] > a.tol_dhash:
        bad.append("dhash")
    return bad


def fmt(v):
    return "%+.1f" % v if isinstance(v, float) else str(v)


def write_png_pair(port_ppm, oracle_ppm, flip, out_png):
    try:
        from PIL import Image
    except ImportError:
        return False
    a = Image.open(port_ppm).convert("RGB")
    b = Image.open(oracle_ppm).convert("RGB")
    if flip:
        b = b.transpose(Image.FLIP_TOP_BOTTOM)
    h = max(a.height, b.height)
    canvas = Image.new("RGB", (a.width + b.width + 8, h), (40, 40, 40))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (a.width + 8, 0))
    canvas.save(out_png)
    return True


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--padlog", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--oracle-padlog")
    ap.add_argument("--port-bin", default=os.path.join(ROOT, "build", "twinsanityevolved"))
    ap.add_argument("--oracle-bin", default=DEFAULT_ORACLE)
    ap.add_argument("--iso", default=DEFAULT_PAL)
    ap.add_argument("--oracle-iso", default=DEFAULT_NTSC)
    ap.add_argument("--frames", type=int, help="port --stop-at (default: the pad log's `# end at boundary N`)")
    ap.add_argument("--oracle-frames", type=int, help="oracle --frames (default: port frames + first-press offset + 100)")
    ap.add_argument("--stride", type=int, default=100, help="frame dumps kept every N presents (both legs)")
    ap.add_argument("--reuse-port", help="an existing port leg dir (skip the run)")
    ap.add_argument("--port-frames-dir", help="the port's frame dumps for --reuse-port (frame_%%06d.ppm, present-indexed)")
    ap.add_argument("--reuse-oracle", help="an existing oracle leg dir (skip the run)")
    ap.add_argument("--oracle-frames-dir", help="the oracle's frame dumps for --reuse-oracle (f<key>.ppm)")
    ap.add_argument("--attributions", help="TSV id<TAB>attribution<TAB>note")
    ap.add_argument("--port-mc", "--port-mc-from", dest="port_mc", help="a prepared card dir copied into the port leg's --mc-dir (LOAD GAME logs)")
    ap.add_argument("--oracle-mc", "--oracle-mc-from", dest="oracle_mc", help="a prepared card dir copied into the oracle leg's paths.mc_dir (LOAD GAME logs)")
    ap.add_argument("--port-args", default="", help="extra runner args for the port leg (e.g. '--load-state S.ps2st')")
    ap.add_argument("--oracle-args", default="", help="extra args for the oracle leg")
    ap.add_argument("--skip-oracle-presses", type=int, default=0,
                    help="ignore the oracle pad log's first N presses for the press anchors (a port log replayed from a state: its oracle log carries the prefix that reaches that state)")
    ap.add_argument("--no-flip-oracle", action="store_true", help="the oracle's PPMs are top-down (default: bottom-up, flipped)")
    ap.add_argument("--tol-lum", type=float, default=10.0)
    ap.add_argument("--tol-rgb", type=float, default=12.0)
    ap.add_argument("--tol-dhash", type=int, default=16)
    ap.add_argument("--tol-clear", type=float, default=0.15)
    ap.add_argument("--tol-timing", type=int, default=6)
    ap.add_argument("--pairs-per-segment", type=int, default=1, help="side-by-side PNGs written per state segment")
    ap.add_argument("--force", action="store_true", help="run even when another game process is up")
    ap.add_argument("--selftest", action="store_true")
    if "--selftest" in argv:
        return selftest()
    a = ap.parse_args(argv[1:])

    a.padlog = os.path.abspath(a.padlog)
    if not a.oracle_padlog:
        a.oracle_padlog = re.sub(r"\.padlog$", ".oracle.padlog", a.padlog)
    a.oracle_padlog = os.path.abspath(a.oracle_padlog)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    flip = not a.no_flip_oracle

    port_presses, end = parse_port_padlog(a.padlog)
    oracle_presses = parse_oracle_padlog(a.oracle_padlog)[a.skip_oracle_presses:]
    if not a.frames:
        a.frames = end or 4200
    press_pairs = match_presses(port_presses, oracle_presses)
    if not a.oracle_frames:
        first = press_pairs[0][1] - press_pairs[0][0] if press_pairs else 0
        a.oracle_frames = a.frames + first + 100

    # ---- legs
    if a.reuse_port:
        port_leg = os.path.abspath(a.reuse_port)
        port_frames = a.port_frames_dir or os.path.join(os.path.dirname(port_leg), "frames")   # diff.py's own layout
    else:
        guard(out, a.force)
        port_leg, port_frames = run_port(a, out)
    if a.reuse_oracle:
        oracle_leg = os.path.abspath(a.reuse_oracle)
        oracle_frames = a.oracle_frames_dir or os.path.join(os.path.dirname(oracle_leg), "frames")
    else:
        guard(out, a.force)
        oracle_leg, oracle_frames = run_oracle(a, out)
    for need in ("frames.fhash", "audio.ahash", "run.meta"):
        for leg in (port_leg, oracle_leg):
            if not os.path.exists(os.path.join(leg, need)):
                sys.exit("diff.py: %s has no %s" % (leg, need))

    port_f = read_fhash(os.path.join(port_leg, "frames.fhash"))
    oracle_f = read_fhash(os.path.join(oracle_leg, "frames.fhash"))
    port_by, oracle_by = dict(port_f), dict(oracle_f)
    port_pd = read_pdesc(os.path.join(port_leg, "frames.pdesc"))
    trace = read_state_trace(os.path.join(port_leg, "state.trace")) if os.path.exists(os.path.join(port_leg, "state.trace")) else []
    port_a, oracle_a = read_ahash(os.path.join(port_leg, "audio.ahash")), read_ahash(os.path.join(oracle_leg, "audio.ahash"))
    port_m, oracle_m = read_meta(os.path.join(port_leg, "run.meta")), read_meta(os.path.join(oracle_leg, "run.meta"))
    oracle_pd = {}
    if oracle_frames and os.path.isdir(oracle_frames):
        oracle_pd = oracle_pdesc(oracle_frames, flip, os.path.join(out, "oracle_frames.pdesc"))

    # ---- alignment
    port_pl, oracle_pl = plateaus(port_f), plateaus(oracle_f)
    # a port plateau overlapping a movie state while the port skips movies is the skip's black: the oracle plays the
    # movie there, so that plateau has no counterpart and must not be matched
    movie_spans = []
    if port_m.get("skip_movies") == "on":
        for i, (b, _, name) in enumerate(trace):
            e = trace[i + 1][0] - 1 if i + 1 < len(trace) else port_f[-1][0]
            if name in MOVIE_STATES:
                movie_spans.append((b, e))
    matchable = [pl for pl in port_pl if not any(min(pl[1], me) - max(pl[0], mb) + 1 > LOOKAHEAD for mb, me in movie_spans)]
    anchors = press_pairs + match_plateaus(matchable, oracle_pl, press_pairs)
    # the audio onset: the first non-silent 100 ms window of each leg (the title music starts with the title on both;
    # the only event that pins the oracle's Title when the transposed pad log waits longer before START)
    silence = silence_hash(port_a)
    onset = [first_nonsilent(port_a, silence), first_nonsilent(oracle_a, silence)]
    if None not in onset:
        anchors.append((onset[0] * WINDOW_TICKS + 1, onset[1] * WINDOW_TICKS + 1, "audio onset (windows %d/%d)" % tuple(onset)))
    anchors.sort(key=lambda t: t[0])
    if not anchors:
        sys.exit("diff.py: no anchors (no press pairs, no matched plateaus)")
    segments = []  # (state_name, port_start, port_end, offset, anchor, internal anchors)
    bounds = [(b, name) for b, _, name in trace] or [(port_f[0][0], "run")]
    fwd = {}  # port key -> oracle key, per segment (piecewise through the segment's internal anchors)
    for i, (b, name) in enumerate(bounds):
        e = bounds[i + 1][0] - 1 if i + 1 < len(bounds) else port_f[-1][0]
        if e < b:
            continue
        internal, an = segment_anchor(anchors, b, e)
        segments.append((name, b, e, frame_offset(internal, an, b), an, internal))
        for k in range(b, e + 1):
            fwd[k] = k + frame_offset(internal, an, k)
    for k, _ in port_f:
        fwd.setdefault(k, k + offset_at(anchors, k))
    with open(os.path.join(out, "alignment.tsv"), "w") as f:
        f.write("# anchors: port_frame\toracle_frame\toffset\tkind\n")
        for p, o, kind in anchors:
            f.write("anchor\t%d\t%d\t%+d\t%s\n" % (p, o, o - p, kind))
        f.write("# plateaus (unmatched included): leg\tstart\tend\tlen\thash\n")
        for ps, pe, h in port_pl:
            f.write("plateau\tport\t%d\t%d\t%d\t0x%016x\n" % (ps, pe, pe - ps + 1, h))
        for os_, oe, h in oracle_pl:
            f.write("plateau\toracle\t%d\t%d\t%d\t0x%016x\n" % (os_, oe, oe - os_ + 1, h))
        f.write("# segments: state\tport_start\tport_end\toracle_start\toffset\tanchor\n")
        for name, b, e, off, an, internal in segments:
            f.write("segment\t%s\t%d\t%d\t%d\t%+d\t%s@%d%s\n" % (name, b, e, b + off, off, an[2], an[0], "" if internal else " (outside the segment)"))

    # ---- divergences
    div = []  # dicts: id, segment, port_frames, oracle_frames, lane, metric, value, detail

    def add(seg_name, seg_b, lane, metric, pf, of, value, detail):
        div.append({"id": "%s@%d:%s" % (seg_name, seg_b, metric), "segment": seg_name, "port_frames": pf, "oracle_frames": of,
                    "lane": lane, "metric": metric, "value": value, "detail": detail})

    # timeline: every port state change vs the oracle's nearest image event around the mapped frame
    timeline = []
    for i, (b, _, name) in enumerate(trace):
        mapped = fwd.get(b, b + offset_at(anchors, b))
        ev = nearest_hash_change(oracle_by, mapped)
        delta = (ev - mapped) if ev is not None else None
        timeline.append((name, b, mapped, ev, delta))
        if delta is None or abs(delta) > a.tol_timing:
            add(name, b, "timeline", "timing", str(b), "%d (event %s)" % (mapped, ev), "%s frames" % (fmt(delta) if delta is not None else "none"),
                "the oracle's nearest presented-image change is %s frames from the mapped frame (tol %d)" % (delta, a.tol_timing))

    # frames: exact + perceptual per segment; the port->oracle map is per frame (the offset follows the anchors)
    inv = {}
    for k in sorted(fwd):
        inv.setdefault(fwd[k], k)
    exact_total = exact_eq = 0
    samples = []  # (seg, port_key, oracle_key, deltas, bad)
    for name, b, e, off, an, internal in segments:
        eq = tot = 0
        for k in range(b, e + 1):
            if k in port_by and fwd.get(k) in oracle_by:
                tot += 1
                eq += port_by[k] == oracle_by[fwd[k]]
        exact_total += tot
        exact_eq += eq
        per_metric = {}
        n = 0
        failing = set()
        for ok in sorted(oracle_pd):
            pk = inv.get(ok)
            if pk is not None and b <= pk <= e and pk in port_pd:
                d = perceptual_delta(port_pd[pk], oracle_pd[ok])
                bad = over_tolerance(d, a)
                samples.append((name, pk, ok, d, bad))
                n += 1
                if bad:
                    failing.add((pk, ok))
                for m in bad:
                    per_metric.setdefault(m, []).append(d[m])
        if per_metric:
            parts = []
            for m in ("lum_sky", "lum_mid", "lum_ground", "mean_r", "mean_g", "mean_b", "clear_share", "dhash"):
                if m in per_metric:
                    vals = per_metric[m]
                    parts.append("%s %s..%s (%d/%d)" % (m, fmt(min(vals)), fmt(max(vals)), len(vals), n))
            add(name, b, "frames", "perceptual", "%d..%d" % (b, e), "%d..%d" % (fwd.get(b, b + off), fwd.get(e, e + off)),
                "%d of %d samples over tolerance" % (len(failing), n),
                "oracle minus port: " + "; ".join(parts) + "; samples " + " ".join("%d/%d" % pk_ok for pk_ok in sorted(failing)[:6]) + (" ..." if len(failing) > 6 else ""))

    # audio per 100 ms window: silent/non-silent pattern per segment, +-1 window slack; exact equality counted
    audio_rows = []
    for name, b, e, off, an, internal in segments:
        w0, w1 = (b - 1) // WINDOW_TICKS, (e - 1) // WINDOW_TICKS
        woff = int(round((fwd.get(e, e + off) - e) / float(WINDOW_TICKS)))  # the offset at the segment's end
        n = agree = eq = ps = os_ = 0
        first_p = first_o = None
        for w in range(w0, w1 + 1):
            if w not in port_a or (w + woff) not in oracle_a:
                continue
            n += 1
            p_sil = port_a[w] == silence
            ps += p_sil
            o_sil = oracle_a[w + woff] == silence
            os_ += o_sil
            eq += port_a[w] == oracle_a[w + woff]
            if not p_sil and first_p is None:
                first_p = w
            if not o_sil and first_o is None:
                first_o = w + woff
            near = any(((w + woff + d) in oracle_a and (oracle_a[w + woff + d] == silence) == p_sil) for d in (-1, 0, 1))
            agree += near
        if n:
            audio_rows.append((name, b, e, n, ps, os_, agree, eq, first_p, first_o))
            if agree < 0.8 * n:
                add(name, b, "audio", "silence_pattern", "windows %d..%d" % (w0, w1), "windows %d..%d" % (w0 + woff, w1 + woff),
                    "%d of %d windows agree" % (agree, n),
                    "port silent %d, oracle silent %d; first non-silent window port %s oracle %s" % (ps, os_, first_p, first_o))

    # census: the rows both legs carry (curated aspects + every raw key both have)
    def port_clock(field):
        return meta_field(port_m.get("clock", ""), field)

    census = [
        ("profile", port_m.get("profile"), oracle_m.get("profile")),
        ("finalized", port_m.get("finalized"), oracle_m.get("finalized")),
        ("boundaries", port_m.get("boundaries"), oracle_m.get("frames")),
        ("presents", port_m.get("presents"), oracle_m.get("presents")),
        ("frame hash rows", str(len(port_f)), str(len(oracle_f))),
        ("audio windows", str(len(port_a)), str(len(oracle_a))),
        ("audio silent windows", str(sum(1 for h in port_a.values() if h == silence)), str(sum(1 for h in oracle_a.values() if h == silence))),
        ("audio underruns", meta_field(port_m.get("audio_ring", ""), "underruns"), meta_field(oracle_m.get("audio_ring", ""), "underruns")),
        ("audio dropped", meta_field(port_m.get("audio_ring", ""), "dropped"), meta_field(oracle_m.get("audio_ring", ""), "dropped")),
        ("audio sink", "device=%s" % meta_field(port_m.get("audio_ring", ""), "device"), oracle_m.get("audio_sink")),
        ("pace late", port_clock("late"), oracle_m.get("pace_late")),
        ("pace timeouts", port_clock("timeouts"), oracle_m.get("pace_timeouts")),
        ("frame size", port_m.get("frame_size"), (oracle_pd and "%dx%d" % (next(iter(oracle_pd.values()))["w"], next(iter(oracle_pd.values()))["h"])) or None),
        ("movies", port_m.get("skip_movies") and "skip_movies=" + port_m["skip_movies"], "fmv pictures=%s" % meta_field(oracle_m.get("fmv", ""), "pictures")),
        ("disc", os.path.basename(port_m.get("disc", "")) or port_m.get("elf"), os.path.basename(a.oracle_iso)),
        ("build", port_m.get("build_id"), oracle_m.get("build_id")),
    ]
    sinks = (meta_field(port_m.get("audio_ring", ""), "device") not in (None, "0"), oracle_m.get("audio_sink") not in (None, "none"))

    def verdict_of(k, p, o):
        if k == "audio dropped" and not all(sinks):
            return "n/a (a leg has no audio sink: its ring drops by construction)"
        return "same" if (p is not None and p == o) else ("port-only" if o is None else ("oracle-only" if p is None else "differs"))

    with open(os.path.join(out, "census.tsv"), "w") as f:
        f.write("aspect\tport\toracle\tverdict\n")
        for k, p, o in census:
            f.write("%s\t%s\t%s\t%s\n" % (k, p, o, verdict_of(k, p, o)))
        shared = sorted(set(port_m) & set(oracle_m))
        f.write("# raw run.meta keys both legs carry: %d (equal %d); port-only %d, oracle-only %d\n" % (
            len(shared), sum(1 for k in shared if port_m[k] == oracle_m[k]), len(set(port_m) - set(oracle_m)), len(set(oracle_m) - set(port_m))))
        for k in shared:
            f.write("raw\t%s\t%s\t%s\t%s\n" % (k, port_m[k][:60], oracle_m[k][:60], "same" if port_m[k] == oracle_m[k] else "differs"))
    # a leg without an audio sink (the oracle headless: audio_sink=none; the port: device=0) has a ring nobody pulls, so
    # its `dropped` counts every pushed frame by construction -- comparable only when BOTH legs have a sink
    for k, p, o in census:
        if k in ("audio underruns", "pace timeouts") and p is not None and o is not None and p != o:
            add("census", 0, "census", k.replace(" ", "_"), "-", "-", "%s vs %s" % (p, o), "run.meta")
        if k == "audio dropped" and p != o and all(sinks):
            add("census", 0, "census", "audio_dropped", "-", "-", "%s vs %s" % (p, o), "run.meta audio_ring")

    # ---- attributions
    attributions = {}
    if a.attributions:
        for l in read_lines(a.attributions):
            if not l.strip() or l.startswith("#"):
                continue
            parts = l.split("\t")
            if len(parts) >= 2:
                attributions[parts[0].strip()] = (parts[1].strip(), parts[2].strip() if len(parts) > 2 else "")
    unattributed = bad_attr = 0
    for d in div:
        attr, note = attributions.get(d["id"], ("", ""))
        if not attr:
            unattributed += 1
        elif attr not in ATTRIBUTIONS:
            bad_attr += 1
        d["attribution"], d["note"] = attr, note

    # ---- outputs
    with open(os.path.join(out, "divergences.tsv"), "w") as f:
        f.write("id\tsegment\tport_frames\toracle_frames\tlane\tmetric\tvalue\tattribution\tnote\tdetail\n")
        for d in div:
            f.write("\t".join(str(d[k]) for k in ("id", "segment", "port_frames", "oracle_frames", "lane", "metric", "value", "attribution", "note", "detail")) + "\n")

    pairs_dir = os.path.join(out, "pairs")
    pair_pngs = []
    if port_frames and os.path.isdir(port_frames) and oracle_frames and os.path.isdir(oracle_frames):
        os.makedirs(pairs_dir, exist_ok=True)
        # the port's dumps are present-indexed (frame_%06d = the n-th present, every --stride); frames.fhash's n-th row is
        # that present's key. The oracle's dumps sit at keys that are multiples of the stride, which map between the port's
        # dumps: pair each oracle sample with the NEAREST dumped port frame inside the same segment (both keys in the name).
        dumped = {}   # port key -> ppm
        for n in os.listdir(port_frames):
            m = re.match(r"^frame_(\d+)\.ppm$", n)
            if m and int(m.group(1)) - 1 < len(port_f):
                dumped[port_f[int(m.group(1)) - 1][0]] = os.path.join(port_frames, n)
        for name, b, e, off, an, internal in segments:
            in_seg = sorted(k for k in dumped if b <= k <= e)
            picked = []
            for ok in sorted(oracle_pd):
                pk = inv.get(ok)
                if pk is None or not (b <= pk <= e) or not in_seg:
                    continue
                near = min(in_seg, key=lambda k: abs(k - pk))
                if abs(near - pk) <= a.stride // 2 + 1:
                    picked.append((near, ok, dumped[near], os.path.join(oracle_frames, "f%d.ppm" % ok)))
            # the first, then evenly spaced through the segment
            n = max(1, a.pairs_per_segment)
            chosen = picked[:1] + [picked[i * len(picked) // n] for i in range(1, n)] if picked else []
            for pk, ok, pp, op in dict((c[1], c) for c in chosen).values():
                # a reused oracle leg may have had its PPMs pruned (the pdesc cache carries the comparison): no pair then
                if not (os.path.exists(pp) and os.path.exists(op)):
                    continue
                png = os.path.join(pairs_dir, "%s_%d_%d.png" % (name, pk, ok))
                if write_png_pair(pp, op, flip, png):
                    pair_pngs.append((name, pk, ok, png))

    md = []
    md.append("# Oracle diff -- %s\n" % os.path.basename(a.padlog))
    md.append("Port leg `%s` (%s), oracle leg `%s` (%s). Oracle PPMs flipped: %s. Tolerances: lum %.0f, rgb %.0f, dHash %d bits, clear share %.2f, timing %d frames.\n" % (
        port_leg, port_m.get("build_id", "?"), oracle_leg, oracle_m.get("build_id", "?"), flip, a.tol_lum, a.tol_rgb, a.tol_dhash, a.tol_clear, a.tol_timing))
    md.append("## Alignment\n\nAnchors (port frame -> oracle frame, offset, kind):\n")
    md.append("| port | oracle | offset | kind |\n|---|---|---|---|")
    for p, o, kind in anchors:
        md.append("| %d | %d | %+d | %s |" % (p, o, o - p, kind))
    md.append("\nSegments (the port's state trace; offset from the anchor at or before the segment start):\n")
    md.append("| state | port frames | oracle frames | offset at start | anchor |\n|---|---|---|---|---|")
    for name, b, e, off, an, internal in segments:
        md.append("| %s | %d..%d | %d..%d | %+d | %s @%d%s |" % (name, b, e, fwd[b], fwd[e], off, an[2], an[0], "" if internal else " (outside)"))
    md.append("\nTimeline (port state change -> mapped oracle frame -> the oracle's nearest image event):\n")
    md.append("| state | port | mapped | oracle event | delta |\n|---|---|---|---|---|")
    for name, b, mapped, ev, delta in timeline:
        md.append("| %s | %d | %d | %s | %s |" % (name, b, mapped, ev, fmt(delta) if delta is not None else "none"))
    md.append("\n## Frames\n\nExact presented-frame hashes equal: %d of %d aligned pairs (different discs / renderers: counted, not gated).\n" % (exact_eq, exact_total))
    md.append("Perceptual samples (oracle dumps at stride %d; oracle minus port):\n" % a.stride)
    md.append("| state | port | oracle | sky | mid | ground | R | G | B | clear | dHash | over tolerance |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, pk, ok, d, bad in samples:
        md.append("| %s | %d | %d | %s | %s | %s | %s | %s | %s | %+.3f | %d | %s |" % (
            name, pk, ok, fmt(d["lum_sky"]), fmt(d["lum_mid"]), fmt(d["lum_ground"]), fmt(d["mean_r"]), fmt(d["mean_g"]), fmt(d["mean_b"]),
            d["clear_share"], d["dhash"], ", ".join(bad) or "-"))
    md.append("\n## Audio (100 ms windows; silent = the hasher's constant 0x%016x)\n" % (silence or 0))
    md.append("| state | port frames | windows | port silent | oracle silent | pattern agree (+-1) | exact equal | first non-silent port/oracle |\n|---|---|---|---|---|---|---|---|")
    for name, b, e, n, ps, os_, agree, eq, fp, fo in audio_rows:
        md.append("| %s | %d..%d | %d | %d | %d | %d | %d | %s / %s |" % (name, b, e, n, ps, os_, agree, eq, fp, fo))
    md.append("\n## Census\n\n| aspect | port | oracle | verdict |\n|---|---|---|---|")
    for k, p, o in census:
        md.append("| %s | %s | %s | %s |" % (k, p, o, verdict_of(k, p, o)))
    md.append("\n## Divergences (%d; unattributed %d)\n" % (len(div), unattributed))
    md.append("| id | lane | port frames | oracle frames | value | attribution | note |\n|---|---|---|---|---|---|---|")
    for d in div:
        md.append("| `%s` | %s | %s | %s | %s | **%s** | %s |" % (d["id"], d["lane"], d["port_frames"], d["oracle_frames"], d["value"], d["attribution"] or "UNATTRIBUTED", d["note"]))
    if pair_pngs:
        md.append("\n## Frame pairs (port left, oracle right)\n")
        for name, pk, ok, png in pair_pngs:
            md.append("- %s: port %d / oracle %d (the nearest dumped port frame to the mapped key) -- `%s`" % (name, pk, ok, png))
    with open(os.path.join(out, "DIVERGENCES.md"), "w") as f:
        f.write("\n".join(md) + "\n")

    print("[diff.py] anchors %d, segments %d, exact frames %d/%d, perceptual samples %d, divergences %d (unattributed %d, bad attribution %d)" % (
        len(anchors), len(segments), exact_eq, exact_total, len(samples), len(div), unattributed, bad_attr))
    print("[diff.py] -> %s" % os.path.join(out, "DIVERGENCES.md"))
    if bad_attr:
        return 4
    return 3 if unattributed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
