#!/usr/bin/env python3
# serial-bridge.py -- the LS-CI serial relay (the #66 fix, finally built; the
# #41 nc-replacement).
#
# Shuttles bytes between a QEMU serial UNIX socket (mon:unix:<sock>,server) and
# this process's stdin/stdout, which `expect` adopts via `spawn -open`:
#
#     expect  <--stdout--  bridge  <--socket-->  qemu serial
#     expect  --stdin-->   bridge  --socket-->   qemu serial
#
# WHY a purpose-built relay, not `nc -U` (the bug this closes): BSD `nc` on
# macOS dies with SIGPIPE (exit 141) when its stdout write races the expect
# reader's buffering under a large boot-output burst -- the guest keeps
# booting (RN+, every probe passing) but the bridge EOFs mid-stream, so the
# login wait sees `eof` and mis-reports "qemu exited before login" (the G-6a/
# G-6c/G-6d clusters; guest exonerated every time). This relay is immune by
# construction:
#   - SIGPIPE is Python-default-ignored, so a transient stdout condition
#     raises a CATCHABLE BrokenPipeError instead of killing the process; a
#     genuinely-closed reader ends the relay cleanly (the expected EOF), a
#     transient one does not.
#   - a BOUNDED select() park re-checks both endpoints level-triggered on
#     every wake, so a lost readable-edge (the #66 macOS poll wakeup loss on a
#     parked reader) cannot strand it -- the periodic re-entry drains whatever
#     is queued.
#   - blocking stdout writes give natural back-pressure under a burst (the
#     socket buffer holds; nothing is dropped; no SIGPIPE) rather than nc's
#     race-to-death.
#
# Exit 0 on either endpoint closing normally (socket EOF = guest serial gone;
# stdout EOF = expect done). Diagnostics + the exit code go to stderr (the
# lib.exp #41 instrument reads it as `bridge exit=<rc>`).

import errno
import os
import select
import socket
import sys

PARK_S = 0.2  # bounded park: re-check levels ~5x/s (the #66 lost-wakeup floor)
CHUNK = 65536


REASON = "unset"  # the exit-path witness (logged to stderr for the #41 instrument)


def _done(reason: str) -> int:
    global REASON
    REASON = reason
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: serial-bridge.py <unix-socket>\n")
        return 2
    path = sys.argv[1]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    sock.setblocking(False)

    stdin_fd = sys.stdin.fileno()
    stdout_fd = sys.stdout.fileno()
    os.set_blocking(stdin_fd, False)
    # stdout stays BLOCKING on purpose: a full pipe back-pressures the socket
    # read (below) instead of dropping or racing -- the anti-SIGPIPE property.

    watch = [sock.fileno(), stdin_fd]
    while True:
        try:
            readable, _, _ = select.select(watch, [], watch, PARK_S)
        except InterruptedError:
            continue
        # PARK_S timeout with empty `readable` == a level re-check: loop.

        # Always attempt a socket drain on wake (level-triggered): the macOS
        # edge-loss the #66 hunt found means "not in `readable`" is not proof
        # that nothing is queued. A non-blocking recv() returns b"" ONLY on a
        # real EOF (a would-block raises BlockingIOError), so an empty read is
        # an unambiguous guest-serial-closed signal.
        try:
            data = sock.recv(CHUNK)
        except BlockingIOError:
            data = None
        except OSError as e:
            if e.errno in (errno.ECONNRESET, errno.EPIPE):
                return _done(f"socket-reset(errno={e.errno})")  # guest serial gone
            raise
        if data == b"":
            return _done("socket-eof")  # QEMU closed the serial (guest gone)
        if data:
            try:
                _write_all(stdout_fd, data)
            except BrokenPipeError:
                return _done("stdout-broken")  # expect closed its read end

        if stdin_fd in readable:
            try:
                keys = os.read(stdin_fd, CHUNK)
            except BlockingIOError:
                keys = None
            if keys == b"":
                # expect closed stdin (spawn teardown): stop watching it, keep
                # relaying serial->stdout until the socket ends. NOT an exit --
                # a would-block (keys is None) also falls through untouched.
                if stdin_fd in watch:
                    watch.remove(stdin_fd)
            elif keys:
                try:
                    _send_all(sock, keys)
                except OSError as e:
                    if e.errno in (errno.ECONNRESET, errno.EPIPE):
                        return _done(f"send-reset(errno={e.errno})")
                    raise


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        n = os.write(fd, view)  # blocking: back-pressure, not drop
        view = view[n:]


def _send_all(sock: socket.socket, data: bytes) -> None:
    view = memoryview(data)
    while view:
        try:
            n = sock.send(view)
            view = view[n:]
        except BlockingIOError:
            select.select([], [sock.fileno()], [], PARK_S)


if __name__ == "__main__":
    try:
        rc = main()
    except (KeyboardInterrupt, BrokenPipeError):
        rc = 0
        REASON = "signal-or-broken"
    except BaseException as e:  # noqa: BLE001 -- the instrument must see everything
        rc = 1
        REASON = f"exception:{type(e).__name__}:{e}"
    sys.stderr.write(f"bridge exit={rc} reason={REASON}\n")
    sys.exit(rc)
