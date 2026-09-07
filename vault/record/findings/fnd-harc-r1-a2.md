---
id: fnd-harc-r1-a2
type: fnd
title: "the resize ack's E_AGAIN conflates STALE with DRAINING, and every consumer implements only STALE -- an offer sent while a generation drains is lost until an unrelated relayout (pre-existing since G-6b; surfaced by the session gate's resize-ack line)"
round: adt-harc-r1
severity: P1
status: fixed
surface: [sub-tapestryd, sub-libtapestry]
threatens: []
fixed-by: chg-2026-09-06-harc-audit-close-r1
regression: "ls-gfx-panes scenario 2a `resize reoffer OK` (ack, do not present, zoom, ack the zoom -> Busy, present -> the compositor's `re-offer ... after the drain` line, the re-offered CONFIGURE lands the zoom); pre-fix the leg times out waiting for the re-offer"
created: 2026-09-06
---
## Prosecution

**File**: usr/tapestryd/src/server.rs (`resize_ack_inner`: the stale-serial arm and the `old_weave.is_some()` arm both answer E_AGAIN; `emit_configure_to`; the test-mode refusal line), usr/lib/libtapestry/src/lib.rs (`handle_configure` / `reweave`: `Busy` = "keep draining events; a newer CONFIGURE carries the current offer"), usr/halcyond/src/session.rs (`Err(TapError::Busy) => {}` inside a drain-everything-then-present loop), the SDL backend's CONFIGURE arm
**Invariant**: the TAPESTRY 18.3 resize protocol; the W-3c-1 round-6 class (a client misreading E_AGAIN as permanent wedges the surface)
**Prosecution**:
1. The server answers E_AGAIN both for a stale serial ("re-ack the newer one") and for a prior reweave still draining ("present a frame, then re-ack"); `old_weave` clears only at the first post-fence present.
2. The library's contract names only the stale meaning; halcyond and the SDL backend follow it.
3. Pass A offers S1 (serial k); the client acks inside its drain loop -> success, the old generation parked. Before it presents, pass B (the restore tool's next split, a dump-read RPC apart) offers S2 (k+1); the same loop reaps and acks k+1 -> the serial is current, the size differs, `old_weave` is Some -> E_AGAIN -> dropped.
4. The client presents at S1; nothing re-issues S2; the tile stays mis-sized until an unrelated structural pass.
5. The session gate's `serial 4 refused ... state Some((6, Some((6, 312, 772)), true, false))` line is the OTHER arm (`draining` false; two same-size passes bumped the serial) and is benign -- the queued serial-6 CONFIGURE is re-acked. The gates pass because every restore ends in hosting passes that re-offer.
**Suggested fix**: at `release_displaced_gen(n)` re-emit a standing offer whose size differs from the current, or have the library re-ack a Busy offer after the next present; pair every refusal with an `ok` line so a gate can prove the line benign.

## Disposition

Fixed server-side at the close, for every client at once: the draining refusal sets `Surface.ack_deferred`, and `release_displaced_gen` (the first post-fence present) re-emits the standing offer under a fresh serial when it still differs from the current size (test builds: `resize-ack <n> re-offer WxH after the drain`); the client's ordinary drain-and-ack path lands it from there. Test builds also pair every refusal with its recovery (`resize-ack <n> WxH serial S ok after a refusal`), and 139-tapestryd now reads the session gate's refusal line as the stale arm at work. The scripture (TAPESTRY.md 18.3) records that the "present, then re-ack" recovery is no longer required of clients; libtapestry's `Busy` contract stands (drain, ack the newest).
