---
id: chg-2026-06-20-weft6b-netd-drive
type: chg
title: "Weft-6b (netd half): the Tweft ring register + the TX/RX zero-copy Tweftio drive"
date: 2026-06-20
arc: arc-weft
commits: ["e3d4d06c", "55e2401b", "f379c64f"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
weft-6b-1 (`e3d4d06c`): the netd `h_weft` handler + the lazy per-flow
ring register (`weft_ensure`: burrow-attach → geometry mirror →
`SYS_WEFT_SHARE`; one ring per flow, idempotent share_id re-return;
every failure degrades to byte-copy) — grant-is-the-share goes LIVE.
weft-6b-2b (`55e2401b`): the TX drive — `h_weftio(WRITE)` reads the
payload IN PLACE from the shared ring (bounds re-checked against netd's
own mapping — defense in depth, a memory bound not a per-op capability
re-check) into `data_send`. weft-6b-3a (`f379c64f`): the RX twin —
`weft_recv_into_ring` recvs in place into the ring, deferring via
`PendingWeftRead`/`poll_weftio` when empty (a 0 would read as EOF), with
the same four-site cancel matrix. The kernel halves of all three
commits (the Tweft/Tweftio ops, the dev9p fast paths, the binding view)
backfill with the kernel weft sweep.
