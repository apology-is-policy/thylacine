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
# WHY IT ALSO REPORTS ITS OWN STALLS (#125 -- the measurement that forced it):
#   The #78 rework made this relay never BLOCK on the reader. It did not, and
#   could not, make the relay always RUN. A relay that is descheduled, paused,
#   or otherwise off-CPU is not draining, which is indistinguishable at the
#   socket from a relay that blocked -- and the consequence is far worse than
#   the #78 drop it replaced:
#
#     MEASURED (tools/stall-amplify.sh): SIGSTOP this relay and QEMU's host CPU
#     falls 100% -> 2.4% within ~2 s and stays there. QEMU's serial write blocks
#     and QEMU STOPS EXECUTING THE GUEST ENTIRELY. The guest is SUSPENDED, and
#     from inside the VM that is indistinguishable from a guest hang.
#
#   The budget for this is tiny and is NOT ours to widen. On macOS a UNIX-stream
#   socket's capacity is governed by the WRITER's SO_SNDBUF -- QEMU's socket,
#   which QEMU creates and we cannot touch (setting SO_RCVBUF on this end
#   measurably does NOTHING: 8192 B either way). So:
#
#     capacity QEMU can push while we are not draining ... 8192 bytes
#     output of one LS-CI boot ....................... ~117-198 KiB
#
#   i.e. ~4% of a single boot. Any stall of this process long enough for the
#   guest to emit 8 KiB freezes the whole VM.
#
#   We therefore cannot prevent it here -- but we can make it DECIDABLE. The
#   loop times its own wake-to-wake gap and reports every stall immediately to
#   stderr (unbuffered, so the evidence survives being killed on a scenario
#   timeout) plus a summary in the exit record. When a boot goes silent, that
#   record answers the question that #125 could not: was the relay stalled
#   (-> the VM was frozen from the host side) or running (-> look elsewhere)?
#   Absent it, the only available answer is a guess, and the convenient guess
#   is "host load" -- which CLAUDE.md forbids precisely because it ends hunts
#   that have not started.
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
#
# WHY the exit record carries counters and splits `stdout-broken` two ways:
#   `stdout-broken` is ALSO the normal end-of-session reason -- when a scenario
#   passes, expect exits, its read end closes, and the relay's next write EPIPEs.
#   So the bare string is ambiguous by construction and says almost nothing about
#   a FAILING attempt; it was nonetheless read as a diagnosis across three
#   sessions of #78. The discriminator the relay already knows but used to throw
#   away is whether expect had closed STDIN first:
#     - stdin EOF seen  -> expect is tearing down    -> `stdout-broken` (benign)
#     - stdin still open -> expect is ALIVE and reading, yet its read end is gone
#                           -> `stdout-broken-live` (the #78 anomaly)
#   The counters (bytes moved, spool depth, idle gaps) size the burst the cut
#   landed in, so the buffer-pressure correlation can be checked from the record
#   instead of re-derived by hand.

import errno
import os
import select
import socket
import sys
import time

PARK_S = 0.2      # bounded park: re-check levels ~5x/s (the #66 lost-wakeup floor)
CHUNK = 65536
DRAIN_MAX = 256   # bound the per-wake socket drain (256 * 64 KiB = 16 MiB/wake)
SPOOL_CAP = 64 * 1024 * 1024  # a genuinely-wedged-reader backstop; a real guest
                              # burst is bounded (hundreds of KiB) and never hits it
FLUSH_TRIES = 50  # bounded tail-flush on socket EOF (never hangs on a gone reader)
STALL_S = 1.0     # #125: a wake-to-wake gap this long means we were NOT RUNNING.
                  # 5x PARK_S, so ordinary scheduling jitter can never reach it;
                  # a gap this size has already let the guest overrun QEMU's
                  # 8 KiB socket budget and freeze the VM.
STALL_REPORTS = 20  # cap the per-stall stderr lines; past this only new maxima
                    # are logged, so a pathological host cannot flood the record


REASON = "unset"  # the exit-path witness (logged to stderr for the #41 instrument)

# Exit-record counters (stderr, alongside REASON). Module-level so every exit
# path reports them without threading state through each return.
STATS = {
    "stdin_eof": 0,   # 1 once expect closed stdin (its teardown signal)
    "in": 0,          # bytes drained from the guest serial socket
    "out": 0,         # bytes delivered to expect
    "spool": 0,       # bytes still undelivered at exit
    "t0": 0.0,        # start (monotonic)
    "t_in": 0.0,      # last socket data
    "t_out": 0.0,     # last successful stdout write
    "stalls": 0,      # #125: wake-to-wake gaps > STALL_S (we were off-CPU)
    "max_stall": 0.0, # longest such gap, seconds
    "stall_logged": 0,
}


