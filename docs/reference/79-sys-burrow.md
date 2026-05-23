# 79 — `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` (P6-pouch-mem-a)

The v1.0 native anonymous-memory interface — the userspace primitive for growing a Proc's address space at runtime. `SYS_BURROW_ATTACH` attaches an anonymous, RW, demand-zero Burrow at a kernel-chosen virtual address; `SYS_BURROW_DETACH` tears one down. Together they are the substrate for `libt`'s and pouch's `malloc`.

Per `ARCHITECTURE.md §6.5` (the two-tier native memory interface — these two syscalls are "Tier 1") + `§11.2` (the syscall table). Invariant `I-7` (the BURROW dual-refcount lifecycle; `specs/burrow.tla`). Audit-trigger surface (`CLAUDE.md` / `ARCHITECTURE.md §25.4`): `kernel/syscall.c` (the handlers), `kernel/burrow.c`, `kernel/vma.c`.

---

## Purpose — the two-tier memory model

`ARCHITECTURE.md §6.5` splits Thylacine's native memory interface into two tiers — an honest distinction Plan 9 conflated into "segments":

- **Tier 1 — anonymous regions.** Private heap memory. No handle: the region's owner is its VMA, its name is its address (exactly as Plan 9's `sbrk` / `segattach` returned raw addresses). `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` *are* Tier 1 — the substrate a `malloc` rests on.
- **Tier 2 — Burrow handles.** Shared / named / transferable memory: a Burrow held as a `KObj_Burrow` handle (`ARCHITECTURE.md §11.3`). Deferred — lands when a native workload needs shared memory.

