#!/usr/bin/env bash
# The window's test, with no guest and nobody at the keyboard. The app attaches to a QEMU that has no
# guest to run and takes a tester's reads and writes in a guest's place (qtest). The machine is
# running all the same, because QEMU's monitor refuses input for one that is stopped, and it has the
# input devices a guest of ours has: a keyboard, a tablet and a mouse, each brought up as a driver
# brings it up and in our guest's order (as_input_guest.py), which leaves the mouse the pointer
# device that came up last. The tablet is bound to the display, as the app's launcher binds it
# (tools/run-vm.sh, THYLACINE_DISPLAY=app).
# On each pixel path (the scanout mapped from QEMU's memory; pixels sent in the messages) three
# things must hold:
#   picture  what the Metal pipeline makes of the scanout at 1:1, read back, is byte-identical to QEMU's
#            own screendump of it;
#   input    a scripted run of AppKit events (view.rs, scripted_input) reaches QEMU as exactly the
#            expected run: keys as the lines of its dbus_kbd_* trace, and the pointer as the reports
#            its input layer was handed (input_event_*; `seen` below). A repeat, a modifier, keys
#            and a button let go of when the window loses the keys or a menu opens, late releases
#            that must send nothing, a modifier whose release never arrived (before a key, a click,
#            a move, and a click and a scroll over the bar, where the view itself says what the
#            guest still holds), the two Shifts told apart by their own bits and what is made of
#            them without, positions through the letterbox, each press and each notch in one report
#            with its place, a release off the picture with none, clicks and moves over the bar
#            that must send nothing, a key-up under Command, and a button still down when the app
#            leaves. QEMU must have refused nothing;
#   pointer  an image defined as the guest defines its pointer is what AppKit draws for the Mac's.
# A window opens for four seconds per path and does not take focus.
#
# Then the size leg, where nothing is drawn or typed, on a machine that is paused and whose display
# has no name: what its virtio GPU answers a driver can be read
# (as_guest.py): the display's rectangle, the millimetres in its EDID, and its display event.
#   size     the device answers with the size the app told it for the window: when the app attaches
#            (a run that ends before a change of size could have been told), and again a moment
#            after the window's content is set to another size in three steps, which is told once.
#            That size is 4200x700 points, a mode too large for QEMU's EDID to have a detailed
#            timing on any screen: the millimetres reach a guest as whole centimetres. A second
#            later the content is set a point wider and back, and nothing more is told.
#            The app is told at the attach that this QEMU has no display named gpu0, and says so.
# The window opens twice more, for a moment and for three seconds, the second time wider than the
# screen.
#
# Then the bar legs, in a window again. In the first, on the first machine, the content is set one point larger
# each way than the scanout has pixels, which with a whole number of pixels to a point is a view one
# guest pixel past a whole scale: the bound of the whole-scale rule (docs/MAC-RUNNER.md 4.2), and
# more than a guest's fitted mode leaves, which is under a guest pixel.
#   bar      the pointer is mapped as for a picture at the whole scale: a click on each bar reaches
#            QEMU as nothing, and one on the first pixel of the middle row and one on the last pixel
#            of the last row arrive as those pixels. A picture stretched to fill the view would have
#            both bar clicks land on its edge pixels. With two pixels to a point the bars are a
#            pixel on every side, and AppKit gives a window no other kind there (it made 641x481
#            of a content size of 640.5x480.5 points, macOS 27.2.1): which side an odd pixel goes
#            to, and the turn from AppKit's way up to the guest's, are the unit tests'.
#   devices  what the three input devices handed their driver over the whole leg, read as the
#            compositor reads a tablet: every click on the tablet, each as one report of a place
#            and a button, and nothing on the mouse or the keyboard.
# The second is the same on a machine whose tablet is bound to nothing, as it is under every other
# launcher. There QEMU's display connection refuses a position once the mouse's driver is up (the
# leg asks QEMU that the mouse is the pointer device it goes by), which is how the pointer was sent
# before and why it is not now. The app's events are taken all the same; QEMU hands the places to
# the tablet and the buttons to the mouse, and the leg says so.
# The legs do not look at what is drawn. The window opens twice more, for three seconds each.
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
want+='at 320,240;'                                               # a move; the click after it is the window's
want+='kbd_press 33;kbd_release 33;'                             # F
want+='at 10,10 press right;at 10,10 release right;'             # right click: each half one report, with its place
want+='kbd_press 29;kbd_release 29;'                             # Control whose release never came, let go of by a click
want+='at 10,10 press right;at 10,10 release right;'             # that click
want+='at 10,10 wheel-up;'                                       # a wheel notch up, with its place
want+='at 320,240 press left;at 320,240 release left;'           # left click
want+='at 10,10 press right;release right;'                      # after a click over the bar: a press let go of over it, with no place
want+='at 320,240 press left;release left;'                      # left held when the window loses the keys
want+='kbd_press 56;kbd_release 56;at 320,240;'                  # Option whose release never came, let go of by a move
want+='kbd_press 42;kbd_release 42;'                             # Shift likewise, by a click over the bar
want+='kbd_press 29;kbd_release 29;'                             # Control likewise, by a scroll over the bar
want+='kbd_press 32;kbd_release 32;'                             # D through NSApplication
want+='kbd_press 219;kbd_press 32;kbd_release 32;kbd_release 219;' # D under Command
want+='at 320,240 press left;release left;'                      # left down when the app leaves: let go of on the way out
lines=$(printf %s "$want" | tr -cd ';' | wc -c | tr -d ' ')

