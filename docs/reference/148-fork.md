# 148 — `SYS_RFORK` + child-context restoration (LINEAGE L-3b)

**Status**: as-built at L-3b. Scripture: `docs/LINEAGE.md` §5.4; invariant
`ARCHITECTURE.md` §28 **I-44**; the authoritative prosecution list is the
`ARCHITECTURE.md` §25.4 LINEAGE row's L-3b addendum.

**Files**: `kernel/thread.c` (`fork_frame_init`, `thread_create_forked`),
`arch/arm64/vectors.S` (`thread_fork_trampoline`), `kernel/proc.c`
(`rfork_forked`, `rfork_internal`'s `fork_context` arm), `kernel/syscall.c`
(`sys_rfork_handler`), `usr/lib/libthyla-rs/src/lib.rs` (`rfork_spawn`),
`usr/fork-probe/`.

---

## Purpose

`rfork(RFPROC|RFMEM)` from EL0: a second Proc that shares the caller's address
space and **resumes the caller's own frame**, distinguished only by `x0`.

This is the tree's first syscall that returns twice, and the first place two
EL0 Procs run in one address space. It is what `posix_spawn`'s `CLONE_VM` child
needs, and — once L-4 supplies copy-on-write — what `fork()` will be built from.

---

## The child never gets an entry point, and that is the whole design

The obvious shape would be "start the child at a function". Measuring musl's
`src/thread/aarch64/clone.s` says otherwise, and this is the finding that moved
child-context restoration from L-5 to L-3b:

```asm
    svc  #0
    cbz  x0, 1f          // x0 == 0 -> child
    ret                  // parent: x0 = pid
1:  ldp  x1, x0, [sp], #16
    blr  x1
```

The raw `clone` syscall **returns twice**. The child resumes at the same PC with
`x0 = 0` and `SP` replaced, and only *then*, in userspace, pops a function
pointer off its new stack and calls it. The kernel never sees a continuation.

So the kernel's job is to **restore a frame**, not construct one — and the
`CLONE_VM` shape needs exactly the same trapframe copy a real `fork` does. Only
shared-vs-COW separates L-3 from L-4.

---

## `fork_frame_init` — the decision, in one pure function

```c
void fork_frame_init(struct exception_context *dst,
                     const struct exception_context *src,
                     u64 child_sp);
```

`dst` is `src` with **exactly two edits**:

| edit | why |
|---|---|
| `regs[0] = 0` | the child's return value. This single difference *is* "fork returns twice" — both Procs resume at the same `elr` on identical registers, and `x0` is all that tells them apart. |
| `sp = child_sp` | **mandatory**, not defaulted. Two Procs sharing an address space must not share a stack pointer; they would corrupt each other's frames on the first push. |

Everything else is copied verbatim, and each omission is load-bearing:

- **`elr`** verbatim is what "resumes at the same instruction" *means*.
- **`spsr`** verbatim carries the parent's NZCV, which is live if a conditional
  follows the syscall. It is safe to copy because EL0 cannot forge it — the
  hardware writes SPSR_EL1 from the current PSTATE on exception entry.
- **`x1..x30`** verbatim because the child *continues the parent's C frame*: its
  callee-saved registers and its link register must be the parent's.
- `esr`/`far` are copied and are meaningless for the child (they describe the
  parent's trap). Nothing on the return path reads them.

**Field-by-field, not `*dst = *src`.** A 288-byte struct assignment compiles to
a `memcpy` the freestanding kernel does not link — `context.h` records the same
hazard for the FP block. Writing the copy out also puts the "everything else
verbatim" claim where a `=` would hide it.

It is a separate pure function because that is what a kernel test can reach:
`fork.frame_init` needs no Proc, no address space and no EL0 thread.

---

## `thread_create_forked` — the third creation shape

| shape | entry | used by |
|---|---|---|
| `thread_create_with_arg` | `blr` a kernel function | kernel threads |
| `thread_create_user` | eret to `(entry, sp, arg)` | `SYS_THREAD_SPAWN` |
| **`thread_create_forked`** | **eret onto a restored frame** | `SYS_RFORK` |

The child's frame is carved off the **top of the child's own fresh kernel
stack** — the exact address `KERNEL_ENTRY` would have chosen had the child taken
a real exception — and `ctx.sp` points at it. That is what lets the trampoline
hand the child to the shared exception-return path instead of open-coding a
second eret.

Two details that are asserted rather than assumed:

- **16-byte alignment.** `sizeof(struct exception_context)` is
  `EXCEPTION_CTX_SIZE` (288, a multiple of 16) and the kstack size is
  page-aligned, so the frame lands aligned without rounding. It is checked
  anyway: a misaligned SP at the eret faults with the frame half-restored.
- **FP/SIMD comes from the LIVE registers** (`fp_save_area`), not from the
  caller's saved `Context`, which holds its last switch-*out* values and is
  stale while the caller is running. POSIX fork copies FP state; none of L-3's
  own consumers read it across the call, but leaving it zeroed would be a
  divergence with nothing behind it. `struct Context`'s FP block (`fp_v` @128 /
  `fpsr` @640 / `fpcr` @644) is byte-identical to the 520-byte area
  `fp_save_area` writes, and its `_Alignas(16)` satisfies the alignment
  requirement — so this is one existing, audited call.

`child_tls` is programmed verbatim. The ABI's "0 means inherit" is resolved by
the syscall handler, which has the caller in scope; the live TLS base is in a
system register, not in any saved structure.

---

## `thread_fork_trampoline` — and why it lives in `vectors.S`

```asm
thread_fork_trampoline:
    bti     c
    bl      sched_finish_task_switch   // release prev's run-tree lock
    bl      el0_return_die_check       // #811 first-entry (I-24)
    msr     daifset, #0xf              // #713: no interruptible ELR window
    b       .Lexception_return         // the SHARED KERNEL_EXIT
```

`.Lexception_return` is deliberately a **local** label. Its own comment explains
why: exporting it "would let any kernel caller invoke KERNEL_EXIT on whatever
happens to sit on the current SP, ERETing to attacker-controllable ELR_EL1 +
SPSR_EL1" — and it explicitly sanctions branching to it *from within
`vectors.S`*, "the same way the IRQ slot does". So the trampoline lives beside
it rather than in `context.S` with its two siblings. On a surface whose own
macro calls itself "the single most load-bearing exit in the kernel", reusing
the audited exit is worth more than keeping the trampolines in one file.

**There is no GPR-zeroing sweep, and that is deliberate.**
`thread_user_trampoline` zeroes every register but `x0` so no kernel state
crosses EL1→EL0. This child is *continuing its parent's userspace frame* — every
register it loads is a value userspace already had. Zeroing would break the
fork, not harden it.

DAIF is masked explicitly even though the trampoline is reached masked
(`sched()` switches under irqsave; `cpu_switch_context` preserves DAIF). It
costs one instruction and makes the #713 property local rather than inherited.

---

## `SYS_RFORK = 102`

```
SYS_RFORK(flags, child_sp, child_tls)
  -> child pid to the PARENT, 0 to the CHILD, -errno on failure
```

Takes `ctx` for the mirror image of `SYS_EXECVE`'s reason: execve **rewrites**
this frame so its own eret starts a new image; rfork **copies** it so a second
Thread can eret onto it. Either way the frame is the subject of the call, not a
means of returning from it.

| check | why it is refused |
|---|---|
| `flags != RFPROC\|RFMEM` | `RFPROC` alone would give the child a fresh **empty** address space and then resume it at its parent's PC — an instruction-fetch fault on the first cycle. A private child address space means copy-on-write (L-4). |
| `child_sp == 0` | the shape a caller lands on if it expects POSIX fork to default the stack. |
| `child_sp & 15` | AAPCS64 requires 16-byte SP alignment; an unaligned SP_EL0 faults. |
| `child_sp >= UACCESS_USER_VA_TOP` | not a user address. |
| `child_sp == ctx->sp` | **a footgun-catcher, not a safety property.** An SP that *overlaps* the parent's stack without equalling it is just as fatal and is not detectable here — the parent's stack has no recorded extent. Non-overlap is the caller's contract, exactly as for a pthread stack. This check just refuses the mistake that is free to see. |

Validation lives in the handler, ahead of `rfork_forked` — which is what makes
every rejection reachable from a kernel test running as kproc.

`child_tls == 0` means **inherit** the caller's TPIDR_EL0: fork semantics, and
what a vfork child needs (it runs the parent's C, thread-locals and all, until
it execs). Note this differs from `thread_create_user`, where 0 means "no TLS" —
there a fresh thread genuinely has none, here the caller always has one.

**Testing this required care, because the obvious test is vacuous.** `execve`
zeroes TPIDR_EL0 (`kernel/proc.c`) and libthyla-rs has no thread-locals, so a
native probe's parent sits at 0 — and `child == parent` would then pass whether
the kernel inherited the value or simply left the child's at zero. So the probe
*installs a sentinel in its own* TPIDR_EL0 first (architecturally RW at EL0 —
it is the TLS register), making "inherited" and "left zero" distinguishable.
Two legs, against **distinct** sentinels so neither can be satisfied by the
other's value:

- **G1** `tls = 0` → the child's TPIDR_EL0 equals the parent's sentinel.
  Revert-probed: removing only the inherit resolution in the handler fails at
  exactly this assertion.
- **G2** `tls = <explicit>` → programmed verbatim. The other arm of the same
  branch; G1 alone would be satisfied by a kernel that copied the whole frame
  blindly, G2 alone by one that ignored inheritance.

### What the child inherits, and what it does not

Identity, capabilities (minus `CAP_ELEVATION_ONLY`), phenotype, hardware
allowance, environment, session + process group, and a clone of the Territory —
everything an rfork child has always inherited, because both child shapes run
**one body**. `rfork_internal` gained a `struct fork_context *fc` parameter and
branches only at the final `thread_create_*` call; keeping one body is what
guarantees the two shapes inherit identically rather than via two lists that
drift.

At L-3b the **handle table is fresh and empty**; the copy is L-3c-1, and #119 is
its hazard (a copy would duplicate hardware handles, which I-5 makes
non-transferable). Descriptor inheritance landed at L-3c-1 — see below.

> This paragraph used to call the empty table "`CLONE_VM` *without*
> `CLONE_FILES` — precisely posix_spawn's shape". **That was wrong.** Linux's
> `CLONE_VM` without `CLONE_FILES` gives a **copied** fd table, which is exactly
> why posix_spawn's `dup2`/`close` file_actions do not disturb the parent. Empty
> and copied are different shapes; the wording made a deferral read as a
> deliberate ABI match. Corrected at L-3c-1, where the same sentence was found in
> **five** places — and found by grepping the *claim's distinctive phrase*, not by
> revisiting the topic file by file, which had already missed two of them.

---

## Userspace: the child comes back on a different stack

`SYS_RFORK` cannot be wrapped by an ordinary `asm!`. After the eret the child
holds every one of the parent's registers except `x0` — including `x29` (frame
pointer) and `x30` (link register), both pointing into the **parent's** stack —
while `SP` points at fresh memory. Frame pointer and stack pointer describe two
different stacks; any compiler-generated local access, any epilogue, any `ret`
would touch the wrong one. A wrapper that returned `0` into safe code would hand
the child a frame that does not exist.