`brk` is deliberately not provided (a single process-global linear cursor is ASLR-hostile, thread-hostile, single-arena). File-backed `mmap` is deliberately refused (a mapped file cannot be 9P-network-transparent — Plan 9's refusal, kept as a Thylacine conviction). See `ARCHITECTURE.md §6.5` for the full rationale.

The native interface was designed first, on its own terms; pouch then translates POSIX `mmap(MAP_ANONYMOUS)` / `munmap` onto it (the `0003-pouch-mman` musl patch — sub-chunk 7b).

---

## Syscall ABI

```c
// kernel/include/thylacine/syscall.h
SYS_BURROW_ATTACH = 37
SYS_BURROW_DETACH = 38
#define BURROW_ATTACH_MAX  (256u * 1024u * 1024u)   // 256 MiB
```

### `SYS_BURROW_ATTACH(length) → vaddr`

| Register | Meaning |
|---|---|
| `x0` | `length` — bytes; `1 .. BURROW_ATTACH_MAX`. Rounded up to a `PAGE_SIZE` multiple. |
| `x8` | syscall number = 37 |
| `x0` (return) | the page-aligned base VA (`≥ EXEC_USER_BURROW_BASE`, so never negative) on success; `-1` on any rejection |

The region is RW, demand-zero, and placed at a kernel-chosen address inside the burrow-attach window (see below). Userspace never chooses the address — there is no `MAP_FIXED`.

**Failure modes** (`-1`):

| Cause | Mechanism |
|---|---|
| `length == 0` | explicit check |
| `length > BURROW_ATTACH_MAX` | explicit check (also bounds the page-rounding so `length + PAGE_SIZE` cannot overflow) |
| no free range of the rounded length in the burrow window | `vma_find_gap` returns -1 |
| `burrow_create_anon` OOM | SLUB / buddy exhaustion |
| `burrow_map` failure (overlap, SLUB OOM for the `Vma`) | defensive — a correct `vma_find_gap` never produces an overlap |

### `SYS_BURROW_DETACH(vaddr, length) → 0 / -1`

| Register | Meaning |
|---|---|
| `x0` | `vaddr` — the base VA a prior `SYS_BURROW_ATTACH` returned |
| `x1` | `length` — the attached length: the original request, or any value that page-rounds to the same span |
| `x8` | syscall number = 38 |
| `x0` (return) | `0` on success; `-1` on any rejection |

`(vaddr, rounded length)` must match an installed VMA **exactly** — no partial detach at v1.0 (mirrors `burrow_unmap`'s existing constraint). The VMA is removed and the Burrow's pages are freed.

**Failure modes** (`-1`): `length == 0`; `length > BURROW_ATTACH_MAX`; `vaddr` not page-aligned; `vaddr` outside `[EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP)`; no VMA matches `[vaddr, vaddr + round_up(length))` exactly (wrong base, wrong length, or already detached).

**Detach is window-confined.** `SYS_BURROW_DETACH` rejects any `vaddr` outside `[EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP)` before it touches the VMA list (P6-pouch-mem-a audit, F1). Without that bound, `burrow_unmap` — which matches a VMA by geometry alone — would let a caller pass the coordinates of its own ELF-segment, stack, or stack-guard VMA and have it dismantled; the stack-guard case silently retires a security-relevant page. Every `burrow_attach` region lives in the window and every ELF / stack / guard VMA sits below it (`_Static_assert`'d in `exec.h`), so the bound structurally excludes them. A driver that deliberately placed an MMIO/DMA mapping *inside* the window could still detach it by these coordinates — that is the driver's own resource and self-harm only, not an isolation violation.

---

## The burrow-attach window

`SYS_BURROW_ATTACH` places regions in a fixed user-VA window, defined in `kernel/include/thylacine/exec.h`:

```c
#define EXEC_USER_BURROW_BASE   0x0000000100000000ull   // 4 GiB
#define EXEC_USER_BURROW_TOP    0x0000400000000000ull   // 64 TiB
```

The base (4 GiB) sits well above the user stack TOP (`EXEC_USER_STACK_TOP` = `0x8000_0000`, 2 GiB), so an attached region can never collide with the ELF image (low VAs), the stack, or the stack guard page. The top (64 TiB) is well under `USER_VA_TOP` (`2^47` = 128 TiB), the hard ceiling `burrow_map` enforces; the headroom above the window is reserved for future Tier-2 placement.

---

## Kernel implementation

### Handlers (`kernel/syscall.c`)

Each SVC handler is a thin `current_thread()` wrapper over a non-static `_for_proc` inner — the testable core (the `sys_pipe_for_proc` pattern):

```c
static s64 sys_burrow_attach_handler(u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t) return -1;
    return sys_burrow_attach_for_proc(t->proc, length_raw);
}
```

`sys_burrow_attach_for_proc(p, length_raw)`:

1. Reject `length_raw == 0` and `length_raw > BURROW_ATTACH_MAX` (the cap check is **before** rounding, so `length_raw + PAGE_SIZE - 1` cannot overflow).
2. Round up to a page multiple.
3. `spin_lock(&p->vma_lock)`.
4. `vma_find_gap` — first-fit a free VA in `[EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP)`.
5. `burrow_create_anon(length)` — `handle_count = 1` (the construction reference), `mapping_count = 0`.
6. `burrow_map(p, b, vaddr, length, VMA_PROT_RW)` — installs the VMA; `vma_alloc` → `burrow_acquire_mapping` takes `mapping_count → 1`.
7. `burrow_unref(b)` — drops the construction handle: `handle_count → 0`. `mapping_count = 1` keeps the Burrow alive — this is the `exec.c` Tier-1 discipline (no handle; the VMA owns the Burrow). On a `burrow_map` failure the construction handle is the *only* reference; `burrow_unref` then frees the Burrow (`mapping_count` still 0).
8. `spin_unlock(&p->vma_lock)`; return the base VA.

`sys_burrow_detach_for_proc(p, vaddr_raw, length_raw)` validates + page-rounds, then under `p->vma_lock` calls `burrow_unmap(p, vaddr_raw, length)` — `vma_remove` + `vma_free` → `burrow_release_mapping` → `mapping_count → 0` with `handle_count` already 0 → the pages are freed.

### `vma_find_gap` (`kernel/vma.c`)

First-fit free-range finder. The per-Proc VMA list is sorted by `vaddr_start` ascending, so a single forward pass — advancing a candidate base past every VMA that blocks it — finds the lowest gap of `length` bytes in `[window_start, window_end)`:

```c
int vma_find_gap(struct Proc *p, u64 length,
                 u64 window_start, u64 window_end, u64 *out_vaddr);
```

A VMA entirely at/below the candidate does not constrain it; a VMA starting at/after `window_end` (and, by sort order, every later one) is irrelevant — the scan stops. Every comparison is subtraction guarded by an ordering check, so no `cand + length` sum is ever formed — overflow-free across the `2^47` user-VA space. Returns `0` + `*out_vaddr` on success, `-1` on no-fit or a constraint violation (zero / unaligned length, unaligned or inverted window). The caller holds `Proc.vma_lock` across `vma_find_gap` *and* the subsequent `burrow_map` — the same single-mutator discipline `vma_insert` / `vma_remove` already assume.

### The per-Proc VMA lock

`struct Proc` gained a `spin_lock_t vma_lock` field (`kernel/include/thylacine/proc.h`; `struct Proc` `_Static_assert` 144 → 152). `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` hold it across their VMA-list work, so the find-gap + `vma_insert` (attach) and the lookup + `vma_remove` (detach) sequences are atomic against each other.

At v1.0 Procs are single-threaded, so the lock is uncontended by construction — it does no functional work yet. It is in place so the surface is correct-by-construction when the `pouch-threads` sub-chunk makes Procs multi-threaded; **that** sub-chunk extends the lock's coverage to the remaining VMA mutators (`exec_setup`'s `burrow_map` calls, `vma_drain`) and the page-fault `vma_lookup` reader. Plain `spin_lock` (not `spin_lock_irqsave`): no IRQ handler touches a Proc's VMA list, and the critical section never sleeps — `burrow_create_anon` / `vma_alloc` are non-blocking allocations (NULL on OOM, never wait). Lock order: `p->vma_lock` → the buddy / SLUB locks taken inside `burrow_create_anon` / `vma_alloc` (no inversion — the allocators never reach back into Proc/VMA code). `KP_ZERO` at `proc_alloc` / `proc_init` zero-inits the field to the valid unlocked state (`SPIN_LOCK_INIT == (spin_lock_t){0}`).

### Eager allocation caveat

`burrow_create_anon` allocates the backing pages **eagerly** via `alloc_pages` (power-of-two page rounding — see `20-burrow.md`). So `SYS_BURROW_ATTACH` commits real, zeroed physical RAM up front; only the PTEs are installed on demand (the user page-fault path). A `length` just over a power-of-two-of-pages wastes up to ~2× in the rounding. This is a pre-existing `burrow_create_anon` property (`burrow.h` documents it as acceptable for v1.0; per-page lazy allocation is post-v1.0). `BURROW_ATTACH_MAX` (256 MiB) bounds a single attach well inside the buddy allocator's order-18 (1 GiB) ceiling.

---

## libt wrappers

`libt` — the native-C library — will expose thin wrappers over the two syscalls:

```c
// usr/lib/libt/include/thyla/syscall.h  (added with the first native consumer)
long t_burrow_attach(unsigned long length);                      // vaddr / -1
long t_burrow_detach(unsigned long vaddr, unsigned long length);  // 0 / -1
```

These are **deferred**: P6-pouch-mem-a ships the kernel ABI (the syscalls) — that *is* the native interface. The `libt` C convenience shim lands when the first native (non-pouch) C program needs it; there is no such consumer yet. pouch does **not** use `libt` — pouch's lower half issues `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` directly through musl's syscall seam (the `0003-pouch-mman` patch, sub-chunk 7b — **landed**; `docs/reference/78-pouch.md` "The anonymous-memory backend").

---

## Tests

### `vma_find_gap` (`kernel/test/test_vma.c`)

| Test | Coverage |
|---|---|
| `vma.find_gap_smoke` | empty list → window base; a sub-window VMA is ignored; a base VMA pushes the gap past it; a 2-page hole between two VMAs is found; first-fit takes the lowest gap |
| `vma.find_gap_no_fit` | window smaller than the request → -1; a window-spanning VMA → -1; a 2-page request cannot fit a 1-page tail, but a 1-page request takes it |
| `vma.find_gap_constraints` | NULL Proc / NULL out / zero length / unaligned length / unaligned or inverted window → -1 |

### `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` (`kernel/test/test_sys_burrow.c`)

Drive the `_for_proc` inners on a fresh `proc_alloc`'d Proc.

| Test | Coverage |
|---|---|
| `sys_burrow.attach_returns_window_va` | attach installs an RW VMA in the window; the Burrow has `mapping_count` 1, `handle_count` 0 (Tier 1) |
| `sys_burrow.attach_detach_round_trip` | detach removes the VMA + frees exactly one Burrow; the freed range is reused by the next attach |
| `sys_burrow.attach_distinct` | repeated attaches yield distinct, non-overlapping, in-window VMAs |
| `sys_burrow.attach_rounds_up` | a `PAGE_SIZE + 1` request rounds up to a 2-page span; detach with the un-rounded length still matches |
| `sys_burrow.attach_rejects_bad_length` | length 0 / `> BURROW_ATTACH_MAX` / NULL Proc → -1; no rejected call installs a VMA |
| `sys_burrow.detach_rejects` | wrong base / wrong length / unaligned base / zero length / double-detach → -1; the VMA survives every rejected detach |

Suite: 538 → 549 (11 new tests: 7 `sys_burrow.*` + 4 `vma.find_gap_*`), **549/549 PASS × default + UBSan**.

---

## Audit-bearing posture

Per `CLAUDE.md` / `ARCHITECTURE.md §25.4` — this is an mm-surface change. A focused opus-prosecutor round (P6-pouch-mem-a) covered the two syscalls, `vma_find_gap`, the per-Proc lock, and the test coverage before merge.

| Surface touched | Why |
|---|---|
| `kernel/syscall.c` (entry points) | New syscalls — capability / argument validation |
| `kernel/burrow.c`, `kernel/vma.c` | VMA + BURROW refcount lifecycle (I-7); VA placement; W^X (RW-only) |

No new TLA+ module — the lifecycle is already pinned by `specs/burrow.tla` (`I-7`); the two syscalls are a thin skin over the audited `burrow_create_anon` → `burrow_map` → `burrow_unref` discipline.

---

## v1.x extensions

| Feature | Notes |
|---|---|
| Tier 2 — handle-backed Burrows | `KObj_Burrow` create / map / transfer for shared / named memory (`ARCHITECTURE.md §11.3`). |
| Per-region address randomization | v1.0 `vma_find_gap` is deterministic first-fit; randomizing the placement within the window is a hardening pass. |
| Partial detach | v1.0 detach is exact-match; splitting a VMA on a sub-range detach is post-v1.0 (mirrors `burrow_unmap`). |
| Lazy (per-page) backing | `burrow_create_anon`'s eager power-of-two allocation → true demand-zero per-page allocation. |
| W↔X for JIT | `SYS_BURROW_ATTACH` is RW-only; an explicit `pkey`-shaped W↔X transition (`ARCHITECTURE.md §6.6`) is the planned JIT path. |
