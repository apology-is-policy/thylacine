# 107 — Loom: the io_uring-inverted 9P ring transport

**Design**: `docs/LOOM.md` (signed off 2026-06-05). **Spec**: `specs/loom.tla`
(TLC-green; gates the impl). **Invariants**: ARCH §28 I-29 (completion
integrity) + I-30 (submit-time capability pin). **Audit-trigger surface**: ARCH
§25.4 + CLAUDE.md.

> **Status (Loom-2a + Loom-2b + Loom-3):** the ring **substrate** (2a —
> `KObj_Loom`, the SQ/CQ ring memory + geometry, `SYS_LOOM_SETUP`, the
> registered-handle table via `SYS_LOOM_REGISTER`, the kobj refcount lifecycle),
> the **pluggable-completion 9P-engine seam** (2b — `p9_rpc.on_complete`,
> `p9_client_submit_async`, `p9_client_reader_pump_once`, the CQ writer
> `loom_post_cqe`), and the **batch-enter core** (3 — `SYS_LOOM_ENTER`, the
> SQE→`p9_client_*` dispatch, the submit-time capability pin I-30, the production
> async-op container, and the `loom_free` quiesce-before-free, #898). v1.0
> dispatches the no-payload opcodes (`NOP` / `FSYNC`); the payload opcodes
> (READ/WRITE/GETATTR/...) land with Loom-6's registered-buffer surface and post
> `-ENOSYS` until then. SQPOLL is Loom-4; multishot + LINK/DRAIN is Loom-5;
> registered buffers + the native libthyla-rs API + the benchmark are Loom-6.

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
  `struct loom_params`; IN `params.flags` (`LOOM_SETUP_*`, must be 0 at
  Loom-2a); OUT the ring geometry. Allocates the ring Burrow, maps it RW into
  the caller (the burrow-attach window), installs a `KObj_Loom` handle, returns
  the fd.
- `SYS_LOOM_REGISTER(loom_fd, op, arg_va, nargs) → 0 / -1` (=67). `op`:
  `LOOM_REGISTER_HANDLES` (install the fixed-handle table) at Loom-2a;
  `LOOM_REGISTER_BUFFERS` reserved (Loom-6). `arg_va`: a `u32[nargs]` of fds
  (each must be `KOBJ_SPOOR` in the caller). Replaces the whole table
  (`IORING_REGISTER_FILES` semantics).
- `SYS_LOOM_ENTER(loom_fd, to_submit, min_complete, flags) → n / -1` (=68,
  Loom-3). Consume up to `to_submit` SQEs (SQ-index order), dispatch each, then —
  if `min_complete > 0` and not `LOOM_ENTER_NONBLOCK` — drive the elected reader
  (blocking on recv, death-interruptible #811) until ≥ `min_complete` CQEs are
  available or no async op remains in flight; finally reap completed-op
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

`loom_enter` (`kernel/loom.c`) is the batch-enter core. **Submit phase**: it
reads the user-owned `sq_tail` (advisory — bounded, never trusted for indexing),
then for each of up to `to_submit` slots reads the SQ-index ring at the
**kernel-private** `sq_head & (sq_entries-1)` (the SA-1 discipline on the SQ
side), range-checks the user-written indirection (`>= sq_entries` → bump
`dropped`, never index `sqes[]`), and **copies the whole SQE to a kernel-local
`struct loom_sqe`** (ring TOCTOU, §6.1) before acting only on the copy.
`loom_submit_one` dispatches the copy:

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
the blocking wait *is* the elected reader's `recv` — `loom_enter` pumps
`p9_client_reader_pump_once` on the client of the first in-flight op until
≥ `min_complete` CQEs are posted or `async_inflight` hits 0 (no more completions
possible) or the pump returns 0 (another reader active — Loom-4/SQPOLL adds the
shared-reader wait; a concurrent enter returns what is posted) or < 0 (session
dead / death-interrupt #811). Then `loom_reap_terminal` unlinks every terminal op
(under `l->lock`) and releases its pin + frees the container (outside the lock).

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

---

## Data structures

`struct Loom` (`kernel/loom.c`): `magic` (`LOOM_MAGIC`, offset 0 — SLUB-free UAF
defense), atomic `refcount`, `lock` (protects `reg[]` + `cq_tail` + `sq_head` +
`inflight_ops` + `async_inflight`; geometry is immutable post-setup), `ring` (the
Burrow, `handle_count` ref held), `ring_kva` (cached direct-map base), the
geometry fields (mirror `loom_params`), the kernel-private `cq_tail` (Loom-2b) +
`sq_head` (Loom-3) authoritative ring indices (the shared-header copies are
userspace-readable mirrors — the SA-1 discipline), `inflight_ops` + `async_inflight`
(Loom-3, the in-flight async-op list), and `reg[LOOM_MAX_REG_HANDLES]`. The ABI
structs are byte-pinned by `_Static_assert` (sqe 64, cqe 16, hdr 64, params 88).

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

### Substrate (`kernel/test/test_loom.c`, 14 cases)

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

### The seam + Loom-3 engine dispatch (`kernel/test/test_9p_client.c`, 6 cases)

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
| Loom-4 | SQPOLL kthread | pending |
| Loom-5 | multishot + LINK/DRAIN | pending |
| Loom-6 | registered buffers + native libthyla-rs API + benchmark | pending |

## Known caveats / footguns

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
