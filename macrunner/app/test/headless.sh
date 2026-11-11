#!/usr/bin/env bash
# The window's test, with no guest and nobody at the keyboard. The app attaches to a QEMU that is paused
# before its first instruction, and on each pixel path (the scanout mapped from QEMU's memory; pixels
# sent in the messages) two things must hold:
#   picture  what the Metal pipeline makes of the scanout at 1:1, read back, is byte-identical to QEMU's
#            own screendump of it;
#   input    22 scripted AppKit events reach QEMU as exactly the expected run of keys and pointer moves
#            (its dbus_kbd_* / dbus_mouse_* trace): a repeat, a modifier, keys released when the window
#            loses them, a late key-up that must send nothing, positions through the letterbox, a move
#            over the bar that must send nothing, and a key-up under Command.
# A window opens for four seconds per path and does not take focus.
#
#   macrunner/app/test/headless.sh       exit 0 pass, 1 fail, 77 skipped (not macOS, or no D-Bus display)
#   THYLACINE_QEMU=<qemu-system-aarch64>  the QEMU to attach to (default: the one on PATH)
#   THYLACINE_MAC_APP=<binary>            the app (default: target/release/thylacine-mac of this crate)
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
crate=$(dirname "$here")
repo=$(cd "$crate/../.." && pwd)
qemu=${THYLACINE_QEMU:-qemu-system-aarch64}
app=${THYLACINE_MAC_APP:-$crate/target/release/thylacine-mac}

[ "$(uname -s)" = Darwin ] || { echo "SKIP: the app is a macOS program"; exit 77; }
"$qemu" -display help 2>/dev/null | grep -qw dbus || { echo "SKIP: $qemu has no D-Bus display"; exit 77; }
[ -x "$app" ] || { echo "FAIL: no app at $app (cargo build --release in $crate)"; exit 1; }

want='kbd_press 30;kbd_release 30;kbd_press 200;kbd_release 200;kbd_press 42;kbd_release 42;kbd_press 30;kbd_press 30;kbd_release 30;kbd_press 31;kbd_release 31;mouse_set_pos x=320, y=240;mouse_set_pos x=10, y=10;mouse_press b2;mouse_set_pos x=10, y=10;mouse_release b2;kbd_press 32;kbd_release 32;kbd_press 219;kbd_press 32;kbd_release 32;kbd_release 219;'

qp=""; ap=""; wd=""
stop() { for p in $wd $ap $qp; do kill "$p" 2>/dev/null; done; }
trap stop EXIT

# leg <name> [VAR=value ...]: one attach, in its own directory so the socket's path stays short.
leg() {
    local name=$1; shift
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f q.sock app.ppm screendump.ppm trace.log app.out qemu.out
    "$qemu" -machine virt,accel=tcg -cpu cortex-a72 -m 256 -S -nodefaults -display dbus,p2p=on \
        -device virtio-gpu-pci -device virtio-tablet-pci -device virtio-keyboard-pci \
        -qmp unix:q.sock,server=on,wait=off -serial none -monitor none \
        -trace 'dbus_*' -D trace.log > qemu.out 2>&1 &
    qp=$!
    local i; for i in $(seq 50); do [ -S q.sock ] && break; sleep 0.1; done
    [ -S q.sock ] || { echo "$name: FAIL: QEMU gave no QMP socket ($(head -1 qemu.out))"; return 1; }
    env "$@" "$app" --attach q.sock --no-activate --exit-after 4 --dump app.ppm --scripted-input > app.out 2>&1 &
    ap=$!
    ( sleep 20; kill "$ap" 2>/dev/null ) &   # the app leaves by itself; this bounds a hang
    wd=$!
    wait "$ap"; local arc=$?
    kill "$wd" 2>/dev/null; wd=""; ap=""
    python3 - <<'PY' >> app.out 2>&1
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect("q.sock"); f = s.makefile("rw")
f.readline()
for cmd in ({"execute": "qmp_capabilities"}, {"execute": "screendump", "arguments": {"filename": os.path.abspath("screendump.ppm")}}):
    f.write(json.dumps(cmd) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "event" not in r: break
    print("qmp", cmd["execute"], "->", r)
PY
    kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; qp=""
    local rc=0 got
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    if cmp -s app.ppm screendump.ppm; then
        echo "$name: picture is QEMU's screendump, byte for byte ($(wc -c < app.ppm | tr -d ' ') bytes)"
    else
        echo "$name: FAIL: the picture differs from QEMU's screendump, or one is missing"; rc=1
    fi
    got=$(grep 'dbus_kbd\|dbus_mouse' trace.log | sed 's/dbus_//; s/keycode //; s/button /b/' | tr '\n' ';')
    if [ "$got" = "$want" ]; then
        echo "$name: input arrived as scripted (22 events)"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want"; rc=1
    fi
    grep -q "pixels $pixels" app.out || { echo "$name: FAIL: the app did not take the '$pixels' path ($(grep '^finish' app.out))"; rc=1; }
    return $rc
}

rc=0
pixels="mapped";          leg map || rc=1
pixels="in the messages"; leg in-band THYLACINE_NO_MAP=1 || rc=1
[ $rc -eq 0 ] && echo "PASS: macrunner/app headless" || echo "FAIL: macrunner/app headless (evidence in $repo/build/macrunner-app-test)"
exit $rc
