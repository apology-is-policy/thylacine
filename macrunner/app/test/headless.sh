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
# the tablet and the buttons to the mouse, and the leg says so. The app asks QEMU at the attach
# what the tablet is bound to, says once that it is not the display, and sends a place the guest
# was not sent yet ahead of its button, in a command of its own.
#
# Then the click leg, on the first machine: the pointer script a running guest is tried with
# (view.rs, scripted_click), which the legs above do not run.
#   click    the middle of the picture, a left click on pixel 600,108 and the last pixel, half a
#            second apart, arrive as those pixels: four reports on the tablet, the press and the
#            release each with the place, and nothing on the mouse. The script says each step as
#            the pixel the view sent, and the app prints the one place QEMU gives for a pointer
#            when there is no guest to move it.
#   stopped  the same script while that machine is stopped, which is when QEMU's monitor refuses
#            the pointer: QEMU is handed nothing and the devices hand their driver nothing; the
#            app says that QEMU refused, and is refused three events and not four, because the
#            release of a press QEMU refused is not sent; and the app still finds the display
#            and what the tablet is bound to, which a stopped machine answers another way.
# The legs do not look at what is drawn. The window opens four times more, for three seconds each.
#
# Then the capture legs, one on each of those two machines: the captured pointer as a person would
# use it (view.rs, scripted_capture), except that the window is not key, so the Mac's own pointer
# is left alone, and the script says that it was.
#   capture  two moves, a left press between them and its release after, a notch and a right press
#            reach the guest's mouse, and its tablet nothing of them. Each move arrives as the
#            whole guest pixels its points make at the view's scale, the part of a pixel left over
#            carried into the next: worked out here from the points and the scale the script says,
#            and not taken from the app. The pointer is given back with the right button down, and
#            that button's release still goes to the mouse. A click after it is the tablet's again,
#            with its place. On the machine whose tablet is bound to nothing that click's place
#            goes ahead of it, and its button reaches the mouse, as in the bar leg.
#   drawn    with the pointer captured again, the picture read back is QEMU's screendump but for
#            the guest's pointer: the image the script defined, its hot spot on the place the
#            guest was said to have it at, the clear pixel showing the picture and the pixel at
#            half strength blended once.
# What the Mac does with its own pointer under a capture (hidden, held, put back) no leg tries: a
# test that did would take the pointer from whoever is at the Mac.
#
# Last the captured click leg, on the first machine: the captured pointer's script for a guest
# that is running (view.rs, scripted_captured_click), which the capture legs do not run.
#   captured-click  the pointer to pixel 600,108 as the tablet's; then captured and moved 200
#            guest pixels left and 100 down, in the whole points that is at the view's scale;
#            then a left click. The script says each step, and the leg works out from the points
#            and the scale it says what the mouse must be handed: one report on the tablet, the
#            place, and three on the mouse, the motion, the press and the release.
# The window opens three times more, for three seconds each.
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

# start_qemu <leg> [the tablet's options [-S]]: in the leg's directory, the machine the header
# describes, its input devices up. With no options the tablet is bound to the display. -S leaves
# the machine stopped; its devices are brought up all the same.
start_qemu() {
    local tablet=${2-,display=gpu0,head=0} stopped=${3-}
    rm -f q.sock t.sock trace.log qemu.out up.out mice.out
    "$qemu" -machine virt,highmem=off -accel qtest $stopped -m 64 -nodefaults -display dbus,p2p=on \
        -device virtio-gpu-pci,id=gpu0 -device virtio-keyboard-pci \
        -device "virtio-tablet-pci,id=tab-pci0$tablet" -device virtio-mouse-pci \
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

# said_of_qemu <leg> [unbound]: what the app must not have said of this machine, and of one whose
# tablet is bound to nothing what it must have said, once and with QEMU's answer.
said_of_qemu() {
    local rc=0 said
    if grep -q '^input: QEMU refused' app.out; then
        echo "$1: FAIL: QEMU refused input ($(grep '^input: QEMU refused' app.out | tail -1))"; rc=1
    fi
    if grep -q '^input: gpu0 is not a display' app.out; then
        echo "$1: FAIL: the app did not find the display its events are addressed to"; rc=1
    fi
    said=$(grep '^input: the tablet ' app.out)
    case "${2-}:$said" in
        :) ;;
        'unbound:input: the tablet tab-pci0 is not known to be bound to gpu0 (its display is ""); '*) ;;
        *) echo "$1: FAIL: what the app said of the tablet's binding is not this machine's (${said:-nothing})"; rc=1 ;;
    esac
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

