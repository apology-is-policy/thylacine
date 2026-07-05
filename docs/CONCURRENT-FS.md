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
| S7 | R171 UAF family | The wait-free read path has a P0 UAF family (in-place `eng_leaf_put` free-vs-reader + engine/tree frees) — UNREACHABLE today (serial worker) but LIVE the instant reads+writes run concurrently on one engine. Stopgap: SH-fallback. True closure: the UNBUILT 9.8-BE write half (chunks 7-11 + 9b) + 2 envelope items (`eng->root` slow-warm plain store; inode `dsstate` rollback reseed) | `stratum docs/phase-9.8-design.md` §5.1.1; `29-concurrency.md` §29.5 |
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

### CF-4 — Stratum: commit-path cost (Area G's designed follow-ups)

Scripture-first on the Stratum side (both are crash-consistency-critical):
barrier batching (~10 fsyncs → 2: one data barrier + one UB barrier) and
the clean-commit short-circuit (a no-dirty commit near-free; must account
for the in-commit CAS-GC + deferred-free sweeps). S11. Sequenced last
because CF-1's Bε buffer changes what a commit contains.

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
  negotiated parameter, not a format break). No on-disk format change
  (the Bε chunks work inside the existing engine's committed format — per
  `phase-9.8-design.md`; if any chunk turns out to need an incompat flag,
  that is an escalation point by standing policy).
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
