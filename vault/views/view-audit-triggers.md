---
id: view-audit-triggers
type: view
title: "Audit-trigger surfaces"
query: audit-triggers
---
# Audit-trigger surfaces

Generated from note fields — do not edit between the markers
(`quaestor render`). Replaces: the CLAUDE.md trigger table + the ARCH section-25.4 twin.

<!-- generated:begin -->
| surface | code | invariants | prosecution |
|---|---|---|---|
| [[sub-kernel-death]] | kernel/proc.c | inv-i24, inv-i9 | The #811 audit's **verified-sound set** is the do-not-re-prosecute preamble |
| [[sub-kernel-devsrv]] | kernel/devsrv.c, kernel/include/thylacine/devsrv.h | inv-i1 | What an auditor attacks here: |
| [[sub-kernel-larder]] | kernel/larder.c, kernel/include/thylacine/larder.h | inv-i38 | - **The gen-ring event-logging completeness**: every NEW mutation path |
| [[sub-kernel-mm-phys]] | mm/phys.c, mm/phys.h, mm/buddy.c, mm/buddy.h, mm/magazines.c, mm/magazines.h, kernel/include/thylacine/page.h |  | - Any new caller of `pa_to_kva` on an allocator-returned PA is bound |
| [[sub-kernel-mm-slub]] | mm/slub.c, mm/slub.h |  | - The destroy guard must stay `alloc_count - free_count` — reverting |
| [[sub-kernel-ninep-attach]] | kernel/9p_attach.c, kernel/include/thylacine/9p_attach.h |  | - **The failure-path ledger**: every exit must leave (adapter ref × |
| [[sub-kernel-ninep-client]] | kernel/9p_client.c, kernel/9p_session.c, kernel/9p_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_mq.c, kernel/9p_attach.c, kernel/include/thylacine/9p_client.h | inv-i9, inv-i10, inv-i11 | What an auditor attacks here (the single home of the trigger-row content for |
| [[sub-kernel-ninep-dev9p]] | kernel/dev9p.c, kernel/include/thylacine/dev9p.h | inv-i38 | - **The coherence pairing**: every mutation path must carry its exact |
| [[sub-kernel-ninep-dev9p-poll]] | kernel/dev9p_poll.c | inv-i9 | - **The I-9 window**: any reordering of register-hook / ensure-probe / |
| [[sub-kernel-ninep-session]] | kernel/9p_session.c, kernel/include/thylacine/9p_session.h | inv-i10, inv-i11 | - **The retirement matrix**: any new path that clears an `awaiting_flush` |
| [[sub-kernel-ninep-transport]] | kernel/9p_transport.c, kernel/9p_spoor_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_loopback.c, kernel/9p_transport_mq.c, kernel/include/thylacine/9p_transport.h |  | - **The EAGAIN classification boundary**: EAGAIN accepted anywhere past |
| [[sub-kernel-ninep-wire]] | kernel/9p_wire.c, kernel/include/thylacine/9p_wire.h |  | What an auditor attacks here (changes to this surface ride the |
| [[sub-kernel-path]] | kernel/path.c, kernel/include/thylacine/path.h | inv-i33 | - The refcount balances on EVERY create/destroy/replace path (the #66a |
| [[sub-kernel-pipe]] | kernel/pipe.c, kernel/include/thylacine/pipe.h | inv-i9 | - Every mutation that can enable a waiter must keep its wake — the |
| [[sub-kernel-poll]] | kernel/poll.c, kernel/include/thylacine/poll.h | inv-i9 | - The sweep's three phases must keep their order: unregister → |
| [[sub-kernel-proc]] | kernel/proc.c, kernel/include/thylacine/proc.h | inv-i1, inv-i32, inv-i33 | - The `rfork` ledger: every field is inherited, freshened or stripped |
| [[sub-kernel-rendez]] | kernel/sched.c, kernel/include/thylacine/rendez.h | inv-i9, inv-i8 | - **The unconditional `r->lock` acquire in `wakeup` is LOAD-BEARING** |
| [[sub-kernel-sched]] | kernel/sched.c, kernel/include/thylacine/sched.h | inv-i8, inv-i17, inv-i21 | - **The mask-before-read rule holds at every per-CPU read.** Any new site |
| [[sub-kernel-sched-smp]] | kernel/sched.c, kernel/smp.c, arch/arm64/context.S | inv-i21, inv-i18, inv-i8, inv-i9 | - **The claim happens under the victim's lock.** Moving `on_cpu = true` |
| [[sub-kernel-srvconn]] | kernel/srvconn.c, kernel/include/thylacine/srvconn.h | inv-i9 | What an auditor attacks here (the CLAUDE.md CF-3 B row absorbed): |
| [[sub-kernel-stalk]] | kernel/stalk.c, kernel/include/thylacine/stalk.h | inv-i28, inv-i33 | Standing obligations for any change (the ARCH §25.4 POUNCE row is the |
| [[sub-kernel-territory]] | kernel/territory.c, kernel/include/thylacine/territory.h | inv-i1, inv-i3, inv-i33 | On any change to this file, prosecute: |
| [[sub-kernel-thread]] | kernel/thread.c, kernel/include/thylacine/thread.h |  | - **#788 is the shape to keep in mind.** `thread_free` freeing a |
| [[sub-kernel-torpor]] | kernel/torpor.c, kernel/include/thylacine/torpor.h | inv-i9, inv-i24 | - The lock-free mismatch return must never be extended to the EQUAL |
| [[sub-netd-nic]] | usr/netd/src/main.rs, usr/netd/Cargo.toml |  | On any change, prosecute: |
| [[sub-netd-server]] | usr/netd/src/server.rs, usr/netd/src/ndb.rs, usr/netd/ndb/local | inv-i9 | On any change, prosecute (the standing list, accreted across |
| [[sub-pouch-fs]] | usr/lib/pouch/patches/0009-pouch-openat.patch, usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch, usr/lib/pouch/patches/0019-pouch-stat.patch, usr/lib/pouch/patches/0023-pouch-fopen.patch, usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch, usr/lib/pouch/patches/0027-pouch-remove.patch, usr/lib/pouch/patches/0030-pouch-fopen-append.patch, usr/lib/pouch/patches/0031-pouch-readlink.patch | inv-i28 | - Every successful-open exit must route through `pouch_open_ret`, or |
| [[sub-pouch-net]] | usr/lib/pouch/patches/0005-pouch-poll.patch, usr/lib/pouch/patches/0006-pouch-sockets.patch, usr/lib/pouch/patches/0014-pouch-srv-stubs.patch, usr/lib/pouch/patches/0015-pouch-poll-tag.patch, usr/lib/pouch/patches/0016-pouch-net-sockets.patch, usr/lib/pouch/patches/0017-pouch-net-datacalls.patch, usr/lib/pouch/patches/0018-pouch-net-poll.patch, usr/lib/pouch/patches/0020-pouch-srv-bulk.patch, usr/lib/pouch/patches/0028-pouch-net-nonblock.patch | inv-i1, inv-i28 | - **Every fd-consuming call must be tag-aware.** The completeness of that |
| [[sub-pouch-process]] | usr/lib/pouch/patches/0026-pouch-process.patch, usr/lib/pouch/patches/0025-pouch-env.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch, usr/lib/pouch/patches/0013-pouch-mallocng-diag.patch, usr/lib/pouch/patches/0003-pouch-mman.patch | inv-i24 | - The fd model's contiguity check and the `PSPAWN_MAXSLOT` bounds must |
| [[sub-pouch-seam]] | usr/lib/pouch/patches/0001-pouch-syscall-seam.patch, usr/lib/pouch/patches/0002-pouch-stdio-no-iovec.patch, usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch, tools/build.sh |  | - **The seam-check list must grow with the series.** `build_sysroot` |
| [[sub-pouch-signal]] | usr/lib/pouch/patches/0007-pouch-signals.patch | inv-i24 | - The bootstrap must reach a `SYS_NOTED` on EVERY arm (unknown name |
| [[sub-pouch-thread]] | usr/lib/pouch/patches/0004-pouch-pthread.patch, usr/lib/pouch/patches/0022-pouch-nanosleep.patch | inv-i9 | - The 1-hour clamp must stay ≤ `TORPOR_MAX_TIMEOUT_US`; raising the |
| [[sub-pouch-tty]] | usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0029-pouch-cons-winsize.patch |  | - The `S_ISCHR`-then-flag gate order, and the disjointness of bit 40 |
| [[sub-stratum-bdev]] | stratum: src/block/bdev_thylacine.c |  | - Any new op must take the mutex; B-2 is what makes the single shared |
| [[sub-stratum-boot]] | usr/joey/joey.c, kernel/syscall.c, kernel/9p_srvconn_transport.c, kernel/territory.c | inv-i28 | - A new readiness signal must be emitted **after** the last fallible step. |
| [[sub-stratum-server]] | stratum: src/9p/server.c, stratum: src/cmd/stratumd/serve.c, stratum: src/cmd/stratumd/peer_creds.c, stratum: src/cmd/stratumd/run.c | inv-i1, inv-i28 | - `n_uname` must stay ignored while `SO_PEERCRED` is the channel. Honouring |
| [[sub-stratum-session]] | usr/login/src/main.rs, stratum: src/cmd/stratumd/proxy_9p.c, stratum: src/cmd/stratumd/dataset_pattern.c, stratum: src/cmd/stratumd/corvus_notify.c | inv-i1 | - The `/ctl` attach must outlive the session; the DEK lease is bound to |
<!-- generated:end -->