# bar_leg <name> <the tablet's options> <what QEMU is handed> <the tablet's reports> <the mouse's>
#         <what that shows>
bar_leg() {
    local name=$1 want_bar=$3 want_tablet=$4 want_mouse=$5
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out
    start_qemu "$name" "$2" || return 1
    run_app 20 -- --exit-after 3 --scripted-bar; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    quit_qemu
    local rc=0 got view
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    # The script says what the view was: the leg is about a view one point past the 640x480
    # scanout QEMU shows before a guest runs, at a whole number of pixels to a point.
    view=$(grep '^scripted: bar: ' app.out)
    case "$view" in
        "scripted: bar: view 641x481 points at "[123]", scanout 640x480") ;;
        *) echo "$name: FAIL: the view was not a point past the scanout at a whole scale (${view:-the script did not get that far})"; rc=1 ;;
    esac
    got=$(seen 640 480)
    if [ "$got" = "$want_bar" ]; then
        echo "$name: two clicks on the bars sent nothing, and the corner pixels arrived as themselves"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_bar"; rc=1
    fi
    if [ "$(cat heard.out)" = "keyboard: nothing
tablet: $want_tablet
mouse: $want_mouse" ]; then
        echo "$name: $6"
    else
        echo "$name: FAIL: the devices handed their driver something else"; sed 's/^/  got:  /' heard.out
        echo "  want: tablet: $want_tablet"; echo "  want: mouse: $want_mouse"; rc=1
    fi
    local how=""; [ -n "$2" ] || how=unbound
    said_of_qemu "$name" $how || rc=1
    return $rc
}

