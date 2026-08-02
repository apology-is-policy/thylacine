# LINEAGE — process creation: `execve`, shared address spaces, and copy-on-write `fork`

**Status**: DESIGN, scripture-first, no code written. User-voted 2026-08-01
(task #93): the **full arc, through COW fork** — so VIVARIUM's stated v1.0
target ("an Alpine container runs a shell") stands rather than being narrowed.

**Binding once signed off.** Implementation deviations either update this
document first or get reverted (CLAUDE.md, "Design-first policy").

Companions: `docs/VIVARIUM.md` §9 (the fidelity ladder this arc moves entries
on), `docs/EXEC-LOAD-DESIGN.md` (REVENANT — the file-backed exec this arc
builds `execve` on top of), `docs/ARCHITECTURE.md` §7 (process model), §28
(invariants).

---

## 1. Why this exists

A Linux process creates another process by handing the kernel a
**continuation**: `clone()` returns *twice*, or spawns a child that runs a
caller-supplied function and only then calls `execve`. Thylacine has no such
primitive. Its `SYS_SPAWN_*` family is **atomic**: it takes a *program* and
produces a running Proc. There is no point at which a half-built child exists
for the parent to configure.

That difference is the whole of #93, and it is the last structural gap between
the VIVARIUM arc as chunked and the arc's own gate.

### 1.1 What the tree already decided, once, on the other side of the boundary

This is not the first time the tree has met this problem. `usr/lib/pouch/patches/0026-pouch-process.patch`
(CL-1b, landed 2026-07-23) translates the entire family for **patched** musl:

| Linux | Thylacine primitive |
|---|---|
| `posix_spawn` | `SYS_SPAWN_FULL_ARGV(49)`, file_actions resolved **statically** into the positional `fd_list` |
| `posix_spawnp` | PATH search via `/env`, then the above |
| `waitpid` / `wait4` | `SYS_WAIT_PID(22)` + flag and status-word translation |
| `pipe` / `pipe2` | `SYS_PIPE(8)` |

Its header states the conclusion plainly: *"Thylacine has NO fork/execve (a
Proc cannot clone-and-replace its image) … the upstream posix_spawn
(clone(CLONE_VM|CLONE_VFORK) -> run file_actions -> execve) cannot apply."*

**Why that does not close #93.** Pouch can resolve `file_actions` statically
because pouch *is* the library — it sees the whole `posix_spawn` call in one
place, at compile time. VIVARIUM runs **stock, already-compiled** binaries. The
continuation arrives as machine code the kernel cannot inspect. Trying to
recover the intent kernel-side is the #101 class: a mechanism that is
impossible on the side you happen to be standing on.

So the pouch patch is prior art for the *shape of the answer*, and a proof that
the shape does not generalise.

---

## 2. Ground truth

Everything in this section was measured in-tree on 2026-08-01, not inferred.
Where a later chunk contradicts one of these, **the measurement is the thing to
re-run** — do not assume this document aged well (the V-8 close found two
scripture sections that had silently stopped being true).

### 2.1 Stock musl has two shapes, not one

| Path | Kernel call | Address space |
|---|---|---|
| `fork()` → `_Fork()` | `clone(SIGCHLD, 0)` | a real **copy** |
| `posix_spawn` | `__clone(child, stack, CLONE_VM\|CLONE_VFORK\|SIGCHLD, &args)` | **shared**, child runs a continuation, then `execve`s |

`third_party/musl/src/process/fork.c`, `.../posix_spawn.c:198`.

### 2.2 Which shape does the world actually use?

- `system()` → `posix_spawn` (`src/process/system.c:35`)
- `popen()` → `posix_spawn` (`src/stdio/popen.c:41`)
- raw clone is confined to `fork.c`, `vfork.c`, `_Fork.c`, `aio.c`

So the **posix_spawn shape covers what a toolchain drives** (clang driver,
ninja, anything using `system`/`popen`), and **raw `fork()` is what a shell
needs** — busybox `ash` forks on an MMU system. Both are in scope under the
vote.

### 2.3 `CLONE_VFORK` is a lifetime requirement, not an optimisation

musl's child stack is `char stack[1024+PATH_MAX]` — a **local in the parent's
`posix_spawn` frame** (`posix_spawn.c:175`). If the parent were allowed to
return before the child execs, the child would be running on a dead frame.
Any implementation that treats VFORK as a scheduling hint is wrong.

### 2.4 The naive "child = a peer Thread" mapping is wrong

musl passes `CLONE_VM` **without** `CLONE_FILES`, so the child gets a **copied**
fd table — which is exactly why its `dup2`/`close` file_actions do not disturb
the parent. A Thylacine peer Thread **shares** the handle table, so a
thread-based child would redirect the *parent's* stdout onto the pipe.

