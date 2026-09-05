---
id: adt-kt1-r2
type: adt
title: "KT-1: round 2 -- the re-prosecution of the round-1 fixes (two prosecutors)"
date: 2026-09-05
scope: [sub-tapestryd, sub-stratum-session]
reviewer: fable
model-start: claude-fable-5-1
model-end: claude-fable-5-1
verdict: dirty
counts: {p0: 0, p1: 1, p2: 4, p3: 7}
findings: [fnd-kt1-r2-c1, fnd-kt1-r2-c2, fnd-kt1-r2-c3, fnd-kt1-r2-c4, fnd-kt1-r2-b5]
round-of: chg-2026-09-05-kt1-audit-close
prior-round: adt-kt1-r1
created: 2026-09-05
---
## Scope

Two Fable 5.1 prosecutors (MODEL start == end on both) on 062efe18's fixes, static: **C2** the compositor + session-authority fixes (server.rs, pane.rs, login, the battery, the scenario, caps-probe) -- 0/0/1/3; **B2** the seam + ingest fixes (halcyond session/tile/transcript/chrome/tiles, kaua-term, ptyhost, vt) -- 0/1/3/4. Not dirty by the count rule (P1 + P2 = 5), but DIRTY by invasiveness: the B2-F1 fix restructures the tile's render path (a windowed layout with a per-block height cache) and the B2-F4 fix restructures the producer/bin emit loop, so a focused round 3 re-reads both.

## Convergence

The owned-surface findings are the `fnd-kt1-r2-*` notes. The unowned-surface findings (halcyond, kaua-term): **B2-F1 [P1, FIXED]** -- the per-render layout transient was O(retained cells) (~1.8x) and outside every budget: one tile with ~20K full-width rows still ended the session at its next paint; `Tile::render` now caches every frozen block's laid height (12 B/block, keyed by width, aligned to the front-evicted/back-appended frozen deque) and lays out only the blocks intersecting the view, one `LaidBlock` alive at a time -- the transient is O(view) wherever the view scrolled (host tests: warm render <= 4 blocks laid for 200 in history; the cache follows eviction + width changes; content height exact). **B2-F2 [P2, FIXED]** -- `set_max_cost` was enforced only at the next freeze, so a quiet tile kept its old share and the retained sum grew as 32 MiB x H(N); it now freezes an over-cap open block and evicts immediately (host test). **B2-F3 [P2, FIXED]** -- the A-F6 queue's drop-newest was record-blind: a Resize dropped at the cap was never re-sent and the tile stayed at the old geometry for life; `DownQueue` gives the geometry record its own never-dropped latest-wins slot delivered before any further key at a record boundary (4 host tests). **B2-F4 [P2, FIXED]** -- `scroll_cap` bounded ONE ScrollOff, not how many one `feed` accumulates before `emit` (`ESC [ 36 S` x 819 in a 4 KiB read = 29K rows, ~60 MiB); `Producer::feed_into` takes a sink that ships each capped ScrollOff as it lands (host test: the sink never sees more than one cap's rows). **B2-F6 [P3, FIXED]** -- a structural change with no hosted surface in it (a split of an EMPTY leaf) fanned nothing to the conn that must claim the empties; a new event kind `TEV_LAYOUT` (10) goes to one surface of the declared session conn on every structural pass. **B2-F7 [P3, FIXED]** -- the shrink's scrolled-off rows reached the transcript only at the next feed; `apply_resize` drains the vt's pending boundaries through the producer before the full diff (host test: [ScrollOff, CellDiff]). **B2-F8 [P3, FIXED]** -- the POLLOUT entries could push `nfds` past the kernel's 64 and the -1 read as "compositor gone"; the entries stop at the ceiling.

Lesson carried (a hazard): **a budget on STORED bytes does not bound the DERIVED working set** -- the render laid out everything the budget allowed to be retained, and the transient it built from that was never charged.
