# 80 — `torpor`: the wait-on-address primitive (P6-pouch-wait-addr)

The v1.0 kernel-side wait-on-address primitive — Thylacine's futex-equivalent and the substrate over which sub-chunk 9 (`pouch-threads`) will run pouch's pthread mutex/condvar implementation. Two syscalls: `SYS_TORPOR_WAIT` (39) blocks the calling thread on a user-VA word until the word may have changed; `SYS_TORPOR_WAKE` (40) wakes waiters whose value-word matches a producer's mutation.

Per `POUCH-DESIGN.md §7` (the v1.0 design) + `ARCHITECTURE.md §11.2` (the syscall table) + `ARCHITECTURE.md §28 I-9` (no wakeup is lost between wait-condition check and sleep — specialized to wait-on-address). Audit-trigger surface (`CLAUDE.md` / `ARCHITECTURE.md §25.4`): `kernel/torpor.c`, `kernel/syscall.c` (the new SVC handlers), `arch/arm64/uaccess.S` (the new `uaccess_load_u32` primitive).

---

## Naming rationale

**Torpor** — the marsupial / dasyuromorph deep-sleep state. A thylacine entering torpor parks its metabolism on an external trigger; a thread entering `torpor_wait` parks its execution on a user-VA word's value. Marsupials wake from torpor when ambient cues change; threads wake from `torpor_wait` when a sibling thread changes the word and calls `torpor_wake`. The biological analogue is exact: same shape, same semantics, same wake conditions.

