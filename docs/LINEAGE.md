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
*"BURROW refcount placeholder"*. It looks exactly like the field COW wants, and
it is not. It is the `T_E_PIPE` class from #100 — a field whose name states a
contract nothing keeps. **Do not assume it is usable as-is.**

> **Correction (L-4).** The two specific claims this section originally made
> were both **wrong**, and re-measuring at L-4 — which §2's own preamble
> instructs — is what caught them. They said the field is written *"**only** by
> the buddy allocator and the magazine layer"* and *"never incremented above
> 1"*.
>
> **SLUB writes it at seven sites** (`mm/slub.c:173/190/215/221/251/255/264`),
> where a slab page's `refcount` is the **inuse count** — `slab->refcount++`
> per allocated object, compared against `c->objects_per_slab`, which for a
> 48-byte cache in a 4 KiB slab is **85**. So it is both written outside the
> two named layers and incremented far above 1.
>
> The source that proves it is the very line §2.8 quoted. `page.h:48` reads
> `"BURROW refcount placeholder; slab: inuse count"` — the original text
> quoted the half before the semicolon and stopped. This is §2's instruction
> firing on §2 itself.

What is true, measured at L-4:

- **It is written per-BLOCK-HEAD, not per-page.** `alloc_locked` sets
  `p->refcount = 1` on the head page of the returned block only
  (`mm/buddy.c:209`); the other 2^order − 1 pages are never written by any
  allocation or free path. On any order > 0 block the field therefore holds
  *stale values from previous use* on every tail page. Promotion is not an
  increment away — it means making the field per-page-maintained across
  `alloc_locked` / `free_locked` / `zone_free_chunk` / `buddy_zone_init`, i.e.
  a loop over 2^order pages on both hot allocator paths, for a property only
  user anon pages ever need.
- **It is already double-booked**, per the correction above. A third meaning
  would make "what does this field mean here?" a three-way case keyed on
  `flags`.

> **Scope, established at L-4b.** Everything above is an argument about *reusing
> this field*, and all of it stands. It is **not** an argument against a per-page
> count as such, and L-4b builds one: a new `struct page.cow_share`, taking the
> free `_pad` so `sizeof` is unchanged, maintained only by the Burrow layer at the
> three sites that put a page into an anon slot. Neither of the two live
> objections reaches it — the allocator never touches it, so per-block-head
> staleness cannot arise, and it carries one meaning rather than a third. The
> third objection ("promotion would still not give eager anon per-page ownership")
> is moot: L-4a delivered exactly that ownership, which is why it was the
> prerequisite. See §5.4's L-4b correction for why the count must be per-page at
> all — the short version is that freeing a shared page requires knowing how many
> holders remain, and that is a fact about the page, not about anyone's slot.

### 2.9 Eager anon has no per-page ownership — and that, not the counter, is what blocks COW

Measured at L-4, and it is the finding that shapes the whole chunk. A COW break
is **per-page**: one page becomes private while its neighbours stay shared. The
two anonymous Burrow types have opposite ownership models:

| Type | Backing | Freed as |
|---|---|---|
| `BURROW_TYPE_ANON_LAZY` | sparse `filepages[]` of **order-0** pages | each page individually (`burrow.c:380`) |
| `BURROW_TYPE_ANON` (eager) | **one** `alloc_pages(order)` block | one `free_pages(v->pages, v->order)` (`burrow.c:320`) |

**You cannot free one page of a buddy block**, and buddy has no
split-an-*allocated*-block operation — splitting happens only on the way out of
`alloc_locked`. So in the eager model there is no per-page thing for a share
count to index, no matter where the count lives.

And **every writable anon a fork must break is eager**:

| Site | What | Size |
|---|---|---|
| `exec.c:143` | the user stack | `EXEC_USER_STACK_SIZE` = 1 MiB |
| `exec.c:519` | writable data + bss (`map_eager_from_file`) | per-binary; see below |
| `exec.c:93` | the blob path's writable segments | per-binary |
| `syscall.c:4331` | eager `SYS_BURROW_ATTACH` | per-caller |

The lazy path is reached only by `SYS_BURROW_ATTACH_LAZY` (`syscall.c:4457`) —
the mmap heap. So the per-page ownership COW needs exists precisely where COW
does *not* need it, and is absent everywhere it does.

**Measured sizes make this concrete**, and they also make the fix free rather
than costly. `llvm-readelf -l` on the built tree:

| Binary | RW `FileSiz` | RW `MemSiz` |
|---|---|---|
| `ut` | 0 | 0x61 (97 B) |
| `joey` | 8 B | 0x56349 (345 KiB) |
| `corvus` | **128 B** | **0x1802c91 (24 MiB)** |

The writable segment is ~all `.bss`. `map_eager_from_file` sizes the Burrow by
**`memsz`**, so corvus's exec calls `burrow_create_anon(0x1803000)` → 6147 pages
→ `order_for_pages` = **13** → `alloc_pages(13, KP_ZERO)` = **8192 pages = 32
MiB**, eagerly allocated *and* zero-filled, for 128 bytes of real data — with
2045 pages (8 MiB) of that being pure power-of-two rounding. Tracked as **#130**;
the fix is L-4a, because demand-zero bss and per-page ownership are the same
change.

### 2.10 Prior art converges on per-page ownership inside the memory object

Checked before choosing a mechanism, per CLAUDE.md's "research prior art before
surfacing a design fork":

- **Plan 9** (the heritage): a `Segment` holds a page-table of per-page `Page *`
  pointers; `Page.ref` **is** the share count; `fork` → `dupseg` bumps the
  per-page refs, and `fixfault` on a write to a shared page calls
  `duppage`/`copypage`.
- **Linux**: per-page `_refcount` / `_mapcount`; ownership is the page table
  itself; the break is `do_wp_page`.
- **Zircon/Fuchsia**: `VmObjectPaged` holds a `VmPageList`; a COW clone
  references its parent and a write forks that page into the child's own list.
- **seL4**: no kernel COW at all — userspace builds it.

All three that have COW put **per-page ownership inside the memory object**.
Thylacine already has exactly that shape — in `BURROW_TYPE_ANON_LAZY`. The
answer is therefore not to invent a second ownership concept but to *reach the
one the tree already has*, which is what L-4a does.

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

#### As built at L-6a — the split, and what envp measured

The phenotype cannot hand `SYS_EXECVE` a user VA: Linux passes a
NULL-terminated array of pointers, and the concatenated blob has to be built in
kernel memory. So the exec itself becomes **`sys_execve_core`**, taking
arguments already copied in, with two front ends producing them — the native
one from its user-VA blob, the phenotype one by walking the `char *const
argv[]`. One implementation of the ordering above, two argument shapes.

Two placements fell out of that and are worth stating because both are load-
bearing rather than tidy:

**The packing contract is validated in the core, below both builders.**
`exec_build_init_stack` EXTINCTS on a NUL count that disagrees with `argc`, so
a mis-built blob must be caught before it reaches the loader. Validating once,
under both front ends, turns a bug in either into `-EINVAL`.

**The blob is the caller's on every path.** The pre-split body freed it inside
what is now the core; carrying those frees across the split made it a **double
free on every execve** — not a leak but a heap corruption, which surfaced as a
mangled argv blob in an *unrelated later spawn* rather than anywhere near
execve. The comment stating the ownership rule was written before the body was
adjusted to match it.