`SYS_THREAD_SPAWN(entry, sp, arg, tls, ptid)` is structurally identical to
musl's `__clone(func, stack, flags, arg)` — and pouch's `0004-pouch-pthread.patch`
already retargets `__clone` onto it for `pthread_create`. That resemblance is a
trap: it is the right object for `CLONE_THREAD`, and the wrong one here.

### 2.5 There is no image-replace path

Every `exec_setup*` caller runs in a **freshly rforked child**: `kernel/joey.c:168`
(init), `kernel/syscall.c:5441`, `:5700`, `:6230` (the spawn thunks). `execve`
as Linux means it does not exist for native Thylacine code either.
`kernel/syscall.c:5408` states the tree's position directly: *"v1.0 has no
SYS_RFORK (which would require COW + child-context restoration); adding it
later is a separate chunk."* This document is that chunk.

### 2.6 There is no address-space object

`pgtable_root`, `context_id`, `vmas`, `vma_lock` and the I-32 `page_count` /
`vma_count` are **inline fields of `struct Proc`** —
`kernel/include/thylacine/proc.h:230`, `:231`, `:239`, `:352`, `:481`, `:508`.
The address space is created at `proc_alloc`
(`kernel/proc.c:372`) and destroyed at `proc_free` (`:631`). Nothing in the
tree can express "two Procs, one address space".

### 2.7 The Plan 9 flag vocabulary is already reserved, and unimplemented

`kernel/include/thylacine/proc.h:1148-1156`:

```
RFPROC   0x0001   create a new Proc (always required)
RFMEM    0x0002   share address space      (future P2-G)
RFNAMEG  0x0004   share territory          (future P2-E)
RFFDG    0x0008   share fd table           (future P2-F)
RFCRED   0x0010   share credentials        (future P2-G)
RFNOTEG  0x0020   share note queue         (future Phase 5)
RFNOWAIT 0x0040   detach from parent's children list
RFREND   0x0080   share rendezvous space
RFENVG   0x0100   share environment        (future P2-G)
```

Only `RFPROC` is implemented; every other flag **extincts** (`:1103`).

Note the polarity: Thylacine normalises **all** flags to *set == share*,
diverging from Plan 9, where `RFFDG`/`RFNAMEG` mean *copy*. Under Thylacine's
polarity, `rfork(RFPROC|RFMEM)` with `RFFDG` **clear** is precisely the
posix_spawn child: new pid, shared address space, copied fd table.

### 2.8 `struct page.refcount` is an allocation marker, not a share count

`kernel/include/thylacine/page.h:43` carries `u32 refcount` commented
*"BURROW refcount placeholder"*. Measured: it is written **only** by the buddy
allocator and the magazine layer — `mm/buddy.c:119/145/201/209/252`,
`mm/magazines.c:120` — always to **0 (free) or 1 (allocated)**. It is never
incremented above 1 and never read as a sharing count anywhere in the tree.

This matters because it looks exactly like the field COW wants. It is the
`T_E_PIPE` class from #100 — a field whose name states a contract nothing
keeps. COW must either promote it to a real count (and teach every buddy free
path to respect it) or carry its own structure. **Do not assume it is usable
as-is.**

---

## 3. Prior art

### 3.1 The heritage system — Plan 9

Plan 9 has **no `fork()`**. It has `rfork(flags)` with explicit sharing bits,
and the position is deliberate: implicit copy-everything is the wrong
primitive, because the caller almost never wants all of it. `rfork(RFPROC|RFMEM)`
is a first-class object — a new process sharing the parent's memory — and it is
what Plan 9's own `rfork`-based spawn idiom uses.

Thylacine inherited the vocabulary (§2.7) and implemented only `RFPROC`.

### 3.2 Linux

`clone(flags, stack, ptid, tls, ctid)` with orthogonal `CLONE_*` bits.
`fork()` is `clone(SIGCHLD)`; `vfork()` is `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`;
a thread is `clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|…)`.
The address space is `struct mm_struct`, refcounted (`mm_users`/`mm_count`) and
shared by every task that has it. COW is per-page via `_refcount`/`_mapcount`.

### 3.3 The capability-microkernel SOTA

| System | Position |
|---|---|
| **Fuchsia / Starnix** | No native fork. The Linux-compat layer creates a new `zx_process` and **COW-clones the VMOs** (`ZX_VMO_CHILD_SNAPSHOT`). |
| **Genode / Noux** | No native fork. Noux implements it by cloning the address space region-by-region. |
| **seL4** | No fork; process creation is explicit CSpace/VSpace construction. |
| **gVisor** | Implements fork in the Sentry with COW page tables. |

**Every system that runs Linux binaries implements COW.** None of them found a
way around it. That convergence is the strongest argument that the vote picked
the honest option: the alternatives all stop short of a shell.

### 3.4 Fit to Thylacine

