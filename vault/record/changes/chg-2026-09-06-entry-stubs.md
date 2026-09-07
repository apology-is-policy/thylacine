---
id: chg-2026-09-06-entry-stubs
type: chg
title: "entry cluster stubs: close 01-boot's PL011 debt (task #32, now sub-kernel-uart) and stub 31-trivial-devs (multi-redirect dev/content/cons/uart; a heavily-superseded P4-B doc, every live atom already homed) -- 64 absorbed / 93 live"
date: 2026-09-06
arc: arc-vault
commits: ["f010494f"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Two doc closures now that the entry cluster's content is homed (sub-kernel-uart
authored + the revoke-asymmetry/RNDR gaps folded in the two prior commits).

- **01-boot** was already a stub; its one open debt was the PL011 driver having
  no home (task #32 -- it "held the only account of arch/arm64/uart.c"). Now
  RESOLVED: updated the stub to redirect the PL011 content to sub-kernel-uart.
  No absorbed-count change (already absorbed).
- **31-trivial-devs** -> multi-redirect stub: null/zero/full + dev_simple_* +
  bestiary to sub-kernel-dev; random + the RNDR FEAT_RNG/NZCV mechanism to
  sub-kernel-content (folded prior); cons to sub-kernel-cons; the PL011 RX to
  sub-kernel-uart. Every live atom verified homed before stubbing. Its "what it
  got wrong" is substantial: a P4-B snapshot whose architecture has moved --
  devcons.read is no longer degenerate (RX landed A-4c-1), random is no longer
  RNDR-only (ChaCha20 stir landed; RNDR is 1 of 3 seed inputs) and its read path
  is the devdev leaf not the standalone devrandom Dev, and urandom/consctl/full
  have all landed. 63 -> 64 absorbed, 93 live.

No code touched; no audit owed. 109-devdev remains (blocked on the kernel/joey.c
orphan for its kernel-boot-mount atom -- next); the revoke-asymmetry it flagged
is already folded.
