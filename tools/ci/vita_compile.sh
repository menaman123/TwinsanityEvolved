#!/usr/bin/env bash
# tools/ci/vita_compile.sh -- the game code COMPILES to native armv7 objects with the vitasdk GCC: every game TU
# (game/src/game/*.cpp, game/src/*.cpp, the decomp's libc / libm / malloc) and the module's own platform side
# (platform/nativeee/wasm/*.cpp), each through tools/te_cxx_arm.py (te_cxx.py's five MIPS-construct rewrites with their ARM
# replacements; zero edits to game code), at -O2. The step past tools/ci/vita_syntax.sh (which only parses): the
# feasibility measurement of a native ARM build of the game (notes: the native plan).
#
#   tools/ci/vita_compile.sh [--out DIR] [--w2c-objs DIR] [-O2|-O3|-Os] [-jN] [VITASDK=/usr/local/vitasdk]
#
# Writes DIR/obj/<tu>.o, DIR/rep/<tu>.json (te_cxx_arm.py's report: the rewrites and the errors) and DIR/report.md: per TU
# compiled / FAILED (the first errors) and its .text bytes, the totals, and -- when --w2c-objs names the Vita build's
# tegame object directory (<build>/CMakeFiles/tegame.dir/w2c) -- the wasm2c module's code size beside them.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VITASDK="${VITASDK:-/usr/local/vitasdk}"
OUT="$ROOT/build-vita-compile"
W2C_OBJS=""
JOBS=4
OPT=2
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --w2c-objs) W2C_OBJS="$2"; shift 2 ;;
    -j*) JOBS="${1#-j}"; shift ;;
    -O*) OPT="${1#-O}"; shift ;;
    *) VITASDK="$1"; shift ;;
  esac
done
GXX="$VITASDK/bin/arm-vita-eabi-g++"
SIZE="$VITASDK/bin/arm-vita-eabi-size"
[ -x "$GXX" ] || { echo "vita_compile: $GXX not found (install vitasdk or pass its path)"; exit 2; }
command -v python3 >/dev/null || { echo "vita_compile: python3 not found"; exit 2; }
cd "$ROOT"
mkdir -p "$OUT/obj" "$OUT/rep" "$OUT/log"
# The seam header the platform side includes (ps2e_seam.h), generated exactly as the build does
python3 tools/gen_seam.py --game game --out "$OUT/seam" --module game > "$OUT/log/gen_seam.txt"
FILES=( game/src/game/*.cpp game/src/*.cpp game/src/platform/ps2/libc.cpp game/src/platform/ps2/libm.cpp
        game/src/platform/ps2/malloc.cpp platform/nativeee/wasm/*.cpp "$OUT"/seam/seam_forwarders_*.cpp )   # bash 3 (macOS): no mapfile
start=$(date +%s)
printf '%s\n' "${FILES[@]}" | xargs -P "$JOBS" -I{} sh -c '
  src="$1"; shift; out="$1"; shift; opt="$1"; shift; gxx="$1"; shift
  tu=$(printf "%s" "$src" | tr "/" "_")
  python3 tools/te_cxx_arm.py --gxx "$gxx" --game game --shim platform/nativeee/sdk_shim -I "$out/seam" -I platform/nativeee \
      -D TE_NATIVE=1 -O"$opt" --report "$out/rep/$tu.json" -o "$out/obj/$tu.o" "$src" > "$out/log/$tu.txt" 2>&1 || true
' sh {} "$OUT" "$OPT" "$GXX"
secs=$(( $(date +%s) - start ))
python3 - "$OUT" "$SIZE" "$W2C_OBJS" "$secs" "$OPT" "$("$GXX" -dumpversion)" "${FILES[@]}" <<'EOF'
import json, os, subprocess, sys
out, size, w2c, secs, opt, ver = sys.argv[1:7]
files = sys.argv[7:]
def text_bytes(paths):
    if not paths:
        return 0
    r = subprocess.run([size, "-A", *paths], capture_output=True, text=True)
    total = 0
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0].startswith(".text") and p[1].isdigit():
            total += int(p[1])
    return total
rows, ok, fail = [], 0, 0
tot_text = 0
rew = {"fences": 0, "thunks_export": 0, "raw_blocks": 0, "touch": 0, "lqsq": 0, "trampoline_f_renamed": 0, "unrecognised_asm": 0}
for src in files:
    tu = src.replace("/", "_")
    rep = os.path.join(out, "rep", tu + ".json")
    obj = os.path.join(out, "obj", tu + ".o")
    try:
        r = json.load(open(rep))
    except Exception:
        r = {"ok": False, "errors": ["(no report: the wrapper did not run)"], "rewrites": None}
    t = text_bytes([obj]) if r.get("ok") and os.path.exists(obj) else 0
    tot_text += t
    if r.get("rewrites"):
        for k in rew:
            v = r["rewrites"].get(k, 0)
            rew[k] += len(v) if isinstance(v, list) else v
    if r.get("ok"):
        ok += 1
        rows.append((src, "compiled", "", t))
    else:
        fail += 1
        errs = [e.strip() for e in r.get("errors", [])][:3]
        rows.append((src, "FAILED", " / ".join(errs)[:400], 0))
w2c_text = 0
if w2c and os.path.isdir(w2c):
    objs = [os.path.join(w2c, f) for f in sorted(os.listdir(w2c)) if f.endswith((".o", ".obj"))]
    w2c_text = text_bytes(objs)
L = [f"# vita_compile: the game TUs as native armv7 objects (arm-vita-eabi-g++ {ver}, -O{opt}, tools/te_cxx_arm.py)", "",
     f"{len(files)} TUs: {ok} compiled, {fail} failed, in {secs} s. Native .text total {tot_text:,} bytes"
     + (f"; the wasm2c module's 8 parts (tegame, -O2) .text {w2c_text:,} bytes (ratio native/w2c {tot_text / w2c_text:.3f})" if w2c_text else "") + ".",
     "", "Rewrites applied (sum over the TUs): " + ", ".join(f"{k} {v}" for k, v in rew.items()), "",
     "| TU | result | .text bytes | first errors |", "|---|---|---:|---|"]
for src, res, err, t in rows:
    L.append(f"| {src} | {res} | {t:,} | {err.replace('|', '/')} |")
open(os.path.join(out, "report.md"), "w").write("\n".join(L) + "\n")
print(f"vita_compile: {len(files)} TUs, {ok} compiled, {fail} failed (armv7 vitasdk {ver}, -O{opt}, {secs} s); "
      f"native .text {tot_text:,} bytes" + (f", w2c .text {w2c_text:,} bytes" if w2c_text else "") + f" -> {out}/report.md")
sys.exit(0 if fail == 0 else 1)
EOF
