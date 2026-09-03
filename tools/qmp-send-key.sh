#!/usr/bin/env bash
# tools/qmp-send-key.sh -- inject a keystroke into the running dev VM over the
# QMP control socket tools/run-vm.sh opens (build/qmp.sock), the same socket
# tools/screendump.sh uses. QEMU's `send-key` delivers QKeyCodes to the guest's
# active keyboard; in a graphical Thylacine boot that is the virtio-keyboard-PCI
# function tapestryd (the compositor) owns, so the key routes to the FOCUSED
# surface -> the SDL_thylacine event pump -> the client (e.g. DOSBox-X). This is
# the "agentic fingers" companion to screendump.sh's "agentic eyes": it lets an
# interactive gate drive a graphical DOS/SDL program and verify what it did.
#
#   tools/qmp-send-key.sh a                     # press qcode 'a'
#   tools/qmp-send-key.sh -s /path/qmp.sock 7   # explicit socket, press '7'
#   tools/qmp-send-key.sh ret                   # press Enter (qcode 'ret')
#   tools/qmp-send-key.sh ctrl c                # a chord: Ctrl held with 'c'
#
# Key names are QEMU QKeyCodes (a, b, ..., 0-9, ret, spc, esc, shift, ctrl,
# alt, up/down/left/right, f1-f12, ...). Multiple names form a single chord
# (all pressed together, released together) -- the QEMU send-key semantics.
#
# Exit: 0 on delivery; nonzero on QMP error or a missing socket.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

sock="${THYLACINE_QMP_SOCK:-$REPO_ROOT/build/qmp.sock}"
hold_ms=100

usage() {
    cat >&2 <<EOF
usage: tools/qmp-send-key.sh [-s QMP_SOCK] [-t HOLD_MS] QCODE [QCODE ...]

  -s QMP_SOCK   QMP unix socket (default: build/qmp.sock)
  -t HOLD_MS    key hold time in ms (default: 100)
  QCODE ...     one or more QEMU QKeyCodes; multiple = a single chord
EOF
    exit 2
}

while getopts "s:t:h" opt; do
    case "$opt" in
        s) sock="$OPTARG" ;;
        t) hold_ms="$OPTARG" ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

[[ $# -ge 1 ]] || usage

if [[ ! -S "$sock" ]]; then
    echo "qmp-send-key: no QMP socket at $sock (VM not running, or THYLACINE_NO_QMP=1)" >&2
    exit 1
fi

exec python3 - "$sock" "$hold_ms" "$@" <<'PYEOF'
import json, socket, sys

sock_path = sys.argv[1]
hold_ms = int(sys.argv[2])
qcodes = sys.argv[3:]


class Qmp:
    """Minimal QMP client (mirrors tools/screendump.sh): greeting ->
    qmp_capabilities -> command. Async events interleaving with the command
    response are skipped."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(15)
        self.s.connect(path)
        self.buf = b""
        greet = self.recv_msg()
        if "QMP" not in greet:
            raise RuntimeError(f"unexpected QMP greeting: {greet}")
        self.cmd("qmp_capabilities")

    def recv_msg(self):
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl].strip()
                self.buf = self.buf[nl + 1:]
                if not line:
                    continue
                return json.loads(line)
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("QMP socket EOF")
            self.buf += chunk

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall((json.dumps(msg) + "\n").encode())
        while True:
            resp = self.recv_msg()
            if "event" in resp:
                continue
            if "error" in resp:
                raise RuntimeError(
                    f"{name}: {resp['error'].get('desc', resp['error'])}")
            if "return" in resp:
                return resp["return"]


q = Qmp(sock_path)
keys = [{"type": "qcode", "data": c} for c in qcodes]
q.cmd("send-key", keys=keys, **({"hold-time": hold_ms} if hold_ms else {}))
print(f"qmp-send-key: sent {'+'.join(qcodes)} (hold {hold_ms}ms)")
PYEOF
