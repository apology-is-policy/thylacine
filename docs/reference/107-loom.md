# 107 — Loom: the io_uring-inverted 9P ring transport

**Design**: `docs/LOOM.md` (signed off 2026-06-05). **Spec**: `specs/loom.tla`
(TLC-green; gates the impl). **Invariants**: ARCH §28 I-29 (completion
integrity) + I-30 (submit-time capability pin). **Audit-trigger surface**: ARCH
§25.4 + CLAUDE.md.

> **Status (Loom-2a):** the ring **substrate** — `KObj_Loom`, the SQ/CQ ring
> memory + geometry, `SYS_LOOM_SETUP`, the registered-handle table
> (`SYS_LOOM_REGISTER`), and the kobj refcount lifecycle. **No op flows yet.**
> The pluggable-completion 9P-engine seam is Loom-2b; `SYS_LOOM_ENTER` + the
> SQE→`p9_client_*` dispatch + the submit-time pin + the CQE post are Loom-3;
> SQPOLL is Loom-4; multishot + LINK/DRAIN is Loom-5; registered buffers + the
> native libthyla-rs API + the benchmark are Loom-6.

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
- `SYS_LOOM_ENTER` (=68) — **reserved**, lands at Loom-3.

### Kernel-internal (`kernel/loom.c`, declared in `loom.h`)

```c
struct Loom *loom_create(u32 sq_entries, u32 cq_entries);
void loom_ref(struct Loom *l);
void loom_unref(struct Loom *l);   // last drop: clunk regs + burrow_unref + free
int  loom_register_handles(struct Loom *l, struct Spoor **spoors,
                           const rights_t *rights, u32 n);
u64  loom_total_created(void);
u64  loom_total_destroyed(void);
// the testable syscall inners (kernel/syscall.c):
int  sys_loom_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                             struct loom_params *out, hidx_t *out_fd);
int  sys_loom_register_for_proc(struct Proc *p, hidx_t loom_fd, u32 op,
                                const hidx_t *fds, u32 n);
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

---

## Data structures

`struct Loom` (`kernel/loom.c`): `magic` (`LOOM_MAGIC`, offset 0 — SLUB-free UAF
defense), atomic `refcount`, `lock` (protects `reg[]`; geometry is immutable
post-setup), `ring` (the Burrow, `handle_count` ref held), `ring_kva` (cached
direct-map base), the geometry fields (mirror `loom_params`), and
`reg[LOOM_MAX_REG_HANDLES]`. The ABI structs are byte-pinned by `_Static_assert`
(sqe 64, cqe 16, hdr 64, params 88).

`KObj_Loom` (`kernel/include/thylacine/handle.h`): `KOBJ_LOOM = 10`, a **fourth**
partition (`KOBJ_KIND_LOOM_MASK`) — non-transferable + non-hardware + non-srv. A
ring is meaningless to another Proc (its VA + registered Spoors are Proc-local),
so it is never transferable and never dup-able (`handle_dup` rejects it).

---

## Spec cross-reference

`specs/loom.tla` models the op-lifecycle (`empty → posted → snap → inflight →
completed → cqd → reaped`, `+ abandoned` on teardown). Loom-2a implements the
spec's substrate: the registered-object table (`UserRegister`), the ring + CQ
(`cq`), and teardown's quiesce obligation (`Teardown`). I-29 (`NoDoubleCompletion`
/ `NoStaleCompletion` / `CqNeverOverfull` / `EventuallyCompletes`) + I-30
(`ArgPinnedToSnapshot` / `ObjPinnedToSnapshot` / `ActedUnderAdmittedRights`) are
enforced by the dispatch landing at Loom-2b/3. See `specs/SPEC-TO-CODE.md`.

---

## Tests (`kernel/test/test_loom.c`, 8 cases)

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

The full `SYS_LOOM_SETUP` user copy-in/out is trivial handler glue (E2E coverage
lands with the native API at Loom-6); the SQE dispatch + CQE post arrive at
Loom-3.

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
| Loom-2b | the pluggable-completion 9P-engine seam + async submit | pending |
| Loom-3 | `SYS_LOOM_ENTER` + dispatch + submit-time pin + CQE post | pending |
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