# click_leg: the paced pointer script, on the machine with the tablet bound to the display.
click_leg() {
    local name=click
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out
    start_qemu "$name" || return 1
    run_app 20 THYLACINE_ECHO_POINTER=1 -- --exit-after 3 --scripted-click 600,108; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    quit_qemu
    local rc=0 got said want_click
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    want_click='at 320,240;at 600,108 press left;at 600,108 release left;at 639,479;'
    got=$(seen 640 480)
    if [ "$got" = "$want_click" ]; then
        echo "$name: the script's middle, its click and the last pixel arrived as themselves"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_click"; rc=1
    fi
    if [ "$(cat heard.out)" = "keyboard: nothing
tablet: $want_click
mouse: nothing" ]; then
        echo "$name: four reports on the tablet's driver, the press and the release each with the place"
    else
        echo "$name: FAIL: the devices handed their driver something else"; sed 's/^/  got:  /' heard.out
        echo "  want: tablet: $want_click"; rc=1
    fi
    # The script says each step and the pixel it meant: what a caller with a guest compares the
    # guest's own account with.
    said=$(grep '^scripted: pointer ' app.out | tr '\n' ';')
    [ "$said" = 'scripted: pointer to the middle, guest pixel 320,240 of 640x480;scripted: pointer a left click, guest pixel 600,108 of 640x480;scripted: pointer to the last pixel, guest pixel 639,479 of 640x480;' ] \
        || { echo "$name: FAIL: the script did not say its three steps as they were ($said)"; rc=1; }
    # That caller reads the guest's account from what THYLACINE_ECHO_POINTER=1 prints. With no
    # guest QEMU gives a pointer's place once, as the app registers with it.
    said=$(grep '^pointer: ' app.out | tr '\n' ';')
    [ "$said" = 'pointer: the guest has its pointer at 0,0, shown 0;' ] \
        || { echo "$name: FAIL: the place QEMU gave for its pointer was not printed once as it was (${said:-nothing})"; rc=1; }
    said_of_qemu "$name" || rc=1
    return $rc
}

# stopped_leg: the paced pointer script on the first machine while it is stopped.
stopped_leg() {
    local name=stopped
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out
    start_qemu "$name" ,display=gpu0,head=0 -S || return 1
    run_app 20 -- --exit-after 3 --scripted-click 600,108; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    quit_qemu
    local rc=0 got said
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    got=$(seen 640 480)
    if [ -z "$got" ] && [ "$(cat heard.out)" = "keyboard: nothing
tablet: nothing
mouse: nothing" ]; then
        echo "$name: QEMU was handed nothing for a machine that is stopped, and its devices handed their driver nothing"
    else
        echo "$name: FAIL: something reached a machine that is stopped"; echo "  got:  $got"; sed 's/^/  got:  /' heard.out; rc=1
    fi
    # The view sent the script's four events. QEMU refuses three of them: the release of a press
    # it refused is not sent. The app says the first refusal and each doubling of their number,
    # and of this machine nothing else: its display and its tablet's binding were found.
    said=$(grep '^input: ' app.out | tr '\n' ';')
    if [ "$said" = 'input: QEMU refused an event (VM not running); 1 so far;input: QEMU refused an event (VM not running); 2 so far;' ]; then
        echo "$name: QEMU refused the two places and the press, and the app found the display and the tablet's binding all the same"
    else
        echo "$name: FAIL: what the app said of its input is not three refusals and nothing else"; echo "  got:  ${said:-nothing}"; rc=1
    fi
    [ "$(grep -c '^scripted: pointer ' app.out)" = 3 ] \
        || { echo "$name: FAIL: the script did not run its three steps ($(grep '^scripted' app.out | tail -1))"; rc=1; }
    return $rc
}

# capture_leg <name> <the tablet's options> <what QEMU is handed after the mouse's events>
#             <the tablet's reports> <the mouse's after its own> <what that shows>
capture_leg() {
    local name=$1 want_after=$3 want_tablet=$4 want_mouse_after=$5
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out app.ppm app-pointer.ppm screendump.ppm moves.out drawn.out
    start_qemu "$name" "$2" || return 1
    run_app 20 -- --exit-after 3 --scripted-capture --dump app.ppm; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    qmp "{\"execute\": \"screendump\", \"arguments\": {\"filename\": \"$PWD/screendump.ppm\"}}" >> app.out 2>&1
    quit_qemu
    local rc=0 got said scale want_seen want_mouse
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    # The script says what it did: the Mac's pointer left alone, the two moves as AppKit read them
    # back from the events it was handed, and the view's points to a guest pixel.
    said=$(grep '^scripted: captured pointer: ' app.out)
    scale=$(printf '%s\n' "$said" | sed -n "s/^scripted: captured pointer: the Mac's pointer held: false; moves of 51,-26 and -20,10 points at \([0-9][0-9.]*\) points to a guest pixel; the guest's pointer drawn at 100,60 of 640x480\$/\1/p")
    if [ -z "$scale" ]; then
        echo "$name: FAIL: the script did not say the capture it was to make (${said:-it did not get that far})"
        return 1
    fi
    # Each move in whole guest pixels, what is left of a pixel carried into the next move; an axis
    # that comes to nothing is not sent. First as QEMU's input layer is handed them, then as the
    # mouse hands them to its driver.
    python3 -I - "$scale" > moves.out <<'PY'
import math, sys
scale = float(sys.argv[1])
carry, moves = [0.0, 0.0], []
for move in ((51, -26), (-20, 10)):
    whole = []
    for i, points in enumerate(move):
        pixels = carry[i] + points / scale
        whole.append(math.trunc(pixels))
        carry[i] = pixels - whole[i]
    moves.append(whole)
def handed(m):
    parts = ["rel %s %d" % (axis, v) for axis, v in zip("xy", m) if v]
    return "con -1: %s;" % " ".join(parts) if parts else ""
def heard(m):
    parts = ["rel %#x %d" % (code, v) for code, v in enumerate(m) if v]
    return "%s;" % " ".join(parts) if parts else ""
print(handed(moves[0]) + "con -1: press left;" + handed(moves[1])
      + "con -1: release left;con -1: wheel-up;con -1: press right;con -1: release right;")
print(heard(moves[0]) + "press left;" + heard(moves[1]) + "release left;wheel-up;press right;release right;")
print("%d,%d and %d,%d" % (moves[0][0], moves[0][1], moves[1][0], moves[1][1]))
PY
    { read -r want_seen; read -r want_mouse; read -r said; } < moves.out
    got=$(seen 640 480)
    if [ "$got" = "$want_seen$want_after" ]; then
        echo "$name: the captured pointer's moves arrived as $said guest pixels ($scale points to one), addressed to no display, with its buttons and its notch"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_seen$want_after"; rc=1
    fi
    if [ "$(cat heard.out)" = "keyboard: nothing
tablet: $want_tablet
mouse: $want_mouse$want_mouse_after" ]; then
        echo "$name: $6"
    else
        echo "$name: FAIL: the devices handed their driver something else"; sed 's/^/  got:  /' heard.out
        echo "  want: tablet: $want_tablet"; echo "  want: mouse: $want_mouse$want_mouse_after"; rc=1
    fi
    if python3 -I - > drawn.out 2>&1 <<'PY'
import sys
W, H = 640, 480
def ppm(path):
    d = open(path, "rb").read()
    head = b"P6\n%d %d\n255\n" % (W, H)
    if not d.startswith(head) or len(d) != len(head) + W * H * 3:
        sys.exit("%s is not a %dx%d picture" % (path, W, H))
    return d[len(head):]
app, qemu = ppm("app.ppm"), ppm("screendump.ppm")
def px(d, x, y):
    return tuple(d[(y * W + x) * 3:(y * W + x) * 3 + 3])
# The 3x2 image with its hot spot, the clear pixel, on 100,60: red, green, blue; white, the picture,
# and (100, 50, 20) with half the picture under it: the colour has its alpha multiplied in.
want = {(99, 59): (255, 0, 0), (100, 59): (0, 255, 0), (101, 59): (0, 0, 255), (99, 60): (255, 255, 255),
        (100, 60): px(qemu, 100, 60),
        (101, 60): tuple(round(c + u * 127 / 255) for c, u in zip((100, 50, 20), px(qemu, 101, 60)))}
slack = {(101, 60): 2}
bad = []
for at, w in sorted(want.items()):
    g = px(app, *at)
    if at != (100, 60) and px(qemu, *at) == w:
        bad.append("QEMU's picture has %s at %d,%d already, which shows nothing" % (w, at[0], at[1]))
    if any(abs(a - b) > slack.get(at, 0) for a, b in zip(g, w)):
        bad.append("%d,%d is %s, want %s" % (at[0], at[1], g, w))
others = [(i // 3 % W, i // 3 // W) for i in range(0, len(app), 3)
          if app[i:i + 3] != qemu[i:i + 3] and (i // 3 % W, i // 3 // W) not in want]
if others:
    bad.append("%d pixels off the pointer differ from QEMU's screendump, the first at %d,%d" % (len(others), others[0][0], others[0][1]))
print("; ".join(bad) if bad else "ok")
sys.exit(1 if bad else 0)
PY
    then
        echo "$name: the picture is QEMU's screendump but for the guest's pointer, drawn with its hot spot on 100,60"
    else
        echo "$name: FAIL: the guest's pointer is not drawn as defined ($(tail -1 drawn.out))"; rc=1
    fi
    local how=""; [ -n "$2" ] || how=unbound
    said_of_qemu "$name" $how || rc=1
    return $rc
}

# captured_click_leg: the paced captured-pointer script, on the machine with the tablet bound.
captured_click_leg() {
    local name=captured-click
    local d=$repo/build/macrunner-app-test/$name
    mkdir -p "$d" && cd "$d" || return 1
    rm -f app.out heard.out moves.out
    start_qemu "$name" || return 1
    run_app 20 -- --exit-after 3 --scripted-captured-click 600,108,-200,100; local arc=$?
    python3 -I "$here/as_input_guest.py" t.sock reports 640 480 > heard.out 2>&1
    quit_qemu
    local rc=0 got said want_seen want_mouse
    [ "$arc" -eq 0 ] || { echo "$name: FAIL: the app exited $arc ($(tail -1 app.out))"; rc=1; }
    # The script's three steps as it says them. From the second, the points it moved and the
    # scale: they must be the whole points 200 pixels left and 100 down are at that scale, and
    # they make the guest pixels the mouse is handed (no remainder is carried into a first move).
    said=$(grep '^scripted: captured click: ' app.out | tr '\n' ';')
    if ! python3 -I - "$said" > moves.out 2>&1 <<'PY'
import math, re, sys
m = re.fullmatch(r"scripted: captured click: the pointer to guest pixel 600,108 of 640x480;"
                 r"scripted: captured click: captured, the Mac's pointer held: false; a move of (-?\d+),(-?\d+) points at ([0-9][0-9.]*) points to a guest pixel;"
                 r"scripted: captured click: a left click;", sys.argv[1])
if not m:
    sys.exit("the script did not say its three steps as they were (%s)" % (sys.argv[1] or "it said nothing"))
px, py, scale = int(m[1]), int(m[2]), float(m[3])
def whole(v):  # Rust's f64::round: halves away from zero
    return int(math.copysign(math.floor(abs(v) + 0.5), v))
if (px, py) != (whole(-200 * scale), whole(100 * scale)):
    sys.exit("the script moved %d,%d points, which is not 200 pixels left and 100 down at %r points to a pixel" % (px, py, scale))
gx, gy = math.trunc(px / scale), math.trunc(py / scale)
handed = " ".join("rel %s %d" % (a, v) for a, v in (("x", gx), ("y", gy)) if v)
heard = " ".join("rel %#x %d" % (c, v) for c, v in ((0, gx), (1, gy)) if v)
print("at 600,108;con -1: %s;con -1: press left;con -1: release left;" % handed)
print("%s;press left;release left;" % heard)
print("%d,%d guest pixels of %d,%d points at %r points to one" % (gx, gy, px, py, scale))
PY
    then
        echo "$name: FAIL: $(tail -1 moves.out)"
        return 1
    fi
    { read -r want_seen; read -r want_mouse; read -r said; } < moves.out
    got=$(seen 640 480)
    if [ "$got" = "$want_seen" ]; then
        echo "$name: the place, then the captured move as $said, and the click, addressed to no display"
    else
        echo "$name: FAIL: input differs"; echo "  got:  $got"; echo "  want: $want_seen"; rc=1
    fi
    if [ "$(cat heard.out)" = "keyboard: nothing
tablet: at 600,108;
mouse: $want_mouse" ]; then
        echo "$name: one report on the tablet's driver, the place, and three on the mouse's: the motion, the press and the release"
    else
        echo "$name: FAIL: the devices handed their driver something else"; sed 's/^/  got:  /' heard.out
        echo "  want: tablet: at 600,108;"; echo "  want: mouse: $want_mouse"; rc=1
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
unnamed = [l.strip() for l in open("app-a.out", errors="replace") if l.startswith("input: gpu0 is not a display of this QEMU (Device 'gpu0' not found)")]
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
clicks='at 0,240 press right;at 0,240 release right;at 639,479 press right;at 639,479 release right;'
bar_leg bar ,display=gpu0,head=0 "$clicks" "$clicks" nothing \
    "each click reached the tablet's driver as one report of a place and a button, and the mouse's nothing" || rc=1
bar_leg bar-unbound "" \
    'at 0,240;at 0,240 press right;at 0,240 release right;at 639,479;at 639,479 press right;at 639,479 release right;' \
    'at 0,240;at 0,240;at 0,240;at 639,479;at 639,479;at 639,479;' 'press right;release right;press right;release right;' \
    "with the tablet bound to nothing each place went ahead of its press; the places reached the tablet's driver and the buttons the mouse's" || rc=1
click_leg || rc=1
stopped_leg || rc=1
capture_leg capture ,display=gpu0,head=0 'at 320,240 press left;at 320,240 release left;' \
    'at 320,240 press left;at 320,240 release left;' '' \
    "all of it reached the mouse's driver, the right button's release after the pointer was given back too; the click after that reached the tablet's with its place" || rc=1
capture_leg capture-unbound "" 'at 320,240;at 320,240 press left;at 320,240 release left;' \
    'at 320,240;at 320,240;at 320,240;' 'press left;release left;' \
    "with the tablet bound to nothing the same, and of the click after it the place went ahead to the tablet's driver and the button to the mouse's" || rc=1
captured_click_leg || rc=1
[ $rc -eq 0 ] && echo "PASS: macrunner/app headless" || echo "FAIL: macrunner/app headless (evidence in $repo/build/macrunner-app-test)"
exit $rc