The design below is Plan 9's *interface* (`rfork` with explicit bits — which
the tree already reserves) over the capability-microkernel *mechanism* (a
refcounted address-space object with COW). That fusion is the usual Thylacine
answer, and here it is nearly forced: the flag word already exists and the
mechanism has no alternative.

---

## 4. The structural findings

Three findings shape the arc, and each *reduces* it relative to how #93 was
originally written.

**F-A. `execve` is the common core, not one option among several.** A forked
child execs. A vfork child execs. Every path to a Linux-shaped new program
passes through image replacement in a *live* Proc. It is also independently
missing for native code (§2.5). So this is not fork-vs-vfork; it is `execve`
first, then a choice of what creates the child — and the vote takes both.

**F-B. The address-space object is the arc's spine.** `RFMEM` needs it (two
Procs, one AS). COW needs it (the child's AS is a structural clone of the
parent's). Both stages are blocked on the same refactor, so it is stage 0, not
a detail of stage 2.

**F-C. Text needs no COW.** REVENANT already maps executable text and read-only
rodata from a **shared, read-only, Image-cached `BURROW_TYPE_FILE`** (I-36).
Two Procs sharing that mapping is the *existing* design, not a new hazard — a
COW fork inherits it by taking a second `burrow_ref`. Only **writable anon**
needs COW: the data segment, the heap, and the stack. This materially narrows
the fault-arm work and should be stated up front so a future reader does not
re-derive it.

---

## 5. The design

### 5.1 Stage 0 — `struct AddrSpace`

Extract from `struct Proc` into a refcounted object:

```
struct AddrSpace {
    int         ref;            // Procs sharing this AS
    spinlock_t  lock;           // was Proc.vma_lock
    paddr_t     pgtable_root;
    u64         context_id;     // rolling ASID (I-31)
    struct Vma *vmas;
    u32         page_count;        // the I-32 RSS axis   (u32 today, keep the width)
    u32         vma_count;         // the I-32 VMA-slab axis
    u32         shared_map_pages;  // the I-32 cross-Proc shared-in axis (G-2)
};
```

**SEVEN fields, corrected at L-1 from the six this section originally listed.**
`shared_map_pages` belongs by exactly the argument that moves `page_count`:
`vma.h` states its invariant as `shared_map_pages == Σ pages of SHARED_IN VMAs`,
and both the charge and the uncharge run off the VMA list — which lives here.
Two Procs sharing an address space would otherwise keep divergent counts for one
VMA set, and the uncharge would not know whose counter to decrement.

`struct Proc` keeps a `struct AddrSpace *as`. Procs with `as == NULL` are
kernel-only (kproc), replacing today's `pgtable_root == 0` test.

**Of the 13 `pgtable_root == 0` sites, NINE convert — also corrected at L-1.**
The count was right; the claim that all 13 convert was wrong. The other four
(`arch/arm64/mmu.c` in `mmu_install_user_pte` / `mmu_uninstall_user_pte` /
`mmu_uninstall_user_range` / `cross_proc_resolve`) are defensive checks on a
`paddr_t pgtable_root` **parameter**, and the mmu API correctly keeps taking a
bare root — that layer has no business knowing what a Proc is. They are
parameter validation, not the kernel-Proc test.

The nine must be converted **as a set**: a missed predicate does not fail to
build, it silently treats a live user address space as a kernel Proc.

#### The method is forced, not chosen

Two of the seven field names are **overloaded in this tree**:

- `struct Burrow.page_count` (`burrow.h`) — the burrow's own page count, which
  has nothing to do with `Proc.page_count`, and which accounts for most of
  `burrow.c`'s ~39 mentions of the name.
- `psci_cpu_on(u64 target, u64 entry_point, u64 context_id)` — the PSCI ABI
  parameter, unrelated to the rolling-ASID context.

So a grep- or sed-driven rename corrupts unrelated code *silently*. The
conversion must instead be **compiler-driven**: delete the fields from
`struct Proc` first, and the compiler reports every real site while being
structurally incapable of flagging `v->page_count` or a `u64` parameter. That
is why "the compiler is the completeness gate" is the right method here rather
than merely a convenient one — and the measurement bore it out: 239 reported
sites, every one of them on `struct Proc`, zero false positives.

The gate has exactly **two** blind spots, and they are mirror images of each
other.

**The nine predicates.** After the fields move, `p->as->pgtable_root == 0` still
*compiles*, and NULL-derefs on a kernel-only Proc. Nine is small enough to
enumerate exhaustively, which is what makes the split safe — the compiler owns
the ~230 value reads, and the nine are converted and re-read by hand.

**Value reads that used to yield 0 for a kernel-only Proc**, which now
dereference NULL. Also not compile errors. The reachable ones were the
Proc-table walkers — `/ctl/procs` and `/proc/<pid>` walk a tree whose root *is*
kproc — plus `format_maps`, and `vma_drain` and the device quiesce on
`proc_free`'s rollback path, where a Proc can reach teardown having failed
before `addrspace_alloc` ran. Each must yield what the old inline field produced
(0, or an empty VMA list), which is what makes the refactor byte-for-byte rather
than merely compiling.

This second class is not theoretical, and L-1 proved it rather than asserting
it: **removing either the `devctl.c` guard or the `proc_page_charge` guard makes
the kernel extinct during boot** with `unhandled kernel translation fault 0x20`.
`0x20` is precisely `page_count`'s offset in `struct AddrSpace` (`ref` 0, `lock`
4, `pgtable_root` 8, `context_id` 16, `vmas` 24, `page_count` **32**) — the two
probes fault at the same address because both read that field. A guard that
cannot be shown to fail is a guard nobody has tested.