`libthyla_rs::rfork_spawn` is a transliteration of musl's `clone.s`:

```rust
pub unsafe fn rfork_spawn(stack_top: u64, tls: u64,
                          entry: extern "C" fn(u64) -> !, arg: u64) -> i64;
```

It pushes `(entry, arg)` onto the **child's** stack before the syscall; the
child pops them and `blr`s, establishing a correct frame on its own stack before
any compiled code runs. It zeroes `x29`/`x30` first so a backtrace cannot walk
into the parent's stack, and never returns in the child.

Safety contract: `stack_top` must be exclusively the child's, 16-aligned, and
non-overlapping with the caller's; `entry` must not return (the shim exits the
child as a backstop, not as a contract).

---

## Tests, and what each layer is blind to

| claim | kernel test | `/fork-probe` |
|---|---|---|
| the frame's contents | **`fork.frame_init`** | indirectly |
| malformed requests refused | **`fork.rfork_arg_rejection`** | leg F, from a Proc that *has* a space |
| the child erets and resumes | — | **yes** |
| the two Procs share the address space | — (kproc has none) | **yes** |
| the child runs on its own stack | — | **yes** |
| `child_tls` inherit + explicit arms | — | **yes** (legs G1/G2) |

`fork.rfork_arg_rejection` carries a **well-formed** leg that must fail *later*
(`-EAGAIN`, because kproc has no address space to share). Without it, every
assertion in that test would also pass if the handler simply rejected
everything.

