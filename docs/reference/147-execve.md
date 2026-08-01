# 147 — `SYS_EXECVE` (LINEAGE L-2a)

**Status**: as-built at L-2a. Scripture: `docs/LINEAGE.md` §5.2; invariant
`ARCHITECTURE.md` §28 **I-44**; audit-trigger row `ARCHITECTURE.md` §25.4.

**Files**: `kernel/syscall.c` (`sys_execve_handler`), `kernel/exec.c`
(`exec_load_into`), `kernel/proc.c` (`proc_exec_alone`, `proc_exec_replace`),
`kernel/sched.c` (`sched_activate_addrspace`), `kernel/vma.c` + `kernel/burrow.c`
+ `kernel/addrspace.c` (the AddrSpace-taking forms).

---

## Purpose

Replace the calling Proc's program image in place. This is the first Thylacine
syscall that changes a **live** Proc's address space — every other path (the
whole `SYS_SPAWN_*` family) creates a fresh child and loads into its empty one.

It exists because a Linux process creates another by handing the kernel a
*continuation*, and every route to that — a forked child, a vfork child, a shell
— passes through image replacement. It is also independently missing for native
code: before L-2a, nothing in the tree could re-exec.

---

## The ordering is the design

```
1. copy path + argv into kernel memory   from the OLD address space
2. resolve the program                   I-28, in the caller's Territory
3. build a DETACHED AddrSpace            ELF parse, segments, stack, auxv
4. [commit] swap + activate              infallible
5. rewrite the trapframe                 the syscall's own eret starts it
```

Steps 1–3 touch nothing the caller can observe. A malformed ELF or an OOM
therefore leaves **nothing to undo**: `execve` returns `-errno` to a Proc whose
address space was never opened. That is POSIX's requirement, and it is also the
only version of the failure that is debuggable.

Linux arrives at the same place from the other side (`bprm->mm`), having learned
it the hard way — its point of no return sits mid-exec, and a failure past it
kills the process rather than returning an error.

**Step 1 must come first for a reason that is easy to miss**: `path` and
`argv_data` are user pointers into the *outgoing* address space. After the swap
those VAs name something else entirely.

---

## Why the load path grew an AddrSpace parameter

Building detached means the load has to target an address space that is not any
Proc's `as` yet. So `exec_load_into(as, exempt, ...)` takes the target
explicitly, and the four VMA primitives plus `burrow_map` gained `_in` forms.

The Proc-taking forms remain as one-line wrappers — resolve `p->as`, ask
`proc_resource_exempt` for the I-32 policy verdict — so the ~90 existing call
sites are untouched. Only exec uses the new ones.

The I-32 counter *arithmetic* moved into `addrspace.c` alongside the counters it
operates on; `proc.c` keeps the two things that are genuinely about the process:
"is there an address space at all" (kproc has none) and "is this Proc exempt".

**What did NOT move**: the two Proc-side stamps (process name, `exe_path`) stay
out of the shared load body. `exec_setup_from_spoor` applies them inline because
its Proc is a fresh child that gets discarded on failure; `sys_execve_handler`
applies them *after* the commit, because a name stamped before a load that then
failed would leave a live Proc advertising a program it is not running.

---

## `sched_activate_addrspace` — and why its failure is silent

Every other TTBR0 change in the tree happens at a context switch, where
`cpu_switch_context` loads the value out of `ctx`. execve is the sole path that
swaps a live thread's address space and then returns straight to EL0 through the
exception frame. No switch happens, so nothing would ever load the new root.

IRQs are masked across the pair for two independent reasons:

- `asid_resolve` publishes into **this** CPU's active slot, and its contract is
  that the caller has IRQs masked on the CPU the address space is about to run
  on;
- `cpu_switch_context` **saves** the live `TTBR0_EL1` into `prev->ctx.ttbr0`
  (`context.S`: `mrs x9, ttbr0_el1`). A preemption between the ctx write and the
  `msr` would overwrite the freshly-composed value with the stale hardware one.

The `isb` is what makes this a barrier for the caller: after it, nothing on this
CPU is still walking the old root — which is exactly `addrspace_unref`'s
precondition.

**Measured at L-2a (revert probe): removing the activate does not fault.** The
old ASID's TLB entries are still warm at the same stack VA, so the successor
reads *plausible but wrong* argv through page tables that have already been
freed. It is a silent-corruption failure mode, not a loud one, which is why the
call has a paragraph rather than a line.

---

## What the old address space's teardown rests on

`addrspace_unref` performs no TLB maintenance, and neither does `vma_drain`
(measured — it frees Vma structs and drops Burrow mapping refs, nothing more).
What makes that sound is the **ASID tag**, not any earlier invalidation:

- every user PTE is non-global (`PTE_NG`), so a stale entry is reachable only
  under its own address space's hardware ASID;
- the rolling allocator's per-CPU `flush_pending` local flush runs before that
  ASID value can go live again (ARCH §6.2.1).

Each caller separately owes "no CPU is translating under this ASID *now*":
`proc_free` by having reaped and `on_cpu`-spun every thread, `proc_exec_replace`
by writing the new TTBR0 (a *different* ASID) and `isb`-ing first.

---

## What a new image does and does not inherit

**Reset** (POSIX, and each for a concrete reason):

