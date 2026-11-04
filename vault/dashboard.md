---
id: dashboard
type: view
title: "Dashboard"
query: dashboard
---
# Dashboard

Generated — do not edit between the markers (`quaestor render`).

<!-- generated:begin -->
## Arcs

| arc | status | chunks |
|---|---|---|
| [[arc-arch81]] | active | 1 |
| [[arc-astra-halcyon-followup]] | active | 4 |
| [[arc-boosty]] | active | 5 |
| [[arc-clade]] | active | 7 |
| [[arc-corvus-srv]] | active | 3 |
| [[arc-deep-smp-review]] | active | 4 |
| [[arc-go-build]] | active | 27 |
| [[arc-go-ide]] | active | 2 |
| [[arc-halcyon-interaction]] | active | 11 |
| [[arc-holotype-rw]] | active | 9 |
| [[arc-identity-detour]] | active | 12 |
| [[arc-life-support]] | active | 2 |
| [[arc-net]] | active | 14 |
| [[arc-phase1-foundation]] | complete | 3 |
| [[arc-phase2-lifecycle]] | active | 4 |
| [[arc-phase5-ipc]] | complete | 3 |
| [[arc-phase5-namespace]] | active | 3 |
| [[arc-phase6-pouch]] | complete | 6 |
| [[arc-pouch-boot]] | active | 9 |
| [[arc-pty]] | active | 4 |
| [[arc-revenant]] | complete | 0 |
| [[arc-tapestry]] | active | 3 |
| [[arc-tickless-idle]] | active | 2 |
| [[arc-user-authority]] | active | 2 |
| [[arc-vault]] | active | 33 |
| [[arc-vivarium]] | active | 2 |
| [[arc-weft]] | active | 4 |

## Open seams: 80

