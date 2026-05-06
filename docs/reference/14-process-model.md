# 14 — Process model bootstrap (as-built reference)

P2-A's deliverable: the kernel's first process descriptor (`struct Proc`), thread descriptor (`struct Thread`), saved-register context (`struct Context`), context-switch primitive (`cpu_switch_context`), trampoline (`thread_trampoline`), and the C-level wrapper (`thread_switch`). After P2-A the kernel can describe processes/threads, allocate them, switch CPU control between them, and observe state transitions — without yet having a scheduler. P2-B layers EEVDF on top.

Scope: `kernel/include/thylacine/proc.h`, `kernel/include/thylacine/thread.h`, `kernel/include/thylacine/context.h`, `kernel/proc.c`, `kernel/thread.c`, `arch/arm64/context.S`, `kernel/test/test_context.c`, `specs/scheduler.tla` (sketch).

Reference: `ARCHITECTURE.md §7.2` (Proc), `§7.3` (Thread), `§7.5` (TLS via TPIDR_EL0), `§8` (Scheduler — design intent for what P2-B builds), `§28` invariants (I-8, I-9, I-17, I-18 — pinned by `scheduler.tla` at progressive levels of fidelity).

---

## Purpose

A real OS multitasks. Phase 1 brought up exactly one execution context — the boot CPU running boot_main on the boot stack. P2-A introduces the kernel's first Proc + Thread descriptors and the assembly that swaps register state between two threads. P2-B will add the EEVDF dispatcher on top of P2-A's primitives.

The discipline P2-A pins, beyond just "things compile":

- **Layout invariants**: `struct Context` is exactly 112 bytes with named offsets that the asm switcher hardcodes. Field reorder without an asm update is impossible without a static-assert build break.
- **Bootstrap order**: `slub_init → proc_init → thread_init`. `current_thread()` returns NULL before `thread_init`; from then on, valid until the kernel exits.
- **Per-CPU current_thread**: parked in `TPIDR_EL1` (the OS-reserved register), so the accessor is a single `mrs` and naturally per-CPU on SMP without extra plumbing.
- **BTI / PAC discipline**: `cpu_switch_context` is reached via `bl` from C — per ARM ARM D24.2.1, BL is a direct branch and sets `PSTATE.BTYPE = 0b00` (no constraint); `bti c` passes. A future indirect-call site reaching `cpu_switch_context` via `blr` would set `BTYPE = 0b10` (call), which `bti c` also matches. `thread_trampoline` is reached via `ret` from `cpu_switch_context` (`BTYPE = 0b00`) and also starts with `bti c` for defense in depth. Fresh threads enter via the trampoline's `blr x21` (`BTYPE = 0b10`), matching the entry function's own `bti c` prologue.
- **Spec-first sketch**: `specs/scheduler.tla` is TLC-clean at this sketch level. P2-B refines it (EEVDF deadline math, wait/wake atomicity, IPI ordering); P2-C adds work-stealing fairness.

---

## Public API

### `<thylacine/proc.h>`

```c
enum proc_state {
    PROC_STATE_INVALID = 0,
    PROC_STATE_ALIVE   = 1,
    PROC_STATE_ZOMBIE  = 2,                       // exits()'d, awaiting wait_pid
};

struct Proc {
    u64               magic;                     // PROC_MAGIC
    int               pid;
    int               thread_count;
    enum proc_state   state;                     // P2-Da
    int               exit_status;               // P2-Da: 0 = clean, non-zero = error
    struct Thread    *threads;                   // doubly-linked list head
    struct Proc      *parent;                    // P2-Da
    struct Proc      *children;                  // P2-Da: list head, chained via sibling
    struct Proc      *sibling;                   // P2-Da: next in parent's children
    const char       *exit_msg;                  // P2-Da: caller-owned, lifetime ≥ wait_pid
    struct Rendez     child_done;                // P2-Da: parent's wait Rendez
};

void          proc_init(void);                  // bootstraps kproc (PID 0)
struct Proc  *kproc(void);                      // accessor for kproc
struct Proc  *proc_alloc(void);                 // SLUB-allocate; returns NULL on OOM
void          proc_free(struct Proc *p);

// P2-Da: rfork (Plan 9 process/thread creation primitive)
int           rfork(unsigned flags,
                    void (*entry)(void *), void *arg);
__attribute__((noreturn))
void          exits(const char *msg);
int           wait_pid(int *status_out);

#define RFPROC      0x0001    // create new Proc (only RFPROC supported at P2-Da)
#define RFMEM       0x0002    // share address space (Phase 5+)
#define RFNAMEG     0x0004    // share namespace (P2-E)
#define RFFDG       0x0008    // share fd table (P2-F)
#define RFCRED      0x0010    // share credentials (Phase 5+)
#define RFNOTEG     0x0020    // share note queue (Phase 5+)
#define RFNOWAIT    0x0040    // detach from parent's children (Phase 5+)
#define RFREND      0x0080    // share rendezvous space (Phase 5+)
#define RFENVG      0x0100    // share environment (Phase 5+)

u64           proc_total_created(void);
u64           proc_total_destroyed(void);
```