qp=""; ap=""; wd=""
stop() { for p in $wd $ap $qp; do kill "$p" 2>/dev/null; done; }
trap stop EXIT
quit_qemu() { kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; qp=""; }

# qmp <command>...: each command's answer on a line, keys in order, from a monitor connection of its
# own. QEMU's monitor serves one client at a time and the app keeps it for as long as it runs, so
# this is for before the app and after it.
qmp() {
    python3 -I - "$@" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect("q.sock"); f = s.makefile("rw")
f.readline()
for n, cmd in enumerate(['{"execute": "qmp_capabilities"}'] + sys.argv[1:]):
    f.write(cmd + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "event" not in r: break
    if n: print(json.dumps(r, sort_keys=True))
PY
}

# start_qemu <leg> [the tablet's options]: in the leg's directory, the machine the header describes,
# its input devices up. With no options the tablet is bound to the display.
start_qemu() {
    local tablet=${2-,display=gpu0,head=0}
    rm -f q.sock t.sock trace.log qemu.out up.out mice.out
    "$qemu" -machine virt,highmem=off -accel qtest -m 64 -nodefaults -display dbus,p2p=on \
        -device virtio-gpu-pci,id=gpu0 -device virtio-keyboard-pci \
        -device "virtio-tablet-pci$tablet" -device virtio-mouse-pci \
        -qmp unix:q.sock,server=on,wait=off -qtest unix:t.sock,server=on,wait=off \
        -serial none -monitor none -trace 'dbus_*' -trace 'input_event_*' -D trace.log > qemu.out 2>&1 &
    qp=$!
    local i; for i in $(seq 50); do [ -S q.sock ] && [ -S t.sock ] && break; sleep 0.1; done
    [ -S q.sock ] && [ -S t.sock ] || { echo "$1: FAIL: QEMU gave no QMP and qtest sockets ($(head -1 qemu.out))"; quit_qemu; return 1; }
    python3 -I "$here/as_input_guest.py" t.sock up > up.out 2>&1
    [ "$(tr '\n' ';' < up.out)" = 'input[0] is the keyboard;input[1] is the tablet;input[2] is the mouse;' ] \
        || { echo "$1: FAIL: the input devices did not come up as a guest's do ($(tail -1 up.out))"; quit_qemu; return 1; }
    # With the tablet bound to nothing the leg is about the state a guest of ours leaves QEMU in,
    # so that is asked of QEMU and not taken from its source: the pointer device it goes by is the
    # one that came up last, and is not an absolute one. (A console with a device bound to it goes
    # by that device, which this answer does not show.)
    qmp '{"execute": "query-mice"}' > mice.out 2>&1
    [ -n "$tablet" ] && return 0
    case "$(cat mice.out)" in
        *'{"absolute": false, "current": true, '*) ;;
        *) echo "$1: FAIL: QEMU's current pointer device is not a relative one, which is not the state a guest of ours leaves it in ($(head -c 300 mice.out))"; quit_qemu; return 1 ;;
    esac
}

