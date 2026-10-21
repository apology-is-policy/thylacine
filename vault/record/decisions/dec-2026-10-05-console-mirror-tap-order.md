---
id: dec-2026-10-05-console-mirror-tap-order
type: dec
title: "A short console write reaches the renderer as it reached the serial line: one tap, after the pushes"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-kernel-cons]
created: 2026-10-05
---
## Fork

The console mirrors a process write to two sinks when the serial line is loud
and a renderer holds the drain: the default (test) posture of DISPLAY-MODES
section 4. GPU mode silences serial (1b), so the drain is then the only sink and
takes the whole chunk; console mode has no drain. The UNIT-ATOMICITY round of
2026-08-17 recorded the order as LS-8 item (f) (ARCH 25.4 and AUDIT-TRIGGERS):
the tap fires first, and a deadline short write leaving the renderer up to one
chunk ahead of serial is "a documented mirror divergence, not a defect".

The signal(7)-list chunk ([[chg-2026-10-05-signal7-list]]) makes the console's
room wait end for a caught note, so a write waiting for room in a full TX ring
returns short whenever a signal lands, which happens often at 115200 baud. A
Linux caller sends the unsent tail again (busybox `safe_write`, stdio, git's
`xwrite`), and with the tap first the renderer shows that tail twice. The
implementer's audit pass reclassified the divergence as a P2 defect (SA-1) and
moved the tap after the push. The second Fable round rated the reversal a P3
and asked for the operator to weigh it, because it overturns a closed audit
disposition.

## Research

- **Plan 9.** `devcons`'s `putstrn0` calls `screenputs` first and then queues
  the bytes on `serialoq` with a blocking `qwrite`. An interrupted `qwrite`
  raises an error, so the program sees a failed write, not a count to resend
  from. The heritage order is screen first, with no short count for a retry to
  duplicate.
- **Linux.** `/dev/console` attaches to one tty. A process write reaches one
  sink, and only `printk` fans out to every registered console, one record at a
  time and never short. Linux never mirrors a process write across two sinks.
- **The tree.** The silenced branch of `cons_emit_bulk_wait` taps the whole
  chunk and returns `n`, so a serial-less posture (item (f)'s stated reason for
  tapping first) is served by 1b and not by the tap order. Echo and the
  diagnostic lines are never sent again and drop whole on a full ring, so tap
  first stays right for them.

## Options

1. **Tap once, after the pushes, with what went out.** The renderer receives
   what the count reports, in one drain hold. A retry never duplicates, and a
   peer's unit can never land inside the chunk on the renderer. Cost: when the
   serial consumer stalls past the 20 ms deadline, the renderer stops with it
   instead of running ahead.
2. **Keep tap first** (item (f), Plan 9's order). A retrying caller shows the
   tail twice whenever a signal cuts a write on a full ring, and more rarely on
   the #67 deadline.
3. **Tap first, and let a caught note end the write only at a chunk boundary.**
   This removes the duplicates a note makes, keeps the #67 duplicate, and adds a
   fourth wait to ARCH 8.8.3's exclusions.

## The call

Option 1, the operator's vote of 2026-10-05. The process write taps the drain
once, after its pushes, with what went out. Echo and diagnostic lines tap first,
as before. Item (f), ARCH 23.5.2's lock-order parenthetical and DISPLAY-MODES
section 3.3 now say so.

## Rationale

A write's count is the only thing a caller acts on, so every sink must agree
with it. The stall-tolerance item (f) bought has no viewer: in the posture that
mirrors, the serial consumer is the harness, and when it stalls a renderer that
runs ahead shows duplicates as soon as the caller retries. A tap per push would
also have matched the count, but a peer's unit landing between two pushes would
then tear the chunk in the drain. The renderer interprets escape sequences, and
tap first had protected it from that tear; one tap after the pushes keeps that
protection.