Two decisions this forces, both taken here:

**`context_id` belongs to the AddrSpace.** The rolling ASID identifies a
*translation table*, which is what the allocator always semantically meant. Two
Procs sharing an AS share one ASID — correct, and cheaper than two. The
inverse (two tables, one ASID) is the I-31 corruption the ASID arc exists to
prevent, and moving the field makes that structurally unrepresentable. **I-31
is unaffected in substance**; `specs/asid.tla` should be re-run unchanged as
the gate.

**The I-32 charge follows the AddrSpace.** `page_count` is documented as true
RSS, checked under the AS lock so it is exact. Sharing an AS means sharing the
pages, so one charge is the honest count; the per-Proc cap becomes a per-AS
cap. A COW break charges **the breaker's** AS, which is where the new page
actually lands. A fork bomb is still bounded — N children means N address
spaces, each capped.

### 5.2 Stage 1 — `execve`

Image replacement in a live Proc: resolve the program through `stalk` in the
caller's Territory (the I-28 path `exec_resolve_from_namespace` already walks),
build a **fresh** AddrSpace, and switch to it — then tear down the old one.

Build-new-then-switch, rather than tear-down-then-build, is load-bearing: a
failed `execve` must return an error to a caller whose address space is still
intact, which is POSIX's requirement and also the only way the failure is
debuggable. The old AS is dropped only once the new one is committed.

#### As built at L-2a

The ordering is the design, and it is *stricter* than Linux's:

```
1. copy path + argv into kernel memory   (from the OLD address space -- after
                                          the swap those user VAs mean something
                                          else)
2. resolve the program                   (I-28, in the caller's Territory)
3. build a DETACHED AddrSpace            (ELF parse, segments, stack, auxv)
4. [commit] swap + activate              (infallible)
5. rewrite the trapframe                 (the syscall's own eret starts it)
```

Steps 1-3 touch nothing the caller can observe, so a malformed ELF or an OOM
leaves **nothing to undo**. Linux arrives at the same place from the other side
(`bprm->mm`) having learned it the hard way: its point of no return sits
mid-exec and a failure past it kills the process.

Two mechanisms fell out of building detached, both new at L-2a:

- **`exec_load_into(as, exempt, ...)`** plus AddrSpace-taking forms of
  `vma_insert` / `vma_remove` / `vma_lookup` / `vma_drain` / `burrow_map`. The
  Proc-taking forms stay as one-line wrappers, so the ~90 existing call sites
  are untouched. The I-32 counter arithmetic moved to `addrspace.c` with the
  counters; what stays in `proc.c` is the policy (is there an address space at
  all, is this Proc exempt).
- **`sched_activate_addrspace`** — install TTBR0 *now* rather than at the next
  context switch. execve is the only path that swaps a live thread's address
  space and then erets straight back to EL0, so nothing else would ever load the
  new root.

**The activate's failure mode is silent, which is why it is worth naming.**
Removing it does not fault: the old ASID's TLB entries are still warm at the
same stack VA, so the successor reads *plausible but wrong* argv out of a
translation whose page tables have already been freed. Measured at L-2a as a
revert probe.

#### Multi-thread: refused at L-2a, built at L-2b

Linux `execve` terminates every thread but the caller. Thylacine does not yet
have the primitive for that, and the reason is worth writing down because it is
not obvious: `proc_group_terminate` flags the **Proc**, via a `group_exit_msg`
that is set-once and deliberately never cleared (I-24). Exempting the execer
from it would therefore break a *later* real kill of the same Proc, permanently.
So de_thread needs a genuinely new per-Thread die flag on the death lineage —
its own chunk, with its own audit.

Nothing in this arc is blocked by that: an `rfork(RFPROC|RFMEM)` child (L-3), a
`fork` child (L-5) and a shell (L-6) are all single-threaded when they exec.
L-2a therefore **refuses** a multi-threaded caller with `-EAGAIN` — loudly,
documented, and covered by the prover's leg C — rather than half-serving it.

### 5.3 Stage 2 — `rfork(RFPROC|RFMEM)` + VFORK

