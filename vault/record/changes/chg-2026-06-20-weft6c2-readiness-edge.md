---
id: chg-2026-06-20-weft6c2-readiness-edge
type: chg
title: "Weft-6c-2 (netd half): the WEFT_READY_RX readiness edge on real RX delivery"
date: 2026-06-20
arc: arc-weft
commits: ["abda20e1"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The netd sliver of the native WeftFlow push/pop/wait commit:
`weft_recv_into_ring` bumps the single-cache-line readiness seq
(`ready_signal(WEFT_READY_RX)`) on each real recv-into-ring delivery —
the syscall-free busy-poll edge a native client's `rx_ready_seq`
observe sees without a Loom ENTER. netd ignores the wake-needed return
(the client parks on the Loom CQ, not a weft Rendez — the direct-park
leg stays validated-not-wired, v1.x). Single-writer-per-word: netd owns
ready_seq/ready_mask, disjoint from the payload region. The
libthyla-rs `WeftFlow`/`weft.rs` mirror halves backfill with their
sweep.
