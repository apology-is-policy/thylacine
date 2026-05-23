# 82 — pouch pthread: the POSIX threading layer (P6-pouch-threads sub-chunk 9b)

The userspace half of pouch's POSIX threading — the boundary-line patch (`0004-pouch-pthread`) that retargets musl's `src/thread/` onto the kernel substrate from sub-chunks 7-9a (`SYS_THREAD_SPAWN` / `SYS_THREAD_EXIT` / `SYS_TORPOR_WAIT` / `SYS_TORPOR_WAKE` / `SYS_BURROW_DETACH`). pouch's pthread surface — `pthread_create` / `_join` / `_detach` / `_mutex_*` / `_cond_*` / `_rwlock_*` / `_once` / `_key_*` / `_barrier_*` — comes from this layer.

Per `POUCH-DESIGN.md §7 [RESOLVED 7.3]` (pouch pthreads ARE Thylacine Threads within one Proc) + `§14` (sub-chunk 9b is the pouch side of `pouch-threads`). Audit-trigger surface (`CLAUDE.md` / `ARCHITECTURE.md §25.4`): the `thread_spawn / thread_exit` row covers both the kernel side (9a) and the boundary-line patch (this chunk).

---

## Purpose

POSIX programs call `pthread_create(t, attr, f, arg)`, `pthread_mutex_lock(m)`, `pthread_cond_wait(c, m)`, etc., and expect them to behave per POSIX.1. pouch presents that surface from musl's portable upper half (most of `src/thread/` is the same C code Linux/musl uses for mutex/cond/rwlock state machines) by replacing musl's LOWER half — the OS-boundary calls — with calls onto Thylacine's primitives.

Eight files in the boundary-line patch series; one mechanical retarget:

| musl file | What it does | pouch retarget |
|---|---|---|
| `arch/aarch64/bits/syscall.h.in` | The syscall-number table | Adds `__NR_torpor_wait=39`, `__NR_torpor_wake=40`, `__NR_thread_spawn=41`, `__NR_thread_exit=42` (Thylacine extensions; no Linux equivalent). musl's build-time sed pass auto-generates `SYS_*` aliases. |
| `src/internal/pthread_impl.h` | Inline `__wake` / `__futexwait` over `SYS_futex` | `__wake` → `__syscall(SYS_torpor_wake, addr, cnt)`; `__futexwait` → `__syscall(SYS_torpor_wait, addr, val, -1)` (indefinite). `priv` discarded (torpor is per-Proc by construction). |
| `src/thread/__timedwait.c` | `__futex4_cp` — the timed-wait primitive | Rewritten to translate musl's `struct timespec *` relative deadline into torpor's relative microsecond timeout, with overflow guard `max_sec = (LONG_MAX - 999999) / 1000000`. Drops the `SYS_futex_time64` fallback cascade. |
| `src/thread/__wait.c` | The kernel-blocking spin-then-wait helper | Single-site retarget: `SYS_futex FUTEX_WAIT` → `SYS_torpor_wait(addr, val, -1)`. The 100-iteration userspace spin is unchanged. |
| `src/thread/pthread_barrier_wait.c` | The barrier-wait inline futex call | Same single-site retarget. |
| `src/thread/pthread_cond_timedwait.c` | `unlock_requeue` (FUTEX_REQUEUE) | Replaced with plain `__wake(l, 1, 1)` — torpor has no `FUTEX_REQUEUE` equivalent; functionally correct (waiters wake, race for the mutex, re-sleep if needed), loses the requeue optimization. |
| `src/thread/pthread_create.c` | The lifecycle (3 sites) | (1) `__pthread_exit`'s tail `for(;;) SYS_exit(0)` → `for(;;) SYS_thread_exit`. (2) `start()` adds `SYS_set_tid_address(&__thread_list_lock)` at thread entry; the sched-failure early-exit path's `for(;;) SYS_exit(0)` → `for(;;) SYS_thread_exit`. (3) `__pthread_create`'s `__clone(...)` → `__syscall(SYS_thread_spawn, entry, sp_top, args, TP_ADJ(new))`; `new->tid` is assigned from the syscall return. |
| `src/thread/aarch64/__unmapself.s` | The detached-stack teardown asm | `mov x8,#215 (SYS_munmap)` → `mov x8,#38 (SYS_BURROW_DETACH)`; `mov x8,#93 (SYS_exit)` → `mov x8,#42 (SYS_THREAD_EXIT)`. C-side ABI unchanged. |