A new Proc that takes a **reference** to the parent's AddrSpace instead of a
clone, with its own pid, handle table (copied), Territory and note queue.

VFORK suspends the parent on a rendez until the child either execs or exits.
Per #811 it must be **death-interruptible**: a parent killed while suspended
must unwind, and a child that dies without ever execing must wake the parent
rather than strand it. That is the `EventuallyResumed` shape from
`debug_stop.tla` and should be modelled the same way.

#### As built at L-3a — the share substrate

`proc_alloc_in(share)` takes a reference to an existing AddrSpace instead of
allocating one (`proc_alloc()` is now `proc_alloc_in(NULL)`), and
`rfork_internal` routes `parent->as` into it when `RFMEM` is set. Note what is
*not* conditional on the flag: Territory, note queue, environment and handle
table remain the child's own, because each is governed by its own flag
(`RFNAMEG`, `RFNOTEG`, `RFENVG`, `RFFDG`), all still refused. That separation is
why Plan 9 uses a flag word at all, and the Linux shape depends on it —
posix_spawn passes `CLONE_VM` *without* `CLONE_FILES` precisely so the child's
`dup2`/`close` cannot disturb the parent (§2.4).

**The load-bearing change is a teardown move, not an allocation one.** The VMA
list is a property of the address space, so it is now drained by
`addrspace_unref`'s last drop rather than by each Proc that dies. The two were
indistinguishable while `ref` could never exceed 1; under `RFMEM` they separate,
and **both** existing callers had the bug latent:

- `proc_free` drained unconditionally — the first sharer to die would have
  unmapped the survivor;
- `proc_exec_replace` (L-2a) drained the outgoing space unconditionally — and
  the reachable case is the ordinary one, not an exotic corner, since a vfork
  child execs while its parent is suspended on exactly that space.

Moving the drain fixes both at the layer where the list belongs, rather than
repeating a gate at each site.

`RFMEM` from a Proc with **no** address space is refused rather than quietly
downgraded to a private one. Only kproc is in that position, so this is a
programming error rather than a user path — but "the flag was ignored" and "the
flag was honoured" would otherwise be indistinguishable from outside, and the
caller would learn better only when the child failed to see a write.

I-31 needed nothing: `asid_resolve` keys on `as->context_id`, so one space is
one ASID however many Procs hold it. That is a direct dividend of L-1 having
moved `context_id` into the object, and `asid.tla` re-ran unperturbed.

**Not proven end to end here.** A *successful* `rfork(RFPROC|RFMEM)` has no
kernel-test witness: the only Proc a kernel test runs on is kproc, which has no
address space by construction, and lending it one while a real child thread ran
against it would make every `as == NULL` kernel-Proc gate answer the wrong
question. So L-3a pins the mechanism at the two layers a kernel test can reach
(`addrspace_unref`'s drain point and `proc_alloc_in`'s share) plus the refusal
through the real `rfork`, and the end-to-end proof lands with the EL0 surface —
the same split L-2a made, where the detached build was unit-tested and the swap
needed `/exec-probe`.

### 5.4 Stage 3 — COW and `SYS_RFORK`

`fork()` = `rfork(RFPROC)` with the AddrSpace **structurally cloned**: same
VMAs, same Burrows, writable-anon mappings installed **read-only in both**
parents and children. A write faults, and the fault arm breaks the share.

Two mechanisms are new:

**The COW break.** On a write fault to a VMA marked COW: if this is the last
sharer, re-install writable in place (no copy — the common case after one side
execs). Otherwise allocate, copy, drop the share, install private, charge the
breaker's I-32 budget. This needs a real per-page share count, which §2.8 shows
does not yet exist.

**Child-context restoration.** The child must resume at the *parent's* EL0 PC
with `x0 = 0`. Today every child begins at an ELF entry point via a thunk. The
fork child instead needs a **copy of the parent's trapframe**, and it must be
taken at a point where that frame is coherent — the syscall entry frame, the
same object `#88` taught `/proc/regs` to read at the EL0-sync choke point.

> **Correction (L-3a, #117): this belongs at stage 2, not stage 3.** Re-measured
> against `third_party/musl/src/thread/aarch64/clone.s`: the raw `clone`
> syscall **returns twice** (`cbz x0,1f` — pid in the parent, 0 in the child).
> The child resumes at the same PC with `x0 = 0` and `SP` replaced by the stack
> argument, and only *then*, in userspace, pops the function pointer off that
> stack and calls it. The kernel never sees a function pointer, so there is no
> "start the child at an entry point" primitive to build — the `CLONE_VM` shape
> needs the identical trapframe copy the fork shape does.

#### As built at L-3b — child-context restoration + `SYS_RFORK`

**`SYS_RFORK = 102` is the tree's first syscall that returns twice.** Both Procs
resume at the instruction after the same `svc`, on the same saved frame; `x0` is
the only thing that tells them apart. Three pieces:

- **`fork_frame_init(dst, src, child_sp)`** — the decision, and deliberately a
  pure function so a test can reach it without a Proc: `dst` is `src` with
  exactly two edits (`regs[0] = 0`, `sp = child_sp`) and everything else copied
  verbatim. `elr` verbatim is what "resumes at the same instruction" means;
  `spsr` verbatim carries the parent's NZCV (live if a conditional follows the
  syscall) and cannot be EL0-forged, because the hardware wrote it on exception
  entry. Field-by-field, not `*dst = *src`: a 288-byte struct assignment
  compiles to a `memcpy` the freestanding kernel does not link.
