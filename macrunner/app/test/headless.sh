#!/usr/bin/env bash
# The window's test, with no guest and nobody at the keyboard. The app attaches to a QEMU that is paused
# before its first instruction, and on each pixel path (the scanout mapped from QEMU's memory; pixels
# sent in the messages) three things must hold:
#   picture  what the Metal pipeline makes of the scanout at 1:1, read back, is byte-identical to QEMU's
#            own screendump of it;
#   input    a scripted run of AppKit events (view.rs, scripted_input) reaches QEMU as exactly the
#            expected lines of its dbus_kbd_* / dbus_mouse_* trace: a repeat, a modifier, keys and a
#            button let go of when the window loses the keys or a menu opens, late releases that must
#            send nothing, a modifier whose release never arrived (before a key, a click, a move, and
#            a click and a scroll over the bar, where the view itself says what the guest still
#            holds), the two Shifts told apart by their own bits and what is made of them without,
#            positions through the letterbox, clicks and moves over the bar that must send nothing,
#            a wheel notch with its place, a key-up under Command, and a button still down when the
#            app leaves;
#   pointer  an image defined as the guest defines its pointer is what AppKit draws for the Mac's.
# A window opens for four seconds per path and does not take focus.
#
# Then the size leg, where nothing is drawn or typed. QEMU has no guest to run, is paused all the
# same, and takes a tester's reads and writes in a guest's place (qtest), so what its virtio GPU
# answers a driver can be read
# (as_guest.py): the display's rectangle, the millimetres in its EDID, and its display event.
#   size     the device answers with the size the app told it for the window: when the app attaches
#            (a run that ends before a change of size could have been told), and again a moment
#            after the window's content is set to another size in three steps, which is told once.
#            That size is 4200x700 points, a mode too large for QEMU's EDID to have a detailed
#            timing on any screen: the millimetres reach a guest as whole centimetres. A second
#            later the content is set a point wider and back, and nothing more is told.
# The window opens twice more, for a moment and for three seconds, the second time wider than the
# screen.
#
# Then the bar leg, in a window again. Its content is set one point larger each way than the scanout
# has pixels, which with a whole number of pixels to a point is a view one guest pixel past a whole
# scale: what a guest's fitted mode leaves (docs/MAC-RUNNER.md 4.2).
#   bar      the picture is drawn at the whole scale and the pointer is mapped where it is drawn: a
#            click on each bar reaches QEMU as nothing, and one on the first pixel of the middle row
#            and one on the last pixel of the last row arrive as those pixels. A picture stretched to
#            fill the view would have both bar clicks land on its edge pixels.
# The window opens once more, for three seconds.
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
command -v "$qemu" > /dev/null || { echo "FAIL: no QEMU at $qemu (THYLACINE_QEMU names one)"; exit 1; }
# One that is there and cannot run has no displays to list either, and is not a QEMU without D-Bus.
"$qemu" --version > /dev/null 2>&1 || { echo "FAIL: $qemu does not run ($("$qemu" --version 2>&1 | head -1))"; exit 1; }
"$qemu" -display help 2>/dev/null | grep -qw dbus || { echo "SKIP: $qemu has no D-Bus display"; exit 77; }
[ -x "$app" ] || { echo "FAIL: no app at $app (cargo build --release in $crate)"; exit 1; }
# A binary older than the source it is said to be is a test of something else.
newer=$(find "$crate/src" "$crate/Cargo.toml" "$crate/Cargo.lock" -newer "$app" | head -1)
[ -z "$newer" ] || { echo "FAIL: $app is older than $newer (cargo build --release in $crate)"; exit 1; }

