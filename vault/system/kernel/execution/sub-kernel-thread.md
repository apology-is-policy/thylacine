---
id: sub-kernel-thread
type: sub
title: "The Thread: context, kstack, and the on_cpu protocol"
parent: moc-kernel-execution
code: ["kernel/thread.c", "kernel/include/thylacine/thread.h"]
audit: hard
guarded-by: [inv-i21, inv-i31, inv-i44, inv-i48]
validated-by: [gate-smp, spec-thread-reap]
locks: [lock-proc-threads, lock-proc-table]
created: 2026-08-01
updated: 2026-10-22
---
## Purpose

A `Thread` is a schedulable register context with a kernel stack, a Proc
backref, and a run state. This dossier owns its allocation, its five
creation shapes, its stack geometry, and the one property that makes freeing
it safe: `on_cpu`.

Thread identity lives in `TPIDR_EL1`, the ARMv8 per-CPU OS-reserved
register, so `current_thread()` is two instructions and SMP-correct with no
per-CPU array indirection. `TPIDR_EL1` reads NULL before `thread_init` (BSS
default), which several guards rely on.

## Contract

| Constructor | Runs at | First switch-in lands at | kstack |
|---|---|---|---|
| `thread_create` / `thread_create_with_arg` | EL1 | `thread_trampoline` → `blr entry` | own 32 KiB alloc |
| `thread_create_user` | EL0 | `thread_user_trampoline` → `eret` | own 32 KiB alloc |
| `thread_create_forked` | EL0 | `thread_fork_trampoline` → the shared exception return | own 32 KiB alloc |
| `thread_init` (kthread) / `thread_init_per_cpu_idle` | EL1 | never (born RUNNING) | none — the boot stack |
| `thread_create_bootcpu_idle` | EL1 | `thread_trampoline` | none — a dedicated BSS stack |

`thread_free(t)` releases the descriptor + kstack of a kernel thread (by its
joiner) or of a thread that never ran (by its creator); the caller must ensure
`t` is not current. An exited user thread is `thread_reap_dead`'s alone, and
`thread_free` extincts on one. `thread_switch(next)` is the **test-only**
direct-switch primitive — production multitasking is `sched()`.

**A thread's end of life** (ARCH 7.3.2, [[inv-i48]]). A thread that exits
(`thread_exit_self`, or `exits` for the last one) commits `THREAD_EXITING`
under [[lock-proc-threads]] and `sched()`s away for good. For a user thread
(its Proc is not kproc) the code on the far side of that switch frees it:
`sched_resume_tail` ([[sub-kernel-sched]]), run by the thread switched to, on
the same CPU, before anything else runs there. It reads EXITING while `on_cpu`
still pins the thread, clears `on_cpu`, releases the run-queue lock and then,
with IRQs still masked, calls `thread_reap_dead`. That scans the dead stack's
depth before any lock (nothing writes a dead stack), takes the Proc's thread
lock once to fold `run_ns` and the depth into the Proc (so `proc_cpu_ns` and
`proc_kstack_peak` never shrink), clear a debug focus that names it (the focus
is validated by pointer, and a recycled slot would match) and unlink it, drops
the lock (its last touch of the Proc), and frees the stack and the slot. Each
thread has exactly one final switch-out and nothing else frees a user thread,
so no claim is needed. A CPU holds at most one exited thread not yet freed: the
one running its tail, or the one its resume path is freeing. A kernel thread is
freed by its joiner; a thread that never ran, by its creator.

`thread_create_user` deliberately does **not** validate its four user-VA
arguments; the syscall handler owns those bounds. A malformed address faults
at EL0, which is a per-Proc termination, not a kernel problem.

## Mechanism

**Stack geometry.** One order-3 (32 KiB) buddy allocation per thread, split
in half: the low 16 KiB is a guard region marked no-access in the kernel
direct map (`mmu_set_no_access_range`), the high 16 KiB is the usable stack
with `ctx.sp` starting at its top edge. `kstack_base` names the **low**
address (the guard base), so `free_pages` and the guard restore both work
from it. Overflow past the usable region lands in the guard and faults.

