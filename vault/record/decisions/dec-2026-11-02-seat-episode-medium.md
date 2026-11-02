---
id: dec-2026-11-02-seat-episode-medium
type: dec
title: "Corvus reads its episode's medium from the kernel, never from the seat phase"
date: 2026-11-02
status: standing
decided-by: user-vote
affects: [sub-corvus, sub-lictor, sub-kernel-proc, abi-trusted-seat]
created: 2026-11-02
---
## Fork

How does Corvus learn where its console episode runs? Operator vote 37, on an
I-27 P1 found by the audit round on [[dec-2026-10-29-seat-service-death]].

## Research

- Tree facts. Corvus chose serial when SEAT_QUERY's phase was normal, and
  `sys_console_open` has no episode check. A graphical SAK posts the `sak` note
  with the seat exclusive; a seat can reach normal before Corvus asks, when the
  service fails the episode and restores at once, or when the service dies. Corvus
  then prompted for the key on the serial console with nothing frozen, on any boot.
  The same code is on branch main at c5451ed84. Read in code, not reproduced.
- Peers. Linux's `VT_GETSTATE` reports the active terminal explicitly and leaves
  no client to infer it. Plan 9 has no seat or SAK to compare.

## Options

1. An explicit medium in QUERY's `code`: 0 none, 1 serial, 2 graphical (chosen).
2. Refuse `SYS_CONSOLE_OPEN` to the trusted Proc outside an episode.
3. Both.
4. Report a normal seat with no episode as failed.

## The call

Option 1. QUERY's `code` states the medium, derived under `g_proc_table_lock`:
active and exclusive is graphical, any other active episode is serial, and no
active episode is none. Other status replies keep the mask length there. Corvus
prompts on what the kernel states; on none it reports the episode over and sends
no END, since an END could close a newer episode.

## Rationale

The struct does not change, and Corvus, the only QUERY caller, never read the
field's old value. The phase keeps meaning the seat's phase. Option 2 enforces the
same fact in the kernel but leaves the medium inferred, so a serial episode under
a failed seat would still be dropped. Option 3 adds a check that cannot fire while
Corvus is correct. Option 4 makes the phase false, and `finish` reads it. Recorded
in TRUSTED-PATH section 7. Main takes the fix on its own line.
