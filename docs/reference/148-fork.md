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

The **handle table is fresh and empty**. That is `CLONE_VM` *without*
`CLONE_FILES` — precisely posix_spawn's shape, and deliberately not POSIX
fork's. The copy is L-3c, and #119 is its hazard: a copy would duplicate
hardware handles, which I-5 makes non-transferable.

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

## Status

Landed at L-3b. Suite 1282 → **1284/1284**; boot OK; 0 EXTINCTION; `asid.tla`
unperturbed.

Reserved: the handle-table copy + the VFORK suspend (L-3c), the VIVARIUM
`clone` row (L-3d), the COW break (L-4), and lifting the `RFPROC`-alone refusal
once COW exists (L-5).