- **`thread_create_forked`** — the third creation shape, and the only one that
  RESTORES rather than constructs. It carves the child's frame off the top of
  the child's own fresh kernel stack, at the exact address `KERNEL_ENTRY` would
  have chosen, and points `ctx.sp` at it. FP/SIMD is inherited from the **live**
  registers via `fp_save_area` — not from the caller's saved `Context`, which
  holds its last switch-OUT values and is stale while the caller is running.
- **`thread_fork_trampoline`** — release, `el0_return_die_check` (#811 first-
  entry, exactly as `thread_user_trampoline` does it), mask DAIF, then
  `b .Lexception_return`. It lives in `vectors.S` rather than `context.S`
  precisely so it can branch to that **local** label: reusing the one audited
  `KERNEL_EXIT` beats hand-rolling a second eret on what that macro's own
  comment calls "the single most load-bearing exit in the kernel". Note the
  absence of a GPR-zeroing sweep — `thread_user_trampoline` has one because a
  fresh thread must not inherit kernel residue, but this child is *continuing
  its parent's userspace frame*, so zeroing x1..x30 would break the fork rather
  than harden it.

**Two things are deliberately NOT here.** The child's handle table is fresh and
empty (`RFFDG` unsupported) — `CLONE_VM` *without* `CLONE_FILES`, which is
exactly posix_spawn's shape and not POSIX fork's; the copy is L-3c, and #119 is
its hazard (a copy would duplicate hardware handles, which I-5 forbids). And
`RFPROC` alone is **refused**, not served: without `RFMEM` the child gets a
fresh empty address space, so resuming it at its parent's PC faults on the first
instruction fetch. A private child address space means copy-on-write, which is
L-4.

##### The gate this chunk was given was unprovable, and measuring said so

The L-3a build-arc table set L-3b's gate as *"a **kernel-driven** RFMEM child
resumes at its parent's PC"*. That cannot be observed. The only Proc a kernel
test runs on is kproc, which has neither an address space to share
(`addrspace.kproc_has_none`) nor an EL0 trapframe to fork from — so a
kernel-driven resume is not merely untested, it is **unobservable**. The gate
therefore moved to an EL0 caller, which is why `SYS_RFORK` landed here rather
than at L-3c as originally sequenced.

The coverage splits cleanly, and the split is the point:

| claim | kernel test | `/fork-probe` |
|---|---|---|
| the frame's contents (x0 = 0, sp replaced, rest verbatim) | **yes** (`fork.frame_init`) | indirectly |
| the argument gate rejects malformed requests | **yes** (`fork.rfork_arg_rejection`) | leg F, from a Proc that *has* a space |
| the child actually erets and resumes | no | **yes** |
| the two Procs share the address space | no (kproc has none) | **yes** |
| the child runs on its own stack | no | **yes** |

Revert-probed to demonstrate exactly that: making `thread_create_forked` ignore
`child_sp` entirely leaves the **unit suite fully green** while only the
in-guest leg fails (`the child did not exit normally`). A kernel test cannot see
this bug at all.

##### A userspace consequence worth stating: the child comes back on a different stack

`SYS_RFORK` cannot be wrapped by an ordinary `asm!` the way every other syscall
can. After the eret the child holds every one of the parent's registers except
`x0` — including `x29` (frame pointer) and `x30` (link register), both pointing
into the **parent's** stack — while `SP` points at fresh memory. Frame pointer
and stack pointer describe two different stacks; any compiler-generated local
access, any epilogue, any `ret` would touch the wrong one.

musl's `clone.s` solves this the only way it can be solved, and
`libthyla_rs::rfork_spawn` is a transliteration: the caller's function pointer
and argument are pushed onto the **child's** stack before the syscall, and the
child's first act is to pop them and `blr`, establishing a correct frame on its
own stack before any compiled code runs. It also zeroes `x29`/`x30` first, so a
backtrace cannot walk into the parent's stack.
>
> So the only thing separating stage 2 from stage 3 is whether the AddrSpace is
> **shared** or **COW-cloned**. Child-context restoration is common to both and
> lands at L-3; L-5 keeps `SYS_RFORK`'s remaining surface and the fork mode.
> This is §2's instruction working as intended — the measurement is the thing to
> re-run, and this document did not age well on exactly this point.