- [[seam-220-netd-listener-poll]] (sub-netd-server)
- [[seam-240-lo-redial]] (sub-netd-server)
- [[seam-242-selftest-nonfatal]] (sub-netd-nic)
- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-372-latched-double-xcheck]] (sub-kernel-stalk)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-66c-proc-fd]] (sub-kernel-path, sub-kernel-territory)
- [[seam-70-tcg-watchpoint]] (sub-substrate-machine, sub-substrate-gates)
- [[seam-791-smp1-joey]] (sub-substrate-machine, sub-substrate-gates)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-87-disk-write-proof]] (sub-substrate-interactive, sub-substrate-gates)
- [[seam-932-devsrv-readdir]] (sub-kernel-devsrv)
- [[seam-9p-async-share-poll-reserve]] (sub-kernel-ninep-client)
- [[seam-affinity-mask]] (sub-kernel-sched-smp)
- [[seam-boot-banner-coupdate-list]] (sub-substrate-gates)
- [[seam-buddy-bulk-op]] (sub-kernel-mm-phys)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-console-chrome-on-handoff]] (sub-tapestryd)
- [[seam-cwg-parenthetical-refuted]] (sub-kernel-boot-sequence)
- [[seam-death-cascade-smp-harness]] (sub-kernel-death)
- [[seam-devdev-winsize-statless]] (sub-kernel-devdev, sub-kernel-cons)
- [[seam-dirent-type-is-qid-type]] (sub-kernel-devproc)
- [[seam-dtb-blob-internally-trusted]] (sub-kernel-dtb)
- [[seam-eevdf-math]] (sub-kernel-sched)
- [[seam-exiting-tails-never-sleep]] (sub-kernel-death)
- [[seam-expect-channel-close]] (sub-substrate-interactive)
- [[seam-f-notif-unwired]] (sub-kernel-weft, sub-kernel-loom)
- [[seam-fid-monotonic-reclaim]] (sub-kernel-ninep-client)
- [[seam-fstat-errno-flattened-above-the-leaf]] (sub-kernel-ninep-dev9p)
- [[seam-gic-handler-slot-never-cleared]] (sub-kernel-gic, sub-kernel-irqfwd)
- [[seam-handle-based-dot]] (sub-kernel-territory)
- [[seam-hmp-push]] (sub-kernel-sched-smp)
- [[seam-hwcap-boot-cpu-only]] (sub-kernel-boot-sequence, sub-kernel-alternatives)
- [[seam-kaslr-link-va-unchecked]] (sub-kernel-kaslr, sub-kernel-boot-entry)
- [[seam-kobj-handle-release]] (sub-stratum-bdev)
- [[seam-larder-cacheable-proxy]] (sub-kernel-larder, sub-kernel-ninep-dev9p)
- [[seam-larder-lazy-array-robustness]] (sub-kernel-larder)
- [[seam-larder-loom-bypass]] (sub-kernel-larder)
- [[seam-larder-reused-dir-dentries]] (sub-kernel-larder)
- [[seam-larder-shrinker]] (sub-kernel-larder)
- [[seam-larder-stale-child-attr]] (sub-kernel-larder)
- [[seam-legate-member-sweep-race]] (sub-kernel-proc)
- [[seam-loom-rearm-needs-blocking-enter]] (sub-kernel-loom)
- [[seam-loom-sqpoll-p3s]] (sub-kernel-loom, sub-kernel-poll)
- [[seam-mm-directmap-cap-absolute]] (sub-kernel-mm-phys)
- [[seam-mount-graph-unmodeled]] (sub-kernel-territory)
- [[seam-native-exec-drops-sig-ign]] (sub-kernel-notes, sub-pouch-signal)
- [[seam-netd-host-tests]] (sub-netd-server, sub-netd-nic)
- [[seam-nuname-trust-stamp]] (sub-kernel-syscall-dispatch, sub-kernel-ninep-attach)
- [[seam-poll-heap-waiters]] (sub-kernel-poll)
- [[seam-pouch-dirfd]] (sub-pouch-fs)
- [[seam-pouch-dup2-target]] (sub-pouch-process)
- [[seam-pouch-errno-channel]] (sub-pouch-seam, sub-pouch-fs, sub-pouch-net)
- [[seam-pouch-forkpty]] (sub-pouch-tty)
- [[seam-pouch-process-shared]] (sub-pouch-thread)
- [[seam-pouch-readyfd-aba]] (sub-pouch-net)
- [[seam-pouch-select-fd-bound]] (sub-pouch-net)
- [[seam-pouch-sendmsg]] (sub-pouch-net)
- [[seam-pouch-sigmask-per-thread]] (sub-pouch-signal)
- [[seam-pouch-sock-single-user]] (sub-pouch-net)
- [[seam-proc-find-no-refcount]] (sub-kernel-proc)
- [[seam-proc-name-torn-read]] (sub-kernel-devproc, sub-kernel-devctl, sub-kernel-proc)
- [[seam-proxy-coord-eof]] (sub-stratum-session)
- [[seam-rfnameg-shared-territory]] (sub-kernel-territory)
- [[seam-rfork-flags-unimplemented]] (sub-kernel-proc)
- [[seam-runq-rbtree]] (sub-kernel-sched)
- [[seam-sak-revoke-note]] (sub-kernel-proc)
- [[seam-self-managing-handler-no-restart]] (sub-kernel-notes)
- [[seam-slub-debug-mode]] (sub-kernel-mm-slub)
- [[seam-sparse-mpidr]] (sub-kernel-sched-smp)
- [[seam-srv-9p-connect-unit]] (sub-kernel-devsrv)
- [[seam-stratum-final-eviction-failure]] (sub-stratum-session)
- [[seam-stratum-notify-peercred]] (sub-stratum-session, sub-stratum-server)
- [[seam-tapestry-battery-unowned]] (sub-tapestryd)
- [[seam-tickless-bare-metal]] (sub-kernel-sched-smp)
- [[seam-timerwait-sharding]] (sub-kernel-rendez)
- [[seam-torpor-cross-proc]] (sub-kernel-torpor)
- [[seam-viv-tier2-frame]] (sub-kernel-syscall-abi, sub-kernel-vivarium)
- [[seam-warp-prove-unowned]] (sub-tapestryd)

## Recent changes

- 2026-11-04 [[chg-2026-11-04-c8a-r2]] — Torpor keyed on the space: round 2's fixes, and a model that can see K06
- 2026-11-04 [[chg-2026-11-04-pinned-wake]] — A pinned thread's wake goes to its home CPU, not the waker's
- 2026-11-04 [[chg-2026-11-04-torpor-walks-go]] — The two torpor walks and the bucket-lock die check go
- 2026-11-04 [[chg-2026-11-04-walk-errno-close]] — The walk errno close: rounds 1 and 2, the first boots, 32 sabotages
- 2026-11-03 [[chg-2026-11-03-c8a-r1]] — Torpor keyed on the space: round 1's fixes
- 2026-11-03 [[chg-2026-11-03-c8a-torpor]] — torpor keyed on the address space, a lock per bucket, oldest first, and no wake under the lock
- 2026-11-03 [[chg-2026-11-03-item-11c-close]] — Item 11c close: rounds 1 to 3, the sweep, three leases
- 2026-11-03 [[chg-2026-11-03-item-11c]] — A per-note table for native programs, SA_RESTART in the kernel, and an open a note can interrupt
<!-- generated:end -->
