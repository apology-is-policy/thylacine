---
id: haz-unread-pipe-wedge
type: haz
title: "A reader that stops reading wedges a writer that must not block"
applies-to: [sub-stratum-boot, sub-substrate-interactive]
instances: []
created: 2026-08-02
updated: 2026-08-02
---
## The failure shape

Process A writes diagnostics down a pipe or socket. Process B reads them —
until it decides it has read enough, or until something downstream of B
stops accepting bytes. The buffer fills. A's next write **blocks**.

If A is load-bearing, that block is not a lost log line. It is a wedge of
whatever A was in the middle of, and it propagates to everything waiting on
A. The diagnostic channel — the thing added to make failures visible —
becomes the failure.

Two properties make this class nastier than it first reads:

- **The victim is not the chatty one.** A is usually writing something
  routine. The wedge lands wherever A's caller was, which can be an
  unrelated subsystem several layers away.
- **Instrumentation triggers it.** Adding prints to diagnose an unrelated
  bug is exactly what pushes a working system over the buffer threshold, so
  the wedge appears *while you are investigating something else* and looks
  like a consequence of your change.

## The tell

A hang whose stack sits in `write`, on a fd nobody is reading — but the
process that *should* be reading is itself alive and doing something else,
so nothing looks dead. Under a filesystem daemon the symptom is the whole
system stalling on I/O with no error anywhere.

Reversed polarity, same class: reasoning that a **blocking** write is the
safe choice because "a full pipe back-pressures the source and nothing is
dropped." That is true only if back-pressure is harmless at the source. When
the source is a guest console or an FS thread, back-pressure *is* the
damage.

## Worked instances

**stratumd's stdout, #370.** joey reads the daemon's pipe only until the
readiness token, then goes on with the boot. Post-startup, stratumd keeps
writing into a pipe with no reader; a full buffer blocks a stratumd thread
mid-write, and under the FS that wedges the whole system. Proven live — an
instrumented stratumd deadlocked a boot. Fix: hand the read end to a
dedicated drainer thread for the daemon's lifetime. If the drainer cannot
be spawned, **close** the read end so later writes fail fast with EPIPE:
silenced diagnostics are an acceptable degraded mode; an FS wedge is not.

**The LS-CI serial relay, #78.** The relay wrote QEMU's serial stream to
stdout *blocking*, on the stated theory that a full pipe back-pressures the
socket read and drops nothing. Exactly inverted: a blocked stdout write
stops the relay draining QEMU's serial socket → QEMU's send buffer fills →
the guest UART TX ring fills → the guest **drops** the rest of its console
write on the kernel's TX deadline, losing whatever token the test was
waiting for. Fix: drain aggressively into an in-process spool and write out
non-blocking. Proven by a host-only differential with no QEMU at all —
against a paused reader the blocking relay stalls at ~80 KB, the spool
relay accepts a full 4 MB burst.

Two layers, two teams, one shape: a diagnostic path became a control path.

## The countermeasure

1. **Name the owner of every read end, for the whole lifetime of the
   writer.** "joey reads it during startup" is not an owner; it is an owner
   for a while, which is the bug.
2. **If no owner exists, close it.** EPIPE is a clean, immediate, local
   failure. A full buffer is a distributed hang.
3. **Never write blocking toward a source that cannot afford back-pressure.**
   Spool in-process and write non-blocking; dropping a log line beats
   stalling the emitter.
4. **Prove it without the expensive substrate.** Both instances are
   provable with a paused reader and no VM. The LS-CI differential runs as a
   preflight before anything boots, precisely so the property cannot rot —
   the exit-record check exists because `stdout-broken` was read as a
   *diagnosis* for three sessions.