W^X (I-12) holds throughout: a COW page is R or RW, never X — text is not COW
at all (F-C), so no page transits a writable state while executable.

---

## 6. Invariants

**Composed, not new**: I-1 (isolation — a shared AS is shared only with a
descendant that asked), I-2 (`rfork` still strips `CAP_ELEVATION_ONLY`), I-7
(the #847 dual refcount — a Burrow mapped into two address spaces), I-12 (W^X
across the COW break), I-21 (a thread runs on ≤1 CPU — unchanged, but the AS
swap must respect it), I-24 (`execve`'s peer-thread cascade), I-28 (`execve`
resolves through `stalk`), I-31 (ASID — see §5.1), I-32 (page and VMA budgets
under sharing), I-36 (REVENANT text, shared read-only by construction).

**New invariant I-44** — *address-space integrity under sharing and
copy-on-write*. The number is **taken now and locked**, following the I-42
precedent ("takes its §28 number now to lock it against I-41"): reserving early
is what stops a concurrent arc claiming it. It is RESERVED, becoming ENFORCED at
L-4/L-5 when the COW break and `SYS_RFORK` land. An
AddrSpace's pages live until the last referencing Proc drops it **and** the
last mapping is gone; a COW break yields a private page whose contents equal
the shared page at the instant of the fault and leaves every other sharer's
view unchanged; no page is ever writable through one mapping and executable
through another.

**Spec-first is RE-ENABLED for this surface.** This is re-enabling point (a) as
CLAUDE.md defines it: the COW break racing a concurrent break, an exit, and an
`execve` on the same AddrSpace is precisely the subtle SMP class that
machine-checked exploration catches and tests do not — the same argument that
re-enabled `asid.tla` and `death_wake.tla`. A model lands **before** stage 3's
implementation, with buggy cfgs for at least: break-vs-break (two CPUs faulting
the same shared page), break-vs-teardown (a sharer exiting mid-break), and the
lost-VFORK-wake.

---

## 7. The build arc

Each row is a chunk with its own gate. Audit rounds where marked; the whole
arc is audit-bearing, so the trigger table in `ARCHITECTURE.md` §25.4 and
`CLAUDE.md` gains a row at L-1.

