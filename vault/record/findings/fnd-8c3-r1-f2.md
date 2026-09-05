---
id: fnd-8c3-r1-f2
type: fnd
title: "The role-release covered one of FOUR reader_active sites"
round: adt-8c3-r1
severity: P2
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-07-17-8c3-reader-role
created: 2026-07-31
---
## Prosecution

stop_unwinds was set only at the client_wait election; the self-pump
(client_pump_or_park_locked) and both pump_once variants also hold
reader_active. A debug-stopped self-pumper (an EL0 Proc back-pressured on
c2s -- the go build's concurrent object writes) parks in place holding the
role -> freezes every survivor on the shared client: the exact #89 bug, on
the send path.

## Disposition

Fixed: the frame-atomic wrapper centralizes the flags so all four sites get
them; each caller handles a stop-unwound recv (no mark_dead on a stop;
send_flow/drain park a stopped sender at loop-top, spilling first so the
stop cannot hang on an eternal EAGAIN).