want='kbd_press 30;kbd_release 30;'                              # A
want+='kbd_press 200;kbd_release 200;'                           # Up, a key with a prefix
want+='kbd_press 42;kbd_release 42;'                             # left Shift
want+='kbd_press 30;kbd_press 30;kbd_release 30;'                # A held: one repeat
want+='kbd_press 31;kbd_release 31;kbd_press 35;kbd_release 35;' # S, the window loses the keys, H, S up late
want+='kbd_press 56;kbd_release 56;kbd_press 48;kbd_release 48;' # Option whose release never came, then B
want+='kbd_press 18;kbd_release 18;kbd_press 34;kbd_release 34;' # E, a menu opens, G, E up late
want+='kbd_press 42;kbd_release 42;kbd_press 54;kbd_release 54;' # the Shifts by their own bits: left up while the guest does not hold it
want+='kbd_press 42;kbd_press 54;kbd_release 42;kbd_release 54;' # the Shifts without bits of their own, and the keys lost
want+='kbd_press 42;kbd_release 42;'                             # left up while right holds their flag: sent as a press, let go of when it clears
want+='mouse_set_pos x=320, y=240;'                              # a move; the click after it is the window's
want+='kbd_press 33;kbd_release 33;'                             # F
want+='mouse_set_pos x=10, y=10;mouse_press b2;mouse_set_pos x=10, y=10;mouse_release b2;'     # right click
want+='kbd_press 29;kbd_release 29;'                             # Control whose release never came, let go of by a click
want+='mouse_set_pos x=10, y=10;mouse_press b2;mouse_set_pos x=10, y=10;mouse_release b2;'     # that click
want+='mouse_set_pos x=10, y=10;mouse_press b3;mouse_release b3;'  # a wheel notch up, with its place
want+='mouse_set_pos x=320, y=240;mouse_press b0;mouse_set_pos x=320, y=240;mouse_release b0;' # left click
want+='mouse_set_pos x=10, y=10;mouse_press b2;mouse_release b2;'  # after a click over the bar: a press let go of over it
want+='mouse_set_pos x=320, y=240;mouse_press b0;mouse_release b0;' # left held when the window loses the keys
want+='kbd_press 56;kbd_release 56;mouse_set_pos x=320, y=240;'  # Option whose release never came, let go of by a move
want+='kbd_press 42;kbd_release 42;'                             # Shift likewise, by a click over the bar
want+='kbd_press 29;kbd_release 29;'                             # Control likewise, by a scroll over the bar
want+='kbd_press 32;kbd_release 32;'                             # D through NSApplication
want+='kbd_press 219;kbd_press 32;kbd_release 32;kbd_release 219;' # D under Command
want+='mouse_set_pos x=320, y=240;mouse_press b0;mouse_release b0;' # left down when the app leaves: let go of on the way out
lines=$(printf %s "$want" | tr -cd ';' | wc -c | tr -d ' ')

qp=""; ap=""; wd=""
stop() { for p in $wd $ap $qp; do kill "$p" 2>/dev/null; done; }
trap stop EXIT

# leg <name> [VAR=value ...]: one attach, in its own directory so the socket's path stays short.
leg() {
    local name=$1; shift
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f q.sock app.ppm app-pointer.ppm screendump.ppm trace.log app.out qemu.out
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
        echo "$name: input arrived as scripted ($lines trace lines)"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want"; rc=1
    fi
    # The trace cannot tell a modifier let go of by an event over the bar from one let go of by
    # its own late release, so the script asks the view after each.
    local what
    for what in 'a click' 'a scroll'; do
        grep -q "^scripted: $what over the bar let go of the modifier\$" app.out \
            || { echo "$name: FAIL: $what over the bar did not let go of a modifier whose release never came ($(grep "^scripted: $what" app.out))"; rc=1; }
    done
    grep -q "pixels $pixels" app.out || { echo "$name: FAIL: the app did not take the '$pixels' path ($(grep '^finish' app.out))"; rc=1; }
    grep -q 'pointer 3x2 hot 1,1 shown$' app.out || { echo "$name: FAIL: the Mac's pointer did not take the image that was defined ($(grep '^finish' app.out))"; rc=1; }
    if python3 - app-pointer.ppm <<'PY' >> app.out 2>&1
import sys
d = open(sys.argv[1], "rb").read()
head = b"P6\n3 2\n255\n"
px = d[len(head):]
# red, green, blue; white, a clear pixel (the grey behind it), and (200, 100, 40) at half strength
# with its alpha multiplied in, over that grey: (114, 89, 74) if the alpha were applied to it again
want = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (128, 128, 128), (164, 114, 84)]
slack = [0, 0, 0, 0, 0, 2]
got = [tuple(px[i:i + 3]) for i in range(0, len(px), 3)]
ok = d.startswith(head) and len(got) == 6 and all(
    abs(a - b) <= t for g, w, t in zip(got, want, slack) for a, b in zip(g, w))
print("pointer pixels:", got, "ok" if ok else "WANT %s" % want)
sys.exit(0 if ok else 1)
PY
    then
        echo "$name: the pointer AppKit draws is the image that was defined (6 pixels)"
    else
        echo "$name: FAIL: the pointer's pixels differ ($(grep '^pointer pixels' app.out | tail -1))"; rc=1
    fi
    return $rc
}

