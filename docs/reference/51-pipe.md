# 51. Kernel pipe (P5-pipe)

Plan 9's `pipe(fd[2])` primitive at the kernel layer: a connected pair of Spoors backed by a shared in-kernel ring buffer. Per ARCH §10.3. The first **production** byte-pipe Spoor backend; replaces the test-only `test_pipe_dev` scaffold from P5-spoor-transport with a real reusable primitive.

---

## Purpose

A pipe is the simplest IPC primitive: one process writes bytes, another reads them, FIFO. Until this chunk, the kernel had no in-kernel byte pipe — the P5-spoor-transport adapter was tested only against an ad-hoc test scaffold defined inline in `test_9p_spoor_transport.c`. Pipe lands the production primitive:

- Test code now uses real pipes instead of scaffolds (`pipe.compose_with_spoor_transport` is the canonical e2e test).
- The future P5-stratumd boot path uses a pipe pair to talk to stratumd before vsock / Unix sockets exist.
- Future shell pipeline integration (post-fd-syscalls) builds the userspace `pipe(2)` syscall on this primitive.

---

## Semantics at v1.0 (non-blocking)

- **read** returns bytes available (0..n); **0 on empty buffer** (no blocking).
- **write** returns bytes accepted (0..n); **0 on full buffer** (no blocking).
- read on the **write** end → `-1` (wrong end).
- write on the **read** end → `-1` (wrong end).

The non-blocking discipline is sufficient for all v1.0 in-kernel uses (single-CPU, synchronous test sequencing, pre-staged frame writes through the 9P client). Blocking semantics (read sleeps until data arrives; write sleeps until space frees) land at **P5-pipe-blocking** with rendez integration — that chunk needs a spec extension because the missed-wakeup hazard (ARCH §28 I-9) enters scope.

EOF/closed-end semantics are **not** modeled at v1.0. Callers know the lifetime of both ends because they hold the Spoor pointers. POSIX `read returns 0 on writer close` is a Phase 5+ extension (when the pipe(2) syscall surfaces these endpoints to userspace).

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

`_Static_assert` pins `sizeof(struct pipe_ring) == 32 + 4096`. The ring is heap-allocated (kmalloc routes 4 KiB+ through alloc_pages; same path as p9_client). The endpoint is 16 bytes; SLUB-cached for compactness.

Two endpoints share one ring. Each Spoor's `aux` is its own `pipe_endpoint`. The Dev vtable's read / write dispatch on `is_read_end`.

### Ring buffer ops

Standard mod-arithmetic two-segment copy. `ring_write` copies up to `PIPE_BUF_SIZE - count` bytes; advances head; bumps count. `ring_read` copies up to `count` bytes; advances tail; drops count. Wraparound handled by splitting the copy into two segments.

### Dev vtable

`devpipe` is registered in the bestiary by `pipe_init` (called from `kernel/main.c` after `dev9p_init`).

- `read` checks `is_read_end`; calls `ring_read`. Wrong-end → -1.
- `write` checks `!is_read_end`; calls `ring_write`. Wrong-end → -1.
- `close` drops the ring's per-endpoint refcount. When the ring's ref hits 0, the ring is freed; the endpoint struct is always freed (it's per-Spoor).
- Other slots: stubs (attach returns NULL — Plan 9's `/srv` posting model isn't wired at v1.0; the Phase 5+ syscall surface lands it).

### Lifecycle

1. `pipe_create` allocates the ring (ref=2) + two endpoints + two Spoors. Each Spoor's aux is set to its endpoint.
2. Caller uses read/write through `Spoor->dev->{read,write}`.
3. `spoor_clunk` on either end → `devpipe_close` → drop ring ref to 1; endpoint freed; Spoor's `c->aux` cleared.
4. `spoor_clunk` on the other end → drop ring ref to 0; ring's magic clobbered; `kfree(ring)`; endpoint freed.

---

## Spec posture

**No new TLA+ module at v1.0.** The non-blocking pipe's correctness is local + structural:
- Ring buffer invariant (count = bytes-written - bytes-read; head / tail / count consistency) is tested directly.
- No wait/wake → no missed-wakeup hazard → I-9 doesn't apply at this chunk.

The blocking variant (P5-pipe-blocking) WILL need a spec extension — at minimum to compose with `specs/scheduler.tla`'s wait/wake state machine. Reader's wait condition is "buffer has data"; writer's is "buffer has space"; wake signals on the other side's mutation. This is the canonical missed-wakeup hazard and gets formal modeling when the impl lands.

---

## Tests

10 tests in `kernel/test/test_pipe.c`:

| Test | Covers |
|---|---|
| `pipe.smoke` | Create pair; write payload; read it back; FIFO order. |
| `pipe.read_on_empty_returns_zero` | Read with empty ring → 0 (non-blocking). |
| `pipe.write_to_full_returns_zero` | Fill ring (PIPE_BUF_SIZE bytes); next write → 0. |
| `pipe.write_short_when_partially_full` | Buffer has K free; write N>K → returns K. |
| `pipe.wraparound` | Write 3000 / read 2500 / write 3000 / read 3500 → all bytes in order across the wrap. |
| `pipe.read_on_write_end_rejected` | Write end's `dev->read` → -1. |
| `pipe.write_on_read_end_rejected` | Read end's `dev->write` → -1. |
| `pipe.close_one_end_keeps_other_alive` | Clunk read end; write end's Spoor still alive; ring still alive. |
| `pipe.close_both_ends_frees_ring` | Clunk both ends; `pipe_total_freed` increments. |
| `pipe.compose_with_spoor_transport` | Two pipe pairs wired into a `p9_spoor_transport` adapter; full Tversion + Tattach handshake through real pipes. **This is the canonical e2e test — proves the entire 9P stack composes against the production byte-pipe primitive.** |

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
| Non-blocking ring + Dev + Spoor pair | **Landed (P5-pipe)** |
| `pipe_init` bestiary registration | **Landed (P5-pipe)** |
| 10 unit tests including e2e composition with spoor-transport | **Landed (P5-pipe)** |
| Blocking semantics + rendez integration + spec extension | Deferred to **P5-pipe-blocking** |
| Userspace `pipe(2)` syscall | Deferred to **P5-fd-syscalls** |
| Plan 9 `/srv` posting (named pipes via the namespace) | Phase 5+ |
| Multi-CPU safety (per-pipe lock) | Deferred to P5-pipe-blocking |

---

## Known caveats / footguns

### Non-blocking semantics

`read` returning 0 means "empty buffer right now," NOT "EOF." Callers that need EOF must track the lifetime of the write end through some out-of-band mechanism (e.g., the Spoor pointer's existence). When the blocking variant lands, EOF becomes the standard "writer closed, buffer drained" signal.

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
