#!/usr/bin/env python3
# serial-bridge.py -- the LS-CI serial relay (the #66 fix; the #41 nc-replacement;
# the #78 spool rework).
#
# Shuttles bytes between a QEMU serial UNIX socket (mon:unix:<sock>,server) and
# this process's stdin/stdout, which `expect` adopts via `spawn -open`:
#
#     expect  <--stdout--  bridge  <--socket-->  qemu serial
#     expect  --stdin-->   bridge  --socket-->   qemu serial
#
# WHY a purpose-built relay, not `nc -U`: BSD `nc` on macOS dies with SIGPIPE
# (exit 141) when its stdout write races the expect reader under a large boot
# burst -- the guest keeps booting (every probe passing) but the relay EOFs
# mid-stream, so the login wait saw `eof` and mis-reported "qemu exited before
# login" (the guest was exonerated every time; #72). SIGPIPE is Python-default
# ignored, so a transient stdout condition raises a CATCHABLE BrokenPipeError.
#
# WHY it SPOOLS instead of blocking on stdout (the #78 rework -- a reversal of
# the original "back-pressure is good" rationale, which was WRONG):
#   The old relay wrote to stdout BLOCKING, on the theory that a full pipe would
#   back-pressure the socket read and "drop nothing". But under a slow expect
#   reader that back-pressure does not prevent drops -- it CAUSES them, silently,
#   at the guest: a blocked stdout write stops the relay from draining QEMU's
#   serial socket, QEMU's send buffer fills, the guest UART TX ring fills, and
#   the guest DROPS the remainder of its console write on the kernel #75 TX
#   deadline (kernel/cons.c:518-542). MEASURED: with a paused reader the blocking
#   relay stalls after ~80 KiB; the spool relay accepts the whole burst
#   (tools/interactive/test-serial-bridge.py, the #78 differential).
#
#   So this relay NEVER blocks on the reader. It drains the guest serial socket
#   AGGRESSIVELY into an in-process spool every wake (keeping QEMU's buffer empty
#   -> the guest is never back-pressured -> the #75 drop never fires) and writes
#   the spool to stdout NON-BLOCKING, letting a slow expect reader catch up at
#   its own pace. No byte is dropped: the spool holds everything until expect
#   reads it.
#
# Preserved properties:
#   - #66 lost-wakeup immunity: a bounded select() park re-checks levels every
#     wake, and the socket is drained on EVERY wake (not only when `readable`),
#     so a lost macOS readable-edge cannot strand queued bytes.
#   - anti-SIGPIPE: a closed reader raises BrokenPipeError -> `stdout-broken`
#     (the clean "expect done" signal), never a fatal SIGPIPE.
#   - stdin EOF (expect teardown) does NOT end the relay -- serial->stdout keeps
#     flowing until the socket closes.
#
# Exit 0 on either endpoint closing normally (socket EOF = guest serial gone;
# stdout closed = expect done). Diagnostics + the exit code go to stderr (the
# lib.exp #41 instrument reads it as `bridge exit=<rc> reason=<why>`).

import errno
import os
import select
import socket
import sys

PARK_S = 0.2      # bounded park: re-check levels ~5x/s (the #66 lost-wakeup floor)
CHUNK = 65536
DRAIN_MAX = 256   # bound the per-wake socket drain (256 * 64 KiB = 16 MiB/wake)
SPOOL_CAP = 64 * 1024 * 1024  # a genuinely-wedged-reader backstop; a real guest
                              # burst is bounded (hundreds of KiB) and never hits it
FLUSH_TRIES = 50  # bounded tail-flush on socket EOF (never hangs on a gone reader)


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
    os.set_blocking(stdout_fd, False)  # #78: NON-blocking + spool -- never stall
                                       # the relay (hence the guest) on the reader.

    spool = bytearray()  # serial->stdout bytes awaiting a slow expect reader
    watch_in = [sock.fileno(), stdin_fd]
    sock_open = True

    while True:
        watch_out = [stdout_fd] if spool else []
        try:
            readable, _, _ = select.select(
                watch_in, watch_out, watch_in + watch_out, PARK_S)
        except InterruptedError:
            continue
        # PARK_S timeout with empty `readable` == a level re-check: fall through.

        # 1. Drain the guest serial AGGRESSIVELY every wake (level-triggered for
        #    the #66 macOS edge-loss): empty the socket buffer so the guest is
        #    never back-pressured (the #78 fix). A non-blocking recv() returns
        #    b"" ONLY on a real EOF; a would-block raises BlockingIOError.
        if sock_open:
            for _ in range(DRAIN_MAX):
                try:
                    data = sock.recv(CHUNK)
                except BlockingIOError:
                    break
                except OSError as e:
                    if e.errno in (errno.ECONNRESET, errno.EPIPE):
                        return _flush_tail(
                            spool, stdout_fd, f"socket-reset(errno={e.errno})")
                    raise
                if data == b"":
                    sock_open = False  # QEMU closed the serial (guest gone)
                    break
                spool += data
                if len(spool) > SPOOL_CAP:
                    return _done("spool-overflow")  # a genuinely-wedged reader
                if len(data) < CHUNK:
                    break  # socket momentarily drained

        # 2. Push the spool to expect NON-BLOCKING (a short write leaves the rest;
        #    the write-set + PARK_S re-wake us when the pipe drains). This never
        #    blocks the loop, so step 1 keeps draining regardless of reader pace.
        if spool:
            try:
                n = os.write(stdout_fd, spool)
                if n:
                    del spool[:n]
            except BlockingIOError:
                pass  # pipe full: keep the spool; wait for writable
            except BrokenPipeError:
                return _done("stdout-broken")  # expect closed its read end

        # 3. Socket EOF: flush the tail so the session's last bytes are not lost.
        if not sock_open:
            return _flush_tail(spool, stdout_fd, "socket-eof")

        # 4. stdin -> guest serial.
        if stdin_fd in readable:
            try:
                keys = os.read(stdin_fd, CHUNK)
            except BlockingIOError:
                keys = None
            if keys == b"":
                # expect closed stdin (spawn teardown): stop watching it, keep
                # relaying serial->stdout until the socket ends. NOT an exit.
                if stdin_fd in watch_in:
                    watch_in.remove(stdin_fd)
            elif keys:
                try:
                    _send_all(sock, keys)
                except OSError as e:
                    if e.errno in (errno.ECONNRESET, errno.EPIPE):
                        return _done(f"send-reset(errno={e.errno})")
                    raise


def _flush_tail(spool: bytearray, stdout_fd: int, reason: str) -> int:
    # Best-effort BOUNDED flush of the remaining spool on socket close, so the
    # session tail (the last prompt / probe line) is delivered. A gone reader
    # ends it at once (BrokenPipeError); the iteration cap means it never hangs.
    for _ in range(FLUSH_TRIES):
        if not spool:
            break
        try:
            n = os.write(stdout_fd, spool)
            if n:
                del spool[:n]
                continue
        except BlockingIOError:
            pass
        except BrokenPipeError:
            break  # reader gone -- nothing more we can deliver
        select.select([], [stdout_fd], [], PARK_S)  # wait for the pipe to drain
    return _done(reason)


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
