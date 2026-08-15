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
| [[arc-clade]] | active | 7 |
| [[arc-corvus-srv]] | active | 3 |
| [[arc-deep-smp-review]] | active | 4 |
| [[arc-go-build]] | active | 27 |
| [[arc-go-ide]] | active | 2 |
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
| [[arc-vault]] | active | 33 |
| [[arc-vivarium]] | active | 2 |
| [[arc-weft]] | active | 4 |

## Open seams: 84

- [[seam-220-netd-listener-poll]] (sub-netd-server)
- [[seam-221-idle-pump-wake]] (sub-kernel-ninep-dev9p-poll)
- [[seam-223-pump-tail-starvation]] (sub-kernel-ninep-dev9p-poll)
- [[seam-240-lo-redial]] (sub-netd-server)
- [[seam-242-selftest-nonfatal]] (sub-netd-nic)
- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-372-latched-double-xcheck]] (sub-kernel-stalk)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-66c-proc-fd]] (sub-kernel-path, sub-kernel-territory)
- [[seam-70-tcg-watchpoint]] (sub-substrate-machine, sub-substrate-gates)
- [[seam-791-smp1-joey]] (sub-substrate-machine, sub-substrate-gates)
- [[seam-80-pivot-orphan-mounts]] (sub-kernel-territory)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-87-disk-write-proof]] (sub-substrate-interactive, sub-substrate-gates)
- [[seam-90-hung-server]] (sub-kernel-ninep-client)
- [[seam-932-devsrv-readdir]] (sub-kernel-devsrv)
- [[seam-9p-tag-block-on-full]] (sub-kernel-ninep-session)
- [[seam-affinity-mask]] (sub-kernel-sched-smp)
- [[seam-buddy-bulk-op]] (sub-kernel-mm-phys)
- [[seam-close-flush-unbounded]] (sub-kernel-death)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-death-cascade-smp-harness]] (sub-kernel-death)
- [[seam-devcap-plain-caps-read]] (sub-kernel-caps)
- [[seam-devdev-winsize-statless]] (sub-kernel-devdev, sub-kernel-cons)
- [[seam-dtb-blob-internally-trusted]] (sub-kernel-dtb)
- [[seam-eevdf-math]] (sub-kernel-sched)
- [[seam-el0-irq-tail-no-notes]] (sub-kernel-exception)
- [[seam-exiting-tails-never-sleep]] (sub-kernel-death)
- [[seam-expect-channel-close]] (sub-substrate-interactive)
- [[seam-f-notif-unwired]] (sub-kernel-weft, sub-kernel-loom)
- [[seam-fid-monotonic-reclaim]] (sub-kernel-ninep-client)
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
- [[seam-mm-directmap-cap-absolute]] (sub-kernel-mm-phys)
- [[seam-mount-graph-unmodeled]] (sub-kernel-territory)
- [[seam-netd-host-tests]] (sub-netd-server, sub-netd-nic)
- [[seam-poll-heap-waiters]] (sub-kernel-poll)
- [[seam-poll-srv-registry-retain]] (sub-kernel-poll)
- [[seam-posix-pathname-form-gates]] (sub-kernel-stalk)
- [[seam-pouch-dirfd]] (sub-pouch-fs)
- [[seam-pouch-dup2-target]] (sub-pouch-process)
- [[seam-pouch-errno-channel]] (sub-pouch-seam, sub-pouch-fs, sub-pouch-net)
- [[seam-pouch-forkpty]] (sub-pouch-tty)
- [[seam-pouch-guard-pages]] (sub-pouch-thread, sub-pouch-process)
- [[seam-pouch-process-shared]] (sub-pouch-thread)
- [[seam-pouch-readyfd-aba]] (sub-pouch-net)
- [[seam-pouch-select-fd-bound]] (sub-pouch-net)
- [[seam-pouch-sendmsg]] (sub-pouch-net)
- [[seam-pouch-sigmask-per-thread]] (sub-pouch-signal)
- [[seam-pouch-sigtstp-ignore]] (sub-pouch-signal, sub-pouch-tty)
- [[seam-pouch-sock-single-user]] (sub-pouch-net)
- [[seam-pouch-spawn-envp]] (sub-pouch-process)
- [[seam-proc-find-no-refcount]] (sub-kernel-proc)
- [[seam-proc-name-torn-read]] (sub-kernel-devproc, sub-kernel-devctl, sub-kernel-proc)
- [[seam-proxy-coord-eof]] (sub-stratum-session)
- [[seam-rfnameg-shared-territory]] (sub-kernel-territory)
- [[seam-rfork-flags-unimplemented]] (sub-kernel-proc)
- [[seam-runq-rbtree]] (sub-kernel-sched)
- [[seam-sak-revoke-note]] (sub-kernel-proc)
- [[seam-slub-debug-mode]] (sub-kernel-mm-slub)
- [[seam-sparse-mpidr]] (sub-kernel-sched-smp)
- [[seam-srv-9p-connect-unit]] (sub-kernel-devsrv)
- [[seam-srv-registry-lifecycle]] (sub-kernel-devsrv)
- [[seam-stratum-notify-peercred]] (sub-stratum-session, sub-stratum-server)
- [[seam-tickless-bare-metal]] (sub-kernel-sched-smp)
- [[seam-timerwait-sharding]] (sub-kernel-rendez)
- [[seam-torpor-cross-proc]] (sub-kernel-torpor)
- [[seam-torpor-lock-wake-spin]] (sub-kernel-torpor)
- [[seam-torpor-reclaim-uaccess]] (sub-kernel-torpor)
- [[seam-union-mount-walk]] (sub-kernel-territory, sub-kernel-stalk)
- [[seam-wb-close-flush-slot]] (sub-kernel-ninep-dev9p)

## Recent changes

- 2026-08-15 [[chg-2026-08-15-build-targets]] — build.sh re-swept: three target lists that disagree, and the vault recommended the shortest
- 2026-08-15 [[chg-2026-08-15-diorama-vivarium]] — diorama re-swept: a second operating mode the dossier never saw, and a read buffer half its source
- 2026-08-15 [[chg-2026-08-15-exec-lineage]] — exec re-swept after LINEAGE: a seam closed sideways, and a prediction that came true
- 2026-08-15 [[chg-2026-08-15-handle-posix-fds]] — The handle table re-swept: POSIX descriptors arrived, and the dossier's ceiling was wrong when written
- 2026-08-15 [[chg-2026-08-15-interactive-gating]] — LS-CI re-swept: the gate went parallel, and its own default is stated three times
- 2026-08-15 [[chg-2026-08-15-joey-boot]] — joey re-swept after Warp-2: a mount deliberately not made, and the scope of what this dossier owns
- 2026-08-15 [[chg-2026-08-15-libthyla-rs-resolution]] — libthyla-rs re-swept: the kernel resolves now, and the primitive that got it wrong is still public
- 2026-08-15 [[chg-2026-08-15-proc-lineage]] — proc.c re-swept after LINEAGE: the address-space extraction, the second rfork shape, and the vfork park
<!-- generated:end -->
