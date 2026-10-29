---
id: chg-2026-10-01-session-service-registries
type: chg
title: "Private session service registries with retained connection budgets"
date: 2026-10-01
arc: arc-astra-halcyon-followup
commits: []
touched: [sub-kernel-devsrv, sub-kernel-srvconn, sub-kernel-proc, sub-kernel-death, sub-kernel-caps, sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-vivarium, sub-kernel-joey, sub-kernel-devctl, sub-corvus, sub-stratum-session, sub-stratum-boot]
established: []
closed: [seam-srv-registry-lifecycle]
opened: [seam-stratum-final-eviction-failure]
mirrors-checked: []
depth: skeletal
---
D7 realizes the operator-ratified contract in `7c723cb58`: login's explicit
factory role creates a private replacement `/srv` with eleven fixed resident
routes and sixteen local posting slots. Routes resolve only trusted boot
services. Proc memberships retain every posted registry through death even if
the namespace changes. A separate lifetime domain charges constructors and
retained transports until final free: 16 local / 48 aggregate session / 64
global connections, at most 16 retained domains. Trusted tombstones are not
recycled; session teardown frees the containing registry instead.

Corvus implements its existing model's per-owner AUTH records. Required Stratum
dependency `61dde3727921e70e2c72fbd3c9e2044a192f4a54` on the isolated
`codex/astra-session-dek` branch proves each new connection, retains the home key
until the last lease leaves, and drains dirty buffers before key removal.
Storage-failure teardown recovery remains [[seam-stratum-final-eviction-failure]].
Neither repository is landed in Main by this checkpoint.

The evidence under `work/oct1-srv-sessions/` covers three distinct concurrent
logins, same-user overlap and surviving home access, twenty distinct login/
logout cycles, and real encrypted npxf post/mount/read/hangup/repost with three
users live. Graphical media, Lantern, full manual logout/relogin and F10 SAK
states pass; screenshots are 1280x800, not fresh 800x720 or Pi qualification.
The initial manual run was interrupted by its outer time bound; the same
pinned image passes with a sufficient bound and a fresh pool.

Actual-source fixtures reject eight admission, four Corvus-session, four DEK
lease and three factory-ref mutants. C/kernel, C/libt and Rust ABI fixtures
agree. Stratum CTest passed 72/73 targets initially; the old singleton lease
expectation was updated to the approved policy and the remaining target passed
independently, with production unchanged. All eight existing Corvus model
negative configurations produce the expected counterexample; the earlier
bounded clean-model run remains incomplete, not PASS.

Final CPU1 build/boot passes 1830/1830 with route trust and factory delegation
refusal assertions. Native observer, readiness and service-wire pass with zero
exits on the matched final image. A harness cleanup initially signalled QEMU
during its exiting state; bounded reap observation corrects that bookkeeping,
with actual exit receipts and no surviving owned VM.


Measured console demand in `haul-measured-1790862575463068000` (exit zero,
42.18s): three live users consume 4+4+4 session connections before mount,
12/48 aggregate and 22/64 global, with 3/16 retained domains. After a completed
read while Haul remains mounted, the active session uses 5/16, the other two
4 each, aggregate 13/48 and global 23/64 (boot 10). Remaining margins at that
snapshot are 11 active-session, 35 session-aggregate, 41 global connections,
and 13 domains. These are measured snapshots, not peak-workload guarantees.

Review is single-agent per operator direction. The October 1-2 50-boot, SMP,
ASan and UBSan waiver is explicit; no fresh result for those gates is claimed.
The four authority/settings drafts remain separate. Clipboard activation and
unfinished stop branches are outside this change.
