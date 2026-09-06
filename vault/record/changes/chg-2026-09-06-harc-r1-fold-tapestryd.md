---
id: chg-2026-09-06-harc-r1-fold-tapestryd
type: chg
title: "H-arc round-1 fold (tapestryd half): the six A-F GPU/compositor findings -- the composed-arm stale-slot expansion, the draining resize-ack re-offer, the process-keyed creator reservation, the latch-flip bar floor, the hosting-gated menu seat, and the re-offer + partial-first test coverage"
date: 2026-09-06
arc: arc-vault
commits: ["eb480b58"]
touched:
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The peer's H-arc audit round-1 close ([[chg-2026-09-06-harc-audit-close-r1]], main's
`839a966f`) carried a `no-dossier-change` deferring the vault prose across five
dossiers -- the KT-1 inheritance pattern. The UI + beacon-relay half folded hours
earlier ([[chg-2026-09-06-harc-r1-fold-ui]]); this is the deferred tapestryd half,
the six A-F GPU/compositor findings, each verified in
`usr/tapestryd/src/{server,pane}.rs` before a word was written.

## [[sub-tapestryd]] -- the H-arc round-1 audit close

A new "The H-arc audit close, round 1" Provenance sub-section carries the round
header ([[adt-harc-r1]]: three Fable 5.1 prosecutors, 0 P0 / 2 P1 / 0 P2 / 11 P3)
plus all six findings; three of them also amend the mechanism's natural home, so the
always-read body stays accurate and the sub-section is the chronological round record.

- **A-F1 [P1]** ([[fnd-harc-r1-a1]]): the composed GPU arm (`server.rs`, the
  `res_stale[slot]` gate) expands a stale slot's first transfer to the FULL surface
  exactly as the direct arm does, then un-stales after ANY transfer -- because once
  the #56 letterbox re-key served single-slot PARTIAL presents, a whole scaled blit
  of a never-filled slot composited bytes no present carried. The zoom section
  already anticipated "the next tapestryd round"; A-F1 + A-F4 discharge it.
- **A-F2 [P1]** ([[fnd-harc-r1-a2]], also amends "The generation fence"):
  `Surface.ack_deferred` + `release_displaced_gen` re-offer a resize-ack refused
  mid-drain under a fresh serial -- a recovery no client implemented, latent since
  G-6b.
- **A-F3 [P3]** (also amends "The creator reservation"): `Pane.creator_peer` beside
  `creator_conn`; `host_for(n, conn, peer)` keys the claim-LESS create's reservation
  on the PROCESS -- the conn-keyed version was caught in ls-gfx-panes' tabbed leg.
- **A-F4 [P3]**: `floor_bars_around(n)` paints the four bands around a surface at
  the #56 latch flip, where no structural pass repaints the pane.
- **A-F5 [P3]** (also amends "The menu"): `role=menu` + the `menu ` verbs gated
  `session_declared && conn_hosts` -- an idle declarer that hosts nothing gets no
  menu (else it could float one, take the grab, and force Composed with no tile).
- **A-F6 [P3]**: ls-gfx-panes gained scenario 2a (the draining re-offer) + a
  partial-FIRST single-slot client E (the A-F1 witness); the battery stays
  [[seam-tapestry-battery-unowned]].

`updated:` -> 2026-09-06. OWED at the peer (the implementation side, not this fold):
the GPU-path witness for A-F1 on the GL host + aux's real-DOSBox-X re-run. This
completes the H-arc round-1 fold across both halves.
