#!/usr/bin/env bash
# tools/qmp-sendtext.sh -- type text on the VM's virtio keyboard over QMP
# (G-4; the ls-gfx graphical-input leg). Sends per-char key press+release
# `input-send-event`s UN-TARGETED (no `device=`), so QEMU routes them to the
# console's input -- which under `-nographic` reaches the PCI keyboard
# tapestryd owns (device-targeting needs a QemuConsole binding that
# -nographic does not create; the un-targeted route is the one that
# delivers). The keystrokes enter tapestryd -> Aurora -> /dev/consfeed ->
# the LS-8 line discipline, exactly as a human at the graphical console. A
# trailing newline in TEXT sends Enter ("ret").
#
#   tools/qmp-sendtext.sh [-s QMP_SOCK] "whoami\n"
#   tools/qmp-sendtext.sh [-s QMP_SOCK] -k "meta_l+left"
#
# Lowercase letters, digits, space, '-', '.', '/' and '\n' only (the
# scenario vocabulary); anything else is a hard error, not a silent skip.
# -k sends ONE chord: '+'-separated qcodes pressed in order, released in
# reverse (the G-6c Super-chord leg; qcodes pass through verbatim, e.g.
# meta_l, shift, left/right/up/down, letters).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

sock="$REPO_ROOT/build/qmp.sock"
mode="text"

while getopts "s:kh" opt; do
    case "$opt" in
        s) sock="$OPTARG" ;;
        k) mode="chord" ;;
        *) echo "usage: qmp-sendtext.sh [-s SOCK] [-k] TEXT|CHORD" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "usage: qmp-sendtext.sh [-s SOCK] [-k] TEXT|CHORD" >&2; exit 2; }

exec python3 - "$sock" "$mode" "$1" <<'PYEOF'
import json, socket, sys, time

sock_path, mode, text = sys.argv[1], sys.argv[2], sys.argv[3]

QCODE = {**{c: c for c in "abcdefghijklmnopqrstuvwxyz"},
         **{c: c for c in "0123456789"},
         " ": "spc", "-": "minus", ".": "dot", "/": "slash", "\n": "ret"}

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sock_path)
buf = b""


def msg():
    global buf
    while True:
        nl = buf.find(b"\n")
        if nl >= 0:
            line = buf[:nl].strip()
            buf = buf[nl + 1:]
            if not line:
                continue
            return json.loads(line)
        chunk = s.recv(65536)
        if not chunk:
            raise RuntimeError("QMP EOF")
        buf += chunk


def cmd(name, **args):
    m = {"execute": name}
    if args:
        m["arguments"] = args
    s.sendall((json.dumps(m) + "\n").encode())
    while True:
        r = msg()
        if "event" in r:
            continue
        if "error" in r:
            raise RuntimeError(f"{name}: {r['error'].get('desc')}")
        return r


def key(qcode, down):
    cmd("input-send-event",
        events=[{"type": "key",
                 "data": {"down": down,
                          "key": {"type": "qcode", "data": qcode}}}])


msg()  # greeting
cmd("qmp_capabilities")
if mode == "chord":
    codes = [c for c in text.split("+") if c]
    if not codes:
        print("qmp-sendtext: empty chord", file=sys.stderr)
        sys.exit(1)
    for q in codes:
        key(q, True)
        time.sleep(0.03)
    for q in reversed(codes):
        key(q, False)
        time.sleep(0.03)
    print(f"qmp-sendtext: chorded {text!r}")
    sys.exit(0)
text = text.replace("\\n", "\n")
for ch in text:
    q = QCODE.get(ch)
    if q is None:
        print(f"qmp-sendtext: unsupported char {ch!r}", file=sys.stderr)
        sys.exit(1)
    key(q, True)
    key(q, False)
    time.sleep(0.03)  # keystroke pacing (the guest drains per FRAME tick)
print(f"qmp-sendtext: typed {text!r}")
PYEOF
