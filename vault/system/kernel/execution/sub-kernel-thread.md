---
id: sub-kernel-thread
type: sub
title: "The Thread: context, kstack, and the on_cpu protocol"
parent: moc-kernel-execution
code: ["kernel/thread.c", "kernel/include/thylacine/thread.h"]
audit: hard
guarded-by: [inv-i21, inv-i31, inv-i44]
validated-by: [gate-smp]
locks: [lock-proc-table]
created: 2026-08-01
updated: 2026-08-16
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

`thread_free(t)` releases the descriptor + kstack; the caller must ensure
`t` is not current and not in any run tree. `thread_switch(next)` is the
**test-only** direct-switch primitive — production multitasking is
`sched()`.

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
one 2 MiB block — one demote chain, one TLB flush.

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
**spin on `on_cpu`**. Only then unlink, restore the guard pages, free.

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
`stop_unwinds`, `stop_no_park`, `stop_unwound`. `cpu_pinned` is the single
clean unstealability predicate that replaced the old `kstack_base != NULL`
gate — the #860 root cause was that `g_bootcpu_idle` owned a real kstack, so
the old gate did not exclude it.

`rendez_blocked_on` is the reverse pointer that makes the death cascade
possible at all: the *only* record of "Thread T sleeps on Rendez R". Only
the owner writes it, always under the owner's `wait_lock`; the cascade only
reads it, under the same lock. That read-only waker→sleeper edge is what
keeps the #811 lock graph acyclic.

## Concurrency

`thread_link_into_proc` / `thread_unlink_from_proc` take
[[lock-proc-table]]. Before multi-thread Procs the lock was unnecessary
(one writer per Proc, ever); the P6 lift introduced a second writer
(`SYS_THREAD_SPAWN` from a peer Thread on a peer CPU) and without the lock a
walker could observe `p->threads = new_head` published before
`new_head->next_in_proc = old_head` was visible — terminating the walk at
the new head and missing every pre-existing thread. The lock makes
`(head, next_in_proc, thread_count)` one linearizable observation.

`g_next_tid` is an atomic fetch-add with an INT_MAX guard (the same
discipline as `g_next_pid`); the pre-P6 non-atomic `++` could hand two peer
spawns the same tid and break `pthread_self` uniqueness.

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

## Error paths

Every constructor returns NULL on OOM with internal cleanup (descriptor
freed, pages freed, guard restored). Everything else extincts: `thread_free`
of a corrupted/already-freed/RUNNING/current/uninitialized Thread;
`thread_switch` into a corrupted, uninitialized, EXITING, or `on_cpu`
thread; `thread_unlink` on a list-head mismatch; tid overflow.

## Performance

Thread creation is one SLUB alloc + one order-3 buddy alloc + one
guard-demote chain + a TLB flush. `thread_free`'s `on_cpu` spin is bounded
by a single in-flight switch (the peer always resumes and clears it) and
cannot deadlock because `thread_free` is always called lock-free;
`g_thread_free_oncpu_waits` counts how often it actually had to wait, which
is the running proof that the window is real.

## Prosecution

- **#788 is the shape to keep in mind.** `thread_free` freeing a
  SLEEPING-but-still-`on_cpu` thread returns its slot and kstack to the
  allocators while a peer CPU is still writing them; buddy LIFO hands the
  same memory to the very next `thread_create`, and the stale register-save
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
  no-access would silently fault its next user, whatever that user is.
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
