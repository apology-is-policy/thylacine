# 51. Kernel pipe (P5-pipe)

Plan 9's `pipe(fd[2])` primitive at the kernel layer: a connected pair of Spoors backed by a shared in-kernel ring buffer. Per ARCH §10.3. The first **production** byte-pipe Spoor backend; replaces the test-only `test_pipe_dev` scaffold from P5-spoor-transport with a real reusable primitive.

---

## Purpose

A pipe is the simplest IPC primitive: one process writes bytes, another reads them, FIFO. Until this chunk, the kernel had no in-kernel byte pipe — the P5-spoor-transport adapter was tested only against an ad-hoc test scaffold defined inline in `test_9p_spoor_transport.c`. Pipe lands the production primitive:

- Test code now uses real pipes instead of scaffolds (`pipe.compose_with_spoor_transport` is the canonical e2e test).
- The future P5-stratumd boot path uses a pipe pair to talk to stratumd before vsock / Unix sockets exist.
- Future shell pipeline integration (post-fd-syscalls) builds the userspace `pipe(2)` syscall on this primitive.

---

## Semantics (blocking; P5-pipe-blocking)

- **read** drains bytes from the buffer (1..n returned) when data is available; **blocks** (a per-call hook on the ring's `poll_list`, sleeping on a private stack Rendez) when empty AND write end open; **returns 0 (EOF)** when empty AND write end closed.
- **write** appends bytes (1..n returned, may be < n if buffer fills mid-write) when space is available; **blocks** (the same hook mechanism) when full AND read end open; **returns `-T_E_PIPE`** when read end closed (#100 -- it returned a flat `-1` until then, which made EPIPE unobtainable through both boundaries; see below).
- read on the **write** end → `-1` (wrong end).
- write on the **read** end → `-1` (wrong end).

The wait/wake protocol is modeled in `specs/pipe.tla` and pinned by `NoStuckReader` / `NoStuckWriter` invariants (specializations of ARCH §28 I-9 to the pipe's two-direction state machine). The TLC matrix is 2 clean cfgs (2 and 3 threads) + 5 buggy cfgs; four buggy variants elide the wake-after-mutation step, the fifth wakes ONE reader instead of all, and each produces a counterexample.

**Multi-waiter, wake-all (2026-09-02)**: any number of threads may block on either direction of one pipe at once. Each blocked reader/writer registers a per-call `struct poll_waiter` on the ring's `poll_list` -- the same list `.poll` uses -- under `r->lock`, atomically with the sample that found the ring not ready, and sleeps on a stack Rendez private to that call; every readiness edge (append, drain, either EOF) walks the list and wakes every hook, and a woken blocker re-samples and sleeps again if another waiter consumed the edge. This replaced a `struct Rendez` per direction on the ring, which is **single-waiter and extincts on a second sleeper** (`rendez.h`): sound while a pipe had one reader and one writer (the in-kernel uses of P5-pipe-blocking), an unprivileged kernel crash once `pipe2` (#155), `fork` (LINEAGE) and `CLONE_THREAD` (N-3) made every endpoint a Spoor many EL0 threads and Procs share -- `make -j 2>&1 | tee` (N children blocked in write on one full pipe) or a jobserver (N children blocked in read on one empty pipe) reached it. Found 2026-09-01 by the socktab holotype's self-audit while reading this file as the eventfd model; `tools/test.sh` on the pre-fix kernel with the two `pipe_blocking.two_*` tests EXTINCTS at the second sleeper.

---

## Public API — `<thylacine/pipe.h>`

```c
#define DEVPIPE_DC          '|'
#define PIPE_BUF_SIZE       4096u   // POSIX PIPE_BUF guarantee
#define PIPE_RING_MAGIC     0x50495045u   // "PIPE"
#define PIPE_ENDPOINT_MAGIC 0x50494550u   // "PIEP"

extern struct Dev devpipe;

void pipe_init(void);
int  pipe_create(struct Spoor **out_read_end, struct Spoor **out_write_end);
u64  pipe_total_allocated(void);
u64  pipe_total_freed(void);
```

`pipe_create` returns 0 on success / -1 on OOM. On success: both `*out_read_end` and `*out_write_end` are populated; caller owns both Spoors at ref=1 each. On failure: both outputs are NULL; no partial state remains.

---

## Implementation

`kernel/pipe.c` (~330 LOC).

### Data structures

```c
struct pipe_ring {
    u32     magic;          // PIPE_RING_MAGIC
    int     ref;            // 2 at creation; per-endpoint close drops by 1
    size_t  count;          // bytes in buffer; 0..PIPE_BUF_SIZE
    size_t  head;           // next write position (mod PIPE_BUF_SIZE)
    size_t  tail;           // next read position (mod PIPE_BUF_SIZE)
    u8      buf[PIPE_BUF_SIZE];   // 4 KiB FIFO
};

struct pipe_endpoint {
    u32                magic;       // PIPE_ENDPOINT_MAGIC
    struct pipe_ring  *ring;
    bool               is_read_end;
};
```

`_Static_assert` pins `sizeof(struct pipe_ring) == 56 + 4096` (was 32 + 4096 in the non-blocking pre-image; P5-pipe-blocking added `read_eof` + `write_eof` flags, a `spin_lock_t lock` and two `struct Rendez` wait queues -> 72; P5-poll-a added the 16-byte `poll_list` -> 88; the multi-waiter rewrite removed the two Rendez -> 56). The ring is heap-allocated (kmalloc routes 4 KiB+ through alloc_pages; same path as p9_client). The endpoint is 16 bytes; SLUB-cached for compactness.

Two endpoints share one ring. Each Spoor's `aux` is its own `pipe_endpoint`. The Dev vtable's read / write dispatch on `is_read_end`.

### Ring buffer ops

Standard mod-arithmetic two-segment copy. `ring_write` copies up to `PIPE_BUF_SIZE - count` bytes; advances head; bumps count. `ring_read` copies up to `count` bytes; advances tail; drops count. Wraparound handled by splitting the copy into two segments.

### Dev vtable

`devpipe` is registered in the bestiary by `pipe_init` (called from `kernel/main.c` after `dev9p_init`).

- `read` is a blocking loop: take `r->lock`; if `count > 0` → drain (via `ring_read`) → drop lock → `poll_waiter_list_wake(poll_list)` → return bytes drained. If `write_eof` + empty → drop lock → return 0 (EOF). If `CNONBLOCK` → drop lock → `-T_E_AGAIN`. Else → `pipe_block_locked(r)`: register a per-call hook on `poll_list` in the SAME hold, drop the lock, `sleep` on the call's private Rendez until the hook is walked, unregister; on `SLEEP_INTR` return -1 (death), else loop and re-sample.
- `write` is symmetric: take lock; if `read_eof` → return `-T_E_PIPE`. If space → append (via `ring_write`) → drop lock → `poll_waiter_list_wake(poll_list)` → return bytes written. Else → `-T_E_AGAIN` under `CNONBLOCK`, or `pipe_block_locked(r)`.
- `close` sets the appropriate EOF flag (`read_eof` or `write_eof`) under `r->lock`, drops the lock, **walks `poll_list`** -- every blocked reader/writer AND every poller wakes (the buggy variant skips this — caught by `BUGGY_CLOSE_*_NO_WAKE_*` spec configs). Then drops the ring's per-endpoint refcount; ring freed at 0; endpoint struct always freed.
- Other slots: stubs (attach returns NULL — Plan 9's `/srv` posting model isn't wired at v1.0; the Phase 5+ syscall surface lands it).

### Wait/wake discipline

The atomic check-then-sleep protocol is provided by `<thylacine/rendez.h>` (one sleeper per Rendez) composed with `<thylacine/poll.h>`'s register-then-observe (`specs/poll.tla`). The pipe's contribution is:
1. State mutation (count++, count--, eof := true) happens under `r->lock`.
2. A would-block caller registers its hook on `poll_list` while STILL holding `r->lock` -- the sample that found the ring not ready and the registration are one critical section, so no mutation can land between them.
3. After dropping `r->lock`, the mutator calls `poll_waiter_list_wake(poll_list)`, which sets each hook's `ready` under the list lock and `wakeup`s each hook's private Rendez. The rendez lock pairs (release/acquire) with the sleeper's cond evaluation, so a wake that ran between the registration and the sleep is seen at sleep entry (fast path) and one that runs after is delivered.
4. Every waiter is woken by every edge (wake-all); a waiter whose condition is false again after a peer consumed the edge re-registers and sleeps again.

Lock order: `r->lock` -> `poll_list.lock` -> the hook's Rendez lock (poll.h's object -> list -> rendez chain). A hook never outlives its call (`NoStaleHook`).

The discipline maps to `specs/pipe.tla` actions:
- `ReadDrain(t)` ↔ devpipe_read's drain branch + `WakeAllWriters`.
- `WriteAppend(t)` ↔ devpipe_write's append branch + `WakeAllReaders`.
- `CloseRead` ↔ devpipe_close's read-side branch (set read_eof + `WakeAllWriters`).
- `CloseWrite` ↔ devpipe_close's write-side branch (set write_eof + `WakeAllReaders`).

The `BuggyXxxNoWake` variants elide the wake → `NoStuckReader` / `NoStuckWriter` violated; `BuggyWriteAppendWakeOne` wakes one chosen reader (the old single-waiter `wakeup`) and leaves a second reader stuck while `CanRead` holds.

### Lifecycle

1. `pipe_create` allocates the ring (ref=2; lock + `poll_list` initialized) + two endpoints + two Spoors. Each Spoor's aux is set to its endpoint.
2. Caller uses read/write through `Spoor->dev->{read,write}`. Read on empty / write on full sleeps until woken.
3. `spoor_clunk` on either end → `devpipe_close` → set EOF flag + walk `poll_list` (so every sleeper and poller exits) → drop ring ref to 1; endpoint freed; Spoor's `c->aux` cleared.
4. `spoor_clunk` on the other end → drop ring ref to 0; ring's magic clobbered; `kfree(ring)`; endpoint freed.

---

## Spec posture

`specs/pipe.tla` (landed at P5-pipe-blocking) models the wait/wake protocol with 7 actions:

- Clean: `ReadDrain` / `ReadEof` / `ReadSleep` / `WriteAppend` / `WriteEpipe` / `WriteSleep` / `CloseRead` / `CloseWrite` (8 — symmetric pairs).
- Buggy: `BuggyWriteAppendNoWake` / `BuggyReadDrainNoWake` / `BuggyCloseWriteNoWake` / `BuggyCloseReadNoWake` (4 — each elides the wake after a state-enabling mutation).

4 invariants (`SingleWaiter` was retired with the single-waiter model; two waiters per side is now the point):
- `TypeOk` — state space type-safety.
- `EofMonotonic` — once set, never cleared.
- `NoStuckReader` — `NOT ∃t : threadState[t] = "WAITING_READ" AND CanRead`. I-9 specialized to the read side.
- `NoStuckWriter` — symmetric.

TLC verdicts at `Threads = {t1, t2}, CAP = 2`:

| Config | Verdict |
|---|---|
| `pipe.cfg` | Model checking completed; no error. |
| `pipe_buggy_write_no_wake_reader.cfg` | NoStuckReader violated. |
| `pipe_buggy_read_no_wake_writer.cfg` | NoStuckWriter violated. |
| `pipe_buggy_close_write_no_wake_reader.cfg` | NoStuckReader violated. |
| `pipe_buggy_close_read_no_wake_writer.cfg` | NoStuckWriter violated. |
| `pipe_multi.cfg` (`Threads = {t1, t2, t3}`) | Model checking completed; no error (40 distinct states; two readers or two writers may sleep at once). |
| `pipe_buggy_wake_one_reader.cfg` (3 threads) | NoStuckReader violated (one reader woken, the other stuck while `ringCount > 0`). |

The clean cfg models the impl discipline; each buggy cfg captures the bug class of "forgot to wake on a state-enabling mutation."

**Composition with `specs/scheduler.tla`**: `scheduler.tla::NoMissedWakeup` proves the atomic cond-check + sleep transition at the rendez API surface. `specs/pipe.tla::NoStuckReader/Writer` proves the pipe-side discipline of "every mutation that COULD enable a waiter MUST issue a wakeup." Together they close the missed-wakeup hazard end-to-end for the pipe.

---

## Tests

10 single-thread tests in `kernel/test/test_pipe.c` + 4 multi-thread tests in `kernel/test/test_pipe_blocking.c`:

### Single-thread (sequential write/read; no sleep)

| Test | Covers |
|---|---|
| `pipe.smoke` | Create pair; write payload; read it back; FIFO order. |
| `pipe.read_on_empty_returns_zero` | (Repurposed for blocking semantics:) close write end FIRST, then read on empty → 0 (EOF). |
| `pipe.write_to_full_returns_zero` | (Repurposed for blocking semantics:) close read end FIRST, then write → `-T_E_PIPE`. |
| `pipe.write_short_when_partially_full` | Buffer has K free; write N>K → returns K. |
| `pipe.wraparound` | Write 3000 / read 2500 / write 3000 / read 3500 → all bytes in order across the wrap. |
| `pipe.read_on_write_end_rejected` | Write end's `dev->read` → -1. |
| `pipe.write_on_read_end_rejected` | Read end's `dev->write` → -1. |
| `pipe.close_one_end_keeps_other_alive` | Clunk read end; write end's Spoor still alive; ring still alive. |
| `pipe.close_both_ends_frees_ring` | Clunk both ends; `pipe_total_freed` increments. |
| `pipe.compose_with_spoor_transport` | Two pipe pairs wired into a `p9_spoor_transport` adapter; full Tversion + Tattach handshake through real pipes. The canonical 9P-stack-composition test. |

### Multi-thread (sleep/wake protocol; P5-pipe-blocking)

Each test spawns a consumer thread that performs a blocking op; the boot thread then triggers the wake. Pattern matches `test_rendez_basic_handoff`. The two `two_*` tests spawn TWO consumers on one direction: on the pre-fix kernel the second `sleep()` extincts the suite ("rendez already has a waiter"); on the fixed kernel both sleep, one edge wakes both, exactly one consumes it, the other re-sleeps, and the next edge releases it. The in-guest twin is probe legs L272-L277 (two forked children blocked in `read()` on one empty pipe).

| Test | Covers |
|---|---|
| `pipe_blocking.write_wakes_sleeping_reader` | Consumer reads on empty → sleeps. Boot writes → reader wakes + drains. |
| `pipe_blocking.read_wakes_sleeping_writer` | Boot fills buffer. Consumer writes 1 more → sleeps. Boot drains → writer wakes + appends. |
| `pipe_blocking.close_write_end_wakes_reader_with_eof` | Consumer reads on empty → sleeps. Boot closes write end → reader wakes + returns 0 (EOF). |
| `pipe_blocking.close_read_end_wakes_writer_with_epipe` | Boot fills buffer. Consumer writes → sleeps. Boot closes read end → writer wakes + returns `-T_E_PIPE` (the blocked-writer arm, so it proves the code survives the wake path). |

---

## `fstat` on a pipe (#96)

`devpipe.stat_native` reports `T_S_IFIFO | 0600`, `nlink` 1, `size` 0,
`blksize` `PIPE_BUF_SIZE`. POSIX requires `fstat(2)` on a pipe to succeed and
report `S_IFIFO`; the pouch boundary-line passes `t_stat.mode` straight through
to `st_mode`, so `S_ISFIFO` works unchanged.

`size` is deliberately 0 rather than the buffered byte count. POSIX leaves
`st_size` unspecified for FIFOs and Linux reports 0; reporting the count would
invite a caller to size a read against it, which races the peer by
construction.

**Both ends of one pipe share a `qid.path`**, drawn from a monotonic counter at
`pipe_create` (never reused; starts at 1, so the historical unset value 0 stays
distinguishable). That is the POSIX convention — one pipe, one inode — and it
lets `fstat` tell two distinct pipes apart. Nothing else keys on a pipe's
`qid.path`, so the stamp is inert outside `stat_native`. The consumers that
*do* key on a Spoor's `qid.path` were each checked: the Larder is dev9p-only;
the cons/pts qid flag bits are dc-gated; the REVENANT FILE Burrow
(`burrow.c::file_qid_path`) and the Image cache (`image.c`) are reached only
from `exec`, which resolves through `stalk` — and a pipe has no name in any
namespace; territory mount keys need a `QTDIR`, and `devpipe_walk` returns
NULL. In any case the stamp is *monotonically* safer than what it replaced:
before it, **every** pipe carried `qid.path == 0`, so any keying that could
see a pipe was already colliding.

`.seekable` stays false, so this does **not** enable `lseek`/`pread` on a pipe
— the flag has decoupled "can fstat" from "can seek" since RW-4 R2-F2, and
`sys_prw.pipe_not_seekable` pins it.

*Why this was missing until CL-5:* no pouch program had ever had a pipe on fd
0/1/2 at startup. GNU make's `-jN` hands every concurrent job but the first the
read end of a broken pipe on fd 0 (`get_bad_stdin`), and clang treats a
non-EBADF fstat failure on a standard fd as fatal — so the on-device build
storm's parallel jobs died silently. See `docs/LLVM-DESIGN.md` §7.2.

---

## Error paths

- `pipe_create` returns -1 if any of ring / endpoint / Spoor allocation fails. Full rollback of partial state.
- `dev->read` / `dev->write` return -1 on: NULL spoor, corrupted endpoint magic, wrong end (read on write end / write on read end), NULL ring pointer in priv.
- `dev->close` extincts on: ring ref underflow (would-be-negative), corrupted ring magic.

---

## Performance characteristics

- `read` / `write` are O(n) byte copies (mandatory).
- Two cache caches: 1 SLUB cache for `pipe_endpoint` (16 B objects); 1 kmalloc path for `pipe_ring` (4 KiB objects via alloc_pages).
- One per-pipe spinlock (`r->lock`) plus the `poll_list` lock; a blocked call costs a stack Rendez + hook and one list register/unregister.

---

## Status

| Component | State |
|---|---|
| Ring + Dev + Spoor pair | **Landed (P5-pipe)** |
| `pipe_init` bestiary registration | **Landed (P5-pipe)** |
| Per-pipe spin lock + 2 rendez wait queues + EOF flags | **Landed (P5-pipe-blocking)** |
| Blocking read / write / close-wakes-other-side | **Landed (P5-pipe-blocking)** |
| `specs/pipe.tla` + 4 buggy cfgs (NoStuckReader / NoStuckWriter) | **Landed (P5-pipe-blocking)** |
| 14 unit tests (10 sequential + 4 multi-thread blocking) | **Landed (P5-pipe-blocking)** |
| Userspace `pipe(2)` syscall | Deferred to **P5-fd-syscalls** |
| Plan 9 `/srv` posting (named pipes via the namespace) | Phase 5+ |
| Multi-waiter wait queues (more than one reader / writer sleeping at once) | Phase 5+ when poll / futex land |

---

## Known caveats / footguns

### `CNBFRAME` — the frame-atomic non-blocking write mode (round-B F1, 2026-08-18)

A write whose tx Spoor carries `CNBFRAME` (`spoor.h` bit 6) takes an early arm in `devpipe_write` that is **atomic** and **non-blocking**: it commits the *whole* buffer iff it fits (`PIPE_BUF_SIZE - count >= n`) and otherwise returns `-T_E_AGAIN` having written **nothing** — never a partial write, never a `sleep()`. The default (unflagged) write is unchanged: partial when the pipe is nearly full, blocking (`sleep(write_rendez)`) when exactly full.

It exists for **one** consumer: the tx end of the byte-pipe 9P transport (`p9_spoor_transport_init` sets the flag). The 9P client holds `c->lock` across `p9_transport_send`, so a **blocking** pipe write there is the `#360` lock-across-sleep extinction (an unprivileged multi-threaded container filling `c2s` with concurrent `/proc` opens kills the guest), and a **partial** write strands a 9P-frame fragment and desyncs the shared stream (`do_send` treats a mid-frame EAGAIN as fatal, `#349`). `-T_E_AGAIN` == `P9_TRANSPORT_EAGAIN` (-11) lets `client_send_flow` drop `c->lock` and retry. A `read_eof` still yields the `pipe` note + `-T_E_PIPE` under CNBFRAME. Regression: `pipe.cnbframe_atomic_nonblocking` (the exact contrast to `pipe.write_short_when_partially_full`). Nothing but the 9P transport tx should set this flag.

### Blocking semantics — read returning 0 means EOF

`read` returning 0 unambiguously means "write end closed AND buffer drained" (EOF). Empty buffer with write end still open → reader sleeps. Symmetric for write: `-T_E_PIPE` means the read end closed. Both signals are
POSIX-shaped.

**#100 (ER-3).** Until #100 that write returned a flat `-1`, and `T_E_PIPE` --
defined and ABI-pinned in `errno.h` since the errno scripture landed -- had **no
emitter anywhere in the tree**. `-1` is what pouch's `__syscall_ret` treats as
the generic flat-error sentinel (so a pouch program saw `EIO`) and what stock
musl reads as `errno = 1` (so a VIVARIUM guest saw `EPERM`). A comment here
asserted "the -1 return is the load-bearing EPIPE signal that musl's write
wrapper translates to errno"; no such wrapper exists -- pouch's
`src/unistd/write.c` is a plain tag-dispatch shim to `syscall_cp`. So the one
errno POSIX makes load-bearing for a closed pipe was unobtainable through both
boundaries, tree-wide, while four separate comments said otherwise.

The wrong-end and torn-endpoint rejects in the same two functions likewise
became `-T_E_BADF` / `-T_E_INVAL`. `!p` (a Spoor with a NULL aux) stays a flat
`-1` per the ERRORS.md preamble-guard rule: an internal invariant violation, not
a caller error, unreachable from EL0.

### Multi-waiter discipline (was: single-waiter)

Until 2026-09-02 this section said "at most one thread sleeps on each direction at a time; a second extincts; for v1.0 in-kernel uses this is fine." The sentence was true when written and silently falsified by `pipe2` + `fork` + `CLONE_THREAD`, which made every pipe end an object shared across EL0 threads and Procs -- the DEBUGGING-PLAYBOOK 6.15 red flag (a single-waiter Rendez on state reachable from more than one thread) that the per-struct sweep did not reach. Any number of sleepers per direction is legal now; the residual footgun is the `sleep()` primitive itself: a future Dev that embeds a `struct Rendez` in an object a shared Spoor can reach must either guard a second sleeper (`irqfwd`'s `KOBJ_IRQ_WAIT_BUSY`, `srvconn`'s `reading`/`writing`) or use the per-call hook shape this file now does. Sweep result at the fix: `devsrv`'s `srv_accept_blocking` sleeps on a service-embedded Rendez with no guard, reachable only by the posting Proc's own threads (trusted servers hold `MAY_POST_SERVICE`) -- tracked, not fixed here.

### Wrong-end calls return -1, not extinct

A caller mistakenly calling `dev->write` on the read end gets -1 back, not an extinction. This is deliberate: in v1.0 the only callers are kernel-internal (tests + future P5-stratumd boot path), and rejecting with -1 lets callers detect bugs without crashing the kernel. When user-visible `pipe(2)` lands, the syscall layer will translate -1 into a POSIX `EBADF`.

### `pipe_create` failure rollback is conservative

If the second Spoor alloc fails after the first succeeded, the rollback path detaches the first Spoor's aux before clunking — `devpipe_close` sees no priv and skips the ring-ref decrement. The ring is then freed manually. This means a partial failure NEVER drops the ring's refcount via the close path; the path that frees the ring is unconditional in the rollback. Documented because the code looks asymmetric.

### Ring is 4 KiB exactly

PIPE_BUF_SIZE = 4096 matches POSIX's PIPE_BUF guarantee (writes ≤ PIPE_BUF are atomic — atomic at the Plan 9 level too once concurrency lands). The struct adds 32 bytes of header → 4128 bytes total → kmalloc routes through alloc_pages (order=2 = 16 KiB allocation). The waste (12 KiB unused per pipe) is acceptable at v1.0; future tightening can pack multiple pipes into a single allocation.

### No `bread` / `bwrite`

The Block-I/O slots are stubs returning NULL / -1. Pipes are byte-stream-only at v1.0. Block-framed pipes would need a deeper structural change (multi-segment ring); not in v1.0 scope.

---

## Naming rationale

`pipe` matches POSIX + Plan 9. No thematic marsupial alternative reads better. The Dev character `'|'` matches Plan 9 9front's devpipe + the shell pipe glyph.

`pipe_ring` for the shared buffer struct; `pipe_endpoint` for the per-Spoor priv. Both prefixed with `pipe_` for consistency with the module name.

---

## Reference

- ARCH §10 (IPC); §10.3 (Pipes).
- `docs/reference/50-9p-spoor-transport.md` (the immediate consumer; pipes are the production byte-pipe Spoor backend).
- `docs/reference/30-dev-spoor.md` (the Spoor / Dev abstraction).
- ROADMAP §7.3 (Phase 5 deliverables; pipe was mentioned as a Phase 5 primitive).
