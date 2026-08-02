#!/usr/bin/env python3
"""Create a pre-widened listening AF_UNIX socket, then exec the VM on it.

WHY THIS EXISTS (#125)
======================
LS-CI's serial console rides a UNIX socket between QEMU and serial-bridge.py.
When the host-side relay stops draining that socket, QEMU's serial write
blocks and QEMU stops executing the guest entirely -- the VM is SUSPENDED, and
from inside it is indistinguishable from a guest hang.

MEASURED (tools/stall-amplify.sh): SIGSTOP the relay and qemu's host CPU falls
100% -> 2.4% within ~2 s, held for the whole freeze, then 167% catching up.

The budget for that is tiny. On macOS an AF_UNIX SOCK_STREAM holds 8192 bytes
by default, against ~117-198 KiB of console output per boot -- so roughly 4%
of one boot's output is all the slack there is.

THE OBVIOUS FIX IS VACUOUS -- and that is the point of this file
----------------------------------------------------------------
The natural move is `setsockopt(SO_RCVBUF)` in the relay. It does NOTHING.
Measured on macOS 26, by writing until the writer blocks:

    default ............................. 8192 B
    READER sets SO_RCVBUF = 8 MiB ....... 8192 B   <-- no effect at all
    WRITER sets SO_SNDBUF = 8 MiB ....... 8 MiB
    ask for 64 MiB ...................... 8 MiB    (macOS clamps here)

Capacity is governed by the WRITER's SO_SNDBUF. The relay is the READER, and
the writer socket belongs to QEMU -- so nothing the relay does to its own end
can matter. Always establish which end owns the buffer before "widening" it.

WHAT ACTUALLY WORKS
-------------------
An accepted connection INHERITS SO_SNDBUF from the LISTENING socket. So if we
create the listener, set SO_SNDBUF on it, and hand the listening fd to QEMU,
then the socket QEMU writes to is one we sized. QEMU takes a pre-made listener
via `-chardev socket,fd=N,server=on`; `exec` preserves the fd, so a plain
wrapper is enough and nothing needs to be passed out of band.

A/B THROUGH A REAL BOOT (the non-vacuous proof)
-----------------------------------------------
Connect to the console socket, then DO NOT READ for 60 s, then drain:

    qemu owns the listener   44221 bytes   boot_ok=False  login=False
    we own the listener     128183 bytes   boot_ok=True   login=True

The old arm drained byte-identically 44221 at 12 s and at 60 s -- zero progress
across 48 seconds. That is a hard stop, not slowness. (44 KiB rather than 8 KiB
because QEMU buffers internally as well, ahead of the socket.) The new arm
completed the boot to a login prompt with nobody reading at all.

WHY A WRAPPER AND NOT AN EDIT TO run-vm.sh
------------------------------------------
run-vm.sh is the canonical launcher for everything -- test.sh, the SMP gate,
manual boots -- and only LS-CI's socket transport has this problem. Wrapping
keeps the blast radius at exactly the caller that needs it: run-vm.sh stays
byte-identical, and its `exec qemu` carries our fd through untouched.

FAIL-SOFT BY DESIGN
-------------------
If SO_SNDBUF cannot be set, we say so and carry on at the default capacity.
This is a harness-robustness measure, not a correctness gate: degrading to the
old behaviour is strictly better than refusing to boot the VM.

USAGE
    serial-listen.py --sock PATH [--sndbuf N] [--fd N] -- CMD [ARGS...]
`{SERIALFD}` in any argument is replaced by the listening fd number.
"""
import argparse
import os
import socket
import sys

# macOS clamps SO_SNDBUF at 8 MiB; asking for more just gets 8 MiB. Linux
# instead caps at net.core.wmem_max (commonly ~208 KiB) -- still ~26x the
# default, and LS-CI uses the pty transport off Darwin anyway.
DEFAULT_SNDBUF = 8 << 20

# A fixed, high target fd keeps {SERIALFD} deterministic and keeps us clear of
# the low fds bash reuses for command substitution inside run-vm.sh.
DEFAULT_FD = 30


def note(msg: str) -> None:
    """Report on stderr, which the caller captures into the VM log."""
    sys.stderr.write(f"serial-listen: {msg}\n")
    sys.stderr.flush()


def main() -> None:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--sock", required=True, help="AF_UNIX path to bind")
    ap.add_argument("--sndbuf", type=int, default=DEFAULT_SNDBUF)
    ap.add_argument("--fd", type=int, default=DEFAULT_FD)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        sys.exit("serial-listen.py: no command given after --")

    try:
        os.unlink(args.sock)
    except FileNotFoundError:
        pass

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

    # Set BEFORE listen(): on BSD the accepted socket inherits the listener's
    # buffer sizes at accept() time, so a later setsockopt would not reach the
    # connection QEMU actually writes through.
    got = -1
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, args.sndbuf)
        got = s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    except OSError as e:
        note(f"WARNING: SO_SNDBUF={args.sndbuf} rejected ({e}); "
             f"continuing at the default capacity -- a host-side console "
             f"stall can still suspend the guest (#125)")

    s.bind(args.sock)
    s.listen(1)

    # dup2 onto a known fd. Guard first: silently closing whatever lives there
    # would be a hard bug to trace back to here.
    if args.fd != s.fileno():
        try:
            os.fstat(args.fd)
        except OSError:
            pass  # not open -- the expected case
        else:
            sys.exit(f"serial-listen.py: fd {args.fd} is already open; "
                     f"pass a different --fd")
        os.dup2(s.fileno(), args.fd, inheritable=True)
    else:
        os.set_inheritable(args.fd, True)

    note(f"sock={args.sock} fd={args.fd} sndbuf_asked={args.sndbuf} "
         f"sndbuf_got={got}")

    cmd = [a.replace("{SERIALFD}", str(args.fd)) for a in cmd]
    os.execvp(cmd[0], cmd)


main()
