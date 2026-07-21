#!/usr/bin/env python3
# test-serial-bridge.py -- the #78 ground-truth differential (host-only; no QEMU).
#
# Proves the property the LS-CI relay MUST have: it drains the guest serial
# socket PROMPTLY even when the expect reader is stalled, so it never
# back-pressures the guest. A back-pressured guest hits the kernel #75 console
# TX-deadline and DROPS output (kernel/cons.c:518-542) -- silently losing the
# very completion token expect waits for, which surfaces as `stdout-broken`
# (the #78 flake; the guest is exonerated every time).
#
# Setup: a fake QEMU (a burst-blasting AF_UNIX server) <-> the relay under test
# <-> a PAUSED stdout reader (a pipe we hold and never read). We measure how
# many bytes the fake QEMU can push before its socket would block.
#   - A SPOOLING relay drains the socket into its own buffer continuously, so
#     the fake QEMU pushes the WHOLE burst.
#   - A BLOCKING-WRITE relay stalls on the paused pipe and stops draining, so
#     the fake QEMU stalls after ~one pipe + socket buffer (a few * 64 KiB).
#
# Usage: test-serial-bridge.py [path-to-relay]   (defaults to the sibling relay)
# Exit 0 = PASS (spools; no back-pressure), 1 = FAIL (back-pressures).

import os
import select
import socket
import subprocess
import sys
import tempfile
import time

RELAY = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "serial-bridge.py")
BURST = 4 * 1024 * 1024   # 4 MiB -- >> any pipe/socket buffer
PAYLOAD = b"A" * 65536


def measure_pushed():
    d = tempfile.mkdtemp(prefix="sbtest-")
    sockpath = os.path.join(d, "q.sock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sockpath)
    srv.listen(1)

    # The relay's stdout goes into a pipe whose read end WE hold and NEVER read
    # -- a permanently-stalled expect reader.
    rd, wr = os.pipe()
    proc = subprocess.Popen(
        [sys.executable, RELAY, sockpath],
        stdin=subprocess.DEVNULL, stdout=wr, stderr=subprocess.PIPE,
    )
    os.close(wr)  # the relay owns the write end now

    conn, _ = srv.accept()
    conn.setblocking(False)

    # Pace the sender to the relay's ACTUAL drain rate: wait for the socket to
    # become writable (no fixed sleep, which would rate-limit the sender itself).
    # A spooling relay keeps the socket writable (it drains continuously); a
    # blocking-write relay leaves it permanently full, so writability never
    # returns and we stall at the initial fill.
    pushed = 0
    deadline = time.monotonic() + 8.0
    while pushed < BURST and time.monotonic() < deadline:
        _, writable, _ = select.select([], [conn], [], 0.5)
        if not writable:
            continue  # relay isn't draining -- socket stays full (blocking relay)
        try:
            n = conn.send(PAYLOAD[: min(len(PAYLOAD), BURST - pushed)])
            pushed += n
        except BlockingIOError:
            pass

    conn.close()
    srv.close()
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
    os.close(rd)
    return pushed


def main():
    pushed = measure_pushed()
    # A spooling relay accepts essentially the whole burst; a blocking one stalls
    # at a few buffers. Half the burst is a wide, unambiguous threshold.
    ok = pushed >= BURST // 2
    print(f"relay={os.path.basename(RELAY)} pushed={pushed} of {BURST} "
          f"({'spooled' if ok else 'BACK-PRESSURED'})")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
