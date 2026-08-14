#!/usr/bin/env bash
# native-gl-bench.sh -- the HW-GL exit bar's NATIVE anchor leg (GPU-DESIGN §13).
# Runs ON the GL host (thyla-pi); driven by `tools/warp-host.sh native-bench`.
#
# Builds native-gl-bench.c if needed, then runs it twice -- once on the real
# GPU (default) and once forced to llvmpipe -- with a thermal guard around
# each, and reports HW, SW, and the HW/SW ratio that the guest ratio is held
# against. Surfaceless EGL + FBO: the only headless path to real V3D on a
# display-less Pi, and the faithful match to the guest's render-to-FBO model.
set -u

WORK="${WARP_WORK:-$HOME/warp}"
SRC="$WORK/native-gl-bench.c"
BIN="$WORK/native-gl-bench"

[[ -f "$SRC" ]] || { echo "NATIVE-BENCH: missing $SRC (sync first)"; exit 2; }
if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
    cc -O2 "$SRC" -lEGL -lGLESv2 -lm -o "$BIN" || { echo "NATIVE-BENCH: build failed"; exit 2; }
fi

throttle() { vcgencmd get_throttled 2>/dev/null | sed 's/throttled=//' || echo "0x0"; }

# A throttled native baseline flatters the guest ratio (§13) -- guard both legs.
t_pre="$(throttle)"
hw_line="$("$BIN")"
t_mid="$(throttle)"
sw_line="$(GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 "$BIN")"
t_post="$(throttle)"

echo "$hw_line"
echo "$sw_line"

if [[ "$t_pre" != "0x0" || "$t_mid" != "0x0" || "$t_post" != "0x0" ]]; then
    echo "NATIVE-BENCH: THROTTLED ($t_pre/$t_mid/$t_post) -- figures void, re-take"
    exit 1
fi

hw_fps="$(sed -n 's/.* fps \([0-9.]*\) .*/\1/p' <<<"$hw_line")"
sw_fps="$(sed -n 's/.* fps \([0-9.]*\) .*/\1/p' <<<"$sw_line")"
if [[ -z "$hw_fps" || -z "$sw_fps" ]]; then
    echo "NATIVE-BENCH: UNPARSED (hw='$hw_line' sw='$sw_line')"
    exit 1
fi

ratio="$(awk -v h="$hw_fps" -v s="$sw_fps" 'BEGIN{ if (s>0) printf "%.2f", h/s; else print "inf" }')"
echo "NATIVE-BENCH RATIO: native HW/SW = ${ratio}x  (HW ${hw_fps} fps / SW ${sw_fps} fps)"
echo "NATIVE-BENCH: the exit bar (GPU-DESIGN §13) wants guest HW/SW >= 0.5x of this = $(awk -v r="$ratio" 'BEGIN{printf "%.2f", r*0.5}')x"