bar_leg() {
    local d=$repo/build/macrunner-app-test/bar
    mkdir -p "$d" && cd "$d" || return 1
    rm -f q.sock trace.log app.out qemu.out
    "$qemu" -machine virt,accel=tcg -cpu cortex-a72 -m 256 -S -nodefaults -display dbus,p2p=on \
        -device virtio-gpu-pci -device virtio-tablet-pci -device virtio-keyboard-pci \
        -qmp unix:q.sock,server=on,wait=off -serial none -monitor none \
        -trace 'dbus_*' -D trace.log > qemu.out 2>&1 &
    qp=$!
    local i; for i in $(seq 50); do [ -S q.sock ] && break; sleep 0.1; done
    [ -S q.sock ] || { echo "bar: FAIL: QEMU gave no QMP socket ($(head -1 qemu.out))"; return 1; }
    "$app" --attach q.sock --no-activate --exit-after 3 --scripted-bar > app.out 2>&1 &
    ap=$!
    ( sleep 20; kill "$ap" 2>/dev/null ) &
    wd=$!
    wait "$ap"; local arc=$?
    kill "$wd" 2>/dev/null; wd=""; ap=""
    kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; qp=""
    local rc=0 got view want_bar
    [ "$arc" -eq 0 ] || { echo "bar: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    # The script says what the view was: the leg is about a view one point past the 640x480
    # scanout QEMU shows before a guest runs, at a whole number of pixels to a point.
    view=$(grep '^scripted: bar: ' app.out)
    case "$view" in
        "scripted: bar: view 641x481 points at "[123]", scanout 640x480") ;;
        *) echo "bar: FAIL: the view was not a point past the scanout at a whole scale (${view:-the script did not get that far})"; rc=1 ;;
    esac
    want_bar='mouse_set_pos x=0, y=240;mouse_press b2;mouse_set_pos x=0, y=240;mouse_release b2;'
    want_bar+='mouse_set_pos x=639, y=479;mouse_press b2;mouse_set_pos x=639, y=479;mouse_release b2;'
    got=$(grep 'dbus_kbd\|dbus_mouse' trace.log | sed 's/dbus_//; s/keycode //; s/button /b/' | tr '\n' ';')
    if [ "$got" = "$want_bar" ]; then
        echo "bar: two clicks on the bars sent nothing, and the corner pixels arrived as themselves"
    else
        echo "bar: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_bar"; rc=1
    fi
    return $rc
}

# One run of the app against the size leg's QEMU: <out> <limit in seconds> <the app's arguments>.
size_run() {
    local out=$1 limit=$2; shift 2
    "$app" --attach q.sock --no-activate "$@" > "$out" 2>&1 &
    ap=$!
    ( sleep "$limit"; kill "$ap" 2>/dev/null ) &
    wd=$!
    wait "$ap"; local arc=$?
    kill "$wd" 2>/dev/null; wd=""; ap=""
    return $arc
}