def _done(reason: str) -> int:
    global REASON
    REASON = reason
    return 0


def _stats_line() -> str:
    now = time.monotonic()
    up = now - STATS["t0"] if STATS["t0"] else 0.0
    # "never" distinguishes "no data ever" from "data, then a gap" -- a cut with
    # idle_in=never means the relay was starved, not that the reader vanished.
    idle_in = f"{now - STATS['t_in']:.2f}" if STATS["t_in"] else "never"
    idle_out = f"{now - STATS['t_out']:.2f}" if STATS["t_out"] else "never"
    return (f"stdin_eof={STATS['stdin_eof']} in={STATS['in']} out={STATS['out']} "
            f"spool={STATS['spool']} idle_in={idle_in} idle_out={idle_out} "
            f"up={up:.2f} stalls={STATS['stalls']} "
            f"max_stall={STATS['max_stall']:.2f}")


def _note_stall(gap: float) -> None:
    # #125: we were off-CPU for `gap` seconds, so nothing drained QEMU's serial
    # socket. Past ~8 KiB of guest output that blocks QEMU's write and SUSPENDS
    # the guest -- so this is the host-side cause of a "silent guest", and the
    # record has to survive us being killed on a scenario timeout. Hence an
    # IMMEDIATE flushed stderr write, not just the exit line.
    STATS["stalls"] += 1
    if gap > STATS["max_stall"]:
        STATS["max_stall"] = gap
        new_max = True
    else:
        new_max = False
    if STATS["stall_logged"] < STALL_REPORTS or new_max:
        STATS["stall_logged"] += 1
        sys.stderr.write(
            f"bridge STALL gap={gap:.2f}s (relay off-CPU; QEMU serial undrained "
            f"-> guest may be frozen) at up={time.monotonic() - STATS['t0']:.2f} "
            f"in={STATS['in']} spool={STATS['spool']}\n")
        sys.stderr.flush()


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
    STATS["t0"] = time.monotonic()

    t_prev_wake = STATS["t0"]

    while True:
        STATS["spool"] = len(spool)
        watch_out = [stdout_fd] if spool else []
        try:
            readable, _, _ = select.select(
                watch_in, watch_out, watch_in + watch_out, PARK_S)
        except InterruptedError:
            continue
        # PARK_S timeout with empty `readable` == a level re-check: fall through.

        # #125: time our OWN loop. select() parks at most PARK_S, so a longer
        # gap means this process was not scheduled -- and while we are off-CPU
        # nothing drains QEMU's serial socket. Measured here rather than around
        # select() alone so a stall ANYWHERE in the loop body counts too: the
        # hazard is "not draining", whatever the reason.
        t_wake = time.monotonic()
        if t_wake - t_prev_wake > STALL_S:
            _note_stall(t_wake - t_prev_wake)
        t_prev_wake = t_wake

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
                STATS["in"] += len(data)
                STATS["t_in"] = time.monotonic()
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
                    STATS["out"] += n
                    STATS["t_out"] = time.monotonic()
            except BlockingIOError:
                pass  # pipe full: keep the spool; wait for writable
            except BrokenPipeError:
                # expect closed its read end. Split the benign teardown from the
                # #78 anomaly: a teardown closes stdin too, but this loop may not
                # have polled it yet, so re-check once before judging.
                STATS["spool"] = len(spool)
                if not STATS["stdin_eof"] and _stdin_at_eof(stdin_fd):
                    STATS["stdin_eof"] = 1
                return _done("stdout-broken" if STATS["stdin_eof"]
                             else "stdout-broken-live")

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
                STATS["stdin_eof"] = 1
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
                STATS["out"] += n
                STATS["t_out"] = time.monotonic()
                continue
        except BlockingIOError:
            pass
        except BrokenPipeError:
            break  # reader gone -- nothing more we can deliver
        select.select([], [stdout_fd], [], PARK_S)  # wait for the pipe to drain
    STATS["spool"] = len(spool)  # >0 here means the tail was NOT delivered
    return _done(reason)


def _stdin_at_eof(stdin_fd: int) -> bool:
    # One-shot non-blocking probe: readable AND read()==b"" means expect closed
    # stdin. Only ever called on the exit path, so consuming a byte here cannot
    # affect the session. An unusable stdin counts as gone.
    try:
        r, _, _ = select.select([stdin_fd], [], [], 0)
        if not r:
            return False
        return os.read(stdin_fd, 1) == b""
    except OSError:
        return True


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
    sys.stderr.write(f"bridge exit={rc} reason={REASON} {_stats_line()}\n")
    sys.exit(rc)
