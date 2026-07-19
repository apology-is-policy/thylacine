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
#
# Lowercase letters, digits, space, '-', '.', '/' and '\n' only (the
# scenario vocabulary); anything else is a hard error, not a silent skip.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

sock="$REPO_ROOT/build/qmp.sock"

while getopts "s:h" opt; do
    case "$opt" in
        s) sock="$OPTARG" ;;
        *) echo "usage: qmp-sendtext.sh [-s SOCK] TEXT" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "usage: qmp-sendtext.sh [-s SOCK] TEXT" >&2; exit 2; }

exec python3 - "$sock" "$1" <<'PYEOF'
import json, socket, sys, time

sock_path, text = sys.argv[1], sys.argv[2]

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


msg()  # greeting
cmd("qmp_capabilities")
text = text.replace("\\n", "\n")
for ch in text:
    q = QCODE.get(ch)
    if q is None:
        print(f"qmp-sendtext: unsupported char {ch!r}", file=sys.stderr)
        sys.exit(1)
    for down in (True, False):
        cmd("input-send-event",
            events=[{"type": "key",
                     "data": {"down": down,
                              "key": {"type": "qcode", "data": q}}}])
    time.sleep(0.03)  # keystroke pacing (the guest drains per FRAME tick)
print(f"qmp-sendtext: typed {text!r}")
PYEOF
