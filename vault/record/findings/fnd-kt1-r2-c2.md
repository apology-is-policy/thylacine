---
id: fnd-kt1-r2-c2
type: fnd
title: "`retire_conn` un-declares BEFORE retiring the conn's surfaces, so a compositor death with N >= 2 hosted tiles un-backgrounds the console N-1 reconciles early -- intermediate structural composed passes tile the dying session's stale frames beside the console before the final Direct(console)"
round: adt-kt1-r2
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none automated (a compositor crash with >= 2 tiles is unconstructed)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:6708-6715 (`session_conns.retain` first, then the retire loop), :6621 (each retire's in-block `reconcile()`), :5793-5808 (has_session_tree -> bg_tiling), :5996-6005 (the structural repaint + `prefill_from_shown` reading the dying tiles' shown slots)
**Invariant**: HALCYON.md 14.12 step 4 ("On logout ... aurora is no longer backgrounded ... resumes + repaints -> Direct(aurora)") -- one transition, not N.
**Prosecution**:
1. State: the declared conn hosts tiles A, B (root = SplitH[aurora(bg), A, B]); halcyond dies (crash, or `logout = Some(1)` on a wedged present) with both surfaces live. The conn EOFs -> `teardown` -> `retire_conn`.
2. `self.session_conns.retain(|&c| c != conn_id);` runs FIRST (6709). `retire(A)` -> `layout.close(A's leaf)` -> `reconcile()` (6621): `has_session_tree` is now false (no declared conn) although B is still hosted -> `bg_tiling` empty -> `apply_backgrounded` clears aurora -> recompute gives aurora a HALF-width column beside B -> `calc_geom_sig` changes (aurora's content ZERO -> real) -> `structural` -> `paint_chrome()` + `prefill_from_shown()` (composes B's last-presented slot -- a dead client's frame -- and aurora's dormant one) + `screen_flush_full()` + the CONFIGURE fan (aurora offered a half-width rect it declines by crop; B's dead conn gets a coalesced CONFIGURE). If the scanout was Direct(A) it goes Composed here (`ensure_screen` may allocate the screen resource mid-retire).
3. `retire(B)` -> the second reconcile -> root dissolves to aurora -> Direct(aurora) pending + the dw x dh CONFIGURE -> the console resumes. Pre-fix (principal-keyed `has_session_tree`) aurora stayed backgrounded until the LAST session surface retired: one transition. The normal logout path (halcyond destroys each tile's surface itself before exiting, so the conn dies with zero surfaces) is unaffected -- only the crash path pays N-1 extra structural passes and shows a frame of stale dead tiles beside a stale console frame.
**Suggested fix**: move `self.session_conns.retain(..)` AFTER the retire loop. The last retire's reconcile already sees a declared conn hosting nothing (`has_session_tree` false) so the final state is identical, and the intermediate passes keep the console backgrounded exactly as before the fix. (`session off` keeps its explicit un-background semantics; the declared-conn crash is the only path changed.)

## Disposition

Fixed in the round-2 close: `retire_conn` clears the declaration AFTER its retire loop, so a compositor crash with N tiles keeps the console backgrounded through the N-1 intermediate reconciles and lands one transition to Direct(console); the last retire already sees a declared conn hosting nothing. `session off` keeps its explicit semantics.