`/fork-probe` is boot-fatal and proves six legs at once; its own header
enumerates them. Legs A/B/C cannot be satisfied by one accident: a kernel that
entered the child at a fixed entry point fails A; one that gave it a private
address space fails B; one that ignored `child_sp` fails C.

**Revert-probed.** Making `thread_create_forked` ignore `child_sp` entirely
leaves the **unit suite fully green** (1284/1284) while only the in-guest leg
fails — `fork-probe: FAIL the child did not exit normally`. No kernel test can
see that bug.

A trap worth carrying: an earlier probe attempt chained `cp … && python …`, the
`cp` failed, and the edit never applied — so the run came back green and *looked
like the probe had passed*. **A revert probe that fails to sabotage reports the
same green as a correct system.** Verify the sabotage is present before
building; never infer it from the script having run.

---

## Descriptor inheritance (L-3c-1)

A forked child's handle table is a **copy** of its parent's, at the same slot
indices — `handle_table_copy_into`, called from `rfork_internal` on the fork
shape only.

That gate is the design decision. The two primitives reaching `rfork_internal`
make opposite promises: `SYS_SPAWN_*` takes an explicit `fd_list` and endows
exactly that (a capability hand-over — the parent states what it means to give),
while `SYS_RFORK` takes no list because there is nothing to state, the child
being the parent continuing on the same frame. Those are not two settings of one
knob. Keying on the fork shape says so directly, and leaves the spawn family
byte-unchanged by construction rather than by a flag a future caller could set
wrong.