size_leg() {
    local d=$repo/build/macrunner-app-test/size
    mkdir -p "$d" && cd "$d" || return 1
    rm -f q.sock t.sock qemu.out qtest.log app-a.out app-b.out seen.out
    # A device made 1024x768, which no window of this test is: a size nobody told it reads as that.
    "$qemu" -machine virt,highmem=off -accel qtest -S -m 64 -nodefaults -display dbus,p2p=on \
        -device virtio-gpu-pci,xres=1024,yres=768 \
        -qmp unix:q.sock,server=on,wait=off -qtest unix:t.sock,server=on,wait=off -qtest-log qtest.log \
        -serial none -monitor none > qemu.out 2>&1 &
    qp=$!
    local i; for i in $(seq 50); do [ -S q.sock ] && [ -S t.sock ] && break; sleep 0.1; done
    [ -S q.sock ] && [ -S t.sock ] || { echo "size: FAIL: QEMU gave no QMP and qtest sockets ($(head -1 qemu.out))"; return 1; }
    local rc=0
    { echo "before: $(python3 -I "$here/as_guest.py" t.sock 2>&1)"
      # Shorter than the wait after a change of size (view.rs, SETTLE): what run a tells, it tells
      # as it attaches.
      size_run app-a.out 15 --exit-after 0.2; echo "a exit: $?"
      echo "a: $(python3 -I "$here/as_guest.py" t.sock 2>&1)"
      size_run app-b.out 15 --exit-after 3 --resize-to 4200x700; echo "b exit: $?"
      echo "b: $(python3 -I "$here/as_guest.py" t.sock 2>&1)"; } > seen.out
    kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; qp=""
    python3 -I - <<'PY' || rc=1
import ctypes, re

seen = dict(l.rstrip("\n").split(": ", 1) for l in open("seen.out"))
def size_line(path):
    for l in open(path, errors="replace"):
        if l.startswith("size: "):
            return l[6:].strip()
    return "no size line (%s)" % (open(path, errors="replace").read().strip().splitlines() or ["no output"])[-1]

# The pixels to a point of every display there is, asked of Core Graphics and not of the app.
cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cg.CGDisplayCopyDisplayMode.restype = ctypes.c_void_p
for f in (cg.CGDisplayModeGetPixelWidth, cg.CGDisplayModeGetWidth):
    f.argtypes, f.restype = [ctypes.c_void_p], ctypes.c_size_t
ids, n = (ctypes.c_uint32 * 16)(), ctypes.c_uint32(0)
cg.CGGetActiveDisplayList(16, ids, ctypes.byref(n))
scales = set()
for i in range(n.value):
    mode = cg.CGDisplayCopyDisplayMode(ids[i])
    if mode and cg.CGDisplayModeGetWidth(mode):
        scales.add(cg.CGDisplayModeGetPixelWidth(mode) / cg.CGDisplayModeGetWidth(mode))

bad = []
def check(what, ok, detail):
    print("size: %s%s" % ("" if ok else "FAIL: ", what if ok else "%s: %s" % (what, detail)))
    if not ok:
        bad.append(what)

before = seen.get("before", "")
check("the device as it was made answers 1024x768, with no event (%s)" % before,
      re.fullmatch(r"display 1024x768 on 1, edid \d+x\d+ mm prefers 1024x768, event 0", before) is not None, before)

# Whether QEMU's EDID has a detailed timing for a mode: not when a side is 4096 or more, or the
# pixel clock at the 75 Hz it takes for a display that named no rate is past the field's 16 bits
# (hw/display/edid-generate.c, qemu_edid_generate).
def timing(px):
    clock = 75000 * (px[0] + px[0] * 35 // 100) * (px[1] + px[1] * 35 // 1000) // 10000000
    return px[0] < 4096 and px[1] < 4096 and clock < 65536

def told(run, points, tellings):
    line = size_line("app-%s.out" % run)
    m = re.fullmatch(r"view ([\d.]+)x([\d.]+) points at ([\d.]+), told (\d+)x(\d+) px (\d+)x(\d+) mm, (\d+) tellings", line)
    if seen.get("%s exit" % run) != "0" or not m:
        check("run %s" % run, False, "the app exited %s; QEMU answers '%s'; %s" % (seen.get("%s exit" % run), seen.get(run), line))
        return
    w, h, scale = float(m[1]), float(m[2]), float(m[3])
    if points and (w, h) != points:
        check("run %s: the view is the size the window was set to" % run, False, line)
        return
    if not scales:
        print("size: %s: no display's pixels to a point could be read, so the app's own %g is not checked" % (run, scale))
    elif scale not in scales:
        check("run %s: the app's pixels to a point are a display's" % run, False, "%s, and the displays have %s" % (line, sorted(scales)))
        return
    px = (round(w * scale), round(h * scale))
    mm = (round(w * 25.4 / 96), round(h * 25.4 / 96))
    if timing(px):
        want = "display %dx%d on 1, edid %dx%d mm prefers %dx%d, event 1" % (px + mm + px)
    else:
        want = "display %dx%d on 1, edid %dx%d cm no preferred timing, event 1" % (px + (mm[0] // 10, mm[1] // 10))
    if points and timing(px):
        check("run %s: the size set is one QEMU has no detailed timing for" % run, False, line)
        return
    got = seen.get(run, "")
    check("%s: QEMU answers a guest with the view's %gx%g points at %g (%s)" % (run, w, h, scale, got),
          got == want, "got '%s', want '%s'; the app says: %s" % (got, want, line))
    check("%s: told in %d" % (run, tellings),
          int(m[8]) == tellings and (int(m[4]), int(m[5]), int(m[6]), int(m[7])) == px + mm, line)

told("a", None, 1)
told("b", (4200.0, 700.0), 2)
# The size told had stood when the content went a point wider and back: two tellings by then,
# and (above) two at the end.
back = [l.strip() for l in open("app-b.out", errors="replace") if l.startswith("scripted: content set a point wider")]
check("b: a size that changed and came back is not told again",
      back == ["scripted: content set a point wider and back, after 2 tellings"], back or "the script did not get that far")
raise SystemExit(1 if bad else 0)
PY
    return $rc
}

rc=0
pixels="mapped";          leg map || rc=1
pixels="in the messages"; leg in-band THYLACINE_NO_MAP=1 || rc=1
size_leg || rc=1
bar_leg || rc=1
[ $rc -eq 0 ] && echo "PASS: macrunner/app headless" || echo "FAIL: macrunner/app headless (evidence in $repo/build/macrunner-app-test)"
exit $rc
