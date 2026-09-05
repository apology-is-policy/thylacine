---
id: adt-kt1-r1
type: adt
title: "KT-1: the pollable Loom + the kaua-term seam + the per-user session compositor -- round 1 (three prosecutors)"
date: 2026-09-05
scope: [sub-tapestryd, sub-kernel-loom, sub-kernel-poll, sub-stratum-session, sub-mechanism-drivers]
reviewer: fable
model-start: claude-fable-5-1
model-end: claude-fable-5-1
verdict: dirty
counts: {p0: 3, p1: 5, p2: 8, p3: 17}
findings: [fnd-kt1-r1-c1, fnd-kt1-r1-c2, fnd-kt1-r1-c3, fnd-kt1-r1-c4, fnd-kt1-r1-c5, fnd-kt1-r1-c6, fnd-kt1-r1-c7, fnd-kt1-r1-c8, fnd-kt1-r1-c9, fnd-kt1-r1-c10, fnd-kt1-r1-c11, fnd-kt1-r1-c12, fnd-kt1-r1-c13, fnd-kt1-r1-a1, fnd-kt1-r1-a2, fnd-kt1-r1-a3, fnd-kt1-r1-a4, fnd-kt1-r1-a5, fnd-kt1-r1-b8]
round-of: chg-2026-09-05-kt1-audit-close
created: 2026-09-05
---
## Scope

The batched KT-1 round on 53ee407f, three Fable 5.1 holotype-reviewer prosecutors in parallel (MODEL start == end on all three), static: **A** the pollable-Loom substrate (`loom_poll` + the KOBJ_LOOM `poll_scan_one` arm + halcyond's unified poll) -- 0/0/0/6; **B** the kaua-term seam (the vt capture, the producer, the wire codec, the bin's two threads, ptyhost, halcyond's ingest + tile model) -- 2/3/4/5; **C** the compositor + session authority (tapestryd's backgrounding/transparency/F2 + `halcyond --session` + login's spawn) -- 1/2/4/6. DIRTY (three P0s; P1 + P2 = 13). Every P0/P1/P2 was fixed in 062efe18 (`KT-1 audit close, round 1`); the closed-list preamble of the era is memory `audit_kt1_closed_list.md`.

## Convergence

The findings on OWNED surfaces are the `fnd-kt1-r1-*` notes above (the tapestryd/login/ptyhost/kernel ones). The findings on the still-UNOWNED surfaces (`usr/halcyond`, `usr/kaua-term`, `usr/lib/vt`, the battery -- no dossier; `docs/reference/150-halcyond.md` + `152-kaua-term.md` are their as-built homes) stay in this body until those sweeps, per the weft7 precedent: **B-F1 [P0]** = C-F1 (the identity capability). **B-F2 [P0, FIXED]** -- the session reconcile read a HIDDEN leaf (a zoom, a tab) as vanished and killed its shell + jobs; `parse_leaves_all` + `Leaf.hidden`, hidden never creates/drops (2 host tests + the ls-gfx-session zoom leg). **B-F3 [P1, FIXED]** -- alt-screen enter/leave left the wrong content in halcyond's single grid; full CellDiffs at both boundaries. **B-F4 [P1, FIXED]** -- a capped ScrollOff x 3 copies exceeded the kaua-term's 4 MiB heap; a 256 KiB cell bound in `scroll_cap` + a lazy 32 MiB span (the per-FEED accumulation was left open -> round 2 B2-F4). **B-F5 [P1, FIXED in part]** -- halcyond's per-tile 32 MiB budgets x N vs a 64 MiB heap + zero-cost empty lines; ONE session budget shared by tile count + `ITEM_OVERHEAD` per line (the render transient + the lazy re-budget were left open -> round 2 B2-F1/F2). **B-F6 [P2, FIXED]** -- the resize applied after the post-SIGWINCH repaint was parsed; `apply_resize` after the read too. **B-F7 [P2, FIXED]** -- an unbounded Title latch; `MAX_TITLE` 256. **B-F8 [P2]** = [[fnd-kt1-r1-b8]]. **B-F9 [P2, FIXED]** -- session shells lacked `--home`; login forwards it, halcyond appends it. **B-F10 [P3, FIXED]** -- TEV_FOCUS + close drive a reconcile (the empty-to-empty focus move left open -> round 2 B2-F6). **B-F11 [P3, OPEN]** -- the untrusted kaua-term inherits the console handle as fd 2 (`Stdio::Null` unimplemented). **B-F12 [P3, FIXED]** -- the vt shrink discarded the top rows with no Scroll boundary; rows scroll off under capture. **B-F13 [P3, FIXED]** -- no AUDIT-TRIGGERS row; two rows appended. **B-F14 [P3, FIXED]** -- the down-channel decoder after TooLarge; teardown. **A-F6 [P3, FIXED]** -- the session compositor wrote a tile's down-pipe with a blocking loop from its only thread; a bounded queue + POLLOUT-gated one-byte writes (the record-blind drop was left open -> round 2 B2-F3).

Lessons carried: a `Command` that inherits every capability by default makes every spawn under a holder a leak until masked; a parser that filters a state (hidden) makes its consumer read that state as absence; a budget per ITEM is not a budget on the CONTAINER.
