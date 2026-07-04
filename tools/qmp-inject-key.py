#!/usr/bin/env python3
"""tools/qmp-inject-key.py -- QMP key injector for tools/test.sh (P4-K-events).

A single long-lived process spawned AT QEMU LAUNCH (not on sentinel match):
it pays interpreter startup + QMP connect + capabilities negotiation during
the guest's multi-second boot, then tail-follows the boot log and issues
`send-key` within ~25 ms of the AWAITING_QMP_KEY sentinel appearing.

Why this shape (#362): the previous injector was a bash grep-poll loop that
spawned python3 only AFTER observing the sentinel -- interpreter startup +
socket connect + capabilities negotiation all landed INSIDE the guest
probe's poll window, and under gate load (tools/ci-smp-gate.sh: 4-8 vCPU
threads starving host-side helpers) it lost the race in 23/40 boots. The
guest probe's window is wall-clock-bounded as of #362 (usr/virtio-input),
but the injector's critical path must stay small regardless.

Exits 0 in every delivery outcome: enforcement is tools/test.sh's job (it
greps for the success marker post-boot). Every stage is timestamped to
stdout (redirected to build/test-inject.log by the caller).

usage: qmp-inject-key.py SOCK LOG SENTINEL SUCCESS_MARKER TIMEOUT_S
"""

import json
import socket
import sys
import time


def lg(msg):
    now = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(now))
    print(f"[{stamp}.{int(now % 1 * 1e6):06d}] {msg}", flush=True)


def qmp_response(f):
    """Read lines until a command response ('return'/'error'), skipping
    asynchronous QMP event notifications."""
    while True:
        line = f.readline()
        if not line:
            raise OSError("qmp: connection closed")
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if "return" in obj or "error" in obj:
            return obj


def qmp_connect(sock_path, deadline):
    """Connect + drain greeting + negotiate capabilities, retrying until
    deadline. The socket exists as soon as QEMU parses -qmp (server,nowait),
    well before the guest reaches the input probe, so this normally succeeds
    on the first few tries."""
    while time.time() < deadline:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(2.0)
            s.connect(sock_path)
            f = s.makefile("rw", buffering=1)
            greeting = f.readline()
            lg(f"qmp: connected; greeting {greeting.strip()[:100]}")
            f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
            f.flush()
            resp = qmp_response(f)
            lg(f"qmp: capabilities {json.dumps(resp)[:100]}")
            return s, f
        except OSError:
            s.close()
            time.sleep(0.05)
    return None, None


def send_key(f, qcode):
    f.write(json.dumps({
        "execute": "send-key",
        "arguments": {"keys": [{"type": "qcode", "data": qcode}]},
    }) + "\n")
    f.flush()
    return qmp_response(f)


class LogTail:
    """Incremental substring watcher over a growing log file. Keeps only a
    small carry buffer (a marker could straddle a read boundary), so each
    poll costs O(new bytes) even on multi-MB boot logs."""

    def __init__(self, path, carry_max):
        self.path = path
        self.carry_max = carry_max
        self.fp = None
        self.carry = b""

    def saw(self, marker):
        if self.fp is None:
            try:
                self.fp = open(self.path, "rb")
            except FileNotFoundError:
                return False
        chunk = self.fp.read()
        if not chunk:
            return False
        buf = self.carry + chunk
        self.carry = buf[-self.carry_max:]
        return marker.encode() in buf

    def wait_for(self, marker, deadline, poll_s):
        while time.time() < deadline:
            if self.saw(marker):
                return True
            time.sleep(poll_s)
        return False


def main():
    if len(sys.argv) != 6:
        sys.exit("usage: qmp-inject-key.py SOCK LOG SENTINEL SUCCESS_MARKER TIMEOUT_S")
    sock_path, log_path, sentinel, success = sys.argv[1:5]
    deadline = time.time() + float(sys.argv[5])
    tail = LogTail(log_path, max(len(sentinel), len(success)) + 8)

    lg(f"injector: start (sock={sock_path})")
    s, f = qmp_connect(sock_path, deadline)
    if s is None:
        lg("injector: QMP socket never became reachable; giving up")
        return

    if not tail.wait_for(sentinel, deadline, 0.02):
        lg("injector: sentinel never appeared; exiting")
        return
    lg("injector: sentinel observed; sending key")
    try:
        resp = send_key(f, "a")
        lg(f"injector: send-key {json.dumps(resp)[:100]}")
    except OSError as e:
        lg(f"injector: send-key failed: {e}")
        return

    # Belt-and-suspenders: confirm the guest logged the success marker; one
    # re-send if it hasn't within 2 s (covers a QEMU-side swallow or a lost
    # first delivery -- extra events are harmless, the probe drains and
    # matches on the target key).
    for attempt in (1, 2):
        if tail.wait_for(success, min(deadline, time.time() + 2.0), 0.05):
            lg("injector: success marker observed; done")
            return
        if attempt == 1:
            lg("injector: success marker not seen in 2 s; re-sending once")
            try:
                resp = send_key(f, "a")
                lg(f"injector: re-send {json.dumps(resp)[:100]}")
            except OSError as e:
                lg(f"injector: re-send failed: {e}")
                return
    lg("injector: success marker never observed (delivery miss); exiting")


if __name__ == "__main__":
    main()