`proc_init` must be called once after `slub_init`. It allocates kproc (PID 0), zero-initializes it, sets state=ALIVE, initializes child_done, and sets `g_next_pid = 1`.

`proc_alloc` returns a fresh Proc with state=ALIVE, child_done initialized, parent/children/sibling NULL. The caller wires linkage (rfork does this).

`proc_free` extincts on:
- `p == NULL` — programming error.
- `p == kproc()` — kproc is permanent.
- `p->thread_count != 0` — the caller must drain threads first.
- `p->threads != NULL` — same as above, expressed structurally.
- `p->children != NULL` — caller must reap or re-parent children first.
- `p->state != PROC_STATE_ZOMBIE` — lifecycle violation (free of ALIVE Proc).

#### `rfork(flags, entry, arg)` (P2-Da)

Creates a new Proc with one initial Thread running `entry(arg)`. At v1.0, only `RFPROC` is supported; other flags trigger an extinction. Subsequent sub-chunks add the resource-sharing flags as their respective subsystems land:

- **RFPROC** — required for "create"; allocates a new Proc + initial Thread.
- **RFNAMEG** (P2-E) — share parent's namespace instead of cloning.
- **RFFDG** (P2-F) — share fd table.
- **RFMEM** (Phase 5+) — share address space (creates a *thread* in current Proc).
- **RFCRED**, **RFNOTEG**, **RFNOWAIT**, **RFREND**, **RFENVG** — Phase 5+ with the syscall surface.

`entry` is the kernel function the new Thread will run; `arg` is passed in x0 via the trampoline's `mov x0, x20` (P2-Da extension to the existing `blr x21`). Returns the child's PID on success, -1 on OOM.

The kernel-internal call signature (entry takes `void *arg`, returns void) is the v1.0 P2-Da form. When P2-G lands the ELF loader and userspace, `rfork` extends to the syscall split — parent gets the child PID, child gets 0 — via a register-set tweak in the syscall-return path.

#### `exits(msg)` (P2-Da)

Terminates the calling process. `msg` is a status string captured by reference into `Proc->exit_msg` ("ok" → exit_status 0; anything else → exit_status 1; caller-owned lifetime, typically a string literal). Steps:
1. Re-parent any orphan children to kproc.
2. Set Proc state=ZOMBIE, exit_status, exit_msg.
3. Mark calling Thread state=EXITING — sched() leaves it out of the run tree.
4. Wake parent's `child_done` Rendez.
5. Yield via `sched()`. Never returns.

At v1.0 P2-Da, `exits` requires `thread_count == 1` (single-thread Procs only). Multi-threaded Procs require IPI-based termination of sibling threads (Phase 5+).

#### `wait_pid(*status_out)` (P2-Da)

Reaps a zombie child. Blocks on the parent's `child_done` Rendez until any child enters ZOMBIE state, then:
1. Unlinks the zombie from parent's children list.
2. Copies `exit_status` to `*status_out` (if non-NULL).
3. `thread_free`s the zombie's Thread (EXITING state accepted) — also releases the kstack pages.
4. `proc_free`s the zombie Proc.
5. Returns the zombie's PID.

Returns -1 if the parent has no children at all (live or zombie) — never blocks indefinitely with no possible wakeup.

The cond predicate for the Rendez sleep is "any child in ZOMBIE state OR no children" — the second clause covers the edge case where children exit between the loop's outer scan and the sleep. `scheduler.tla`'s `WaitOnCond`/`WakeAll` discipline (NoMissedWakeup invariant) covers the wait/wake atomicity here at the spec level.

### `<thylacine/thread.h>`

```c
enum thread_state {
    THREAD_STATE_INVALID = 0,    // zero-initialized; not usable
    THREAD_RUNNING       = 1,
    THREAD_RUNNABLE      = 2,
    THREAD_SLEEPING      = 3,    // P2-B
    THREAD_EXITING       = 4,    // Phase 2 close
};

struct Thread {
    int                tid;
    enum thread_state  state;
    struct Proc       *proc;
    struct Context     ctx;
    void              *kstack_base;
    size_t             kstack_size;
    struct Thread     *next_in_proc;
    struct Thread     *prev_in_proc;
};

#define THREAD_KSTACK_SIZE         (16 * 1024)             // usable
#define THREAD_KSTACK_GUARD_SIZE   (16 * 1024)             // no-access
#define THREAD_KSTACK_TOTAL_SIZE   (THREAD_KSTACK_SIZE + THREAD_KSTACK_GUARD_SIZE)
#define THREAD_KSTACK_TOTAL_ORDER  3                        // 8 pages
#define THREAD_KSTACK_GUARD_PAGES  4                        // bottom 4 pages

static inline struct Thread *current_thread(void);              // mrs TPIDR_EL1
static inline void           set_current_thread(struct Thread *);  // msr TPIDR_EL1

void           thread_init(void);                           // bootstraps kthread (TID 0)
struct Thread *kthread(void);                               // accessor
struct Thread *thread_create(struct Proc *, void (*entry)(void));
void           thread_free(struct Thread *t);
void           thread_switch(struct Thread *next);
u64            thread_total_created(void);
u64            thread_total_destroyed(void);
```

