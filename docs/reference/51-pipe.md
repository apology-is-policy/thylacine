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

- **read** drains bytes from the buffer (1..n returned) when data is available; **blocks** (sleeps on `read_rendez`) when empty AND write end open; **returns 0 (EOF)** when empty AND write end closed.
- **write** appends bytes (1..n returned, may be < n if buffer fills mid-write) when space is available; **blocks** (sleeps on `write_rendez`) when full AND read end open; **returns -1 (EPIPE)** when read end closed.
- read on the **write** end → `-1` (wrong end).
- write on the **read** end → `-1` (wrong end).

The wait/wake protocol is modeled in `specs/pipe.tla` and pinned by `NoStuckReader` / `NoStuckWriter` invariants (specializations of ARCH §28 I-9 to the pipe's two-direction state machine). The TLC matrix is 1 clean cfg + 4 buggy cfgs; each buggy variant elides the wake-after-mutation step and produces a counterexample.

**Single-waiter discipline**: at most one thread sleeps on each direction at a time. Mirrors the impl's `struct Rendez` (single-waiter; see `kernel/include/thylacine/rendez.h`). Multi-waiter wait queues are Phase 5+ (poll, futex).

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

`_Static_assert` pins `sizeof(struct pipe_ring) == 72 + 4096` (was 32 + 4096 in the non-blocking pre-image; P5-pipe-blocking added `read_eof` + `write_eof` flags, a `spin_lock_t lock`, and two `struct Rendez` wait queues, growing the header from 32 to 72 bytes). The ring is heap-allocated (kmalloc routes 4 KiB+ through alloc_pages; same path as p9_client). The endpoint is 16 bytes; SLUB-cached for compactness.

Two endpoints share one ring. Each Spoor's `aux` is its own `pipe_endpoint`. The Dev vtable's read / write dispatch on `is_read_end`.

### Ring buffer ops

Standard mod-arithmetic two-segment copy. `ring_write` copies up to `PIPE_BUF_SIZE - count` bytes; advances head; bumps count. `ring_read` copies up to `count` bytes; advances tail; drops count. Wraparound handled by splitting the copy into two segments.

### Dev vtable

`devpipe` is registered in the bestiary by `pipe_init` (called from `kernel/main.c` after `dev9p_init`).

- `read` is a blocking loop: take `r->lock`; if `count > 0` → drain (via `ring_read`) → drop lock → `wakeup(write_rendez)` → return bytes drained. If `write_eof` + empty → drop lock → return 0 (EOF). Else → drop lock → `sleep(read_rendez, cond_can_read, r)`; on wake, loop.
- `write` is symmetric: take lock; if `read_eof` → return -1 (EPIPE). If space → append (via `ring_write`) → drop lock → `wakeup(read_rendez)` → return bytes written. Else → sleep on `write_rendez`.
- `close` sets the appropriate EOF flag (`read_eof` or `write_eof`) under `r->lock`, drops the lock, **calls `wakeup` on the OTHER side's rendez** (the buggy variant skips this — caught by `BUGGY_CLOSE_*_NO_WAKE_*` spec configs). Then drops the ring's per-endpoint refcount; ring freed at 0; endpoint struct always freed.
- Other slots: stubs (attach returns NULL — Plan 9's `/srv` posting model isn't wired at v1.0; the Phase 5+ syscall surface lands it).

### Wait/wake discipline

The atomic check-then-sleep protocol is provided by `<thylacine/rendez.h>`. The pipe's contribution is:
1. State mutation (count++, count--, eof := true) happens under `r->lock`.
2. After dropping `r->lock`, call `wakeup(rendez)` to deliver the wake to any sleeper.
3. The rendez's wakeup acquires its own lock, which pairs (release/acquire) with the next sleeper's cond evaluation at sleep entry — ensuring the cond reads the post-mutation state.

The discipline maps to `specs/pipe.tla` actions:
- `ReadDrain(t)` ↔ devpipe_read's drain branch + `wakeup(write_rendez)`.
- `WriteAppend(t)` ↔ devpipe_write's append branch + `wakeup(read_rendez)`.
- `CloseRead` ↔ devpipe_close's read-side branch (set read_eof + `wakeup(write_rendez)`).
- `CloseWrite` ↔ devpipe_close's write-side branch (set write_eof + `wakeup(read_rendez)`).

The `BuggyXxxNoWake` variants elide the wakeup call → `NoStuckReader` / `NoStuckWriter` violated.

### Lifecycle

1. `pipe_create` allocates the ring (ref=2; lock + rendezes initialized) + two endpoints + two Spoors. Each Spoor's aux is set to its endpoint.
2. Caller uses read/write through `Spoor->dev->{read,write}`. Read on empty / write on full sleeps until woken.
3. `spoor_clunk` on either end → `devpipe_close` → set EOF flag + wake the other side's rendez (so any sleeper exits) → drop ring ref to 1; endpoint freed; Spoor's `c->aux` cleared.
4. `spoor_clunk` on the other end → drop ring ref to 0; ring's magic clobbered; `kfree(ring)`; endpoint freed.

---

## Spec posture

`specs/pipe.tla` (landed at P5-pipe-blocking) models the wait/wake protocol with 7 actions:

- Clean: `ReadDrain` / `ReadEof` / `ReadSleep` / `WriteAppend` / `WriteEpipe` / `WriteSleep` / `CloseRead` / `CloseWrite` (8 — symmetric pairs).
- Buggy: `BuggyWriteAppendNoWake` / `BuggyReadDrainNoWake` / `BuggyCloseWriteNoWake` / `BuggyCloseReadNoWake` (4 — each elides the wake after a state-enabling mutation).

5 invariants:
- `TypeOk` — state space type-safety.
- `SingleWaiter` — at most one thread in `WAITING_READ`; at most one in `WAITING_WRITE`. Mirrors the rendez API.
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
| `pipe.write_to_full_returns_zero` | (Repurposed for blocking semantics:) close read end FIRST, then write → -1 (EPIPE). |
| `pipe.write_short_when_partially_full` | Buffer has K free; write N>K → returns K. |
| `pipe.wraparound` | Write 3000 / read 2500 / write 3000 / read 3500 → all bytes in order across the wrap. |
| `pipe.read_on_write_end_rejected` | Write end's `dev->read` → -1. |
| `pipe.write_on_read_end_rejected` | Read end's `dev->write` → -1. |
| `pipe.close_one_end_keeps_other_alive` | Clunk read end; write end's Spoor still alive; ring still alive. |
| `pipe.close_both_ends_frees_ring` | Clunk both ends; `pipe_total_freed` increments. |
| `pipe.compose_with_spoor_transport` | Two pipe pairs wired into a `p9_spoor_transport` adapter; full Tversion + Tattach handshake through real pipes. The canonical 9P-stack-composition test. |

### Multi-thread (sleep/wake protocol; P5-pipe-blocking)

Each test spawns a consumer thread that performs a blocking op; the boot thread then triggers the wake. Pattern matches `test_rendez_basic_handoff`.

| Test | Covers |
|---|---|
| `pipe_blocking.write_wakes_sleeping_reader` | Consumer reads on empty → sleeps. Boot writes → reader wakes + drains. |
| `pipe_blocking.read_wakes_sleeping_writer` | Boot fills buffer. Consumer writes 1 more → sleeps. Boot drains → writer wakes + appends. |
| `pipe_blocking.close_write_end_wakes_reader_with_eof` | Consumer reads on empty → sleeps. Boot closes write end → reader wakes + returns 0 (EOF). |
| `pipe_blocking.close_read_end_wakes_writer_with_epipe` | Boot fills buffer. Consumer writes → sleeps. Boot closes read end → writer wakes + returns -1 (EPIPE). |

---

## Error paths

- `pipe_create` returns -1 if any of ring / endpoint / Spoor allocation fails. Full rollback of partial state.
- `dev->read` / `dev->write` return -1 on: NULL spoor, corrupted endpoint magic, wrong end (read on write end / write on read end), NULL ring pointer in priv.
- `dev->close` extincts on: ring ref underflow (would-be-negative), corrupted ring magic.

---

## Performance characteristics

- `read` / `write` are O(n) byte copies (mandatory).
- Two cache caches: 1 SLUB cache for `pipe_endpoint` (16 B objects); 1 kmalloc path for `pipe_ring` (4 KiB objects via alloc_pages).
- No locking at v1.0 (single-CPU); blocking variant adds a per-pipe spinlock + a pair of rendez wait queues.

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

### Blocking semantics — read returning 0 means EOF

`read` returning 0 unambiguously means "write end closed AND buffer drained" (EOF). Empty buffer with write end still open → reader sleeps. Symmetric for write: -1 means EPIPE (read end closed). Both signals are POSIX-shaped.

### Single-waiter discipline

At most one thread sleeps on each direction at a time. A second thread attempting to sleep extincts (rendez.h's `single-waiter` invariant). For v1.0 in-kernel uses (single consumer + single producer per pipe), this is fine. Multi-waiter wait queues (multiple consumers competing for one pipe end, or multiple producers blocking on full) need poll / futex extensions; not in v1.0 scope.

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
