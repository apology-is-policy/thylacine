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
| [[arc-clade]] | active | 1 |
| [[arc-corvus-srv]] | active | 3 |
| [[arc-deep-smp-review]] | active | 2 |
| [[arc-go-build]] | active | 25 |
| [[arc-go-ide]] | active | 2 |
| [[arc-holotype-rw]] | active | 7 |
| [[arc-identity-detour]] | active | 12 |
| [[arc-life-support]] | active | 1 |
| [[arc-net]] | active | 12 |
| [[arc-phase2-lifecycle]] | active | 4 |
| [[arc-phase5-namespace]] | active | 3 |
| [[arc-pouch-boot]] | active | 1 |
| [[arc-pty]] | active | 2 |
| [[arc-tickless-idle]] | active | 2 |
| [[arc-vault]] | active | 11 |
| [[arc-weft]] | active | 4 |

## Open seams: 46

- [[seam-220-netd-listener-poll]] (sub-netd-server)
- [[seam-221-idle-pump-wake]] (sub-kernel-ninep-dev9p-poll)
- [[seam-223-pump-tail-starvation]] (sub-kernel-ninep-dev9p-poll)
- [[seam-240-lo-redial]] (sub-netd-server)
- [[seam-242-selftest-nonfatal]] (sub-netd-nic)
- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-372-latched-double-xcheck]] (sub-kernel-stalk)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-66c-proc-fd]] (sub-kernel-path, sub-kernel-territory)
- [[seam-80-pivot-orphan-mounts]] (sub-kernel-territory)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-90-hung-server]] (sub-kernel-ninep-client)
- [[seam-932-devsrv-readdir]] (sub-kernel-devsrv)
- [[seam-9p-tag-block-on-full]] (sub-kernel-ninep-session)
- [[seam-affinity-mask]] (sub-kernel-sched-smp)
- [[seam-close-flush-unbounded]] (sub-kernel-death)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-death-cascade-smp-harness]] (sub-kernel-death)
- [[seam-eevdf-math]] (sub-kernel-sched)
- [[seam-exiting-tails-never-sleep]] (sub-kernel-death)
- [[seam-fid-monotonic-reclaim]] (sub-kernel-ninep-client)
- [[seam-handle-based-dot]] (sub-kernel-territory)
- [[seam-hmp-push]] (sub-kernel-sched-smp)
- [[seam-larder-cacheable-proxy]] (sub-kernel-larder, sub-kernel-ninep-dev9p)
- [[seam-larder-lazy-array-robustness]] (sub-kernel-larder)
- [[seam-larder-loom-bypass]] (sub-kernel-larder)
- [[seam-larder-reused-dir-dentries]] (sub-kernel-larder)
- [[seam-larder-shrinker]] (sub-kernel-larder)
- [[seam-larder-stale-child-attr]] (sub-kernel-larder)
- [[seam-legate-member-sweep-race]] (sub-kernel-proc)
- [[seam-mount-graph-unmodeled]] (sub-kernel-territory)
- [[seam-netd-host-tests]] (sub-netd-server, sub-netd-nic)
- [[seam-posix-pathname-form-gates]] (sub-kernel-stalk)
- [[seam-proc-find-no-refcount]] (sub-kernel-proc)
- [[seam-rfnameg-shared-territory]] (sub-kernel-territory)
- [[seam-rfork-flags-unimplemented]] (sub-kernel-proc)
- [[seam-runq-rbtree]] (sub-kernel-sched)
- [[seam-sak-revoke-note]] (sub-kernel-proc)
- [[seam-sparse-mpidr]] (sub-kernel-sched-smp)
- [[seam-srv-9p-connect-unit]] (sub-kernel-devsrv)
- [[seam-srv-registry-lifecycle]] (sub-kernel-devsrv)
- [[seam-tickless-bare-metal]] (sub-kernel-sched-smp)
- [[seam-timerwait-sharding]] (sub-kernel-rendez)
- [[seam-union-mount-walk]] (sub-kernel-territory, sub-kernel-stalk)
- [[seam-wb-close-flush-slot]] (sub-kernel-ninep-dev9p)

## Recent changes

- 2026-08-01 [[chg-2026-07-31-stalk-sweep]] — The stalk sweep: the resolver + the Path substrate + the namespace audit backfill
- 2026-08-01 [[chg-2026-08-01-proc-thread-sweep]] — The proc/thread sweep: the death lineage, and a doc that contradicts itself four lines apart
- 2026-08-01 [[chg-2026-08-01-sched-sweep]] — Vault sweep: the scheduler area (dispatch, the SMP protocol, the wait/wake primitive)
- 2026-08-01 [[chg-2026-08-01-territory-sweep]] — The territory sweep: the namespace tables, the two locks, and a dead bind graph
- 2026-07-31 [[chg-2026-07-31-larder-sweep]] — The Larder sweep: the guest-FS-cache mechanism dossier + the full L1/B1/D44/term audit backfill
- 2026-07-31 [[chg-2026-07-31-netd-sweep]] — The netd sweep: the first userspace area — nic + server dossiers + the net/weft audit backfill
- 2026-07-31 [[chg-2026-07-31-ninep-area-sweep]] — The 9P-area sweep: wire, session, transports, attach, dev9p, dev9p.poll
- 2026-07-31 [[chg-2026-07-31-ninep-pilot]] — The 9P-client pilot: one subsystem end-to-end across all four planes
<!-- generated:end -->