`thread_init` must be called once after `proc_init`. It allocates kthread (TID 0), zero-initializes it, attaches to kproc, and parks it in TPIDR_EL1 as the boot CPU's current thread. From this point `current_thread()` is valid.

`thread_create` allocates a Thread descriptor + 16 KiB kernel stack and seeds the saved context so the first switch-into the new thread lands at `thread_trampoline`, which then `blr`s `entry`. Returns NULL on OOM (Thread alloc fail or kstack alloc fail; cleanup is internal).

`thread_free` extincts on:
- `t == NULL` — programming error.
- `t == kthread()` — kthread is permanent.
- `t == current_thread()` — can't free yourself.
- `t->state == THREAD_RUNNING` — same as above expressed structurally; running threads aren't freeable.

`thread_switch` extincts on:
- `current_thread() == NULL` — `thread_init` not called.
- `next == NULL` — programming error.
- `next->state == THREAD_EXITING` — can't switch into a doomed thread.

`thread_switch` does NOT extinct on `prev == next`; it returns immediately as a no-op.

### `<thylacine/context.h>`

```c
struct Context {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    u64 fp;          // x29 — frame pointer (callee-saved by AAPCS64)
    u64 lr;          // x30 — link / resume PC
    u64 sp;          // stack pointer
    u64 tpidr_el0;   // EL0 TLS pointer (kernel threads: 0; userspace: per-thread)
};

void          cpu_switch_context(struct Context *prev, struct Context *next);
extern void   thread_trampoline(void);
```

`cpu_switch_context` saves the AAPCS64 callee-saved registers + SP + LR + TPIDR_EL0 from the live CPU into `prev`; loads the same fields from `next` into the CPU; rets to the loaded LR. Pure asm; from C's view it's a normal function call (caller-saved registers are clobbered, void return).

The C-level state bookkeeping (current_thread pointer, state field updates) is the caller's responsibility — `thread_switch` is the canonical wrapper that does both.

`thread_trampoline` is the initial `ctx.lr` for fresh threads. cpu_switch_context's `ret` lands here; trampoline `blr`s the entry function pointer parked in x21. If entry returns, the trampoline halts on WFE — Phase 2 close adds `thread_exit` + reap.

---

## Implementation

### `arch/arm64/context.S`

Hand-written assembly. ~50 lines. Two symbols: `cpu_switch_context`, `thread_trampoline`.

`cpu_switch_context`:

```
bti c                           // BL → BTYPE=00 (common); BLR → BTYPE=10; bti c passes both
stp x19, x20, [x0, #0]
stp x21, x22, [x0, #16]
stp x23, x24, [x0, #32]
stp x25, x26, [x0, #48]
stp x27, x28, [x0, #64]
stp x29, x30, [x0, #80]         // FP, LR
mov x9, sp
str x9, [x0, #96]               // SP (stp can't source SP directly)
mrs x9, tpidr_el0
str x9, [x0, #104]              // TPIDR_EL0

ldp x19, x20, [x1, #0]
ldp x21, x22, [x1, #16]
ldp x23, x24, [x1, #32]
ldp x25, x26, [x1, #48]
ldp x27, x28, [x1, #64]
ldp x29, x30, [x1, #80]
ldr x9, [x1, #96]
mov sp, x9
ldr x9, [x1, #104]
msr tpidr_el0, x9

ret
```

The function is a leaf-ish primitive: it doesn't call into C, doesn't push its own stack frame. x30 at entry is the BL return address (unsigned per AAPCS64); we save it directly into `prev->lr` without paciasp / autiasp. The caller's pac-ret discipline pushes its OWN signed lr to its stack at the caller's prologue — what we hold in x30 across the call is just the fresh BL return address. See the file commentary for details.

`thread_trampoline`:

```
bti c                           // defense in depth — passes BTYPE=00 too
blr x21                         // entry function (set by thread_create via ctx.x21)

// entry returned (no thread_exit at P2-A) — halt
1:  wfe
    b 1b
```

