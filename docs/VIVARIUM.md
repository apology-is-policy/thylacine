# VIVARIUM — running unmodified Linux binaries on Thylacine

**STATUS**: DESIGN (2026-07-23). Scripture-first per CLAUDE.md's design-conversation
pattern: this document lands BEFORE any code. Two architectural forks (§4, §12) are
OPEN and need the user's vote; everything else is settled by the research below.

Phase 8's fourth pole (`ROADMAP §9`, `docs/phase8-status.md`: *"the network arc →
container runner (#70) → on-system toolchain (#67) → **Linux binary shim**"*). The
network arc shipped; the toolchain is the main track's Clade arc; this document is
the remaining two, which are one design.

---

## 0. Thesis

> A Linux program should run on Thylacine because its **territory** gives it a
> Linux-shaped world and its **phenotype** gives it a Linux-shaped ABI — not
> because the kernel became Linux.

Both halves are per-Proc, capability-bounded, and revocable. Neither is a global
kernel mode. That is the whole design, and it is the part that is genuinely ours
rather than borrowed (§3.4).

**Vivarium** is the umbrella name for the subsystem (§11): an enclosure that
maintains an organism in conditions simulating its natural environment.

---

## 1. Ground truth — what the tree actually has (the deep review)

Verified against the tree at `367f082d`, not from memory.

### 1.1 What already works FOR us

| Asset | Where | Why it matters here |
|---|---|---|
| **Per-Proc namespace** (Territory) | `kernel/territory.c`, I-1/I-28 | *Containers are territories.* The isolation primitive already exists and is audited. No cgroups/seccomp needed at v1.0 — the namespace **is** the boundary. |
| **Namespace exec** (#58) | `kernel/syscall.c::exec_resolve_from_namespace` | The hard prerequisite is **already landed**: spawn resolves binaries through stalk, retiring the flat `devramfs_lookup`. Without it a container's binaries were categorically unexecutable. |
| **REVENANT** file-backed demand-paged exec | I-36, `kernel/exec.c` | Loads a large ELF off the FS with demand paging. A Linux binary is just an ELF; the loader path is built and audited. |
| **The top-50 semantics already exist natively** | `kernel/syscall.c` | `open/read/write/pread/pwrite/lseek/mmap(lazy)/munmap/close/dup/pipe/clone/exit_group/wait_pid/futex(torpor)/poll/getdents(readdir)/stat/chdir/getcwd/getrandom/clock_gettime/nanosleep/rename/unlink/mkdir/fsync/…` — Pouch **proves** these carry a real musl. The binary shim is largely **renumbering + argument-shape translation**, NOT new semantics. This is the single biggest reason the effort is tractable. |
| **Loom** | I-29/I-30, `kernel/loom.c` | An audited async submission/completion ring — exactly the substrate a userspace-personality design needs (§4 Option B). |
| **The 9P/`/net` stack** | netd, `NET-DESIGN.md` | The network exists and is reachable as files. |
| **A per-Proc resource floor** | I-32, I-34 | A container already has bounded pages/threads/children/hardware. |

### 1.2 Finding R-1 — the syscall number spaces **collide** (a scripture defect)

`ARCH §11.6` states:

> *"The kernel uses a monotonic syscall number territory; Linux syscall numbers
> don't overlap (kernel has its own numbering). The Linux shim has a separate entry
> path that decodes Linux numbers and dispatches."*

**The "don't overlap" clause is false as-built.** Thylacine's numbers run `0..100`
(`SYS_WEFT_UNSHARE = 100`, `SYS_DMA_CREATE_WEAVE = 99`, `SYS_TTY_CONT = 98`); Linux
aarch64's run `0..~460` (`read=63`, `write=64`, `openat=56`, `fsync=82`…). They
occupy the same low range and collide densely.

Consequence — and this is the load-bearing architectural fact: **`svc #0` alone
cannot tell the kernel which ABI the caller speaks.** A Linux binary emits `svc #0`
with `x8=64` meaning `write`; a native Proc emits `svc #0` with `x8=64` meaning
whatever Thylacine's #64 is. Since we cannot rewrite arbitrary binaries and cannot
require a different `svc` immediate, the ABI must be **per-Proc state consulted at
the syscall entry** — i.e. a *personality* in the FreeBSD/illumos sense. That is
forced, not chosen. §11.6's "separate entry path" is therefore correct in spirit
but under-specified: it must say *how* the entry path knows. **§11.6 gets corrected
when this design lands.**

### 1.3 Finding R-2 — the socket gap (a real tension with NOVEL #1)

`ARCH §11.5` commits, deliberately and proudly:

> *"**Network sockets are intentionally absent from the kernel syscall surface.**
> … Linux/pouch binaries reach them through a pouch boundary-line that translates
> each to `/net/tcp/clone` file operations."*

That works because Pouch is a **compile-time** seam — we patch musl. **An unmodified
Linux binary has no pouch libc.** It issues `socket(2)`/`connect(2)`/`sendto(2)` as
raw syscalls. So either:

- the Vivarium provides the socket family by translating to `/net` file ops **inside
  the personality layer**, or
- **no Linux binary can use the network** — which fails ROADMAP §9.2's own exit
  criteria (`curl` fetching a URL is the headline).

This is not a contradiction of NOVEL #1 so much as a needed refinement: *the native
kernel surface stays socket-free; the Linux **phenotype** supplies BSD sockets as a
translation, in exactly one place.* Where that place lives is the §4 fork. The
NOVEL #1 claim must be restated as **"zero socket syscalls natively"** rather than
"zero socket syscalls," and `ARCH §11.5` gains the same clause.

### 1.4 Finding R-3 — signals are the genuine hard part

Ground truth: `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch` rewrites
`__NR_rt_sigaction` and `__NR_rt_sigreturn` to `0xFFFF` — the ENOSYS stub. There is
**no POSIX signal machinery in the kernel at all**; Thylacine has Plan 9 *notes*
(I-19), and the pouch boundary maps a small subset (`snare:*`, `interrupt`,
`tty:*`).

An unmodified Linux binary expects real `rt_sigaction`/`rt_sigprocmask`/
`rt_sigreturn` with a **kernel-constructed signal frame on the user stack**
(`ucontext`/`sigcontext`, correct `pstate`/`pc`/`sp`/`x0-x30`, the restorer
trampoline, and `rt_sigreturn` restoring it atomically). That is real, careful,
arch-specific kernel work — the same piece every peer system (Linuxulator, LX,
gVisor, Starnix) calls its hardest. It is the single largest line item in §10, and
it is **on the death/notes lineage** (#809/#811/LS-5), so it is audit-bearing.

Mitigation that makes v1.0 tractable: the target is **static musl binaries**, whose
signal use in practice is narrow (`SIGPIPE` ignore, `SIGCHLD`, `SIGINT`/`SIGTERM`
default-terminate, occasional `SIGALRM`). A **fidelity ladder** (§9) is honest here
rather than claiming full POSIX signals at v1.0.

### 1.5 Finding R-4 — the brand hook already exists

`kernel/elf.c:77`: `if (eh->e_ident[EI_OSABI] != ELFOSABI_NONE) return
ELF_LOAD_BAD_OSABI;` and `PT_INTERP` is already parsed at `elf.c:179`.

(That check is **widened to also accept `ELFOSABI_GNU`** by the Clade arc -- committed
on `clade-cl4-wip` @ `7cfcabce`, reaching `main` when that branch merges. On `main`
today the check is still the narrow `!= ELFOSABI_NONE` form quoted above. Either form
serves R-4 identically: the point is that the byte is already read at exec time.)

`EI_OSABI` is **precisely the byte FreeBSD brands on**. So the detection hook sits
exactly where it needs to, already reading the right field. Brand inference (§5.2)
is a small, well-understood change to an existing check rather than new machinery.

### 1.6 Smaller deltas (each tractable, each real)

- **auxv**: `exec_fill_auxv` supplies 8 entries (incl. `AT_HWCAP` from the CF-4 A
  work, `AT_RANDOM`). A Linux binary additionally wants `AT_PHDR/PHENT/PHNUM/ENTRY/
  PAGESZ/BASE/FLAGS/UID/EUID/GID/EGID/SECURE/CLKTCK/PLATFORM`. Mechanical.
- **vDSO**: Thylacine's vDSO (#343) is published under the *deliberately private*
  `AT_VDSO_CLOCK = 0x5654`, **not** Linux's `AT_SYSINFO_EHDR = 33`. A Linux binary
  finding no `AT_SYSINFO_EHDR` simply falls back to the syscall — correct by
  absence, no work needed. (A Linux-shaped vDSO is a v1.x speed item.)
- **TLS**: aarch64 TLS is `TPIDR_EL0`, which the kernel already sets at
  exec/thread-create. Static musl sets up its own TLS block. No delta.
- **Dynamic linking**: `glibc`-dynamic is explicitly *best-effort* per the risk
  register (risk #9, LOW). **v1.0 targets musl-static**, matching ROADMAP §9.2's
  own wording ("pre-built … **static** binary"). `ld-musl-aarch64.so.1` support is
  a v1.x step that mostly needs `/lib` layout + `mmap` fidelity, not new ABI.
- **`ioctl`**: the long tail. Terminal `ioctl`s already have a real home (the PTY-3
  boundary-line proved the tty dispatcher shape); the rest degrade to `ENOTTY`.

---

## 2. SOTA — how everyone else does this

Per CLAUDE.md's research discipline: heritage first, then the capability-microkernel
peers, then fit.

### 2.1 Heritage (Plan 9)

Plan 9 **never** ran foreign binaries. Its answer to "the POSIX world" was **APE**
(the ANSI/POSIX Environment) — a *source*-compat library you recompiled against.
That is exactly what **Pouch** is, and it is why Pouch was the right Phase-6 answer.
The heritage therefore gives us the *namespace* half brilliantly (per-process
namespaces are the container primitive, decades before containers) and gives us
**nothing** for the binary half. This is a place where Thylacine must go past Plan 9,
and it should be honest about that rather than pretending a lineage exists.

### 2.2 The peers

| System | Model | Where the Linux ABI lives | Lesson for us |
|---|---|---|---|
| **FreeBSD Linuxulator** (`COMPAT_LINUX`) | In-kernel personality | Kernel: per-process `sysentvec` chosen at exec by ELF brand | The canonical proof that brand-at-exec + a per-process syscall vector works and lasts (25+ yrs, runs Steam games). Also proves `linprocfs`/`linsysfs` are **mandatory in practice**, not optional polish. |
| **illumos / SmartOS LX brand zones** | In-kernel personality **fused with the container** | Kernel, scoped to a *zone* | The closest shape to ours: personality is a property of the **container**, not a loose per-process flag. Ran entire unmodified Ubuntu/Alpine userlands. Validates "territory + phenotype" as one object. |
| **WSL1** (pico processes) | In-kernel personality via a provider driver | NT kernel drivers (`lxcore`) | **The cautionary tale.** Ran unmodified Ubuntu, then Microsoft *abandoned it for a VM* — beaten by the syscall long tail (`inotify`, `/proc` completeness, `io_uring`, `ptrace`) and filesystem performance. Says: pick a bounded target and publish the fidelity ladder, or drown. |
| **gVisor (Sentry)** | **Userspace kernel** | A Go process implementing 200+ syscalls; traps via ptrace or KVM | Proves a userspace Linux kernel is viable and gives real containment. Their syscall-coverage tables are the best public map of what workloads actually need. |
| **Fuchsia Starnix** | **Userspace kernel on a capability microkernel** | A Starnix component per container | **The single closest peer to Thylacine.** Zircon is a capability microkernel with no POSIX ABI; Starnix runs unmodified Linux/Android binaries by implementing the UAPI in *userspace*, using Zircon's **restricted mode** (`zx_restricted_enter`) so the guest's syscalls trap back to the userspace kernel rather than into Zircon. This is the modern answer to exactly our question. |
| **NetBSD `COMPAT_LINUX`, Solaris `lxrun`** | In-kernel personality | Kernel | Same family as FreeBSD; corroborates the brand mechanism. |
| **Wine / Darling** | Userspace loader + libraries | Userspace | Different problem (no kernel cooperation), but the "personality is a loader + a library set" idea informs the diorama. |

### 2.3 What the survey actually settles

1. **Brand-at-exec + per-process ABI vector is universal.** Every system that runs
   unmodified foreign binaries does this. It is not a design choice; it is the
   mechanism. (Confirms §1.2.)
2. **Synthetic `/proc` and `/sys` are load-bearing, not decoration.** Linuxulator
   ships `linprocfs`/`linsysfs`; gVisor and Starnix implement procfs in full. Real
   programs read them constantly (`ldd`, allocators, thread counts, `/proc/self/exe`,
   `/proc/self/maps`, `/dev/urandom`).
3. **The two viable homes for the ABI are the kernel (fast, fat trust surface) and a
   userspace server (idiomatic on a microkernel, needs a trap-redirect mechanism).**
   The industry's *newest* systems on capability kernels (gVisor, Starnix) chose
   userspace. The oldest (Linuxulator, LX) chose the kernel because they were
   monolithic already.
4. **Scope discipline decides success.** WSL1 died of unbounded fidelity ambition;
   Linuxulator thrives on a bounded, honestly-documented subset.

---

## 3. Fit to Thylacine

### 3.1 The grain of the system

Everything substantial in Thylacine is a **userspace 9P server**: netd (network),
stratumd (filesystem), tapestryd (compositor), ptyfs (pseudoterminals), corvus
(identity), the whole Menagerie driver tier. The kernel is deliberately ~100
syscalls. Adding ~150 Linux syscalls **in the kernel** would roughly double the
kernel's syscall surface and put a large, historically bug-dense compatibility layer
inside the trust domain — against the grain of every other decision in the tree.

### 3.2 What we have that Starnix has

A per-Proc namespace, a capability handle table, an async ring (Loom), a
demand-paged file-backed exec, and userspace servers as the normal way to build
things. The parallel is close enough that Starnix should be read as the reference
architecture.

### 3.3 What we lack that Starnix has

Zircon's **restricted mode** — a hardware-assisted way to run a thread such that its
syscalls return to a *userspace* supervisor instead of the kernel. Thylacine has no
such mechanism today. Building one is the crux of §4 Option B. (It is not exotic: it
is "on `svc` from a phenotyped Proc, don't dispatch — park the thread and hand the
register frame to its supervisor," which is structurally the debug-stop machinery
from I-39 plus a Loom ring.)

### 3.4 The novel angle (NOVEL.md candidate)

On Linux, "being in a container" is a bundle of global kernel features (namespaces,
cgroups, seccomp) that a process is placed into. On FreeBSD/illumos, "being Linux"
is a kernel mode. On Thylacine:

> **A Linux container is a territory plus a phenotype.** The territory supplies the
> *world* (a Linux-shaped `/proc`, `/sys`, `/dev` assembled from ordinary per-container
> 9P servers, mounted only where that Proc can see them); the phenotype supplies the
> *ABI shape*. Both are per-Proc, both are capability-bounded, neither is global, and
> neither confers authority.

The consequence is nicer than the peers': `/proc` is not a global kernel filesystem
that must be namespaced after the fact — it is a **per-container userspace server**
that never had global reach to begin with. The Plan 9 thesis ("the filesystem is the
OS") turns out to be a *better* substrate for Linux compat than Linux's own, because
the isolation is structural rather than retrofitted. That is the claim worth
recording in `NOVEL.md`.

---

## 4. THE ARCHITECTURAL FORK — **RESOLVED: Option C** (user vote, 2026-07-23)

Where does the Linux ABI live? **Decided: the hybrid (C).** The split rule below is
binding: a syscall may live in the kernel table **iff** its translation is *total and
stateless*; the moment it needs state the kernel does not already own, it forwards.
Options A and B are recorded for the reasoning, not as live choices.

### Option A — in-kernel phenotype (the Linuxulator/LX model)

A per-Proc `phenotype` field; `syscall_dispatch` branches to `linux_dispatch()`;
a `kernel/linux/` translation table calls the existing `sys_*_for_proc` bodies.

- **+** Fastest (no extra hop; a translated syscall costs what a native one costs).
- **+** Simplest to build; reuses every existing `*_for_proc` body directly.
- **+** No new wait/wake protocol — the riskiest kind of code in this tree.
- **−** ~150 syscalls of compat code inside the kernel trust domain; the historical
  bug density of exactly this layer (Linuxulator CVEs) lands in ring 0.
- **−** Against the grain of §3.1; the kernel roughly doubles in syscall surface.
- **−** Socket translation (R-2) would put 9P-client-driving socket emulation **in
  the kernel**, which is a genuinely unattractive place for it.

### Option B — userspace phenotype server (the Starnix/gVisor model)

The kernel gains **one** new mechanism: a phenotyped Proc's `svc` does not dispatch;
it parks the thread and delivers `(x0..x7, x8)` to a **supervisor Proc** over a Loom
ring, which replies with the return value. The Linux ABI (all ~150 calls, sockets,
`/proc` glue) lives in a userspace server.

- **+** Idiomatic: the compat layer is a server like netd/stratumd/ptyfs.
- **+** Blast radius contained — a bug in Linux-ABI code cannot corrupt the kernel.
- **+** Socket translation happens in userspace, where `/net` already lives, so R-2
  dissolves into "the supervisor opens `/net/tcp/clone` on the guest's behalf."
- **+** Composes with Loom exactly as designed; per-container supervisors fall out.
- **−** Per-syscall latency: a park + ring round-trip + wake on **every** syscall.
  For a build-heavy workload that is the dominant cost. Mitigable (batching,
  SQPOLL-style spinning, in-kernel fast paths) but real.
- **−** A **new wait/wake protocol on the death lineage** — the single most bug-prone
  surface in this tree (#788/#806/#860/#809/#811/#68/#89). Needs spec-first
  (`specs/phenotype.tla`) and a hard audit.
- **−** More total work than A.

### Option C — hybrid: in-kernel fast path, userspace tail (**recommended**)

The top-N Linux syscalls **that map 1:1 onto an existing Thylacine syscall** are
translated in-kernel by a pure, table-driven renumber + argument shuffle (no new
semantics, no new state — `read/write/openat/close/lseek/mmap/munmap/futex/clock_gettime/
exit_group/…`). Everything else — sockets, `ioctl`, signals-beyond-default, the
`/proc` glue, anything needing judgement — forwards to the userspace supervisor via
Option B's mechanism.

- **+** The hot path (which is >90% of dynamic syscall count in real workloads) pays
  **zero** extra cost, because those calls are literally the same operations under a
  different number.
- **+** The dangerous, stateful, historically-CVE-dense tail lives in userspace.
- **+** The in-kernel part is a **table**, not logic — auditable by inspection, and
  it adds no new kernel *semantics*, only a decode.
- **−** Two code paths to reason about; the split must be principled (see below) or
  it becomes ad hoc.

**The principled split** (this is what keeps C from being a mess): a syscall may live
in the kernel table **iff** its translation is *total and stateless* — a pure
renumber plus an argument-order/flag-bit mapping onto exactly one existing
`sys_*_for_proc`, with no new kernel state, no new error semantics, and no policy.
The moment a call needs state the kernel doesn't already own (socket tables, signal
dispositions, `/proc` content, `ioctl` dispatch), it forwards. That rule is
mechanical enough to audit and to enforce in review.

**Recommendation: C.** It gets Option A's performance on the calls that matter and
Option B's containment on the calls that are dangerous, and its split rule is
objective. Option B alone is the purist answer and would be defensible if per-syscall
latency proves acceptable; Option A alone should be rejected on trust-surface grounds
regardless of convenience.

### 4.1 The forward *destination* — an unresolved premise (V-3 entry, 2026-07-30)

Option C's split rule is sound and stands. Its other half — "everything else
**forwards to the userspace supervisor**" — names a destination that, measured
against the tree, cannot serve what it is supposed to serve. This was found on
entry to V-3, before any of it was built.

**The verified fact.** No syscall lets one Proc mutate another Proc's address
space, handle table, or process tree. `burrow_share_into` (`kernel/burrow.c:790`)
and `handle_alloc(struct Proc *p, ...)` both *take* a target Proc, but neither is
reachable from EL0 for a Proc other than the caller; `I-4` records the 9P
handle-transfer path as still future. So a separate supervisor Proc cannot serve
`mmap`, `munmap`, `mprotect`, `brk`, `openat`, `close`, `pipe2`, `dup3`, `clone`,
`execve`, `socket`, `futex`, `chdir`, or an `ioctl`/`read`/`write` on a guest fd.
That is substantially all of `ARCH §11.5`'s top-50: the forwarded set a
supervisor Proc could actually serve is **empty**.

It is not a corner case. `third_party/musl/src/env/__init_tls.c:137` mmaps for
TLS whenever it exceeds the builtin block, and mallocng needs
`mmap`/`madvise`/`mremap` — a Linux guest cannot reach `main` without `mmap`.

**Why the peers do not have this problem.** The research that should have
accompanied §4 originally:

- **Heritage — Plan 9 `linuxemu`.** A *userspace* program that intercepts
  syscalls through the **`/proc` debug interface**: the traced process halts in
  the kernel at each syscall and the tracer drives it. It works because the
  tracer never acts *for* the guest — it makes the **guest** act, by rewriting
  the guest's own registers. Thylacine has this mechanism already: the I-39
  debug-fs, modeled in `specs/debug_stop.tla` and audited at 8a/8b/8c.
- **SOTA — Fuchsia Starnix.** Restricted mode (`zx_restricted_enter`): the
  Starnix kernel runs in the *normal mode of the same thread* that runs the Linux
  code. No IPC, no context switch. Fuchsia built it (RFC-0261, *"Fast and
  efficient user space kernel emulation"*) precisely because the
  separate-process approach was too slow. It needs a hardware/kernel mode
  Thylacine does not have.
- **gVisor** reaches the same place by a third road: the Sentry *owns* the
  guest's address space (via ptrace, or as a VM), which is exactly the authority
  a Thylacine supervisor Proc lacks.

The common shape: a compat supervisor works only when it either **is** the guest
(same thread), or **owns** the guest (debug/VM authority). "A peer Proc on the
other end of a ring" is neither, and that is the premise §4 assumed without
checking.

**Decision (user-voted 2026-07-30): V-3 is DEFERRED, and its destination is
decided by V-5.** Building the channel now would repeat the arc's own corrected
error — V-2's tables deliberately had no caller until V-7 made one possible
(§6.18 "why the table came first"), and a forward channel with an empty servable
set is the same mistake one layer up. Sockets (V-5) are the first chunk that
genuinely needs a destination, and its requirement — multi-step orchestration
(`open /net/tcp/clone`, read the number, open `ctl`) that no single
call-and-reply can express — is what will decide the shape. The live candidates,
recorded so V-5 does not re-derive them:

1. **Debug-injection** (the heritage answer): the supervisor stops the guest at a
   forwarded call, rewrites its registers so the **guest** performs the work with
   its own authority, and resumes. I-43 holds trivially. Near-zero new kernel
   surface — `proc_debug_fault_stop` is already the template. Costs a stop +
   several `/proc` ops + a resume per forward, and occupies the guest's debug
   slot.
2. **A native helper thread inside the guest Proc** (Starnix-by-threads): threads
   of one Proc share address space, handle table, Territory *and* authority, so a
   helper can serve everything with exactly the guest's rights. Needs the
   phenotype to become per-Thread and a new intra-Proc park/wake.
3. **As sketched** (a ring to a peer Proc) — only viable with new cross-Proc
   mmap/handle-install/spawn authority, each a new privilege relationship needing
   its own invariant. That is the trust surface choosing C over A was meant to
   avoid.

**The consequence that binds code today: with no supervisor, `FORWARD` means
`ENOSYS`, which inverts the argument-domain calculus.** §6.19 argues "declining
is always safe (the supervisor is strictly more capable)". That was true with a
live supervisor and is **false now** — a declined call is a guest-visible
failure, not a slow path. So a T2 translator must admit everything it can prove
*exactly equivalent*, not merely the minimum that is easy to defend. Narrowness
is no longer free. (This is what promotes `mmap`/`munmap` from §6.18's FORWARD
rows to T2 rows under the rule V-2b already established — see §6.21.)

---

## 5. Mechanism (settled, given any of A/B/C)

### 5.1 The phenotype

A per-Proc field (`Proc.phenotype`), values `PHENO_NATIVE` (0, the default) and
`PHENO_LINUX` (1). Set **only** at exec, from the brand (§5.2). Inherited by `rfork`
like the other per-Proc properties. Never settable by a syscall — a Proc cannot
change its own ABI at runtime (that is both unnecessary and an obvious attack shape).

### 5.2 Brand detection at exec

In priority order, at `kernel/elf.c` (the check already at `:77`):

1. `PT_INTERP` naming a Linux loader (`/lib/ld-musl-aarch64.so.1`,
   `/lib/ld-linux-aarch64.so.1`) → `PHENO_LINUX`.
2. `EI_OSABI == ELFOSABI_LINUX (3)` → `PHENO_LINUX`. (Note: `ELFOSABI_GNU` is the
   same value 3; the Clade arc already widened the accept-list to it, so this must be
   reconciled — a Thylacine-native Clade binary must **not** be mis-branded. See
   §12 Q3.)
3. A `.note.ABI-tag` / `NT_GNU_ABI_TAG` note naming Linux → `PHENO_LINUX`.
4. Otherwise → `PHENO_NATIVE`.

Explicit escape hatch: the **vivarium** manifest may force a phenotype for a whole
container, which is how ambiguous or unbranded binaries (very common for
`musl-static`, which often carries `EI_OSABI = 0`) are handled. In practice **the
container declares the phenotype and the ELF byte is a hint** — this is the LX-zone
lesson from §2.2, and it is the reason the fused container+phenotype object is the
right granularity.

### 5.3 Argument translation

Structural, not semantic: Linux `struct stat`/`statx` ↔ `t_stat`; `O_*`/`AT_*`/
`MAP_*`/`PROT_*` flag bits; `iovec` scatter-gather → the existing bulk paths;
`getdents64` ↔ `SYS_READDIR`'s dirent stream (the pouch `0024` patch already proved
this exact translation in userspace); errno is already POSIX-aligned by `ERRORS.md`
(`T_E_* == POSIX`), which removes an entire class of work.

### 5.4 Signals (the R-3 ladder)

- **Tier 0 (v1.0)**: default dispositions only. `SIGPIPE`→ignore-or-terminate,
  `SIGINT`/`SIGTERM`/`SIGQUIT`→terminate (already the `interrupt`/`tty:*` note
  semantics, LS-5), `SIGCHLD`→`wait4`. `rt_sigaction` for these returns success and
  records the disposition; a handler for a *fatal* signal is honored via the
  full-frame path when it lands, else the default applies.
- **Tier 1 (v1.0 if the ladder allows)**: real handler delivery — kernel-built
  `ucontext` frame on the user stack + `rt_sigreturn`. This is the audit-bearing
  piece; it composes with notes (I-19) and the EL0-return-tail delivery checkpoint
  that already exists for note delivery and for the I-39 debug stop.
- **Tier 2 (v1.x)**: full `sigprocmask`/`sigaltstack`/`SA_RESTART`/queued
  `siginfo`/`tgkill` fidelity.

### 5.5 Sockets (R-2)

The Linux socket family is **translated to `/net` file operations** — the same
mapping the pouch boundary-line (`0016`) already implements and proved, relocated to
whichever side §4 chooses. Under Option C it forwards to the supervisor, which is
also where `/net` already naturally lives. `AF_UNIX` maps to the existing `/srv`
byte-mode services (the `0006` patch's proven mapping). `AF_INET6` → `EAFNOSUPPORT`
at v1.0, honestly.

---

## 6. The diorama — the synthetic Linux world

`ROADMAP §9.1`'s `proc-linux/`, `sys-linux/`, `dev-linux/`, named as one thing: the
**diorama** (a constructed habitat presented to a specimen; §11).

Ordinary userspace 9P servers, mounted **into the container's territory only**:

- **`/proc`** — the load-bearing one (§2.3.2): `self/{exe,cwd,maps,fd/,status,cmdline,
  environ,auxv}`, `<pid>/…`, `meminfo`, `cpuinfo`, `stat`, `uptime`, `mounts`,
  `sys/kernel/{ostype,osrelease,version,hostname}`. Sourced from the *native*
  `/proc` (devproc) + `/ctl` where they already carry the data.
- **`/sys`** — minimal: enough for `ldd`, allocator probes, and CPU topology reads.
- **`/dev`** — **NOT a diorama tree. Corrected at V-4c; see §6.15.** This bullet
  originally asked the diorama to present `null`/`zero`/`full`/`random`/`urandom`/
  `tty`/`pts/`/`ptmx`/`fd/`/`std{in,out,err}` as a "re-presentation, not a
  reimplementation" — but the diorama is read-only by §6.1, and `/dev/null` is
  defined by *accepting writes*. Native devdev already serves these correctly, so
  routing them through a read-only server would break working files. A container's
  `/dev` is composed by **bind**, in its own territory; the entries devdev lacks
  land in the phenotype (`ptmx` — already done at PTY-3 — and `fd/`/`std*`) or in
  `viv` (`tty`, a bind of the container's own pts).

Because each is a per-container mount, `uname`/`hostname`/`meminfo` can differ per
container **without any namespacing machinery** — the property Linux needed six
namespace types to get.

### 6.1 The server shape (settled — the ptyfs precedent)

`usr/diorama`, a **native libthyla-rs, device-less `/srv` server** — exactly
`usr/ptyfs`'s shape (`usr/ptyfs/src/main.rs`), which is the newest and cleanest
example in the tree: joey spawns it with `T_SPAWN_PERM_MAY_POST_SERVICE`, it posts
`/srv/diorama`, joey mounts its tree, and it runs a **selftest before it serves** so
a logic failure gates the boot instead of surfacing later as a mystery. It owns no
hardware (so it is the corvus/ptyfs tier, NOT a warden-bound driver).

It is **read-only**. There is no write path at v1.0: every file is a rendered view,
`h_write` returns `E_PERM`. That single decision removes most of the surface a
`/proc` would otherwise carry.

### 6.2 Where the content comes from — and why I-43 holds by construction

The diorama renders from three sources, all of which the calling Proc could already
reach natively:

| Source | Supplies |
|---|---|
| native `/proc` (devproc: `status`, `cmdline`, `ctl`) | per-Proc state |
| native `/ctl` (`/ctl/procs`, `/ctl/mm/`, the boot/uptime counters) | system state |
| the caller's own syscalls (`getpid`/`getuid`/`clock_gettime`) | identity + time |

**The diorama can therefore never leak anything the native surface would not have
handed over** — it is a *reformatter*, not a new authority. That is I-43 satisfied
structurally rather than by review: the kernel's existing gates (`I-26` two-axis on
`/proc/<pid>`, `CAP_HOSTOWNER` on `/ctl/kernel-base`, the #57a-F2 lifetime rules)
run unchanged underneath, and a read the kernel refuses the diorama is a read the
diorama cannot serve. **Do not "improve" a diorama file by reading kernel state
through any path a native Proc could not use** — that is precisely how a
compatibility shim becomes a privilege hole.

### 6.3 The file set, in priority order

**Tier 1 — has a consumer TODAY** (this is why V-4 is not scaffolding):

- **`/proc/self/exe`** — *the load-bearing one*. Nothing in the tree provided it
  (grep-verified 2026-07-23), which is exactly why the Clade fork had to patch LLVM's
  `getMainExecutable` to fall back to `argv[0]` (fork patch `0001`, CL-3).

  **CORRECTION (V-4b-3): V-4a served this file, but not yet in the shape that
  consumer needs, so the fork delta could not be revisited yet.** Linux serves
  `/proc/self/exe` as a **symlink** — `readlink()` yields the path, while
  `open()`+`read()` yields the executable's *bytes*. The diorama serves a regular
  file whose *contents* are the path, so `readlink()` failed and LLVM (which calls
  exactly `readlink`) still fell through to `argv[0]`; the LLVM patch `0005` says as
  much in its own words. The blocker was structural rather than a diorama bug:
  **there is no `SYS_READLINK` — Thylacine has no EL0 symlink surface at all** (the
  9P `Treadlink`/`Rreadlink` ops exist but are used only by the kernel's own client
  and Loom's `READLINK` opcode).

  **RESOLVED (V-4b-4)** by the predicted route — translating `readlink()` in the
  **phenotype**, at the pouch boundary-line (§6.11), rather than growing a symlink
  surface. `readlink("/proc/self/exe")` now returns the path in-guest, and
  `realpath()` works, so LLVM's `__linux__` branch would run verbatim. **The fork
  delta is not yet dropped**: doing so requires rebuilding the LLVM fork and
  re-running the Clade gates, which belongs to the Clade track that owns that fork.
  Recording it as available, not as done — the mistake this entry exists to correct
  was claiming a consumer-level win from a mechanism that had not been run.
- `/proc/self/cmdline`, `/proc/self/status`, `/proc/self/maps` — allocators, crash
  handlers, and every "what am I" probe.
- `/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/stat`.

**Tier 2 — needed by the v1.0 target binaries**: `/proc/<pid>/…` (the same fields,
subject to the native gate), `/proc/mounts`, `/proc/sys/kernel/{ostype,osrelease,
version,hostname}`, `/proc/self/{cwd,fd/,environ,auxv}`.

**Tier 3 — `/sys` + `/dev`**: minimal `/sys` (enough for `ldd` + allocator probes +
CPU topology) — **the CPU-topology half LANDED at V-4c-1** (§6.16), served as a
sibling tree of `proc` under one root and delivered by bind. **`/dev` was corrected
out of the diorama at V-4c (§6.15)**: it is composed by bind from the native devdev
+ ptyfs trees, with the missing Linux entries landing in the phenotype or in `viv`,
because a read-only server cannot serve `/dev/null`. Both halves of Tier 3 thus
arrive by the *same* mechanism — which is the finding, not a coincidence.

### 6.4 Sub-chunks

| # | Contents | Gate |
|---|---|---|
| **V-4a-0** | **LANDED.** The kernel prerequisite §6.5 found: `Proc.exe_path` + `/proc/<pid>/exe` | `/proc/<ptyfs-pid>/exe` reads `/bin/ptyfs` in-guest (joey, boot-fatal) |
| **V-4a-0b** | **LANDED.** The second prerequisite §6.6 found: `srv_peer_info.pid` (how the server resolves `self`) | a live peer snapshot reports its pid; a dead one fail-closes to 0 |
| **V-4a** | **LANDED.** `usr/diorama` + Tier 1 + the selftest + `/bin/diorama-probe` (as-built: `docs/reference/141-diorama.md`) | **MET** -- the probe mounts the diorama itself and reads `/self/exe` back as `/bin/diorama-probe` |
| **V-4b-1** | **LANDED.** `/self/cwd` + its kernel source `/proc/<pid>/cwd` | the probe reads a non-empty absolute cwd in-guest |
| **V-4b-2** | **LANDED.** `/self/maps` + its kernel source `/proc/<pid>/maps` | the probe reads a Linux-shaped map with `[stack]` + a file-backed row naming the binary |
| **V-4b-3** | **LANDED.** the numeric `/proc/<pid>/…` dirs + the root pid enumeration + `sys/kernel/{ostype,osrelease,version,hostname}` | the probe reads its OWN pid's dir, finds itself in the root readdir, and gets ENOENT for a pid that cannot exist |
| **V-4b-4** | **LANDED.** the *shape* half (§6.11): `readlink()` in the phenotype — the four `/proc` link-shaped paths, plus the truthful `EINVAL`/`ENOENT` that repairs `realpath()` system-wide; + the `/proc` apex stat | the prover readlinks its own `exe`/`cwd` in-guest against NATIVE `/proc`, `self` and `<pid>` agree, truncation is POSIX-silent, and `realpath()` canonicalizes |
| **V-4b-5** | **LANDED.** the synthetic-Dev stat family (§6.12): `stat_native` for `/ctl` + `/env`, the POSIX file-type bits on devproc's modes, and the leading-zero pid reject that restores native-vs-diorama coherence | the prover stats `/ctl` + `/env` as directories in-guest, `S_ISDIR("/proc/<pid>")` is true, `realpath("/ctl/./procs")` canonicalizes, and a zero-padded pid does not resolve |
| **V-4b-6** | **LANDED.** `/self/environ` (§6.13): a new kernel source `/proc/<pid>/environ` rendering the Env as Linux's NUL-separated block — offset-aware, and the first devproc info file to carry a real read gate | the prover sets a variable in its own `/env` and reads the record back through the diorama in-guest; the per-pid variant is proven ABSENT (the cross-principal leak) |
| **V-4b** | **CLOSED.** the rest of Tier 2 (`self/{fd,auxv}`) is dispositioned, not built: `auxv` **weighed and deliberately not built** (§6.14 — zero live readers, and a `viv`-launched binary gets its auxv on the stack by construction); `fd` **blocked on #66c**, the #926 handle-table lifetime restructure, which is a kernel chunk and not a Vivarium one | both dispositions recorded with evidence + a named trigger; neither is a silent omission |
| **V-4c** | **RESCOPED by §6.15** (scripture-first, no code written): `/dev` is *not* a diorama tree — a read-only server cannot serve `/dev/null`, and native devdev already serves it correctly — so V-4c is a minimal `/sys` + the per-container mount wiring (now the substantive half, since bind is the answer for `/dev` as well as the delivery for `/proc`) + the two Tier-1 stragglers `cpuinfo`/`stat`, each only partly sourced + the focused audit | audit close on the §6.2 no-new-authority property — now including §6.13's **deputy-authority** half (a proxy must not be allowed where its client would be denied) — and on §6.12's file-identity claim (devenv is an ARCH §25.4 trigger surface; V-4b-5/6 landed on self-audit, as V-4b-1..4 did, with the formal round scheduled here) |
| **V-4c-1** | **LANDED.** one server, two trees, **by bind** (§6.16): the root becomes the synthetic *world* with `proc` and `sys` as siblings, and `/sys/devices/system/cpu/{online,possible,present}` + the `cpuN` dirs land, all sourced from `/ctl/cpu`. The aname route §6.15 named is **closed**, not merely harder — `devsrv_open_connect` attaches with a hardcoded empty aname and `SYS_ATTACH_9P_SRV` is byte-mode-gated — and `SYS_MOUNT` already takes a subdirectory, so this cost **no kernel change** | the prover reads the cpulists in-guest AND binds `/dio/sys` at a second path, reading the same bytes through the new name — the composition V-7 depends on, proven rather than assumed; the sibling-isolation and online-mask selftest legs are revert-probed (each fails the boot when broken) |
| **V-4c-2a** | **DECIDED (§6.17), scripture-first, no code.** The three recorded instances turned out to be **five exposures, one trap, and a sixth instance nobody had counted**: four fields already have a kernel source, `intr`'s source is real but *narrower than the field* (the back-door fabrication), and `stat`'s user/system split has no material at all and cannot be omitted because the columns are positional | one rule covering all seven: give the kernel a source, per-CPU, in the kernel's own shape — and omit only what has no truth to tell |
| **V-4c-2b** | the kernel sources §6.17 calls for: the two per-CPU counters (`gic_dispatch`, the `sched()` switch chokepoint), the per-CPU MIDR read at bring-up, `CTR_EL0` + the hwcap word surfaced, `g_next_pid` accessor — all landing as `/ctl/cpu` columns + one `/ctl/sched` scalar | each new column read back in-guest; prowl unaffected (positional 3-token parse); the two counters advance under load |
| **V-4c-2c** | **LANDED.** the diorama half: `/proc/stat`, `/proc/cpuinfo`, and the `cpuN/cache/index0/coherency_line_size` leaf that lifts V-4c-1's deliberately-empty `cpuN`. The cpu qid gains a *kind* above the index, so `cpu_qid(n)` stays bit-identical and the subtree is an extension rather than a renumbering | the prover reads all three in-guest and asserts the properties a consumer depends on, not mere presence: `intr`/`ctxt` must be NONZERO (a zero means the column did not parse -- a well-shaped lie a presence check would pass), the line size must be a power of two >= 16, `Features` must carry at least `fp asimd`, the implementer must not be the reserved `0x00` unread-MIDR sentinel, and `BogoMIPS` must be ABSENT |
| **V-4c-3** | the arc's **focused audit** — OWED across V-4b-1..6 + V-4c, all of which landed on self-audit only, and a **merge gate** for `gfx-4 → main` | as the V-4c row above: §6.2 no-new-authority, §6.13 deputy-authority, §6.12 file-identity |

**V-4 is UNBLOCKED** — it neither waits on nor collides with the main track's Clade
work. It is *almost* pure userspace: see §6.5 for the one kernel prerequisite the
build surfaced.

### 6.5 The one kernel prerequisite (V-4a-0, LANDED)

**This section corrects §6.4's original "no kernel file touched" claim**, which was
written before the file set was ground-truthed against the tree.

`/proc/self/exe` — Tier 1's load-bearing entry — turned out to be unrenderable from
any existing surface. The system retained **no executable identity for a running
Proc at all**: `struct Proc` had no name and no path field, the REVENANT Image
cache is keyed by qid (not by name), the text Burrow is anonymous, native
`/proc/<pid>/status` reports only pid/state/threads/pages/children, `/ctl/procs`
only pid/state/threads, and `format_cmdline` is a literal stub. So the diorama had
nothing to reformat.

The §6.2 rule made the resolution obvious rather than optional. A diorama that
*derived* an exe path some other way — or accepted one asserted by its client —
would stop being a reformatter and start being an authority, which is exactly the
failure mode §6.2 exists to prevent. **The fix therefore belongs in the kernel**,
and it was nearly free: `exec_resolve_from_namespace` already holds a Spoor whose
#66 `Path` *is* the resolved absolute path of the binary. Pin a ref to it.

As-built (`docs/reference/32-devproc.md` → `/proc/<pid>/exe`):

- `Proc.exe_path` — a ref-held `struct Path`, set at the tail of a **successful**
  `exec_setup_from_spoor` (the single chokepoint every production exec funnels
  through), `rfork`-inherited, released at `proc_free`. Strictly non-load-bearing
  (I-33): NULL is valid and renders empty, and a Path-alloc failure never fails an
  exec. `struct Proc` grew 352 → 360 (the first genuine growth since the 328
  baseline — no tail pad remained).
- `/proc/<pid>/exe`, mode `0444`, ungated like its `ns`/`status` siblings. It adds
  nothing to the disclosure envelope: `/proc/<pid>/ns` already renders the target's
  whole mount list, which strictly dominates one path.

Proven by `devproc.read_exe` (both legs revert-probed) plus a boot-fatal joey probe
reading the just-spawned ptyfs's `exe` back as `/bin/ptyfs` — the E2E leg the unit
test structurally cannot cover, since it drives `format_exe` with a synthetic Path
and so cannot prove a *real* spawn records what `stalk` resolved.

### 6.6 The second prerequisite — how the diorama knows who `self` is (V-4a-0b, LANDED)

`/proc/self/…` is not a file, it is a *question about the caller*. A 9P server
answers it by identifying its peer, and Thylacine's channel for that is
`SYS_SRV_PEER` (`struct srv_peer_info`, CORVUS-DESIGN §6.3 / C-22) — kernel-stamped,
never client-supplied, and gated to the service's own poster.

But the struct reported `stripes` (an opaque per-Proc identity tag), `caps`,
`principal_id`, `primary_gid`, `console`, `flags` — **and no pid**. `stripes` has
no userspace pid mapping (nothing renders it in `/proc` or `/ctl`), so the diorama
could learn *which principal* was talking to it but never *which process* — and
therefore could not resolve `self` to a `/proc/<pid>` at all.

Taking the pid from the client instead is precisely the §6.2 failure mode, so the
fix is again kernel-side, and again nearly free: the struct's `_reserved` u32 at
offset 36 is exactly the right size. `srv_peer_info.pid` fills it **in place** —
same size, same offsets, so it is an append rather than an ABI break (0 was what a
pre-V-4a consumer read there, and 0 remains the "unknown" value). It rides the same
alive-gated `g_proc_table_lock` walk as `caps`/`principal_id`, so a dead or reaped
peer fail-closes to 0 rather than reporting a pid that a *reused* table entry now
owns.

Disclosure-neutral: `/ctl/procs` already lists every pid to everyone, ungated, and
the poster gate means the pid reaches only the server the peer chose to connect to.

Proven by `proc.identity_peer_snapshot_by_stripes` (the pid on a live match; the
out-param untouched on both no-match paths, so a caller cannot mistake "no match"
for "the peer is pid 0" — which is kproc).

*(The pouch `struct pouch_srv_peer_info` mirror keeps calling the slot `_reserved`
and ignores it. That is correct, not stale: the size is unchanged at 40, so nothing
overflows — cf. the #100 `t_stat` lesson, where the size DID change and the stale
mirrors overflowed at runtime.)*

### 6.7 The lesson for the rest of the arc

Two of Tier 1's requirements turned out to need kernel fields, in a sub-chunk
specced as pure userspace. That is not an accident, it is §6.2 working as intended:
**when a diorama file has no native source, the answer is to give the kernel one —
never to let the diorama invent it, and never to let its client supply it.** A
compatibility shim that starts sourcing answers outside the native surface has
stopped being a reformatter and become an authority, and I-43 stops being
structural.

Expect the question again, at least at:

- `/proc/self/cwd` — **DONE at V-4b-1**, and it played out exactly as predicted: a
  renderer, not a new mechanism. The Territory already had `dot_path` and
  `territory_getdot` already owned the `dot_lock` discipline, so `/proc/<pid>/cwd`
  is a thin call into it (the `format_ns` → `territory_format_ns` shape), and the
  diorama re-presents it unchanged. No new kernel state at all — cheaper even than
  `exe`, which had to *grow* a field.
- `/proc/self/maps` — **DONE at V-4b-2.** The prediction was right that the VMA
  list was unexposed and that a walk needs `vma_lock` rather than an existing
  accessor, but wrong about the lock being the hard part: `devproc_mem_walk_cb`
  (8a-1b-gamma-1) had *already* established and audited the
  `g_proc_table_lock → vma_lock` nest for cross-Proc `/proc/<pid>/mem`, so the
  ordering argument was inherited rather than made. The real work was the
  translation split — see §6.8.

Both were Tier 2 / **V-4b**, and the prediction to budget them as *kernel +
userspace* held for both, though for `cwd` the kernel half was a pure renderer
and for `maps` the lock discipline was already paid for. The pattern that keeps
holding is narrower than "these need kernel work": **it is worth grepping for an
existing accessor and an existing lock-order precedent before budgeting either.**

**V-4b-3 is the counter-case, and it sharpens the rule.** Per-pid `/proc/<pid>/…`
looked like the biggest of the three and needed **no kernel work at all** — because
`/self` was never a special file. It was always a per-pid render with the pid
supplied by the *connection's peer* rather than by the *path*, so the pid had been
a parameter since V-4a and per-pid was a generalization of an existing mechanism.
So the question to ask before budgeting is not only "does a native source exist"
but **"is this file already being rendered under another name?"** — and here the
answer was yes, five times over.

### 6.8 Where the Linux shape lives — the `maps` split

`/proc/<pid>/maps` is the first file where the native and Linux renderings differ
enough to force the question: which layer speaks Linux?

The answer is the one the whole design implies — **the kernel stays Thylacine and
the diorama does the phenotype.** The kernel emits a native six-column table
(`0x`-prefixed ranges, a backing-`type` column, a `devno:qid` file identity, a
`role` column) that a Thylacine tool would want; the diorama translates it into
Linux's column layout. Letting the kernel emit Linux's shape directly would be
phenotype leaking into the kernel, which is exactly the inversion VIVARIUM
exists to avoid. The `status` file set the precedent: native `key: value`, Linux
`Name:`/`Pid:`/`Uid:` out here.

Three translations are worth recording because each was a judgement call:

- **`dev`.** Thylacine's `devno` is flat — no major/minor split. It renders as
  `00:<devno>`, and that is *not* a fabricated major: Linux uses `00:xx` for
  every filesystem with no backing block device (tmpfs, and 9P mounts
  specifically), which is precisely what a Stratum mount is.
- **The pathname column** comes from `/proc/<pid>/exe`, under a stated premise:
  at v1.0 the only FILE Burrows in an address space are the exec'd binary's
  segments (`burrow_create_file` has exactly one caller, `image_lookup_or_create`,
  from exec, and there is no file-mmap syscall). The premise is written down at
  the call site rather than assumed away — **when a file-mmap surface lands, the
  kernel line must start carrying a path and the diorama must read it instead of
  substituting `exe`.**
- **`[vdso]`.** Thylacine's vdso is a read-only *data* page (the clock struct),
  so it renders `r--p`, not Linux's `r-xp` code vDSO. The tag is still worth
  emitting — the consumers that look for `[vdso]` in maps (sanitizers) do so to
  *exclude* the region, which is correct here too — but nothing should read the
  tag as promising an ELF object. Thylacine publishes no `AT_SYSINFO_EHDR`
  (it uses the private `AT_VDSO_CLOCK`), so no correct Linux program goes looking.

A guard VMA is emitted, never hidden: `---p` with no pathname is byte-for-byte
how Linux shows a `PROT_NONE` guard page, and dropping the row would make the map
claim the range is free.

### 6.9 The fourth source — the phenotype's self-description (V-4b-3)

`/proc/sys/kernel/{ostype,osrelease,version}` are the first diorama files that do
**not** reformat a native source. There is no kernel state to reformat: the answer
*is* the phenotype. This looks like a violation of §6.2's "three sources" and is
not, but the distinction has to be stated precisely or it becomes the loophole
every future file is argued through.

§6.2 exists to stop the diorama becoming an **authority** — serving something the
native surface would have refused. A constant carries no information about the
system at all, so there is nothing for it to leak; what it describes is this
server's own property. Hence the rule to hold to, for every file added after
these:

> A value **derived from kernel state** needs a native source, no exceptions. A
> **constant declaring which ABI the caller is looking at** is the phenotype
> speaking about itself. If a file cannot be argued into the second category in
> one sentence, it belongs in the first.

Two of the four are worth their own note:

- **`osrelease` = `6.1.0-thylacine`.** This is the one constant with teeth:
  glibc-linked programs parse it and some refuse to start below a minimum kernel
  (3.2 for modern glibc). 6.1 clears every such check. The `-thylacine` suffix is
  the honesty — Linux's own convention carries local suffixes (`-generic`,
  `-arch1-1`), so a parser that copes with real distro kernels copes with this,
  while anything that *prints* the string tells the truth. **Stated tradeoff**: a
  program could version-gate a feature on the number and take a path we do not
  implement. Declaring low instead makes those same programs refuse to run at all,
  which is strictly worse, and runtime feature probing — the overwhelmingly common
  pattern, and the one Linux itself pushes people toward — degrades gracefully
  where version-gating does not.
- **`hostname` is NOT in this category.** It would be system state if Thylacine had
  any; it does not (there is no hostname surface — `usr/coreutils/src/bin/uname.rs`
  hardcodes `(none)` for exactly this reason). So the render is the answer the
  *native tool already gives*: one answer for the system, not two. That it is also
  byte-identical to real Linux with no hostname set — the kernel's
  `init_uts_ns.name.nodename` is literally `(none)` — is a happy accident, not the
  justification. If a hostname surface ever lands, this reads from it.

### 6.10 What is left in V-4b, and what each actually costs

Ground-truthed against the tree at V-4b-3, because the three remaining Tier-2
files are *not* one chunk — they have very different blockers:

| File | Native source | Status |
|---|---|---|
| `self/environ` | **none reachable** — `/env` (devenv, §9.7) resolves `current_thread()->proc->env` **by construction**, so the diorama reading it gets its OWN environment, never the peer's | **DONE at V-4b-6** (§6.13). The §6.7 prediction held for the shape — a renderer over an existing group — but missed two things it could not have known from outside: the render had to be *offset-aware* rather than format-and-slice, and it is the first proxied file whose gate makes the *deputy's* authority differ from its client's. |
| `self/auxv` | **none** — `exec_fill_auxv` writes the block onto the user stack at exec and nothing retains it | **WEIGHED AND NOT BUILT** (§6.14). Zero live readers in the tree (every consumer takes the stack path; SDL2's is compiled out twice on aarch64), and structurally: auxv-on-the-stack is a *prerequisite* of V-7, so a `viv`-launched binary always has one. Named trigger + the retained-copy-not-reconstruction constraint in §6.14. |
| `self/fd` | **BLOCKED**, and not on us | `/proc/<pid>/fd` is deferred to **#66c** (ARCH §9.6.9): a cross-Proc fd-list read of a live peer races the #926 at-exit handle-table free, which runs outside `g_proc_table_lock` with lockless slot-zeroing. Closing it needs the #926 table-lifetime restructure — a death-path-lineage change that ARCH already says "warrants its own focused chunk + audit". **The diorama must not route around this**: there is no other native source for a Proc's fd list, and inventing one would be exactly the §6.7 failure. |

So the honest sequencing was: `environ` a normal kernel+userspace sub-chunk
(landed, V-4b-6); `auxv` a smaller one whose value should be weighed before
building it; `fd` waiting on a kernel chunk that is not a Vivarium chunk at all.

### 6.11 The other half of the shape problem — `readlink` (V-4b-4)

§6.2 governs *where a value comes from*. V-4b-4 is about the second, independent
question the diorama raises: **in what SHAPE does a consumer expect to read it?**
Serving the right bytes under the wrong shape is not compatibility.

Four `/proc` files are the case in point. Linux presents
`/proc/{self,<pid>}/{exe,cwd}` as **symlinks** — `readlink()` gives the target
path — while Thylacine presents them as regular files whose *contents* are that
path. Identical information, incompatible shape, and the consumer that matters
(LLVM's `getMainExecutable`, §6.3) calls `readlink` specifically.

Thylacine has no symlink surface to grow: there is no `SYS_READLINK`, and no EL0
path creates or observes a symlink. So the translation belongs in the **phenotype**
— concretely the pouch boundary-line (`0031-pouch-readlink.patch`), the layer whose
entire job is presenting Thylacine mechanisms in Linux shape. For those four path
shapes `readlink` is an open + read; the kernel renders both files as *bare bytes*,
no NUL and no newline, and `kernel/devproc.c` says so at `format_exe`/`format_cwd`
naming this consumer, so no reformatting is needed on either side.

**The rule this establishes, and its limit.** Reading a file to answer `readlink`
is only legitimate for a *closed whitelist* of paths the system defines; done
generally it would turn `readlink()` into a file-contents oracle. The whitelist is
four shapes, and a **miss is fail-safe**: an unmatched path falls to the general
arm, whose answer for a regular file is `EINVAL`, which is literally true of every
file we serve. `self` is rewritten to the caller's own pid rather than passed
through — native `/proc` has no `self` entry at all, and the diorama's `self` names
the *mounter* under a shared mount (§6.6), so a process asserting its own pid is
both more portable and strictly more correct.

**The finding that made this bigger than one file.** The seam had parked
`readlink` at the `ENOSYS` sentinel, and on a system with no symlinks that is the
*wrong* answer rather than an absent one — the result is knowable for every path,
and POSIX has the word: `EINVAL`. It matters because musl's `realpath()` is a pure
userspace resolver (1.2.x; it does **not** use `/proc/self/fd`, contrary to the note
the LLVM fork's patch carries) that calls `readlink()` on each path prefix and reads
the errno as a fork in the road — `EINVAL` means "not a link, keep walking", any
other errno is fatal. Under `ENOSYS`, **`realpath()` failed on its first component,
for every path on the system, for every ported program.** The truthful `EINVAL`
repairs it whole with no `realpath` patch: on a symlink-free system musl's resolver
degenerates into exactly what realpath should do there — canonicalize `.`, `..` and
duplicate slashes, and verify every component exists. Demonstrated by revert probe:
with the general arm returning `ENOSYS`, `realpath("/proc/./")` fails with errno 38
in-guest (and `realpath("/")` still passes — no components to walk).

**And a kernel gap it surfaced.** `devproc_stat_native` answered `-1` for the
`/proc` apex ("no per-Proc owner"), which `spoor_stat_native` surfaces as `EIO` — so
`stat("/proc")` failed, and `realpath()` on *any* path under `/proc` with it. "No
owner" and "stat fails" are different statements; the apex is a real directory and
now answers as one (SYSTEM-owned, `0555`, the `devdev` `DEV_KIND_ROOT` and devramfs
synth-dir posture). Two siblings in the same family were tracked, not fixed there:
`/ctl` and `/env` have no `stat_native` slot at all, and devproc's per-pid modes
carry no `S_IFDIR`/`S_IFREG` bits — each needs a per-qid posture decision across a
whole Dev, which is a chunk rather than a footnote. That chunk is §6.12.

### 6.12 The synthetic-Dev stat family (V-4b-5)

The three gaps V-4b-4 surfaced, closed together because they are one question
asked of three Devs: *what does this synthetic object claim to be?*

**`/ctl` and `/env` had no `stat_native` slot at all**, so `spoor_stat_native`
returned `-1` → `EIO` for `stat()` on the directory, for `fstat` on any fd beneath
it, for `lseek(SEEK_END)` — and, by the §6.11 mechanism, for `realpath()` of
anything underneath. Both now answer. The interesting part is that they answer
*differently about size*, and the difference is the point:

| | `/ctl` leaf | `/env` entry |
|---|---|---|
| content | generated at read time from live state | a stored byte string |
| size | **0** | **real** |

A `/ctl` file's length measured at `stat` is already stale when the read runs, so
a caller that fstat'd, malloc'd, and read exactly that many bytes would truncate a
table that grew in between — which is why Linux reports 0 for `/proc/meminfo` and
readers loop to EOF. An `/env` value does not move unless someone writes it, so
its size is honest, and `SEEK_END` (the same call behind a different syscall)
lands on it. devhw and devpci already reported real sizes and were already right:
a DTB property and a PCI config register do not change between stat and read.
"Report the size" and "report 0" are both correct answers to different questions.

**devproc's modes carried no file-type bits.** `S_ISDIR`/`S_ISREG` read the
`S_IFMT` field *alone*, so a bare `0555` left a pid directory typeless, and every
POSIX walker that classifies before descending — `find`, `nftw`, `du`, a shell
glob, Go's `os.FileInfo.IsDir`, and the readdir-then-stat loop any `/proc` reader
is built from — read it as not-a-directory and stopped. `qid_type` had carried the
distinction for Thylacine-native callers all along; the type bits carry it for
POSIX ones, emitted from the same switch so the two cannot drift.

**And a fourth, found while writing the prover's regression rather than while
designing the fix**: devproc's `parse_decimal` accepted **leading zeros**, so
`/proc/7`, `/proc/07`, and `/proc/00000007` all resolved to pid 7. One Proc
answering to unboundedly many names is not a curiosity — it breaks injectivity, so
readdir lists a name that is not *the* name, a path-keyed cache holds N entries for
one Proc, and **native `/proc` disagrees with the diorama about which paths
exist** (the diorama's `parse_pid` already rejected them). Coherence between the
two renderings is the entire point of §6.2, so the divergence mattered more than
the leniency did. Linux rejects them for the same reason
(`fs/proc/base.c::name_to_int`); `"0"` alone stays legal, because kproc is pid 0.

**And a fifth, caught by self-audit before it shipped — the one that mattered
most.** Reporting a size for `/env` entries has a consequence beyond `fstat`:
`exec_resolve_from_namespace` gates only on `dev->read` and a non-zero `st.size`,
so a real size makes `exec("/env/FOO")` reach the REVENANT Image cache — which is
keyed on `(dc, devno, qid_path, …)`. Every *other* Dev's qid namespace is global,
so a static `devno == 0` still leaves that pair unique; **devenv's is per-Proc**
(`next_id` restarts at 1 in every `Env`), so two Procs' unrelated variables both
reported `(0, 1)`. The cache would then serve one Proc the text of another's
variable — an I-1 leak out of the one device whose entire premise is that a Proc
sees only its own environment. `struct Env` now carries a `devno` minted at
`env_alloc`, stamped onto the walked Spoor by `devenv_walk` (it has to land there:
`spoor_stat_native` overwrites `out->devno` with the Spoor's own). The lesson is
narrow and worth keeping: **reporting a field that was previously never reported
is a claim, and the claim has to be true before the report is added** — the
identity was equally wrong before, but nothing had asked.

None of this is an authority change: all three Devs run `perm_enforced == false`,
and every `perm_check` site in `stalk`/`syscall.c` is gated on that flag, so no
gate consults these modes. What a mode does here is *document* the gate that lives
at the read site — which is why `/ctl/kernel-base` is `0400` rather than `0444`
(`CAP_HOSTOWNER`, #57a F1), and why `/env` reports the **calling** Proc as owner
(every devenv op resolves the caller's own env, so that is simply true).

### 6.13 The environment, and the first gated proxy (V-4b-6)

`/proc/self/environ` needed a kernel source, as §6.10 predicted. What §6.10 did
*not* predict is that it would be the first file in this arc where the diorama's
own authority and its client's differ — and that turns out to be the interesting
half.

**The source.** `/proc/<pid>/environ` renders the per-Proc `Env` as Linux's flat
`NAME=VALUE\0` block. The renderer lives in `env.c`, where the lock discipline
already is (the `territory_format_ns` shape), and devproc calls it.

It is **offset-aware**, unlike every other `/proc` file, which formats into a
2 KiB `DEVPROC_READ_BUF` and slices. That would not do here: an `Env` holds up to
64 values of 4096 bytes, and a single long `PATH` overflows the buffer on its own.
The failure mode of a format-and-slice render is *silently dropping environment
variables* — which does not look like an error to the consumer, it looks like the
variable was never set, and sends it down a different path with nothing to notice.
So the render walks records, skips wholly-before-window ones in O(1), and copies
only `[off, off+n)`. One *call* is still clamped (8 KiB, because the copy runs
with IRQs off under `g_proc_table_lock`), but a clamp is a **short read**, which
is POSIX-legal and which every `/proc` consumer already loops through — the file
itself is unbounded.

**Entries the Linux shape cannot carry are skipped, not mangled.** Linux's
environment is a `char*[]` of NUL-terminated `NAME=VALUE` strings, so the encoding
cannot carry a `'='` inside a NAME (the first one *is* the separator) or a NUL
inside a VALUE (the NUL *is* the terminator). Thylacine's `/env` is looser —
`name_valid` rejects only NUL and `/`. Emitting such an entry raw would not
truncate the answer, it would **corrupt** it: a `'='` in the name makes one
variable parse as a prefix of another's value, and a NUL in the value splits the
record so its tail parses as a variable that was never set. Both hand a consumer a
wrong value it has no way to distrust, where absence is a state every `getenv`
caller already handles. `/env` stays the complete truth; the reformatter says
nothing it cannot say correctly.

**The gate, and why this file is 0400.** `exe`/`cwd`/`ns`/`maps` are `0444`
all-pids-visible on the argument that they disclose nothing a peer could not
already read (`/proc/<pid>/ns` strictly dominates any of them). That argument does
not extend here: `/env` resolves `current_thread()->proc->env` **by
construction**, so *nothing today lets one Proc read another's environment*, and
environment variables are where secrets live by universal convention. So this file
is a genuinely new cross-Proc disclosure and carries a real owner-or-`CAP_HOSTOWNER`
check at the read site — the same policy `sched` uses (extracted into one
`devproc_owner_or_hostowner` so the two cannot drift), and the same posture Linux
itself takes (`0400` plus `ptrace_may_access`). `CAP_DEBUG` is deliberately not an
axis: this is an info file, and a debugger's authority to stop and single-step a
Proc is a different grant from a reader's authority to see its internals.

**The self-audit find: a gate cuts both ways, and the second way is the leak.**
The gate keys on the *reader*. For a 9P server proxying the file, the reader is
the **server**, not its client — and the obvious consequence (the SYSTEM boot
diorama is *denied* a user-principal client's environ) is benign: the client loses
a file and gains nothing.

The non-obvious consequence is the opposite one. The diorama runs as
`PRINCIPAL_SYSTEM`, so the kernel would **allow** it to read any SYSTEM Proc's
environ — and it would then hand those bytes to a client of any principal, who
natively would have been denied. `/srv` is the shared immortal boot registry
re-grafted post-pivot, so a logged-in user Proc can mount the diorama; the leak
was reachable, not theoretical. That is exactly the deputy-as-authority failure
§6.2 forbids, and it appeared now rather than at V-4b-1..5 because environ is the
first proxied file whose native gate is anything but "everyone".

The fix keeps the diorama a pure reformatter: **`environ` is served under `/self`
only.** `/self` is sound by construction — the target is the connection's own
peer, so a read the kernel allows is a read of the client's own environment, which
it could have done itself, and a read the kernel denies renders empty. A walk to
`/<pid>/environ` is an honest ENOENT. Replicating the kernel's owner check against
`peer.principal_id` was considered and rejected: it would work, but it turns a
component whose entire design property is *having no policy* into a policy point,
to serve a file no v1.0 consumer reads. Two things would make the per-pid variant
servable, and neither is a change in the diorama — a per-container diorama running
as its container's principal (V-7), where server and client authority coincide by
construction, or MANDATE (I-35), which would let a deputy act with its client's
authority instead of its own.

The generalized lesson, and the one to carry into V-4c and V-7: **before proxying
a file, ask not only "could the client read this natively" but "could the client
read this natively *for this target*" — a deputy with more authority than its
client is as much a §6.2 violation as one that invents an answer.** V-4b-1..5 never
raised it because every file they proxy is world-readable.

### 6.14 `auxv` — weighed, and deliberately not built (V-4b)

§6.10 parked `self/auxv` behind "weigh the value first". Weighed at V-4b close:
**not built**, and recorded here rather than left implicit, because an unbuilt
Tier-2 file that nobody wrote down is exactly the silent omission the
chunk-completeness rule exists to prevent.

**The evidence.** Every `auxv` consumer in the tree reads it *from the stack*:
`getauxval()` (musl saves `libc.auxv` in `__libc_start_main`) for the pouch side,
and a hand-walk of the `_start` frame for `libthyla-rs` and the Go fork's
`sysargs`. Exactly one file in the tree contains the string `/proc/self/auxv` —
SDL2's `SDL_cpuinfo.c` — and both of its readers are compiled out on this target
twice over: `CPU_haveARMSIMD`'s auxv arm sits under `#elif defined(__LINUX__)` in
an `__arm__` chain that aarch64 exits at `!defined(__arm__) → return 0`, and
`readProcAuxvForNeon` is guarded `defined(__arm__) && !defined(HAVE_GETAUXVAL)`
while `usr/ports/sdl2/SDL_config.h` sets `HAVE_GETAUXVAL 1`. The live aarch64 NEON
check is `getauxval(AT_HWCAP)` — the CF-4 A lever, on the stack. So the count of
live `/proc/self/auxv` readers is zero, and the one port that could plausibly have
needed it is already served.

**Why "no consumer today" is a weak argument here, and what the real one is.**
VIVARIUM's premise is *unmodified foreign* binaries, so an in-tree grep is poor
evidence about a compat surface by construction. The argument that actually
carries is structural: **auxv on the stack is a prerequisite of V-7, not an
optional extra.** A Linux ELF bootstraps out of `AT_PHDR`/`AT_PHENT`/`AT_ENTRY`
(and `AT_BASE` for the dynamic case) — `ld.so` and every static CRT read them from
the initial frame — so `viv` cannot launch a foreign binary *at all* without
building one. By the time anything runs under the vivarium it has its auxv.
`/proc/self/auxv` is the fallback for a thread that never received an entry frame:
code `dlopen`'d into a host that owns `_start` (Go's `os_linux.go` fallback exists
for exactly this, and for Android where the file is unreadable anyway), or a
sanitizer runtime initialized off the main path.

**The trigger.** Build it when something needs its auxv without having been given
one — the first `dlopen`-into-a-foreign-host case, or a sanitizer runtime under
V-8. Not before.

**The constraint, if it is ever built:** the source must be a *retained kernel
copy*, which is what Linux does (`mm_struct.saved_auxv`), never a reconstruction.
Most of the eight entries `exec_fill_auxv` writes are recomputable from state the
kernel still holds — `AT_PAGESZ`, `AT_HWCAP` from `g_hw_features.linux_hwcap`,
`AT_VDSO_CLOCK` from the shared page — but `AT_RANDOM` and `AT_PHDR` are per-exec
*pointers into that process's own image and stack*. A recomputed answer for those
is not a stale answer, it is a wrong one, and a consumer dereferences `AT_RANDOM`.
§6.7 already ranks an invented answer as worse than an absent one; this is that
rule with a segfault attached.

### 6.15 `/dev` is not a diorama tree (V-4c, scripture-first)

Ground-truthing Tier 3 before writing it — the discipline §6.7 exists to enforce —
found that **§6's third bullet contradicts §6.1**, and that the contradiction is
not close. This section corrects it. No code was written first; that is the point
of the pattern.

**The collision.** §6.1 makes the diorama read-only — `h_write` returns `E_PERM`
for every file, unconditionally (`server.rs`, the `P9_TWRITE` arm) — and calls
that decision load-bearing, because it "removes most of the surface a `/proc`
would otherwise carry". §6's `/dev` bullet then asks the same server to present
`null`, `zero`, `full`, `random`, `urandom`, `tty`, `pts/`, `ptmx`. But
**`/dev/null` is *defined* by accepting writes.** So is `/dev/tty`, and so is
`/dev/ptmx`. A read-only `/dev/null` is not a compatibility shim for `/dev/null`;
it is a file that silently fails the one operation it exists for.

**And it would be a downgrade, not a win.** Native devdev already implements these
correctly: `devdev_write` consumes for `NULL`/`ZERO`/`RANDOM`/`URANDOM` and
*fails* for `FULL`, which is the right Linux shape (the errno is the generic `-1`
rather than `ENOSPC` — a real but much smaller gap, and one that belongs to
devdev). Routing them through the diorama would take files that work today and
break them. That generalizes past this arc, so it is worth stating as a rule
beside §6.2's:

> **A re-presentation that loses a capability the native tree already has is a
> downgrade wearing a compatibility label.** §6.2 forbids the diorama from
> serving *more* than the native surface would. This is the other edge: do not
> route a file through the diorama that the native tree already serves *better*.

**The composition mechanism already exists, and it is the Thylacine one.** A
container's `/dev` is assembled by **bind**, in the container's own territory —
which is what `viv` does at V-7 and what joey already does at boot. Per-container
divergence needs no server: it is the same property §6 claims for `uname`, gotten
the same way. Interposing a 9P server buys nothing and costs a hop.

**The residue resolves without the diorama too** — each of the entries native
`/dev` lacks lands somewhere that already owns the question:

| Missing entry | Where it belongs | Why |
|---|---|---|
| `/dev/ptmx` | **already done**, in the phenotype (PTY-3) | `0021-pouch-pty.patch` redirects `posix_openpt`/`openpty` to `/dev/pts/ptmx`, and says so in its own words: "`/dev/ptmx` is a compat symlink Thylacine cannot provide". That is §6.11's shape-in-the-phenotype pattern, landed before it was named. |
| `/dev/std{in,out,err}`, `/dev/fd/N` | the phenotype | `open("/dev/fd/N")` is `dup(N)`. No kernel and no server. Honest caveat to record with it: Linux *reopens* the underlying file (fresh flags, independent offset) where `dup` shares the offset — right for the common `cmd < /dev/fd/3` / `cmd > /dev/stderr` uses, and the same simplification the BSDs' fdescfs makes. |
| `/dev/tty` | `viv` (V-7), as a bind | The controlling terminal is already kernel state — `ct_sid` on the pts entry (PTY-1d). A container's `/dev/tty` is a bind of its own pts slave, decided when `viv` builds the territory, not rendered by a server that would have to guess which terminal the *caller* controls. |

**So V-4c's scope is smaller and better than specced**: a minimal `/sys`, and the
per-container mount wiring — which this finding promotes from an afterthought to
the substantive half, since binding is now the answer for `/dev` as well as the
delivery mechanism for `/proc`.

**`/sys`, ground-truthed the same way**, is thin: the entire tree contains exactly
one `/sys` path — SDL2's `SDL_GetCPUCacheLineSize` reading
`/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size` — and it is a
*soft* read: `SDL_CACHELINE_SIZE` (128) is assigned first and the file only
overrides it, so a failed `fopen` is entirely benign. That does not make `/sys`
worthless (a compat surface's consumers are foreign by definition — §6.14's
caveat), but it does mean it is a small, low-risk tree serving `devices/system/cpu/`
topology, not a project.

One mechanism note for whoever builds it, found in the same pass: `/sys` is a
**second tree**, not a subdirectory. The diorama's existing `N_SYS`/`N_SYS_KERNEL`
nodes are `/proc/sys/kernel/…` — a different thing that happens to share a name.
A container mounts `/proc` and `/sys` separately, and 9P's answer for one server
exporting two trees is **`Tattach` with a different `aname`** (Stratum's `ds:<name>`
form is the in-tree precedent). Today `h_attach` **ignores `aname` entirely** and
every attach lands on `N_ROOT`, so the aname dispatch is the actual work — small,
but a real mechanism rather than a table entry, and the per-container mount wiring
wants it regardless, since V-7 will attach twice for every container.

> **CORRECTED at V-4c-1 (see §6.16).** The paragraph above is kept because it is
> half right and the half it gets wrong is instructive: `/sys` *is* a second tree,
> but **the aname dispatch is not the work, and cannot be** — an aname is
> unreachable from EL0 for a 9P-mode `/srv` service. The answer is the one §6.15
> had already chosen one paragraph earlier, for `/dev`: **bind**.

**And the same pass found two Tier-1 stragglers.** §6.3 lists `/proc/cpuinfo` and
`/proc/stat` under *Tier 1 — has a consumer TODAY*, and neither has ever been
served; the V-4a/V-4b sub-chunks quietly carried them as "deferred with their
kernel prerequisites". Ground-truthed now, two things are true at once, and both
matter:

* **The "consumer TODAY" claim does not survive a grep.** The only in-tree
  `/proc/cpuinfo` readers are libsodium's `config.guess` (a *host* build script)
  and SDL2's `__ANDROID__` branch; `/proc/stat` appears only in PROWL-DESIGN.md,
  describing what Linux's htop does. So the tier assignment was made from what
  Linux programs typically read rather than from this tree — the same gap §6.14
  found for `auxv`. (It becomes live the moment a `./configure` runs in-guest,
  which the Clade arc's on-device toolchain makes a real V-8 scenario.)
* **Both are only *partly* sourceable, and the unsourced fields are the
  interesting part.** `cpuinfo`'s processor count comes from `/ctl/cpu` and its
  `Features` line is `AT_HWCAP`, but `CPU implementer`/`part`/`variant`/`revision`
  come from `MIDR_EL1`, which **EL0 cannot read at all** — an EL0 `mrs midr_el1`
  is `snare:ill`, which is the same reason `AT_HWCAP` must never advertise
  `hwcap_CPUID`. `/proc/stat`'s `cpu`/`cpuN` idle columns come from `/ctl/cpu`'s
  per-CPU `idle_ns` and `btime` from `uptime` + `CLOCK_REALTIME`, but `ctxt`,
  `intr` and `processes` have no native source whatsoever.

  So each raises §6.7's question per *field* rather than per *file*, and it has a
  third answer neither §6.7 nor §6.9 covers yet: a Linux consumer parses these
  line-by-line, so **omitting** a line and **fabricating** one are different
  failures, and "report 0" is fabrication with a plausible face. Deciding that —
  omit, or give the kernel a source — is V-4c's real design work on these two,
  and it is deliberately not being decided here alongside the `/dev` correction.

### 6.16 One server, two trees — by bind, not by aname (V-4c-1)

§6.15 left `/sys` needing a **second tree** and named `Tattach` with a different
`aname` as the mechanism. Ground-truthing that before writing it — the §6.7
discipline again, and the second time in two sub-chunks that it has changed the
answer — showed the aname route is not merely harder than assumed but **closed**,
and that the right answer was already sitting in §6.15's own `/dev` finding.

**Why aname is unreachable here.** There are exactly two kernel paths to a
mountable dev9p root, and neither delivers a client-chosen aname to a 9P-mode
service:

| Path | Carries an aname? | Reaches the diorama? |
|---|---|---|
| `open("/srv/x")` → `devsrv_open_connect` | **No** — `kernel/devsrv.c` calls `srvconn_attach_dev9p_root(cn, NULL, 0, …)`, hardcoded empty | Yes — this is how every client reaches it |
| `SYS_ATTACH_9P_SRV(fd, aname, …)` | Yes | **No** — byte-mode-gated, and rejects a 9P-mode conn |

The byte-mode gate is not an oversight to lift: a 9P-mode conn already has a
kernel-owned `p9_client` driving its rings, and a second one over the same rings
would interleave frames. So serving `/sys` by aname would need a *new kernel ABI*
— a syscall issuing an additional `Tattach` on an existing session — which is
exactly the kind of thing this arc should not add without a much better reason
than "the first mechanism that came to mind".

**The answer, which costs nothing.** `SYS_MOUNT` takes **any readable Spoor**,
not only a tree root (`sys_mount_for_proc` gates on `RIGHT_READ` and nothing
else). So the diorama serves ONE tree whose root is the synthetic *world*, with
children named for the mount points Linux expects — `proc` and `sys` — and a
container binds each where it belongs. That is Plan 9's namespace answer, it is
byte-identical to the mechanism §6.15 chose for `/dev` one paragraph earlier,
and it needs no kernel change at all.

Two consequences worth stating, because they are what make this *sound* rather
than merely convenient:

* **The root had to move.** Today's root content is now `/proc/…`. Hanging `sys`
  off a root that IS `/proc` would put a `/proc/sys`-shaped directory into every
  container's namespace that Linux has never had — §6.15's "fabrication with a
  plausible face", one level up from the per-field version. The cost was one
  probe's paths; V-7 does not exist yet, so there was no other consumer.
* **A bound subtree is genuinely sealed.** The obvious worry is `..`: the server
  still records `/sys`'s parent as the world root, so could a container climb
  `/sys/..` into `/proc`? No — `stalk` resolves `..` by **popping its own trail**
  (`kernel/stalk.c`, the `..` arm) and never sends `Twalk("..")` to a server, so
  `<mount>/..` lands on the mount point's parent *in the client's namespace*.
  The server-side parent link is unreachable through a bind. (This is the same
  property that contains `..` at `root_spoor` for I-28; it is doing double duty.)

**What landed.** `/sys/devices/system/cpu/{online,possible,present}` plus one
`cpuN` dir per CPU — all sourced from `/ctl/cpu`, whose `cpus:` header is the
*declared* set and whose `offline` row marker (prowl-5 F2) is exactly Linux's
present-vs-online distinction. That mapping is a gift from devctl having had to
make the same distinction for prowl, and it means both files are sourced rather
than guessed.

**What did not, and why — the same question a third time.** `kernel_max` is
omitted: Linux sources it from a compile-time `NR_CPUS`, and Thylacine's
equivalent (`DTB_MAX_CPUS`) is on no EL0-readable surface. The `cpuN` dirs are
**empty**: their Linux contents (`cache/index0/coherency_line_size`, `topology/`)
are hardware facts read from `CTR_EL0`, which is EL0-trapped exactly as
`MIDR_EL1` is (`SCTLR_EL1.UCT` is clear in `INIT_SCTLR_EL1_MMU_OFF`). So the
per-field question §6.15 raised for `cpuinfo` and `stat` now has a **third**
instance with an identical shape, and all three await **one** decision — omit the
unsourced fields, or give the kernel a source — deliberately made once rather
than piecemeal. That is V-4c-2's design work.

The dirs themselves are not fabrications: each genuinely names a CPU the kernel
reports, and their existence is what the legacy "count the `cpuN` entries"
enumeration path reads (busybox `nproc`, older glibc `_SC_NPROCESSORS_CONF`).
Modern consumers read the range files one level up, which is why those were the
ones worth sourcing first.

### 6.17 The unsourced fields, decided once (V-4c-2)

§6.15 and §6.16 accumulated three instances of one shape — `cpuinfo`'s `MIDR_EL1`,
`stat`'s `ctxt`/`intr`/`processes`, and `cpuN/cache`'s `CTR_EL0` — and deliberately
deferred them to **one** decision rather than three ad-hoc ones. Grepping for the
sources before deciding (§6.7's discipline, and the **third** time in three
sub-chunks that doing so has changed the answer) mostly **dissolves** the fork:
four of the five fields already have a kernel source and need only exposing.

| Linux field | source status (verified) | decision |
|---|---|---|
| `stat: processes` | **already exposed** — `proc_total_created()` (`kernel/proc.c:599`) over a dedicated `u64 g_proc_created`, and already covered by ~10 kernel tests (see the correction below) | **call it** — no kernel code at all |
| `stat: intr` | **exists but PARTIAL** — `kobj_irq_total_fires()` (`kernel/irqfwd.c:124`) counts only IRQs forwarded to a userspace driver; timer, UART and IPIs never reach that hook | **widen, then expose** (see below) |
| `stat: ctxt` | **material exists** — prowl's per-thread `nsched` (`kernel/sched.c:1414`); no global or per-CPU aggregate | **add a per-CPU counter** at the same chokepoint |
| `cpuN/cache/…` | **exists in-kernel** — `CTR_EL0`, read at `arch/arm64/mmu.c:962`/`:982` for the I-cache stride, simply unexposed | **expose** |
| `cpuinfo: Features` | **exists** — `g_hw_features.linux_hwcap` (`arch/arm64/hwfeat.h:52`), already carrying the arm64 *uapi* bit numbers for the exec auxv | **expose** — the AT_HWCAP chunk already paid for this |
| `cpuinfo: implementer/variant/part/revision` | **none** — `grep MIDR` over `kernel/` + `arch/` returns nothing | **read it**, per-CPU at bring-up |
| `cpuinfo: BogoMIPS` | **none, and none possible** | **omit** |

**The rule that covers all seven**, and the reason it is one decision and not
seven: *give the kernel a source, per-CPU, in the kernel's own shape — and omit
only what has no truth to tell.* Per-CPU is not a detail. It is what makes the two
new counters free (each CPU stores to a line it already owns and is already
writing — `sched()` holds `cs`, and `gic_dispatch` runs on the CPU taking the
IRQ), it is how Linux itself accounts both, and it is the only form that stays
correct on a heterogeneous board, where `MIDR_EL1` genuinely differs per core —
which is precisely why Linux prints a per-`processor` block. A boot-CPU-only MIDR
would be wrong exactly where the field earns its keep. (That is the AT_HWCAP row's
recorded seam arriving early, on a value that cannot be papered over with an AND.)

**`intr` is the trap, and it is a shape §6.15 did not anticipate.** A source
*exists*, so the danger is no longer an invented zero but a **real number that
means something narrower than the field it fills** — fabrication with a plausible
face, arriving by the back door. `kobj_irq_total_fires` is truthful about what it
counts; publishing it as `intr` would not be. The fix is not to relabel it but to
count at the **universal** entry: `gic_dispatch` (`arch/arm64/gic.c:703`) is where
*every* INTID arrives before routing. `kobj_irq_total_fires` stays exactly as it
is — the driver-forwarded subset it has always honestly been.

**The sixth instance, which nobody had counted: `stat`'s `cpu`/`cpuN` jiffies
line.** Per-CPU `idle_ns` is sourced (prowl-3b), and `iowait`/`steal`/`guest` are
legitimately zero for us — but **no EL0-vs-EL1 time accounting exists anywhere in
the tree**, so the user/system split has no source and no material. Unlike every
field above, this one cannot be omitted: the columns are positional, so a missing
middle column is not an absent answer but a wrong one. Every available choice is
wrong for a reader who wants the split; only the *shape* of the wrongness is ours
to pick. So: **all non-idle time is reported as `system`, under a stated premise**,
the pattern `maps` already uses for its `/self/exe` substitution. The premise —
"Thylacine does not account user-vs-kernel time separately" — is true, checkable,
and named at the render site, so it is visible rather than hidden, and flagged for
revisit if per-mode accounting ever lands. Utilization (`1 − idle/total`), which is
what essentially every consumer computes, is exactly right either way.

There *is* material for a plausible-looking split — attribute kernel threads'
`run_ns` to `system` and user threads' to `user` — and it is rejected for the same
reason `intr` is: it would be a different quantity wearing the field's name.

**What is omitted, and why each is not a silent omission.** `BogoMIPS` has no
truth to tell (it is a calibration artifact of a loop Thylacine does not run, and
is meaningless on Linux too). `kernel_max` stays omitted per §6.16. `CPU
architecture: 8` is emitted as a constant — the §6.9 category, a declaration about
which ABI the caller sees rather than a measurement of the machine.

**Where the exposures land.** Not in a `/proc`-shaped kernel file — that would be
§6.8's phenotype leaking inward. The per-CPU values (`ctxt`, `intr`, cache line
size, the MIDR quartet) become **columns on the existing `/ctl/cpu` table**, whose
row already *is* the kernel's native per-CPU description. Appending is safe:
prowl's parser (`usr/prowl/src/sample.rs:242`) matches three tokens positionally
and ignores the rest, and an `offline` row stays two tokens and is still skipped —
which is also the right answer for the diorama, since Linux lists only online CPUs
as `cpuN`. The one global scalar (`processes`) has no per-CPU form and no natural
row, so it joins `/ctl/sched`'s global block beside `runnable`.

This adds two counters to two audit-trigger surfaces — the scheduler switch
chokepoint and the GIC dispatch path. Both are read-only telemetry that no
decision consults (prowl's discipline, `PROWL-DESIGN §3.1`), and both land
*before* V-4c-3, which is the arc's owed focused audit and must cover them.

**Correction, found by building it (V-4c-2b).** This section originally named
`g_next_pid − 1` as the `processes` source. That was wrong, and wrong in a way
worth recording: **`proc_total_created()` already existed** (`kernel/proc.c:599`),
over a dedicated `u64 g_proc_created` bumped in the same two places, declared in
`proc.h`, and already asserted by roughly ten kernel tests. It is also strictly
better than the derivation — a `u64` with no `INT_MAX` guard to reason about,
purpose-built for exactly this question rather than a pid allocator repurposed by
arithmetic. So the fifth exposure dissolved to **zero kernel code**: one call.

This is §6.7's own lesson recurring — *grep for an existing accessor before
budgeting* — and the interesting part is how it slipped past a pass that was
explicitly doing that research. The grep went looking for **where the value is
produced** and stopped the moment it found `g_next_pid`; it never asked the second
question, **whether the value is already published**. Producer and accessor are
different searches, and finding the first is what makes you stop doing the second.
The comment naming `proc_total_created` was sitting nine lines above the
`g_next_pid` line that got read.

---

### 6.18 The translation table, tier by tier (V-2a, as-built)

`kernel/vivarium.c` + `<thylacine/vivarium.h>` land the in-kernel half of Option
C. `vivarium_translate(linux_nr, args, out)` is **pure** — no Proc, no uaccess,
no locks, no allocation — and returns `VIV_TRANSLATED` / `VIV_FORWARD` /
`VIV_ENOSYS`. Nothing is wired into `syscall_dispatch`; see "why the table came
first" below.

**Applying §4's rule to the calls a static `hello` makes produced a smaller table
than expected, and that is the result, not a shortfall.** Of nine candidates,
five qualify:

| Linux | tier | why |
|---|---|---|
| `read` 63 → `SYS_READ` 9 | **T1** | args identical in order, width, meaning |
| `write` 64 → `SYS_WRITE` 10 | **T1** | ditto |
| `close` 57 → `SYS_CLOSE` 11 | **T1** | ditto |
| `lseek` 62 → `SYS_LSEEK` 51 | **T1** | `T_SEEK_*` are 0/1/2, so are Linux's `SEEK_*` — the enumerations coincide, so there is nothing to map |
| `exit_group` 94 → `SYS_EXIT_GROUP` 60 | **T1** | args identical |
| `openat` 56 | **T2** (built, §6.19) | Linux passes a NUL-terminated path; `SYS_OPEN` wants an explicit `path_len`, so translating means scanning user memory. Plus `AT_FDCWD` → `SYS_WALK_OPEN_FROM_ROOT` and `O_*` → `omode`. *(V-2a said "still total + stateless"; V-2b found that wrong — see §6.19.)* |
| `fstat` 80 | **T2** (built, §6.19) | `t_stat` is 88 B, Linux aarch64 `struct stat` is 128 B with a different field order: a struct conversion. This one **is** total |
| `newfstatat` 79 | **T2** (built, §6.20) | `stat()`/`lstat()` compile to *this* on aarch64, so it is the row that matters for real binaries. `SYS_STAT` 88 is its exact counterpart |
| `mmap` 222 | FORWARD | addr hints, `PROT_*`, `MAP_FIXED`/`ANONYMOUS`/`PRIVATE`, fd-backing are **policy** |
| `munmap` 215 | FORWARD | **the instructive one — see below** |
| `statx` 291 | FORWARD | musl-aarch64 issues *this* instead of 79 (§6.20). A request mask + a 256 B struct with per-field validity bits — a bigger translator, not this shape |
| `brk` 214 | ENOSYS | no counterpart at all; the heap is Burrow-based. Both musl and glibc fall back to `mmap`, so an honest ENOSYS is serviceable where faking success would strand the allocator |

**`munmap` is why "total" is the word that does the work.** `munmap(addr, len)`
and `SYS_BURROW_DETACH(vaddr, length)` take the same two words in the same order
and read as a free row. They are not equivalent: `burrow_detach` requires an exact
VMA match and explicitly refuses a partial detach (`syscall.h:611-620`), while
Linux permits partial and multi-mapping unmaps. The renumber would be silently
wrong for a legal class of inputs. **Arguments aligning is not semantics
aligning** — every row's equivalence must be checked against the Thylacine side's
documented contract, not its signature. The rejections are therefore stored as an
explicit list with per-entry reasons, not left to fall through a default: a number
we have rejected and one we have never considered are different facts.

**Why the table came before the branch (V-1b).** V-1a landed `Proc.phenotype` but
**nothing can set it to `PHENO_LINUX`** — verified, not assumed: `exec.c` never
touches the field, the only assignment in the tree is the rfork inherit, and
`PHENO_LINUX` appears nowhere outside its own enum. A dispatch branch would today
be branching on a field that is provably always 0 — dead code, and unprovable
end-to-end. Two further reasons make table-first the *proper* order rather than
merely the available one:

- **The declaration's correct shape is not yet knowable.** Every peer system
  except FreeBSD has the **container** declare (illumos LX zones, Starnix,
  gVisor); FreeBSD is the lone inference-based design and also the one whose CVE
  history §4 cites when rejecting Option A. §5.2 already concludes the fused
  container+phenotype object is the right granularity — and that object is V-7.
  Designing a per-spawn declaration ABI now means guessing a granularity the
  research says is probably wrong.
- **Reversibility is asymmetric.** A syscall signature is append-only ABI and
  every caller churns; a table is data and can be rewritten freely. Under genuine
  uncertainty the reversible half goes first.

V-2b (§6.19) promotes `openat` + `fstat` to T2 translators. V-1b remains the
declaration + the branch, and stays gated on V-7's object being decided.

### 6.19 Tier 2 — the translators (V-2b, as-built)

A T1 row is a renumber, so one table and one loop serve every row. A T2 call is a
real translation, so each gets its own named function. They stay **pure**, which is
a constraint rather than a convenience:

- **`vivarium_openat_decide(dirfd, flags, &start_fd, &omode)`** makes the entire
  decision without touching user memory. **`vivarium_openat_build(...)`** assembles
  the call from a `path_len` the *caller* measured. The measurement is hoisted out
  deliberately — see "why decide before measure" below.
- **`vivarium_stat_to_linux(const struct t_stat *, struct viv_linux_stat *)`** is
  data-in/data-out. Its shell (`spoor_stat_native` into a kernel `t_stat`, then one
  128-byte copy-out) touches no translation logic at all.

`struct viv_linux_stat` is the Linux aarch64 `struct stat`, pinned at 128 bytes with
per-field offset asserts exactly as `struct t_stat` is — it is an ABI type the
kernel writes into a guest buffer. The layout was taken from
`third_party/musl/arch/aarch64/bits/stat.h` in-tree, not from memory.

#### The argument domain — a refinement of §4, and the real finding

§4 admits "a pure renumber plus an argument-order/**flag-bit mapping**". A flag map
is inherently **partial**: `openat` accepts flags (`O_CREAT`, `O_DIRECTORY`,
`O_APPEND`) that `SYS_OPEN` has no way to honour. So a T2 row is admitted over a
**stated argument domain**, and a call outside it forwards.

This is not a loosening of "total" — it is *stricter* in practice, because it
replaces "openat is a table row" (which §4's illustrative list implies, and which
V-2a's own note repeated) with a per-call check. The property that matters:

> the translator never silently mistranslates; it either produces an
> exactly-equivalent call or declines.

Declining is always safe — the supervisor is strictly more capable. Accepting a flag
we cannot honour is the failure mode the whole tier exists to prevent.

**A flag may be ignored only when it requests behaviour we already provide
unconditionally.** Three qualify, and each rests on a structural fact rather than on
"nothing seems to break":

| flag | why ignoring it is *correct*, not merely harmless |
|---|---|
| `O_CLOEXEC` | Asks that the fd not survive exec. Thylacine has no close-on-exec concept because there is nothing to opt out of: a spawned child "inherits no Spoor handles" (`syscall.h:327`) and `SYS_SPAWN_WITH_FDS` passes an **explicit** list |
| `O_NOCTTY` | Asks not to acquire a controlling terminal. Thylacine acquires one only via the explicit `SYS_TTY_ACQUIRE` (PTY-1), never implicitly on open — already relied on by the pouch pty patch |
| `O_LARGEFILE` | Asks that >2 GiB offsets be permitted. Every Thylacine offset is 64-bit, exactly as on 64-bit Linux, whose kernel force-sets the bit internally |

Contrast the rejects, where ignoring the bit is a **wrong answer**: `O_CREAT` would
turn "create if absent" into `ENOENT`; `O_DIRECTORY` would turn `ENOTDIR` into a
successful open of a regular file; `O_APPEND` would silently corrupt a log writer.
`O_NOFOLLOW` is rejected on a different basis worth naming — ignoring it is harmless
*today* only because the resolver has no symlinks, and would become wrong the moment
they land with nothing to catch it. **A flag whose correctness depends on a feature
being absent is a trap, not an admission.**

Two further domain notes:

- **`AT_FDCWD` ↔ `SYS_WALK_OPEN_FROM_ROOT` is exact, not approximate.** `SYS_OPEN`
  with the sentinel joins a *relative* path against the per-Proc cwd (LS-4) and
  resolves an *absolute* one from the Territory root — precisely `AT_FDCWD`. Both
  sides make the absolute/relative split identically, so nothing needs inspecting.
  It is compared as a **signed 32-bit** value: `dirfd` is an `int`, so x0 may arrive
  sign-extended *or* merely zero-extended, and both mean −100. Recognising only one
  would work on some toolchains and silently forward every `open()` on others.
- **A real dirfd forwards at V-2b.** Not because the fd would not carry (a
  phenotyped Proc's fds *are* Thylacine handles) but because Linux ignores the
  dirfd for an absolute path, and deciding that needs the path's first byte out of
  user memory. `open()` never generates it — musl compiles every `open()` to
  `AT_FDCWD`; only the `*at()` family passes a real fd. V-2c can revisit it with the
  path already measured.

#### Why decide before measure

Measuring the path is a user-memory read that can fault. Doing it before knowing
whether the call is translatable would waste the read on every forwarded call *and*
let a call we are going to hand to the supervisor anyway take a fault inside the
kernel fast path, on a buffer the supervisor would have validated itself. The API is
shaped so the wrong order is awkward.

#### Notes on the stat conversion

`st_dev ← t_stat.devno` and `st_ino ← t_stat.qid_path`: the `(devno, qid.path)` pair
*is* Thylacine's file identity (#100), and is already the pair userspace maps onto
`(st_dev, st_ino)` — pouch patch `0010` does exactly this and gopls's robustio keys
`FileID` on it. The correspondence is inherited, not invented here.

The output is **zeroed wholesale before filling**, by byte loop (the kernel links no
`memset` — `dev9p.c:701` does the same for the same reason). That is an **I-13**
obligation: the buffer is copied to a guest, so any word left unwritten — a reserved
field today, a field added tomorrow — would ship a slice of the kernel stack. The
test poisons the destination with `0xA5` and asserts **not one of the 128 bytes
survives**, which catches a future field addition that the per-field asserts would
not. The nsec words stay 0: `t_stat` carries whole seconds only, and 0 is the honest
"unknown sub-second" the native surface already gives.

Coverage: `vivarium.openat_domain` (every admission and every reject, by name),
`openat_at_fdcwd` (both encodings; a real dirfd forwards; the high half of `dirfd`
is not consulted), `openat_build` (argument order; unused words zeroed),
`stat_to_linux` (field carry + the no-leak sweep). Revert-probed: comparing the raw
`u64` for `AT_FDCWD` fails `openat_at_fdcwd`, and dropping the zero loop fails
`stat_to_linux` — 1225/1227, exactly two tests, no collateral.

#### Still deliberately absent

The **impure shells** — the uaccess path scan for `openat`, and
`spoor_stat_native` + the 128-byte copy-out for `fstat` — are *not* built here.
They have no caller until V-1b's dispatch branch exists, and writing an
unreachable uaccess helper would repeat exactly the mistake V-2a declined to make.
There is likewise **no `docs/reference/NN-vivarium.md` yet**: a reference doc
describes as-built runtime behaviour, and this surface has none until it is
reachable. It lands with the dispatcher.

> **Both landed at V-1b, as promised.** The shells are `viv_tier2` /
> `viv_measure_user_path` / `viv_stat_copy_out` in `kernel/syscall.c` — kept
> there, not in `vivarium.c`, so the pure halves stay unit-testable with no
> kernel plumbing. The reference doc is `docs/reference/145-vivarium.md`.

Named V-2c candidates, in rough order of value: `O_CREAT` routed to
`SYS_WALK_CREATE` (a second target, not a flag map — task #50 tracks the userspace
half); a real dirfd, decidable once the path is measured; and `newfstatat` 79,
which is `fstat`'s path-taking sibling and reuses both translators as-is.

> **V-2c checked all three, and two of these three sentences were wrong.**
> `newfstatat` was the good call and is built (§6.20). `O_CREAT` is **not
> admissible at all**, and the real-dirfd blocker is not the path measurement.
> The corrected analysis is §6.20; this paragraph is left standing because the
> arc's pattern — each chunk's candidate list corrected by the next chunk's
> ground truth — is itself the record worth keeping.

### 6.20 Tier 2 — `newfstatat`, and two corrections (V-2c, as-built)

**`newfstatat` 79 is the stat row that matters.** There is no `stat(2)` on
aarch64: `stat()` and `lstat()` both compile to `newfstatat`. `SYS_STAT`
(`syscall.h:1603`) is its counterpart, and the correspondence is unusually clean —
`SYS_STAT` takes `(path_va, path_len, stat_va)` and **no base argument at all**,
because it is hardcoded to "absolute from the Territory root, relative joined with
the LS-4 cwd". That is precisely the `AT_FDCWD` rule, and it is not two
implementations that happen to agree: `sys_stat_for_proc` and `sys_open_handler`
call the same `territory_join_cwd` (renamed from `territory_resolve_cwd` at #83,
when the join stopped resolving dots -- see docs/reference/104-stalk.md).

**`vivarium_fstatat_decide(dirfd, flags)` returns a verdict and nothing else**, and
that emptiness is the finding. `openat` had to compute a rewritten `start_fd`
because `SYS_OPEN` *takes* one; here there is nothing to rewrite. The consequence
cuts both ways: `AT_FDCWD` is free, and a real dirfd is not merely unimplemented
but **inexpressible** — there is no argument to put it in.

**There is no `vivarium_fstatat_build`,** and its absence is structural rather than
an omission. `openat` gets a build function because its translation ends in a
native `SYS_OPEN` the dispatcher can run. This one cannot: `SYS_STAT` copies out an
88-byte `t_stat`, and the guest's buffer wants the 128-byte Linux layout, so
dispatching `SYS_STAT` at the guest's pointer would write the wrong struct into it.
The shell must call `sys_stat_for_proc` into a *kernel* `t_stat`, run it through
`vivarium_stat_to_linux`, and copy out 128 bytes. **`newfstatat` is `openat`'s
front half joined to `fstat`'s back half, and the missing build function is what
that join looks like.**

The flag domain is one admission and four rejects:

| flag | verdict | why |
|---|---|---|
| `0` | admit | plain `stat()` |
| `AT_NO_AUTOMOUNT` | admit as a **no-op** | a Thylacine namespace is composed *explicitly*; nothing mounts as a side effect of traversal. That is a property of the Plan 9 model, not a v1.0 gap — there is no automount to defer, ever |
| `AT_SYMLINK_NOFOLLOW` | **reject** | the costly one — see below |
| `AT_EMPTY_PATH` | reject | means "operate on `dirfd` itself"; serving it would mean **synthesising** a `"."` the caller never passed. Translating maps what you were given, not what you were not |
| `AT_REMOVEDIR`, `AT_SYMLINK_FOLLOW` | reject | not valid on `fstatat` (they belong to `unlinkat`/`linkat`); Linux answers `EINVAL`. Forwarded rather than errored — minting errors is not the table's job, the same call `openat` makes for `(flags & O_ACCMODE) == 3` |

**Why `AT_SYMLINK_NOFOLLOW` is rejected even though the kernel says it is safe.**
This is what `lstat()` compiles to, so rejecting it forwards every `lstat` — real
lost reach, so the reasoning has to hold. `SYS_STAT`'s own contract says *"Symlinks
do not exist at v1.0 (G11), so stat == lstat"* (`syscall.h:1615`), which reads like
a licence to admit it. It is not. That equivalence is scoped to v1.0 and holds
**only because the feature is absent** — the O_NOFOLLOW trap V-2b named, on the
stat surface. Admitting it would mean that the day symlinks land, every `lstat()`
in every Linux guest silently reports the *target* instead of the link, with
nothing in this file or in any build that would fail. Forwarding costs a supervisor
round trip; admitting costs correctness later, silently. **A flag whose correctness
depends on a feature being absent is a trap, not an admission** — and the rule is
worth more than the reach it costs here.

#### `statx` is why 79 is not the whole stat story

musl on aarch64 defines no `__NR_fstatat` (only `__NR_newfstatat`), so `SYS_fstatat`
is undefined, so its `fstatat.c` compiles the 79 path **out** and issues `statx`
(291) instead — verified in `third_party/musl`, not assumed. Go and glibc *do* use
79. Those are the binaries VIVARIUM exists to run: a musl target is one we could
rebuild through pouch, so 79 is still the right row to build first, and `statx` is
recorded as a deliberate FORWARD rather than an oversight.

#### Correction 1 — `O_CREAT` cannot be routed to `SYS_WALK_CREATE`

V-2b filed this as V-2c's top candidate ("a second target, not a flag map"). It is
not admissible at all. **Three independent blockers, any one fatal:**

1. **Shape.** `SYS_WALK_CREATE` takes a **single component** name and rejects `/`
   (`syscall.h:1105`); `openat` takes a path. Routing means splitting the path,
   resolving the parent as a separate `O_PATH` open, and closing that handle on
   every exit — two syscalls and an intermediate handle, i.e. exactly the state and
   logic §4 excludes.
2. **Semantics.** Plain `O_CREAT` (no `O_EXCL`) means "create if absent, **open** if
   present". `SYS_WALK_CREATE` always creates, returning `-EEXIST` otherwise. A
   try-create-then-open retry is control flow, not a mapping.
3. **The sharpest, because it is silent.** `SYS_WALK_CREATE`'s `FROM_ROOT` sentinel
   resolves at the caller's Territory **root** (`syscall.c:2968`, no cwd join),
   while `SYS_OPEN`'s identical-looking sentinel joins a relative path against the
   LS-4 **cwd** first (`syscall.c:2870`). The "obvious" `AT_FDCWD` mapping would
   therefore create the file in the **wrong directory** whenever `cwd != "/"` —
   wrong for a legal class of inputs, with no error. That is the `munmap` failure
   mode precisely: *two sentinels that look identical and are not.*

There is no create-by-path syscall in the tree (the only other cwd-joining site,
`exec_resolve_from_namespace`, resolves a binary to exec). Task #50 tracks the
userspace half; the kernel half wants a syscall that does not exist.

#### Correction 2 — a real dirfd is blocked by handle *state*, not by the path

V-2b said the blocker was that Linux ignores `dirfd` for an absolute path, so
deciding needs the path's first byte, and "V-2c can revisit it with the path
already measured". Measuring would not have helped: it leaves the **relative** case
untouched, and there the blocker is handle state, which §4 excludes outright.

A Linux dirfd comes from `open(dir, O_RDONLY|O_DIRECTORY)` — a **normally-opened**
handle. *"9P forbids Twalk from an OPENED fid, so a normally-opened handle is NOT a
valid base for … walking … CHILDREN; an O_PATH handle IS"* (`syscall.h:2370`). So
the dirfd Linux programs actually produce is not a usable `SYS_OPEN` `start_fd`;
only an `O_PATH` one is. The failure is loud (a walk error, not corruption) but
still wrong for the common legal input — and telling the two handles apart means
reading the handle table. Left as a FORWARD: the supervisor holds the process's fd
view anyway and is the right place to resolve one.

Coverage: `vivarium.fstatat_domain` (the admission, all four rejects by name, an
unadmitted bit forwarding *beside* an admitted one, both `AT_FDCWD` encodings, a
real dirfd, the high half of `dirfd` unread), plus the `newfstatat`-is-T2 and
`statx`-forwards rows in `rejects_are_deliberate`. Revert-probed with two probes in
one build: admitting `AT_SYMLINK_NOFOLLOW` fails `fstatat_domain`, and flipping the
79 row to FORWARD fails `rejects_are_deliberate` — **1226/1228, exactly two tests,
no collateral.**

### 6.21 Tier 2 — `mmap`, `munmap`, and the protection question (V-2d, design)

§4.1 defers V-3 and thereby makes `FORWARD` mean `ENOSYS`, so `mmap` — which
V-2a classified FORWARD as "the 'needs judgement' case the rule exists to
exclude" — stops being a slow path and becomes a wall. It is on musl's critical
path twice over (`__init_tls.c:137` for TLS, mallocng for every heap area), so a
Linux guest cannot reach `main` without it. V-2b's argument-domain rule is
exactly the tool: `openat` was promoted the same way, and `vivarium.c` already
carries that correction of a V-2a FORWARD.

**`mmap` (222) → `SYS_BURROW_ATTACH_LAZY` (83).** The target takes a length and
returns a kernel-chosen vaddr. The admitted domain:

| argument | admitted | why |
|---|---|---|
| `addr` | **any** | Without `MAP_FIXED`, Linux states `addr` is *a hint* the kernel may ignore; the caller learns the real address from the return value. Ignoring it is conforming, not a compromise |
| `len` | not judged in `_decide` at all | it is a *semantic* question, not a domain one. The shell answers `EINVAL` for 0 (Linux's own answer) and lets the target refuse the rest, which becomes `ENOMEM` — so both of Linux's errors survive instead of collapsing into the decline. The lazy bound is `BURROW_RESERVE_MAX` (1 GiB), not `BURROW_ATTACH_MAX` |
| `prot` | any subset of `PROT_READ\|PROT_WRITE` (so `PROT_NONE` too) | see below. Measured: aarch64 musl also defines `PROT_BTI`/`PROT_MTE`, and generic musl `PROT_GROWSDOWN`/`GROWSUP` — none honourable, so the admission is an allow-list of the two bits, not "everything but `PROT_EXEC`" |
| `flags` | exactly `MAP_PRIVATE\|MAP_ANONYMOUS` | measured: both musl `pthread_create` sites and all four mallocng sites pass exactly this. `MAP_STACK`/`MAP_NORESERVE` are *not* passed by musl, so admitting them would be speculation |
| `fd` | `-1` | Linux ignores `fd` under `MAP_ANONYMOUS`; requiring `-1` is conforming and is what musl and glibc emit |
| `offset` | `0` | anonymous |

`MAP_FIXED` and `MAP_FIXED_NOREPLACE` decline: there they are a requirement, not
a hint, and the target chooses the address. `MAP_SHARED` declines — no shared
anonymous memory exists to map.

**The protection question, decided explicitly.** Thylacine anonymous memory is
always RW/XN, and **there is no prot-mutation syscall at all** — that is an I-12
design choice, not a gap. So a phenotyped `mmap` cannot honour `PROT_NONE` or
`PROT_READ` exactly; it grants read+write regardless. Two options, and the strict
one loses:

- Declining every prot but `PROT_READ|PROT_WRITE` is the letter of §6.19's "never
  silently mistranslate". But `PROT_NONE` is the *dominant* anonymous shape in
  musl — the guard page (`pthread_create.c:295`) and mallocng's meta areas
  (`malloc.c:82`) — so declining it means malloc never initialises and nothing
  runs at all.
- Admitting it is a **stated fidelity degradation**, and musl itself is the
  evidence that it is the sanctioned one: `mallocng/malloc.c:92` reads
  `if (mprotect(p, pagesize, PROT_READ|PROT_WRITE) && errno != ENOSYS) return 0;`
  — the libc *anticipates* a system without `mprotect` and proceeds on the
  assumption that the `PROT_NONE` mapping is already usable, which is precisely
  what Thylacine produces.

So: **any subset of `PROT_READ|PROT_WRITE` is admitted and yields RW.** The consequence
is named rather than buried — *guard pages are not protective under the Linux
phenotype, and a `PROT_READ` anonymous mapping is writable* — and it belongs in
§9's ladder. It is a fidelity loss, never an authority grant: the pages are the
guest's own, every gate is unchanged, and nothing crosses a Proc boundary, so
I-43 is untouched.

`PROT_EXEC` is the hard line and declines. An executable anonymous mapping is
what `CAP_JIT`/I-42 governs (`JIT-ON-WX-DESIGN.md`); a phenotype must never hand
one out, and W^X (I-12) forbids the RW-and-X mapping the naive translation would
produce.

**`mprotect` (226) becomes an explicit `ENOSYS` row.** It is currently unclassified,
so it reaches `ENOSYS` through `vivarium_translate`'s default — the right answer
by accident. The file's own standard is that "a number we have never considered
and one we have considered and rejected are different facts", so it is recorded
with its reason: musl tolerates `ENOSYS` here by construction, and Thylacine has
no prot-mutation syscall to translate to.

**`munmap` (215) → `SYS_BURROW_DETACH` (38), over a domain the arguments cannot
express.** V-2a's rejection stands on its facts: detach demands an exact VMA
match while Linux permits partial and multi-mapping unmaps, and — the part that
makes a bare renumber worse than merely incomplete — *Linux `munmap` of an
unmapped range succeeds*, while detach returns `-1`. So the translation is wrong
in both directions.

This is the first T2 row whose domain is a question about **state**, not about
arguments, so it has no pure `_decide` at all — no pure function can know whether
a VMA matches. The resolution is that it needs none: **`sys_burrow_detach_for_proc`
already enforces the exact match**, so the shell simply attempts it and reads the
answer. Success means the semantics were exactly Linux's; refusal means the call
was outside the domain, and declines. Nothing re-derives the matching logic,
there is no second lookup and therefore no window to race, and the row stays a
decode rather than a second implementation.

The one divergence is named rather than papered over: Linux `munmap` of an
**unmapped** range *succeeds*, and this declines it. Distinguishing "nothing
there" from "a partial overlap" needs a range scan the VMA API does not expose
(`vma_lookup` is a point probe, so it cannot see a VMA lying strictly inside the
range), and the two must not be conflated — claiming success on a partial
overlap would leave a mapping the guest believes is gone, which is exactly the
silent wrong answer the rule forbids. Declining is honest; faking success is not.

The exact-match subset is the one that matters: a program unmaps what it mapped,
and mallocng frees whole groups it allocated whole. It is also not load-bearing
for liveness — measured, mallocng **ignores `munmap`'s return** at both of its
call sites (`free.c:148`, `malloc.c:318`), so a declined unmap costs memory, not
correctness.

Coverage: `vivarium.mmap_domain` (each admitted argument and each decline by
name, `PROT_EXEC` especially, both `MAP_FIXED` spellings), the `mprotect`-ENOSYS
and `mmap`/`munmap`-are-T2 rows in `rejects_are_deliberate`, and `viv-pheno-probe`
legs **L16–L23**: mmap 8 KiB, write a pattern through both ends of the mapping,
read it back, unmap it exactly, then assert in-guest that `PROT_EXEC` and
`MAP_FIXED` are refused, that `mprotect` answers `ENOSYS` *specifically* (musl
depends on that errno, not merely on failure), and — deliberately — that a
`PROT_NONE` mapping is writable, so the degradation cannot go stale unnoticed.

The two layers are not redundant, which V-2d's revert probes demonstrate rather
than assert. Reverting the `mmap` table row, or widening the prot allow-list to
admit `PROT_EXEC`, fails **exactly two** unit tests and nothing else. But
breaking only the *shell* — leaving the table intact — keeps the unit suite at a
full **1239/1239 PASS** while the in-guest leg fails at `L16`. The pure tests
prove the decision; the guest legs prove the plumbing.

`thylacine-run` from `ROADMAP §9.1`, named `viv` (§11). Userspace; no new kernel
surface beyond §4's.

1. Fetch/unpack an OCI image (layers → a Stratum dataset; the reflink/snapshot
   machinery makes layering natural). *v1.0 realization: §7.2 — `viv` consumes a
   pre-assembled bundle; image acquisition is the v1.x sibling tool.*
2. Build the territory: the image root as `/`, the diorama mounts, `/net` if the
   manifest grants it, the resource floor (I-32) and hardware allowance (I-34).
3. Set the phenotype (§5.2). *Lands at V-1b per §10's corrected sequencing; a
   V-7 container spawns `PHENO_NATIVE`.*
4. Spawn the entrypoint via the #58 namespace-exec path.

"No cgroups, no seccomp at v1.0; territory isolation is the boundary" (ROADMAP
§9.1) — which is exactly right, because I-32/I-34 already provide the resource and
hardware bounds cgroups/seccomp were retrofitted onto Linux to provide.

### 7.1 Owed at V-7 — pid visibility (surfaced by V-4b-3)

V-4b-3 gave the diorama the numeric `/proc/<pid>` dirs and a root enumeration, and
that is the first time it answers about a Proc other than its caller. The
containment question this raises belongs here rather than in §6, because it is
**not a diorama question**:

- The five per-pid files are `0444` with `devproc.perm_enforced == false` — Plan 9's
  all-pids-visible posture — and `/ctl/procs` lists every Proc on the box. So the
  diorama serves exactly what native `/proc` serves, to exactly the same readers.
  §6.2 holds: no new authority.
- But a *contained* Proc seeing every host pid is a leak across the container
  boundary, and **that leak is in native `/proc` and `/ctl/procs` first.** Scoping
  the diorama alone would be theatre — a contained Proc that can reach native
  `/proc` reads around it.

So: when V-7 builds a container territory, "which pids does it see" must be decided
for the **native** surface (a per-territory pid view, or simply not mounting `/proc`
and `/ctl` into the container). Deciding it in the diorama alone would invert the
design.

**Correction (V-4c-3 F6).** An earlier draft of this section closed by saying the
diorama "then inherits the answer for free, because it only ever re-presents what it
can itself read." That clause is **wrong**, and it is wrong in exactly the way §6.13
warned about one section earlier.

"What it can itself read" is not "what its **client** can read". Every native source
in the server — `native_pid_exists`, `native_pid_list`, `read_ctl_cpu`, and the
per-pid path builder — resolves with `T_WALK_OPEN_FROM_ROOT` against the
**diorama's own** SYSTEM territory, not the caller's. So *withholding* `/proc` and
`/ctl` from a container's territory does **not** withhold them from the container:
the diorama still reads them, and still reformats them across the 9P boundary. The
proposed remedy closes the native path and leaves the diorama standing as a read
oracle for the whole surface it proxies — not merely as a pid enumerator.

Nothing is exploitable today: no restricted territory exists before V-7, a
logged-in Proc already has `/proc` and `/ctl` mounted, and all five per-pid files
are `0444` ungated. This is a scripture correction plus a **V-7 obligation**, not a
live defect.

The two remedies that actually work are the ones §6.13 already named, and V-7 must
pick one: a **per-container diorama** running as its container's principal (server
and client authority coincide by construction — the same argument that makes
`/self/environ` sound), or **MANDATE (I-35)**, which would let a deputy act with its
client's authority rather than its own. The general lesson is §6.13's, restated at
the level of the whole tree rather than one file: **a deputy's territory is part of
its authority, and confining the client without confining the deputy confines
nothing.**

**RESOLVED (2026-07-30): the per-container diorama — see §7.2.** MANDATE stays the
general deputy answer and stays RESERVED at Phase 8; V-7 does not pull it forward.

### 7.2 The v1.0 realization — bundle-consumer `viv` (user-voted 2026-07-30)

**`viv` is the runtime half of the OCI split, and only that.** The OCI ecosystem
itself factors "run a container" into two tools: the *runtime* (runc) consumes a
pre-assembled **bundle** — a directory holding `rootfs/` plus `config.json` — and
the *image* tooling (umoci, skopeo) turns registry images into bundles. Adopting
the same seam means step 1's fetch/unpack half is not a silently dropped
obligation but a separately-owned sibling: at v1.0 the bundle is **host-baked**
(the 16c host-bake precedent — `tools/build.sh` unpacks an Alpine ARM64 rootfs
into a pool dataset at build time, using host-side tar+gzip), and the in-guest
image tool (`viv pull`: registry, TLS, JSON manifests, layer tar.gz →
per-layer datasets with the reflink layering) is a **named v1.x seam**. The tree
has no native tar reader and no inflate today (verified 2026-07-30), and
building them buys no v1.0 fidelity the baked bundle does not already deliver;
§9's OUT list carries the entry.

**The manifest is a `config.json` subset** (OCI runtime-spec shaped, so a future
`viv pull`'s output composes without translation): v1.0 reads `root.path`,
`process.args`, `process.env`, `process.cwd`, and the annotation
`org.thylacine.net` (`"granted"` mounts `/net`). Unknown fields are ignored;
missing required fields fail closed. The phenotype declaration (§12.1 rule 1)
rides this same manifest once V-1b lands the kernel side.

**The territory recipe** — assembled by `viv`; the container inherits nothing
the manifest does not name:

| Mount | Source | Why |
|---|---|---|
| `/` | the bundle's rootfs dataset | the image root IS the world (step 2) |
| `/proc`, `/sys` | binds of the **per-container diorama**'s world (the §6.16 one-server-two-trees-by-bind composition, proven at V-4c-1) | §7.1's resolution, below |
| `/dev` | assembled **by bind** per §6's finding (the trivial devdev leaves; `/dev/tty` = a bind of the pts slave `viv`'s session controls, per its §6 row; omitted when `viv` has none) | binding is the answer for `/dev`; no server interposed |
| `/env` | a bind of the kernel env device (as-built completion, V-7) | zero-authority substrate: devenv resolves the **calling** Proc's own `Env` at op time, so one bind serves every container Proc its own environment (native binaries read env through `/env`); carries nothing across the boundary |
| `/net` | only if the manifest grants it | I-1: the namespace firewall is the grant |

Native `/proc` and `/ctl` are **not mounted** into the container — a
per-territory pid *view* would be a new kernel surface §7 forbids, and
not-mounting is the sound native half of §7.1 once the deputy is confined too.
The container is I-32 resource-floored and I-34-narrowed (no hardware allowance
conferred).

**§7.1's resolution, precisely.** Each `viv run` spawns its own diorama
instance, and two distinct properties do two distinct jobs — stated separately
because `devproc.perm_enforced == false` means principal alignment alone scopes
*nothing* about pid reads:

- **Pid scoping is a container-tree filter on the diorama's export.** The
  per-container diorama holds its native sources (`/proc`, `/ctl`) in its own
  territory, *unreachable by the container* (the container speaks only 9P to
  it); both enumeration and per-pid existence answer only for pids in the
  container's process tree — membership by ppid-descent from the entrypoint
  (the `/ctl/procs` PPID column). A pid outside the tree does not resolve, so
  the diorama cannot be a read oracle for the surface the native mounts
  withheld (the F6 close). Pids are **host-numbered** — consistent with what
  `getpid()` returns inside the container, which is what `busybox ps`
  correlates; a virtualized pid-1 namespace is a named v1.x seam.
- **The principal is aligned** — the diorama runs as the container's principal,
  which keeps the `/self/*` files sound by the `/self/environ`
  authority-coincidence argument, and is defense-in-depth for everything else.
  *As-built precision (V-7 self-audit):* soundness here is an **authority**
  claim and holds; the `self` **content** answers the connection's peer, and
  the container's one connection was opened by `viv` — so `/proc/self/*` read
  by a container Proc reports the *runner*, not the reader (9P carries no
  per-op caller identity; the kernel client multiplexes every territory member
  over the one session). Same-principal data, never a cross-boundary leak, but
  wrong content for a multi-Proc container's self-readers — task #90; weighed
  at V-2/V-8 (a per-op identity channel would be a new kernel surface).

**The container principal is the invoker's** at v1.0. A container is a
namespace-confined process tree of the user who ran `viv`, not a new identity —
no new principal machinery, and the per-container diorama inherits the right
principal by plain spawn (no `CAP_SET_IDENTITY` anywhere in the path). A
fresh-principal-per-container (stronger user↔container isolation) is a v1.x
upgrade riding A-5's identity machinery.

**The V-7 gate is a native prover** (`usr/viv-probe`, spawned inside a
viv-assembled container): the bundle rootfs is `/` (its tree resolves; host
paths do not); `/proc` and `/sys` serve from the per-container diorama; host
pids outside the container tree neither enumerate nor resolve; host `/srv`
names are unreachable; `/net` is absent unless granted; the I-32 floor holds.
"An Alpine shell runs" (§10's V-7 row) is the **arc** gate — it additionally
needs V-1b's declaration + dispatch and V-2's tables, and lands with them.

---

## 8. Invariants and audit surface

### Proposed **I-43 — a phenotype confers ABI shape, never authority**

> A Proc's phenotype changes only how its syscall numbers and argument structures are
> *decoded*. Every capability check, namespace resolution, handle right, permission
> check, and resource charge is the **native** one, applied identically to a
> phenotyped and a native Proc. A Linux binary can reach exactly what its territory,
> handles, and capabilities allow — no more, and by no different path.

This is the invariant that keeps Vivarium from being a hole in the model, and it is
the thing the focused audit must prosecute hardest: *every* forwarded or translated
call must land on the same `sys_*_for_proc` body, with the same gates, that a native
caller reaches. (`I-42` is Clade's; `I-43` is the next free number.)

### Audit-trigger surfaces (to be added to `ARCH §25.4` + `CLAUDE.md` when built)

| Surface | Why |
|---|---|
| The syscall entry phenotype branch | The privilege boundary: a mis-branded Proc decodes numbers wrong. Prosecute I-43 completeness — *no* translated path may bypass a gate. |
| Brand detection at exec | Mis-branding is the attack: a native binary branded Linux (or vice versa) decodes every syscall wrong. Fail-closed. |
| The supervisor forward channel (B/C) | **New wait/wake on the death lineage** — I-9 register-then-observe, death-interruptibility (#811), no lost/double reply, supervisor death unwinds every parked guest. Spec-first. |
| Signal frame construction (§5.4 Tier 1) | Writes a frame to a user stack and restores `pstate`/`pc` from user memory at `rt_sigreturn` — a classic privilege-escalation shape. Must reject any frame that would elevate. |
| Socket translation | Every `/net` op must run with the *guest's* authority, never the supervisor's (I-43 + I-1). |
| The diorama servers | `/proc/<pid>` cross-Proc reads are the #57a-F2 class (UAF/lifetime under `g_proc_table_lock`) and an info-leak surface (KASLR, other principals' data). |

---

## 9. Scope — the fidelity ladder (the WSL1 lesson)

Published honestly, because §2.3.4 says scope discipline is what decides success.

**v1.0 target**: *a pre-built `musl`-static Linux ARM64 binary runs, does file I/O
and network I/O, and exits correctly; an Alpine container runs a shell and a
non-trivial script.* Concretely `curl`, `wget`, `python3`, `busybox`, `redis-cli`.

- IN: the §11.5 top-50, BSD sockets via `/net`, static ELF, the diorama, `viv`,
  signals Tier 0 (+Tier 1 if the arc allows).
- OUT at v1.0, by decision and stated plainly: `epoll` (v1.1 candidate), `inotify`
  (degrade), `io_uring`, `bpf`, `perf_event_open`, `ptrace`, glibc-dynamic
  (best-effort), `AF_INET6`, cgroups/seccomp, full signal fidelity (Tier 2), and
  in-guest OCI image acquisition (`viv pull` — registry/TLS/layer-unpack; the
  v1.0 bundle is host-baked, §7.2).

A Linux binary needing anything in the OUT list gets a clean `ENOSYS`, never a silent
wrong answer. **`ENOSYS` is a supported outcome; a lie is not.**

**DEGRADED — the third tier (added at V-2d).** IN and OUT are not exhaustive: a
call can be served, and correct enough to run real programs, while differing from
Linux in a way a program could in principle observe. Those belong neither in the
silence of IN nor the honesty of OUT, so they are listed here explicitly. The
standing rule is that a degradation may cost **fidelity**, never **authority** or
**containment** — a difference confined to the guest's own state is a listable
degradation; anything that changes what the guest can *reach* is not, and is OUT.

| Degradation | Detail |
|---|---|
| **Memory protection is advisory below `PROT_EXEC`** (§6.21) | Thylacine anonymous memory is always RW/XN and there is no prot-mutation syscall (an I-12 design choice), so a phenotyped `mmap` grants read+write whatever `prot` asks. Guard pages are therefore **not protective**, and a `PROT_READ` mapping is writable. `mprotect` answers `ENOSYS`, which musl anticipates (`mallocng/malloc.c:92`). `PROT_EXEC` is refused outright rather than degraded — that is I-42/`CAP_JIT` territory. Self-harm only: the pages are the guest's own |
| **`exit(N)` is boolean** (task #91) | Any nonzero status reports 1, a Thylacine-wide v1.0 property (`sys_exit_group_handler` collapses to `exits("fail")`). A shell reading `$?` in a container sees 0-or-1 |
| **`/proc/self` names the mounter** (task #90) | The per-container diorama reports `viv` rather than the reading Proc |

---

## 10. Build arc — V-0..V-8

| # | Chunk | Contents | Gate |
|---|---|---|---|
| V-0 | Scripture | This document; the §4 fork resolved; `ARCH §11.5/§11.6` corrected (R-1, R-2); I-43 minted; NOVEL entry | user signoff |
| V-1 | Phenotype + brand | `Proc.phenotype`, brand detection at exec, the dispatch branch, a native-unchanged proof. **V-1a LANDED** (the field + the advisory `elf_brand_hint`). **V-1b LANDED**: the declaration (`SPAWN_PHENO_LINUX` in `sys_spawn_args.pheno_flags`, consuming the must-be-0 `_pad_allow` slot at offset 92 -> a zero-filled pre-V-1b request still means inherit) + the syscall-entry branch (T1 renumber-in-place then FALL THROUGH to the native switch; the three T2 shells over the V-2 pure translators; FORWARD and ENOSYS kept as separate arms so V-3 is a one-line change) + `sys_fstat_for_proc` extracted so the phenotyped path shares the native core + rule 4's advisory diagnostic (the hint's first caller, on an already-failed load) + `viv`'s `org.thylacine.phenotype` manifest annotation | native suite byte-unchanged (1237/1237); **PASS in-boot on two vantages** -- leg A `viv-pheno-probe native` proves a Linux number is NOT translated without a declaration (`brk` -> -1, not -ENOSYS), leg B `viv run /vivarium/pheno` proves the whole chain with a container entrypoint that speaks only raw Linux numbers and moves real bytes (openat/read/lseek/fstat/newfstatat/write/close, the two stat paths cross-checked on `(st_dev, st_ino)`, the `AT_SYMLINK_NOFOLLOW` reject still rejecting) and dies through Linux `exit_group`; revert-probed |
| V-2 | The translation table | The §4-C stateless 1:1 set; the split rule enforced | a static `hello` (built by *Linux* toolchain) runs and exits 0 |
| V-3 | Supervisor channel | **DEFERRED (user-voted 2026-07-30) — §4.1.** The sketched destination (a ring to a peer Proc) is verified unable to serve the forwarded set: no Proc can mutate another's address space, handle table or process tree, so the servable set is empty. Not "hard"; *empty*. The shape is decided by **V-5**, the first chunk that genuinely needs a destination; §4.1 records the three live candidates and the peer evidence so V-5 does not re-derive them. `specs/phenotype.tla` lands with whatever V-5 chooses | (deferred; the gate travels with V-5) |
| V-4 | The diorama | `/proc`, `/sys`, `/dev` servers + per-container mounts | `busybox ps`, `ldd`, `/proc/self/exe` |
| V-5 | Sockets | The `/net` translation | **`curl` fetches a URL** (ROADMAP §9.2) |
| V-6 | Signals | Tier 0, then Tier 1 (audit-bearing) | Ctrl-C kills a guest; `SIGPIPE`; handler round-trip |
| V-7 | `viv` | bundle-consumer runtime (§7.2): host-baked bundle → territory + per-container diorama + `/dev` binds → #58 spawn. **LANDED**: `usr/viv` + `usr/viv-probe` + the `/vivarium` pool bake (the synthetic probe bundle always; the Alpine bundle stages when a minirootfs tarball is provided) + the boot-fatal joey leg; PGRP_MAX_MOUNTS 20→32 (the container recipe overflowed the territory table) | the native `viv-probe` gate (§7.2) — **PASS in-boot**, revert-probed (an unfiltered diorama fails the pid-enumeration leg); **an Alpine shell runs** is the ARC gate (needs V-1b + V-2 too; ROADMAP §9.2) |
| V-8 | Close | Focused audit (I-43), SMP gate, `docs/reference/NN-vivarium.md`, the fidelity ladder published | clean close |

Track note: V-1..V-3 are kernel-track (main); V-4/V-5/V-7 are userspace and
aux-shaped; V-6 is kernel and audit-bearing.

**Sequencing — CORRECTED 2026-07-29. The numeric order is not the dependency
order, and this row previously said the arc could split "after V-3".** V-2c
falsified that: **V-1b cannot precede V-7.** `Proc.phenotype` exists but nothing
can set it to `PHENO_LINUX` (verified — `exec.c` never touches the field, the only
assignment is the rfork inherit, and `PHENO_LINUX` appears nowhere outside its own
enum), because per the Q3 resolution + §12.1 the **container** is what declares a
phenotype. So a dispatch branch written before V-7 would branch on a provably-zero
field: dead code, unprovable end-to-end. The landed V-2 translation tables
correspondingly have **no caller** yet, by design (§6.19/§6.20).

The real order, and the standing plan (user-directed 2026-07-29):

1. **The queued bugs first** — #80 (`SYS_WALK_OPEN` bare `-1`), #81 (`/file/..`
   lexical dots), and **#66c/#926** (the handle-table lifetime restructure, which
   is what unblocks `self/fd`). #80 shares a fix shape with the tracked
   unlink-path errno-loss; they are efficiently done together.
2. **V-7** — the container object, which is what makes a phenotype declarable. Its
   §7.1 obligation (pid visibility) must be decided on the **native** surface.
3. **V-1b** — the declaration + the syscall-entry dispatch branch; this is what
   gives V-2's tables their first caller, and `docs/reference/NN-vivarium.md`
   lands with it.
4. **V-3 / V-5 / V-6**, then **V-8** (the I-43 focused audit + close).

**CORRECTED AGAIN 2026-07-30, at V-3's entry, and for the same reason as the
first correction.** V-3 cannot precede V-5: §4.1 shows the forward channel's
sketched destination has an empty servable set, so building it now would be a
mechanism without a consumer — precisely the error the paragraph above corrects
for V-1b/V-7. The order from here:

1. **V-2d** — `mmap` + `munmap` as argument-domain T2 rows (§6.21). This is the
   unblocking work: `mmap` is on musl's critical path (TLS + malloc), and §4.1's
   inverted calculus makes it a compatibility requirement rather than an
   optimisation. It applies a rule that is *already binding* (V-2b), so it needs
   no new decision.
2. **V-6** — signals. Kernel-side either way, so it is independent of §4.1.
3. **V-5** — sockets, which **decides V-3's shape** and then builds it.
4. **V-8** — the I-43 focused audit + close.

---

## 11. Naming — **ADOPTED** (user, 2026-07-23)

Per CLAUDE.md's thematic-naming discipline. The thylacine's defining biological fact
is **convergent evolution** — a marsupial that arrived at the canid form from a
different lineage (*Thylacinus cynocephalus*, "pouched dog-head"). A compatibility
layer is precisely that: the same outward form, a different ancestry. The extinction
half of the theme supplies the habitat vocabulary (museum diorama, zoo enclosure).

| Name | What it names | Why |
|---|---|---|
| **Vivarium** | the subsystem + the container runner (`viv`) | An enclosure maintaining an organism *in conditions simulating its natural environment* — a literal description of what a Linux container on Thylacine is. Short, memorable, pronounceable, and it is the user-facing verb (`viv run alpine`). |
| **phenotype** | the per-Proc ABI mode (`Proc.phenotype`, `PHENO_LINUX`) | The **expressed outward form** of the same genotype. Linux calls this concept *personality*; the biological term is exact, thematically native, and reads perfectly in code and prose ("a Proc's phenotype is native or linux"). |
| **diorama** | the synthetic `/proc` + `/sys` + `/dev` | A constructed habitat built around a specimen — and the thylacine's own most iconic museum presence. It is exactly what these servers are: a convincing fake world assembled for the creature inside. |

Alternates considered for the umbrella, and why not: **Convergence** (perfect
concept, but "the convergence detour" already names the identity arc — collision);
**Cynocephalus** (the sharpest metaphor, unusable as a command); **Homoplasy** (the
precise term for convergent-not-homologous traits — accurate but academic);
**Menagerie** (ideal, already the driver framework).

Not renamed: **Pouch** stays the *source*-compat environment (APE's analogue).
Vivarium is the *binary*-compat one. The two names sit correctly beside each other —
a pouch is part of the animal, a vivarium is built around it.

---

## 12. Decisions (all four RESOLVED 2026-07-23)

- **Q1 — the §4 fork: C (hybrid).** In-kernel for total-and-stateless translations,
  userspace supervisor for everything with state. The split rule in §4 is binding.
- **Q2 — scope: BUILD NOW, on the aux track.** Vivarium is adopted as the aux
  track's arc, running beside the main track's Clade work. This makes it a v1.0
  candidate rather than a v1.1 deferral; the `ROADMAP §11.5` fallback discipline is
  unchanged (v1.0-rc.1 ships without it if the arc does not converge).
- **Q3 — the `ELFOSABI_GNU` collision: RESOLVED, and it was never a fork.** It was
  posed as a vote in error; the facts decide it:
  - `ELFOSABI_NONE (0)` is what most `musl`-static Linux binaries carry — **and** what
    native Thylacine binaries carry. The byte cannot positively identify the v1.0
    target.
  - `ELFOSABI_GNU (3)` means "a GNU/LLVM toolchain emitted GNU extensions," not
    "this is Linux" — Clade's own native output carries it (which is why the Clade
    arc widened `elf.c:77` to accept 3; see §1.5 for the branch state).
  - So `EI_OSABI` is **non-discriminating in both directions**; there is nothing to
    trade off. `PT_INTERP` is a strong positive signal but exists only on *dynamic*
    binaries — absent precisely on the static v1.0 target.
  - **Therefore**: the **vivarium declares the phenotype** (the LX-zone lesson,
    §5.2), the default is `PHENO_NATIVE`, and ELF bytes are corroborating hints that
    may raise suspicion but may never *decide*. The fail-safe direction is forced —
    a Proc is never *inferred* into a non-default ABI, it is only ever *declared*
    into one. §5.2's priority list is amended accordingly below.
- **Q4 — naming: ADOPTED.** Vivarium / phenotype / diorama are the names.

### 12.1 §5.2 amended by the Q3 resolution

Brand detection is **advisory input to a declaration**, never an inference:

1. The **vivarium manifest** declares the container's phenotype. This is the only
   thing that can *set* `PHENO_LINUX`.
2. Within a declared-Linux vivarium, every exec is `PHENO_LINUX` unless the binary is
   positively identified as native (a Thylacine-native brand, §12.2).
3. Outside a vivarium, the phenotype is always `PHENO_NATIVE`. No ELF byte, note, or
   interpreter path changes that.
4. `PT_INTERP` / `EI_OSABI` / `NT_GNU_ABI_TAG` are used only to *warn* on an obvious
   mismatch (a Linux-interp binary exec'd outside a vivarium gets a diagnostic and a
   clean failure, not a silent mis-decode).

**As-built (V-1b).** Rule 1 is `sys_spawn_args.pheno_flags & SPAWN_PHENO_LINUX`,
set by `viv` from the manifest's `annotations["org.thylacine.phenotype"]` on the
container's **entrypoint** spawn only (the per-container diorama stays native —
it is a Thylacine server that happens to serve a Linux-shaped world). Rule 2 is
the ordinary `rfork` inherit in `rfork_internal`. Rule 3 is realised in the
ABI's *shape* rather than in a check: only `SYS_SPAWN_FULL_ARGV` carries the
field, so the register-argument spawn variants cannot declare at all. Rule 4 is
`elf_brand_hint`'s single caller in `exec_setup_from_spoor`, consulted **only
after** `elf_load` has already failed with `HAS_INTERP`/`HAS_DYNAMIC` — so it
can explain an outcome but never change one, which is the fail-safe direction
Q3 forced. The declaration is deliberately **ungated**; `ARCHITECTURE.md §28
I-43` and `docs/reference/145-vivarium.md` §3 carry the argument for why that is
sound (every translated Linux number collides with a live native one, so a
mis-declared Proc mis-decodes its own calls behind its own gates and reaches
nothing new).

### 12.2 Owed: a native brand (v1.x seam)

Because `EI_OSABI` cannot distinguish native from Linux, Thylacine-native binaries
should eventually carry a positive brand of their own (an `NT_GNU_ABI_TAG`-shaped
`.note.thylacine`, emitted by the Clade toolchain and by `pouch-ld`). Not needed for
v1.0 — rule 3 above makes the default safe without it — but it is what would let
rule 2 be exact instead of heuristic, and it is cheap to add while Clade is being
built. Recorded for the main track.

---

## References

- `ROADMAP.md §9` (Phase 8), `§11.5` (v1.0-rc fallback) · `docs/phase8-status.md`
- `ARCHITECTURE.md §11.5` (syscall coverage — gains the R-2 clause), `§11.6` (ABI —
  gains the R-1 correction), `§25.4` (audit surfaces), `§28` (I-43)
- `POUCH-DESIGN.md` (the source-compat sibling) · `NET-DESIGN.md §7` (the socket
  boundary-line this relocates) · `LOOM.md` (the forward channel substrate)
- `EXEC-LOAD-DESIGN.md` / I-36 (REVENANT, the loader) · `IDENTITY-DESIGN.md` (I-32
  resource floor) · `MENAGERIE.md` (I-34)
- SOTA: FreeBSD `COMPAT_LINUX`; illumos LX brand zones; WSL1 pico processes; gVisor
  Sentry; **Fuchsia Starnix** (the closest peer); Plan 9 APE (the heritage answer)