The 32 KiB allocation is 32 KiB-aligned, so all four guard pages fall inside
one 2 MiB block: one L3 (pre-demoted at boot, #808) and one batch of by-VA
invalidates ([[sub-kernel-mmu]]).

**The stack cache.** A freed stack does not go straight back to the buddy.
`kstack_free` pushes it on a bounded LIFO (`THREAD_KSTACK_CACHE_MAX`, 32
blocks, 1 MiB of the TCB reserve) with its guard still no-access, and
`kstack_alloc` pops the most recent block before it asks the buddy. Past the
bound a freed stack is released -- guard restored, `free_pages` -- as before.
So a create that finds a block, and a free that finds room, write no PTE and
invalidate nothing, where every create and every free used to flush every
CPU's whole TLB. Whatever its source, a block is poisoned whole before it is
handed out. That resets the watermark, and it is the only scrub a reused block
gets, so it is also what keeps a dead thread's frames off the next thread's
stack. The stack is not zero-filled: the guard is never read and the poison
covers the usable region. `thread_kstack_cache_drain` releases every cached
block; only tests call it, since the cache is a fixed slice of the reserve.

**First-switch-in.** There is no "initial save". Each constructor lays out
`ctx` so `cpu_switch_context`'s `ret` lands at a trampoline: `ctx.lr` is the
trampoline, `ctx.sp` the stack top, and the callee-saved slots carry the
arguments (`x20` = arg, `x21` = entry for the EL1 form; `x19`/`x20`/`x21`/`x22`
= user sp/arg/entry/tls for the EL0 form). `ctx.ttbr0` is baked to the
**kernel** TTBR0 (ASID 0) at create — not the Proc's — because the rolling
ASID is not known until switch time; the context-switch pre-hook installs
the real value. Baking the kernel root means a hypothetical missing-pre-hook
path faults *safely* instead of aliasing ASID 0 onto a user page table.

### The fourth shape restores a frame instead of building one

Every other constructor *composes* a starting context: pick a trampoline, stuff
the callee-saved slots with arguments, let the trampoline assemble a world. The
forked one is the inverse. It receives the parent's live exception frame and
reproduces it, because the child is not starting — it is **continuing the
parent's C frame**, at the same instruction, with the same condition flags.

The decision is a **pure function** of (destination, source, child stack), split
out deliberately so it is reachable by a test with no Proc in existence. It is
the source frame with exactly **two** edits: the first argument register zeroed,
which is the entirety of "this call returns twice", and the stack pointer
replaced, which is mandatory — two Procs sharing an address space must not share
a stack pointer.

**Every omission from that list is load-bearing**, which is why the copy is
written out field by field rather than as an assignment. The return address
copied verbatim *is* "resumes at the same instruction". The saved process state
copied verbatim carries the parent's condition flags, live if a conditional
follows the syscall — and it cannot be forged from EL0 because the hardware
wrote it on exception entry. The remaining general registers are verbatim
because the child continues the parent's frame.

There is a second, blunter reason for the field-by-field form: a whole-struct
assignment of this size compiles to a memory-copy call that a freestanding
kernel does not link.

**The frame is carved at the exact address the exception entry path would have
chosen** — the top of the child's own fresh kernel stack, less one frame. That
address is the whole trick: it lets the trampoline hand the child to the
**shared, audited exception-return** rather than open-coding a second return to
user mode. The return path's loads are stack-relative and its final adjustment
lands precisely on the stack top, exactly as from any other return. Alignment is
*asserted* rather than assumed, because a misaligned stack pointer at the return
faults at kernel level with the frame half-restored.

**The absence of a register-zeroing sweep is the correctness argument.** The
ordinary user-thread trampoline scrubs the general registers so a fresh thread
cannot inherit kernel residue. This one deliberately does not, and the inversion
is exact: the child is resuming its parent's userspace frame, so scrubbing would
break the fork rather than harden it. Two trampolines, opposite dispositions on
the same question, each right for its own case.

**Floating-point state comes from the live registers, not the caller's saved
context.** The caller is *running*, so its saved context holds whatever it had at
its last switch-out — stale by definition. This is the same trap as the note
delivery path's [[sub-kernel-notes]] register save: the structure that looks like
"the caller's state" is only current for a caller that is not currently running,
and the fork path is exactly the case where it isn't.

**The thread-local pointer is programmed verbatim, and that is a layering
decision.** The interface documents zero as "inherit the caller's". Resolving it
here would require this layer to read a live system register on another thread's
behalf; the syscall handler has the caller in scope and owns the meaning. The
constructor stores what it is given.

**The `on_cpu` protocol.** `on_cpu` is true while a thread is actively
running on some CPU — registers live, saved `ctx` stale. It is set when the
thread is picked and cleared by the *destination* CPU's resume frame **after**
`cpu_switch_context` completes. It is Linux's `task->on_cpu` in role and it
is the answer to a question the run states cannot answer: a thread that has
already gone SLEEPING or EXITING may *still be executing*, on its own kstack,
in the middle of the register-save half of its own switch.

`thread_free` therefore gates in four stages: refuse RUNNING/current/
uninitialized; `sched_remove_if_runnable` (walks every CPU's run tree under
their locks); re-check RUNNING as a loud backstop (a peer transitioning `t`
here should now be impossible, so extinct rather than free-under-run); then
**spin on `on_cpu`**. Only then unlink and hand the stack to `kstack_free`,
which caches it or restores its guard and frees it.

## Data structures

`struct Thread` is 1760 bytes, pinned by a `_Static_assert` whose message is
a running changelog of every append and why. `magic` at offset 0, same SLUB
double-free defence as `Proc`. Alignment is `_Alignof(struct Thread)` (16,
from the embedded `Context`'s `_Alignas(16)` vector block) — passing a
hardcoded 8 worked only by the accident that the size happened to be a
multiple of 16, and a future field could have silently misaligned every
slab object after the first.

The largest single append is a **520-byte floating-point save area** for note
delivery, which took the structure from 1232 to 1760 and cost the dedicated
slab cache one object per page — three per page down to two. The alternative,
allocating that area lazily or on the heap, was considered and rejected on a
hard constraint rather than a preference: **note delivery must stay
allocation-free**, so the space has to be pre-committed in the descriptor or it
cannot be relied on at the moment it is needed. The assertion message carries
the whole argument, which is why it reads as a changelog rather than a number.

The tail is dense with single-purpose flags that each fit an existing
padding hole: `cpu_pinned`, `exit_close_active`, `debug_ss_armed`,
`stop_unwinds`, `stop_no_park`, `stop_unwound` (the elected 9P reader holds
the first two for its whole recv, so a stop unwinds it at any byte and the
caught arm latches `note_unwound` for it; no die-check reads them since the
seam-90 close, [[chg-2026-10-06-seam90-close]]), and since 2026-09-29
`note_interruptible`: whether a caught note may unwind the thread's current
wait, set only by the vivarium dispatcher for signal(7)'s list and cleared at
the syscall exit and around a page-in ([[sub-kernel-notes]]); and since
2026-09-30 `note_claim`, the caught-note families this thread has claimed, which
only the thread itself writes and its EL0-return tail releases; and since
2026-10-12 `note_target`, the families the posting walk designated this thread
for, set under its `wait_lock`, turned into its claim when it unwinds, released
at its tail, handed off at a wait that cannot take them and dropped at its exit,
and `caught_wait`, the kind of wait the thread is in (none, unwinds, holds),
published under its `wait_lock` by the sleep cores (ARCH 8.8.3; both bytes come
out of the padding beside `in_handler`); and since
2026-10-05 `cons_frozen_unwound`, the console's record that a caught note
unwound one of the thread's frozen reads, so its next console read re-takes the
reader slot by waiting; the thread sets and consumes it, and exec clears it
([[sub-kernel-cons]]); and since 2026-10-07 two exit-close flags in the same
hole (ARCH 7.9.1): `kthread_join_active`, which `loom_free` sets with save and
restore around its SQPOLL kthread join so no death reaches that sleep, not
even the kill that forces a final close ([[sub-kernel-loom]]), and
`closes_never_wait`, which the SQPOLL kthread sets on itself at entry so its
reap's last dev9p close hands a staged write-behind run to the closer instead
of waiting on the server ([[sub-kernel-ninep-dev9p]]). `exit_close_active` has
one setter again, the at-exit close. The struct size is unchanged at 1760.
`cpu_pinned` is the single
clean unstealability predicate that replaced the old `kstack_base != NULL`
gate — the #860 root cause was that `g_bootcpu_idle` owned a real kstack, so
the old gate did not exclude it.

`rendez_blocked_on` is the reverse pointer that makes the death cascade
possible at all: the *only* record of "Thread T sleeps on Rendez R". Only
the owner writes it, always under the owner's `wait_lock`; the cascade only
reads it, under the same lock. That read-only waker→sleeper edge is what
keeps the #811 lock graph acyclic.

### Two fields added by ARCH 8.12

`Thread.in_syscall` -- set at SVC entry, cleared before the EL0-return tail.
While it is set, `preempt_check_irq` refuses to switch this thread out: the
syscall body runs with interrupts ON but is still NON-PREEMPTIBLE, which is the
property Phase 0 deferred and the implementation accidentally built as
"interrupts off" instead. It is deliberately NOT `preempt_count`: three live
assertions forbid a syscall-wide count, the decisive one being the leaked-count
check at the EL0 return, where a syscall-wide count is definitionally the leak
it extincts on. A kernel thread carries no marker and stays preemptible, which
#810 depends on.

The kernel-stack **watermark** -- the usable region is filled with a
distinctive constant at every `thread_create`, a cached block's as well as a
fresh one's, and scanned upward from the guard
for the first word that is not it. That word is the deepest the stack has EVER
reached. Reading a RUNNING thread's watermark is deliberate and its answer is
honest in the direction that matters: a running thread only pushes its frontier
lower, so a concurrent write makes the reported depth deeper, never shallower.
The caller pins the Thread under its Proc's thread lock; the scan needs no lock.
`/ctl/kstack` reports it per Proc and as a whole-system peak.

It exists because nothing in the tree could report stack depth at all, and ARCH
8.12 puts an IRQ frame on a syscall stack that has never carried one. The bound
that sized that chunk is static and has 797 unfollowed indirect edges under it,
so it is a LOWER bound and wanted a witness.

`thread_kstack_used(t, budget_words)` takes a **scan budget** (NULL = unlimited)
because the scan's cost is INVERTED: it stops at the first touched word, so a
SHALLOW thread costs MORE than a deep one, and the only in-tree caller walks
every live Proc with IRQs masked. That made an unprivileged program able to
inflate a masked window simply by spawning threads -- it needs no privilege to
do so, and only the READER is gated. The budget is decremented per word; a
caller that finds it 0 on return MUST report the answer as a LOWER BOUND,
because a truncated scan returns a SHALLOWER number than the truth and a
silently-truncated watermark is worse than none. `/ctl/kstack` spends
131072 words (1 MiB of loads) per read and says so in its output when it runs
out. Found by the ARCH 8.12 audit round (F2).

## Concurrency

`thread_link_into_proc` / `thread_unlink_from_proc` take the Proc's
[[lock-proc-threads]] (they took [[lock-proc-table]] until thread-model C3).
The link counts the thread in `thread_count` and `live_threads`; the unlink
drops `live_threads` too when the thread never committed. The user spawn arms
build a thread unlinked (`thread_alloc_user` / `thread_alloc_forked`) and link
it through `proc_thread_admit`, or free it with `thread_free_unlinked`.
`thread_commit_exiting_locked` is the only writer of `THREAD_EXITING`, always
on the calling thread; a kernel thread's terminal uses `thread_kthread_retire`,
under the same lock. `thread_reap_dead` unlinks under the same lock, so a
walker holding it never meets a freed thread; it takes the lock after the
run-queue lock is released, the only lock its CPU held, and it is the one
threads_lock that CPU holds. Before multi-thread Procs the lock was unnecessary
(one writer per Proc, ever); the P6 lift introduced a second writer
(`SYS_THREAD_SPAWN` from a peer Thread on a peer CPU) and without the lock a
walker could observe `p->threads = new_head` published before
`new_head->next_in_proc = old_head` was visible — terminating the walk at
the new head and missing every pre-existing thread. The lock makes
`(head, next_in_proc, thread_count)` one linearizable observation.

`g_next_tid` is claimed by a saturating compare-and-swap (`alloc_next_tid`).
A tid is never reused, so the counter is cumulative rather than a count of
live threads, and an unprivileged create-and-join loop walks it toward
INT_MAX; at the end of the space it answers -1 and stays there, never
wrapping into the -1 sentinel or onto a tid a live thread may still hold. The
three constructors claim the tid first, before anything is allocated, and
return NULL on a spent space, which both spawn arms report as EAGAIN
(`thread_tid_space_spent`), a system limit, rather than ENOMEM ([[inv-i32]]);
only the boot idles, which cannot fail, still extinct. The pre-P6 non-atomic
`++` could hand two peer spawns the same tid and break `pthread_self`
uniqueness. `g_next_pid` saturates the same way ([[sub-kernel-proc]]).

`thread_switch` masks IRQs across its entire mutate-switch-resume window.
The `set_current_thread(next)`-before-`cpu_switch_context` sequence is a
torn state — `current_thread() == next` while the CPU still runs `prev`'s
registers — and a timer tick landing there drives `sched()`, which reads
`current == next` and saves `prev`'s live state into `next->ctx`. The mask
is balanced per-thread: the saved state rides `prev`'s kstack and is
restored when `prev` is switched back to.

## Invariants enforced

- The `on_cpu` gate is the impl of "a ctx/kstack is never written by two
  CPUs concurrently" ([[inv-i21]]'s thread half; the scheduler dossier owns the
  migration protocol proper).
- [[inv-i44]]'s thread half: a forked child resumes the parent's frame on its
  **own** stack, and the constructor refuses to be the place that decides
  otherwise — the stack pointer is a required argument, not a default.
- [[inv-i31]] composes here rather than being enforced: a shared-memory child
  gets a fresh `Thread` over the *parent's* address space, so the pre-switch hook
  resolves the same address-space identifier. One address space is one ASID
  however many threads reach it.
- `cpu_pinned` keeps every boot/idle-stack thread on its own CPU.
- The kstack guard region enforces stack-overflow detection per thread.
- [[inv-i48]]'s free half: `thread_reap_dead` is the one free of an exited user
  thread, run after its last switch-out completes. Under `KERNEL_TESTS`,
  `thread_commit_exiting_locked` extincts if a Proc's `thread_count` exceeds its
  `live_threads` by more than the online CPUs.

## Error paths

Every constructor returns NULL on OOM with internal cleanup (descriptor
freed; a fresh block whose guard could not be set goes back to the buddy),
and on a spent tid space. Everything else extincts: `thread_free`
of a corrupted/already-freed/RUNNING/current/uninitialized Thread, or of an
exited user thread (a second reaper); `thread_reap_dead` of a corrupted,
not-exited or kernel thread;
`thread_switch` into a corrupted, uninitialized, EXITING, or `on_cpu`
thread; `thread_unlink` on a list-head mismatch; a stack whose guard cannot
be restored on its way back to the buddy (`kstack_release`); a spent tid
space at a boot idle's create.

## Performance

Thread creation is one SLUB alloc + a stack from the cache (a leaf lock, a
pop, the 16 KiB poison) or, on a miss, one order-3 buddy alloc + four leaf
PTE writes + four by-VA invalidates + the poison. A free is a push, or past
the bound the miss in reverse. Measured on the Mac under HVF (`--config ci`,
the suite's `thread.kstack_create_free_cost`), a create+free cycle costs
924-1018 ns from the cache and 1417-1570 ns from the buddy, against 2802 ns
before C2. `thread_free`'s `on_cpu` spin is bounded by a single in-flight
switch (the peer always resumes and clears it) and cannot deadlock because
`thread_free` is always called lock-free;
`g_thread_free_oncpu_waits` counts how often it actually had to wait, which
is the running proof that the window is real.

A user thread's free runs on the resume path of its last switch-out, with IRQs
masked: one unbounded scan of the dead stack, one thread-lock hold, a cache
push (or a guard restore and buddy free past the bound) and a slab free. It
adds about 200 B of frames to that path (`thread_reap_dead` 48 B,
`thread_destroy` 48 B with the stack free inlined, then
`mmu_restore_normal_range` 48 B or `kmem_cache_free` 64 B and `free_pages`
32 B, from `-fstack-usage` at C1b), which can be the resume of a thread asleep
at the bottom of the deepest syscall chain (ARCH 8.12).

## Prosecution

- **#788 is the shape to keep in mind.** `thread_free` freeing a
  SLEEPING-but-still-`on_cpu` thread returns its slot and kstack to the
  allocators while a peer CPU is still writing them; the stack cache's LIFO
  (buddy LIFO before it) hands the same memory to the very next
  `thread_create`, and the stale register-save
  corrupts the *recycled* thread's `ctx.sp` → a wild SP fault in its own
  guard, reported as "kernel stack overflow". SMP-only, ~10-20% on the exact
  clean binary. Any change to the free path must preserve the gate order.
- The `thread_switch` safe-use contract is **narrower than its gates**: the
  target must be off-CPU *and* off-tree. Off-CPU is gated; off-tree is a
  documented contract only, deliberately left ungated to avoid a false trip
  on a link field a future scheduler refactor might repurpose.
- `ctx.ttbr0` baked to the kernel root is a fail-safe, not an optimization —
  do not "fix" it to the Proc's root.
- The guard restore before `free_pages` is not optional: a page left
  no-access would silently fault its next user, whatever that user is. It
  is the ONLY restore: a cached block whose guard is open loses overflow
  detection for every thread that runs on it next. A restore that fails
  extincts; a guard that could not be restored never reaches the buddy.
- **A reused block is never zeroed.** The poison in `kstack_alloc` is the only
  scrub between two owners, so it must cover the whole usable region on both
  paths. Re-poisoning only down to the last owner's watermark would be sound
  only from an exact scan; a budgeted scan is a lower bound and would leave the
  deepest frames in place.
- `_Alignof` in the cache creation must stay; a hardcoded alignment is a
  latent slab-misalignment bug.
- **The fork frame copy enumerates fields, so it is complete only for the
  struct as it stands.** A member added to the exception frame is silently not
  copied — the child would inherit a zero where the parent had a value. What
  actually prevents that is a size assertion living with the *assembly* frame
  layout, in another file, whose purpose is the entry/exit ABI: an added field
  trips it and forces the author to update the assembly. Nothing routes them
  here. The completeness is real and the guard is incidental.
- **The two user trampolines must keep disagreeing about register scrubbing.**
  Fresh-thread zeroes, forked does not. Making them consistent breaks whichever
  one is changed.
- **Floating-point for a fork must come from the live registers.** The caller's
  saved context is stale precisely because the caller is running.
- **The forked frame must stay at the top-of-stack address the entry path would
  have used.** Moving it silently breaks the shared exception return, whose
  offsets are stack-relative — the reuse of that audited path is the reason the
  address is what it is.

## Seams

- No dedicated per-CPU exception/IRQ stack: a kernel IRQ builds its frame on
  the interrupted thread's own kstack. Unrelated to #788 and recorded as
  future hardening in `thread.h`.
- `thread_switch`'s off-tree half of the contract is unenforced (above).

## Caveats

- **`kstack_base` is the guard base, not the stack base.** Arithmetic that
  assumes it points at usable memory is off by 16 KiB.
- **kthread and the per-CPU idles own no kstack** (`kstack_base == NULL`) —
  they run on boot stacks. Every free path is gated on that pointer.
- `thread_switch` is test-only. The comment in `thread.c` that once claimed
  it needed no IRQ mask was the *false premise that caused #101*; the
  regression hook `g_thread_switch_test_window_ns` exists solely to force a
  preempt into the torn window and is 0 in production.
- The absorbed `14-process-model.md` documented the thread state machine as
  `thread_block`/`thread_wake` — functions that do not exist; the primitives
  are `sleep`/`wakeup`.
- **A tid-addressed post finds an exited worker only until its free.**
  `proc_holds_tid` (behind tkill and tgkill) walks the list under the lock, so it sees a
  worker that has committed EXITING and not yet switched out (at most one per
  CPU) and answers ESRCH once the resume path has freed it.

## Provenance

Originally written 2026-08-01; the section was left empty at the time, so the
first read's exact tip is not recoverable and is not guessed at here.

Re-read 2026-08-16 at `efd83109` — `kernel/thread.c` (820 lines) and
`kernel/include/thylacine/thread.h` (773). The only change since was the
fork-restore constructor and its pure decision function. Cross-checked this
pass: the exception-frame struct against the copy's field list (complete), the
size and offset assertions that guard it, both user trampolines' opposite
register-scrub dispositions, and the floating-point save's source.

[[chg-2026-08-16-thread-fork-restore]].

Thread-model C1 (2026-10-22): an exited user thread is freed by the resume path
of its last switch-out (`thread_reap_dead`), replacing the free at the next
create ([[seam-exited-threads-wait-for-a-create]], closed).
