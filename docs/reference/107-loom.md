# 107 — Loom: the io_uring-inverted 9P ring transport

**Design**: `docs/LOOM.md` (signed off 2026-06-05). **Spec**: `specs/loom.tla`
(TLC-green; gates the impl). **Invariants**: ARCH §28 I-29 (completion
integrity) + I-30 (submit-time capability pin). **Audit-trigger surface**: ARCH
§25.4 + CLAUDE.md.

> **Status (Loom-2a + Loom-2b + Loom-3 + Loom-4a/4b/4c/4d + Loom-5a + Loom-5b):** the ring **substrate**
> (2a — `KObj_Loom`, the SQ/CQ ring memory + geometry, `SYS_LOOM_SETUP`, the
> registered-handle table via `SYS_LOOM_REGISTER`, the kobj refcount lifecycle),
> the **pluggable-completion 9P-engine seam** (2b — `p9_rpc.on_complete`,
> `p9_client_submit_async`, `p9_client_reader_pump_once`, the CQ writer
> `loom_post_cqe`), the **batch-enter core** (3 — `SYS_LOOM_ENTER`, the
> SQE→`p9_client_*` dispatch, the submit-time capability pin I-30, the production
> async-op container, and the `loom_free` quiesce-before-free, #898), and the
> **SQPOLL** arc (4a — the transport recv-deadline + a deadline-aware reader pump;
> 4b — the **CQ wait-list** `l->cq_waiters` + the `loom_enter` wait-phase rework,
> so a concurrent enter that finds a sibling holding the reader role sleeps until a
> CQE is posted; 4c — the **SQPOLL poll-thread** `loom_sqpoll_main`, a per-ring
> `kproc()` kthread that drains the SQ zero-syscall + drives the reader, started by
> `SYS_LOOM_SETUP(LOOM_SETUP_SQPOLL)` and joined by `loom_free`). v1.0 dispatches
> the no-payload opcodes (`NOP` / `FSYNC`); the payload opcodes
> (READ/WRITE/GETATTR/...) land with Loom-6's registered-buffer surface and post
> `-ENOSYS` until then. **4d** is the focused SQPOLL audit + close (one Opus
> prosecutor 0/1/1/3 + a concurrent self-audit; the core cruxes SOUND): **F1 [P1]**
> the `loom_first_inflight_client` borrowed-client UAF (a concurrent reap +
> re-register could free the Spoor → the `p9_client` while the kthread/ENTER held
> the bare `cl`) → fixed with a borrow-guard `spoor_ref` clunked after the pump;
> **F2 [P2]** the SQPOLL park busy-loop on CQ-full backpressure → the park cond
> gates on CQ admittability; **SA-2/F3/F5 [P3]** the `P9_PUMP_BUSY` yield + the
> NULL-deadline-transport comment + the test `cq_tail` mirror; **F4 [P3]** the
> mid-frame-recv-unbounded v1.x untrusted-server seam. **5a** adds **multishot**
> (`LOOM_SQE_MULTISHOT`): one SQE produces a STREAM of CQEs — a `LOOM_CQE_MORE`-set
> shot per reply that RE-ARMS the op, then exactly one MORE-clear terminal — built
> against the synthetic FSYNC vehicle (a real re-armable op is Loom-6's event-fd
> READ). **5b** adds **LINK/DRAIN** (`LOOM_SQE_LINK` / `LOOM_SQE_DRAIN`): an
> ordering-relevant SQE is HELD in a per-ring chain (`l->chain`) and dispatched
> only once its gates open — a linked successor after its predecessor is done ok
> (a failed link member cancels the rest of the chain, each posting one `-ECANCELED`
> CQE); a drain barrier after all prior ops are done. `CQE_SKIP` is deferred (it
> needs a `loom_order.tla` carve-out). Registered buffers + the native libthyla-rs
> API + the benchmark are Loom-6.

---

## Purpose

Loom is the inversion of Linux's io_uring. Rather than import io_uring's opcode
zoo, it exposes the kernel's existing pipelined 9P client (the #841
elected-reader engine) to userspace through a shared-memory submission/completion
ring living in a Burrow. Userspace posts 9P-shaped op descriptors into a
submission queue; the kernel drives them through `p9_client_*`; R-messages return
as completion-queue entries correlated by `user_data`. The opcodes *are* the 9P
operation set, so the async batching layer covers files, `/net`, `/proc`, `/srv`,
and devices uniformly — Linux's hardest io_uring question ("what is the operation
vocabulary?") dissolves because 9P is already the uniform vocabulary.

`KObj_Loom` is a per-Proc kernel object naming one ring instance. It owns the
ring Burrow (mapped into the Proc's address space + reachable by the kernel via
the direct map) and a fixed-handle table of registered `KObj_Spoor`s.

---

## Public API

### ABI (`kernel/include/thylacine/loom.h`) — mirrored by libthyla-rs at Loom-6

```c
struct loom_sqe  { u8 opcode; u8 flags; u16 _resv0; u32 handle_idx;
                   u64 offset; u32 len; u32 buf_idx_or_off;
                   u64 user_data; u64 _resv1[4]; };          // 64 bytes
struct loom_cqe  { u64 user_data; s32 result; u32 flags; };  // 16 bytes
struct loom_ring_hdr { u32 sq_head, sq_tail, sq_mask, sq_entries,
                           cq_head, cq_tail, cq_mask, cq_entries,
                           flags, dropped, overflow; u32 _pad[5]; }; // 64 bytes
struct loom_params { u32 flags, sq_entries, cq_entries, ring_size;
                     u64 ring_va; u32 hdr_off, sq_array_off, sqe_off, cqe_off,
                     sq_array_size, sqe_size, cqe_size, _resv0;
                     u64 _resv1[4]; };                        // 88 bytes
```

Opcodes (`LOOM_OP_*`): the `p9_client_*` surface (WALK / LOPEN / LCREATE / READ /
WRITE / GETATTR / SETATTR / READDIR / FSYNC / CLUNK / RENAMEAT / UNLINKAT / MKDIR
/ SYMLINK / LINK / MKNOD / READLINK / STATFS) + `LOOM_OP_NOP` + the **reserved**
`LOOM_OP_WIRE_PASSTHROUGH` (designed, not built at v1.0). Defined now as the fixed
ABI; dispatched at Loom-3..5.

### Syscalls (`kernel/syscall.c`)

- `SYS_LOOM_SETUP(entries, params_va) → loom_fd / -1` (=66). `entries`: SQ
  entries, power of two, 1..`LOOM_MAX_ENTRIES` (4096). `params_va`: a
  `struct loom_params`; IN `params.flags` accepts `LOOM_SETUP_SQPOLL` (Loom-4c —
  start a per-ring poll-thread); `LOOM_SETUP_CQSIZE` + any unknown bit reject; OUT
  the ring geometry (with `params.flags` echoing the granted bits). Allocates the
  ring Burrow, maps it RW into the caller (the burrow-attach window), spawns the
  SQPOLL kthread if requested (before the handle is returned, so every earlier
  rollback path runs with no kthread to join), installs a `KObj_Loom` handle,
  returns the fd.
- `SYS_LOOM_REGISTER(loom_fd, op, arg_va, nargs) → 0 / -1` (=67). `op`:
  `LOOM_REGISTER_HANDLES` (install the fixed-handle table) at Loom-2a;
  `LOOM_REGISTER_BUFFERS` reserved (Loom-6). `arg_va`: a `u32[nargs]` of fds
  (each must be `KOBJ_SPOOR` in the caller). Replaces the whole table
  (`IORING_REGISTER_FILES` semantics).
