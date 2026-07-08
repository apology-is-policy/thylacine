# CONCURRENT-FS — the concurrent 9P filesystem arc (design)

> **STATUS: RATIFIED (user, 2026-07-05) — binding scripture for the
> concurrent-FS arc.** The §8 votes are recorded below; deviations update
> this doc first (CLAUDE.md design-first policy).
> Charter: user-directed 2026-06-25 — "right after the Go arc, we must
> schedule the concurrent FS in the Thylacine roadmap. Together with the
> Stratum lockless this should substantially improve the build speed."
> Design pass: 2026-07-05, after the go-build mission's exit bar check;
> ratified same day.

---

## 1. The problem, quantified

A single user's concurrent FS workload (`make -j`, parallel `go build`) is
serialized at the 9P server, so parallelism above the FS buys ~nothing:

- **The sharpest witness (2026-07-05 bar check):** the gofmt 91-pkg build
  runs its 33 compile actions at default parallelism (`-p 4`), yet takes
  ~560 ms/action ≈ the whole 18.5 s cold-warm delta — i.e. `-p 4` executes
  at serial wall-clock, because every action's FS ops funnel through one
  serial server loop. Host does ~15 ms/action.
- **The warm floor:** 3.3 s for a fully-warm 91-pkg build (~30x host) is
  dominated by the per-invocation cache-check stat/read storm — pure reads,
  wait-free in the Stratum core, serialized only by the server loop.
- **Area D ground truth (2026-06-25):** `bench_concurrent_write` scales
  FLAT ~57 MB/s from 1→8 writer threads (the core's write path serializes
  on global structures); single-thread small-record writes are
  per-extent-overhead-bound (~52 MB/s at 64 KiB vs ~200 MB/s at 4 MiB).
- **Cold-read profile:** a cold go build reads ~594 MiB at ~27 MiB/s
  effective; per-op FS read latency ~1.26 ms.

## 2. Ground truth — the serialization stack, layer by layer

Each layer below is a fact with a source; the arc must address them in
soundness-first order.