**envp declines when non-empty, and the measurement is why.** The argument-
domain rule (VIVARIUM §4) admits only values whose effect the native mechanism
reproduces exactly. Linux's envp means "the new image's environment is exactly
this", and Thylacine cannot produce that effect **at any layer**:
`exec_build_init_stack` writes a lone NULL for envp in both frame shapes
(`exec.c:405` Shape A, `:438` Shape B), and musl's `__libc_start_main` does
`__environ = envp`. So every program — native, pouch, and phenotyped — starts
with an empty environment, `/env` is the only channel, and only the Go fork
reads it. Writing envp into `/env` would therefore not honour it; it would only
move the loss. An empty envp is served exactly (the guest asked for nothing);
a non-empty one is refused, which makes the decline a **detector** for whether
the L-6c gate needs the `/env` -> envp projection that is the real fix (#140).

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

> **Refinement (L-4).** It needs more than a count. §2.9 measures that eager
> anon has no **per-page ownership** for a count to index — and that every
> writable anon a fork must break is eager. "Re-install writable in place" and
> "install private" are both per-page operations on a Burrow that owns one
> indivisible block. The count was never the hard part; see "As designed at
> L-4" below.

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
empty; the copy is L-3c, and #119 is its hazard (a copy would duplicate
hardware handles, which I-5 forbids). And `RFPROC` alone is **refused**, not
served: without `RFMEM` the child gets a fresh empty address space, so resuming
it at its parent's PC faults on the first instruction fetch. A private child
address space means copy-on-write, which is L-4.

> **Correction (L-3c-1).** This paragraph originally described the empty table
> as "`CLONE_VM` *without* `CLONE_FILES`, which is exactly posix_spawn's shape".
> That was wrong, and §2.4 of this document already said so: Linux's
> `CLONE_VM` without `CLONE_FILES` gives the child a **copied** fd table, which
> is *why* posix_spawn's `dup2`/`close` file_actions do not disturb the parent.
> Empty and copied are not the same shape, and the sentence read as though the
> gap were a deliberate ABI match rather than a deferral. The deferral itself
> was real and correctly flagged; only the justification was fiction. This is
> §2's instruction firing on this document's own newest section — the
> measurement was there, one section away, and the later text drifted off it.

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

#### As built at L-3c-1 — the handle-table copy

`handle_table_copy_into(dst, src)` copies a Proc's handle table into another
**preserving slot indices**, and `rfork_internal` calls it on the fork shape
only. That gate — `if (fc)` — is the chunk's one design decision, and it is
about contracts rather than convenience:

| primitive | descriptors | why |
|---|---|---|
| `SYS_SPAWN_*` | fresh + exactly the `fd_list` endowed | a capability hand-over: the parent states what it means to give |
| `SYS_RFORK` | inherited | the child *is* the parent, continuing on the same frame, so its very next instruction may name any descriptor the parent held |

These are not two settings of one knob; they are what the two calls **mean**.
Every existing `rfork`/`rfork_with_caps` call site (joey plus the five spawn
thunks) passes a kernel `entry` and no `fc`, so gating on the fork shape leaves
the whole spawn family byte-unchanged **by construction** rather than by a flag
a future caller could set wrong. `RFFDG` stays unsupported: under this tree's
polarity it would mean two Procs *sharing* one table, which needs a refcounted
`HandleTable` — an L-3a-style extraction, not this.

**Index preservation is the reason this is not a loop over `handle_dup`.** Dup
installs into the first free slot, so one skipped handle would renumber every
descriptor after it and the child's inherited stdout would land somewhere else
entirely. A skipped slot leaves a **hole**.

**What is skipped, and why a hole rather than a refusal.** The admissibility
test is `handle_slot_may_alias` — the *same* predicate `handle_dup` uses,
extracted rather than rewritten, because both operations create a second handle
naming one object and so face the identical hazard. Hardware (I-5), a `/srv`
connection Spoor (the SO_PEERCRED origin), and a Loom ring (pinned to the table
its registered handles index) do not cross. The fork still succeeds: I-5 is a
property of the *handle* — "pinned to the Proc that created it" — not a property
of forking, so the child simply does not hold what it was never eligible to
hold. Refusing the whole fork would instead punish a parent for holding a
handle it never intended to pass, and would leave a driver unable to create a
process at all. A child needing hardware authority gets it the way every other
Proc does: the warden's confer path (the I-34 allowance).

The hole is observable — the child sees `EBADF` there, and its next `open`
lands at an index Linux's would not — which is the honest report of an
authority it could not inherit.

##### The two coverage layers are blind to each other, in both directions

Demonstrated by three revert probes rather than asserted:

| sabotage | unit suite | `/fork-probe` |
|---|---|---|
| admit every kind (drop both clauses) | **1283/1285 FAIL** — and at *two* assertions, `fork.table_copy` on the hw skip and `devsrv.open_connect_byte` on the `dc='s'` clause, so a fix to one cannot mask the other | unreachable (a failing suite aborts the boot) |
| remove `rfork_internal`'s call | **1285/1285 PASS** — fully green through the bug | **FAIL**, at its own assertion |
| compact instead of preserving the index | **1284/1285 FAIL**, at the *hole* assertion — a different one from the first probe | would pass (no v1.0 parent has a skipped slot below a live one) |

The middle row is the one worth carrying: the kernel test can reach
`handle_table_copy_into` directly but nothing kernel-side can reach
`rfork_internal`'s *call* to it, which sits behind an `RFMEM` gate kproc can
never pass. The rule and the wiring need separate proofs.

#### As built at L-3c-2 — the VFORK suspend

An `RFMEM` fork does not return to the parent until the child has left the
parent's address space. `rfork_internal`'s tail calls `vfork_await_release`,
which parks on `child_waiters` until `vfork_child_released(parent, child)`.

##### Where the request goes — the open question, answered

§9's fourth question asked how VFORK is requested. It is **not requested**: it
follows from `RFMEM`. Three things forced that.

A new bit in the `RF*` word would have been a **category error**. Every flag
there answers *what does the child get?*, and the word's stated polarity is
`set == share`. Suspending the **parent** answers a different question, so a bit
among those would cost the word the one property that makes it readable. Linux
puts `CLONE_VFORK` beside `CLONE_VM` only because its clone word is a grab-bag
with no polarity to protect; Plan 9 has no vfork at all, and its nearest flag —
`RFNOWAIT` — is about the children list, not suspension. Neither heritage
settles it.

Keying on `RFMEM` is **not** an arbitrary coupling of two orthogonal things.
`RFMEM` is exactly the precondition of the hazard: sharing the address space is
the *only* way the child can reach the parent's frame. The condition and the
danger are the same condition.

And the **fail-safe direction** is one-sided. A caller who wanted concurrency
and got a suspend sees its parent block until the child finishes — visible,
terminating, diagnosable. A caller who wanted a suspend and got concurrency sees
memory corruption three layers from its cause. Concurrent shared-memory
execution is anyway already served, and better, by `SYS_THREAD_SPAWN`; when
someone genuinely wants two *Procs* in one space running at once they can ask
for it explicitly, with their own reasoning, exactly as every reserved `RF*` bit
will. The gate is `fc && (flags & RFMEM)` rather than plain `fc`, so L-5's
`RFPROC`-alone `fork()` — where both Procs must run — is correct without a
future edit.

##### The release condition is not a record of the release, it is the release

The obvious design is a flag: set it at fork, clear it at exec and at exit. It
is strictly worse, because it records the release somewhere other than where the
release happens, so a third release path added later silently strands every
vfork parent.

"The child is off my frame" means "the child no longer maps my address space",
and that fact is already written down: `child->as`. At an `RFMEM` fork the two
Procs' pointers are equal by construction; `proc_exec_replace` swaps the child's
to a freshly allocated one; death takes it out of `ALIVE`. Nothing else can
change it — the only other route to a private space is a fork the child cannot
perform (`RFPROC` alone is refused) and an exec the parent cannot perform (it is
parked). So the condition cannot drift from reality, because it *is* reality.

