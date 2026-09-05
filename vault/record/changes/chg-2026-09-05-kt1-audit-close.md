---
id: chg-2026-09-05-kt1-audit-close
type: chg
title: "KT-1 audit close, rounds 1-2: the identity mask, the declared seat (takeover, undeclared fallback, TEV_LAYOUT), the windowed tile layout, the never-dropped Resize, the shipped ScrollOffs"
date: 2026-09-05
arc: arc-tapestry
commits: ["062efe18", "cf499fe1"]
touched: [sub-tapestryd, sub-libtapestry, sub-stratum-session, sub-mechanism-drivers, sub-kernel-poll, sub-kernel-loom, seam-tapestry-battery-unowned]
established: [adt-kt1-r1, adt-kt1-r2, haz-budget-stored-not-derived]
closed: [fnd-kt1-r1-c1, fnd-kt1-r1-c2, fnd-kt1-r1-c3, fnd-kt1-r1-c4, fnd-kt1-r1-c5, fnd-kt1-r1-c6, fnd-kt1-r1-c7, fnd-kt1-r1-c8, fnd-kt1-r1-c9, fnd-kt1-r1-c10, fnd-kt1-r1-c13, fnd-kt1-r1-a5, fnd-kt1-r1-b8, fnd-kt1-r2-c1, fnd-kt1-r2-c2, fnd-kt1-r2-c3, fnd-kt1-r2-c4, fnd-kt1-r2-b5]
opened: [seam-loom-sqpoll-p3s, seam-console-chrome-on-handoff, seam-login-halcyond-fallback]
mirrors-checked: []
depth: skeletal
no-dossier-change: "sub-kernel-poll + sub-kernel-loom: round 1 changed only a poll.h comment (A-F5); the substrate findings are deferred to seam-loom-sqpoll-p3s with no kernel code change"
created: 2026-09-05
---
Two commits close the batched KT-1 holotype: 062efe18 fixed round 1's 3 P0 + 5 P1 + 8 P2 (the identity capability every session process inherited; the hidden-leaf kill; the transparent `close`; Direct vs inset; the alt-screen diffs; the seam's heap bounds; the declared handoff replacing the any-user trigger; the session surface cap; the resize/title/back-pressure/`--home` fixes), and the round-2 close re-prosecuted those fixes and landed the residue: the seat is the principal's (takeover from a same-principal or idle holder; E_BUSY only for another principal's live tiles; halcyond runs UNDECLARED rather than exiting into the login loop), the declaration clears after the retire loop, `TEV_LAYOUT` reaches the declared conn on every structural pass, the tile render is windowed on a per-block height cache (the O(history) transient is gone), the re-budget evicts at once, the geometry record is never dropped (`DownQueue`), the producer ships each capped ScrollOff as it forms, the shrink's rows precede the full diff, and the poll set stops at the kernel ceiling. [[sub-tapestryd]] gains the declared-seat section; the unowned halcyond/kaua-term prose lands in `docs/reference/150-halcyond.md` + `152-kaua-term.md`. Round 3 (focused on the windowed layout + the feed sink) follows; see [[adt-kt1-r2]].