| | Why |
|---|---|
| note handler (`handler_va`) | an inherited handler is an address in an image that no longer exists |
| Linux signal dispositions (`sigtab`) | same; lazily re-allocated |
| the thread's note mask | per-image policy |
| `TPIDR_EL0` | musl's `__pthread_self` reads it; a stale value is a live pointer into the old image's TCB |
| FP/SIMD (V0–V31, FPSR, **FPCR**) | FPCR carries rounding mode and trap enables, which a fresh image expects at defaults |

TLS and FP are written to the **hardware**, not just to `ctx`: execve erets
straight back to EL0, so a `ctx`-only reset would not take effect until a context
switch that may never come.

**Kept** (and each is correct):

- **the handle table** — POSIX keeps fds across exec (there is no `O_CLOEXEC` at
  v1.0). The prover leans on this: its post-exec PASS line is written to an fd
  inherited across the swap.
- **the Territory**, the process tree, identity, capabilities, `/env`.
- **the pid** — which is what distinguishes execve from spawn, and what the
  prover checks so a degradation to "spawn a child and exit" would fail.
- **the phenotype** (VIVARIUM I-43). It is a spawn-time declaration; execve does
  not change it. Outside a vivarium the answer is always native anyway (§12.1
  rule 3).

**A consequence worth naming**: a Loom ring or Weft binding registered before the
exec keeps its kernel object (the fd survives) but loses its *mapping* — the new
address space has none. The G-2 audit's identity guard already covers the weave
case, since it re-checks that the VMA at the recorded `guest_va` is still backed
by the same Burrow. The same class exists on Linux; a shared mapping's fd
survives exec and the mapping does not.

---

## Errors

| | |
|---|---|
| `-ENOENT` / `-EACCES` / `-ENOTDIR` … | the path did not resolve (I-28 applies unchanged: contained at the Territory root, per-component X-search, OEXEC on the leaf) |
| `-EINVAL` | bad arguments — **and also** "not a loadable static ELF", which POSIX calls `ENOEXEC` |
| `-ENOMEM` | out of memory building the new image |
| `-EFAULT` | a user pointer was unreadable |
| `-EAGAIN` | the Proc has more than one live thread |

**The `ENOEXEC` gap is real and tracked.** `docs/ERRORS.md` is ABI-bearing and
its additions need signoff, so the code reports `EINVAL` and the gap is recorded
rather than closed by fiat. It matters at L-6: a shell reads `ENOEXEC` as "this
is a script, re-run it under an interpreter", and `EINVAL` as "bad arguments".

---

## The multi-thread refusal

POSIX says exec terminates every thread but the caller. L-2a **refuses** a
multi-threaded caller with `-EAGAIN` instead, and the reason is structural rather
than a matter of effort.

The only "terminate these threads" primitive in the tree is
`proc_group_terminate`, which flags the **Proc** via a `group_exit_msg` that is
set-once and deliberately never cleared (I-24). Exempting the execer from that
flag would make a *later*, genuine kill of the same Proc silently ineffective —
permanently. So de_thread needs a new per-Thread die flag on the death lineage
(#788/#806/#860/#809/#811/#68/#89), which is its own chunk with its own audit.

Nothing in the LINEAGE arc is blocked by it: an `rfork(RFPROC|RFMEM)` child
(L-3), a `fork` child (L-5) and a shell (L-6) are all single-threaded at the
moment they exec.

---

## Tests

**Kernel unit** — `execve.load_into_detached`, `execve.load_into_rejects_dirty`,
`execve.failed_load_leaves_target_drainable` (in `kernel/test/test_exec.c`, which
already had the synthetic-ELF and blob-Dev fixtures). These cover the detached
build; they cannot reach the swap, because `proc_exec_replace` needs a live EL0
thread with a real trapframe.

The detached-build test is non-vacuous on `vma_count`: a build that targeted
`p->as` would install the same VMAs and pass every shape check, just on the wrong
object.

**In-guest** — `/exec-probe`, boot-fatal. One binary, two incarnations: it runs
the failure legs, then execve's *itself* with a marker argument, and the PASS
line comes from the incarnation that replaced it. Legs: A (a failed execve
returns and the caller keeps running), A2 (the heap and pid survive it), B
(argument validation), C (the multi-thread refusal, with a real spawned peer), D
(the swap).

**Revert probes**, three, each failing distinctly:

| Removed | Result |
|---|---|
| `sched_activate_addrspace` | silent wrong argv (stale translation) — no fault |
| the `proc_exec_alone` gate | `EXTINCTION: proc_exec_replace: a live peer thread appeared` (the inner re-check catches it) |
| `ctx->elr = entry` | execve "returns 0", then `snare:segv` at `pc=0` |

---

## Known caveats

**Not tested, argued**: the FP/SIMD and TLS resets. Both are fidelity properties
with no in-guest observer at v1.0 — a native binary that inspected FPCR before
and after its own exec could pin them, and none exists. Recorded here rather than
claimed as covered.

**`proc_exec_replace` is infallible on purpose.** If a future change makes any
step of it able to fail, the "a failed execve returns with the caller intact"
property goes with it — the fix is to move the failable work before the commit,
not to add a rollback.

**Do not relax the multi-thread refusal without the per-Thread flag.** The
tempting shortcut (suppress `group_exit_msg` for the execer) is the one that
breaks a later kill permanently.