- `SYS_LOOM_ENTER(loom_fd, to_submit, min_complete, flags) → n / -1` (=68,
  Loom-3). Consume up to `to_submit` SQEs (SQ-index order), dispatch each, then —
  if `min_complete > 0` and not `LOOM_ENTER_NONBLOCK` — wait for completions:
  drive the elected reader itself (blocking on recv, death-interruptible #811) or,
  when a sibling thread already holds the reader role, sleep on the ring's CQ
  wait-list until that reader posts a CQE (Loom-4b), until ≥ `min_complete` CQEs
  are available or no async op remains in flight; finally reap completed-op
  containers. Returns the count of SQEs consumed. Per-op failures (bad opcode /
  handle / rights) surface as an **error CQE** (`result < 0`), not a syscall
  error — the SQE is still consumed (io_uring semantics). `flags`:
  `LOOM_ENTER_GETEVENTS` (informational) / `LOOM_ENTER_NONBLOCK`.

### Kernel-internal (`kernel/loom.c`, declared in `loom.h`)

```c
struct Loom *loom_create(u32 sq_entries, u32 cq_entries);
void loom_ref(struct Loom *l);
void loom_unref(struct Loom *l);   // last drop: clunk regs + burrow_unref + free
int  loom_register_handles(struct Loom *l, struct Spoor **spoors,
                           const rights_t *rights, u32 n);
u64  loom_total_created(void);
u64  loom_total_destroyed(void);
int  loom_enter(struct Loom *l, u32 to_submit, u32 min_complete, u32 flags);  // Loom-3
// the testable syscall inners (kernel/syscall.c):
int  sys_loom_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                             struct loom_params *out, hidx_t *out_fd);
int  sys_loom_register_for_proc(struct Proc *p, hidx_t loom_fd, u32 op,
                                const hidx_t *fds, u32 n);
int  sys_loom_enter_for_proc(struct Proc *p, hidx_t loom_fd, u32 to_submit,
                             u32 min_complete, u32 flags);                    // Loom-3
```

### The async engine front-end (`kernel/9p_client.{c,h}`, Loom-2b)

```c
int  loom_post_cqe(struct Loom *l, u64 user_data, s32 result, u32 flags);
// the pluggable-completion seam on the in-flight op:
typedef void (*p9_rpc_complete_fn)(struct p9_rpc *rpc, int status,
                                   struct p9_dispatch_result *dr);  // in struct p9_rpc
typedef int  (*p9_session_build_fn)(struct p9_session *s, u8 *out, size_t cap, void *ctx);
int  p9_client_submit_async(struct p9_client *c, struct p9_rpc *rpc,
                            p9_session_build_fn build, void *build_ctx);
int  p9_client_reader_pump_once(struct p9_client *c);
void p9_client_handoff_reader(struct p9_client *c);
void p9_client_abandon_async(struct p9_client *c, struct p9_rpc *rpc);  // Loom-3 (#898/#845)
// resolve a registered dev9p Spoor -> (client, fid) for the submit-time pin:
int  dev9p_client_fid(struct Spoor *c, struct p9_client **out_client, u32 *out_fid);  // kernel/dev9p.c
```

---

## Implementation

### Ring geometry (`loom_create`, `kernel/loom.c`)

One anonymous Burrow holds, in order, each region 64-aligned, the whole
page-rounded:

| Region | Offset | Size |
|---|---|---|
| `loom_ring_hdr` | 0 | 64 |
| SQ index ring (`u32[sq_entries]`) | `sq_array_off` | `sq_entries * 4` |
| SQE array (`loom_sqe[sq_entries]`) | `sqe_off` | `sq_entries * 64` |
| CQE array (`loom_cqe[cq_entries]`) | `cqe_off` | `cq_entries * 16` |

`cq_entries` defaults to `2 * sq_entries` (the io_uring default). The kernel
stamps the immutable masks + entry counts into the header at create (the Burrow
pages are `KP_ZERO`, so head/tail/flags/diagnostics start at 0), then `dsb ish`
to publish the stores to the inner-shareable domain.

### Kernel ↔ user shared memory

The ring Burrow is anonymous + physically contiguous (an `alloc_pages` chunk).
The kernel reaches the bytes via the Burrow's direct-map alias
(`pa_to_kva(page_to_pa(ring->pages))`, cached as `l->ring_kva`) — the `exec.c`
precedent for kernel writes through a Burrow alias. Userspace sees the same pages
through its `burrow_map`'d VMA. The direct-map base is stable for the Burrow's
lifetime because the Loom holds a `handle_count` ref so the pages are never freed
under it.

### Refcount lifecycle (the #847 dual-refcount + the kobj refcount)

`loom_create` returns the Loom with `refcount = 1` (adopted by the handle
installed by `SYS_LOOM_SETUP`) and the ring Burrow with `handle_count = 1` (held
by the Loom). `burrow_map` adds a separate `mapping_count = 1` for the user VMA.
The ring pages stay alive while **either** count is non-zero (#847). Teardown:

- `handle_close` (or `proc_free`'s handle-table teardown) → `handle_release_obj`
  (outside the table lock) → `loom_unref`. The last drop runs `loom_free`:
  clunk every registered Spoor (may sleep — safe, no lock held), `burrow_unref`
  the ring (drops `handle_count`), clobber the magic, `kfree`.
- `handle_get`/`handle_put` balance via `loom_ref`/`loom_unref` — `handle_get`
  acquires a ref so the snapshot's `obj` stays alive across the borrow.

`SYS_LOOM_SETUP` rolls back fully on failure: `handle_alloc` failure →
`burrow_unmap` + `loom_unref`; a `params` copy-out fault → `handle_close` (drops
the ring's `handle_count`) + `burrow_unmap` (drops `mapping_count` → frees the
pages), so a faulting caller never leaks a ring it cannot see.

### Registered-handle table (the I-30 substrate)

`struct Loom.reg[LOOM_MAX_REG_HANDLES]` (64) holds
`{ struct Spoor *spoor; rights_t rights; }`. `SYS_LOOM_REGISTER`'s inner resolves
each fd via `handle_get` (KOBJ_SPOOR-gated), takes the table's **own**
`spoor_ref` (decoupled from the caller's fd — the caller may close it), snapshots
the handle rights, and `handle_put`s the get's ref. `loom_register_handles`
replaces the whole table under `l->lock`, snapshotting the old Spoors and
clunking them **outside** the lock (`spoor_clunk` may sleep). The held
`spoor_ref` + the rights snapshot are the I-30 submit-time-pin substrate: Loom-3's
dispatch resolves `(client, fid)` from `reg[idx].spoor` at submit and the held
ref pins the object for the op's lifetime.

### The pluggable-completion seam (Loom-2b)

Every synchronous `p9_client_*` op blocks its submitter on a stack `rendez`
until the matching reply; the #841 pipelining (`inflight[tag]`, the elected
reader, the demux — lock dropped across recv) is internal + reusable. Loom-2b
adds a **completion front-end** to the in-flight `struct p9_rpc`:

- `on_complete == NULL` — **WAKE_RENDEZ** (the existing synchronous path). The
  reader copies the reply into the submitter's `reply_buf` + `wakeup`s its
  `rendez`; the submitter dispatches + extracts. Unchanged.
- `on_complete != NULL` — **POST_CQE** (async / Loom). There is no submitter, so
  the engine *itself* dispatches the reply at demux time (under `c->lock`) and
  hands the mapped result to `on_complete`, which writes a `loom_cqe`.

The elected-reader / demux machinery is **unchanged** — only the completion
*action* is pluggable (one engine, two front-ends; LOOM.md §8.4). Three sites
branch on `on_complete`:

- **`demux_frame_locked`** (owned tag): async → clear `inflight[tag]`,
  `p9_session_dispatch_rmsg` (clears `session.outstanding[tag]` + applies fid
  state, as `client_run` does for a sync op), `map_error`, then `on_complete`
  with the result + the dispatch result (`dr` aliases the recv buffer, valid
  only for the callback). Sync → the existing copy-into-`reply_buf` + `wakeup`.
- **`client_mark_dead_locked`** (transport EOF / error): async → clear
  `inflight[tag]` + `on_complete(rpc, -P9_E_IO, NULL)` (an error CQE — there is
  no rendez to wake). Sync → `dead = true` + `wakeup`.
- **`client_handoff_reader_locked`**: **skips** async ops as handoff targets —
  an async op has no thread to run the reader loop. Its reply is demuxed by the
  reap caller (`p9_client_reader_pump_once` / `SYS_LOOM_ENTER` / SQPOLL),
  never by becoming the elected reader.

`p9_client_submit_async(c, rpc, build, ctx)` builds the Tmsg under `c->lock`
(via the `build` thunk, which allocates the tag), registers `rpc` as async
in-flight, sends, and returns **without** waiting. It **takes ownership** of
`rpc`: exactly one `on_complete` fires on every path — a build/peek/send failure
fires it immediately (`-P9_E_IO`); success fires it later via demux / mark_dead.
`p9_client_reader_pump_once(c)` becomes the reader for one frame (drops `c->lock`
across recv, demuxes, hands the role on) — the reap-side drive for async ops.

`loom_post_cqe(l, user_data, result, flags)` writes a `loom_cqe` and publishes
the tail bump with a release store. **It computes its write index from
KERNEL-PRIVATE geometry only** — `l->cq_tail` (a kernel-private authoritative
tail; the shared `loom_ring_hdr.cq_tail` is a userspace-readable *mirror*) and
the private `l->cq_entries` mask — **never** from the shared `cq_mask` / `cq_tail`
(both userspace-writable). Trusting the shared header would let a hostile Proc
(under Loom-3, where the ring is userspace-controlled) set `cq_mask = 0xFFFFFFFF`
+ `cq_tail = 0x10000` and drive an out-of-bounds kernel write — the CQ-side of
the ring-TOCTOU discipline (LOOM.md §6.1). **CQ back-pressure
(`CqNeverOverfull`, I-29):** a full CQ is *not* overwritten — the post is
refused (`-1`) and the shared `overflow` counter is bumped. The `cq_head` is
read from the shared header for the fullness check (userspace owns it); a
hostile `head` can only make a Proc overwrite its OWN claimed-reaped CQE (the
masked index stays in-bounds — never OOB), the io_uring self-trust model. The
no-LOST-completion liveness (hold until a slot frees) is realized by Loom-3's
submit-time admission (consume an SQE only when the CQ can hold its completion =
io_uring's model), which makes a full CQ at completion unreachable in production.

> **The seam contract — `on_complete` runs under `c->lock`** (from `demux`
> *and* from `mark_dead`), so it MUST NOT sleep and MUST NOT re-enter the
> `p9_client_*` API. `loom_post_cqe` (a leaf `l->lock`) + `kfree` are fine;
> dropping the Loom's last ref (`loom_unref` → `spoor_clunk` may sleep) is NOT.
> The **production** async op (Loom-3, below) honors this: `loom_async_complete`
> only posts a CQE + flags the op terminal under `l->lock` — it never clunks the
> pin or frees the container (the reap sweep / `loom_free` do that outside the
> lock).

Lock order: `c->lock → l->lock` (the only direction; `loom.c` never calls into
`p9_client_*`, so it is acyclic). The Loom-3 reap sweep + quiesce respect it: the
reap takes only `l->lock`; the quiesce releases the loom list lock-free (at
refcount 0) before taking each op's `c->lock` via `p9_client_abandon_async`.

---

### SYS_LOOM_ENTER: dispatch + the submit-time pin + the async-op lifetime (Loom-3)

`loom_enter` (`kernel/loom.c`) is the batch-enter core. **Submit phase**: for
each of up to `min(to_submit, sq_entries)` slots, **under `l->lock`** it reads
the user-owned `sq_tail` (advisory; `sq_tail == sq_head` → drained → stop),
applies **submit-time CQ admission** (`loom_cq_ready(l) + async_inflight >=
cq_entries` → stop, leaving the SQE for the next enter — io_uring's model, so a
non-reaping consumer back-pressures here instead of dropping a completion at post
time; F2 / I-29), claims one slot by reading + advancing the **kernel-private**
`sq_head & (sq_entries-1)` (the SA-1 discipline on the SQ side — the per-iteration
`l->lock` also serializes two concurrent enters so neither double-dispatches a
slot nor loses an `sq_head` update; F1), range-checks the user-written
indirection (`>= sq_entries` → bump `dropped`, never index `sqes[]`), and
**copies the whole SQE to a kernel-local `struct loom_sqe`** (ring TOCTOU, §6.1).
Then — **outside** the lock — `loom_submit_one` dispatches the copy (it re-takes
`l->lock` for the FSYNC pin + the in-flight link, a separate non-nested
acquisition):

- **`LOOM_OP_NOP`** — completes inline (`loom_post_cqe` result 0); no engine, no
  handle.
- **`LOOM_OP_FSYNC`** — the submit-time pin (I-30): under `l->lock`, read
  `reg[handle_idx]`'s Spoor + rights snapshot and take an **independent
  `spoor_ref`** (held for the op's whole lifetime — a concurrent re-register
  cannot free it mid-grab); gate `RIGHT_WRITE` (fsync is a durability barrier on
  written data; a read-only handle → `-EACCES`, never re-checked at completion);
  resolve `(client, fid)` via `dev9p_client_fid`; allocate a `struct
  loom_async_op` (the production container, `rpc` at offset 0), link it into
  `l->inflight_ops` + bump `async_inflight`, and `p9_client_submit_async` it
  (which takes ownership of `&op->rpc`).
- **other in-range opcodes** — `-ENOSYS` (the payload opcodes need Loom-6's
  registered-buffer surface); **out-of-range** — `-EINVAL`; **any reserved SQE
  flag set** — `-EINVAL` (so a future LINK/DRAIN/MULTISHOT is never silently
  ignored). Each posts an error CQE inline; the SQE is still consumed.

**Wait/reap phase** (only if `min_complete > 0` and not `LOOM_ENTER_NONBLOCK`):
`loom_wait_for_completions` loops until ≥ `min_complete` CQEs are posted,
`async_inflight` hits 0 (no more completions possible), the session dies, or the
caller's Proc is dying. Each iteration the caller tries to become the elected
reader and drive one frame (`p9_client_reader_pump_once`):

- pump returns 1 — a frame was demuxed; re-sample (bounded by the flood budget so
  a Byzantine server flooding ownerless frames cannot spin a CPU unbounded in one
  syscall);
- pump returns < 0 — session dead / death-interrupt (#811) → stop;
- pump returns 0 — a **sibling thread of the same Proc** already holds the reader
  role. The caller then **sleeps on the ring's CQ wait-list** (`l->cq_waiters`)
  until that reader's demux posts a CQE (Loom-4b — this resolves the Loom-3
  limitation where a concurrent enter merely returned what was posted).

The sleep is **register-then-observe** (poll.tla lineage): under `l->lock` the
caller installs a `struct poll_waiter` on `l->cq_waiters` AND re-samples
`loom_cq_ready` — a CQE posted just before the hook went live is caught by the
sample, one posted after is caught by the wake flag the sleep's cond reads (under
its own Rendez lock). The producer (`loom_post_cqe`, after publishing the CQE +
releasing `l->lock`) calls `poll_waiter_list_wake` — the pipe.c release-then-wake
precedent; it takes only the list + rendez locks (below `l->lock` in the global
order) and never sleeps / re-enters `p9_client_*`, so it composes with the
`c->lock` the async-completion path holds (the seam contract). The sleep is
death-interruptible: a `SLEEP_INTR` return (#811) unwinds at the EL0-return
die-check. `loom.tla`: `CqWaitRegister` ↔ the register + sample; `PostCqe`-wake ↔
`loom_post_cqe`'s wake; `CqWaitCommitOrSleep` ↔ the flag/give-up/sleep decision;
invariants `CqFlagTracksCq` + `NoMissedCqWake` (I-9 on the CQ wait-list) +
`CqWaitFlagSound`.

**NoStrandedWaiter holds vacuously at Loom-4b/4c**: `SYS_LOOM_ENTER` holds a loom
ref for its whole duration, so `loom_free` cannot run while a waiter sleeps; and a
Loom ring is mapped into exactly one Proc (`KObj_Loom` is non-transferable), so
every concurrent enter is a sibling thread of that one Proc — if the Proc
group-terminates, all of them are death-interrupted together. The spec's
`Teardown`-wakes-the-wait-list action is still made **literal** in `loom_free`
(`poll_waiter_list_wake(&l->cq_waiters)`, a no-op walk of the empty list here),
kept as defense if that ref invariant ever weakens. The SQPOLL kthread (Loom-4c)
reads on the ring's behalf but is **not** a CQ waiter — it is joined by an
explicit handshake (below), not woken via the wait-list.

Then `loom_reap_terminal` unlinks every terminal op (under `l->lock`) and releases
its pin + frees the container (outside the lock).

**Completion** (`loom_async_complete`, the `on_complete` callback): runs under
`c->lock` from the demux / `mark_dead`; it posts the op's CQE (`loom_post_cqe`)
and, under `l->lock`, sets `terminal` + decrements `async_inflight`. It does
**not** free/clunk/unref — the reap sweep (or quiesce) does, outside the lock.

**Quiesce-before-free (#898)** (`loom_free`, at refcount 0): an in-flight async
op does **not** hold a loom ref, so `loom_free` can run with ops still linked
(e.g. the Proc closed `loom_fd` or died with an op submitted-but-unreaped). It
must abandon them so a late reply on a *shared* client cannot post into freed
memory. For each linked op: `p9_client_abandon_async` (under the client's
`c->lock`, mutually exclusive with the demux) clears `inflight[tag]` — after
which no `on_complete` can fire for it — and Tflushes (the #845 mechanism: the
tag stays reserved `awaiting_flush`, a late original reply is discarded
ownerless); then the pin is clunked + the container freed. The loom is freed only
**after** this loop, so a demux that wins `c->lock` first posts at-most-one CQE
into the still-allocated ring (the loom is alive throughout) — no double, no
stale, no UAF (I-29). Because `SYS_LOOM_ENTER` holds a loom ref (via
`handle_get`) for its whole duration, `loom_free` cannot run concurrently with a
live enter's reap.

### The SQPOLL poll-thread (Loom-4c)

`LOOM_SETUP_SQPOLL` starts a per-ring `kproc()` kthread (`loom_sqpoll_main`, the
`console_mgr` precedent — a kernel thread of the immortal PID-0 Proc) so
steady-state submission is **zero syscalls**: userspace writes SQEs + bumps
`sq_tail`, the kthread drains them. The loop:

1. check `sqpoll_stopping` (acquire) → break to the terminal;
2. `loom_drain_sq(l, sq_entries)` — the submit loop factored out of `loom_enter`
   (NOPs complete inline; FSYNCs go async, linked into `inflight_ops`);
3. if any op is in flight, `loom_first_inflight_client` returns that op's client
   AND a **borrow-guard `spoor_ref`** on its pin (F1 — so a concurrent reap +
   re-register cannot free the Spoor → the client while the kthread derefs it in
   the pump; clunked after the pump); drive the elected reader with a
   `timer_now_ns() + LOOM_SQPOLL_IDLE_NS` (10 ms) **frame-boundary idle-deadline**
   (the 4a `p9_client_reader_pump_once_deadline`): `PROGRESS` posts a CQE +
   wakes `cq_waiters`; `IDLE` (the deadline lapsed at a frame boundary, no bytes
   consumed → stream still synced, #841-safe) re-checks the stop flag; `DEAD`
   lets the session-death error-CQEs drain `inflight` to 0; `BUSY` (a peer ENTER
   momentarily holds the reader) **yields (`sched()`, SA-2)** so it does not
   tight-loop — all loop;
4. else (SQ empty + nothing in flight) announce `LOOM_RING_SQ_NEED_WAKEUP` and
   `sleep` on the new `sqpoll_park` Rendez. The park cond is lock-free
   (`stopping || (sq_tail != sq_head && loom_cq_ready(l) < cq_entries)`, F2) —
   sound because `async_inflight` is always 0 at park time (only the kthread adds
   in-flight ops, and it is parked), so the kthread is the sole `cq_tail`/`sq_head`
   writer and the reads are race-free. Gating on CQ **admittability** (not "SQ
   non-empty" alone) is what keeps a CQ-full + pinned-`sq_tail` ring from
   busy-looping: with the CQ full the kthread parks until the user reaps (advances
   `cq_head`) + ENTER-wakes it (the NEED_WAKEUP contract). `sq_head` is the
   kthread's own single-writer field (on an SQPOLL ring `loom_enter` does **not**
   submit — it only wakes the park), so reading it in the cond is same-thread.

**`SYS_LOOM_ENTER` on an SQPOLL ring** does not consume SQEs (the kthread owns
submission); it clears `NEED_WAKEUP` + wakes `sqpoll_park`, then — if
`min_complete > 0` — sleeps on `cq_waiters` (the kthread is the reader →
`loom_wait_for_completions` gets `P9_PUMP_BUSY` and takes the sleep arm).

**Deadline-capable gate.** The kthread block-recvs in process context with **no**
death-interrupt (kproc never group-terminates), so it relies on the idle-deadline
to re-check `stopping`. A NULL-deadline transport (the **spoor** pipe-pair backend,
`SYS_ATTACH_9P`; srvconn + the loopback test backend ARE deadline-capable) would
block it un-interruptibly → hang teardown. `loom_register_handles` therefore
rejects a NULL-deadline dev9p client (`p9_client_recv_is_deadline_capable`) into an
SQPOLL ring; a non-dev9p Spoor is allowed (it can never go async, so the kthread
never recvs on it). A mid-frame (post-header) recv is NOT deadline-bounded (a
mid-frame timeout would desync the stream, #841), so a Byzantine server that stalls
a frame body can delay the join until the body arrives / the connection EOFs — a
v1.x untrusted-server seam (F4); v1.0 servers complete frames promptly.

**Lifetime + the join.** The kthread holds **no** loom ref (a ref would deadlock
`loom_free`'s join). The Loom owns it; the join is the sole lifetime authority.
`loom_free` (at refcount 0) **first** joins: set `sqpoll_stopping` (release), wake
`sqpoll_park`, spin until `sqpoll_exited` (acquire), then `thread_free`. Only then
does the #898 quiesce run (the kthread is the other mutator of `inflight_ops`, so
it must be stopped first). The kthread's **terminal handshake** masks IRQs
(`spin_lock_irqsave(NULL)`, the `bootcpu_idle_main` idiom) across
`current_thread()->state = THREAD_EXITING` + the `sqpoll_exited` release + `sched()`:
the IRQ mask removes the preempt window that could lose the signal, and the
release/acquire pair guarantees the joiner sees `EXITING` (so `thread_free`'s
not-RUNNING gate holds) before it reaps. `sched()` then switches away permanently
(`EXITING` is never re-enqueued); `thread_free` spins on `on_cpu` (cleared by the
next thread's finish-task-switch) before reclaiming the Thread + kstack. This is
the `wait_pid` reap terminal minus the Proc-zombie bookkeeping a kproc thread
cannot run (`thread_exit_self` extincts from kproc). Spawn (`loom_start_sqpoll`,
called from `sys_loom_setup_for_proc` after the map, before `handle_alloc`):
`thread_create_with_arg(kproc(), loom_sqpoll_main, l)` → set `l->sqpoll` → `ready`.
Spawning before `handle_alloc` means a `handle_alloc` failure reclaims the kthread
through the same `loom_unref` → `loom_free` join.

### Multishot streams (Loom-5a)

A `LOOM_SQE_MULTISHOT` op produces a STREAM of CQEs rather than a single
completion: a `LOOM_CQE_MORE`-set shot per reply (the op RE-ARMS after each) then
exactly one MORE-clear terminal CQE, and nothing after the terminal. It is the
I-29 "exactly one terminal completion" property generalised from a single CQE to a
stream (`specs/loom_multishot.tla`: `ExactlyOneTerminal` + `TerminalEndsStream`).
A single-shot op (Loom-3) is the 1-shot special case. v1.0 exercises the mechanism
against the synthetic FSYNC vehicle (re-fsync until a per-op `shot_limit`); Loom-6's
event-fd READ is the real re-armable consumer (its terminal is the event source's
EOF/error, not a count).

The mechanism lives entirely in three places:

- **Submit** (`loom_submit_one`): the FSYNC case parses `LOOM_SQE_MULTISHOT`, sets
  `op->multishot` + `op->shot_limit` (the synthetic bound, carried in `sqe->offset`),
  and stores `op->build` (the Tmsg builder, re-run verbatim on each re-arm). The
  I-30 pin (`spoor_ref`) + the tag are taken ONCE at submit and held for the whole
  stream — never re-resolved per shot (`ObjPinnedAcrossShots`; the io_uring
  credential-vs-work race amplified by the long multishot lifetime). An inline op
  (`NOP`) ignores `MULTISHOT` and posts a single terminal CQE — the mechanism is
  async-only.

- **Completion** (`loom_async_complete`, UNDER `c->lock`): decides MORE vs terminal
  (`term = !multishot || status < 0 || shots+1 >= shot_limit`), posts the CQE
  (MORE-set for a shot, clear for the terminal), then under `l->lock` drops the op's
  `async_inflight` reservation and EITHER flags `op->rearm` + `rearm_pending++` (a
  MORE shot) OR sets `op->terminal` (the terminal). It does NOT re-submit — the seam
  contract forbids re-entering `p9_client_*` under `c->lock`.

- **Re-arm** (`loom_rearm_pending`, in BOTH drive loops — `loom_wait_for_completions`
  at the loop top + `loom_sqpoll_main` after the SQ drain, OUTSIDE `c->lock`): for
  each `op->rearm` op the CQ can admit (`loom_cq_ready + async_inflight < cq_entries`
  — the Loom-3 submit-time admission, now per shot, which makes a MORE shot ALWAYS
  have a slot when it completes → `CqNeverOverfull`), it clears `rearm`, reserves the
  next slot (`async_inflight++`), and re-issues `op->build` on the SAME pinned
  `(client, fid)`. A claim is atomic under `l->lock`, so two concurrent drivers (a
  sibling ENTER + the SQPOLL kthread) never double-issue. The claim releases `l->lock`
  BEFORE `submit_async` (which takes `c->lock`), so the seam's `c->lock → l->lock`
  order is never inverted. A back-pressured shot (CQ full at re-arm) stays `rearm`-
  flagged and HELD — never dropped — until userspace reaps a slot.

**SQPOLL back-pressure resume**: a held re-arm on an SQPOLL ring would strand if the
kthread parked (the user reaps without a new SQE, so the SQE-only park cond never
re-fires). `loom_sqpoll_park_cond` therefore also wakes on `rearm_pending > 0 && CQ
has room`, so the user's post-reap ENTER (which wakes the park) resumes the stream.
`rearm_pending` is mutated under `l->lock` but read lock-free by the park cond (the
cond cannot take `l->lock`; it is atomic for the cross-lock read).

**Teardown** is unchanged: an armed (or `rearm`-pending) multishot op is quiesced by
`loom_free`'s #898 abandon (`p9_client_abandon_async` — a no-op when the op holds no
outstanding tag, e.g. a `rearm`-pending op between shots) + the pin clunk, so no late
shot posts a stale CQE (`NoStaleAfterTeardown`).

### LINK / DRAIN chains (Loom-5b)

`LOOM_SQE_LINK` and `LOOM_SQE_DRAIN` impose ORDER on otherwise-concurrent ring ops
(`specs/loom_order.tla`). LINK chains a successor behind its predecessor's success;
DRAIN is a full pipeline barrier. The mechanism is a per-ring **held-submission
chain** (`l->chain`, an ordered singly-linked list under `l->lock`) **layered on top
of** the audited Loom-3/4 `loom_async_op` lifecycle — a chain entry (`struct
loom_chain_op`) is a kernel SQE copy + the `link`/`drain` flags + a `state`, and is
dispatched (via `loom_submit_one`, which for an async op carries a back-pointer
`op->chain`) only once its ordering gates open.

- **Routing** (`loom_drain_sq`): an SQE is ordering-relevant — HELD in `l->chain`
  rather than dispatched immediately — iff it sets LINK/DRAIN, OR the chain is
  already non-empty (a pending drain/link forces every subsequent op to order after
  it; an independent op so routed just admits immediately). The chain only GROWS
  during a drain call (admit/reclaim run between calls), so routing is monotone
  within a batch: once non-empty, every later SQE in the batch joins it — which
  keeps a LINK group together. Independent fast-path ops (chain empty, no flags)
  dispatch exactly as Loom-3.

- **Admission** (`loom_admit_chain`, in the drive loops — `loom_enter` after the
  submit, `loom_wait_for_completions` + `loom_sqpoll_main` at the loop top, OUTSIDE
  `c->lock`): walk head→tail, take ONE action per pass, loop until nothing is
  actionable, then reclaim. The gates (`loom_order.tla`):
  - **Link cancel-cascade** — a HELD op whose immediate predecessor LINKs to it and
    finished non-ok (`DONE_FAIL` / `CANCELLED`) is CANCELLED with exactly one
    `-ECANCELED` CQE, never dispatched (`EveryDoneOpPosted` + `NoOrphanCancel`). The
    cascade is a single head→tail walk: a just-cancelled victim becomes the non-ok
    predecessor of the next.
  - **Link gate** — a linked successor waits until its predecessor is `DONE_OK`
    (`LinkAdmits`).
  - **Drain gates** (`loom_chain_drain_admits`, `DrainAdmits`) — a post-drain op
    waits behind every earlier drain; a drain op itself waits until every earlier
    chain entry is done AND `async_inflight == 0` AND `rearm_pending == 0`. The
    `async_inflight == 0` is the load-bearing catch for prior FAST async ops not in
    the chain (once the chain is non-empty all later ops route to it, so when every
    chain-before entry is done `async_inflight` counts only ops submitted before the
    chain started); the `rearm_pending == 0` (audit F1) catches a prior FAST
    **multishot** op that is back-pressured between a MORE shot and its re-arm —
    momentarily out of `async_inflight` but its stream not done — so a drain
    admitted via `loom_enter`'s submit-phase `loom_admit_chain` (which has no
    preceding `loom_rearm_pending`) cannot jump ahead of a live multishot stream.
  - All gates open → claim the entry `INFLIGHT` under `l->lock` (so a concurrent
    admit never double-dispatches; `loom_chain_done` also writes `chain->state`
    under `l->lock` — audit F3), then `loom_submit_one` sets the real terminal
    (inline) or leaves it INFLIGHT (async, until the reply, when `loom_async_complete`
    records `DONE_OK`/`DONE_FAIL` from the status).
  - Every action posts one CQE, so it needs CQ admission room
    (`loom_cq_ready + async_inflight < cq_entries`) — no room HOLDS the whole chain
    (back-pressure), exactly like `loom_rearm_pending`. On an SQPOLL ring,
    `loom_sqpoll_park_cond` also wakes on a non-empty chain so a CQ-back-pressured
    held op resumes after the user reaps + ENTER-wakes. A **cancel** whose
    `-ECANCELED` post fails (the CQ filled between the room check and the post under
    a concurrent driver — the inherited Loom-3 over-admit window) reverts the victim
    to HELD and retries on the next admit pass, so a cancel is never lost, only
    deferred (audit F2 cancel leg, `EveryDoneOpPosted`).

- **Reclamation** (`loom_reclaim_chain`): frees terminal entries, but ONLY when NO
  entry is HELD — a HELD entry may still read a predecessor's terminal result, so a
  terminal predecessor under a live HELD successor must not be freed. With no HELD
  entry, every entry is INFLIGHT (already admitted; never re-consults predecessors)
  or terminal → freeing the terminal ones is safe, and the chain returning to empty
  restores fast-path routing. The chain is bounded at `cq_entries` entries
  (`loom_drain_sq` stops consuming at the cap) so a drain-blocked flood can't grow it
  unbounded (held ops don't count in `async_inflight`, so CQ admission alone would
  not bound them).

A chain member is never multishot — the combo is rejected (`-EINVAL`); a chain
member must be single-completion (`loom_order.tla` models one CQE per op).
`LOOM_SQE_CQE_SKIP` is rejected too (deferred: suppressing a success CQE breaks
`EveryDoneOpPosted`; it needs a `loom_order.tla` carve-out first). **Single-producer
note:** link integrity holds for the io_uring single-producer SQ contract (one
submitting thread / the SQPOLL kthread); a hostile multi-thread submitter on one
ring may see its own links mis-ordered, but never a kernel-safety violation (no UAF
— a reclaimed predecessor is simply gone from the list; a successor links to
whatever survives).

**Teardown** (`loom_free`): with `refcount == 0` and the SQPOLL kthread joined, the
chain is freed silently — a HELD entry never started (no pin/tag/async op), an
INFLIGHT entry's backing `loom_async_op` was already abandoned by the #898 quiesce.

---

## Data structures

`struct Loom` (`kernel/loom.c`): `magic` (`LOOM_MAGIC`, offset 0 — SLUB-free UAF
defense), atomic `refcount`, `lock` (protects `reg[]` + `cq_tail` + `sq_head` +
`inflight_ops` + `async_inflight` + `rearm_pending` (Loom-5a) + `chain`/`chain_tail`/`chain_len` (Loom-5b); geometry is immutable post-setup), `ring` (the
Burrow, `handle_count` ref held), `ring_kva` (cached direct-map base), the
geometry fields (mirror `loom_params`), the kernel-private `cq_tail` (Loom-2b) +
`sq_head` (Loom-3) authoritative ring indices (the shared-header copies are
userspace-readable mirrors — the SA-1 discipline), `inflight_ops` + `async_inflight`
(Loom-3, the in-flight async-op list), `cq_waiters` (Loom-4b, the CQ wait-list — a
`struct poll_waiter_list` carrying its **own** lock; a `SYS_LOOM_ENTER` thread that
finds a sibling holding the reader role sleeps here, woken by `loom_post_cqe`), the
Loom-4c SQPOLL fields (`sqpoll` — the kthread `struct Thread *`, NULL unless
`LOOM_SETUP_SQPOLL`; `sqpoll_stopping` / `sqpoll_exited` — the single-writer
release/acquire flags of the join handshake; `sqpoll_park` — the Rendez the kthread
parks on when idle), and `reg[LOOM_MAX_REG_HANDLES]`. The ABI structs are
byte-pinned by `_Static_assert` (sqe 64, cqe 16, hdr 64, params 88).

`struct loom_async_op` (`kernel/loom.c`, Loom-3): one in-flight async op,
heap-allocated per dispatched SQE, owned by the Loom (in `inflight_ops`). `rpc`
(the embedded `struct p9_rpc`) is at **offset 0** (`_Static_assert`) so the
completion callback recovers the container by a single cast. Carries `loom`,
`client` (borrowed, valid while `pinned` held), `pinned` (the independent
`spoor_ref` — the I-30 submit-time pin), `user_data`, `op_fid` + `op_arg`
(resolved at submit), `opcode`, `terminal` (set under `l->lock` by completion),
and `next`. It does **not** hold a loom ref (see the quiesce-before-free note).

`KObj_Loom` (`kernel/include/thylacine/handle.h`): `KOBJ_LOOM = 10`, a **fourth**
partition (`KOBJ_KIND_LOOM_MASK`) — non-transferable + non-hardware + non-srv. A
ring is meaningless to another Proc (its VA + registered Spoors are Proc-local),
so it is never transferable and never dup-able (`handle_dup` rejects it).

---

## Spec cross-reference

`specs/loom.tla` models the op-lifecycle (`empty → posted → snap → inflight →
completed → cqd → reaped`, `+ abandoned` on teardown). Loom-2a implements the
spec's substrate (the registered-object table `UserRegister`, the ring + CQ
`cq`); **Loom-2b implements the `PostCqe` completion action** — the seam's async
front-end maps to `Dispatch → ReplyArrives → PostCqe`, and `loom_post_cqe`'s
full-CQ refusal is the spec's `Cardinality(cq) < CQ_CAP` guard (`CqNeverOverfull`
holds at runtime, verified by `loom.post_cqe_back_pressure`). The session-death
arm is a `PostCqe` with a negative result (the spec models the phase + the
exactly-once count, not the result value). **Loom-3 implements the remaining
actions**: `Consume` (`loom_submit_one`'s SQE copy + the admission gate) with the
I-30 submit-time pin (`ObjPinnedToSnapshot` / `ActedUnderAdmittedRights` — the
`spoor_ref` + the `RIGHT_WRITE` gate, never re-resolved at completion); `Dispatch`
(act on the kernel snapshot, `ArgPinnedToSnapshot` — the SQE is copied to kernel
memory, never re-read); `Reap` (`loom_reap_terminal`); and the ring-teardown
`abandoned` quiesce (`NoStaleCompletion` — `loom_free` → `p9_client_abandon_async`,
#898). **Loom-3 adds no new spec mechanism** — `loom.tla` (clean + liveness + the
5 buggy cfgs) is unchanged and remains the pre-commit gate; the engine-touching
`p9_client_abandon_async` re-runs `9p_client.tla` (clean + 4 buggy cfgs) clean.
See `specs/SPEC-TO-CODE.md`.

---

## Tests

### Substrate (`kernel/test/test_loom.c`, 22 cases)

- `loom.create_geometry` — the ring layout is consistent (64-aligned,
  non-overlapping, page-rounded), the header is stamped (masks + counts; heads /
  tails / flags zero), `ring_kva` populated + readable.
- `loom.create_rejects_bad_args` — 0 / non-power-of-2 / over-max / `cq < sq` /
  non-power-of-2 cq rejected.
- `loom.refcount_lifecycle` — `ref`/`unref` balance; freed only on the last drop
  (`loom_total_destroyed` delta).
- `loom.setup_via_proc` — `sys_loom_setup_for_proc` installs the handle + maps
  the ring (VMA present at `ring_va`, in the burrow window); the handle is
  `KOBJ_LOOM`; the ring header is kernel-readable; `proc_free` frees the ring
  exactly once.
- `loom.setup_rejects` — entries 0 / 3 / over-max / unsupported flag / NULL proc.
- `loom.register_handles` — register two pipe Spoors; the ring holds them with
  rights snapshots; slot 2 empty.
- `loom.register_rejects` — `LOOM_REGISTER_BUFFERS` / over-cap nargs /
  non-KOBJ_SPOOR fd / bogus loom_fd rejected.
- `loom.register_replaces` — re-register fewer (whole-table replace; old slots
  cleared + clunked); clear (n = 0) releases all.
- `loom.post_cqe_back_pressure` (Loom-2b) — fill the CQ; a post into a full CQ
  is refused (`-1`), bumps `overflow`, and does **not** overwrite a staged CQE
  (`CqNeverOverfull`); a user reap frees slots so later posts land (wrapping).
- `loom.post_cqe_ignores_hostile_header` (Loom-2b, the SA-1 regression) —
  corrupt the shared `cq_mask` + `cq_tail` to hostile values; `loom_post_cqe`
  still writes at the **kernel-private** index (idx 0), proving it never trusts
  the userspace-writable header for its write position (no OOB).
- `loom.dup_rejected` (Loom-2b) — `handle_dup(loom_fd)` is rejected (KOBJ_LOOM
  is non-transferable, the same gate as the hardware / srv kinds).
- `loom.enter_nop` (Loom-3) — a `LOOM_OP_NOP` SQE consumed by `loom_enter`
  completes inline (CQE result 0, `user_data` echoed); the kernel-private +
  shared-mirror `sq_head` both advance; nothing goes in flight.
- `loom.enter_submit_rejects` (Loom-3) — four error-CQE paths: bad `handle_idx`
  (`-EINVAL`), empty registered slot (`-EBADF`), unimplemented in-range opcode
  (`-ENOSYS`), out-of-range opcode (`-EINVAL`); all consumed, none dispatched.
- `loom.enter_flags_and_bad_index` (Loom-3) — a reserved SQE flag → `-EINVAL`;
  an out-of-range SQ-index indirection is **dropped** (bumps `dropped`, never
  indexes `sqes[]` — the SA-1 discipline on the SQ side).
- `loom.cq_waiter_wake` (Loom-4b) — register a `poll_waiter` on `l->cq_waiters`;
  a successful `loom_post_cqe` sets its `ready` flag (PostCqe-wake → `NoMissedCqWake`).
- `loom.cq_waiter_no_spurious_wake_on_full` (Loom-4b) — fill the CQ, register a
  waiter; a **refused** post (CQ full) does NOT set the flag (`CqWaitFlagSound`).
- `loom.enter_inline_min_complete` (Loom-4b) — a BLOCKING `loom_enter` with inline
  NOPs and `min_complete` == the NOP count returns without sleeping (the first
  sample sees the CQ satisfied; `CqWaitCommitOrSleep`'s satisfied arm).
- `loom.enter_min_complete_no_inflight` (Loom-4b) — a BLOCKING `loom_enter` with
  `min_complete > 0` but no work staged returns at once (the give-up arm —
  `async_inflight == 0` → nothing more can complete; must not hang).
- `loom.sqpoll_setup_and_teardown` (Loom-4c) — `LOOM_SETUP_SQPOLL` spawns the
  kthread (`l->sqpoll != NULL`; `params.flags` echoes the grant); proc teardown
  joins it + frees the ring exactly once (a hang would mean the join never
  converged — the spawn → park → stop → EXITING-terminal → `thread_free` path
  end to end).
- `loom.sqpoll_drains_sq` (Loom-4c) — on an SQPOLL ring, stage 3 NOPs + bump
  `sq_tail`, `ENTER(to_submit=0)` wakes the kthread (it never submits on SQPOLL);
  a bounded cooperative-`sched()` wait observes `cq_tail` reach 3 — the kthread
  drained + posted zero-syscall.
- `loom.sqpoll_parks_on_cq_full` (Loom-4d, the F2 regression) — fill the CQ to
  `cq_entries` in `sq_entries`-sized batches (no reaping), stage one more pending
  NOP: the kthread cannot admit it and must reach `THREAD_SLEEPING` (the park),
  not busy-loop (pre-F2 the park cond fired on `sq_tail != sq_head` alone and
  `sleep` returned without `sched()`ing → a 100% spin); reap + ENTER-wake and the
  extra NOP drains (the park releases on the NEED_WAKEUP contract).

(The reworked wait phase's reader-drive path is also exercised end-to-end by
`9p_client.loom_fsync_e2e` below, which calls `loom_enter(1, 1, 0)` — a blocking
`min_complete = 1` enter that pumps the elected reader. The full sleep → woken-by-
a-real-sibling-reader interleaving needs a second kernel thread + the loopback and
lands with the deferred multi-in-flight / cross-Proc-death SMP harness, OWED since
#841.)

### The seam + Loom-3 engine dispatch + Loom-5a multishot + Loom-5b LINK/DRAIN (`kernel/test/test_9p_client.c`, 13 cases)

- `9p_client.async_op_posts_cqe` — `submit_async(clunk)` (no wait) →
  `reader_pump_once` demuxes the Rclunk → `on_complete` posts a CQE with the
  op's `user_data` + result 0.
- `9p_client.async_session_death_posts_error_cqe` — an in-flight async op
  completes with an error CQE (`-P9_E_IO`) when the transport dies (the
  `mark_dead` async arm; no rendez to wake).
- `9p_client.async_handoff_skips_async` — the elected-reader handoff picks a
  pending sync op and **skips** an async op (white-box: one of each in
  `inflight[]`).
- `9p_client.loom_fsync_e2e` (Loom-3) — register a dev9p Spoor (RIGHT_WRITE) in
  a Loom, stage an FSYNC SQE, `loom_enter(1,1,0)` dispatches the Tfsync via the
  submit-time pin, pumps the Rfsync, posts a success CQE; the container is reaped
  (`async_inflight` back to 0).
- `9p_client.loom_rights_deny` (Loom-3) — a read-only registered handle denies
  FSYNC at submit (`-EACCES` CQE) and **never dispatches** (the I-30 rights pin;
  `async_inflight` stays 0).
- `9p_client.loom_quiesce_abandons_inflight` (Loom-3, #898) — submit FSYNC
  without reaping (op in flight), then `loom_unref` tears the ring down:
  `loom_free` abandons the op (Tflush + pin clunked + container freed), the loom
  + the dev9p Spoor are freed exactly once, and a late reply pumped afterward is
  discarded ownerless (no UAF on the freed container).
- `9p_client.loom_multishot_stream` (Loom-5a) — one `LOOM_SQE_MULTISHOT` FSYNC SQE
  (`shot_limit=3`) drives 3 Tfsync/Rfsync round-trips in ONE `loom_enter(1,3,0)`:
  CQEs 0+1 set `LOOM_CQE_MORE`, CQE 2 clears it (the terminal); all echo the
  `user_data`; the single container is reaped after the terminal; the dev9p Spoor
  is freed exactly once (the pin held across all shots, released once);
  `overflow == 0` (`ExactlyOneTerminal` / `TerminalEndsStream` / `ObjPinnedAcrossShots`).
- `9p_client.loom_multishot_backpressure` (Loom-5a) — `cq_entries=2`, `shot_limit=4`:
  the first `loom_enter` fills the CQ with 2 MORE shots then the op HOLDS (rearm-
  pending, not reaped, `async_inflight==0`, `overflow==0` — the shot was held, not
  dropped); after userspace reaps both (advance `cq_head`), the second `loom_enter`
  re-arms + drains the remaining MORE shot + the terminal, `overflow` still 0
  (`CqNeverOverfull`, the no-`BUGGY_SHOT_LOST_ON_FULL` discipline).
- `9p_client.loom_link_cancel_cascade` (Loom-5b) — a chain `[FSYNC(read-only, LINK)]
  [NOP]`: the head fails inline (`-EACCES`, `DONE_FAIL`); the linked NOP successor is
  CANCELLED with exactly one `-ECANCELED` CQE and never dispatched (`LinkOrdered` +
  `EveryDoneOpPosted` + `NoOrphanCancel`).
- `9p_client.loom_link_success_ordering` (Loom-5b) — a chain `[FSYNC(write, LINK)]
  [NOP]`: the FSYNC's async Rfsync CQE is posted BEFORE the NOP CQE (CQE0 = FSYNC,
  CQE1 = NOP) — the linked successor admits only after the predecessor is done ok
  (`LinkAdmits`; a dropped gate would post the inline NOP first).
- `9p_client.loom_drain_barrier` (Loom-5b) — a chain `[FSYNC A][NOP DRAIN B][NOP C]`:
  A is a FAST async op (before the chain); the DRAIN B waits for A's async completion
  (`async_inflight -> 0`, the drain-self gate's catch for prior non-chain async ops)
  → CQE1 = B (after A, not inline-first), CQE2 = C after B (`DrainOrdered`). One prior
  async op, since the loopback test transport is single-in-flight.
- `9p_client.loom_independent_past_held` (Loom-5b) — a chain `[FSYNC A (LINK)][NOP B]
  [NOP C]`: B is A's held linked successor; C is independent (B does not link to it),
  so C admits IMMEDIATELY out of order while B is held — CQE0 = C (before A and B),
  then A, then B (the head→tail `continue`-not-`break` admission).
- `9p_client.loom_drain_waits_for_rearm_pending` (Loom-5b, the audit F1 regression) —
  a FAST `MULTISHOT` op (`cq=2`, `shot_limit=3`) left rearm-pending (back-pressured,
  `async_inflight==0` but its stream not done) + a `DRAIN` submitted in a NONBLOCK
  enter: the drain HOLDS (`cq_tail` stays 2 — pre-fix it would admit early to 3),
  then completes only after the multishot terminal once driven (`DrainOrdered`; the
  `rearm_pending` term in the drain gate).

The full `SYS_LOOM_SETUP` / `SYS_LOOM_ENTER` user copy-in/out is trivial handler
glue (E2E coverage lands with the native API at Loom-6); the payload-opcode
dispatch lands with Loom-6's registered-buffer surface.

---

## Error paths

`SYS_LOOM_SETUP` / `sys_loom_setup_for_proc` → -1 on: NULL out args; non-zero
`flags` (no flags supported at Loom-2a); `entries` 0 / not a power of two / over
`LOOM_MAX_ENTRIES`; `loom_create` OOM; `vma_find_gap` / `burrow_map` failure;
`handle_alloc` table-full (rolled back); a `params` copy-out fault (rolled back).
`SYS_LOOM_REGISTER` / `sys_loom_register_for_proc` → -1 on: NULL proc;
unsupported `op`; `nargs` over `LOOM_MAX_REG_HANDLES`; bad `loom_fd` (not
`KOBJ_LOOM`); any listed fd not a `KOBJ_SPOOR` handle (all already-ref'd Spoors
rolled back via `spoor_clunk`); a user-VA fault reading the fd array.

---

## Status

| Sub-chunk | What | State |
|---|---|---|
| Loom-2a | the ring substrate (this doc) | **landed** |
| Loom-2b | the pluggable-completion 9P-engine seam + async submit + `loom_post_cqe` | **landed** |
| Loom-3 | `SYS_LOOM_ENTER` + SQE dispatch (NOP/FSYNC; payload ops → -ENOSYS until Loom-6) + the I-30 submit-time pin + the async-op container + the `loom_free` quiesce (#898) | **landed** |
| Loom-4 | SQPOLL kthread (4a recv-deadline + 4b CQ wait-list + 4c kthread + 4d audit) | **landed** |
| Loom-5a | multishot streams (`LOOM_SQE_MULTISHOT` → `LOOM_CQE_MORE` shots + terminal) | **landed** |
| Loom-5b | LINK/DRAIN held-submission chain (`l->chain` + `loom_admit_chain`) | **landed** (Loom-5 audit closed 0/0/2/4) |
| Loom-6 | registered buffers + native libthyla-rs API + benchmark | pending |

## Known caveats / footguns

- **LINK/DRAIN concurrency contract (Loom-5b; audit F4).** The chain scheduler
  assumes *effectively one admitter per ring* — the SQPOLL kthread is the sole
  admitter on an SQPOLL ring, and a non-SQPOLL ring follows the io_uring
  single-producer SQ contract (one submitting thread). The chain is **memory-safe
  even under concurrent non-SQPOLL drivers** (the INFLIGHT claim under `l->lock`
  prevents UAF / double-dispatch / double-CQE), and the **cancel** leg is hardened
  (revert-to-HELD + retry, so a cancel is never lost). The residual under
  *concurrent* non-SQPOLL drivers is the inherited Loom-3 over-admit window
  (`loom_cq_ready + async_inflight` transiently exceeding `cq_entries` because the
  room check and the `async_inflight` bump are not atomic): a chain op's **terminal**
  CQE can be dropped — the chain still *continues* (its `chain->state` is set
  regardless), so only that op's completion *notification* is missed, the same
  bounded residual the Loom-3 close documented. The exact-concurrent-admission
  coordination is **OWED to Loom-6** with the deterministic two-thread-same-`loom_fd`
  SMP harness (carried since #841 / Loom-2b/3/4; TSan is also restored there). v1.0
  has no userspace Loom consumer, so concurrent chained ENTERs cannot fire it.
- **LINK group = single-producer submission (audit F6).** A LINK/DRAIN group must be
  submitted by ONE thread (the SQ single-producer contract). Under a hostile
  multi-thread submitter on one non-SQPOLL ring, `loom_drain_sq` can append a LINK
  group out of SQ-index order (the chain uses chain-order, not SQ-index, for the
  predecessor), mis-ordering that app's own links — self-harm, never a kernel-safety
  violation (no UAF: the chain ops are order-relative, not index-relative).
- **OOM degrades LINK ordering (audit F5).** If `kmalloc` of a `loom_chain_op` fails
  while enqueuing a LINK predecessor, the op posts `-ENOMEM` (not lost) but is not
  appended; its linked successor then routes as a fresh chain head and runs
  *without* waiting for / being cancelled by the failed predecessor. Best-effort
  under memory pressure; no safety issue (the predecessor's failure CQE still posts).
- A closed `loom_fd` whose user mapping was not detached leaves a zeroed RW
  region mapped (the `mapping_count` lingers until `proc_free` or an explicit
  `SYS_BURROW_DETACH`). Harmless (the dual-refcount keeps the pages alive; no
  UAF), and matches `SYS_BURROW_ATTACH`'s "the user owns unmapping" contract. A
  v1.x setup could track the VA for auto-unmap on close.
- `KObj_Loom`'s `handle_acquire_obj` branch **must** be `loom_ref` (not a no-op):
  it is reached via `handle_get`, not only `handle_dup`. A no-op there would
  underflow the refcount on the get/put pairing and free the ring early.
- **`p9_rpc.on_complete` runs under `c->lock`** (from `demux_frame_locked` AND
  `client_mark_dead_locked`) and MUST NOT sleep / re-enter `p9_client_*`. The
  Loom-3 production callback `loom_async_complete` honors this — it only posts a
  CQE + flags terminal + decrements `async_inflight` (all under `l->lock`); the
  pin clunk + container free happen outside the lock (the reap sweep / the
  `loom_free` quiesce). The container holds **no** loom ref, so the
  ref-drop-under-lock hazard is structurally absent.
- **The Loom-3 quiesce (#898) reads `inflight[tag] == rpc` under `c->lock` as
  authoritative.** A late original reply for an abandoned op is discarded
  ownerless (the Tflush reserved the tag); a racing demux that wins `c->lock`
  before the abandon posts at-most-one CQE into the still-allocated ring. The
  loom is freed only after the abandon loop, so this is never a UAF — but the
  per-op `c->lock`-serialized abandon is load-bearing: do not "optimize" it to a
  lock-free `terminal`-flag check.
- **`p9_client_reader_pump_once` precondition**: call it only when a reply is
  expected (≥1 in-flight async op). On a real transport recv blocks for a frame;
  on a synchronous test loopback the frame must already be staged — pumping an
  idle client recv's EOF and latches the session dead. `loom_enter`'s reap pumps
  only while in-flight async ops remain (`loom_first_inflight_client`).
- **Concurrent `SYS_LOOM_ENTER` on one ring**: at most one thread drives
  completions (the elected reader); a second concurrent enter whose pump finds
  another reader active returns what is posted (it may return < `min_complete`).
  The full multi-waiter coordination (a CQ waitqueue) lands with Loom-4 (SQPOLL).
  Single-threaded enter — the common case — blocks correctly via the reader's
  recv. A ring whose registered handles span multiple 9P clients should also use
  NONBLOCK + re-enter (the blocking wait pumps one client at a time); a
  single-FS ring is the v1.0 norm.
- **Async completions are reap-driven, not perpetually-read.** If the only
  in-flight ops are async and no thread pumps the reader, their replies sit
  un-demuxed (the handoff skips them). This is the io_uring model — userspace
  reaps by `SYS_LOOM_ENTER` / the SQPOLL kthread (Loom-4). Not a hang: nothing is
  lost, the completion just waits for a pump (or the `loom_free` quiesce).
