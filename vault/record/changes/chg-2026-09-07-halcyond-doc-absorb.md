---
id: chg-2026-09-07-halcyond-doc-absorb
type: chg
title: "absorb docs/reference/150-halcyond (the Halcyon environment client): clean redirect -- sub-halcyond is fresh, audit:hard, all audit anchors covered"
date: 2026-09-07
arc: arc-vault
commits: ["35f0bfca"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
halcyond (the Halcyon environment client, H-2..H-4c: the untrusted-OSC-1936
transcript renderer + the --session per-user compositor). quaestor owner:
usr/halcyond/src/*.rs -> sub-halcyond (audit:hard, fresh 2026-09-06). All 19
halcyond source files claimed (no orphan). Verified atom-by-atom.

VERIFIED all six of the doc's load-bearing invariants ("the audit anchors")
present in the dossier: (1) the streaming property (safe_cut holdback protecting
the escape OPEN at buffer end; the last-ESC-heuristic bug; the byte-by-byte
fingerprint regression) [7 hits]; (2) the robustness contract (never panic,
FRAME_MAX+16 flush, malformed-skips-fail-safe) [5]; (3) span hygiene (em/obj/hdr
die at block boundaries, SGR pen persists) [2]; (4) the section-13.5 metrics rule
(body box owns mixed lines) [3]; (5) budgets (deque eviction + stored cost +
per-block line cap) [13]; (6) raw_vt_intent (alt-screen latch) [6]. Plus the
lib/bin split, the --session compositor, the tile model, chrome (H-3b), menu
(H-3c), session init (H-4c), and the poll/Loom ingest.

CLEAN REDIRECT, zero fold. The dossier is the current source of truth (this doc
froze at H-4c; the dossier tracks the post-KT-1-audit + H-4d beacon-tier state).
Redirect stub. Zero code change.