The name was reserved for this primitive at the project-naming-discipline pass (CLAUDE.md "Thematic naming"); sub-chunk 8 brings it to life. Distinct from `_torpor()` (the WFI halt loop in `arch/arm64/start.S` — same metaphor, different scope: that's a CPU torpor, this is a thread torpor).

---

## Spec-to-code suspended — the model validated by reasoning

Per CLAUDE.md "Spec-to-code suspended" (broadened 2026-05-23), no `specs/futex.tla` is written for sub-chunk 8. The I-9-specialized invariant ("no wakeup is lost between the value check and the sleep") is validated by careful prose reasoning in `kernel/include/thylacine/torpor.h` + `kernel/torpor.c`. The audit round + the seven runtime tests (below) are the rigor floor.

The proof sketch, reproduced for the reader:

The consumer's WAIT performs the atomic-load `*addr_va` UNDER `torpor_lock`; if the load matches `expected`, the consumer registers itself in the wait queue and only THEN sleeps. The producer's WAKE acquires the same `torpor_lock` to walk the queue. By lock acquire/release pairing:

- *WAKE-unlock before WAIT-lock*: the consumer's load (under the lock it just acquired) observes the producer's pre-WAKE store (which happened before WAKE acquired the lock); the load returns the new value, fails the compare, returns 0 immediately without sleeping.
- *WAIT-lock before WAKE-lock*: the consumer registers before WAKE can walk the bucket; WAKE finds the consumer and delivers the wakeup.

The remaining case is a *spurious wake*: the consumer's load returned the pre-store value (consumer registered just before the producer stored + WAKEd). The consumer registers, sleeps; WAKE finds it and wakes; the consumer's user-side atomic-recheck on return (musl's standard futex discipline) handles it — re-checks the word, either proceeds or re-sleeps.

The userspace contract is that the producer ALWAYS calls WAKE after a state-changing store that should unblock waiters (the standard Linux/musl futex contract). Without it, the kernel cannot guarantee delivery (the producer's store never reaches the kernel's lock-acquire chain).

---

## Syscall ABI

```c
// kernel/include/thylacine/syscall.h
SYS_TORPOR_WAIT  = 39
SYS_TORPOR_WAKE  = 40

// kernel/include/thylacine/torpor.h
#define TORPOR_OK                 0
#define TORPOR_ERR_EINVAL       (-22)
#define TORPOR_ERR_EFAULT       (-14)
#define TORPOR_ERR_ETIMEDOUT   (-110)
#define TORPOR_MAX_TIMEOUT_US  (60ull * 60ull * 1000000ull)   // 1 hour
#define TORPOR_HASH_BUCKETS  64u
```

### `SYS_TORPOR_WAIT(addr_va, expected, timeout_us) → 0 / -errno`

| Register | Meaning |
|---|---|
| `x0` | `addr_va` — user-VA pointer to a 4-byte aligned `u32` word |
| `x1` | `expected` (low 32 bits) — compare against `*addr_va` under `torpor_lock`; if they don't match, return 0 immediately (fast path) |
| `x2` | `timeout_us` — `s64`; `< 0` blocks indefinitely; `>= 0` blocks at most this many microseconds (capped at `TORPOR_MAX_TIMEOUT_US`); `0` is a probe that returns `-ETIMEDOUT` immediately if the value still matched |
| `x8` | syscall number = 39 |

**Returns** (Linux/musl-numeric, decoded by pouch's `syscall_ret.c` as `-errno`):

| Value | Meaning |
|---|---|
| `0` (`TORPOR_OK`) | woken — OR value already differed from `expected` at entry (the fast path; userspace re-checks the predicate) |
| `-22` (`-EINVAL`) | bad args: NULL Proc, NULL addr, unaligned (4-byte), `addr_va >= UACCESS_USER_VA_TOP`, word straddles top, or `timeout_us > TORPOR_MAX_TIMEOUT_US` |
| `-14` (`-EFAULT`) | `addr_va` in range but unmapped or permission-denied at the kernel-side load |
| `-110` (`-ETIMEDOUT`) | `timeout_us >= 0` and the deadline lapsed before any wake |

### `SYS_TORPOR_WAKE(addr_va, count) → woken / -errno`

| Register | Meaning |
|---|---|
| `x0` | `addr_va` — user-VA pointer (same alignment + bound rules as WAIT) |
| `x1` | `count` — max waiters to wake. `0` is a no-op (returns 0); `UINT32_MAX` is the "wake all" pattern (the `pthread_cond_broadcast` shape) |
| `x8` | syscall number = 40 |

**Returns**: the number of waiters actually woken (`>= 0`), or `-EINVAL` on bad args. WAKE does NOT load the futex word — it only hashes `(Proc *, addr_va)` and walks the bucket.

---

## Kernel implementation

### The mechanism (`kernel/torpor.c`)

```c
struct torpor_waiter {
    struct Rendez         rendez;       // private rendez for the sleep
    struct Proc          *proc;         // owner Proc (key component 1)
    u64                   user_va;      // address being waited on (k2)
    struct torpor_waiter *next;         // bucket-chain link
    u32                   awoken;       // 0 sleeping, 1 woken (tsleep cond)
    u32                   _pad;
};

static spin_lock_t           torpor_lock = SPIN_LOCK_INIT;
static struct torpor_waiter *torpor_buckets[TORPOR_HASH_BUCKETS];
```

One global `torpor_lock` (a `spin_lock_t`) guards the bucket array AND every waiter's `awoken` flag. The waiter struct is **stack-allocated** in the consumer's WAIT frame — every exit path unlinks it from its bucket before the stack frame is reaped (the lifetime invariant). Each waiter carries its own `struct Rendez` for the actual sleep transition; `tsleep` handles the cond-checked / deadlined sleep itself, with the existing `scheduler.tla`-pinned wakeup atomicity.

Hash function: `(Proc * ^ user_va >> 3) & (TORPOR_HASH_BUCKETS - 1)`. 64 buckets at v1.0; the table is ~512 B and uncontended (single-threaded Procs). v1.x can size up once `pouch-threads` introduces real contention. Per-Proc keying means cross-Proc shared-anon-memory futexes aren't supported at v1.0; that landed for Tier 2 burrows (POUCH-DESIGN.md §7).

### WAIT state machine (`sys_torpor_wait_for_proc`)

1. Validate args. NULL Proc / addr_va == 0 / unaligned / above USER_VA_TOP / straddles top / `timeout_us > MAX` → `-EINVAL`.
2. Initialise the stack-local `struct torpor_waiter` — set `awoken = 0`, `proc = p`, `user_va = addr_va`.
3. **Pre-fault load, NO lock** (REVENANT R-5 F1): `uaccess_load_u32(addr_va, &prefault)`. On fault → `-EFAULT`. Exists so the authoritative under-lock load below cannot fault-and-sleep (a file-backed futex page would otherwise block a `dev->read` under `torpor_lock` — a system-wide futex stall).
4. **Lock-free compare** (the Linux-futex compare-before-queue shape; the go-arc osyield-floor fix): if `prefault != expected` → return `0` **without taking `torpor_lock`**. Sound because NO waiter is registered on this path — I-9's missed-wakeup window exists only between a waiter's bucket-register and its sleep, which only the equal path reaches. A stale mismatch (the word changed under us) is a benign spurious return the futex contract permits: the caller re-evaluates its own predicate. **Ordering contract**: the mismatch return provides only the plain aligned-u32 load's single-copy-atomicity, NOT the acquire-over-`torpor_lock` the pre-change locked path incidentally provided; every consumer (musl `__futexwait` loops, the Go runtime `futexsleep` re-checks, libthyla-rs `torpor::wait`'s documented re-check contract) orders its own data with its own atomics, per the universal futex contract.
5. `spin_lock(&torpor_lock)`.
6. `uaccess_load_u32(addr_va, &observed)` — the authoritative load (fault-free by the pre-fault; a re-fault after a concurrent decommit still routes to `spin_unlock` + `-EFAULT`).
7. If `observed != expected` → `spin_unlock`, return `0` (the near-zero residue: the word changed between the pre-fault compare and here).
8. Link the waiter at the bucket head — published under the lock so WAKE's walk sees it.
9. Compute `deadline_ns` (timer_now_ns timebase): `timeout_us < 0` → `0` (no deadline); `timeout_us == 0` → `now` (a past deadline that times out immediately at tsleep entry); `timeout_us > 0` → `now + timeout_us * 1000` with wrap-clamping.
10. `spin_unlock(&torpor_lock)`.
11. `tsleep(&waiter->rendez, cond_awoken, &waiter, deadline_ns)` — blocks until `waiter->awoken != 0` OR the deadline lapses.
12. Re-`spin_lock(&torpor_lock)`. Unlink. Scribble `rendez.waiter = NULL` (defense-in-depth). `spin_unlock`.
13. Return `0` on `TSLEEP_AWOKEN`, `-ETIMEDOUT` on `TSLEEP_TIMEDOUT`.

### WAKE state machine (`sys_torpor_wake_for_proc`)

1. Validate args. NULL Proc / bad addr → `-EINVAL`. `count == 0` returns `0` without locking.
2. `spin_lock(&torpor_lock)`.
3. Walk the matching bucket. For each `(proc, user_va)`-matching waiter with `awoken == 0` (and at most `count`): set `awoken = 1`; `wakeup(&waiter->rendez)`; `count--`.
4. `spin_unlock(&torpor_lock)`.
5. Return the count of waiters woken (`>= 0`).

The walk *leaves* the waiter linked — the consumer unlinks on its own post-tsleep path. Subsequent WAKEs filter via `awoken == 0` so a previously-woken-but-not-yet-unlinked waiter is not re-woken.

### Lock order

`torpor_lock` → `waiter->rendez.lock` (held briefly inside `wakeup`).
`torpor_lock` → `proc->vma_lock` → `buddy_lock` (held across the fault-path of `uaccess_load_u32` if it triggers demand-paging).

Only WAKE ever takes both `torpor_lock` and a `rendez.lock` simultaneously; the consumer never holds `rendez.lock` while taking `torpor_lock` (it acquires `rendez.lock` only inside `tsleep`, after releasing `torpor_lock`). No inversion is possible.

The original held-across-fault discipline (torpor_lock held during a faulting `uaccess_load_u32`) was retired in two steps: REVENANT R-5 F1 moved the fault-in BEFORE the lock (a file-backed futex page must never block a `dev->read` under `torpor_lock`), and the go-arc lock-free compare then made the mismatch path avoid the lock entirely — the Linux-futex compare-before-queue shape this paragraph originally deferred to v1.x. The under-lock load remains the authoritative register-vs-WAKE serialization for the equal (parking) path.

### `uaccess_load_u32` (`arch/arm64/uaccess.S`)

New primitive added at sub-chunk 8 — mirrors the `uaccess_load_u8` / `uaccess_store_u8` pattern. One `ldr w2, [x0]` instruction as the fault point; a fixup-table entry maps it to the `_fault` label. Plain `LDR` (not `LDAR`) — the consumer's `torpor_lock` acquire pairs with the producer's `torpor_lock` release on its preceding WAKE to provide memory ordering; the standard Linux-futex discipline.

The caller (`torpor.c`) is responsible for validating the user VA's 4-byte alignment + bound before invoking the primitive; an unaligned load would alignment-fault outside the fixup table's class and pass through to the standard ELE path.

---

## Data structures

`struct torpor_waiter` — 56 bytes on aarch64 (Rendez 24 B + pointers 16 B + u64 user_va 8 B + u32 awoken + u32 _pad). One per concurrently-waiting thread; stack-allocated in the consumer's WAIT frame.

`torpor_buckets[64]` — a static array of `struct torpor_waiter *` heads. ~512 B in `.bss`. Each bucket is a singly-linked list (links via `waiter->next`); the chain at v1.0 stays near-empty (per-Proc keying, single-threaded Procs).

`torpor_lock` — one global `spin_lock_t`. Plain `spin_lock` (not `spin_lock_irqsave`): no IRQ handler touches the torpor surface, and the critical section never sleeps blocking on torpor_lock itself.

---

## Tests (`kernel/test/test_torpor.c`)

Eight tests, all PASS × default + UBSan (suite 549 → **557/557**):

| Test | Coverage |
|---|---|
| `torpor.wait_rejects_bad_args` | NULL Proc / NULL addr / unaligned / above USER_VA_TOP / word straddles top / timeout above cap → `-EINVAL` |
| `torpor.wait_rejects_unmapped_va` | valid user-VA shape but no VMA → `-EFAULT` (also exercises the userland_demand_page kproc-refusal path). Pinned to a kproc thread via TEST_ASSERT — F5 (P3 audit). |
| `torpor.wake_rejects_bad_args` | symmetric arg validation for WAKE |
| `torpor.wake_empty_bucket_returns_zero` | no waiters → 0; `count == 0` → 0 |
| `torpor.wait_value_mismatch_fast_path` | consumer thread on a fresh Proc: value 0 vs expected `0xDEADBEEF` → 0 without sleeping |
| `torpor.wait_timeout_zero_returns_etimedout` | consumer thread, value matches, `timeout_us == 0` → `-ETIMEDOUT` via the tsleep past-deadline-immediate path |
| `torpor.wait_wake_handoff` | end-to-end: consumer thread registers + sleeps; boot thread WAKEs; consumer resumes, returns `0` |
| `torpor.wake_two_waiters_count_bound` | F9 (P3 audit): two consumers on the same `(Proc, addr_va)`, first `WAKE(count=1)` wakes one; second `WAKE(count=1)` wakes the other. Verifies the bucket-walk visits multiple matching waiters, the `count` bound stops the walk, and the count reflects actual wakeups (F1 P2 fix). |

The uaccess-success tests use the same pattern as `test_tsleep`: `proc_alloc()` a fresh Proc, install an anon page via `sys_burrow_attach_for_proc` (so the consumer thread's TTBR0 sees the page on demand-paging), spawn a consumer thread with `thread_create(p, entry)`, drive the wait-then-wake handoff with `sched()` yields. (The fault path refuses to demand-page kproc since `kproc->pgtable_root == 0`, so the consumer cannot run on kproc.)

The full no-lost-wakeup race (real producer/consumer threads in the same Proc with concurrent stores) requires `pouch-threads` and lands with sub-chunk 9. v1.0's test suite covers the mechanism's deterministic paths; the lock-ordering + register-then-observe protocol is the load-bearing invariant.

`usr/joey/joey.c` adds a 4-line boot-path smoke (`joey: torpor SVC-dispatch ok ...`) that validates the SVC dispatch is wired: WAIT on an unmapped addr → `-EFAULT`, WAKE on an empty bucket → 0.

---

## libt wrappers (`usr/lib/libt/include/thyla/syscall.h`)

```c
long t_torpor_wait(unsigned int *addr_va, unsigned int expected,
                   long timeout_us);
long t_torpor_wake(unsigned int *addr_va, unsigned int count);
```

Standard inline-asm SVC stubs. Used by `usr/joey/joey.c`'s SVC-dispatch smoke; the production consumer is sub-chunk 9's pthread layer (which will reach the syscalls via the musl seam, not libt directly — the same pattern `pouch-mem` uses for `mmap` / `munmap`).

The `T_TORPOR_*` error constants mirror the kernel's `TORPOR_ERR_*`; numerically chosen to be Linux/musl-compatible so the pouch syscall seam decodes them as familiar `errno`s.

---

## Error paths

Every WAIT / WAKE error path takes one of three forms:

- **EINVAL** — at the entry validation. NULL Proc, malformed `addr_va` (zero, unaligned, out of bound, straddling top), or `timeout_us` above the cap. No state was touched.
- **EFAULT** — WAIT only. After the validation passed and `torpor_lock` was acquired, `uaccess_load_u32` faulted (the addr is in range but no VMA covers it, or the fault path refused to demand-page e.g. for `pgtable_root == 0`). `torpor_lock` is released; no waiter is registered.
- **ETIMEDOUT** — WAIT only. The consumer was registered + tsleeped, and the deadline lapsed before any wake. The waiter is unlinked on resume; `torpor_lock` is released.

All other return paths return `0` (success). Userspace re-checks the user-side atomic word on return regardless of whether `0` came from a wake or a value-mismatch fast path — the standard futex discipline.

---

## Performance characteristics

Not yet measured. v1.0 is single-threaded so torpor is uncontended; the budget-defining workload is pthread-heavy multithreaded user code (post sub-chunk 9). The micro-benchmark would be: 1000 pthread_mutex_lock/unlock pairs in a tight loop, measured against Linux's futex-backed mutex on equivalent hardware. Land with `pouch-threads`.

Known potential bottlenecks:
- Single global `torpor_lock`: serializes all bucket operations. v1.x can shard per-bucket (the Linux futex pattern) once contention warrants.
- Lock held across the demand-paging fault on the user-VA load. v1.x can adopt the Linux `get_user_inatomic` + retry-without-lock pattern.

---

## Status

| Sub-chunk | Status | Tests |
|---|---|---|
| 8 `pouch-wait-addr` | landed | 7 torpor.* + 1 joey-side SVC-dispatch smoke |

The pouch-side wiring (musl's `__timedwait` + the futex calls in `src/thread/`) lands with sub-chunk 9 (`pouch-threads`), where the no-lost-wakeup model gets a real-world stress test through pthread mutex/condvar/rwlock workloads.

---

## Known caveats / footguns

- **Spurious wake** is part of the contract — userspace must re-check the atomic word on every return. The kernel ALSO returns 0 for the value-mismatch fast path (no sleep happened); musl's pthread layer re-checks the predicate after `__timedwait` returns regardless, so this is harmless. (Linux's futex returns `-EAGAIN` for the value-mismatch fast path; v1.0 collapses both onto 0 for simplicity. v1.x can split if a PI futex or similar discriminator surfaces.)
- **No `-EINTR`** at v1.0. Notes / signals don't yet propagate through the wait path; they will when sub-chunk 13 (`pouch-signals`) lands. Until then, tsleep returns `AWOKEN` regardless of cause; torpor maps to `TORPOR_OK`.
- **Per-Proc keying**. v1.0 does NOT support cross-Proc shared-memory futexes; the bucket key is `(Proc *, addr_va)`. Cross-Proc futexes land with Tier-2 burrows (POUCH-DESIGN.md §7).
- **`sys_torpor_wait_for_proc(p, ...)` requires `p == current_thread()->proc`** (F4, P3 audit). The user-VA load implicitly walks the current thread's TTBR0; mismatched `p` would silently read the wrong word from a coincidentally-mapped VA in current's namespace. Enforced via an `extinction` assert at WAIT entry — the SVC wrapper always satisfies it; tests driving the `_for_proc` form must run on a thread of `p`.
- **Lock held across demand-paging** (F8, P3 audit) — `torpor_lock` is held during the user-VA load, which may demand-page (`vma_lock` + buddy). Bounded but not trivially short. Acceptable at v1.0; v1.x may adopt the Linux `get_user_inatomic` + retry-without-lock pattern.
- **Lock held across `wakeup()`'s `on_cpu` spin** (F2, P2 audit) — `WAKE`'s bucket walk calls `wakeup()` while holding `torpor_lock`; if the woken thread is mid-context-switch on a peer CPU, `wake_rendez_waiter` spins on `t->on_cpu`. Under heavy multi-Proc contention this serializes all torpor traffic. v1.0 is single-Proc-mostly so the hazard is dormant; v1.x options: (a) per-bucket lock; (b) two-pass "mark + list under lock, dispatch wakes after lock-drop".
- **`WAKE(addr, 0)` is a literal no-op** (F7, P3 audit) — does NOT take `torpor_lock`, does NOT serve as a memory barrier. Producers MUST NOT rely on it for memory ordering. Use a `count > 0` WAKE if the bucket may be non-empty, or a userspace atomic-release for pure announce-without-wake semantics.
- **WAKE's count reflects ACTUAL wakeups** (F1, P2 audit fix), not just matched-and-marked waiters. A waiter whose tsleep already lapsed on its own deadline is not double-counted: `wakeup()` returns 0 in that race and we do NOT bump the count. User-side outcome remains consistent (consumer returns `ETIMEDOUT`; user re-checks atomic).
- **u32 `count` narrowing at the SVC** (F12, P3 audit) — `count_raw` is u64; the handler narrows to u32. The libt wrapper takes `unsigned int count`, bounding userspace using libt. A raw SVC caller passing `count > UINT32_MAX` has its high bits silently discarded; `UINT32_MAX` already means "wake all", so above-UINT32_MAX values are the same intent.
- **`uaccess_load_u32` alignment check is load-bearing** (F10, P3 audit) — the asm primitive's fault-fixup table catches translation / permission faults only; an unaligned LDR would alignment-fault outside the fixup class. `torpor_addr_va_ok` rejects unaligned addr_va at the C boundary.
- **Stack-allocated waiter** — the per-call waiter struct lives on the kernel stack. Every exit path unlinks it before the frame is reaped (the lifetime invariant). A future code change to this path MUST preserve the invariant; the `torpor_bucket_unlink_locked` call in the post-tsleep re-lock is the lifetime-enforcement seam.

---

## Audit-bearing posture

Sub-chunk 8 took a focused opus-prosecutor round (CLAUDE.md `§"Audit-triggering changes"`). **0 P0 / 0 P1 / 2 P2 / 10 P3**, all dispositioned:

- **F1 [P2] fixed**: `WAKE` count now reflects actual wakeups (`if (wakeup(...)) woken++`), not matched-and-marked.
- **F2 [P2] documented**: `wakeup()`'s `on_cpu` spin under `torpor_lock` is a v1.0-acceptable latency hazard; v1.x can split the lock or use a two-pass dispatch.
- **F3 [P3] fixed**: header proof sketch extended with the timeout-then-WAKE case.
- **F4 [P3] fixed**: `extinction` assert at WAIT entry pins `p == current_thread()->proc`.
- **F5 [P3] fixed**: `torpor.wait_rejects_unmapped_va` test asserts it runs on a kproc thread.
- **F6 [P3] fixed**: `_Static_assert(sizeof(struct torpor_waiter) % 8 == 0)` pins layout.
- **F7 [P3] documented**: `count == 0` is a no-op, no barrier.
- **F8 [P3] documented**: lock held across demand-paging.
- **F9 [P3] fixed**: added `torpor.wake_two_waiters_count_bound` test (8 total).
- **F10 [P3] fixed**: comment in `torpor_addr_va_ok` documents the alignment-fault class concern.
- **F11 [P3] withdrawn**: joey smoke is intentionally minimal; the kernel tests cover the EL0 EINVAL/EFAULT paths.
- **F12 [P3] documented**: libt's `t_torpor_wake` comment notes the silent u64→u32 narrowing at the SVC.

The audit's verdict on the prose validation: the I-9 specialization in `<thylacine/torpor.h>` "captures the canonical no-lost-wakeup case correctly and soundly"; the lock acquire/release pairing in `kernel/include/thylacine/spinlock.h` provides the happens-before edges the proof relies on. The post-audit header proof now also covers case (c) (the timeout-then-WAKE race; F3).

---

*Binding design: `docs/POUCH-DESIGN.md §7`. Phase pickup: `docs/phase6-status.md`. Spec discipline: `feedback_spec_to_code_suspended.md` (broadened 2026-05-23 — no `specs/futex.tla` is written; prose validation + audit + tests are the rigor floor).*
