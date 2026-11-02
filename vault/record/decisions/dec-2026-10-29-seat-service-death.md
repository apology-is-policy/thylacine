---
id: dec-2026-10-29-seat-service-death
type: dec
title: "A dead seat service ends the seat: the kernel returns it to normal"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-proc, sub-kernel-death, sub-lictor]
created: 2026-10-29
---
## Fork

When Lictor, the graphical seat's service, dies after boot, what does the kernel
do with the seat? Operator vote 36.

## Research

- Tree facts. Lictor is `lifecycle = persistent`. Once it is READY, warden drops
  the child unwaited and walks away, and `next_step(Served)` settles `Up` under
  every restart policy, so nothing restarts it; `restart = on-crash` covers only
  a crash before readiness. Tapestry is started once and never restarted either.
  The zombie-path arm failed the seat and cleared the service, and
  `proc_console_sak_from` refuses the serial BREAK while the seat is not NORMAL.
  A restarted service could not have bound anyway while the compositor lived,
  since `proc_set_seat_service` requires an empty client slot. The seat stayed
  FAILED for the boot, with the display dark and the serial recovery medium
  refused: the outcome operator vote 30 rejected for a dead GPU engine, reached
  through a different door.
- Heritage and peers. Linux resets a VT whose `VT_PROCESS` owner has died to
  `VT_AUTO`, so no handshake is owed to a dead process, and `do_SAK` is honoured
  in any mode. systemd-logind restores `KD_TEXT` and `VT_AUTO` when a session's
  controller drops. Plan 9 has no seat; when rio dies, the kernel console is what
  remains. Each returns to the safe default when the owner dies.

## Options

1. The service's death returns the seat to NORMAL, as RESTORED from FAILED does,
   releasing nothing (chosen).
2. Admit the serial BREAK into a FAILED seat that has no service, leaving the
   seat state FAILED.
3. Option 1 plus warden supervision of Lictor and Tapestry after READY, with a
   new service allowed to bind over a FAILED seat whose compositor lives.

## The call

Option 1. The zombie-path arm fails the seat (cancelling the held grant,
scrubbing the key queue and the frame, closing the console episode only if the
seat opened it), clears the service, and sets the phase to NORMAL with the held
keys and the mask cleared. The serial BREAK reaches the console again; the
display stays dark until a reboot. A FAILED seat whose service lives still
refuses the BREAK and recovers through RESTORED.

## Rationale

The failure has already cancelled every authority the seat held, so NORMAL
releases nothing. A seat with no service routes no input and no frame, and every
seat op needs the service or corvus (corvus's frame, key, mask and grant ops need
EXCLUSIVE), so a compositor that outlives the service gains nothing.
Option 2 recovers the same way but leaves a state that no longer means
"recoverable by its owner". Option 3 is a warden design change, and rebuilding
the GPU state after a restart is unproven. Recorded in TRUSTED-PATH section 7.
