---
id: chg-2026-09-06-weft-doc-absorb
type: chg
title: "absorb docs/reference/125-weft (I-37 capability network dataplane): zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/125-weft.md -> ABSORBED (I-37 audit-trigger surface)

Absorbed the 1217-line Weft reference doc into a multi-redirect stub. The owning
dossier `sub-kernel-weft` (404 lines, guarded-by inv-i37/i30/i9/i32, updated
2026-08-24) is deep and current -- verified atom-by-atom, it covers the whole
kernel mechanism: the three-call Share/Map/Unshare contract (the remote-memory-key
shape, identifier never reaches the client), the four minted-not-asserted kinds
INCLUDING the HOSTMEM Venus ring and the V-2/V-3b-1c-2b-F1 lockstep half-widen
bug, the-pin-is-the-lifetime, the private-view I-30 geometry, the drain snapshot
(extent summed wide), the I-9 readiness poke (the store-buffer register-then-
observe across a Proc boundary), the F_NOTIF three-holder zero-copy-send
completion, the orphan reaper, the I-32 sharer-settles-on-shared-out leak finding,
and the kind-gate-is-the-single-chokepoint for all three data-drive consumers
(weft_binding_validate_rw's role, covered by meaning at Prosecution).

Zero fold. The doc's other regions are owned by the right dossiers, verified:

- the synchronous Tweftio data-drive (SYS_WRITE/READ kick + dev9p_weft_try_write/
  read + the RX no-copy-out) -> sub-kernel-ninep-dev9p; the Tweftio wire op ->
  abi-ninep-wire + sub-kernel-ninep-wire. Code-grounded: weft_binding_validate_rw
  is in weft.c:501 (the validator, covered by sub-kernel-weft's kind-gate
  chokepoint), dev9p_weft_try_write/read in dev9p.c (owned by ninep-dev9p).
- the netd h_weft/h_weftio + the RX PendingWeftRead defer -> sub-netd-server /
  sub-netd-nic.
- the Loom data drive (LOOM_OP_READ/WRITE -> Tweftio) -> sub-kernel-loom.
- the weave share G-2 (SYS_DMA_CREATE_WEAVE, burrow_share_into) -> sub-kernel-burrow
  + sub-tapestryd + sub-libtapestry.
- the shared-in budget (I-32 fifth axis) -> sub-kernel-addrspace + sub-kernel-proc.

Flagged: the opening prose self-describes as an earlier sub-chunk (registry, claim
path, framebuffer kind, reaper are all already in weft.c -- a drift the dossier's
Caveats record); the "10x slower" perf claim was wrong (dossier carries the
corrected dead-heat number).

89 -> 90 absorbed of 157. lint 0-fail.
