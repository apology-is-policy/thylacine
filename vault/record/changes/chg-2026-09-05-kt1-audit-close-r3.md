---
id: chg-2026-09-05-kt1-audit-close-r3
type: chg
title: "KT-1 audit close, round 3: the restructures re-read -- the held-cells sink, the constant open-block cap, the seat held while it hosts, TEV_LAYOUT coalesced"
date: 2026-09-05
arc: arc-tapestry
commits: ["65bb05b3"]
touched: [sub-tapestryd, sub-libtapestry]
established: [adt-kt1-r3]
closed: [fnd-kt1-r3-c2, fnd-kt1-r3-c6]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-05
---
Round 3 (one prosecutor on the round-2 restructures, 0/0/1/6, clean; [[adt-kt1-r3]]) lands the residue of [[chg-2026-09-05-kt1-audit-close]]: the feed sink bounds every screen-sized record class (the alt-screen full diffs piled up as the ScrollOffs had -- 512 screens per 4 KiB read, measured), a constant open-block cap (`OPEN_BLOCK_MAX_COST`) bounds the render transient and the re-budget residue, the resize drain ships no stale-geometry diff, `TEV_LAYOUT` coalesces like CONFIGURE/FOCUS, and the seat is held only while it hosts (no same-principal steal of a live compositor; halcyond re-declares after its first surface). Self-found in parallel: the height cache is keyed on (id, exit, height) -- a floating exit mark is the one post-freeze mutation of a frozen block -- and the poll-set ceiling is a bounded wait. The unowned halcyond/kaua-term findings live in the adt body; the seat rule's text lives in [[sub-tapestryd]] (the declared-seat section) and HALCYON.md 14.12.
