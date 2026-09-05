---
id: adt-kt1-r3
type: adt
title: "KT-1: round 3 -- the round-2 restructures re-read (one prosecutor)"
date: 2026-09-05
scope: [sub-tapestryd]
reviewer: fable
model-start: claude-fable-5-1
model-end: claude-fable-5-1
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 6}
findings: [fnd-kt1-r3-c2, fnd-kt1-r3-c6]
round-of: chg-2026-09-05-kt1-audit-close-r3
prior-round: adt-kt1-r2
created: 2026-09-05
---
## Scope

One Fable 5.1 prosecutor (MODEL start == end) on the round-2 fixes at cf499fe1 plus the coordinator's parallel self-audit edits, static with two host experiments (the real producer under an alt-toggle stream; the equal-count resize): the windowed tile layout, the eager re-budget, `DownQueue`, the feed sink, the declared seat's takeover rule, `retire_conn`'s order, `TEV_LAYOUT`. Verdict 0/0/1/6 -- clean by count, no restructure. Every finding fixed in the round-3 close. The coordinator's self-found item (a floating exit mark could mutate a frozen block after its height was cached -- the cache is now keyed on (id, exit, height), with a host test) landed before the report and was re-derived by the prosecutor as the ONLY post-freeze mutation path.

## Convergence

The tapestryd findings are [[fnd-kt1-r3-c2]] (the un-coalesced `TEV_LAYOUT`) and [[fnd-kt1-r3-c6]] (the same-principal takeover of a live holder; the declare -> first-mint window). The unowned-surface findings, all FIXED: **F1 [P2]** -- the round-2 feed sink fired only on the Scroll arm, so an alt-screen toggle stream (eight bytes and a full screen each way) piled 512 full diffs per 4 KiB read (45 MiB at 128x36; 32 MiB from 320 bytes at 4K, measured); the sink now triggers on the CELLS held in `out` (`cells_in`: ScrollOff rows + CellDiff entries) reaching `SCROLL_ACC_BYTES`, checked after every boundary (host test: 256 toggle pairs, the sink never holds more than the bound plus one screen; the sink-less control accumulates 512 screens). **F3 [P3]** -- the B2-F7 drain ended in `feed`'s screen diff against the OLD-geometry shadow, so an equal-cell-count resize (80x24 -> 96x20) shipped a stale-pitch CellDiff before the full one (measured); `Producer::drain_pending` drains the pending boundaries with no diff (host test: exactly [ScrollOff, CellDiff(full)]). **F4 [P3]** -- the windowed render's comment claimed one laid block alive and an O(view) transient, while the OPEN block (no cache can position it) is laid whole every render and a straddling block whole too: up to `share/8` = 4 MiB of cells at a 32 MiB share; `OPEN_BLOCK_MAX_COST` (512 KiB) now caps the open block whatever the share, the comment and 150-halcyond.md state the O(view + 2 x cap) bound, and `laid_lines_last` witnesses it (<= 12 lines laid for 200 blocks of history). **F5 [P3]** -- the re-budget's eviction floor kept one block sized by the OLD open cap (4 MiB x H(N) across quiet tiles); the same constant bounds it (host test: after a re-budget from 32 MiB to 1 MiB the residue is within the constant). **F7 [P3]** -- three doc/comment drifts the round-2 close introduced (the 12 B/block and one-block claims; the `emit_celldiff` mismatch claim; the "32 live tiles" ceiling comment -- 30 is the reachable maximum, so the ceiling arm is a defence and now says so).

The round-3 lesson refines the round-2 hazard: the bound must be per record CLASS -- a sink keyed on one class (ScrollOff) left the other screen-sized class (the full diff) exactly where the first fix found the first.
