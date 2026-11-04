#!/usr/bin/env python3
"""tools/vita/readlog.py LOG [--tables NAMES] [--from P] [--to P] [--area NAME] [--ticks all|play]

A PS Vita log (ux0:data/twinsanity/twinsanityevolved.log, from the console or from Vita3K) as markdown tables. The census
comes in windows of TE_GFX_SUMMARY_EVERY presents (the built-in diagnosis set: 300); every row below is one window, named
by its presents. Lines printed between two windows' census (the slow ticks, the area changes, the audio's lines) belong to
the window whose census follows them: it is the window they happened in.

  windows   the pace (pictures a second, the longest gap), the hand-over, the threads' CPU milliseconds a present
  work      the GPU's milliseconds a present, the GXM draws (plain + instanced), the game seams and the game's own code,
            the tick work and the drawing thread's milliseconds a picture (p90 / max)
  seams     the game seams per present, group by group (milliseconds)
  profiles  the top sections of the game, drawing and build threads (their idle sections left out)
  bursts    what a burst of particles, sounds and pieces costs: particle blocks and triangles a present, the shadow
            builds, the vertex ring, the build pool's waits, the build threads' particle share, the voices playing and the
            mix, vitaGL's allocations
  textures  the textures made in the window, the textures loaded ahead of their use (made at their first draw, released
            before made, failed, pending), the textures alive
  io        the disc: card reads, the game's waits for them
  events    the area changes, the pauses (a player's mark: START), the lives kept, the slow-tick log's lines not logged
  ticks     every slow tick (TE_TICK_LOG_MS: "tick: boundary N took X ms") with its profile and its load work
  pictures  every slow picture of the drawing thread (TE_PICTURE_LOG_MS: "picture: present N took X ms") with its profile
  groups    the slow ticks grouped by what they were and the top section of their profile; the slow pictures by theirs;
            the ones a window's cap (TE_TICK_LOG_CAP / TE_PICTURE_LOG_CAP) did not print, as the window line counts them

--tables windows,ticks,... picks tables (default: all); --from / --to keep the windows whose presents end in [P, Q];
--area NAME keeps the windows that were in an area whose name contains NAME; --ticks play leaves out of the ticks table the
waits for the disc, a movie or the music (the loads' ticks; the groups table always has them all).
"""
import argparse
import os
import re
import sys

STAMP = re.compile(r"^\[(\d+\.\d+)\] ")
LABEL = re.compile(r"presents (\d+)-(\d+)")
IDLE = ("park (the pacer's edge)", "rt idle", "pool idle", "io idle", "movie thread idle", "audio: SDL + the device's buffer wait")
SEAM_GROUPS = ["rays", "collision", "boxes", "place", "skins", "joints", "matrices", "bounds", "shadows", "particles", "decals", "sky"]


def num(rx, s, group=1, cast=float):
    m = re.search(rx, s)
    if not m:
        return None
    try:
        return cast(m.group(group))
    except (TypeError, ValueError):
        return None


def short_area(path):
    if not path:
        return "-"
    return path.replace("/", "\\").rsplit("\\", 1)[-1]


def sections(text):
    """'(own code) 62% (31), seam: x (a, b) 20% (10)' or '... 36.0%, ...' -> [(name, pct)] (a name may hold a comma)"""
    return [(m.group(1).strip(), float(m.group(2)))
            for m in re.finditer(r"\s*(.+?) (\d+(?:\.\d+)?)%(?: \(\d+\))?(?:,|$)", text)]


class Window:
    def __init__(self, first, last):
        self.first, self.last = first, last
        self.end_s = None
        self.v = {}
        self.ticks = []
        self.events = []
        self.areas = []      # the areas the window was in (the first is the one it started in)
        self.audio = []      # the audio lines' (playing, mix ms, max ms)
        self.prof = {}       # thread -> [(section, pct)]

    def name(self):
        return f"{self.first}-{self.last}" if self.first else f"(after {self.last})"


class Tick:
    def __init__(self, boundary, ms, s, area, parts, load, prof, skip=False):
        self.boundary, self.ms, self.s, self.area = boundary, ms, s, area
        self.parts, self.load, self.prof = parts, load, prof
        self.skip = skip     # inside a cutscene's skip (the cutscene's remaining frames run at once)
        self.window = None

    def top(self):
        return self.prof[0][0] if self.prof else "(no profile)"

    def kind(self):
        """what the tick waited for or did, by its profile's top section and its load notes"""
        top = self.top()
        if self.skip:
            return "cutscene skip"
        disc = num(r"disc waits \d+ \(([\d.]+) ms\)", self.load or "")
        if top.startswith("disc:") or (disc and disc >= 0.5 * self.ms):
            return "disc wait"
        if top.startswith("movie:"):
            return "movie"
        if top.startswith("music:"):
            return "music"
        if top.startswith("model:") or top.startswith("texture:") or top.startswith("mesh wait"):
            return "load work"
        if top.startswith("present:"):
            return "hand-over"
        return "play"


