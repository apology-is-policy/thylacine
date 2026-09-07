---
id: adt-harc-r1
type: adt
title: "The batched H-arc round 1: the fullscreen-zoom fix, H-4c, H-4d-1, H-4d-2a/2/3 under three parallel prosecutors"
date: 2026-09-06
scope: [sub-tapestryd, sub-halcyond, sub-kaua-term, sub-lib-vt, sub-ptyhold, sub-kernel-syscall-dispatch]
reviewer: fable
model-start: claude-fable-5-1
model-end: claude-fable-5-1
verdict: clean
counts: {p0: 0, p1: 2, p2: 0, p3: 11}
findings: [fnd-harc-r1-a1, fnd-harc-r1-a2]
round-of: chg-2026-09-06-harc-audit-close-r1
created: 2026-09-06
---
## Scope

The double-the-distance rule batched six chunks into one round: the fullscreen-zoom fix `f25781ad`, H-4c `26f903a0`, H-4d-1 `c96f5173`, H-4d-2a `8f553c78`, H-4d-2 `946ac379`, H-4d-3 `4f4e7a9f` (+ the repair `da0a5c10`), on audit-trigger rows 42, 136, 142 and 151. Three prosecutors in parallel, each with a scoped brief and one common preamble (the ten H-arc closed lists concatenated as the do-not-re-report set): **A** = tapestryd (the #56 latch re-keyed on rotation, `ComposeOp.clip`, the H-4d-1 creator reservation, the widened menu role) -- 80 turns on Fable 5.1, start == end; **B** = the session compositor + the pts-host producer (the cell-span model, the tile menu, the typed choice, the H-4c/H-4d-1 spawns, the 17-byte wire cell) -- 51 Fable turns then an 11-turn Opus 4.8 tail (the JSONL `model` field; `MODEL(end)` claimed Fable); **C** = the kernel `'t'` arm + the emission gate + the halcyon tool + the bake -- 60 Fable turns (the kernel arm, pts.c, the transport, the gate, ut) then a 40-turn Opus 4.8 tail (the tool, the layout parser, the consumer sweep, the report). Two finished fallback rounds are CLOSED under the never-skip rule; the coordinator's parallel self-audit had independently read every Opus-tail surface. Static, HEAD f626fe04.

## Convergence

**A: 0/2/0/4.** [[fnd-harc-r1-a1]] (P1, introduced by the batch): the composed GPU arm whole-op-blitted a slot resource that never received the full frame -- the letterbox arm now serves a single-slot client's PARTIAL presents, and `res_stale` was never consulted there. [[fnd-harc-r1-a2]] (P1, PRE-EXISTING since G-6b, owned under whole-system stewardship): the resize ack's DRAINING E_AGAIN told the client to "present, then re-ack" and no client did. A-F3 [P3]: a claim-less create's focused-leaf fallback took a leaf another PROCESS had reserved (`Layout::host_for(n, conn, peer)` now treats it as occupied; the first, conn-keyed cut moved the battery's B beside its own pre-split leaf and ls-gfx-panes caught it in the tabbed leg's geometry -- a reservation keyed on the conn holds off the same process's other conns). A-F4 [P3]: the latch flip left the first frame's scaled projection outside the crop until the next structural repaint (`floor_bars_around` at the latch). A-F5 [P3]: the widened menu seat admitted an IDLE declarer (`session_declared && conn_hosts` on the `role=menu` create and the `menu ` verbs). A-F6 [P3]: the singleslot leg's placement-line arm was satisfied by pre-fix code -- D presents FULL before the zoom, so the pixels discriminate, not the line (said in the leg; the partial-FIRST client E added as the arm the line can only pass post-fix).

**B: 0/0/0/5.** The anti-clickjack chain VERIFIED end to end (the frame record precedes every cell it stamps; a rejected frame maps to the pre-frame state; the ring validates the full serial; the grid run resolves through the current cell at act time). B-F1/F2: `scroll_cap` sized a row by the pre-span 16-byte cell, bounded either way (`size_of::<Cell>()` + the comments). B-F3: `local_obj`'s remap cache was a linear scan (a `BTreeMap`). B-F4: the span ring was an eager 196 KiB per tile outside the scrollback budget (lazy on the first note + 16-byte slots: `SPAN_MAP_BYTES` = 128 KiB per RICH tile, recorded in the I-32 accounting, not charged). B-F5: Normal mode lingered through an alt-screen entry (left on `Record::Mode(AltScreen)`).

**C: 0/0/0/2.** The `'t'` arm sound on all six row-136 points (identity = the registry's ref-held (conn, qid) pointer compare under the leaf `g_pts_lock`; every stale / foreign / master / unopened / loopback Spoor fails closed to `'9'`; one clunk per exit; I-27's `spoor_is_console` untouched). C-F1: ptyhost declared no tier -- an upstream `BEACON=rich` passed through a host that renders nothing (cosmetic, never authority; `ptyhold::declare_beacon` + `relayed_tier`, pty-4's inner shell witnesses `beacon cells inherited (pts host)`, the aurora console's tier relayed). C-F2: the arm had only negative unit controls (`9p_srvconn_transport.pts_slave_spoor_classifies_t`, the two existing fixtures composed).

The coordinator's self-audit found three of the P3s independently (B-F1/F2, B-F5, and the resize-ack diagnostic line's meaning, now documented in 139-tapestryd) and re-derived the batch's central claims sound before any report landed. Not dirty: no P0, P1 + P2 = 2, every fix local (a mirror of the direct arm; a flag and a re-emit; a predicate; a fill; a conjunct). OWED: the GPU-path witness for A-F1 on the GL host (ls-gfx-panes runs on HVF, the CPU compose) and aux's real DOSBox-X re-run of the zoom fix.