| Chunk | Scope | Gate |
|---|---|---|
| **L-0** | This document + the ARCH §25.4 row + the §28 invariant number + `VIVARIUM.md` §9/§10 updates. **Scripture only, no code.** | user signoff |
| **L-1** ✅ **LANDED** | `struct AddrSpace` extraction. Pure refactor: `ref` is 1 everywhere, nothing shares yet. **Seven** fields moved; `struct Proc` 408 → 376 B; 239 compiler-enumerated conversion sites across 22 files; 23 offset asserts re-baselined. As-built: `docs/reference/146-addrspace.md`. | **MET** — 1272/1272 → 1276/1276 (the four new `addrspace.*`), boot OK, 0 EXTINCTION, `asid.tla` clean (443457 states, depth 18), SMP gate; the nine Proc-field predicates converted as a set (the four `mmu.c` parameter checks correctly did not) |
| **L-2a** ✅ **LANDED** | `execve` (stage 1) for a **single-threaded** Proc: the AddrSpace-targeted load path (`exec_load_into` + the `*_in` VMA/burrow forms), `proc_exec_replace`, `sched_activate_addrspace`, `SYS_EXECVE = 101`. A multi-threaded caller is refused with `-EAGAIN`. | **MET** — `/exec-probe` re-execs itself and the successor prints from an fd inherited across the swap; a **failed** execve returns with the caller's address space intact; all three load-bearing pieces revert-probed to *distinct* failures |
| **L-2b** | The de_thread primitive: a per-Thread die flag + wait-for-EXITING + reap, removing L-2a's multi-thread refusal. | a multi-thread Proc's peers are all EXITING and off-CPU before the swap; own audit round |
| **L-3a** ✅ **LANDED** | The share substrate: the VMA drain moves into `addrspace_unref`'s last drop (fixing the same latent bug in `proc_free` *and* `proc_exec_replace`), `proc_alloc_in(share)`, `rfork_internal` accepts `RFPROC\|RFMEM`, the device quiesce gates on sole ownership, `RFMEM`-without-a-space refused. No EL0 surface. | **MET** — 1279/1279 → 1282/1282, boot OK, 0 EXTINCTION, `asid.tla` unperturbed (443457, depth 18); all three pieces revert-probed to *distinct* failures |
| **L-3b** ✅ **LANDED** | Child-context restoration (pulled forward from L-5 per §5.4's correction) **+ the `SYS_RFORK = 102` EL0 surface**, which had to come with it: `fork_frame_init`, `thread_create_forked`, `thread_fork_trampoline`, `rfork_forked`, and `libthyla_rs::rfork_spawn` (the musl-`clone.s` shim the different-stack return demands). The handle-table copy moved OUT to L-3c — it is orthogonal, and #119 is a decision that deserves its own reasoning. | **MET, but not by the gate as written** — that gate said *kernel-driven*, and measuring showed a kernel test cannot observe an eret to EL0 at all (kproc has neither an address space nor a frame). Replaced by `/fork-probe`: six legs, boot-fatal. Revert-probed to the sharpest available pairing — ignoring `child_sp` leaves the **unit suite fully green** while only the in-guest leg fails |
| **L-3c** | The handle-table copy (§2.4; #119 — a copy would duplicate hardware handles, which I-5 forbids, so this is skip-or-refuse and the choice must be stated) + the VFORK suspend. | a native prover forks-and-execs; parent-suspend released on both exec and exit; a killed parent unwinds (#811) |
| **L-3d** | The VIVARIUM `clone` row. | **a stock `posix_spawn` binary runs in a vivarium** |
| **L-4** | Per-page share counts + the COW break arm (stage 3a). | the model (§6) TLC-green **first**; then break-vs-break under `-smp 8` |
| **L-5** | Stock `fork()`: `RFPROC` alone, admitted once L-4's COW break can give the child a private-but-populated address space. (`SYS_RFORK` and child-context restoration are **already built** — L-3b — so what remains here is only lifting the `RFPROC`-alone refusal.) | stock `fork()` returns twice, correctly, in a vivarium |
| **L-6** | The VIVARIUM phenotype rows: `clone`/`execve`/`wait4`/`vfork`. | **the arc gate — an Alpine `/bin/sh` runs a command** |
| **L-7** | Focused audit (Fable, max effort) over L-1..L-6 + the full SMP gate + docs. | close |

Ordering is forced, not chosen: L-1 blocks L-3 and L-4 (F-B), L-2a blocks L-3
and L-5 (F-A), and L-4 blocks L-5. **L-2b blocks nothing in the arc** -- every
consumer (the RFMEM child, the fork child, a shell) is single-threaded at the
moment it execs -- which is why it could be split out rather than pulled
forward.

L-3 split into four because its gate ("a stock posix_spawn binary runs")
turns out to need four independent mechanisms, only the first of which is about
address-space sharing at all. Each lands with its own gate; the *arc*'s gate is
unchanged and still sits at the end.

---

## 8. Effect on the VIVARIUM fidelity ladder

`VIVARIUM.md` §9 currently lists **process creation** under OUT, recorded at
the V-8 close. On completion of L-6 it moves to **IN**, and §9's v1.0 target
regains its second clause honestly rather than by assertion.

Until then it stays OUT and the §9 status paragraph stays as the V-8 close left
it: the single-process clause MET and boot-gated, the shell clause explicitly
not. **A chunk landing is not the trigger — L-6's gate is.** Moving the ladder
entry before an Alpine shell actually runs would reproduce exactly the WSL1
failure §9 exists to prevent.

---

## 9. Open questions for signoff

1. **Does `SYS_RFORK` expose the whole Plan 9 flag word to EL0, or only the
   subset this arc implements?** `RFCRED` and `RFNOTEG` are privilege-adjacent
   (I-2, I-19). Recommendation: expose only `RFPROC|RFMEM` and reject the rest
   as today, so each remaining flag arrives with its own reasoning rather than
   inheriting approval from this arc.
2. ~~The new invariant's number~~ — **resolved: I-44**, taken and locked per
   §6. Treated as **one** invariant rather than two: AS sharing and COW are the
   same claim about when a page may be observed and when it may be freed, and
   splitting them would let a future change satisfy one while breaking the
   other. Recorded here because the alternative was considered, not overlooked.
3. **`struct page.refcount`**: promote it to a real share count, or carry a
   separate COW structure? Promotion touches every buddy free path — a
   correctness-critical, well-audited surface — so this deserves its own
   measurement at L-4 rather than a decision here.
4. **Is `vfork()` itself in scope**, or only `posix_spawn`'s use of
   `CLONE_VFORK`? Stock musl has `vfork.c`, but almost nothing calls it
   directly.

---

## 10. Naming

**LINEAGE**: descent, and what an offspring inherits from its parent — which is
literally the job of the `rfork` flag word this arc implements. Drawn from
CLAUDE.md's thematic vocabulary ("lineage, taxon, clade, crepuscular"). `CLADE`
is taken (the LLVM arc), `POUCH` is taken (the Linux compat layer), and `JOEY`
is taken (init) — the reproductive vocabulary is unusually crowded in this tree
precisely because it was the natural well to draw from.

This names a new document, not a rename of any load-bearing identifier, so no
ABI or tooling surface is affected.