**Indices are preserved, and a skipped slot leaves a hole.** This is why the
copy is not a loop over `handle_dup`: dup installs into the first free slot, so
one skip would renumber every descriptor after it.

### What does not cross, and why the fork still succeeds

Admissibility is `handle_slot_may_alias` — the *same* predicate `handle_dup`
uses, extracted rather than rewritten, because both operations create a second
handle naming one object. Two clauses, both load-bearing:

| clause | excludes | why |
|---|---|---|
| kind | hardware (I-5), `KObj_Srv`, `KObj_Loom` | a hw handle is pinned to its creating Proc; a Loom ring is pinned to the table its registered handles index |
| object | a devsrv Spoor (`dc == 's'`) | the only case where the *kind* is admissible and the *object* is not — a copy written against kinds alone would inherit a `/srv` connection and blur its kernel-stamped SO_PEERCRED origin |

The fork **proceeds** and the child gets a hole. I-5 is a property of the
handle — "pinned to the Proc that created it" — not a property of forking, so
the child simply does not hold what it was never eligible to hold. Refusing the
whole fork would punish a parent for holding a handle it never intended to pass,
and would leave a driver unable to create a process at all. A child needing
hardware authority gets it the way every other Proc does: the warden's confer
path (the I-34 allowance).

The hole is observable — `EBADF` at that index, and the child's next `open`
lands where Linux's would not — which is the honest report of an authority it
could not inherit.

Rights cross **verbatim**. I-6 is satisfied by non-increasing, and narrowing
would make an inherited fd less capable than the one it was forked from.

### Both coverage layers are blind to the other's bugs

| sabotage | unit suite | `/fork-probe` |
|---|---|---|
| admit every kind | **1283/1285 FAIL**, at *two* assertions in *two* tests (the kind clause and the object clause — so a fix to one cannot mask the other) | unreachable (a failing suite aborts the boot) |
| remove `rfork_internal`'s call | **1285/1285 PASS** — fully green through the bug | **FAIL**, at its own assertion |
| compact instead of preserving | **1284/1285 FAIL**, at the *hole* assertion | would pass |