The pointer comparison is sound **only because the parent still holds a
reference** to the shared AddrSpace, so the outgoing object cannot be freed
while the parent is parked and its address cannot be recycled underneath the
comparison. That is a direct dividend of L-3a having moved the VMA drain into
`addrspace_unref`'s last drop; before it, this would have been an ABA waiting to
happen.

##### One new wake, because the other one already existed

Parking on `child_waiters` — the `#344` multi-waiter list `wait_pid_for` uses,
with its register-then-observe discipline verbatim — makes the **death** release
free: `proc_become_zombie_locked` already wakes it. Only the **exec** release
needed a new wake, one line under the same `g_proc_table_lock` section as the
`p->as` swap it reports. It wakes unconditionally rather than testing whether
anyone is suspended, because a spurious wake costs a re-scan whereas a test
would be a second place that has to agree with the park about who is waiting.

A parent killed while parked returns `SLEEP_INTR` and unwinds to its EL0-return
die-check (#811), leaving nothing behind — it registered no state anywhere but
its own stack. A child that loops forever parks its parent forever; that is the
vfork contract, and the parent stays killable.

##### Three coverage layers, each blind to the others

| sabotage | unit suite | `/fork-probe` |
|---|---|---|
| remove `rfork_internal`'s park call | **1286/1286 PASS** — fully green through the bug | **FAIL** at leg I's own assertion ("the parent resumed before the child released"), plus an orphan-adoption line showing the child had not run at all |
| remove `proc_exec_replace`'s wake | **1286/1286 PASS** — green again | **HANG**: the boot stops dead after `exec-probe` and never reaches fork-probe's PASS |
| — | `fork.vfork_release` covers all four release cases deterministically | — |

The second row is worth stating plainly: **a missing exec release is a hang, not
an error.** That is not an artifact of the probe's construction — it is the
bug's real production symptom, since a `posix_spawn` parent whose child execs
would park forever in exactly the same way. If a future boot stops between
`exec-probe` and `fork-probe`, this wake is the first thing to check.

Leg I (death arm) is the wiring proof and is **not** race-free in its failing
direction — on another CPU a no-suspend child could in principle reach `t_exits`
before the parent's `WNOHANG`. The window is a handful of instructions against a
whole context-switch-in, so the failing kernel loses it overwhelmingly, which
the probe above measured rather than assumed. Determinism lives in
`fork.vfork_release`, which drives the predicate directly.

##### What the self-audit found: a dangling `child` past the park

`rfork_internal` ended `ready(ct); … return child->pid;` — dereferencing the
child **with no lock held**, after `ready` has made it runnable. A peer thread
of a multi-threaded parent sitting in `wait_pid_for(-1)` can reap that child:
`wait_pid_for` unlinks under `g_proc_table_lock`, *drops* the lock, then
`proc_free`s. The pointer can be dangling by the time the return reads it.

The window predates this chunk — it has been `ready()` → `return`, a handful of
instructions, since L-3b. What the park does to it is worse than widening:

- it stretches the window from instructions to the child's **entire lifetime**;
- and it **aligns** the two events. The park's release edge is the child's
  death, which is the very edge that makes the child reapable, so the peer's
  wake and ours are the *same* `child_waiters` wake.

Fixed by capturing `child->pid` into a local before `ready(ct)` and never
dereferencing `child` again. The park loop itself needed nothing: it only
touches the child under `g_proc_table_lock`, where being in the list implies not
yet freed, and not-found already counts as released.

Worth recording as a pattern rather than an incident: **a new park inherits
every unsynchronised access that follows it, and converts "narrow" into
"aligned with the wake that ends it."** The audit question is not "is this new
code correct" but "what did putting a sleep here do to the code after it".

##### Leg J

Leg J (exec arm) is the one leg I cannot see, and it covers the only new code:
a kernel that released solely on death passes every other leg here and then
hangs the first real `posix_spawn`. Its discriminator is that the child is
**still alive** when the parent looks — we are executing, so something released
us; `WNOHANG` says the child has not died; the only other release is the exec.
"Still alive" is a fact rather than a race because the exec'd successor blocks
reading a pipe only the parent can write.

#### As designed at L-4 — reach the per-page model the tree already has

User-voted 2026-08-02, with §2.8/§2.9/§2.10's measurements attached. L-4 splits
in two, and **the first half is not COW work at all** — it is the prerequisite
that COW turns out to need.

**L-4a — exec's stack and writable data become `BURROW_TYPE_ANON_LAZY`.**
`exec_map_user_stack` and `map_eager_from_file` (plus the blob path's writable
segments) stop calling `burrow_create_anon` and call `burrow_create_anon_lazy`,
pre-populating only the leading `ceil(filesz / PAGE_SIZE)` slots — the pages
that carry real file bytes. Everything past `filesz` is `.bss` and stays
demand-zero, which is what the lazy arm already does.

Three things fall out, and only the first is what we came for:

1. **Per-page ownership**, so a COW break has something to replace.
2. **#130 closes.** corvus's exec stops allocating and zeroing a 32 MiB
   order-13 block for 128 bytes of `.data`; it allocates one page.
3. **#49 is subsumed.** "Lazy demand-grown EL0 stack (Linux model)" is what a
   lazy stack Burrow *is*.

I-36-4 is untouched, and deliberately: the file bytes are copied in **at exec
time** exactly as today, just per-page instead of into one contiguous kva. No
userspace writable mapping becomes file-backed — that would be a private
file-backed mapping, a different and much larger design.

> **As built (L-4a, landed).** The gate is **not executable**, not *writable* —
> `seg_may_be_sparse()` admits a segment iff `(flags & PF_X) == 0`. That is
> broader than this section predicted, and the reason is one the design pass did
> not have: **the demand-zero fault arm performs no I-cache maintenance.** An
> executable `.bss` tail arriving through it would map executable with a prior
> occupant's lines still in the I-cache — #107's hazard, which the eager paths
> close by syncing the whole span and REVENANT's file-backed arm closes per page.
> Rather than teach a third arm to sync, every executable page stays on a path
> that already does.
>
> The broadening costs nothing and loses nothing. W^X (I-12) makes `PF_W` imply
> `!PF_X`, so **all** writable data — the whole of what COW must break, and where
> #130's 32 MiB lives — is covered either way; the extra reach is the rare
> read-only segment with a bss tail, which is free to make sparse and which no
> fork will ever break. State the predicate as the safety property, because that
> is what it is: nothing executable is ever demand-zeroed.
>
> One consequence worth naming, because a test would otherwise have discovered
> it: **exec-image pages are now on the I-32 page axis.** `burrow_map_in` charges
> only the VMA axis, so eager exec pages were uncharged (the I-32 row calls the
> exec image "one-shot bounded"); the lazy path charges per page, and L-4a's
> pre-populate charges the run it makes resident. So `page_count` now tracks true
> RSS across exec, which is what ARCH §6.5 claims it does — an improvement — but
> it also means **stack growth can fail** where before it could not. It fails the
> way the overcommit model already fails: `proc_fault_terminate`, gracefully, per
> Proc. A stack is 256 pages against `PROC_PAGE_MAX` = 65536, so the practical
> cost is ~0.4% of the cap, and the TCB is exempt.

**What stays eager, and why it is not an inconsistency.** `burrow_create_anon`
remains for DMA buffers and the Weft rings. Those need *physical contiguity* —
a device DMAs into a PA range, and a Weft ring is registered whole — which is a
property of the backing, not of the ownership model. Exec's segments never
needed contiguity; they had it only because that was the one anon constructor
when they were written.

**L-4b — the share count and the break arm.** The count lives beside
`filepages[]` as a per-slot array on the Burrow, allocated at fork; it does
**not** go in `struct page.refcount`, for the three reasons §2.8 measures
(per-block-head, already double-booked, and promotion would still not give
eager anon per-page ownership). The break then reads exactly as §5.4's prose
already describes it, because by then the substrate matches the description.

> **Correction (L-4b, user-voted 2026-08-02).** The count is **per-page**, and it
> lives on `struct page` — a new `cow_share` field taking the existing free
> `_pad`, so `sizeof(struct page)` stays 48 and the per-RAM BSS reservation is
> unchanged. A per-slot array *shared across the sharing Burrows* — the reading
> the paragraph above invites — is not implementable, and the reason is worth
> stating because it is not the obvious one.
>
> **First, each address space needs its own Burrow.** The break has to put the
> private page somewhere. It cannot go in a shared Burrow's slot, because another
> sharer still needs the pristine page there; and it cannot be owned by the PTE
> alone, because nothing in this tree frees user pages from a page table —
> `vma_drain` frees `Vma` structs and drops mapping refs, and
> `proc_pgtable_destroy` frees *table* pages, never leaves. A PTE-owned page would
> simply leak at teardown. So a fork clones the Burrow: same size, its own
> `filepages[]`, the same page pointers. That is Plan 9's `dupseg` exactly.
>
> **Then the count cannot be indexed by slot.** After a break, my slot and the
> page the count describes have diverged, so a later fork bumps an entry that now
> covers two different pages. The take-in-place decision *survives* this — the
> recorded value is the sum over the groups sharing the entry, so it can never
> under-report any one group, and `== 1` still implies a sole holder. The **free**
> decision does not: one group drives the shared entry to zero and frees its page,
> leaving the other group's page with a count of zero, to leak or to underflow.
> Freeing a shared page requires knowing how many holders remain, and that number
> is a property **of the page**. Any scheme that stores it elsewhere must maintain
> a bijection between the slot and the page — which is precisely what a break
> exists to destroy.
>
> The alternative that *preserves* the per-slot array is a **Burrow tree**
> (Zircon's `VmObjectPaged`): the child's Burrow starts empty and inherits from
> the parent's, so nothing is ever conflated. Weighed, and rejected on blast
> radius. It makes a Burrow's `filepages[]` **partial**, and completeness is the
> one assumption all ~20 of its readers in `burrow.c` and `arch/arm64/fault.c`
> rest on — "not in my array" would stop meaning "not resident" and start meaning
> "walk the chain", across the REVENANT fault arm, the read-ahead cluster,
> `decommit`, `resident_count` and the teardown loop, all of them audited on the
> strength of the assumption it removes. It also imports Zircon's hidden-parent
> retention (a dead parent pinning pages its children already broke away from) and
> multi-lock ordering along the chain. Against all that, its only win over the flat
> count is skipping the pointer-array copy at fork — and both designs are equally
> lazy about copying page *contents*, which is the expensive half and the entire
> point of COW.
>
> **§2.8's reasoning is untouched, and still forbids what it forbade.** Its three
> objections are all objections to *reusing* `refcount`: written per-block-head so
> every tail page is stale, already double-booked as SLUB's inuse count, and
> promotion would still not give eager anon per-page ownership. A new field
> maintained only by the Burrow layer has neither of the first two, and the third
> is moot — L-4a delivered exactly that ownership. What §2.8 ruled out turns out
> not to be what L-4b needs.
>
> The obligation the new field inherits is §2.8's own hazard, *"a field whose name
> states a contract nothing keeps"*. So the contract is: `cow_share` is meaningful
> only while the page sits in an anon Burrow's `filepages[]` slot, and it is
> **established, never inherited**, at every site that puts a page into one.
> Measured, that is a closed set of three — `burrow_lazy_populate`
> (`burrow.c:332`), the demand-zero install (`fault.c:523`), and the break itself.
> Every other `filepages[]` writer is `BURROW_TYPE_FILE`: text, shared read-only
> through the Image cache, never broken.
>
> **The lock is global, not per-Burrow.** Two sharers of a page hold *different*
> Burrow locks, so no per-Burrow lock can serialise the decide. `cow.tla` says
> "under the Burrow lock", but its actual requirement is that drop-decide-act be
> **one atomic step** — that is what `BUGGY_BREAK_UNLOCKED` splits — and a global
> COW leaf lock provides it. Plan 9 serialises `Page.ref` under `palloc.lock` for
> the same reason. Held across the decide only, never across the copy or the
> allocation. Hashing it by page index is a recorded seam, to be taken if it ever
> measures.

> **As built (L-4b-2).** Three pieces, in the order the dependency forces:
> `burrow_clone_cow` (dupseg — a separate Burrow, the same page pointers, one
> `cow_page_get` per resident slot), `addrspace_clone` (the address-space half),
> and the break arm in `demand_page_locked`'s `ANON_LAZY` case.
>
> `addrspace_clone` clones every `ANON_LAZY` VMA — **including read-only ones**.
> Sharing those would be safe (a write can never reach them), but cloning keeps
> the *one Burrow, one address space* property uniform, and that property is what
> lets the break's slot swap be serialised by the faulting address space's own
> lock. `FILE` VMAs are shared, not cloned: REVENANT's dispatch gate admits only
> non-writable segments, so there is nothing to break, and sharing is the point of
> the I-36 Image cache. Guard VMAs are reproduced (dropping one silently deletes
> the child's stack guard page). `MMIO`, `DMA` and `SHARED_IN` are refused, and
> the fork fails whole.
>
> **Corrected at L-5 (#136): eager `ANON` splits on WRITABILITY.** This paragraph
> said "everything else is refused", and that refused *every real address space* —
> the vDSO clock page is a single kernel-owned eager-anon page mapped read-only
> into every EL0 Proc, so the first real fork was refused outright. A **writable**
> eager-anon VMA still refuses (one indivisible buddy block, no per-page ownership
> to break); a **read-only** one is shared, on exactly the `FILE` reasoning — no
> prot-mutation syscall exists (I-12), so read-only is permanent and there is
> nothing to break. L-4b's tests could not see this: they BUILD their address
> spaces, so the one VMA every real Proc has was in none of them.
>
> **The parent is modified too.** Its already-installed writable PTEs for every
> COW range are *uninstalled*, so its next touch re-faults and re-installs
> read-only. Leaving them is the I-44 violation: the parent would write through a
> stale writable translation into a page the child now shares. Uninstalling rather
> than write-protecting costs one extra fault per page and needs no new MMU
> primitive.
>
> **The uninstall runs FIRST, before anything is shared (#134).** L-4b-2 ran it
> after the clone, on success only, and justified holding `src->lock` across both
> as closing the window. It does not: the lock only reaches a peer that *faults*,
> and a peer holding an already-installed writable PTE stores in hardware — no
> fault, no kernel entry, no lock. So the child held a share of a page the parent
> could still write, for the duration of the clone. Uninstalling first closes it by
> construction (once the PTE is gone the peer MUST fault, and faulting needs the
> lock), which is also Linux's structure. The **flag** stays in the success-only
> pass: an uninstall is recoverable by re-faulting, `VMA_FLAG_COW` is not, so a
> refused fork still leaves the parent semantically as it was found.
>
> **The break must clear the stale PTE before installing.**
> `mmu_install_user_pte` *refuses* a mismatching install over a valid leaf — it
> returns -1 rather than overwriting (`mmu.c:1649`) — and both break outcomes
> mismatch: the copy path changes the PA, the take-in-place path changes the
> permission bits. Since the read that precedes a write installs read-only, the
> first COW write after a read would otherwise fail its install and kill the Proc.
> The `mmu_uninstall_user_pte` at the top of the write branch is what makes the
> read-then-write sequence work at all, and `cow.break_read_then_write_copies`
> exists to hold it.
>
> **The I-32 charge is taken at the fork, not at the break.** Each address space
> maps the shared page, so each counts it — the Linux RSS reading. That
> deliberately over-counts physical memory between the fork and the break, in the
> safe direction: the fork fails up front, where the failure can be reported,
> instead of the break OOMing later, where there is nowhere good to put it. The
> break itself therefore takes no charge — one mapped page becomes one mapped page.
>
> **The impl realizes the model's `pin` with the retained share**, rather than a
> second counter: the breaker holds its own share across the alloc and the copy and
> calls `cow_page_put` only once the copy is done. That is strictly stronger than
> the model's separate `pin` (a held share also keeps the count off zero), so the
> implementation refines `cow.tla` rather than deviating from it.
>
> **`VMA_FLAG_COW` is never cleared.** The flag is routing; the per-page count is
> the truth. A VMA whose pages have all been taken in place costs one extra fault
> per page, where clearing it would need a scan proving no page in the range is
> still shared.

**Ordering inside the chunk is forced**: L-4b cannot be written against a
substrate L-4a has not yet produced. The model comes before both.

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

> **LANDED at L-4, model-first, before any implementation**: `specs/cow.tla`
> with all three buggy cfgs, plus the clean cfg TLC-green at **580 distinct
> states, depth 13** (3 sharers) on `Safety` + the `EventuallyReleased`
> liveness witness. Each buggy cfg fails at its OWN named invariant, verified
> from the traces rather than the labels:
>
> | cfg | mechanism | violates |
> |---|---|---|
> | `cow_buggy_break` | drop/decide not atomic -> two sharers both read zero, both take the page in place | `NoAliasedWritable` |
> | `cow_buggy_teardown` | share dropped before the copy, no pin -> a concurrent exit frees it mid-copy | `NoUseAfterFree` |
> | `cow_buggy_vfork` | check-then-park outside the lock -> the release in the window is lost | `EventuallyReleased` (Safety HOLDS -- it is a hang) |
>
> The third is the L-3c-2 suspend modeled **retroactively** (the
> `death_wake.tla` precedent). That it violates only liveness is the point: a
> lost vfork wake corrupts nothing, which is exactly why an invariant cannot
> witness it. The action-to-site obligations are in
> `specs/SPEC-TO-CODE.md::cow.tla`.

### 5.5 Stage 4 — reaping (`wait4`)

Creating a process is only half a process model; a parent that cannot reap
accumulates zombies and a shell cannot tell a finished job from a running one.
`wait4` is the other half, and it is the last row `/bin/sh` needs.

#### As built at L-6b — the map is the work

`wait_pid_for` (PTY-1e) is **already a POSIX `waitpid`**: the pid selectors are
Linux's (`-1` any, `>0` that child, `0` the caller's group, `<-1` the group
`-pid`), it has the non-blocking flag, and it has the stop/continue reports. So
this row builds no machinery. What it builds is a **map** — and the map is the
work, because the option words look interchangeable and are not.

**The collision.** Measured from `third_party/musl/include/sys/wait.h`:

| flag | Linux | Thylacine |
|---|---|---|
| `WNOHANG` | 1 | 1 |
| `WUNTRACED` / `WSTOPPED` | 2 | 2 |
| `WCONTINUED` | 8 | 4 |
| `WEXITED` (waitid's) | 4 | — |

The first two agree **by coincidence**. The third does not, and the gap it
leaves is **occupied**: Linux's `WEXITED` is numerically Thylacine's
`WAIT_CONTINUED`. So a passthrough is wrong in both directions at once — a
guest asking for `WCONTINUED` sets a bit the native handler rejects as unknown,
and a guest passing `WEXITED` silently opts into continue-reports *and* into the
packed status encoding. Neither is a decline; both are answers that look
plausible. That is why the row is a translator and why the admitted set is an
allow-list.

**The status encoding is already Linux's** — PTY-1e built `WAIT_STATUS_*` as
"the Linux wait(2) layout so the Pouch boundary-line maps 1:1", and it checks
out against musl's own accessors. But the kernel applies it **conditionally**:
`wait_pid_for` packs only when a PTY-1e flag was passed and returns the RAW exit
status otherwise, for pre-PTY callers. Linux always wants packed. So the
translator packs exactly when the kernel did not — and it cannot decide that by
inspecting the returned value, because a raw exit status of 5247 and a packed
`WAIT_STATUS_STOPPED` are both `0x147f`. It has to know what it **asked** for,
which is why the pack decision is derived from the flag word one line after it
is built and before the call is made.

**The pure layer hands back a description, not a flag word.** The obvious shape
would be to return the native `WAIT_*` word directly, which would drag `proc.h`
into `vivarium.c` — the same import the clone row already refused for
`RFPROC`/`RFMEM`. The split also lands the risk in the right half: the dangerous
direction is Linux bit 4 silently becoming `WAIT_CONTINUED`, and that is decided
in the pure layer, by an allow-list a unit test pins with no kernel plumbing at
all.

**`-1` becomes `-ECHILD`, and it covers two conditions.** `wait_pid_for` answers
`-1` both for "no matching child" and for a #811 death-interrupted sleep. Mapping
both to `ECHILD` is exact rather than lossy: the death path returns through the
sync-from-EL0 tail, where `el0_return_die_check` is **noreturn** on the die
branch (`vectors.S`), so a group-terminating Thread never carries a value back
to EL0. There is no observer that could tell the two apart. `T_E_CHILD` (10) was
appended under signoff for this line — a bare `-1` would reach a Linux libc as
`EPERM`, the #100 class of wrong answer, and ECHILD is the *termination
condition of every reap loop*, so a near-miss is not serviceable.

**`rusage` declines when non-NULL.** Filling it would mean inventing figures the
kernel does not collect per child; zeroing it would be a stored lie about a child
that used no CPU. musl's `waitpid` and `wait` pass a literal 0
(`src/process/waitpid.c`), so the shell path and every ordinary reap are
unaffected. The prowl arc's per-Proc `run_ns` is the substrate a future row would
use.

**What the in-guest legs finally discharge.** L-6a's fork leg carried a stated
gap: with a private address space the child had no channel back, so "the child
RAN" was unassertable until something could reap. L-6b reaps both children the
earlier legs created, which turns that into an assertion (L170) and adds the
**COW-privacy** leg (L170c) — the child writes a witness before exiting, the reap
orders that write before the parent's read, and the parent's copy must be
untouched.

**Not proven in-guest, and named rather than left silent**: the `WNOHANG`
"alive but nothing to report" return of 0. It needs a child reliably
alive-and-not-yet-exited at a chosen instant, which needs a synchronisation
channel this phenotype does not have (`pipe2` is not a row, and a private address
space rules out shared memory). Timing a loop would be a flake in a boot-fatal
probe. L-6c's shell exercises it naturally.

---

## 7. The build arc

Each row is a chunk with its own gate. Audit rounds where marked; the whole
arc is audit-bearing, so the trigger table in `ARCHITECTURE.md` §25.4 and
`CLAUDE.md` gains a row at L-1.

| Chunk | Scope | Gate |
|---|---|---|
| **L-0** | This document + the ARCH §25.4 row + the §28 invariant number + `VIVARIUM.md` §9/§10 updates. **Scripture only, no code.** | user signoff |
| **L-1** ✅ **LANDED** | `struct AddrSpace` extraction. Pure refactor: `ref` is 1 everywhere, nothing shares yet. **Seven** fields moved; `struct Proc` 408 → 376 B; 239 compiler-enumerated conversion sites across 22 files; 23 offset asserts re-baselined. As-built: `docs/reference/146-addrspace.md`. | **MET** — 1272/1272 → 1276/1276 (the four new `addrspace.*`), boot OK, 0 EXTINCTION, `asid.tla` clean (443457 states; the depth figure TLC reports varies with worker count and is not a fingerprint), SMP gate; the nine Proc-field predicates converted as a set (the four `mmu.c` parameter checks correctly did not) |
| **L-2a** ✅ **LANDED** | `execve` (stage 1) for a **single-threaded** Proc: the AddrSpace-targeted load path (`exec_load_into` + the `*_in` VMA/burrow forms), `proc_exec_replace`, `sched_activate_addrspace`, `SYS_EXECVE = 101`. A multi-threaded caller is refused with `-EAGAIN`. | **MET** — `/exec-probe` re-execs itself and the successor prints from an fd inherited across the swap; a **failed** execve returns with the caller's address space intact; all three load-bearing pieces revert-probed to *distinct* failures |
| **L-2b** | The de_thread primitive: a per-Thread die flag + wait-for-EXITING + reap, removing L-2a's multi-thread refusal. | a multi-thread Proc's peers are all EXITING and off-CPU before the swap; own audit round |
| **L-3a** ✅ **LANDED** | The share substrate: the VMA drain moves into `addrspace_unref`'s last drop (fixing the same latent bug in `proc_free` *and* `proc_exec_replace`), `proc_alloc_in(share)`, `rfork_internal` accepts `RFPROC\|RFMEM`, the device quiesce gates on sole ownership, `RFMEM`-without-a-space refused. No EL0 surface. | **MET** — 1279/1279 → 1282/1282, boot OK, 0 EXTINCTION, `asid.tla` unperturbed (443457, depth 18); all three pieces revert-probed to *distinct* failures |
| **L-3b** ✅ **LANDED** | Child-context restoration (pulled forward from L-5 per §5.4's correction) **+ the `SYS_RFORK = 102` EL0 surface**, which had to come with it: `fork_frame_init`, `thread_create_forked`, `thread_fork_trampoline`, `rfork_forked`, and `libthyla_rs::rfork_spawn` (the musl-`clone.s` shim the different-stack return demands). The handle-table copy moved OUT to L-3c — it is orthogonal, and #119 is a decision that deserves its own reasoning. | **MET, but not by the gate as written** — that gate said *kernel-driven*, and measuring showed a kernel test cannot observe an eret to EL0 at all (kproc has neither an address space nor a frame). Replaced by `/fork-probe`: six legs, boot-fatal. Revert-probed to the sharpest available pairing — ignoring `child_sp` leaves the **unit suite fully green** while only the in-guest leg fails |
| **L-3c-1** ✅ **LANDED** | The handle-table copy (§2.4; #119). `handle_table_copy_into`, index-preserving, gated on the fork shape so the spawn family is byte-unchanged by construction. **Skip, not refuse** — the choice is stated at §5.4's as-built section, and the admissibility test is the SAME predicate `handle_dup` uses, extracted so the two cannot drift. | **MET** — 1284 → 1285/1285, boot OK, `/fork-probe` leg H (a child writes to a pipe fd only the parent opened). Three revert probes, each failing at its own assertion; removing the wiring leaves the **unit suite fully green** |
| **L-3c-2** ✅ **LANDED** | The VFORK suspend (#122). `vfork_await_release` parks the parent on `child_waiters` until `vfork_child_released` — which reads `child->as`, so the condition IS the release rather than a record of it. Keyed on `RFMEM`, not a new flag: §9's fourth question, answered at §5.3's as-built section. One new wake (exec); death's already existed. | **MET** — 1285 → 1286/1286, boot OK, `/fork-probe` legs I (death arm) + J (exec arm, the successor blocks on a pipe so "still alive" is a fact not a race). Two revert probes: removing the park leaves the **unit suite fully green** while only the in-guest leg fails; removing the exec wake leaves it green *and* hangs the boot — the honest production symptom |
| **L-3d** ✅ **LANDED** | The VIVARIUM `clone` row. `vivarium_clone_decide` (pure) + one `VIV_TIER2` entry + a shell that calls the SAME `sys_rfork_core` the native handler calls (extracted here, the V-8 `sys_fstat_for_proc` discipline). `CLONE_VM` without `CLONE_VFORK` is **refused**, not served — §8's as-built section carries why L-3c-2's fail-safe reasoning inverts for a stock Linux caller. Also replaced the four stale copies of the "native ceiling" literal with `VIV_NATIVE_CEILING` + a `_Static_assert`. | **MET, but not by the gate as written** — that gate named `posix_spawn`, which needs `execve` (221) and `wait4` (260) as well; both are L-6's, are real translators, and are dependencies of *posix_spawn* rather than of *the clone translation*. Replaced by: a clone whose child runs, writes into the shared address space, and exits, with the parent suspended until it does. 1286 → 1287/1287; `viv-pheno-probe` legs L155–L163, boot-fatal |
| **L-4a** ✅ | **Prerequisite, not COW work**: exec's stack + every NON-EXECUTABLE segment move from `burrow_create_anon` to `burrow_create_anon_lazy`, pre-populating only the `ceil(filesz/PAGE_SIZE)` leading slots (the stack: only the run its argv/auxv frame occupies at the top). Gives the per-page ownership §2.9 measures COW needs; closes **#130**; subsumes **#49**. Executable segments stay eager — the I-cache reason above. DMA + Weft rings stay eager too (they need physical contiguity). | **LANDED**: `exec.writable_segment_is_sparse` (4 MiB memsz, 64 B filesz -> 1024 slots reserved, 1 resident) + `exec.stack_is_sparse` (1 MiB reserved, 1 resident), both revert-probed; 1289/1289 + boot OK |
| **L-4b** *(landed: b-1 substrate, b-2 clone + break)* | Per-page share counts + the COW break arm (stage 3a). The count is a **new** `struct page.cow_share` (taking the free `_pad`, so `sizeof` stays 48) — **not** the double-booked `refcount` (§2.8), and **not** a per-slot array, which §5.4's L-4b correction measures to be unimplementable: after a break the slot and its counted page diverge, and the free decision corrupts. A fork clones the Burrow per address space (Plan 9 `dupseg`); the break decides under a **global** COW leaf lock, because two sharers hold different Burrow locks. | the model (§6) TLC-green **first**; then break-vs-break under `-smp 8` |
| **L-5** ✅ **LANDED** | Stock `fork()`. Two mechanical changes — `rfork_internal` clones for the fork shape (`fc && !RFMEM`), and `sys_rfork_core` admits `RFPROC` alone with `child_sp == 0` meaning INHERIT (the SP rules are RFMEM's, not the fork's). **Plus the two defects making it live exposed**, neither of which L-4b's tests could see: **#136** — `addrspace_clone` refused every real address space (the vDSO is read-only eager anon, and the tests build their own spaces); **#137** — `ESR_ISS_WNR_BIT` was 9, not 6, so `fi->is_write` had been ALWAYS FALSE tree-wide, leaving the COW write-break unreachable (an infinite fault loop) and the write-permission gate inert. | **MET, and not by the gate as written** — "in a vivarium" needs L-6's `execve`/`wait4` rows; the fork itself is proven natively instead. `/fork-probe` legs K (three separately-falsifiable COW claims, both Procs running) + L (a store to read-only text is DENIED — the half of #137 leg K does not cover). Five independent revert probes, the sharpest being: **drop the clone wiring and the unit suite stays at a full 1300/1300 while only the in-guest leg fails** |
| **L-6a** ✅ **LANDED** | The VIVARIUM `execve` row + the `clone` **fork** shape. `sys_execve_core` extracted so the native front end keeps its user-VA copy-in while the phenotype walks a `char *const argv[]` into the blob (the V-8 `sys_fstat_for_proc` discipline), and `vivarium_clone_decide` gains `VIV_CLONE_FLAGS_FORK` — the half L-3d refused because copy-on-write did not exist, which L-4/L-5 discharged. **envp declines when non-empty**: §5.2's as-built section measures that `exec_build_init_stack` writes a lone NULL for envp in BOTH frame shapes, so no Thylacine process has ever had a POSIX environment on its stack (#140) and the effect cannot be reproduced at any layer. | **MET** — 1300/1300 (the assertions land inside `vivarium.clone_domain`), boot OK, 0 EXTINCTION; `viv-pheno-probe` L156/L156b (a real fork returning twice, and the fork word still exact) + L164–L169 (the whole argv walk exercised by a *failing* execve, where ENOENT-not-EINVAL is a positive statement that the blob passed the core's self-check). Two revert probes, complementary rather than redundant: dropping the **fork admission** fails the unit test at its own assertion, while disabling the **envp gate** leaves the unit suite at a full **1300/1300** and fails only in-guest at L166 |
| **L-6b** ✅ **LANDED** | The VIVARIUM `wait4` row. A genuine flag map, not a passthrough: Linux `WNOHANG`/`WUNTRACED`/`WCONTINUED` are 1/2/8 against Thylacine's 1/2/4, and Linux's `WEXITED` (4) collides with `WAIT_CONTINUED` — so a passthrough is wrong in BOTH directions at once. `wait_pid_for` is already a POSIX `waitpid` with the Linux packed status layout (PTY-1e), so the work is the map, the pack-when-the-kernel-did-not rule, and `-ECHILD` (`T_E_CHILD` = 10, appended under signoff). The pure decide hands back a DESCRIPTION rather than a native flag word, keeping `proc.h` out of the pure layer exactly as the clone row keeps `RFPROC`/`RFMEM` out. | **MET** — 1301/1301, boot OK, 0 EXTINCTION; `vivarium.wait4_domain` pins the collision against the REAL `WAIT_*` constants, and `viv-pheno-probe` L170–L176 reaps both children the earlier legs created — which is what finally discharges L-6a's stated gap (the fork child RAN) and adds the COW-privacy leg (L170c) the private address space had made unobservable. Two revert probes, opposite directions: dropping the **allow-list** fails the unit test at its own assertion and never reaches the guest, while removing the **conditional pack** leaves the unit suite at a full **1301/1301** and fails only in-guest at L173 |
| **L-6c** *(in progress -- gate WIRED, KNOWN-BLOCKED)* | The arc gate, built and running every boot. The fixture is a real Alpine minirootfs whose `/bin/sh` is Alpine's own busybox, run as a declared-`linux` container through a 9-leg script (shell-runs, external exec, both status directions, pipeline, substitution, loop, nested shell, `$?`). It SOFT-SKIPS without the two external fixture inputs, so the default build stays hermetic. Blockers measured rather than assumed, and the gate has already moved once. **#149 (FIXED here)** exec refused a non-page-aligned `PT_LOAD` vaddr and every real-world binary has one (busybox's data segment is at `0x51d2e0`; everything OUR toolchain emits is aligned, which is why the precondition was universally true until the first foreign ELF) -- it failed SILENTLY, because the reject ran in the child thunk after the pid was returned, so three instruments had to come back EMPTY before the shape of the evidence (a process making no syscalls at all cannot have run) pointed at the loader. With it fixed **busybox RUNS**: zero syscalls before, 13 distinct ones after, through musl startup into its own logic. **#150 (FIXED here)** landed the whole startup batch -- the set busybox issues between `_start` and its first useful instruction, MEASURED off the running guest rather than guessed: `writev` was why nothing printed (busybox's `echo` writes through it), and `getcwd`/`uname`/`getpid`/`getppid`/`getuid`/`getgid`/`set_tid_address`/`setuid`/`setgid` came with it. Unserved numbers went **13 -> 3**, and busybox now RUNS *and SPEAKS*: its own error message reaches the console, which is the proof `writev` works. **#151** is what blocks it now, and it is a different KIND of thing -- instrumenting the last row showed busybox issues `fcntl` exactly twice, `F_SETFD(FD_CLOEXEC)` and `F_DUPFD_CLOEXEC(10)` (ash's `savefd`), and Thylacine has no close-on-exec at all: no bit in the handle table, and `proc_exec_replace` never touches it, so exec preserves EVERY fd. Serving the dup while dropping the flag would pass the gate and leave a known leak in the tree, so it waits for the kernel feature. That measurement also VOIDED A STATED SCRIPTURE FACT: VIVARIUM.md justified ignoring `O_CLOEXEC` because "there is nothing to opt out of" under spawn's explicit fd list -- true when written, and voided by **this arc**, since L-2a's `execve` preserves the table and L-3c-1's `rfork` copies it. Neither commit had any reason to look at an openat flag table; a premise a later commit voids is invisible to the round that approved it. Still standing: **#145** every stock Alpine ELF is `ET_DYN` PIE with `PT_INTERP` (zero static binaries in the image), worked around with Alpine's own `busybox-static`; **#146** `/bin/sh` is one of 335 symlinks and the resolver has no symlink handling. Also FIXED here: **#148** `fstat` on a pipe returned -1 (devpipe had no `.stat_native`), which made `viv` spawn the container fd-less and report a healthy shell as one that never ran. | **an Alpine `/bin/sh` runs a command** -- not met; the gate reports how far it gets every boot, which is how #149's fix was SEEN to move it, and flips to boot-fatal when the last blocker lands (`L6C_GATE_FATAL`) |
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

#### As built at L-3d — the `clone` row

The row itself is small: `vivarium_clone_decide` (pure, in `kernel/vivarium.c`),
one `VIV_TIER2` table entry, and a shell in `viv_tier2` that calls the same
`sys_rfork_core` the native handler calls. Everything worth recording is in what
the domain refuses and why.

**The mapping is a constant, so the decide function decides only the domain.**

```
clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack, ptid, tls, ctid)
    ->  SYS_RFORK(RFPROC|RFMEM, stack, 0)
```

**The garbage-register hazard, which is this row's real content.** arm64 selects
`CONFIG_CLONE_BACKWARDS`, so the order is `flags, stack, parent_tid, tls,
child_tid` — `tls` *before* `child_tid`, not the x86-64 order most people
remember. But the sharper fact is that on the only call that matters, **x2, x3
and x4 hold garbage**: `posix_spawn` invokes `__clone(child, stack, flags, arg)`
with four arguments (`posix_spawn.c:198`), and musl's `clone.s` then executes
`mov x2,x4 / mov x3,x5 / mov x4,x6`, moving three registers the caller never
set. Linux tolerates it because `CLONE_PARENT_SETTID`, `CLONE_SETTLS` and
`CLONE_CHILD_SETTID` are all clear, so its kernel never reads them.

A translator that reached for `args[3]` as the child's TLS would therefore hand
the child an uninitialised register as its `TPIDR_EL0`, and the child would
fault or corrupt at its first thread-local access — at a site with no visible
connection to the clone. So the shell reads **only** `args[0]` and `args[1]`,
passes a literal `0` for `child_tls` (`SYS_RFORK`'s inherit sentinel, which is
what a vfork child needs), and the domain's exclusion of `CLONE_SETTLS` is what
makes that correct rather than merely lucky.

This is the *inverse* of the arity property ARCH §25.4 states for T1 rows. There
the risk is a native target reading more argument words than the Linux call
supplies; here the words are supplied and meaningless. Both reduce to one rule:
read a register only when the call's own contract says it holds something.

**The flags comparison is full 64-bit width, and the self-audit is what found
that.** The first draft narrowed to `(u32)` by copying `vivarium_mmap_decide`'s
shape — but mmap and openat narrow because *their* Linux parameters are `int`,
which is the ABI. `clone`'s `flags` is an `unsigned long`, so narrowing there is
an assumption about Linux's own source rather than about its ABI, and that source
is not in this tree. Under that uncertainty the stricter reading is correct:
declining is always safe, admitting an unreasoned bit is exactly the failure this
tier exists to prevent, and the cost is nil because musl's `clone.s` zero-extends
(`uxtw x0,w2`, verified in the built object). Worth generalizing: **a shape copied
from a sibling carries that sibling's justification, which may not survive the
copy** — re-derive it rather than inherit it.

**`CLONE_VM` without `CLONE_VFORK` is REFUSED, and this is the one place L-3c-2's
reasoning does not carry over.** L-3c-2 keyed the suspend on `RFMEM` rather than
on a flag of its own, arguing that an unwanted suspend blocks visibly while an
unwanted concurrency corrupts silently — a one-sided fail-safe. That holds for a
*native* caller, who reaches `SYS_RFORK` through a Thylacine ABI whose only
shape is the vfork one.

It does not hold here. A stock Linux binary that sets `CLONE_VM` and clears
`CLONE_VFORK` has said, in the only vocabulary it has, **"do not suspend me"** —
and serving it with a suspend converts a working program into a deadlock the
moment its child neither execs nor exits promptly. That is not conservative; it
is a hang with our name on it. So the domain is an exact equality, the caller
gets an honest decline, and the genuinely concurrent shape keeps the target it
already has (`CLONE_THREAD` onto `SYS_THREAD_SPAWN`, whenever that row is
written).

**A zero `stack` declines**, which is what keeps `vfork()` proper out of scope
(§9's fourth question, second half). Linux reads `stack == 0` under `CLONE_VM`
as "share the parent's stack", safe there only because `CLONE_VFORK` suspends
the parent so the two never push concurrently. `SYS_RFORK` refuses a zero
`child_sp` by contract, and weakening a landed kernel gate to widen a phenotype
row would be the wrong direction of change.

**What this row makes reachable and does NOT fix (task #127).**
`rfork_internal` copies `phenotype` to the child but not `sigtab`, and the
comment there — written at #102 F7 — argued the gap was *unreachable*: "no
clone/fork/execve number is a table row, so a `PHENO_LINUX` Proc cannot create
another Proc at all." **L-3d makes clone a table row, so that sentence is now
false**, and the seam is live: `child->sigtab` stays NULL, which reads as
all-`SIG_DFL`, while POSIX `fork(2)` inherits both caught handlers and `SIG_IGN`.

It is not fixed here, and the comment itself says why: `execve(2)` needs the
*opposite* rule (reset caught dispositions to `SIG_DFL`, preserve ignored ones),
so this is two behaviours and a design decision rather than a copy — and the
sigtab sits on the V-6 audit surface. The v1.0 exposure is also narrow: the only
admitted clone shape is vfork-then-exec, and musl's `posix_spawn` child resets
its own dispositions before exec'ing. It belongs with `execve` and `wait4` at
L-6. The comment was corrected in this chunk to say *reachable but unfixed*
rather than *unreachable*, because a stale unreachability claim is exactly what
stops the next reader from asking.

**The gate is not the one this row was given, and measuring is what showed it.**
§7 said *a stock `posix_spawn` binary runs in a vivarium*. Reading what
`posix_spawn` actually drives shows the child needs `execve` (221) and the
parent usually `wait4` (260) — neither of which is a table row, and neither of
which is a *dependency of the clone translation*. They are dependencies of
`posix_spawn`, which is L-6's deliverable; each is a real translator with its own
reasoning (execve must walk a `char *[]` and repack it into `SYS_EXECVE`'s
concatenated blob), and pulling both here would make L-3d into L-6 rather than
complete L-3d. So the gate became: **a clone whose child runs, writes into the
shared address space, and exits — with the parent suspended until it does.**
This is the same correction L-3b's gate needed, for the same reason: a gate
written before the work was measured named a consumer instead of the property.

**What the in-guest legs prove, and what the unit tests cannot.** The eight
`viv-pheno-probe` legs (L155–L163) go through `__viv_clone`, a transliteration
of musl's `clone.s` — necessary because the child returns on a *different stack*
while holding the parent's `x29`/`x30`, so no `asm!` wrapper can be safe. It
diverges from musl in exactly one way: it loads **recognisable poison** into
x2/x3/x4 rather than leaving them uninitialised. "Uninitialised" is the real
hazard but is not a value a test can assert against; poisoning makes it
deterministic, so a translator that ever read x3 produces a child whose
`TPIDR_EL0` is `0xBAD3` and L162 says so precisely.

L161 is the suspend: the child publishes its token *before* exiting, and the
parent reads it with no wait at all. That leg also proves `CLONE_VM` delivered —
the token lives in the parent's own `.bss`, and the child wrote it.

**L161 is not claimed as independently revert-probed, and the reason is worth
recording.** A third probe disabling the park does fail — at `/fork-probe` leg I,
the *native* leg, which runs earlier in the boot, so the container never starts.
The park is therefore proven load-bearing on exactly the path this row uses (the
shell reaches `rfork_internal` through the same `sys_rfork_core` the native
handler does); what L161 adds is that the property survives *the translation*.
Isolating it would need a probe that disables the park only for a phenotyped
caller — testing a configuration the system never runs.

**And the first two runs of the L162 probe were a FALSE GREEN, which is the
durable lesson of this chunk.** `viv-pheno-probe`'s *containered* copy lives in
the POOL (`/vivarium`, baked by `populate_stratum_pool`), and
`THYLACINE_MKFS_PRESERVE=1` skips that populate — so the production run and the
revert probe both executed a binary that predated the new legs entirely, while
`build/ramfs-src/viv-pheno-probe` disassembled correctly and the boot printed
`phenotype ... PASS`. The ramfs copy is fresh and only the container's is stale,
which is exactly what makes it invisible: every artifact you would naturally
inspect is right. One `PRESERVE=0` build is the discriminator, and it is now a
precondition for any chunk that adds viv / diorama / alpine-bundle legs
(task #126).

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
3. ~~**`struct page.refcount`**: promote it to a real share count, or carry a
   separate COW structure?~~ — **resolved at L-4: carry a separate structure,
   and the question was downstream of a bigger one.** Measuring at L-4 (as this
   entry asked) falsified both of §2.8's stated facts *and* found that the
   counter was never the obstacle: eager anon has no per-page ownership for any
   count to index (§2.9), and every writable anon a fork must break is eager.
   Recorded here rather than silently superseded, because the entry's own
   instruction — measure it at L-4 rather than decide it here — is what
   produced the correction.
4. ~~**How is VFORK requested?**~~ — **resolved at L-3c-2: it is not.** The
   suspend follows from `RFMEM`, because that flag is exactly the precondition
   of the hazard, and because a bit in the `RF*` word would answer a different
   question than every other bit there (§5.3's as-built section carries the
   full reasoning, including why the fail-safe direction is one-sided).
   Recorded here because a future reader will reasonably expect a flag and
   should find out at once that its absence is deliberate.

   What remains open is the narrower original question: **is `vfork()` itself
   in scope**, or only `posix_spawn`'s use of `CLONE_VFORK`? Stock musl has
   `vfork.c`, but almost nothing calls it directly.

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
