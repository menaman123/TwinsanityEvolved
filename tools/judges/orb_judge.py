#!/usr/bin/env python3
"""orb_judge.py -- the title's ICE BLOCK env-map pass: the "orb" of the title scene.

The title's ice block (rigid model 0xc5f7e4 on the PAL disc) has two shaders, [0x01 textured] [0x0C env map]: retail's second pass
(TexturedTristrip::RunEnvCall, the sphere map of the half vector eye + normal through the clip matrix, additive) is the glassy glint
the player sees as the orb. The port drew the first shader only: a flat cyan translucent block. The judge: on the deterministic boot
(--skip-movies) the title frame 1500's block box has a luminance standard deviation and a 4-bit colour count above the floors the
textured pass alone never reaches (36.1 / 318 on bin/twinsanityevolved.0f92911; 59.5 / 406 with the env pass).

Usage: orb_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR] | orb_judge.py --frame PPM [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = nothing to judge.
"""
import argparse, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "title_orb_pal.json")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def measure(image, box):
    w, h, px = image
    x0, y0, x1, y1 = box
    lum, colours = [], set()
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(max(0, x0), min(w, x1)):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            lum.append(0.299 * r + 0.587 * g + 0.114 * b)
            colours.add((r >> 4, g >> 4, b >> 4))
    n = len(lum)
    mean = sum(lum) / n if n else 0.0
    sd = (sum((v - mean) ** 2 for v in lum) / n) ** 0.5 if n else 0.0
    return {"pixels": n, "mean": round(mean, 1), "sd": round(sd, 1), "colours": len(colours)}


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    dump = os.path.join(out, "dump")
    shutil.rmtree(dump, ignore_errors=True)
    os.makedirs(dump)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state", "--stop-at", str(fx["stop_at"])]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_EVERY=str(fx["dump_every"]))
    print("orb_judge: running", " ".join(args), flush=True)
    with open(os.path.join(out, "run.log"), "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("orb_judge: exit", rc, flush=True)
    return os.path.join(dump, "frame_%06d.ppm" % fx["present"])


def judge(frame, fx):
    if not os.path.exists(frame):
        print("orb_judge: frame missing", frame)
        return 2
    st = measure(read_ppm(frame), fx["box"])
    print("orb_judge:", json.dumps(st))
    reasons = []
    if st["sd"] < fx["min_sd"]: reasons.append("luminance SD %.1f < %.1f (the block is flat: no env pass)" % (st["sd"], fx["min_sd"]))
    if st["colours"] < fx["min_colours"]: reasons.append("%d colours at 4 bits < %d" % (st["colours"], fx["min_colours"]))
    if st["mean"] > fx.get("max_mean", 255): reasons.append("mean luminance %.1f > %.1f (blown out)" % (st["mean"], fx["max_mean"]))
    for r in reasons: print("  RED:", r)
    print("orb_judge:", "RED" if reasons else "GREEN")
    return 1 if reasons else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin"); ap.add_argument("--iso"); ap.add_argument("--frame"); ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out")
    a = ap.parse_args()
    with open(a.fixture) as f: fx = json.load(f)
    if a.frame: return judge(a.frame, fx)
    if not a.bin or not a.iso: ap.error("--bin and --iso (or --frame)")
    return judge(run(a.bin, a.iso, a.out or os.path.join(ROOT, "build", "orb_judge"), fx), fx)


if __name__ == "__main__":
    sys.exit(main())