The middle row is the one to carry. The kernel test can call
`handle_table_copy_into` directly, but nothing kernel-side can reach
`rfork_internal`'s *call* to it — that sits behind an `RFMEM` gate kproc can
never pass. The rule and the wiring need separate proofs.

---

## The VFORK suspend (L-3c-2)

`SYS_RFORK` with `RFMEM` **does not return to the parent** until the child has
left the parent's address space. The parent parks in `vfork_await_release`, at
the tail of `rfork_internal`, after the child is runnable and every lock is
released.

The reason is a lifetime one and it is not theoretical: musl's `posix_spawn`
hands the child `char stack[1024+PATH_MAX]` — a local in the frame it is about
to return from (LINEAGE §2.3). A parent that resumed would be running on the
same stack the child is executing on.

### There is no VFORK flag, and that is deliberate

Every `RF*` bit answers *what does the child get?*, under a polarity the header
states plainly: `set == share`. Suspending the **parent** answers a different
question. A bit among those would be a category error, and would cost the word
the property that makes `RFPROC|RFMEM` readable as "posix_spawn's child" at a
glance.

So the suspend follows from `RFMEM` instead — which is not an arbitrary
coupling, because `RFMEM` is *exactly* the precondition of the hazard. Sharing
the address space is the only way the child can reach the parent's frame at all;
the condition and the danger are the same condition.

The default falls out of the fail-safe direction, which is one-sided:

| wanted | got | consequence |
|---|---|---|
| concurrency | suspend | the parent blocks until the child finishes — visible, terminating, diagnosable |
| suspend | concurrency | the child runs on a dead frame — corruption, three layers from its cause |

Concurrent shared-memory execution is anyway already served, and better, by
`SYS_THREAD_SPAWN`. Two *Procs* in one space running at once is a thing someone
can ask for later, explicitly, with its own reasoning — exactly as every reserved
`RF*` bit will.

The gate is `fc && (flags & RFMEM)`, not plain `fc`. That is what keeps L-5
correct without a future edit: stock `fork()` is `RFPROC` alone, and there both
Procs must run.

### The release condition is the release, not a record of it

```c
bool vfork_child_released(const struct Proc *parent, const struct Proc *child) {
    if (!child) return true;
    return (child->state != PROC_STATE_ALIVE) || (child->as != parent->as);
}
```

The obvious design is a flag — set at fork, cleared at exec and exit. It is
strictly worse, because it records the release somewhere other than where the
release happens; a third release path added later would silently strand every
vfork parent.

"The child is off my frame" means "the child no longer maps my address space",
and that fact is already written down. Three ways out, and this is all of them:

| exit | what changes | why nothing else can |
|---|---|---|
| exec | `proc_exec_replace` swaps in a fresh AddrSpace | — |
| death | no longer `ALIVE` | — |
| gone | not in the children list | counts as released: the alternatives are "resume" and "hang forever", and a parent that resumes slightly early corrupts a frame already abandoned, while one that hangs looks unkillable |

The only other route to a private space is a fork the child cannot perform
(`RFPROC` alone is refused) and an exec the parent cannot perform (it is parked).

**The pointer comparison is sound only because the parent still holds a
reference** to the shared AddrSpace. The outgoing object cannot be freed while
the parent is parked, so its address cannot be recycled underneath the
comparison — a direct dividend of L-3a having moved the VMA drain into
`addrspace_unref`'s last drop. Before that, this would have been an ABA waiting
to happen.

### One new wake

Parking on `child_waiters` — `wait_pid_for`'s `#344` multi-waiter list, with its
register-then-observe discipline verbatim — makes the **death** release free:
`proc_become_zombie_locked` already wakes it. Only the **exec** release needed a
new wake, one line inside the same `g_proc_table_lock` section as the `p->as`
swap it reports. It wakes unconditionally rather than testing whether anyone is
suspended: a spurious wake costs a re-scan, whereas a test would be a second
place that has to agree with the park about who is waiting.

