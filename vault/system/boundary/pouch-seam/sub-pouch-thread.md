---
id: sub-pouch-thread
type: sub
parent: moc-pouch-seam
title: "pthreads and sleeping — over SYS_THREAD_SPAWN and torpor"
code:
  - usr/lib/pouch/patches/0004-pouch-pthread.patch
  - usr/lib/pouch/patches/0022-pouch-nanosleep.patch
audit: hard
guarded-by: [inv-i9]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

POSIX threads on a kernel with no `clone` and no futex. musl's pthread
layer is retargeted onto three Thylacine primitives — `SYS_THREAD_SPAWN`
(41), `SYS_THREAD_EXIT` (42), and torpor (`SYS_TORPOR_WAIT` 39 /
`SYS_TORPOR_WAKE` 40) — plus `SYS_BURROW_DETACH` for stack reclamation.
Sleeping (`nanosleep`, `usleep`, `clock_nanosleep`) lands here too,
because on Thylacine a sleep IS a wait-on-address with a timeout.

## Contract

- `pthread_create` → `SYS_THREAD_SPAWN(entry, sp_top, arg, tls, ptid)`.
- `pthread_exit` / the sched-failure early exit → `SYS_THREAD_EXIT`.
- Every futex wait/wake in the layer (`__wake`, `__futexwait`, `__wait`,
  `__futex4_cp`, the barrier's inline loop) → torpor, with the `priv`
  argument discarded.
- `__unmapself` (asm) → `SYS_BURROW_DETACH` then `SYS_THREAD_EXIT`.
- `clock_nanosleep(clk, flags, req, rem)` → a torpor wait on a private
  stack word nobody wakes, looped against a deadline.

## Mechanism

**The tid publication has two independent guarantees, and the history is
why.** Pre-#111 the parent assigned `new->tid` only AFTER
`SYS_THREAD_SPAWN` made the child runnable, so at `-smp>1` the child
could reach `__pthread_exit`'s `__tl_lock` with `tid` still 0 — and
`__tl_lock`'s recursive fast path (`lock == tid`, with `0 == 0` when the
lock is momentarily free) then returned WITHOUT acquiring, so the child
unlinked itself from the thread list holding no lock and crashed on
`self->prev == NULL`. #111 fixed it child-side: `start()` captures
`SYS_SET_TID_ADDRESS`'s return (the kernel-authoritative tid) into
`self->tid` before any `__tl_lock` can run. #112 then fixed it at the
root, kernel-side: the 5th spawn argument `ptid = &new->tid` restores
Linux's `CLONE_PARENT_SETTID`, so the kernel publishes the tid BEFORE
making the child runnable and the racy post-spawn store is gone. The
#111 self-set is RETAINED — it now writes the identical value as a
second, independent guarantee.

**The thread-list lock is the clear-child-tid target.** `start()`
registers `&__thread_list_lock` (not a per-thread word) as this Thread's
clear_child_tid address; `SYS_THREAD_EXIT` atomically zeroes it and
torpor-wakes `UINT32_MAX` waiters — Linux's `CLONE_CHILD_CLEARTID`
applied to a userspace spinlock, so a dying thread releases the list lock
to any spinner without a userspace unlock it could not reach. The
sched-failure early-exit path re-targets clear_child_tid to
`&args->control` first, so the parent's `__wait` observes the exit
through its own word.

**The stack pointer is aligned DOWN.** After the `start_args`
subtraction `stack` is 8-aligned (`sizeof(start_args)` is 8- but not
16-aligned), so `sp_arg = stack & ~0xF` meets the kernel's strict
16-alignment gate; `args` lives ABOVE the worker's SP so its frame
writes cannot overwrite it.