| # | Layer | Fact | Source |
|---|---|---|---|
| S1 | Kernel 9P client | Already CONCURRENT: multi-in-flight tag demux, elected reader, 64-tag ceiling per session (`P9_SESSION_MAX_OUTSTANDING`), death-interruptible, EAGAIN-parking senders | `kernel/9p_client.c` (#841/#845/#349); `kernel/include/thylacine/9p_session.h:94` |
| S2 | Transport (boot FS mount) | SrvConn byte rings; `SRVCONN_MSIZE = 32 KiB` → the negotiated msize ≈ 32 KiB caps every op's payload (a 256 KiB REVENANT cluster read = 8 serial round trips); ring = 2 frames each way; backpressure sound (#348/#349) | `kernel/include/thylacine/srvconn.h:74,91`; `kernel/9p_attach.c:273-275` |
| S3 | stratumd connection dispatch | Thread-per-CONNECTION, strictly SERIAL per connection: read frame → `stm_9p_server_handle` to completion → write reply → next. Thylacine routes every Proc through ONE mount = ONE connection = ONE serial worker | `stratum src/cmd/stratumd/serve.c:356-470` (SWISS-4g) |
| S4 | stm_9p_server | `stm_9p_server_handle` holds the per-connection `s->lock` across the WHOLE request (fid table + all dispatch under it) | `stratum src/9p/server.c:3612/3740` |
| S5 | Tflush | A no-op reply today — correct ONLY because processing is serial (nothing can be in flight when a Tflush arrives) | `stratum src/9p/server.c:1196-1200` |
| S6 | stm_fs core | PARALLEL-3 capable: pure reads WAIT-FREE (LF-3 EBR), single-inode mutators parallel on disjoint inodes (`fs->global` SH + per-inode pins), compound ops EX — the machinery sits UNUSED at v1.0 | `stratum docs/reference/29-concurrency.md` §29.1/§29.4 |
| S7 | R171 UAF family | The wait-free read path's P0 UAF family: **P0-1 (in-place upsert) + P0-4 (memtree free) ENGINE-CLOSED at chunk 9 (9.8-BE-prepend); P0-2 (engine-struct free at rollback/destroy/close) CLOSED at chunk 9b (engine EBR-retire) + both envelope items DONE (`eng->root` slow-warm → mvcc publish + R174-F1 `load_root_locked`; inode `dsstate` rollback reseed → `stm_inode_dsstate_invalidate` at chunk 9b)**. The fs.c write-op port onto `_concurrent` is chunk 10; P0-3 (unmount drain, #1232) stays production-mitigated | `stratum docs/phase-9.8-design.md` §5.1.1; `29-concurrency.md` §29.5 |
| S8 | Core write funnel | Concurrent writers to DISTINCT inodes still serialize on the global `dirty_buffer->mu`, per-dataset `extent_index->lock`, global `alloc->lock` — the flat 1→8 curve | `29-concurrency.md` §29.3/§29.6/§29.8 |
| S9 | Per-extent overhead | Small writes pay a fixed per-extent cost (extent-index insert + Merkle + AEAD setup + alloc): 64 KiB records ~52 MB/s vs 4 MiB ~200 MB/s. The designed fix is the 9.8-BE Bε buffer (N small updates → O(N/B) physical writes) — the SAME work as S7's closure | `29-concurrency.md` §29.6 |
| S10 | Block device | bdev_thylacine invariant B-2: at most ONE virtqueue request in flight; `d->lock` serializes every read/write/fsync — cold-read disk I/O serializes at the driver no matter what the layers above do | `stratum src/block/bdev_thylacine.c:37,219` |
| S11 | Commit path | Fixed ~10 fsync barriers + ~650 KiB metadata re-COW per commit, no clean-commit short-circuit (Area G) | `project_concurrent_fs_arc` memory; `stratum sync.c:2468+` |
| S12 | srvconn server send | #348's F1: the server-side blocking send has a LATENT for a second concurrent writer (single wrendez) — unreached while stratumd writes replies from one thread; must be closed (#354-class) before/with any multi-writer reply path | `memory/bug_348_9p_c2s_backpressure.md` |

## 3. Prior art (the research that dissolves the fork)

**Heritage (Plan 9):** the kernel mount driver (`devmnt`) pipelines RPCs —
exactly our #841 client. On the server side, `exportfs(4)` spawns **slave
procs per request** precisely so one blocked request cannot wedge the
connection — per-request server concurrency IS the Plan 9 idiom. The
single-threaded servers (u9fs, kfs) are the historical counter-examples,
not the model.

**Modern SOTA:** diod (the npfs/libnpfs lineage) dispatches each request
to a **worker thread pool per server**; QEMU virtfs runs a **coroutine per
request**; Linux knfsd is the classic per-request thread pool; Linux v9fs
(the client) multiplexes many in-flight tags on one connection — which is
why serious 9P servers are concurrent per connection. NFSv4
sessions/slots and SMB3 multichannel solve the same problem one layer
lower (many channels); 9P's tag space already provides the multiplexing,
so the server-side pool is the natural shape.

**The fork (worker-pool-per-connection vs multiple-connections-per-mount)
dissolves:** multiple kernel-side connections would duplicate fid/session
state per connection, force fid→connection affinity (fids are
per-connection), multiply ring memory, and add an audit-bearing kernel
surface — for capacity the single connection does not lack (64 tags ×
concurrent server processing). Worker pool per connection wins on both
heritage and SOTA; multi-connection stays a recorded v1.x seam (it becomes
interesting only if one connection's byte stream itself saturates).

**9P semantics check:** 9P imposes NO cross-tag ordering — clients enforce
their own dependencies by waiting (our sync ops block per-caller; the
kernel already refuses new ops on a fid with in-flight ops via
`any_outstanding_on_fid`). The obligations a concurrent server DOES carry:
(a) Tflush must reply only after the flushed request's reply is sent or
discarded; (b) Tclunk/Tremove must not race an executing op on the same
fid (fid pin/refcount); (c) Tversion resets must quiesce in-flight ops.
This is the diod/libnpfs discipline, and it is exactly what CF-2 builds.

## 4. The design — staged, soundness first

### CF-1 — Stratum: the 9.8-BE write half (the soundness gate + write batching)

Build chunks 7-11 + 9b of `phase-9.8-design.md` (writers CAS-prepend Bε
delta messages; EBR-retire superseded values/trees/engines) plus the two
envelope items from §5.1.1 (atomic/`commit_mu`-covered `eng->root`
slow-warm store; inode `dsstate` invalidation on rollback root-swap).

- **Why first:** the instant CF-2 makes a reader and a writer concurrent on
  one engine, the R171 P0 UAF family (S7) is reachable. The SH-fallback is
  a stopgap, not a closure. Landing BE-prepend first means CF-2 ships onto
  a sound core.
- **Also the write-throughput fix:** the Bε buffer batches the go-build's
  small-object-write storm (S9) — the same chunk closes soundness AND the
  write-amplification regime.
  **[AS MEASURED at chunk 11 (`bench_write_amp` + the engine node-COW
  counters): the write-amp claim was the MODEL's promise, not the
  mechanism's. Apples-to-apples at the same commit cadence the Bε
  format reduces node COW writes 1.0×–2.0× (2.04× max, at
  fsync-per-op; 1.00× at batch cadences — both regimes batch dirty
  state in RAM and write only at commit), not the modeled 20–50×,
  whose math double-counted (per-op-flush baseline vs batch-amortized
  Bε + buffer(52 msgs) < fanout(~150) as built). The S9 small-write
  storm is cheap because of in-RAM commit batching — true in BOTH
  regimes — and CF-1's load-bearing deliverables stand unchanged: the
  R171 P0 closure + wait-free writers (CF-2's prerequisite). Details +
  the ε/flush-strategy seam: stratum `phase-9.8-design.md` §9.2
  as-measured; the §14 ship-criterion disposition is flagged for the
  user's vote.]**
- Spec-first: the 9.8 specs exist (`concurrency.tla`, `concurrency_mvcc.tla`
  — `BuggyImmediateFree` is the executable counterexample; the Bε chunk
  table names its specs). This satisfies the re-enabled spec-first bar for
  the surface.
- Riders (same half, separately committed): #38 (the LF-3 read-vs-chmod
  intermittent — same family), #39/#40 (space amplification /
  write-overcommit) where the Bε/commit work touches them naturally.

### CF-2 — stratumd: per-connection worker pool (the concurrency delivery)

The diod/exportfs model applied to `serve.c` + `server.c`:

- Per connection: ONE reader thread (frame reads stay serial — that is the
  byte stream's nature) demuxes frames into a bounded dispatch queue; N
  workers (default ≈ min(4, ncpu)) execute `stm_9p_server_handle`; replies
  serialize through a per-connection writer mutex (one `write_full` at a
  time — frames are atomic units).
- **Lock-granularity surgery (S4):** split the whole-request `s->lock` into
  (a) parse + fid resolve/pin under `s->lock`, (b) the FS-core call OUTSIDE
  it (the core provides its own PARALLEL-3 discipline), (c) reply build +
  fid-state update under `s->lock`. Add a per-fid pin so Tclunk/Tremove
  waits for (or refuses during) an executing op on the same fid.
- **Real Tflush (S5):** an in-flight registry per connection; Tflush of a
  queued request drops it + Rflush; of an executing request, waits for
  completion, discards the reply, then Rflush (the 9P contract).
- Per-worker response buffers (the per-connection single `resp` buffer
  becomes per-worker).
- **Config:** pool size knob (`--fs-workers N`), default tuned on-device;
  `N=1` must degrade to byte-identical serial behavior (the fallback +
  bisect lever).
- **What this unlocks immediately:** the go build's dominant READ storm —
  wait-free in the core (S6) — parallelizes across workers; disjoint-inode
  writes overlap up to the S8 funnel.
- **R175-carried obligations (close within CF-2):** (a) the write path's
  SERIAL range scans (inode seed/find_freed on every alloc; extent
  collect/overlap on every RMW; dirent unlink sweep; xattr serial list)
  hold `serial_mu` for the whole walk and suppress the mini-consolidation's
  trylock under a scan-heavy pool — chain-growth/space-amp pressure (the
  #39/#40 family), NOT a UAF (a fold requires `serial_mu` or the fs-level
  EX envelope); close via `_concurrent` scan variants for the alloc-path
  scans or a commit-cadence chain bound (phase-9.8-design.md 7.2.1). (b)
  The write funnels PROPAGATE engine `STM_EBUSY` (seal back-pressure) —
  decide retry-at-dispatch vs propagate-to-client when the pool makes it
  reachable (the READ side already resolves it via the R175-F2 widened
  SH-fallback).

### CF-3 — transport: measurement-gated frame/latency levers (Thylacine side)

After CF-2 lands, re-measure (fsbench multi-thread + the gofmt bench).
Two candidate levers, chosen by what the measurement says — NOT
pre-committed:

- **Bigger frames for the FS service:** raise the FS mount's msize past
  32 KiB (S2) via a per-service SrvConn ring size (ring memory is
  4×msize per connection kernel-side, so a global bump is the wrong
  shape; a post-time/service-scoped size keeps the default small). Cuts a
  256 KiB cluster read from 8 round trips to 1.
- **Client-side chunked-read fan-out:** `p9_client_read`'s chunk loop
  issues its msize-chunks as concurrent tags instead of serially (the
  read-side analog of what the 64-tag window already permits).
- Kernel-audit-bearing either way (ring memory accounting is I-32-adjacent;
  the #348/#349 backpressure paths get re-validated; S12/#354-class server
  send multi-writer latent must be closed in the same pass).

**[AS MEASURED at CF-3 open (2026-07-07; the CF3RT throwaway instrument —
dev9p_read size/latency/contiguous-run/path counters + a syscall-level
requested-len histogram, one bench boot on the post-#367 twins): the
binding constraint was NEITHER candidate lever — it sat one layer below
both, at the SYSCALL staging. `SYS_RW_MAX = 4096` (the 4 KiB kernel-stack
bounce, whose copy-out was additionally a per-byte `uaccess_store_u8`
call loop) capped every EL0 read at one 4 KiB Tread, and the Go port
mirrors the constant (`rwMax`, a userspace pre-chunk), so bulk reads
arrive as runs of contiguous 4 KiB RPCs. 4096 B per ~145 µs turnaround ≈
the long-measured ~27 MiB/s effective read ceiling. The negotiated
32 KiB msize was never reached by the storm — S2's "a 256 KiB cluster
read = 8 round trips" held only for kernel-internal reads (`maxgot` =
32757 confirms those already ride the full payload). The windows:
gofmt-COLD = 39,052 Treads / 444 MB delivered, 67% of calls exactly
4096, ≥48% of ops in detected contiguous same-fid runs (the single-slot
run tracker undercounts under `-p 4` interleave), Tread latency sum
35.4 s inside a 21.2 s wall (reads overlap across Procs) with median
< 100 µs and a fat serial-server queueing tail (1,957 calls ≥ 500 µs,
max 200 ms — CF-2/CF-4 territory, not a frame-size problem); gofmt-WARM
(2.79 s) = 4,678 Treads / 29.2 MB, 71% exactly-4096, 79% of bytes in
runs, latency sum 3.23 s ≈ 1.16× the wall — the warm build IS the read
stream. The stage therefore splits:**

- **CF-3 A (LANDED with this note): lift the syscall byte-I/O ceiling.**
  `SYS_RW_MAX` 4096 → 128 KiB via a two-tier bounce (ops ≤ the new
  `SYS_RW_STACK` = 4096 keep the stack scratch — the metadata storm pays
  nothing; above it a transient kmalloc, degrading to the stack tier on
  allocation failure so memory pressure shortens an op, never fails it)
  + bulk `uaccess_copy_out` / `uaccess_copy_in` primitives (word-wise,
  three fixup entries each — head/body/tail; replaces the per-byte
  uaccess call loops in all four byte-I/O handlers) + the port mirror
  lifts (go `rwMax` → 128 KiB with `Readdir` PINNED at the kernel's kept
  4 KiB dirent bound; libt/libthyla-rs doc caps). `SYS_READDIR` /
  `SYS_GETRANDOM` / `SYS_EXPLICIT_BZERO` deliberately KEEP the 4 KiB
  bound (`SYS_RW_STACK`) — no bulk need, and the scrub arm's oversize
  REJECT is ABI. At today's msize a bulk read RPC now carries 32,757 B
  (8× fewer RPCs per byte); the full 128 KiB per-RPC payload arrives
  with CF-3 B. 128 KiB = `STM_9P_MSIZE_DEFAULT`, the server's accepted
  default — the client proposal is the only thing pinning 32 KiB.
  **As-landed numbers (fresh-twin protocol, 2 boots, the lifted-rwMax
  toolchain re-baked):** gofmt cold **19.8 / 20.0 s** (vs the post-#367
  21.1/20.6 pair — the cold window's Treads 39.0k → 18.5k, 2.1× fewer,
  same 444 MB moved, `eq4096` 26.1k → 1.9k), warm **2.73 / 2.77 s**
  (~flat vs 2.77/2.79 — warm is small-op + queue-bound). The syscall
  layer is no longer the binding constraint; the residual cold time is
  per-byte server work + the serial-server queueing tail — CF-3 B's and
  CF-2/CF-4's territory respectively.
  **The lift's first bench EXPOSED a latent the 4 KiB cap had masked:
  the 9P client never clamped a single op's count to the negotiated
  payload — an over-payload Twrite failed the frame build and returned
  EIO (the compiler's bulk object writes all failed → no cache puts →
  the warm build ran cold → the build exited 1). Fixed in the same
  chunk: `p9_client_read`/`p9_client_write` clamp to the negotiated
  msize's payload (Twrite additionally to the client out_buf bound) and
  return SHORT — the protocol's own contract; callers loop. Regression:
  `9p_client.bulk_write_clamps_short`.**
- **CF-3 B (LANDED; B2 user-signed-off 2026-07-08 per §8.3): the
  per-service bulk ring — msize 32 KiB → 128 KiB on the FS mounts.**
  As-built: a `DMSRVBULK` create-perm bit on the /srv service post (the
  `DMSRVBYTE` idiom; bit 24) selects the BULK ring class —
  `SrvService.ring_msize` = `SRVCONN_BULK_MSIZE` (128 KiB) — and every
  connection minted on the service gets HEAP rings of 2× its class
  (the inline 64 KiB `srvconn_chan` array is retired; a default conn
  now carries 2×64 KiB heap rings — which also stops the old ~129 KiB
  struct rounding every conn up to a 256 KiB kmalloc — a bulk conn
  2×256 KiB). `srvconn_attach_dev9p_root` proposes the CONNECTION's
  msize (`srvconn_msize`), so the kernel client negotiates 131072 with
  stratumd (`msize_max` default ≥ 128 KiB) and `p9_client` grows a
  two-tier out_buf (inline 32 KiB default / heap msize-sized bulk,
  OOM-degrading to inline). Delivery to stratumd is POSIX-shaped: the
  pouch AF_UNIX layer (patch 0020) maps a pre-bind
  `setsockopt(SO_SNDBUF/SO_RCVBUF ≥ 128 KiB)` to DMSRVBULK, and
  `stm_stratumd_listen_unix` gains a `sockbuf` param the two FS
  listeners pass (`STM_STRATUMD_FS_SOCKBUF` = 2× msize; the /ctl
  listener stays default-class) — so BOTH the system mount and the
  per-user home proxies negotiate 128 KiB, and the proxy's upstream
  dial rides bulk rings automatically (a 128 KiB Tmsg forwarded
  through its conn Spoor fits whole). The default class is
  byte-identical for every other service (corvus, /srv/net); the
  user-drivable exposure stays the two-point class policy ×
  `SRV_MAX_CONNS` (≤ ~32 MiB pathological, documented). One landed
  latent: `srvconn_client_send_frame`'s free-space bound still read
  the old compile-time cap, so the FIRST bulk Twrite frame "never fit"
  and `client_send_flow` EAGAIN-spun — a whole-boot wedge at fsbench;
  fixed + pinned by the in-test big-frame regression
  (`srvconn.bulk_ring_class`). The joey `cf3-bulk` probe now
  round-trips 160 KiB asserting the EXACT clamp counts (first write
  131049 / first read 131061) — the boot-fatal end-to-end proof of the
  128 KiB negotiation.
- **#354 (closed in the same srvconn surgery): the blocking-role park.**
  The `reading`/`writing` single-role guards no longer refuse (-1) a
  2nd concurrent blocking party — a contender PARKS on a per-chan
  `role_waiters` list (the #349 stack-Rendez-per-waiter pattern) until
  the holder releases; rendez/wrendez stay single-waiter (the audited
  #348/#349 machinery untouched), writes stay call-atomic (the role is
  held across the whole delivery). This retires the cross-project
  pre-condition that stratumd's own `write_mu` is the only thing
  keeping a threaded server's concurrent replies from EPIPE-closing
  the mount (the #348-audit F1 latent, live-class since CF-2 made
  stratumd threaded). The third producer of the family also goes
  blocking: `srvconn_client_send_blocking` (the byte-mode CLIENT write
  — the per-user proxy's upstream `write_full` treats a 0 as EPIPE),
  with drain-wakes added to the server-side recv paths. Regressions:
  `srvconn.role_park_second_writer` / `role_park_second_reader` /
  `client_send_blocking_backpressure`.
- **CF-3 C (fan-out): HOLD** — re-measure the post-A residual first; the
  measured latency tail is server-side queueing, which fan-out cannot
  help while the pool defaults to workers=1.]

### CF-4 — Stratum: commit-path cost (Area G's designed follow-ups)

Scripture-first on the Stratum side (both are crash-consistency-critical):
barrier batching (~10 fsyncs → 2: one data barrier + one UB barrier) and
the clean-commit short-circuit (a no-dirty commit near-free; must account
for the in-commit CAS-GC + deferred-free sweeps). S11. Sequenced last
because CF-1's Bε buffer changes what a commit contains.

- **Chunk-11 datum for this stage:** the clone-arm commit (every
  production commit since chunk 10's latch) costs CPU proportional to
  the RESIDENT tree — measured 24 ms vs 1.6 ms serial per commit on a
  500K-key fully-resident tree (shadow consolidate + flush walk +
  whole-tree EBR retire per cycle; `bench_write_amp` B/C=1). Negligible
  at boot-scale trees (the R175 gate showed no regression) and hidden
  behind a real device fsync, but a per-commit O(resident) term the
  clean-commit short-circuit should also account for.

**[AS MEASURED at CF-4 open (2026-07-08; the CF4RT throwaway instrument —
a per-op-type handler-time table in the stratumd serial loop + flush/
commit phase lines + bdev-read / AEAD-decrypt counters; enabled by the
#370 stderr drainer + the pouch clock_gettime seam fix, both keepers):
the go-build queueing-tail hypothesis was WRONG for the build window —
the CF3RT lesson repeating one layer down. Ground truth on the fresh
goroot twins:**

- **Commits do NOT fire mid-build** (Tfsync n=2 in the whole gofmt
  window). The whole build's writes accumulate in the dirty buffer
  (27.5 MB at the post-bench commit; the 256 MiB global cap is never
  approached). The CF3RT "max 200 ms" tail = the Tread max (234 ms) +
  the rare Tfsync (159 ms); 1,294 of the ~1,957 >=500 us calls are
  slow TREADS themselves.
- **The cold window IS the AEAD software-decrypt bill**: Tread handler
  sum 22.5 s inside a 20.7 s wall, decomposing to 20.0 s inside
  `stm_aead_decrypt` (5,918 calls, 860 MB at ~43 MB/s soft AEGIS-256)
  + 1.4 s of device reads + ~1.1 s of everything else. 860 MB decrypted
  vs ~444 MB delivered = whole-covering-extent decrypt amplification x
  the 16-entry #343 dcache. Root cause of the rate: the pouch libsodium
  compiled ONLY `aegis256_soft.c` and Thylacine's auxv carried no
  AT_HWCAP, so `sodium_runtime_has_armcrypto()` could never see the
  M2's idle hardware AES.
- **The commit path is still real debt where commits DO fire**: every
  commit = 10 fsyncs; an EMPTY commit costs ~23-33 ms (the fsbench
  fsync 39-files/s ceiling IS the commit cost); the post-build commit
  drained 27.5 MB in 686 ms. The barrier-batching + clean-commit design
  facts are pinned (the bootstrap dual-slot/self-csum fallback [R7a
  P2-1] makes every intermediate fsync deferrable to ONE pre-final-UB
  data barrier; the mid-commit error path keeps fail-before-final-UB);
  the build lands when this stage's turn comes — it is a
  fsync-workload lever, NOT a go-build lever.
- **Workers re-test (the CF-2f follow-up)**: `--fs-workers 4` on the
  now-ms-scale ops is FLAT (cold 20723 vs 20706 ms) — the 4-vCPU guest
  is CPU-saturated by the build + the server's decrypt; parallelizing a
  CPU-bound server re-slices the same cores. Cut the work, not the
  queue. The workers=1 default stands.
- **Found + tracked**: #374 (Tunlinkat 14.6 ms/call x 222 = 3.2 s per
  build — the serial dirent-unlink sweep on the $WORK cleanup, the
  R175-carried scan family); the #343 dcache sizing (the 2x decrypt
  amplification) is the follow-up lever after hardware AES lands.

**The stage therefore SPLITS: CF-4 A (LANDED with this note) = the AEAD
hardware lever** — the kernel publishes a Linux-compatible `AT_HWCAP`
word in the exec auxv (`g_hw_features.linux_hwcap` from ID_AA64ISAR0/
PFR0; hwcap_CPUID deliberately never set — Thylacine does not
trap-and-emulate EL0 MIDR reads), the pouch libsodium compiles the
self-arming armcrypto TUs runtime-gated on that word (fail-safe soft
fallback on crypto-less cores — RPi4's A72), and the Go fork wires
`internal/cpu` hwcap init (hardware SHA-256 for the toolchain's cache
hashing). **CF-4 B (the commit path as designed above) follows.**

**[CF-4 B LANDED (2026-07-08; Stratum-side, kernel byte-unchanged;
design = stratum `docs/cf-4-design.md`, as-built = stratum
`docs/reference/31-durability-commit.md`).]** The three pieces as
pinned: (1) the bdev **barrier-defer window** — a dirty commit issues
exactly TWO real fsyncs (one pre-final-UB data barrier covering the
reservation UB + the single staged bootstrap COW + every phase-2 node
write, then the final UB's own fsync); (2) the **single staged
bootstrap COW** per commit (the N per-component `stm_bootstrap_commit`
calls record into a defer window; stage-without-promote keeps
failed-commit retries sound [the fsyncgate axis], and single-COW is
load-bearing — two un-barriered COWs would ping-pong onto both
dual-slot pairs and a crash could tear both headers); (3) the
**clean-commit short-circuit** — a content-identical commit (masked
prototype compare vs the last durably-written UB) is a true no-op:
zero writes, zero fsyncs, gens unchanged (gen-age = real-commit count;
the CAS `min_age_txgs` policy ages with activity). Plus the **#369
rider** (R176 F1): the 8 create/symlink rollback `(void)
stm_inode_free` sites now check + surface a rollback-free failure
loudly (the orphan-record scrub kind stays a named seam). Measured:
host bench_commit empty 19,756 → **13.6 us** / 6 → **0 fsyncs**;
data-4k 51,324 → 27,732 us / 10 → **2 fsyncs** / write-amp 163× →
118×; **in-guest fsbench fsync 39 → 340 files/s** (2,933 us/durable-
file). The crash sweeps (test_durability + test_crash_inject) pass
unchanged over the new ordering; + 8 new tests (the 2-fsync witness,
the clean-skip contract, the inject-fail + in-process-retry sweep, the
defer single-COW lifecycle, torn staged header/bitmap R7a fallbacks).
Named seams: the per-engine O(resident) clean-flush cycle (CPU-only,
~24 ms at 500K resident keys — needs a provably-sound mutation-counter
choke point; cf-4-design.md §6.1) and the metadata re-COW density
(Area-S S-2). The 39-files/s fsync ceiling was the last pinned CF-4
item — **the CF-4 stage is COMPLETE**.

**[CF-4 C LANDED (2026-07-08; Stratum-side, kernel byte-unchanged;
design = stratum `docs/cf-4c-design.md`, as-built = stratum
`docs/reference/31-durability-commit.md` §31.2.2).]** The #374 fix, the
third commit-path lever (CF-4 B batched the fsyncs *within* a commit;
CF-4 C batches the *commits themselves* for reclaim). The measurement
refuted #374's "dirent sweep" premise: an INLINE-file unlink is 8.84 us
(the tombstone the task blamed), but an EXTENT-file unlink was 47,000 us
with gen_delta = 4 per unlink = **two full commits** -- the SWISS-4q
eager reclaim double-commit handing freed blocks back immediately. The
go-build `$WORK` cleanup's 222 extent unlinks = **444 commits = 3.2 s**.
Fix (user-approved Option A): (1) remove the eager double-commit from the
3 inode-free paths -- freed blocks stay PENDING (crash-safe: #791 mount
reconcile rebuilds `pending_head`) and return to FREE lazily at the next
commit's sweep, aligning unlink with truncate-shrink + COW-overwrite
which already deferred; (2) a reserve-path **reclaim-on-ENOSPC** backstop
(`fs_reclaim_on_enospc_locked`: on `STM_ENOSPC` with pending > 0, sweep
via the double-commit + retry once; loop-free + wedge-on-failure) at
every data-reserve choke (the buffer drain, direct writes, inline->extent
transitions, migrate/promote) -- which ALSO closes a latent gap (truncate/
overwrite-then-rewrite near-full-no-commit had no backstop). Deliberate +
POSIX-correct: unlink is no longer durable-without-fsync / gen-advancing,
consistent with every other lazy-durable mutation. The direction is the
COW-FS norm (Btrfs pinned-extents + delayed-refs + `flush_space`->
`COMMIT_TRANS`; ZFS `ms_defer` + `async_destroy`; WAFL CP deferral) --
the eager per-unlink double-commit was the outlier. Measured (host
bench_create_many CHURN, 222 x 8 KiB extent files): extent unlink
**47,000 -> ~15 us/file**, gen_delta **888 -> 0** (the blocks reclaim in
one sweep at the next commit). Host suite 70/70 incl. test_durability +
test_crash_inject unchanged over the new ordering; +5 tests (deferral
proof, the availability guarantee [non-vacuous], the closed truncate gap,
the flush-internal reclaim path, loop-freedom). **Focused audit
(holotype-reviewer, Opus-4.8-max -- Fable quota out; MODEL start==end):
0 P0 / 0 P1 / 0 P2 / 4 P3, NOT dirty** -- the prosecutor cross-confirmed
the whole self-audit sound-set and WITHDREW the commit-under-SH-wedge
concern with a stronger grounding (metadata + data draw from DISJOINT
pools -- engine/alloc-COW nodes from the bootstrap bitmap, only file data
from the data alloc tree -- so deferred DATA pending can never starve a
metadata commit's engine flush). F1 (wrap the test-only stm_fs_reserve) /
F3 (the flush-path test) / F4 (3 stale comments) FIXED; F2 (fsync-ENOSPC
partial-durability) doc'd. Guest boot-OK confirmed (new stratumd):
1047/1047 kernel suite, boot OK, login E2E, DEK-home provision, 0
EXTINCTION -- kernel byte-unchanged, so no SMP gate owed. **In-guest
re-measure (2026-07-08; a confound-free A/B -- ONE freshly-baked goroot
pool, same toolchain + kernel + joey vehicle, only the 5 stratumd source
files reverted between CF-4 B eager-reclaim and CF-4 C deferred): the
91-pkg `cmd/gofmt` cold build drops 4118 -> 3366 ms (2 boots each; CF-4 B
4120/4116, CF-4 C 3287/3444) = ~750 ms / ~18%; warm flat (1273/1424 vs
1248/1271 -- few extent $WORK files at warm).** The saving is smaller than
the pre-CF-4-A 3.2 s attribution because CF-4 A's hardware AEAD already
collapsed the per-commit cost the eager reclaim was paying 2x of: the ~17
extent $WORK archives (the compiled `_pkg_.a`, ~50 ms/double-commit each)
were the expensive unlinks -- the 222-figure counted ALL unlinks, mostly
cheap inline. So the host mechanism (47,000 -> 15 us/unlink) IS confirmed
to translate to a real-workload wall-clock win, and the ground-truth
number corrects the attribution. The `-work` control (a $WORK-preserving
build) was made moot by the direct stratumd A/B.

### CF-5 — measure, gate, audit, close

- fsbench: multi-threaded through ONE mount (the new capability) — expect
  reads to scale toward min(64-tag window, workers); writes to move with
  CF-1's batching; name what does NOT scale (S8 funnel, S10 bdev B-2).
- The gofmt bench (the mission register's recipe; pool-twin restore for
  cold) + the go-build oracle re-run.
- Focused audits: one per half (Stratum CF-1+CF-2; Thylacine CF-3 if it
  lands kernel bytes) per the audit-trigger discipline; the SMP gate on
  any kernel-touching chunk.
- **Named seams (recorded, not built):** bdev multi-in-flight (S10 — lift
  B-2 into a queue-depth window with per-request completion), the S8
  dirty-buffer/alloc lock granularity (re-measure after CF-1; the Bε
  buffer may absorb most of it), kernel multi-connection-per-mount, and
  stratumd worker-pool NUMA/affinity tuning.

## 5. What the arc does NOT do

- No 9P wire/ABI change (the tag space already multiplexes; msize is a
  negotiated parameter, not a format break).
- **CORRECTION (2026-07-05, ground-truthed at CF-1 open; supersedes this
  doc's ratified draft, which wrongly claimed "no on-disk format
  change"):** CF-1's chunk 7 (9.8-BE-format) DOES carry an on-disk
  change — the internal-node buffer region makes the since-9.7-reserved
  `n_buffer_used` field load-bearing, with **STM_UB_VERSION 32 → 33** at
  the first non-zero buffer write. The compat posture is designed
  (`phase-9.8-design.md` line ~1152): upgrade-on-mount (a v33 binary
  reads v32 pools — every internal node's buffer region is zero), no
  downgrade (a v33-written pool mis-parses under v32), pre-release, no
  converter; the field has zero deployed pools and Thylacine's pool.img
  is a rebuildable build fixture (a PRESERVE'd pre-bump pool re-bakes
  once). Per standing policy this bump was ESCALATED — and the user
  granted a standing approval (2026-07-05): **"Any format change
  automatically approved in this arc."** So STM_UB_VERSION 32 → 33 (and
  any further format change CF-1..CF-5 needs) proceeds without a
  per-change round-trip; the grant is scoped to THIS arc only.
- No change to the kernel 9P client's public API (it is already
  concurrent).
- No weakening of PARALLEL-3 / commit exclusion: compound ops (commit,
  snapshot, unmount) keep `fs->global` EX against everything.

## 6. Invariant + audit sketch (for the scripture commit)

- **Thylacine ARCH §25.4:** a CF row on the audit-trigger table IF CF-3
  lands kernel bytes (srvconn ring sizing / client chunk fan-out) —
  composes I-32 (ring memory bound), I-9/#348/#349 (backpressure
  wait/wake), I-10/I-11 (tags/fids under higher concurrency). No new §28
  invariant expected on the Thylacine side.
- **Stratum side:** the 9.8-BE chunks carry their own spec gates
  (`concurrency_mvcc.tla` + the Bε specs); the stratumd worker pool adds
  the fid-pin + Tflush + writer-serialization invariants (audit-bearing;
  the R94/R95 serve.c lineage). The `29-concurrency.md` reference gets the
  as-built rewrite.
- **NOVEL.md candidate:** "a network-FS server fully concurrent across the
  9P boundary (per-request worker pool) backed by a lockless (Bε+EBR)
  metadata core — giving a single user's `make -j` the concurrency of a
  kernel FS *through* a 9P mount." Most 9P servers serialize per
  connection; diod parallelizes requests but sits on a conventional
  kernel FS. The fusion is the novel angle. Record at the scripture
  commit.

## 7. Expected wins (honest, from the measurements)

- Warm floor (3.3 s cross-boot / ~4.6 s in-boot): the stat/read storm is
  wait-free + latency-bound → CF-2 divides the serial-latency component by
  up to the effective in-flight count (bounded by 4 vCPUs on-device).
- Cold 21.8 s: ~12-13 s of it is FS ops at ~1.26 ms × serial; CF-2
  overlaps them across the 4-way action parallelism; CF-1 collapses the
  small-write per-extent regime; CF-3 cuts large-read round trips. The
  bar-table target ("< ~10 s cold, then par") is the realistic near goal;
  host-par cold (0.6 s) additionally needs the seam items (bdev
  queue-depth, commit batching) — named, sequenced, measured.
- Nothing here is speculative machinery: every stage removes a measured,
  file:line-pinned serialization.

## 8. The ratified decisions (user vote, 2026-07-05)

1. **Staging RATIFIED**: CF-1 soundness-first → CF-2 delivery → CF-3
   measurement-gated → CF-4 commit path → CF-5 close.
2. **CF-1 scope CONFIRMED**: the 9.8-BE write half (chunks 7-11 + 9b +
   the §5.1.1 envelope items) runs INSIDE this arc; it is the closure
   vehicle for R171 and MUST land before CF-2 ships.
3. **CF-3 ABI flag ACKNOWLEDGED**: if the per-service ring size is the
   chosen lever, the post-time ring hint is a small ABI addition and
   comes back for signoff per standing policy before it lands.
4. **NOVEL.md entry RECORDED** at this scripture commit (the captured
   post-v1.0-candidates list, the Weft/Loom precedent).