A parent killed while parked returns `SLEEP_INTR` and unwinds to its EL0-return
die-check (#811), leaving nothing behind — it registered no state anywhere but
its own stack. A child that loops forever parks its parent forever; that is the
vfork contract, and the parent stays killable.

### Three layers, each blind to the others

| sabotage | unit suite | `/fork-probe` |
|---|---|---|
| remove `rfork_internal`'s park | **1286/1286 PASS** — green through the bug | **FAIL** at leg I, plus an orphan-adoption line showing the child had not run |
| remove `proc_exec_replace`'s wake | **1286/1286 PASS** — green again | **HANG**: the boot stops after `exec-probe` and never reaches fork-probe |

**A missing exec release is a hang, not an error** — and that is not an artifact
of how the probe is built. It is the bug's production symptom: a `posix_spawn`
parent whose child execs would park forever in exactly the same way. A boot that
stops between `exec-probe` and `fork-probe` should have this wake checked first.

`fork.vfork_release` carries the determinism, driving all four cases directly.
Testing all four rather than the interesting one matters because the two failing
directions are opposites and a wrong predicate usually gets only one: never
releasing hangs every `posix_spawn` parent, always releasing lets the parent run
on a frame still in use — and *that* one would leave every other fork-probe leg
passing, since they all reap before they observe.

Leg I (death arm) is the wiring proof and is **not** race-free in its failing
direction: on another CPU a no-suspend child could in principle reach `t_exits`
first. The window is a handful of instructions against a whole
context-switch-in, so the failing kernel loses it overwhelmingly — measured by
the revert probe above, not assumed.

### `child` must not be dereferenced past `ready()`

`rfork_internal` captures `child->pid` into a local before `ready(ct)` and
returns the local. It used to end `return child->pid`, with no lock held, and
that is a use-after-free: a peer thread of a multi-threaded parent sitting in
`wait_pid_for(-1)` can reap the child, and `wait_pid_for` unlinks under
`g_proc_table_lock`, *drops* the lock, then `proc_free`s.

The window predates L-3c-2 — `ready()` → `return` has been a handful of
instructions since L-3b. What the park did to it was worse than widening: it
stretched the window to the child's entire lifetime **and aligned it**, because
the park's release edge is the child's death, which is the very edge that makes
the child reapable. The peer's wake and the parent's are the same
`child_waiters` wake.

Found by the self-audit, not by a test — no test in the tree can produce that
interleaving, and the SMP gate ran clean both before and after. The general
form is worth carrying: **a new park inherits every unsynchronised access that
follows it.** When a sleep is added, the question is not whether the new code is
correct but what putting a sleep there did to the code after it.

### Leg J

Leg J (exec arm) is the only leg covering the new wake, and the only one leg I
cannot see: a kernel releasing solely on death passes everything else here and
then hangs the first real `posix_spawn`. Its discriminator is that the child is
**still alive** when the parent looks — we are executing, so something released
us; `WNOHANG` says the child has not died; the only other release is the exec.
That is a fact rather than a race because the exec'd successor blocks reading a
pipe only the parent can write.

---

## Stock `fork()` (L-5)

`RFPROC` alone is served. The two changes are small; what they cost was not.

**`rfork_internal` has three answers to "what address space does the child
get", and each is what its shape MEANS.** `entry` alone → a fresh EMPTY space
(the child runs a kernel thunk and execs; copying would be thrown away).
`RFMEM` → the parent's, shared. `fc` without `RFMEM` → an `addrspace_clone`.
The discriminator is `fc` — the same one descriptor inheritance uses, and for
the same reason rather than by coincidence: `fc` means the child *is* the
parent, continuing on its frame, so it must see the parent's memory **and** its
descriptors or its next instruction reads something that is not there. One
fact, two consequences.

The clone is taken *after* the cheap rejections (child cap, narrowed
allowance), so a fork bomb never pays for one. It is born with one reference;
`proc_alloc_in` takes the child's; the constructor drops its own.

**The SP rules turn out to belong to RFMEM, not to fork.** `child_sp == 0` and
`child_sp == ctx->sp` are refused only when the two Procs write the *same*
stack. Under `RFPROC` alone both are normal — in fact `child_sp == 0` is what
`fork()` MEANS, since the child runs on its own copy-on-write copy at the same
VA. That zero is resolved to `ctx->sp` in `sys_rfork_core`, the same layer that
already resolves `child_tls == 0`, which keeps `fork_frame_init` at two
unconditional edits instead of teaching the primitive a "0 means keep" case.
Alignment and the VA_TOP bound still bind any non-zero SP under either shape.

The VFORK suspend needed no edit at all: L-3c-2 wrote its gate as
`fc && (flags & RFMEM)` on purpose, and said so.

### Two defects were found by making the clone live

Neither was visible to the tests that already covered the code they were in.

**#136 — the clone refused every real address space.** `clone_one_vma` refused
eager `ANON` outright ("no per-page ownership for a break to take"), and the
vDSO clock page is eager anon mapped read-only into *every* EL0 Proc. So the
first real fork was refused, while L-4b's tests passed — because those tests
**build** their address spaces out of exactly the VMAs they want, and the one
VMA every real Proc has was in none of them. The arm now splits on
**writability**: a writable eager-anon VMA still refuses; a read-only one is
shared, on precisely the reasoning read-only `FILE` text already used (no
prot-mutation syscall exists, so read-only is permanent and there is nothing to
break). MMIO and DMA refuse at any prot — sharing a device window is an
authority transfer, not a memory copy.

**#137 — `fi->is_write` had been ALWAYS FALSE, tree-wide, since the fault path
was written.** `ESR_ISS_WNR_BIT` was 9; WnR is ISS bit 6 (bit 9 is EA, which is
0 for every normal abort). It survived because nothing before the COW break
branched on it for *correctness* — the demand-zero and FILE arms install at
`vma->prot` whichever way it reads, so first touch works either way, and a write
to genuinely read-only memory is a buggy program nobody ran. With the wrong bit
the break's write arm is simply unreachable: the store re-installs read-only,
re-faults, and loops forever, which is why the symptom was a *hang* with no
fault logged rather than anything that looked like a memory bug.

The seam it lived in is worth stating plainly, because both sides were
"tested". The decode's own unit test **mirrored the constant** — it set bit 9
and asserted the decoder had read bit 9, so it agreed with the code instead of
with the hardware and could not have failed however wrong both were. And every
test on the *consuming* side — the COW arm's, the vDSO's write-denial one —
builds a synthetic `struct fault_info` and assigns `is_write` directly, never
executing the decode at all. Nothing in the tree composed the two. The test's
bit is now an independent literal with an ARM ARM citation, deliberately *not*
`#include`d from the kernel header: sharing the constant would restore the
tautology.

### Coverage

`fork.rfork_arg_rejection` pins the gate in both directions — the RFMEM-only
refusals must keep firing under RFMEM and must *not* fire under RFPROC alone,
and it takes both to say that (a gate that dropped them would satisfy one half,
one that applied them everywhere the other). `cow.clone_shares_readonly_eager_anon`
pairs read-only-shares with writable-still-refuses.
`cow.addrspace_clone_refuses_and_leaves_parent_intact` now distinguishes all
three clone phases on the failure path. `/fork-probe` leg K forks for real and
checks three separately-falsifiable COW claims (the child sees the parent's
pre-fork write; the child's write does not reach the parent; the parent's later
write does not reach the child); leg L proves a store to read-only text is
DENIED, which is the half of #137 leg K does not cover.

Five independent revert probes. The sharpest: **drop the clone wiring and the
unit suite stays at a full 1300/1300 while only the in-guest leg fails** — the
decision and the wiring are blind to each other, again, and in the direction
where the suite is entirely green through a broken kernel.

## Status

L-3b + L-3c-1 + L-3c-2 + **L-5** landed. Suite **1300/1300**; boot OK;
0 EXTINCTION; `asid.tla` + `cow.tla` unperturbed.

Reserved: the VIVARIUM `execve`/`wait4` rows (L-6) and the arc's focused audit
(L-7).
