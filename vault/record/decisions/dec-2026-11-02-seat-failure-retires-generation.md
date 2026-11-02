---
id: dec-2026-11-02-seat-failure-retires-generation
type: dec
title: "A seat back at normal through a failure retires its generation"
date: 2026-11-02
status: standing
decided-by: research-collapsed
affects: [sub-corvus, sub-lictor, sub-kernel-proc, sub-kernel-death, abi-trusted-seat]
created: 2026-11-02
---
## Fork

How does Corvus tell, polling after END, that a restore committed the held grant
and that a failure did not cancel it? Raised by the audit round on
[[dec-2026-10-29-seat-service-death]] and [[dec-2026-11-02-seat-episode-medium]],
which found that vote 36 made the wrong answer deterministic.

## Research

- Tree facts. `EpisodeIo::finish` returned true on phase normal at its generation.
  A service death in restoring cancels the grant and sets normal with the
  generation unchanged, all in one lock hold, so failed is never observable.
  Corvus then sent the requester OK and the session, and the redeem failed.
  RESTORED from failed reached the same read with a live service.
- Scripture already answers it. GRAPHICAL-SAK-OWNERSHIP says "Cancel the pending
  grant and invalidate the generation" and GRAPHICAL-SAK-PORTABILITY says "revoke
  the old generation before recovery". The code did neither.

## Options

1. Move the generation at each return to normal through a failure (chosen).
2. Move it inside `proc_seat_fail_locked`.
3. A new reply field stating that the generation committed.

## The call

Option 1. The service's death and RESTORED from failed move the generation, and a
commit keeps it. Corvus's existing `m.generation != self.generation` check then
returns false, with no change to any reply field. No vote: this is the scripture's
stated behaviour, and the size, layout and operations do not change.

## Rationale

Option 2 moves the generation while the seat still reads failed, and Lictor keys
its private failure notice on the generation it activated, so the notice would be
lost. Option 3 adds an ABI field to say what the generation can already say.

## Corrections to the two earlier records

The records are append-only, so these stand here. Of
[[dec-2026-10-29-seat-service-death]]: the serial BREAK recovers the seat only on a
boot with `thylacine.serial-sak=1`; on a graphical-only boot a dead Lictor leaves no
SAK path until a reboot, which fails closed. Of
[[dec-2026-11-02-seat-episode-medium]]: Corvus is the only reader of QUERY's reply,
not its only caller, since caps-probe calls it to assert the refusal.

## Also landed

Corvus asks QUERY before a graphical END and sends it only while the medium names
its own episode. END names no episode, so a stale one could close a serial episode
the operator has just opened. The two calls leave a window that only an END naming
its generation could close, an ABI change not taken here.