# seen <width> <height>: what QEMU's trace says reached it, as one line. A key is the display
# connection's own trace line. The pointer is what the input layer was handed, gathered into the
# reports it was handed as: `at X,Y` is the pixel a guest of ours makes of the two axis values on a
# screen that size (usr/tapestryd/src/main.rs: value * (side - 1) / 32767, rounded down), and after
# it what a button did. All of it is addressed to console 0 or says which it was. A call on the
# display connection's pointer interface is given as its own trace line, which no expectation has.
seen() {
    python3 -I - trace.log "$1" "$2" <<'PY'
import re, sys
w, h = int(sys.argv[2]), int(sys.argv[3])
out, axes, rest, cons = [], {}, [], set()
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    if not line.startswith(("dbus_kbd", "dbus_mouse", "input_event")):
        continue
    if line.startswith("input_event_key"):  # a key has the display connection's line
        continue
    m = re.fullmatch(r"dbus_kbd_(press|release) keycode (\d+)", line)
    if m:
        out.append("kbd_%s %s" % m.groups()); continue
    if line.startswith("dbus_mouse"):
        out.append(line[5:]); continue
    m = re.fullmatch(r"input_event_abs con (-?\d+), axis ([xy]), value (0x[0-9a-f]+)", line)
    if m:
        cons.add(m[1]); axes[m[2]] = int(m[3], 16); continue
    m = re.fullmatch(r"input_event_btn con (-?\d+), button ([\w-]+), down ([01])", line)
    if m:
        cons.add(m[1])
        if m[2].startswith("wheel") and m[3] == "1":
            rest.append(m[2])
        else:
            rest.append("%s %s" % ("press" if m[3] == "1" else "release", m[2]))
        continue
    m = re.fullmatch(r"input_event_rel con (-?\d+), axis (\w+), value (-?\d+)", line)
    if m:
        cons.add(m[1]); rest.append("rel %s %s" % (m[2], m[3])); continue
    if line == "input_event_sync":
        parts = []
        if set(axes) == {"x", "y"}:
            parts.append("at %d,%d" % (min(axes["x"], 32767) * (w - 1) // 32767, min(axes["y"], 32767) * (h - 1) // 32767))
        elif axes:
            parts.append("ONE AXIS ONLY %r" % axes)
        parts += rest
        if parts:
            if cons != {"0"}:
                parts.insert(0, "con %s:" % ",".join(sorted(cons)))
            out.append(" ".join(parts))
        axes, rest, cons = {}, [], set()
        continue
    out.append("UNREAD %s" % line)
if axes or rest:
    out.append("NO SYNC AFTER %r %r" % (axes, rest))
print("".join(o + ";" for o in out), end="")
PY
}

# run_app <limit in seconds> [VAR=value ...] -- <the app's arguments>: the app against q.sock, its
# output in app.out. It leaves by itself; the limit bounds a hang.
run_app() {
    local limit=$1; shift
    local vars=()
    while [ "$1" != -- ]; do vars+=("$1"); shift; done
    shift
    env ${vars[@]+"${vars[@]}"} "$app" --attach q.sock --no-activate "$@" > app.out 2>&1 &
    ap=$!
    ( sleep "$limit"; kill "$ap" 2>/dev/null ) &
    wd=$!
    wait "$ap"; local arc=$?
    kill "$wd" 2>/dev/null; wd=""; ap=""
    return $arc
}

# said_of_qemu <leg>: what the app must not have said of this machine.
said_of_qemu() {
    local rc=0
    if grep -q '^input: QEMU refused' app.out; then
        echo "$1: FAIL: QEMU refused input ($(grep '^input: QEMU refused' app.out | tail -1))"; rc=1
    fi
    if grep -q '^input: this QEMU has no device named' app.out; then
        echo "$1: FAIL: the app did not find the display its events are addressed to"; rc=1
    fi
    return $rc
}

# leg <name> [VAR=value ...]: one attach, in its own directory so the socket's path stays short.
leg() {
    local name=$1; shift
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.ppm app-pointer.ppm screendump.ppm app.out
    start_qemu "$name" || return 1
    run_app 20 "$@" -- --exit-after 4 --dump app.ppm --scripted-input; local arc=$?
    qmp "{\"execute\": \"screendump\", \"arguments\": {\"filename\": \"$PWD/screendump.ppm\"}}" >> app.out 2>&1
    quit_qemu
    local rc=0 got
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    if cmp -s app.ppm screendump.ppm; then
        echo "$name: picture is QEMU's screendump, byte for byte ($(wc -c < app.ppm | tr -d ' ') bytes)"
    else
        echo "$name: FAIL: the picture differs from QEMU's screendump, or one is missing"; rc=1
    fi
    got=$(seen 640 480)
    if [ "$got" = "$want" ]; then
        echo "$name: input arrived as scripted ($lines keys and reports)"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want"; rc=1
    fi
    said_of_qemu "$name" || rc=1
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

# bar_leg <name> <the tablet's options> <the tablet's reports> <the mouse's> <what that shows>
bar_leg() {
    local name=$1 want_tablet=$3 want_mouse=$4
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out
    start_qemu "$name" "$2" || return 1
    run_app 20 -- --exit-after 3 --scripted-bar; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    quit_qemu
    local rc=0 got view want_bar
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    # The script says what the view was: the leg is about a view one point past the 640x480
    # scanout QEMU shows before a guest runs, at a whole number of pixels to a point.
    view=$(grep '^scripted: bar: ' app.out)
    case "$view" in
        "scripted: bar: view 641x481 points at "[123]", scanout 640x480") ;;
        *) echo "$name: FAIL: the view was not a point past the scanout at a whole scale (${view:-the script did not get that far})"; rc=1 ;;
    esac
    want_bar='at 0,240 press right;at 0,240 release right;at 639,479 press right;at 639,479 release right;'
    got=$(seen 640 480)
    if [ "$got" = "$want_bar" ]; then
        echo "$name: two clicks on the bars sent nothing, and the corner pixels arrived as themselves"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_bar"; rc=1
    fi
    if [ "$(cat heard.out)" = "keyboard: nothing
tablet: $want_tablet
mouse: $want_mouse" ]; then
        echo "$name: $5"
    else
        echo "$name: FAIL: the devices handed their driver something else"; sed 's/^/  got:  /' heard.out
        echo "  want: tablet: $want_tablet"; echo "  want: mouse: $want_mouse"; rc=1
    fi
    said_of_qemu "$name" || rc=1
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
# This machine's display has no name, and the app found that out at the attach.
unnamed = [l.strip() for l in open("app-a.out", errors="replace") if l.startswith("input: this QEMU has no device named gpu0")]
check("a: the app says this QEMU has no display named gpu0", len(unnamed) == 1, unnamed or "it did not say so")
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
bar_leg bar ,display=gpu0,head=0 \
    'at 0,240 press right;at 0,240 release right;at 639,479 press right;at 639,479 release right;' nothing \
    "each click reached the tablet's driver as one report of a place and a button, and the mouse's nothing" || rc=1
bar_leg bar-unbound "" \
    'at 0,240;at 0,240;at 639,479;at 639,479;' 'press right;release right;press right;release right;' \
    "with the tablet bound to nothing the places reached the tablet's driver and the buttons the mouse's" || rc=1
[ $rc -eq 0 ] && echo "PASS: macrunner/app headless" || echo "FAIL: macrunner/app headless (evidence in $repo/build/macrunner-app-test)"
exit $rc
