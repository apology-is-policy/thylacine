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

## Open seams: 90

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
- [[seam-boot-banner-coupdate-list]] (sub-substrate-gates)
- [[seam-buddy-bulk-op]] (sub-kernel-mm-phys)
- [[seam-close-flush-unbounded]] (sub-kernel-death)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-console-chrome-on-handoff]] (sub-tapestryd)
- [[seam-cwg-parenthetical-refuted]] (sub-kernel-boot-sequence)
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
- [[seam-login-halcyond-fallback]] (sub-stratum-session)
- [[seam-loom-rearm-needs-blocking-enter]] (sub-kernel-loom)
- [[seam-loom-sqpoll-p3s]] (sub-kernel-loom, sub-kernel-poll)
- [[seam-mm-directmap-cap-absolute]] (sub-kernel-mm-phys)
- [[seam-mount-graph-unmodeled]] (sub-kernel-territory)
- [[seam-netd-host-tests]] (sub-netd-server, sub-netd-nic)
- [[seam-poll-heap-waiters]] (sub-kernel-poll)
- [[seam-poll-srv-registry-retain]] (sub-kernel-poll)
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
- [[seam-tapestry-battery-unowned]] (sub-tapestryd)
- [[seam-tickless-bare-metal]] (sub-kernel-sched-smp)
- [[seam-timerwait-sharding]] (sub-kernel-rendez)
- [[seam-torpor-cross-proc]] (sub-kernel-torpor)
- [[seam-torpor-lock-wake-spin]] (sub-kernel-torpor)
- [[seam-torpor-reclaim-uaccess]] (sub-kernel-torpor)
- [[seam-warp-prove-unowned]] (sub-tapestryd)
- [[seam-wb-close-flush-slot]] (sub-kernel-ninep-dev9p)

## Recent changes

- 2026-09-06 [[chg-2026-09-06-builders-config-overlay]] — sub-substrate-builders de-stale: the clade builders must sync build-config.sh + configs/ (the build-configurator arc's silent dependency)
- 2026-09-06 [[chg-2026-09-06-burrow-borrowed]] — sub-kernel-burrow re-verified borrowed: the only post-update change is a comment-only round-3 refinement the dossier's prose already reflects
- 2026-09-06 [[chg-2026-09-06-cons-consctl-verbs]] — sub-kernel-cons brought current: the three consctl surfaces since 2026-08-18 -- the beacon-tier verb, the serialsilent display-routing verb, and the C2-k1b termios projection
- 2026-09-06 [[chg-2026-09-06-coreutils-filters-destale]] — coreutils-filters de-stale: the ps-driven partition recount (51->52, 15->16), the which drift narrowed to the single / entry, realpath's shared path::normalize, and mkdir -p's race-tolerant re-check
- 2026-09-06 [[chg-2026-09-06-devdev-fdclass-console-identity]] — sub-kernel-devdev de-stale: devdev_fd_devclass (H-1a fd-class; /dev/cons -> 'c') and spoor_is_console (viv-C2 unforgeable console identity, not a qid bit)
- 2026-09-06 [[chg-2026-09-06-elf-distro-dynamic]] — kernel-elf de-stale: the DISTRO D-2/D-4 dynamic-binary surface -- ET_DYN/PIE placement + AT_ENTRY (D-2), the shared elf_read_interp for the rewrite-to-ldso route (D-4/#215), and the measured code count (twenty-four, not twenty-two)
- 2026-09-06 [[chg-2026-09-06-fault-distro-hostmem]] — kernel-fault de-stale: the seventh (HOSTMEM) backing arm + the MAIR-index install generalization (Warp-6 V-2), the #190 geometry verify-and-bail (D-3 retires the R-5 F2 premise, and F2's recompute remedy was wrong), and the #194 past-EOF SIGBUS
- 2026-09-06 [[chg-2026-09-06-harc-audit-close-r1]] — H-arc audit close, round 1 (batched: the zoom fix, H-4c, H-4d-1, H-4d-2a/2/3): the composed GPU arm's stale-slot expansion, the draining resize-ack re-offer, the reservation on claim-less creates, the floor at the latch, the menu seat requires hosting; the span ring lazy + packed, the obj-copy cache a map, Normal mode left on AltEnter; ptyhost declares its tier; the pts-slave 't' unit positive
<!-- generated:end -->
