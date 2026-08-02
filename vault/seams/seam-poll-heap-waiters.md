---
id: seam-poll-heap-waiters
type: seam
title: "POLL_MAX_NFDS = 64 is a stack-frame bound — lifting it needs heap-backed waiters"
status: open
surface: [sub-kernel-poll]
opened-by: chg-2026-06-24-355-poll-decouple
tracker: "#355 companion"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`sys_poll_for_proc` stack-allocates `waiters[64]` (~2 KiB) +
`held[64]` (~1.5 KiB). When the fd table grew 64→256 the poll bound
deliberately did NOT follow — at 256 the frame is ~14 KiB against a
16 KiB kstack. A caller polling more than 64 fds gets -1 today.

## The lift

Heap-backed waiter + held arrays (sized to nfds, freed on every
exit path including the INTR arm), or a hybrid (stack up to 64, heap
above). The natural companion of the growable-fd-table chunk. The
sweep-order discipline (unregister → scribble → put → free) gains a
fourth step and every step keeps its order.
