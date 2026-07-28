#!/usr/bin/env python3
# test-serial-bridge.py -- the #78 relay property tests (host-only; no QEMU).
#
# TWO properties, both host-only and fast, so this runs as a test-interactive.sh
# preflight: the relay is the carrier every LS-CI scenario depends on, and a
# harness that fails OPEN is the #74 lesson.
#
# ---------------------------------------------------------------------------
# Property 1 (the original #78 differential): no guest back-pressure.
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
# ---------------------------------------------------------------------------
# Property 2: the exit record DISCRIMINATES.
#
# `stdout-broken` is the relay's NORMAL end-of-session reason (a passing scenario
# ends with expect exiting, which closes the read end). So on a FAILING attempt
# the bare string carries almost no information -- yet it was read as a diagnosis
# across three sessions of #78. The relay must therefore split the two cases by
# whether expect had closed STDIN first (its teardown signal):
#     expect tearing down -> `stdout-broken`       (benign, expected)
#     expect still ALIVE   -> `stdout-broken-live`  (the anomaly worth chasing)
# This test drives both cases and fails if they do not separate -- i.e. it pins
# the discriminator as non-vacuous, so it cannot silently collapse back into one
# uninformative string.
#
# Usage: test-serial-bridge.py [path-to-relay]   (defaults to the sibling relay)
# Exit 0 = PASS (both properties), 1 = FAIL.

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


def run_exit_case(stdin_open: bool) -> str:
    # Drive the relay to a BrokenPipeError on stdout with expect either still
    # alive (stdin held open) or tearing down (stdin closed first), and return
    # its stderr exit record.
    d = tempfile.mkdtemp(prefix="sbtest-exit-")
    sockpath = os.path.join(d, "q.sock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sockpath)
    srv.listen(1)

    out_rd, out_wr = os.pipe()   # relay stdout; we hold the read end
    in_rd, in_wr = os.pipe()     # our write end feeds the relay's stdin
    proc = subprocess.Popen(
        [sys.executable, RELAY, sockpath],
        stdin=in_rd, stdout=out_wr, stderr=subprocess.PIPE,
    )
    os.close(out_wr)
    os.close(in_rd)

    conn, _ = srv.accept()
    conn.send(b"hello-from-guest\n")   # one successful stdout write first
    time.sleep(0.4)
    os.read(out_rd, 4096)

    if not stdin_open:
        os.close(in_wr)               # expect closes stdin (teardown signal)
        time.sleep(0.4)               # let the relay observe the EOF

    os.close(out_rd)                  # the reader vanishes
    time.sleep(0.3)
    conn.send(b"more-after-reader-gone\n")   # force the next stdout write

    try:
        _, err = proc.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
    if stdin_open:
        os.close(in_wr)
    conn.close()
    srv.close()
    return err.decode().strip()


def main():
    pushed = measure_pushed()
    # A spooling relay accepts essentially the whole burst; a blocking one stalls
    # at a few buffers. Half the burst is a wide, unambiguous threshold.
    spool_ok = pushed >= BURST // 2
    print(f"[1] relay={os.path.basename(RELAY)} pushed={pushed} of {BURST} "
          f"({'spooled' if spool_ok else 'BACK-PRESSURED'})")

    live = run_exit_case(stdin_open=True)
    tear = run_exit_case(stdin_open=False)
    # The trailing space pins `stdout-broken` exactly -- without it the benign
    # case would also match the `-live` string and the check would be vacuous.
    exit_ok = ("stdout-broken-live" in live) and ("reason=stdout-broken " in tear)
    print(f"[2] expect-alive    -> {live}")
    print(f"[2] expect-teardown -> {tear}")
    print(f"[2] exit record {'DISCRIMINATES' if exit_ok else 'IS VACUOUS'}")

    ok = spool_ok and exit_ok
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