class Picture:
    """a slow picture (TE_PICTURE_LOG_MS: "[te.gfx] picture: present N took X ms on the drawing thread (...)")"""
    def __init__(self, present, ms, gpu, draw, call, s, area, prof):
        self.present, self.ms, self.gpu, self.draw, self.call = present, ms, gpu, draw, call
        self.s, self.area, self.prof = s, area, prof
        self.window = None

    def top(self):
        return self.prof[0][0] if self.prof else "(no profile)"

    def kind(self):
        """a window's last picture carries the window's census lines (printed on the drawing thread at its present)"""
        return "census" if self.window is not None and self.present == self.window.last else "drawing"


class Log:
    def __init__(self, path):
        self.path = path
        self.head = {}
        self.windows = []
        self.ticks = []
        self.pictures = []
        self.events = []
        self.parse()

    def parse(self):
        area = ""
        cur = None           # the window whose census block is being read
        closed = False       # its block ended (the "[te.gfx] present=" line)
        pending_ticks, pending_events, pending_areas, pending_audio = [], [], [], []
        start_area = ""
        last_s = None
        skipping = False     # between "[te.cutscene] skipped:" and "[te.cutscene] skip over"
        with open(self.path, errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                m = STAMP.match(line)
                s = float(m.group(1)) if m else None
                body = line[m.end():] if m else line
                if s is not None:
                    last_s = s
                self.head_line(body)
                lab = LABEL.search(body)
                if lab and ("[te." in body[:20]):
                    first, last = int(lab.group(1)), int(lab.group(2))
                    if cur is None or cur.last != last:
                        closed = False
                        cur = Window(first, last)
                        cur.ticks, cur.events = pending_ticks, pending_events
                        cur.areas = [start_area] + [a for a in pending_areas if a != start_area]
                        cur.audio = pending_audio
                        for t in cur.ticks:
                            t.window = cur
                        self.windows.append(cur)
                        pending_ticks, pending_events, pending_areas, pending_audio = [], [], [], []
                        start_area = area
                    if body.startswith("[te.gfx] pace: presents"):
                        cur.first = first   # (the pace line's range is the window's; a seams line may span several)
                if body.startswith("[te.gfx] picture: present "):
                    m2 = re.match(r"\[te\.gfx\] picture: present (\d+) took ([\d.]+) ms on the drawing thread \(waiting for the GPU "
                                  r"([\d.]+), drawing ([\d.]+), the present call ([\d.]+)\)(?: prof: (.*))?$", body)
                    if m2:
                        self.pictures.append(Picture(int(m2.group(1)), float(m2.group(2)), float(m2.group(3)), float(m2.group(4)),
                                                     float(m2.group(5)), s, area, sections(m2.group(6) or "")))
                    continue
                if body.startswith("[te.cutscene] skipped:"):
                    skipping = True
                    ev = ("cutscene skipped", None, s, short_area(area))
                    self.events.append(ev)
                    pending_events.append(ev)
                    continue
                if body.startswith("[te.cutscene] skip over"):
                    skipping = False
                    continue
                if body.startswith("[twinsanityevolved] tick: boundary "):
                    t = self.tick(body, s, area)
                    if t:
                        t.skip = skipping
                        self.ticks.append(t)
                        pending_ticks.append(t)
                    continue
                if body.startswith("[twinsanityevolved] area: "):
                    m2 = re.match(r"\[twinsanityevolved\] area: '([^']*)' @ boundary (\d+)", body)
                    if m2:
                        area = m2.group(1)
                        ev = ("area", int(m2.group(2)), s, short_area(area) if area else "(between areas)")
                        self.events.append(ev)
                        pending_events.append(ev)
                        pending_areas.append(area)
                    continue
                if body.startswith("[twinsanityevolved] game state @ boundary "):
                    m2 = re.match(r"\[twinsanityevolved\] game state @ boundary (\d+): (\w+)", body)
                    if m2 and m2.group(2) == "Paused":
                        ev = ("pause", int(m2.group(1)), s, short_area(area))
                        self.events.append(ev)
                        pending_events.append(ev)
                    continue
                if body.startswith("[twinsanityevolved] infinite lives: a life kept"):
                    b = num(r"@ boundary (\d+)", body, cast=int)
                    ev = ("life kept", b, s, short_area(area))
                    self.events.append(ev)
                    pending_events.append(ev)
                    continue
                if body.startswith("[te.audio] mix per tick"):
                    pending_audio.append((num(r"playing ([\d.]+)", body), num(r"the mix ([\d.]+) ms a tick", body),
                                          num(r"the mix [\d.]+ ms a tick \(max ([\d.]+)\)", body)))
                    continue
                if cur is None or closed:
                    continue
                if s is not None:
                    cur.end_s = s
                self.census(cur, body)
                if body.startswith("[te.gfx] present="):
                    closed = True   # the block's last line: what follows happens in the next window
        if pending_ticks or pending_events:
            w = Window(0, self.windows[-1].last if self.windows else 0)
            w.ticks, w.events, w.areas, w.audio = pending_ticks, pending_events, [start_area] + pending_areas, pending_audio
            w.end_s = last_s
            for t in w.ticks:
                t.window = w
            self.windows.append(w)
        self.end_s = last_s
        for pic in self.pictures:
            for w in self.windows:
                if w.first and w.first <= pic.present <= w.last:
                    pic.window = w
                    break
        prev = None
        for w in self.windows:
            c = w.v.get("cum")
            if c and prev:
                n = max(1, c["presents"] - prev["presents"])
                for k in ("particle_blocks", "particle_tris", "shadow_passes", "decal_quads", "skins"):
                    if c.get(k) is not None and prev.get(k) is not None:
                        w.v[k + "_pp"] = (c[k] - prev[k]) / n
                for k in ("dropped", "textures", "tex_failed"):
                    if c.get(k) is not None and prev.get(k) is not None:
                        w.v[k + "_w"] = c[k] - prev[k]
            if c:
                prev = c

    def head_line(self, body):
        h = self.head
        # the slow-line knobs of the run: the built-in set's or an args.txt's tokens
        for k in ("TE_TICK_LOG_MS", "TE_TICK_LOG_CAP", "TE_PICTURE_LOG_MS", "TE_PICTURE_LOG_CAP"):
            m = re.search(r"\b" + k + r"=([\d.]+)", body)
            if m and ("diagnosis set" in body or "from args.txt" in body):
                h[k] = m.group(1)
        if "diagnosis set (built in" in body:
            h["set"] = body.split(":", 2)[-1].strip()
        elif "] vita: arguments (" in body:
            h["args"] = body.split("):", 1)[-1].strip()
            h["args_from"] = num(r"arguments \(([^)]*)\)", body, cast=str)
        elif "module: the game's code is NATIVE ARM" in body:
            h["lane"] = "native"
        elif "module: memory" in body and "bound to MainMemory" in body:
            h["lane"] = "module"
        elif "vita: game rate" in body:
            h["hz"] = num(r"game rate (\d+) Hz", body, cast=int)
        elif body.startswith("[twinsanityevolved] disc: "):
            h["disc"] = body.split("disc: ", 1)[1].split(" (", 1)[0]
        elif "] vita: resolution" in body:
            h["resolution"] = num(r"resolution (\S+)", body, cast=str)
        elif "--stop-at: exit" in body or body.startswith("[twinsanityevolved] exit"):
            h["exit"] = body

    def tick(self, body, s, area):
        m = re.match(r"\[twinsanityevolved\] tick: boundary (\d+) took ([\d.]+) ms \(([^)]*)\)(.*)$", body)
        if not m:
            return None
        rest = m.group(4)
        prof = []
        if " prof: " in rest:
            rest, ptext = rest.split(" prof: ", 1)
            prof = sections(ptext)
        load = rest.split(" load: ", 1)[1].strip() if " load: " in rest else ""
        parts = {}
        for k, v in re.findall(r"(draw submit|present|pump|game) ([\d.]+)", m.group(3)):
            parts[k] = float(v)
        return Tick(int(m.group(1)), float(m.group(2)), s, area, parts, load, prof)

    def census(self, w, body):
        v = w.v
        if body.startswith("[te.gfx] pace: presents"):
            v["fps"] = num(r"at ([\d.]+) fps", body)
            v["gap"] = num(r"longest gap ([\d.]+) ms", body)
            v["frame_ms"] = num(r"a frame ([\d.]+) ms", body)
        elif body.startswith("[te.cpu] presents"):
            for name, key in (("te-game", "game"), ("te-render", "render"), ("te-build-0", "build0"), ("te-build-1", "build1"),
                              ("sdl-audio", "audio"), ("te-io", "io")):
                ms = num(re.escape(name) + r" [\d.]+% \(prio \d+, ([\d.]+) ms", body)
                if ms is not None:
                    v["cpu_" + key] = ms
            v["cores"] = num(r"the three cores ([\d.]+)% busy", body)
        elif body.startswith("[te.prof] presents") and "game seams per present" in body:
            for g in SEAM_GROUPS:
                v["seam_" + g] = num(r"\b" + g + r" [\d.]+ \(([\d.]+) ms\)", body)
            v["seams"] = num(r"the groups ([\d.]+) ms", body)
            v["own"] = num(r"\(own code\) ([\d.]+) ms", body)
            v["game_wall"] = num(r"of the game thread's ([\d.]+) ms of wall time", body)
        elif body.startswith("[te.prof] presents"):
            m = re.match(r"\[te\.prof\] presents \d+-\d+ (\S+) \(\d+ samples\): (.*)$", body)
            if m:
                w.prof[m.group(1)] = sections(m.group(2))
        elif body.startswith("[te.gfx.vita] gpu:"):
            v["gpu_ms"] = num(r"([\d.]+) ms of GPU work a present", body)
            v["gpu_busy"] = num(r"busy ([\d.]+)% of the window", body)
        elif body.startswith("[te.gfx.vita] gxm calls per frame:"):
            v["draws"] = num(r"= draws ([\d.]+)", body)
            v["inst"] = num(r"\+ instanced ([\d.]+)", body)
        elif body.startswith("[te.gfx.vita] hand-over"):
            v["made_ps"] = num(r"pictures made \d+ \(([\d.]+) a second\)", body)
            v["replaced_pct"] = num(r"replaced before drawn \d+ \(([\d.]+)%\)", body)
            v["boundaries_ps"] = num(r"game boundaries ([\d.]+) a second", body)
        elif body.startswith("[te.vita.lock] presents"):
            v["flips_ps"] = num(r"display flips ([\d.]+) a second", body)
            v["flip_gap"] = num(r"longest flip gap ([\d.]+) ms", body)
            v["misses"] = num(r"misses (\d+)", body, cast=int)
            m = re.search(r"tick work ms p50/p90/max (\S+) / (\S+) / ([\d.]+)", body)
            if m:
                v["tick_p90"], v["tick_max"] = m.group(2), float(m.group(3))
            m = re.search(r"drawing thread per picture ms (\S+) / (\S+) / ([\d.]+)", body)
            if m:
                v["rt_p90"], v["rt_max"] = m.group(2), float(m.group(3))
        elif body.startswith("[te.load] presents") and re.search(r"slow (ticks|pictures) not logged", body):
            k = "ticklog" if "slow ticks not logged" in body else "piclog"
            v[k + "_not"] = num(r"not logged (\d+) of", body, cast=int)
            v[k + "_over"] = num(r"not logged \d+ of (\d+) over", body, cast=int)
            v[k + "_logged"] = num(r": (\d+) logged", body, cast=int)
            v[k + "_not_ms"] = num(r"the ones not logged ([\d.]+) ms", body)
            v[k + "_not_max"] = num(r"the ones not logged [\d.]+ ms, the longest ([\d.]+) ms", body)
            m = re.search(r"by the top section of their profile: (.*)$", body)
            v[k + "_tops"] = m.group(1).strip() if m else ""
            what = "ticks not logged" if k == "ticklog" else "pictures not logged"
            w.events.append((what, None, None, f"{v[k + '_not']} of {v[k + '_over']} ({v[k + '_not_ms'] or 0:.0f} ms; "
                                               f"the longest {v[k + '_not_max']} ms)"))
        elif body.startswith("[te.load] presents"):
            m = re.search(r"over ([\d.]+) ms (\d+) \(([\d.]+) ms\)", body)
            if m:
                v["long_ms"], v["long_n"], v["long_sum"] = float(m.group(1)), int(m.group(2)), float(m.group(3))
            m = re.search(r"the longest ([\d.]+) ms at boundary (\d+) \(([^)]*)\)", body)
            if m:
                v["longest"], v["longest_b"], v["longest_what"] = float(m.group(1)), int(m.group(2)), m.group(3)
            v["tex_decoded"] = num(r"texture decode: (\d+) textures", body, cast=int)
        elif body.startswith("[te.load] textures ahead of their use"):
            v["eager_loaded"] = num(r"\): (\d+) loaded", body, cast=int)
            v["eager_first_draw"] = num(r"at their first draw (\d+)", body, cast=int)
            v["eager_released"] = num(r"released before made (\d+)", body, cast=int)
            v["eager_failed"] = num(r"failed (\d+)", body, cast=int)
            v["eager_pending"] = num(r"(\d+) pending now", body, cast=int)
            v["eager_avg_ms"] = num(r"a load to its texture made ([\d.]+) ms on average", body)
        elif body.startswith("[te.gfx.vita] textures made in the window:"):
            v["tex_made"] = num(r"in the window: (\d+)", body, cast=int)
            v["tex_nostorage"] = num(r"left without storage by vitaGL (\d+)", body, cast=int)
        elif body.startswith("[te.gfx] texture memory:"):
            v["tex_alive"] = num(r"texture memory: (\d+) alive", body, cast=int)
            v["tex_mb"] = num(r"alive, ([\d.]+) MB held", body)
            v["tex_win_made"] = num(r"in the window made (\d+)", body, cast=int)
            v["tex_win_destroyed"] = num(r"destroyed (\d+)", body, cast=int)
        elif body.startswith("[te.io] presents"):
            v["card_reads"] = num(r"(\d+) card reads", body, cast=int)
            v["card_mb"] = num(r"card reads ([\d.]+) MB", body)
            v["io_waits"] = num(r"the callers waited (\d+) times", body, cast=int)
            v["io_wait_ms"] = num(r"the callers waited \d+ times ([\d.]+) ms", body)
            v["io_wait_max"] = num(r"\(max ([\d.]+) ms", body)
        elif body.startswith("[te.gfx.vita] vitaGL allocations in the window:"):
            total = 0
            for kind in ("vram", "ram", "phycont", "budget", "newlib"):
                n = num(r"\b" + kind + r" (\d+) \(", body, cast=int)
                total += n or 0
            v["gl_allocs"] = total
            v["gl_alloc_ms"] = num(r"([\d.]+) ms in the allocator", body)
            v["gl_sleeps"] = num(r"1-second allocator sleeps (\d+)", body, cast=int)
            v["gl_failed"] = sum(int(x) for x in re.findall(r"KB, (\d+) failed\)", body))
        elif body.startswith("[te.gfx.vita] memory per frame: vertex ring"):
            v["ring_kb"] = num(r"vertex ring ([\d.]+) KB", body)
            v["ring_peak"] = num(r"peak frame ([\d.]+) KB", body)
            v["ring_respec"] = num(r"storage re-specified (\d+) x", body, cast=int)
        elif body.startswith("[te.gfx.vita] presents") and "builders per present" in body:
            v["shadow_builds"] = num(r"SHADOW builds ([\d.]+)", body)
            v["strips"] = num(r"strips ([\d.]+)", body)
            v["build_vertices"] = num(r"vertices ([\d.]+) \(distinct", body)
        elif body.startswith("[te.gfx.vita] presents") and "build pool per present" in body:
            v["pool_jobs"] = num(r"jobs posted ([\d.]+)", body)
            v["pool_slept"] = num(r"waits slept ([\d.]+)", body)
        elif body.startswith("[te.gfx] summary: presents"):
            c = {"presents": num(r"summary: presents (\d+)", body, cast=int),
                 "particle_blocks": num(r"particle blocks (\d+)", body, cast=int),
                 "particle_tris": num(r"particle blocks \d+ \(triangles (\d+)", body, cast=int),
                 "shadow_passes": num(r"shadow passes (\d+)", body, cast=int),
                 "decal_quads": num(r"decals aged .*?quads (\d+)", body, cast=int),
                 "skins": num(r"skins (\d+) \(", body, cast=int),
                 # the draws the drawing device refused (a texture it does not have, the screen copy not taken, ...) and
                 # the textures loaded / failed: the corridor's walls not drawn would show here
                 "dropped": num(r"draws \d+ \(merged \d+, submitted \d+, dropped (\d+)\)", body, cast=int),
                 "textures": num(r"\btextures (\d+) \(failed", body, cast=int),
                 "tex_failed": num(r"\btextures \d+ \(failed (\d+)\)", body, cast=int)}
            if c["presents"] is not None:
                v["cum"] = c


# ---- the tables ----------------------------------------------------------------------------------------------------------

def fmt(x, nd=1):
    if x is None:
        return ""
    if isinstance(x, str):
        return x
    if isinstance(x, int):
        return str(x)
    return f"{x:.{nd}f}"


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    for r in rows:
        out.append("| " + " | ".join(fmt(c) for c in r) + " |")
    return "\n".join(out)


def areas_of(w):
    names = []
    for a in w.areas:
        n = short_area(a)
        if not names or names[-1] != n:
            names.append(n)
    return " > ".join(names) if names else "-"


def top_busy(prof, rows=2):
    out = [f"{n} {p:.0f}%" for n, p in prof if not any(n.startswith(i) for i in IDLE)][:rows]
    return "; ".join(out)


def t_windows(log, ws):
    head = ["presents", "end s", "area", "fps", "longest gap ms", "flips/s", "replaced %", "game ms", "render ms", "build0 ms",
            "build1 ms", "audio ms", "cores %"]
    rows = []
    for w in ws:
        v = w.v
        rows.append([w.name(), v.get("_end") or w.end_s, areas_of(w), v.get("fps"), v.get("gap"), v.get("flips_ps"),
                     v.get("replaced_pct"), v.get("cpu_game"), v.get("cpu_render"), v.get("cpu_build0"), v.get("cpu_build1"),
                     v.get("cpu_audio"), v.get("cores")])
    return ("## The windows: the pace and the threads (CPU milliseconds a present, [te.cpu])\n\n" + table(head, rows))


def t_work(log, ws):
    head = ["presents", "area", "GPU ms", "GPU busy %", "GXM draws", "instanced", "seams ms", "own code ms", "tick work p90 / max",
            "drawing thread p90 / max", "ticks over 25 ms (ms)", "the longest tick"]
    rows = []
    for w in ws:
        v = w.v
        tick = f"{v['tick_p90']} / {v['tick_max']:.1f}" if v.get("tick_max") is not None else ""
        rt = f"{v['rt_p90']} / {v['rt_max']:.1f}" if v.get("rt_max") is not None else ""
        lng = f"{v['long_n']} ({v['long_sum']:.0f})" if v.get("long_n") is not None else ""
        longest = f"{v['longest']:.1f} @ {v['longest_b']}" if v.get("longest") is not None else ""
        rows.append([w.name(), areas_of(w), v.get("gpu_ms"), v.get("gpu_busy"), v.get("draws"), v.get("inst"), v.get("seams"),
                     v.get("own"), tick, rt, lng, longest])
    return "## The work: the GPU, the draws, the game thread's seams, the ticks\n\n" + table(head, rows)


def t_seams(log, ws):
    head = ["presents", "area"] + SEAM_GROUPS + ["the groups", "own code"]
    rows = [[w.name(), areas_of(w)] + [w.v.get("seam_" + g) for g in SEAM_GROUPS] + [w.v.get("seams"), w.v.get("own")]
            for w in ws if w.v.get("seams") is not None]
    return "## The game seams (milliseconds a present, [te.prof] game seams)\n\n" + table(head, rows)


def t_profiles(log, ws):
    head = ["presents", "area", "game", "drawing thread", "build0", "build1"]
    rows = [[w.name(), areas_of(w), top_busy(w.prof.get("game", []), 3), top_busy(w.prof.get("render", []), 3),
             top_busy(w.prof.get("build0", [])), top_busy(w.prof.get("build1", []))] for w in ws if w.prof]
    return "## The threads' top sections ([te.prof], their idle sections left out)\n\n" + table(head, rows)


def t_bursts(log, ws):
    head = ["presents", "area", "fps", "longest gap ms", "replaced %", "tick max", "drawing max", "particle blocks a present",
            "particle triangles", "build threads' particle %", "shadow builds", "vertex ring KB (peak)", "build waits slept",
            "voices playing", "mix ms (max)", "vitaGL allocations (ms, sleeps)"]
    rows = []
    for w in ws:
        v = w.v
        part = 0.0
        for th in ("build0", "build1"):
            part += sum(p for n, p in w.prof.get(th, []) if n.startswith("particle"))
        ring = f"{v['ring_kb']:.0f} ({v['ring_peak']:.0f})" if v.get("ring_kb") is not None and v.get("ring_peak") is not None else ""
        voices = max((a[0] for a in w.audio if a[0] is not None), default=None)
        mix = [a for a in w.audio if a[1] is not None]
        mixs = f"{max(a[1] for a in mix):.2f} ({max(a[2] or 0 for a in mix):.2f})" if mix else ""
        gl = f"{v['gl_allocs']} ({v.get('gl_alloc_ms') or 0:.2f}, {v.get('gl_sleeps') or 0})" if v.get("gl_allocs") is not None else ""
        rows.append([w.name(), areas_of(w), v.get("fps"), v.get("gap"), v.get("replaced_pct"), v.get("tick_max"), v.get("rt_max"),
                     v.get("particle_blocks_pp"), v.get("particle_tris_pp"), part / 2 if w.prof else None, v.get("shadow_builds"),
                     ring, v.get("pool_slept"), voices, mixs, gl])
    return ("## The bursts: the effect (fps, the longest gap, pictures replaced, the longest tick and picture) beside what a "
            "burst of particles, pieces and sounds costs (the particle counts: the summary line's difference over the window; "
            "the build threads' particle share: the mean of the two threads' particle sections; the voices and the mix: the "
            "audio lines of the window, at their most)\n\n" + table(head, rows))


def t_textures(log, ws):
    head = ["presents", "area", "loaded (failed)", "made in the window", "loaded ahead", "made at their first draw",
            "released before made", "failed", "pending", "load to made ms", "without storage", "alive", "MB held",
            "decoded on the game thread", "draws refused"]
    rows = []
    for w in ws:
        v = w.v
        if not any(v.get(k) for k in ("tex_made", "eager_loaded", "tex_win_made", "tex_win_destroyed", "tex_decoded", "dropped_w",
                                      "textures_w")):
            continue
        made = v.get("tex_made") if v.get("tex_made") is not None else v.get("tex_win_made")
        loaded = f"{v['textures_w']} ({v.get('tex_failed_w') or 0})" if v.get("textures_w") is not None else ""
        rows.append([w.name(), areas_of(w), loaded, made, v.get("eager_loaded"), v.get("eager_first_draw"), v.get("eager_released"),
                     v.get("eager_failed"), v.get("eager_pending"), v.get("eager_avg_ms"), v.get("tex_nostorage"),
                     v.get("tex_alive"), v.get("tex_mb"), v.get("tex_decoded"), v.get("dropped_w")])
    return ("## The textures (the windows that loaded, made or released any, or had a draw refused; [te.load] textures ahead "
            "of their use, [te.gfx.vita] textures made in the window, [te.gfx] texture memory; loaded (failed) and the draws "
            "the drawing device refused -- a texture it does not have, the screen copy not taken -- are the summary line's "
            "differences over the window)\n\n" + table(head, rows))


def t_io(log, ws):
    head = ["presents", "area", "card reads", "MB", "the game waited (times)", "ms", "the longest wait ms"]
    rows = [[w.name(), areas_of(w), w.v.get("card_reads"), w.v.get("card_mb"), w.v.get("io_waits"), w.v.get("io_wait_ms"),
             w.v.get("io_wait_max")] for w in ws if w.v.get("card_reads") or w.v.get("io_waits")]
    return "## The disc (the windows with card reads or waits; [te.io])\n\n" + table(head, rows)


def t_events(log, ws):
    head = ["presents", "boundary", "s", "what", "detail"]
    rows = []
    for w in ws:
        for kind, b, s, what in w.events:
            rows.append([w.name(), b, s, kind, what])
    return ("## The events (area changes, pauses -- a player's mark --, lives kept, cutscenes skipped, the slow ticks "
            "not logged)\n\n" + table(head, rows))


def t_ticks(log, ws, mode):
    head = ["boundary", "s", "presents", "area", "ms", "game ms", "kind", "the profile's top sections", "load work"]
    rows = []
    keep = set(id(w) for w in ws)
    for t in log.ticks:
        if id(t.window) not in keep:
            continue
        if mode == "play" and t.kind() in ("disc wait", "movie", "music", "cutscene skip"):
            continue
        prof = ", ".join(f"{n} {p:.0f}%" for n, p in t.prof[:3])
        rows.append([t.boundary, t.s, t.window.name() if t.window else "", short_area(t.area), t.ms, t.parts.get("game"),
                     t.kind(), prof, t.load])
    what = ("every slow tick" if mode == "all" else
            "the slow ticks that are not a wait for the disc, a movie or the music, nor a cutscene's skip")
    return f"## The slow ticks ({what}; TE_TICK_LOG_MS)\n\n" + table(head, rows)


def t_pictures(log, ws):
    head = ["present", "s", "presents", "area", "ms", "waiting for the GPU", "drawing", "the present call", "kind",
            "the drawing thread's top sections"]
    keep = set(id(w) for w in ws)
    rows = [[p.present, p.s, p.window.name() if p.window else "", short_area(p.area), p.ms, p.gpu, p.draw, p.call, p.kind(),
             ", ".join(f"{n} {q:.0f}%" for n, q in p.prof[:4])]
            for p in log.pictures if p.window is None or id(p.window) in keep]
    return ("## The slow pictures (the drawing thread: TE_PICTURE_LOG_MS; kind census = a window's last picture, which also "
            "prints the window's census lines)\n\n" + table(head, rows))


def grouped(items, key, head0):
    """rows of items grouped by key(item): count, ms, mean, the longest, the sections under the top, the areas"""
    groups = {}
    for it in items:
        groups.setdefault(key(it), []).append(it)
    rows = []
    for k, its in sorted(groups.items(), key=lambda kv: -sum(t.ms for t in kv[1])):
        longest = max(its, key=lambda t: t.ms)
        under = {}
        for t in its:
            for name, pct in t.prof[1:]:
                under[name] = under.get(name, 0.0) + pct
        under_s = ", ".join(f"{n} {q / len(its):.0f}%" for n, q in sorted(under.items(), key=lambda kv: -kv[1])[:3])
        areas = []
        for t in its:
            a = short_area(t.area)
            if a not in areas:
                areas.append(a)
        at = getattr(longest, "boundary", None) or getattr(longest, "present", None)
        rows.append(list(k) + [len(its), sum(t.ms for t in its), sum(t.ms for t in its) / len(its),
                                f"{longest.ms:.1f} ({at}, {short_area(longest.area)})", under_s, ", ".join(areas)])
    return rows


def t_groups(log, ws):
    keep = set(id(w) for w in ws)
    ticks = [t for t in log.ticks if id(t.window) in keep]
    rows = grouped(ticks, lambda t: (t.kind(), t.top()), "kind")
    head = ["kind", "the top section", "ticks", "ms", "mean ms", "the longest (boundary, area)", "the sections under it (mean share)",
            "areas"]
    out = ("## The slow ticks by what they were (the kind: a disc / movie / music wait, load work, a cutscene's skip, "
           "the hand-over, or play) and the top section of their profile\n\n" + table(head, rows))
    nots = [w for w in ws if w.v.get("ticklog_not")]
    if nots:
        out += ("\n\nNot logged (past the slow-tick lines' cap a window: TE_TICK_LOG_CAP), by the top section of their "
                "profile:\n\n" +
                table(["presents", "area", "not logged", "of", "ms", "the longest ms", "by the top section"],
                      [[w.name(), areas_of(w), w.v.get("ticklog_not"), w.v.get("ticklog_over"), w.v.get("ticklog_not_ms"),
                        w.v.get("ticklog_not_max"), w.v.get("ticklog_tops")] for w in nots]))
    pics = [p for p in log.pictures if p.window is None or id(p.window) in keep]
    if pics or any(w.v.get("piclog_not") for w in ws):
        prow = grouped(pics, lambda p: (p.kind(), p.top()), "kind")
        out += ("\n\n## The slow pictures by kind (drawing, or the census window's own lines) and the top section of the "
                "drawing thread's profile\n\n" +
                table(["kind", "the top section", "pictures", "ms", "mean ms", "the longest (present, area)",
                       "the sections under it (mean share)", "areas"], prow))
        pnots = [w for w in ws if w.v.get("piclog_not")]
        if pnots:
            out += ("\n\nNot logged (past the slow-picture lines' cap a window: TE_PICTURE_LOG_CAP):\n\n" +
                    table(["presents", "area", "not logged", "of", "ms", "the longest ms", "by the top section"],
                          [[w.name(), areas_of(w), w.v.get("piclog_not"), w.v.get("piclog_over"), w.v.get("piclog_not_ms"),
                            w.v.get("piclog_not_max"), w.v.get("piclog_tops")] for w in pnots]))
    return out


TABLES = ["windows", "work", "seams", "profiles", "bursts", "textures", "io", "events", "ticks", "pictures", "groups"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--tables", default=",".join(TABLES), help="comma-separated: " + ", ".join(TABLES))
    ap.add_argument("--from", dest="first", type=int, default=None, help="keep the windows whose presents end at P or later")
    ap.add_argument("--to", dest="last", type=int, default=None, help="keep the windows whose presents end at Q or earlier")
    ap.add_argument("--area", default=None, help="keep the windows that were in an area whose name contains NAME")
    ap.add_argument("--ticks", choices=["all", "play"], default="all", help="play: the ticks table without the loads' waits")
    a = ap.parse_args()
    want = [t.strip() for t in a.tables.split(",") if t.strip()]
    bad = [t for t in want if t not in TABLES]
    if bad:
        ap.error("unknown table(s): " + ", ".join(bad))
    log = Log(a.log)
    ws = log.windows
    if a.first is not None:
        ws = [w for w in ws if w.last >= a.first]
    if a.last is not None:
        ws = [w for w in ws if w.last <= a.last]
    if a.area:
        ws = [w for w in ws if any(a.area in (x or "") for x in w.areas)]
    h = log.head
    span = f"{log.end_s:.0f} s" if log.end_s else "no time stamps (TE_LOG_STAMP)"
    hz = f"{h['hz']} Hz, " if h.get("hz") else ""
    print(f"# {os.path.basename(a.log)}: the {h.get('lane', 'unknown')} lane, {hz}{span}, "
          f"{len([w for w in log.windows if w.first])} census windows, {len(log.ticks)} slow-tick lines, "
          f"{len(log.pictures)} slow-picture lines")
    print()
    for what, ms_k, cap_k in (("slow-tick", "TE_TICK_LOG_MS", "TE_TICK_LOG_CAP"), ("slow-picture", "TE_PICTURE_LOG_MS", "TE_PICTURE_LOG_CAP")):
        if h.get(ms_k):
            cap = f", at most {h[cap_k]} a window ({cap_k})" if h.get(cap_k) else ", no cap"
            print(f"- the {what} lines: over {h[ms_k]} ms ({ms_k}){cap}")
    if h.get("disc"):
        print(f"- disc: {h['disc']}")
    if h.get("set"):
        print(f"- the diagnosis set (built in): {h['set']}")
    if h.get("args"):
        print(f"- arguments ({h.get('args_from')}): {h['args']}")
    over = sum(w.v.get("long_n") or 0 for w in log.windows)
    if over:
        long_ms = next((w.v["long_ms"] for w in log.windows if w.v.get("long_ms")), 25.0)
        print(f"- ticks over {long_ms:.0f} ms counted by the load census ([te.load] window lines): {over} in the run")
    for t in want:
        print()
        if t == "ticks":
            print(t_ticks(log, ws, a.ticks))
        else:
            print(globals()["t_" + t](log, ws))
    return 0


if __name__ == "__main__":
    sys.exit(main())