**Timeouts clamp at one hour, and must.** The kernel REJECTS
`timeout_us > TORPOR_MAX_TIMEOUT_US` (1 h) with `-EINVAL` rather than
clamping — and `__timedwait_cp`'s dispatch (`if (r != EINTR && r !=
ETIMEDOUT && r != ECANCELED) r = 0`) translates that `EINVAL` into "no
error, no wait", so the caller's `do…while` in `pthread_cond_timedwait`
spun at 100% CPU until the absolute deadline came back under an hour.
That was the threads round's first **P1** ([[fnd-threads9b-r1-f1]]);
pouch now clamps at the boundary so torpor returns a clean `ETIMEDOUT`
and the outer loop re-enters with a fresh sub-hour relative timespec.

**`clock_nanosleep` is the same shape, deliberately.** It computes a
deadline on the requested clock, then waits in ≤50-minute chunks under
the 1 h clamp, re-measuring each iteration; a spurious return costs one
re-measure. `-ETIMEDOUT` from torpor means the CHUNK completed, not the
sleep — the loop decides that by re-reading the clock. Before it landed,
`nanosleep` was an unwired sentinel, so every `SDL_Delay` and frame pacer
busy-returned instantly.

**Requeue is gone.** `unlock_requeue` cannot move a waiter from the
condvar word to the mutex word (torpor has no `FUTEX_REQUEUE`), so it
stores and plain-`__wake`s; waiters race for the mutex and re-sleep on
it. Functionally correct, loses the thundering-herd optimization under
heavy broadcast.

## Data structures

musl's `struct pthread` unchanged. `struct start_args` unchanged, placed
on the new thread's stack above its SP.

## Concurrency

The whole surface. Its wait/wake correctness is the kernel's
([[sub-kernel-torpor]]); pouch's contribution is (a) not losing a wake by
mis-clamping a timeout, (b) the tid ordering above, and (c) the
`cnt < 0 → INT_MAX` normalization in `__wake` with an explicit
`(unsigned int)` cast at the syscall site, so a future caller bypassing
the normalization cannot sign-extend a negative into a near-`UINT32_MAX`
"wake all" by accident.

## Invariants enforced

- **[[inv-i9]] (consumer side).** Every pouch wait is a torpor
  `(addr, expected, timeout)` whose compare-and-register the kernel does
  under `torpor_lock`; pouch's obligation is to pass an expected value it
  read before the decision to wait, which musl's futex idiom already
  guarantees. The F1 clamp bug was the one place pouch broke the contract
  — not by losing a wake, but by turning a legal wait into a spin.
- **P-3** — `pthread_attr_setprotocol(PRIO_INHERIT)` fails at attr-set
  time rather than degrading silently; `SYS_THREAD_SPAWN`'s `-EINVAL` is
  preserved (not collapsed to `EAGAIN`) so a bad `pthread_attr_setstack`
  is diagnosable.

## Error paths

`EAGAIN` for spawn failures (POSIX), except `EINVAL` preserved.
`ETIMEDOUT` from the clamped waits. `a_crash()` if
`SYS_SET_TID_ADDRESS` returns negative — defense-in-depth: the kernel
gates `tidptr` on 4-byte alignment and the user-VA bound, and
`&__thread_list_lock` is BSS-resident so it satisfies both today, but a
linker-script change landing it misaligned would otherwise silently
disable the handoff and hang every `pthread_join`.

## Performance

A spawn is one syscall. An uncontended lock/unlock never enters the
kernel (musl's userspace fast path is untouched). A contended wait is one
torpor call; since the #343 lock-free mismatch return, a wait whose value
already changed does not even take `torpor_lock`.

## Prosecution

- The 1-hour clamp must stay ≤ `TORPOR_MAX_TIMEOUT_US`; raising the
  kernel bound without raising this reintroduces the spin.
- Both tid guarantees must stay (the kernel publish AND the child's
  self-set); deleting either restores a race that only appears at
  `-smp>1`.
- `__unmapself` must remain the only stack-reclaiming path for a detached
  thread (a C path cannot munmap its own stack — SP would dangle).
- Any new futex-shaped call site must go through the four retargeted
  helpers, not a hand-rolled `SYS_futex` (which is a sentinel).

## Seams

[[seam-pouch-guard-pages]] (stack guard pages are silently absent) ·
[[seam-pouch-process-shared]] (`PTHREAD_PROCESS_SHARED` compiles and
links but does not synchronize cross-Proc — torpor's wake set is keyed on
the caller's Proc).

## Caveats

- **Stack guard pages do not exist.** musl allocates the stack
  `PROT_NONE` then mprotects the usable part RW; pouch's `mmap` ignores
  `prot` (always RW) and `mprotect` returns `ENOSYS`, which
  `pthread_create` tolerates by design (`&& errno != ENOSYS`). Overflow
  therefore corrupts the guard region instead of faulting, until it runs
  past the whole region into an unmapped page. Needs a kernel
  VMA-permission syscall.
- `pthread_cancel` sets the flag but cannot interrupt a blocked syscall
  (no `SIGCANCEL`); `pthread_atfork` is a no-op (no fork);
  `SYS_set_robust_list` is a sentinel, so a thread that dies mid-hold
  leaves a robust mutex held with no kernel-side cleanup.
- `tl_lock_count` would go stale if a future caller took
  `__thread_list_lock` recursively and then exited (the kernel force-zeroes
  the lock; the count does not follow). Unreachable today — every caller
  takes it non-recursively.
- The sched-failure early-exit branch is unexercised by
  `/pouch-hello-threads`.
- C11 `thrd_create` (the `start_c11` path) is patched but untested.

## Provenance

[[chg-2026-05-23-p6-threads-b]] (0004 + [[adt-threads9b-r1]], 2 P1) →
#111 / #112 (the tid ordering, kernel-side; see
[[sub-kernel-thread]]) → [[chg-2026-07-20-g7a-sdl-seam]] (0022,
`nanosleep` onto torpor for the SDL frame pacers).