Reached via `cpu_switch_context`'s `ret` (BTYPE=00). The `bti c` is defensive — passes under any BTYPE — and forward-compatible with future indirect-jump dispatch into the trampoline.

### `kernel/proc.c` (~80 LOC at P2-A; ~270 LOC by P3-A)

`proc_init`: extincts on second call; creates the SLUB cache for `struct Proc`; allocates kproc; zero-initializes; sets `g_next_pid = 1`.

`proc_alloc`: kmem_cache_alloc + zero-init via `proc_zero_init`. Returns NULL on OOM. **P3-A**: PID assignment via atomic `__atomic_fetch_add(&g_next_pid, 1, RELAXED)` to defend against cascading-rfork SMP races where multiple Procs allocate concurrently from different CPUs.

`proc_free`: extinct on null / kproc / live-threads / non-empty list; kmem_cache_free.

The struct grows by appending across sub-chunks (P2-B: scheduler stats; P2-C: namespace; P2-D: handles; etc.). Existing offsets stay stable so the SLUB cache size doesn't churn.

#### Proc-table lock (P3-A, R5-H F75 close)

`g_proc_table_lock` is the global SMP serialization point for the Proc-lineage state machine. It protects:

- Each Proc's children list head + per-child sibling chain.
- Each Proc's `parent` pointer.
- Each Proc's `state` transitions ALIVE → ZOMBIE.
- Each Proc's `exit_msg` / `exit_status` mutations.
- The companion Thread's transition to THREAD_EXITING in `exits()`.

**Lock ordering**: `proc_table_lock → r->lock` (single direction; established in `exits()` where wakeup of parent's `child_done` happens INSIDE `proc_table_lock` to prevent the parent from being reaped between lock release and wakeup).

The reverse order (`r->lock → proc_table_lock`) is **forbidden**. `wait_pid_cond` is the only `r->lock`-holder that needs to read children-list state; at v1.0 it does so WITHOUT acquiring `proc_table_lock`, relying on three invariants:

1. **Single-writer children list**: at v1.0 single-thread Procs, only the parent's own thread mutates the children list. When the parent is in `wait_pid_cond` (called from `sleep` on the parent's thread), it is not concurrently in `exits` or `rfork`. The list is structurally stable.
2. **State visibility via wakeup→sleep handshake**: per-child `state` is mutated under `proc_table_lock` in the child's `exits`; the child's subsequent `wakeup(&parent->child_done)` performs a release on `r->lock`; the parent's sleep-resume re-acquires `r->lock` (acquire-pairs). Plain reads of `c->state` in `wait_pid_cond` see the post-wakeup state via the acquire/release transitivity.
3. **First-call tolerance**: the very first cond evaluation at sleep entry (before any wakeup) may see stale state — but that's correct; if no zombie visible, sleep; the next wakeup re-evaluates.

