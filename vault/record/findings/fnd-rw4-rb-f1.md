---
id: fnd-rw4-rb-f1
type: fnd
title: "RW-4 R-B-F1: the round-1 fail-closed latch was over-broad — a local fid-full killed the shared session"
round: adt-rw4-r2
severity: P2
status: fixed
surface: [sub-kernel-ninep-session]
threatens: []
fixed-by: chg-2026-06-10-rw4-fixes
regression: "9p_session.walk_fid_full_no_latch"
created: 2026-08-01
---
## Prosecution

Round-1's R3-F1 fix latched the client dead on ANY negative dispatch —
but the premise "dispatch failure == protocol violation" is FALSE for
the Twalk fid_bind-full leg: a LOCAL 256-fid-table exhaustion, not a
server fault. The fix therefore took the whole shared root-FS session
down on the 257th concurrent fid — strictly WORSE than the round-1
slot leak it closed. Found by the dirty-close re-prosecution of the
fixes themselves.

## Disposition

Fixed: a send-side fid-capacity pre-check (fail-closed at send) + the
dispatch fid_bind-full case surfaced as a SYNTHETIC per-op error
(reusing the audited Rlerror path; dispatch returns 0 → no latch).
Non-invasive classification refinement — no round 3 owed. The recorded
LESSON (the reason this note exists beyond its fix): latching a SHARED
resource dead on a LOCAL/per-op condition is the recurrent "fix more
severe than the bug" dirty-close hazard — distinguish
protocol-violation (latch) from local-resource (per-op error) AT the
classification site; never let a -1 token conflate them. Residual
(pre-existing, out of scope then): the local bind failure leaks the
SERVER-side fid — bounded by the fid cap + the trusted server; the
v1.x Tclunk-on-local-bind-failure rides
[[seam-fid-monotonic-reclaim]]'s hygiene chunk.
