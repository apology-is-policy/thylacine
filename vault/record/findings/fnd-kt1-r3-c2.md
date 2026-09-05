---
id: fnd-kt1-r3-c2
type: fnd
title: "`TEV_LAYOUT` is non-droppable and NOT coalesced -- every structural pass from ANY client appends one to the declared conn's lowest surface, and 128 of them while halcyond is not draining wedge-retire that surface, which logs the whole session out"
round: adt-kt1-r3
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close-r3
regression: "unconstructed (server-side coalescing; no host test harness for push_event)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:6113-6128 (the push in the structural branch), :6825-6895 (`push_event`: FRAME/PTR_REL/CONFIGURE/FOCUS have coalescing arms; everything else is the non-droppable push; at `EVENT_QUEUE_CAP` = 128 (:388) with no `coalescible()` event -- `{FRAME, PTR_MOVE, PTR_REL}` only, :854-859 -- it says `WEDGED (event overflow)` and returns false -> `retire(n)`), :6755-6758 (`session_notify_surface` = the LOWEST slot the declared conn owns, i.e. the root tile's surface by mint order); usr/halcyond/src/session.rs:648-657 (`Err(_)` from ANY tile's `poll_event` -> "session event stream ended (compositor gone); exiting" -> `logout = Some(1)`)
**Invariant**: I-9/I-32 as the R2-F1 precedent framed them (a focus flap storm filling a victim's queue with non-droppable FOCUS records wedge-retired it -- the reason FOCUS became coalescible); HALCYON 14.11.12 (no cross-tile effect, no halcyond death)
**Prosecution**:
1. Every `reconcile` whose `calc_geom_sig` changes is structural, and every structural pass pushes one `TEV_LAYOUT` (server.rs:6116-6128) -- a split, a close, a host, a retire of ANY surface (`retire` -> `reconcile`, 6640-6660), a resize ack that changes content, `session on/off`. The push is keyed on the declared conn, not on whose action it was: a foreign client's create/destroy churn (`tapestry-demo`, the battery, any program looping `Surface::open` + drop) lands two LAYOUT records per iteration on the session's root surface.
2. LAYOUT has no coalescing arm: the queue grows by one per pass, and once it holds 128 non-droppables the next push wedges the surface (6887-6893) and the structural branch retires it (`wedged.push(n)` -> `self.retire(n)`, 6126-6131). halcyond's `poll_event` on that surface answers `Err(Closed)` (ring.rs:175-186) -> the whole session exits (session.rs:648-652, 795-800): a session-wide death from another client's window churn.
3. Reachability is the honest caveat: halcyond drains every surface each loop iteration, so 128 must accumulate inside one non-draining window -- a multi-tile render + `cartoon::execute` (tens of ms), a `SessionTile::spawn` in `reconcile`, or `layout_verb`'s E_AGAIN naps (up to 40 x 10 ms, session.rs:91-105) -- against a compositor whose structural pass costs a full-screen flush (~250 passes/s at best). Marginal today; it is a NEW non-coalesced structural stream to a surface whose non-droppable input was previously user-rate only (KEY/BTN/CLOSE), and the R2-F1 lesson is that such streams get coalesced.
**Suggested fix**: coalesce LAYOUT exactly like CONFIGURE/FOCUS (`if let Some(c) = s.events.iter_mut().find(|e| e.kind == TEV_LAYOUT) { *c = ev; return true; }` -- the newest epoch subsumes the older ones; halcyond re-reads `layout` once per event anyway). Regression: a server unit test pushing 200 LAYOUT events on one surface asserts the queue holds one and the surface is not wedged.

## Disposition

Fixed in the round-3 close: `push_event` coalesces `TEV_LAYOUT` exactly like CONFIGURE and FOCUS -- an unread queued one is replaced wholesale by the newest (the epoch subsumes older ones; the reader re-reads `layout` once per event), so a structural churn from any client against a momentarily non-draining session can never fill the declared conn's queue. Unconstructed by a gate (a 128-deep churn against a stalled halcyond).