**Phase 5+ trip-hazard**: when multi-thread Procs land, invariant (1) weakens (sibling threads can mutate the parent's children list concurrently). At that point `wait_pid_cond` MUST acquire `proc_table_lock` AND the sleep protocol must be refactored to break the resulting `r->lock → proc_table_lock` cycle. Documented in `docs/handoffs/014-p2h-r5h.md` and `docs/handoffs/015-p3a-f75.md`.

**The race that motivated this lock (R5-H F75)**: parent A in `exits()` runs `proc_reparent_children` walking A's children to rewrite each `c->parent = kproc`. If a child B is concurrently in its own `exits()` reading `c->parent` (= A) for the wakeup target, and B's wakeup line fires after A's full exits → ZOMBIE → wakeup-grandparent → grandparent reaps A → A freed chain, B accesses freed memory at `wakeup(&A->child_done)`. The lock serializes A's mutation with B's read; the wakeup-inside-lock structure additionally prevents A from being reaped between B's read and B's wakeup.

### `kernel/thread.c` (~160 LOC)

`thread_init`: extincts on second call or pre-proc_init call; creates the SLUB cache for `struct Thread`; allocates kthread; zero-init context (boot CPU is already running on the boot stack — kthread doesn't own a separate kstack); attaches to kproc; sets `set_current_thread(g_kthread)` parking it in TPIDR_EL1.

`thread_create`:
1. SLUB-alloc the Thread descriptor.
2. `alloc_pages(THREAD_KSTACK_ORDER, KP_ZERO)` — 4 pages = 16 KiB. On fail, kmem_cache_free the Thread; return NULL.
3. Zero the Context, then set:
   - `ctx.x21 = entry` — trampoline's `blr x21` target.
   - `ctx.lr  = thread_trampoline` — cpu_switch_context's `ret` target.
   - `ctx.sp  = kstack_base + THREAD_KSTACK_SIZE` — top of stack (16-byte aligned because alloc_pages returns page-aligned + 16 KiB is itself a multiple of 16).
4. Link into proc's threads list (head insertion); increment proc->thread_count.
5. Increment `g_thread_created`.

`thread_free`:
1. Extinct on null / kthread / current / RUNNING.
2. Unlink from proc's threads list; decrement proc->thread_count.
3. If `kstack_base != NULL`, `free_pages(pa_to_page(kstack_base), THREAD_KSTACK_ORDER)`.
4. kmem_cache_free; increment `g_thread_destroyed`.

`thread_switch`:
1. `prev = current_thread()`. Extinct on NULL.
2. Extinct on `next == NULL` or `next->state == THREAD_EXITING`. Return on `prev == next`.
3. Update state: `prev->state = RUNNABLE; next->state = RUNNING`.
4. `set_current_thread(next)` — TPIDR_EL1 = next.
5. `cpu_switch_context(&prev->ctx, &next->ctx)`.
6. (Resumption point — prev was switched back to.)

The state-update before the asm switch is intentional: from prev's perspective, after the asm switch returns it observes `current_thread() == prev` and `prev->state == RUNNING` (because whichever peer switched back into prev set those values before its own asm switch). The "current pointer says next but registers say prev" window between step 4 and step 5 is invisible to prev — and at v1.0 single-CPU + no scheduler-tick preemption, no other observer races it. P2-B refines the ordering when SMP + scheduler-tick preemption make this window observable.

---

## Data structures

### `struct Context` — 112 bytes (14 × 8)

| Field | Offset | Description |
|---|---|---|
| `x19..x28` | 0..72 | Callee-saved general-purpose registers per AAPCS64. |
| `fp` (x29) | 80 | Frame pointer — callee-saved per AAPCS64 with `-fno-omit-frame-pointer`. |
| `lr` (x30) | 88 | Link register / resume PC. For fresh threads = `thread_trampoline`. |
| `sp` | 96 | Stack pointer. |
| `tpidr_el0` | 104 | EL0 TLS pointer; 0 for kernel threads, set per-thread for userspace. |

Pinned by `_Static_assert`s in `<thylacine/context.h>`:
- `sizeof(struct Context) == 112`
- `_Alignof(struct Context) >= 8`
- offsets for x19, x20, x21, fp, lr, sp, tpidr_el0 match the table.

`arch/arm64/context.S` hardcodes the offsets in the immediate-form `[x0, #N]` addressing of stp/ldp/str/ldr. A field reorder without an asm update would silently corrupt context switches — the static asserts catch it at build before the asm executes.

### `struct Proc` — appended to per sub-chunk

P2-A:
| Field | Description |
|---|---|
| `pid` | int — PID 0 = kproc; PID 1+ = subsequent procs. |
| `thread_count` | int — number of threads attached. |
| `threads` | struct Thread* — head of doubly-linked list (Thread.next_in_proc). |

Future appends:
- P2-B: scheduler statistics (per-Proc CPU time, vruntime accumulation).
- P2-C: namespace pointer (`struct Pgrp *`).
- P2-D: handle table head (`struct HandleTable *`).
- P2-E: address space (page table root, vma_tree).
- P2-F: notes queue, parent/children, `exit_status`.
- P2-G: credentials, capability bitmask.

### `struct Thread` — appended to per sub-chunk

P2-A:
| Field | Description |
|---|---|
| `tid` | int — TID 0 = kthread within kproc. |
| `state` | enum thread_state — RUNNING / RUNNABLE / SLEEPING / EXITING. |
| `proc` | struct Proc* — owning process. |
| `ctx` | struct Context — saved registers (valid only when not RUNNING). |
| `kstack_base` | void* — base of 16 KiB kernel stack (NULL for kthread; boot stack is shared). |
| `kstack_size` | size_t — 0 for kthread; THREAD_KSTACK_SIZE for thread_create'd threads. |
| `next_in_proc`, `prev_in_proc` | doubly-linked list links into proc->threads. |

Future appends:
- P2-B: EEVDF data (vd_t, ve_t, band, weight); scheduler queue links.
- P2-C: per-CPU affinity hint.
- Phase 2 close: `errstr` buffer (Plan 9 idiom); guard page below kstack.

---

## State machines

### Thread state transitions (P2-A subset)

```
              thread_create
INVALID  ────────────────────►  RUNNABLE
                                   │
                                   │ thread_switch (becomes next)
                                   ▼
INVALID                         RUNNING
   ▲                               │
   │                               │ thread_switch (becomes prev)
   │                               ▼
   │                            RUNNABLE
   │                               │
   │  thread_free                  │ thread_free
   └───────────────────────────────┘
```

P2-B adds:
- `RUNNABLE → SLEEPING` via `thread_block` (wait protocol).
- `SLEEPING → RUNNABLE` via `thread_wake` (wake protocol).
- `RUNNING → SLEEPING` via `thread_block` from current.

Phase 2 close adds:
- `RUNNING → EXITING` via `thread_exit`.
- `EXITING → freed` via reap path.

### Spec cross-reference

`specs/scheduler.tla` (P2-A sketch) models the transitions above and pins these invariants under TLC:

- `StateConsistency` — a thread is RUNNING iff some CPU's `current` is the thread.
- `NoSimultaneousRun` — a thread runs on at most one CPU at a time.
- `RunnableInQueue` — a thread is RUNNABLE iff it sits in some CPU's runqueue.
- `SleepingNotInQueue` — a SLEEPING thread is in no runqueue and is no CPU's current.

P2-A's `scheduler.tla.cfg` runs at `Threads = {t1, t2, t3}, CPUs = {c1, c2}` — 99 distinct states explored, no invariant violations.

The mapping from spec actions to source locations (canonical at `specs/SPEC-TO-CODE.md`, populated as P2-B fills in):

| Spec action | Source location |
|---|---|
| `Yield(cpu)` | `kernel/thread.c::thread_switch` (P2-A direct switch; P2-B refines) |
| `Block(cpu)` | (P2-B: `thread_block`) |
| `Wake(t)` | (P2-B: `thread_wake`) |
| `Resume(cpu)` | (P2-B: scheduler dispatch from idle) |

### Future spec refinements (gate-tied to phases)

Per `ARCHITECTURE.md §25.2` `scheduler.tla` is mandatory for Phase 2 close. P2-A's sketch is the framing; P2-B and P2-C extend:

- **P2-B**: EEVDF deadline math (vd_t, ve_t, virtual time advancement); pick-earliest-deadline replacing CHOOSE-from-runq.
- **P2-B**: Wait/wake atomicity refinement — split `Block` into separate `CheckCond` + `Sleep` actions; prove `NoMissedWakeup`. Land a `_buggy.cfg` showing the missed-wakeup race when atomicity is violated (executable documentation pattern per CLAUDE.md).
- **P2-B**: IPI ordering (ARCH §28 I-18) — model send-order delivery; prove ordering preserved across cross-CPU operations.
- **P2-C**: Work-stealing fairness — cross-CPU dequeue with locks; prove no preferential treatment of bands.
- **Phase 2 close**: Liveness — every runnable thread eventually runs (I-8) via weak fairness; latency bound (I-17 — slice_size × N).

---

## Tests

`kernel/test/test_context.c` registers two tests in `g_tests[]`:

### `context.create_destroy`

- Capture `thread_total_created`, `thread_total_destroyed`, `kproc()->thread_count` before.
- `thread_create(kproc(), test_thread_entry)`. Verify:
  - Returned Thread* is non-NULL.
  - `state == THREAD_RUNNABLE`.
  - `kstack_base != NULL`.
  - `kstack_size == 16 * 1024`.
  - `proc == kproc()`.
  - `thread_total_created` advanced by 1.
  - `kproc()->thread_count` advanced by 1.
- `thread_free(t)`. Verify:
  - `thread_total_destroyed` advanced by 1.
  - `kproc()->thread_count` retreated by 1.

### `context.round_trip`

- Capture `g_test_main = current_thread()`. Verify it equals `kthread()` and is `THREAD_RUNNING`.
- `thread_create(kproc(), test_thread_entry)`. The entry: `g_test_state++; thread_switch(g_test_main);`.
- `thread_switch(t)` — boot kthread switches into the new thread, which increments the counter and switches back.
- After resume, verify:
  - `g_test_state == 1`.
  - `current_thread() == g_test_main`.
  - `g_test_main->state == THREAD_RUNNING`.
  - `t->state == THREAD_RUNNABLE`.
- `thread_free(t)`.

This test exercises the entire context-switch primitive end-to-end:
- `cpu_switch_context` save + load.
- `thread_trampoline` reach via `ret`; `bti c` passing under BTYPE=00.
- `blr` from trampoline to entry; entry's `bti c` passing under BTYPE=10.
- `thread_switch` from entry back to boot kthread; `cpu_switch_context` reverse direction.
- Entry's stack frame is allocated, used, and abandoned (entry doesn't return — boot resumes mid-stack and never re-enters entry).

What the tests do NOT cover (deferred to P2-B / Phase 2 close):
- IRQ-masking discipline around the switch (no preemption-via-IRQ at v1.0).
- TLS save/restore correctness (TPIDR_EL0 = 0 throughout — no userspace).
- Multi-thread round-trips through the scheduler (no scheduler at P2-A).
- SMP correctness (single CPU at v1.0; SMP at Phase 2 close).
- Stack overflow detection (no guard page at P2-A).

---

## Error paths

| Function | Error | Action |
|---|---|---|
| `proc_init` | called twice | extinct — "proc_init called twice" |
| `proc_init` | meta-cache create returned NULL | extinct — belt-and-braces (PANIC_ON_FAIL also extincts) |
| `proc_alloc` | called pre-proc_init | extinct — "proc_alloc before proc_init" |
| `proc_alloc` | OOM | return NULL (no extinct — caller decides) |
| `proc_free` | NULL | extinct — "proc_free(NULL)" |
| `proc_free` | kproc | extinct — "proc_free attempted on kproc" |
| `proc_free` | thread_count != 0 | extinct — "proc_free with live threads" |
| `proc_free` | threads list non-empty | extinct — "proc_free with non-NULL threads list" |
| `thread_init` | called twice | extinct — "thread_init called twice" |
| `thread_init` | called pre-proc_init | extinct — "thread_init before proc_init" |
| `thread_create` | called pre-thread_init | extinct — "thread_create before thread_init" |
| `thread_create` | NULL proc | extinct — "thread_create with NULL proc" |
| `thread_create` | NULL entry | extinct — "thread_create with NULL entry" |
| `thread_create` | Thread alloc OOM | return NULL |
| `thread_create` | kstack alloc OOM | kmem_cache_free Thread; return NULL |
| `thread_free` | NULL | extinct |
| `thread_free` | kthread | extinct |
| `thread_free` | current | extinct |
| `thread_free` | RUNNING | extinct |
| `thread_switch` | no current_thread | extinct — "thread_switch with no current thread" |
| `thread_switch` | NULL next | extinct |
| `thread_switch` | next is EXITING | extinct |
| `thread_switch` | prev == next | return (no-op) |

---

## Performance characteristics

P2-A is intentionally minimal — no scheduler, no preemption — so performance numbers are the cost of the primitives themselves:

| Operation | Cost (P2-A measurement) |
|---|---|
| `current_thread()` | 1 cycle (mrs TPIDR_EL1) |
| `set_current_thread(t)` | 1 cycle (msr TPIDR_EL1) |
| `cpu_switch_context` | ~30 instructions (saves + loads + ret); single-digit cycles on out-of-order ARM cores |
| `thread_create` | ~1 µs (SLUB alloc + alloc_pages + memset zeroing) |
| `thread_free` | ~0.5 µs (SLUB free + free_pages + unlink) |
| `thread_switch` (round trip via test) | ~100 ns measured indirectly via boot-time delta of context.round_trip vs absent (sub-microsecond) |

Boot-time impact: P1-I close ~38 ms → P2-A ~42 ms (production build, single boot). The +4 ms accounts for the two new tests (context.create_destroy + context.round_trip) running.

VISION §4 budget: 500 ms. Headroom remains generous.

---

## Status

| Component | State |
|---|---|
| `struct Proc`, `struct Thread`, `struct Context` | Landed (P2-A) |
| `cpu_switch_context`, `thread_trampoline` | Landed (P2-A) |
| `proc_init`, `proc_alloc`, `proc_free` | Landed (P2-A) |
| `thread_init`, `thread_create`, `thread_free`, `thread_switch` | Landed (P2-A) |
| `current_thread`, `set_current_thread` | Landed (P2-A; inline mrs/msr TPIDR_EL1) |
| In-kernel tests | 2 added: `context.create_destroy`, `context.round_trip` |
| `specs/scheduler.tla` sketch | Landed; TLC-clean at 99 states (Threads=3, CPUs=2) |
| `specs/SPEC-TO-CODE.md` | Stubbed; populated by P2-B |
| Reference doc | This file (14-process-model.md) |
| Status doc | `docs/phase2-status.md` |
| EEVDF scheduler | P2-B |
| Wait/wake (block/wake) | P2-B |
| IRQ masking discipline around switch | P2-B |
| Work-stealing | P2-C |
| Per-thread guard page | Phase 2 close |
| `errstr` buffer | Phase 2 close |
| Real `thread_exit` + reap | Phase 2 close |
| TLS for userspace | Phase 5 |

---

## Known caveats / footguns

### TPIDR_EL1 reset value is UNKNOWN

ARM ARM specifies TPIDR_EL1's reset value as architecturally UNKNOWN. P2-A does not initialize it in start.S — `current_thread()` is undefined-ish before `thread_init` runs.

**Mitigation**: All callers of `current_thread()` post-date `thread_init`. The only path that runs pre-thread_init is `boot_main`'s prologue (banner + DTB + phys_init + slub_init + exception_init + gic_init + timer_init + proc_init), and none of those call current_thread.

**Defense in depth, deferred**: a `msr tpidr_el1, xzr` near the start of `_real_start` would make pre-thread_init reads deterministic-NULL. Two instructions; held for an explicit signoff because it's a start.S edit not strictly required by P2-A.

### Boot-stack guard page

The boot stack from start.S (16 KiB, defined by `_boot_stack_*` symbols in the linker script) has a guard page below it (P1-C-extras Part A). kthread inherits this stack — so the boot path's stack-overflow detection covers TID 0.

`thread_create`'s 16 KiB stacks have NO guard page at P2-A. Stack overflow in a P2-A kthread silently corrupts adjacent SLUB / buddy / page-array memory.

**Mitigation**: P2-A's only thread_create call site is the test, which uses minimal stack. No risk in practice.

**Phase 2 close**: per-thread guard page lands. The order=2 alloc gets bumped to order=3 (8 pages = 32 KiB) with the bottom page unmapped via `mmu_unmap` to act as a guard.

### IRQ masking around `thread_switch`

P2-A does NOT mask IRQs around the switch. The only IRQ source live at v1.0 is the timer; its handler increments `g_ticks` and returns — does not touch thread state, current_thread, or any per-thread data. Reentrancy is trivially safe.

**Phase 2 close / P2-B**: scheduler-tick preemption (timer IRQ → trigger reschedule) needs the IRQ-mask discipline around the switch. The `spin_lock_irqsave` machinery is already in place from P1-I (audit F30); P2-B applies it.

### `prev->state = RUNNABLE` in `thread_switch`

`thread_switch` unconditionally sets prev's state to RUNNABLE. This is correct for "yield to another thread" semantics. For "block on a condition" semantics (sleep), prev should be SLEEPING — but P2-A has no block primitive. P2-B adds `thread_block(cond)` which sets SLEEPING explicitly before calling into the switch primitive.

### `thread_trampoline` halts on entry-return

If a thread's entry function returns, the trampoline halts on WFE. There's no `thread_exit` plumbing yet — the thread descriptor isn't reclaimed; its kstack remains allocated; if the trampoline's WFE loop is preempted (it isn't at v1.0), the thread reports as "running but stuck."

**Mitigation at P2-A**: test entries don't return — they call `thread_switch(boot_main_kt)` which doesn't return.

**Phase 2 close**: real `thread_exit(int exitcode)` syscall + kernel-side `thread_reap` path replace the WFE halt.

### Spec is a sketch — `_buggy.cfg` deferred

`specs/scheduler.tla` at P2-A is the framing — TLC-clean at small bounds, but doesn't yet model EEVDF deadline math, wait/wake atomicity, or IPI ordering. The CLAUDE.md "executable documentation" pattern says we should pair the primary `.cfg` with a `_buggy.cfg` showing how a specific invariant violation arises. P2-A defers this to P2-B because the relevant bug-classes (missed wakeups, deadline starvation, IPI reordering) are introduced by P2-B's actions, not P2-A's.

### Thread-context size pinned at 112 bytes

Adding a field to `struct Context` (e.g., FPSIMD state for userspace at Phase 5) requires updating the `_Static_assert`s in `context.h` AND the offsets / stp/ldp/str/ldr immediates in `context.S`. The build break is loud but multi-step.

**Recipe** when extending: bump the size assert; add the new offset assert; extend `context.S` with the matching save/load; rerun `tools/test.sh`. The test will fault catastrophically on first switch if the asm is inconsistent — there is no quiet failure mode.

### `kstack_base` is PA-cast-to-void*

Like P1-D's `kpage_alloc`, `thread_create` stores the kernel stack as `(void *)(uintptr_t)page_to_pa(stack_pg)`. TTBR0 identity-maps low 4 GiB at v1.0, so dereferencing this pointer works. Phase 2 introduces the kernel direct map and converts both `kpage_alloc` and `thread_create` to high-VA pointers in the same chunk.

### Naming candidates (held)

Per CLAUDE.md "Thematic naming" — keeping an eye out for marsupial-themed alternatives:
- `THREAD_RUNNABLE` / `THREAD_RUNNING` — Plan 9 / POSIX-standard; renaming would make code less obvious to readers without project context. Keep standard names.
- `thread_trampoline` — clear function name; trampoline is the standard CS term. Keep.
- `cpu_switch_context` — Plan 9 calls this `swtch`; we're using the more explicit form. Could shorten if the file gets noisy. Held.

---

## Build + verify

```bash
# Default build
tools/build.sh kernel

# UBSan build
tools/build.sh kernel --sanitize=undefined

# All tests
tools/test.sh                          # 11/11 PASS expected
tools/test.sh --sanitize=undefined     # 11/11 PASS expected (UBSan-clean)

# Deliberate-fault matrix (regression check for Phase 1 hardening)
tools/test-fault.sh                    # 3/3 PASS expected

# KASLR variance
tools/verify-kaslr.sh -n 5             # 5 distinct expected (≥ 70% threshold)

# All TLA+ specs
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config scheduler.cfg scheduler.tla
# Expected: "Model checking completed. No error has been found."
# 99 distinct states explored; depth 9; sub-second.
```