Untouched: `arch/aarch64/clone.s` (the `__clone` Linux-syscall wrapper, now unreferenced; static-link DCE drops it from `libc.a`'s effective surface).

---

## The clear-child-tid handoff — Thylacine-side mechanics

Linux/musl uses `CLONE_CHILD_CLEARTID` to wire the new thread's `&__thread_list_lock` as its kernel-tracked clear-tid address; on the thread's `SYS_exit`, the kernel atomically zeros that word + futex-wakes spinners. The pattern is "exit futex address pointing at the lock" — a thread holds `__thread_list_lock` during the linked-list manipulation in `__pthread_exit`, then calls `SYS_exit`, and the kernel atomically zeros the lock + wakes any other thread spinning on `__tl_lock`.

pouch replicates this exactly, in two steps:

1. `start()` (the new thread's entry trampoline) calls `__syscall(SYS_set_tid_address, &__thread_list_lock)` at thread entry. This registers the shared `__thread_list_lock` word as the new Thread's `clear_child_tid` (kernel side: `t->clear_child_tid` per `81-sys-thread.md`).
2. `__pthread_exit`'s tail calls `__syscall(SYS_thread_exit)` (no args). The kernel atomically: (a) `uaccess_store_u32(t->clear_child_tid, 0)` — zero the lock word, (b) `sys_torpor_wake_for_proc(p, t->clear_child_tid, UINT32_MAX)` — wake every spinner, (c) marks the Thread `EXITING`, (d) if last live Thread, transitions the Proc to `ZOMBIE` with status 0.

The race-freedom invariant (kernel side, I-9 specialized): the lock-zero + wake pair is a single atomic transition from the spinner's perspective. The spinner's loop is `while ((val = a_cas(&__thread_list_lock, 0, tid))) __wait(&__thread_list_lock, &tl_lock_waiters, val, 0);` — `__wait` checks `*addr == val` AFTER registering the waiter with the kernel, so even if the wake fires between the value-check and the sleep, the value-check observes the new (zero) value and bypasses the sleep. No lost wakeup.

The Linux-equivalent invariant is enforced symmetrically on the pouch side: every non-main thread's exit path passes through `__pthread_exit`, which holds `__thread_list_lock` at SYS_THREAD_EXIT time (acquired via `__tl_lock` just before the linked-list manipulation). The kernel's zero-the-lock action only happens at exit; no other path zeros it.

### The sched-failure early-exit path

`pthread_create` supports `pthread_attr_setschedpolicy` / `setschedparam` (the `attr._a_sched` bit). pouch declines `SYS_sched_setscheduler` (the seam returns `-ENOSYS`), so any attr with `_a_sched` set will fail at the post-spawn `__syscall(SYS_sched_setscheduler, ...)` call. The parent then sets `args->control = 3` and waits on it via `__wait(&args->control, 0, 3, 0)`.

The new thread, in `start()`, observes `args->control == 1` initially. After `a_cas(&args->control, 1, 2)`, the child waits on it; the parent later swaps it to 3 (failure). The child then sees `args->control == 3` and falls into the early-exit path:

```c
__syscall(SYS_set_tid_address, &args->control);  // re-target ctid to args->control
for (;;) __syscall(SYS_thread_exit);
```

The first call re-targets `clear_child_tid` from `&__thread_list_lock` to `&args->control` so the kernel's exit-time zero hits THAT word; the parent's `__wait` observes the zero (via the kernel's torpor_wake) and proceeds. This path does NOT release `__thread_list_lock` because the child never acquired it — clearly a path that doesn't manipulate the thread linked list at all.

### The main thread

The main thread of a Proc was NOT created via `pthread_create`; it was the original Thread set up by `exec_setup`. It doesn't call `__syscall(SYS_set_tid_address, &__thread_list_lock)` at startup. When the main thread calls `pthread_exit`, `__pthread_exit` observes `self->next == self` (single-threaded) and calls `exit(0)` — process exit, not thread exit. No SYS_THREAD_EXIT.

---

## The stack-allocation story

pouch's `pthread_create` allocates the new Thread's stack via `__mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)` — which `0003-pouch-mman.patch` retargets onto `SYS_BURROW_ATTACH(size)`. The kernel chooses the VA in the burrow-attach window (4 GiB-64 TiB; per `79-sys-burrow.md`). The stack is demand-zero — fresh pages on first touch.

The args struct (`struct start_args` — start_func, start_arg, control, sig_mask) lives on the new thread's stack, exactly as in Linux/musl. pthread_create computes:

```c
stack -= (uintptr_t)stack % sizeof(uintptr_t);   // 8-align
stack -= sizeof(struct start_args);              // make room
struct start_args *args = (void *)stack;
// initialize args...
long sp_arg = ((long)(uintptr_t)stack) & ~0xFL;  // 16-align DOWN
ret = __syscall(SYS_thread_spawn,
    (long)(uintptr_t)start_func, sp_arg,
    (long)(uintptr_t)args, (long)(uintptr_t)TP_ADJ(new));
```

The 16-align-DOWN may move SP below the args struct (if the 8-aligned `stack` value was not 16-aligned). args remains valid because the SP is BELOW it; the args pointer (`arg_va`) is the original 8-aligned `stack` value, and the args struct spans `[stack, stack+sizeof(start_args))`. The new thread's function prologue pushes BELOW SP, never touching args. The kernel `entry_va`-alignment gate (4-byte, per F2 from 9a) is satisfied by the static function pointer's natural alignment (C language guarantees ≥4-byte for function pointers on aarch64).

`TP_ADJ(new)` (the thread-local-storage base address) is passed as `tls_va`. The kernel writes that to `TPIDR_EL0` at thread eret (per `thread_user_trampoline`). The new thread's first instruction sees `TPIDR_EL0` pointing at its pthread struct + TLS area; `__pthread_self()` works from that.

### Stack reclamation — `__unmapself`

A detached thread (`pthread_detach` or `pthread_attr_setdetachstate(PTHREAD_CREATE_DETACHED)`) is responsible for unmapping its own stack at exit. The thread CANNOT use the C path (`__munmap` then `SYS_THREAD_EXIT`) because once `__munmap` succeeds, SP is dangling — the next pushed frame would fault.

`src/thread/aarch64/__unmapself.s` is the aarch64 primitive for this. With our retarget:

```asm
__unmapself:
    mov x8, #38   // SYS_BURROW_DETACH
    svc 0
    mov x8, #42   // SYS_THREAD_EXIT
    svc 0
```

The C-side ABI is unchanged: `__unmapself(map_base, map_size)` puts `x0 = base`, `x1 = size` (AAPCS64). The kernel's `SYS_BURROW_DETACH(vaddr, length)` consumes them exactly. After the burrow detach, the thread's stack VA is gone; SP_EL0 dangles but the instruction stream (in `.text`, separate VMA) is unaffected — the second `svc 0` fetches normally. `SYS_THREAD_EXIT` never returns; the dangling SP_EL0 is never dereferenced.

---

## Futex semantics — what pouch implements

pouch's `__wake` / `__wait` / `__futexwait` / `__timedwait` all route through `SYS_TORPOR_WAIT` / `SYS_TORPOR_WAKE` (sub-chunk 8). Key differences from Linux futex:

- **No `FUTEX_PRIVATE` distinction.** Linux futexes have two tiers (process-shared and per-process); torpor is structurally per-Proc (the kernel's hash key is `(Proc *, addr_va)`). The `priv` argument to the helpers is discarded.
- **No `FUTEX_REQUEUE`.** `pthread_cond_broadcast` under heavy contention loses the optimization where waiters are atomically moved from the cond's futex to the mutex's futex without waking. pouch's replacement is to wake them; they race to acquire the mutex, and the loser re-sleeps on it. Functionally correct.
- **No `FUTEX_LOCK_PI` / `FUTEX_UNLOCK_PI`.** Priority-inheritance mutexes (per `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`) are not implemented at v1.0. The call sites in `pthread_mutex_*.c` still issue `__syscall(SYS_futex, ...)`, which reaches the `0xFFFF` sentinel and returns `-ENOSYS`. A program that calls `setprotocol(PRIO_INHERIT)` will get the failure at the attr-set call; runtime PI lock attempts degrade silently (the mutex still functions correctly, just without PI semantics).
- **No `FUTEX_WAIT_BITSET` / `FUTEX_WAKE_BITSET`.** Not consumed by the pthread layer.
- **Timeout semantics.** Linux's futex takes a `struct timespec` (absolute deadline if `FUTEX_CLOCK_REALTIME`; relative otherwise). torpor takes a relative microsecond timeout (signed s64; negative = block indefinitely). pouch's `__timedwait_cp` converts caller-absolute deadlines to relative timespec (above the retarget) and the retarget converts that to microseconds with overflow clamping. `TORPOR_MAX_TIMEOUT_US` (kernel-side, 1 hour) caps anything longer.

---

## Public API

The patched musl headers + linker expose the standard POSIX 1.c surface:

```c
// pthread_create.h equivalents (declarations in <pthread.h>)
int  pthread_create(pthread_t *, const pthread_attr_t *,
                    void *(*start_routine)(void *), void *arg);
int  pthread_join(pthread_t, void **retval);
int  pthread_detach(pthread_t);
void pthread_exit(void *retval) __attribute__((noreturn));

int  pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int  pthread_mutex_lock(pthread_mutex_t *);
int  pthread_mutex_trylock(pthread_mutex_t *);
int  pthread_mutex_timedlock(pthread_mutex_t *, const struct timespec *);
int  pthread_mutex_unlock(pthread_mutex_t *);
int  pthread_mutex_destroy(pthread_mutex_t *);

int  pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int  pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int  pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int  pthread_cond_signal(pthread_cond_t *);
int  pthread_cond_broadcast(pthread_cond_t *);
int  pthread_cond_destroy(pthread_cond_t *);

int  pthread_rwlock_*       // full set (init / destroy / rdlock / wrlock / trylock / unlock / timed variants)
int  pthread_once(pthread_once_t *, void (*)(void));
int  pthread_key_create(pthread_key_t *, void (*)(void *));
void *pthread_getspecific(pthread_key_t);
int  pthread_setspecific(pthread_key_t, const void *);
int  pthread_barrier_init(pthread_barrier_t *, const pthread_barrierattr_t *, unsigned);
int  pthread_barrier_wait(pthread_barrier_t *);
int  pthread_barrier_destroy(pthread_barrier_t *);

pthread_t pthread_self(void);
int       pthread_equal(pthread_t, pthread_t);
```

All static functions; no `.so` (v1.0 is static-only per POUCH-DESIGN §2.2).

### Documented errors

| Surface | Error | When |
|---|---|---|
| `pthread_mutexattr_setprotocol(attr, PTHREAD_PRIO_INHERIT)` | `ENOTSUP` (Linux: -ENOSYS-translated) | PI mutexes deferred to v1.x |
| `pthread_cancel(t)` | `ENOTSUP` / no-op cancellation | Cancellation deferred — SIGCANCEL never fires |
| `pthread_atfork(...)` | weak alias to dummy | fork() declined (POUCH-DESIGN §8.3); atfork handlers never fire |
| `pthread_setschedparam(t, ...)` | `EINVAL` or `ENOSYS` propagated | SYS_sched_setscheduler is 0xFFFF |
| `pthread_kill(t, sig)` | depends on signal infra | Notes-as-signals deferred to sub-chunk 11 |
| `PTHREAD_PROCESS_SHARED` on mutex/cond/rwlock/barrier | Sets the attr but cross-Proc semantics aren't provided | torpor is per-Proc; fork() declined |

---

## The proving binary — `/pouch-hello-threads`

`usr/pouch-hello/pouch-hello-threads.c`: closes POUCH-DESIGN.md §13's multithreaded-test exit criterion.

```c
#define NTHREADS         5u
#define ITER_PER_THREAD  1000u
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_counter = 0;

static void *worker(void *arg) {
    for (unsigned i = 0; i < ITER_PER_THREAD; i++) {
        pthread_mutex_lock(&g_mtx);
        g_counter++;
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

int main(void) {
    pthread_t tids[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++)
        pthread_create(&tids[i], NULL, worker, NULL);
    for (unsigned i = 0; i < NTHREADS; i++)
        pthread_join(tids[i], NULL);
    // verify g_counter == NTHREADS * ITER_PER_THREAD
}
```

Boot-time output:

```
pouch-hello-threads: 5 threads, 1000 iters each
pouch-hello-threads: counter = 5000 (expected 5000)
pouch-hello-threads: ok (5 workers, mutex-protected counter, joined)
pouch-hello-threads: exit 0
joey: pouch-hello-threads smoke ok (pthread + mutex over SYS_THREAD_* / SYS_TORPOR_*)
```

Every boundary-line site is exercised: `SYS_THREAD_SPAWN` (the spawns), `SYS_TORPOR_WAIT` / `SYS_TORPOR_WAKE` (the mutex contention path), `SYS_THREAD_EXIT` (the worker exits, the clear_child_tid handoff to `__thread_list_lock`, the kernel's torpor_wake). The counter is the race-detector: any lost increment trips the COUNT MISMATCH return, which propagates to `_Exit(6)` → joey's content-check fails.

joey content-checks for `pouch-hello-threads: exit 0`. Detection of any pthread bug — a race on the counter, a lost wakeup, a missed join — produces a non-"exit 0" boot log, which joey reads as failure.

---

## Spec cross-reference

**Spec-to-code FULLY suspended project-wide** (broadened 2026-05-23 per `feedback_spec_to_code_suspended.md`). No `specs/futex.tla` or `specs/pthread.tla` is written. The invariants are validated by:

- **Prose reasoning** in this reference doc + the patch's prose preamble.
- **The audit round** — focused opus prosecutor over the patch + the proving binary.
- **The runtime tests** — `/pouch-hello-threads` boots and passes every run (CI matrix: default + UBSan).

The buggy cfgs of EXISTING specs (`scheduler.tla`, `corvus.tla`, `burrow.tla`, `poll.tla`, `tsleep.tla`, ...) remain pre-commit gates for impl changes that touch those mechanisms. Sub-chunk 9b touches none of them — pouch is a userspace patch layer; the kernel mechanisms it consumes were spec'd before suspension or are validated by prose post-suspension.

---

## Tests

- **`/pouch-hello-threads`** (this chunk): the multi-thread proving binary. Every boot.
- **`/thread-probe`** (9a, retained): the single-spawn-and-join probe via libt's `t_thread_spawn` / `t_thread_exit` / `t_torpor_wait` — kernel-substrate sanity. Every boot.
- **joey smoke** (`do_pouch_hello_smoke` in `usr/joey/joey.c`): content-checks `pouch-hello-threads: exit 0` on the boot log.
- **Kernel test suite**: 561/561 PASS × default + UBSan (unchanged from 9a — the kernel didn't change in 9b).

The kernel tests do NOT directly exercise the pouch-side patch (kernel tests run in-kernel before joey starts, before pouch-hello-threads runs). The proving binary IS the test for the boundary-line patch.

---

## Error paths

| Caller | Error | Return |
|---|---|---|
| `pthread_create` | kernel-side `SYS_THREAD_SPAWN` returned `-ENOMEM` (Thread cache or kstack alloc) | `EAGAIN` (Linux-equivalent) |
| `pthread_create` | kernel-side `SYS_THREAD_SPAWN` returned `-EINVAL` | `EAGAIN` (the SPAWN args from pthread_create should never trigger EINVAL — entry is a valid C function pointer, stack came from a successful mmap, tls from `__copy_tls`; an EINVAL indicates a pouch bug, not a user bug) |
| `pthread_join` | thread already exited or detached | `EINVAL` / `ESRCH` (musl's pthread_join logic, unchanged) |
| `pthread_mutex_*timedlock` | timeout elapsed | `ETIMEDOUT` (translated from torpor's `TORPOR_ERR_ETIMEDOUT = -110`) |
| `pthread_mutex_*timedlock` | bad timespec | `EINVAL` (musl's clock_gettime / range check above the retarget) |
| `pthread_mutexattr_setprotocol(PRIO_INHERIT)` | PI mutexes not implemented | `ENOTSUP` (the FUTEX_LOCK_PI call returns -ENOSYS; musl's protocol handling translates) |

---

## Performance characteristics

- **Uncontended mutex lock/unlock**: zero syscalls (pure userspace `a_cas`). Same as Linux/musl.
- **Contended mutex lock**: one `SYS_TORPOR_WAIT` syscall per sleeping waiter. Per-syscall cost is the SVC entry path + torpor's hash-table lookup + `tsleep` setup; per `80-torpor.md`, the wait latency is dominated by the timer-tick granularity (TBD, not yet measured).
- **`pthread_cond_broadcast` under contention** (N waiters): one `__wake(l, INT_MAX, ...)` → one `SYS_TORPOR_WAKE` that wakes all N — they race for the mutex, N-1 re-sleep via N-1 `SYS_TORPOR_WAIT` calls. Linux's `FUTEX_REQUEUE` avoids the N-1 re-sleeps; pouch eats them. For N ≤ ~10, the difference is in the microsecond range; for N >> 10, pouch trends linearly worse. v1.0 doesn't have a benchmark that triggers the regression — stratumd uses ≤ 4 threads typically.
- **pthread_create / pthread_exit / pthread_join**: dominated by the kernel-side spawn cost (Thread + kstack alloc, page-table setup) and the user-stack mmap. Each ~150 µs on a quiet host (rough estimate; not yet measured).

---

## Audit posture

Focused opus-prosecutor round (P6-pouch-threads-b): **0 P0 / 2 P1 / 3 P2 / 10 P3**, all dispositioned at the substantive close commit. The two P1s (**F1** — pthread_cond_timedwait > 1 hour timeout silently infinite-loops because the kernel rejects long timeouts with EINVAL which musl's __timedwait_cp maps to "no error, no wait"; **F2** — stack guard pages silently disabled because pouch's mmap ignores PROT_NONE and mprotect returns ENOSYS) are concrete real-world hazards the runtime test did not surface (the test uses default timeouts and doesn't deep-recurse). F1 fixed by clamping `timeout_us` at TORPOR_MAX_TIMEOUT_US in pouch (kernel returns ETIMEDOUT cleanly; caller's outer loop re-evaluates). F2 fixed by **documentation only** at v1.0 — the real fix needs a new kernel syscall to flip VMA permissions (PROT_NONE-capable); deferred to v1.x. Closed list: `audit_p6_pouch_threads_9b_closed_list.md`.

## Status

**Sub-chunk 9b LANDED** (substantive close `551be97`). The patch series at `usr/lib/pouch/patches/` is now 4 patches (0001-0004). Build target `tools/build.sh sysroot` produces a 2,398,772-byte `libc.a` carrying the patched pthread layer; `tools/build.sh pouch-progs` builds `/pouch-hello-threads` (~60 KiB ET_EXEC).

Boot posture: 561/561 PASS × default + UBSan, 0 UBSan errors, `/pouch-hello-threads` green on every boot.

POUCH-DESIGN.md §13 exit-criteria progress:
- ✓ `aarch64-thylacine` cross-toolchain complete (compiler + libc + CRT + runtime)
- ✓ Static "hello" C program runs (sub-chunk 5)
- ✓ `printf`-shaped hello works (sub-chunk 6c)
- ✓ **Multithreaded test program runs** (this chunk — POUCH-DESIGN.md §13 box ticked)
- ☐ `AF_UNIX` `SOCK_STREAM` echo client/server (sub-chunks 10-12)
- ☐ libsodium cross-compiles (later sub-chunks)
- ☐ stratumd compiles + boots (the Phase 6 finale)
- ☐ TSan multithread test (sanitizer support deferred)

Naming retention: standard musl/POSIX names (`pthread_*`). No Thylacine-themed rename — the pouch surface is what POSIX C programs expect to see, and a thematic rename here would defeat the purpose.

---

## Known caveats / footguns

- **Stack guard pages are SILENTLY DISABLED at v1.0 (F2 audit).** musl's pthread_create allocates `[map, map+size)` with PROT_NONE then mprotects the writable portion to PROT_READ|PROT_WRITE. pouch's `__mmap` (0003-pouch-mman) ignores `prot` and always returns RW; `__mprotect` returns -ENOSYS, silently tolerated by pthread_create's `&& errno != ENOSYS` guard. **Net effect**: the whole pthread stack region is RW, including the "guard" bytes at the bottom. Stack overflow corrupts the guard region instead of immediately faulting; only an overflow past the entire stack region (default = 128 KiB usable + 8 KiB nominal "guard") hits an unmapped page in the next VMA and faults. Stratum-class workloads (stratumd, libsodium) don't deep-recurse → bounded hazard at v1.0. **Real fix**: needs a new kernel syscall to flip VMA permissions (PROT_NONE-capable) — deferred to v1.x. Workaround for sensitive workloads: allocate a larger stack with `pthread_attr_setstacksize` to add headroom past the corruption-tolerant zone.

- **`pthread_cond_timedwait` with > 1 hour relative deadline clamps to 1 hour (F1 audit close).** The kernel rejects timeouts > TORPOR_MAX_TIMEOUT_US (1 hour) with EINVAL; pouch's `__futex4_cp` clamps at the 1-hour boundary so torpor returns ETIMEDOUT cleanly. The caller's outer loop in `pthread_cond_timedwait` re-evaluates the absolute deadline and re-enters with a fresh sub-1-hour relative timespec. Net behavior: a > 1 hour wait wakes spuriously every hour to re-check; the deadline is still honored correctly.

- **`SYS_THREAD_SPAWN` with `tls_va = 0` is permitted by the kernel but causes the worker to die on first `__pthread_self()` deref** (F9 audit, defense-in-depth). pthread_create always passes a valid `TP_ADJ(new)` so the path isn't reachable from pouch today; native callers using libt's `t_thread_spawn` directly with `tls = NULL` must `msr tpidr_el0, x_valid_addr` before any pthread library call.

- **PI mutex paths degrade with reduced fidelity** (F10 audit). `pthread_mutexattr_setprotocol(PRIO_INHERIT)` returns ENOSYS at attr-set time (the kernel's SYS_futex sentinel returns -ENOSYS via the seam). A program that bypasses `setprotocol` (e.g., manually sets `_m_type & 8`) and calls `pthread_mutex_timedlock` on a "PI" mutex falls into a busy-wait fallback that returns ETIMEDOUT after the deadline — not ENOSYS. The recommended pattern is to use `setprotocol` and check its return.

- **`tl_lock_count` stale-state hazard is LATENT** (F11 audit). If a future caller takes `__thread_list_lock` recursively then exits via SYS_THREAD_EXIT, the kernel force-zeros the lock but `tl_lock_count` (a per-process static int in pthread_create.c) stays incremented — next CAS-acquirer's unlock decrements the stale count instead of releasing → deadlock. All current `__tl_lock` callers (pthread_create, pthread_exit, pthread_key_create, synccall, membarrier) take it non-recursively, so unreachable today. A future caller introducing recursive lock-then-exit would need to reset `tl_lock_count = 0` on successful CAS-acquire.

- **`__unmapself` ignores SYS_BURROW_DETACH error and leaks the stack VMA** (F12 audit). The asm primitive does the burrow detach + thread exit unconditionally; if the kernel rejects the detach (window-confinement, bad length), the stack VMA leaks until proc death. Unreachable today — pthread_exit passes `self->map_base` + `self->map_size` from a successful SYS_BURROW_ATTACH so the kernel always accepts.

- **The sched-failure early-exit path is unexercised by `/pouch-hello-threads`** (F13 audit). `start()`'s `args->control == 3` branch (reachable via `pthread_attr_setschedpolicy`, which fails because SYS_sched_setscheduler returns -ENOSYS) lacks a runtime test. A regression in that branch would land undetected; a second test program with explicit sched-failure would close it (not 9b-scoped).

- **`PTHREAD_PROCESS_SHARED` attributes compile + link + run but the cross-Proc semantics aren't provided.** A pthread_cond / mutex / rwlock / barrier with the `pshared` attribute set will SET the attr, but two pouch Procs sharing one memory region will NOT synchronize through it correctly — torpor's wake-set is keyed on the caller's Proc *, so a wake from Proc A doesn't reach a waiter in Proc B. v1.0 lacks the cross-Proc shared-memory machinery this would require; POUCH-DESIGN §8.2 documents it as unsupported.

- **`pthread_cancel` doesn't.** musl's cancellation machinery is intact in the upper half but the `SIGCANCEL` signal never fires (notes-as-signals is sub-chunk 11+). A program that calls `pthread_cancel(t)` will set the cancel flag; the target thread will check it at the next cancellation point but `SIGCANCEL` won't interrupt a blocking syscall — the target stays blocked. v1.0-cancellation is best-effort cooperative; POUCH-DESIGN §7 documents it as deferred.

- **`pthread_atfork` is a no-op.** pouch declines `fork()` (POUCH-DESIGN §8.3); `pthread_atfork` is weak-aliased to a dummy.

- **Detached threads with mmap'd stacks always go through `__unmapself`.** A `PTHREAD_CREATE_DETACHED` thread cannot have a caller-provided stack (`pthread_attr_setstack`) and use `__unmapself` — the asm primitive only handles the mmap'd-stack case. musl's `__pthread_exit` checks `state==DT_DETACHED && self->map_base` before calling `__unmapself`, so the caller-provided-stack path is naturally excluded. But that means a detached thread with a caller-provided stack LEAKS its bookkeeping (`map_base == 0` → no detach action). musl-upstream behavior; pouch inherits it. Workaround: caller manages the stack lifetime explicitly.

- **The `__thread_list_lock` clear_child_tid pattern requires that EVERY non-main pthread thread reach `__pthread_exit` via the normal path.** A thread that exits via `_Exit(N)` / `abort()` / `_exit(N)` will process-terminate the whole Proc (those all map to `SYS_EXITS`), not just the thread. That's the POSIX-correct behavior for `_Exit` — `pthread_exit` is the per-thread exit, `_exit` / `_Exit` are process-wide. The distinction is intentional.

- **No `SYS_set_robust_list`.** musl's robust-list cleanup (`__pthread_exit`'s loop over `self->robust_list.head`) still runs but doesn't register with the kernel (the syscall hits the 0xFFFF sentinel → -ENOSYS). Means: robust mutexes work for live threads (the userspace cleanup loop fires), but a thread that crashes mid-mutex-hold leaves the mutex held without kernel-side cleanup. Robust mutexes are deferred per POUCH-DESIGN §7.

---

## Naming rationale

The retargeted syscalls keep Thylacine-native names: `torpor` (the deep-sleep state — marsupial dormancy, the futex-equivalent), `thread_spawn` / `thread_exit` (utilitarian — there's no marsupial equivalent that adds clarity), `burrow_detach` (the pouch's nursery teardown). pthread_* names themselves stay POSIX-standard — pouch is explicit about presenting POSIX from the user's perspective, even when the implementation is Thylacine-native underneath.
