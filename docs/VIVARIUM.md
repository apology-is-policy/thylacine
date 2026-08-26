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

### 4.1.1 V-5's answer, measured — sockets need no supervisor at all

**Resolved 2026-07-31 (user-voted), at V-5's entry, by measuring `/net` rather
than reasoning from the paragraph above.** The three candidates are all
*supervisor* shapes, and the requirement §4.1 predicted would choose between them
turns out not to be a requirement for a supervisor at all. The sentence "sockets
need multi-step orchestration that no single call-and-reply can express" is an
argument against a **ring**. It is not an argument against the **kernel** doing
the orchestration, and the kernel does exactly this kind of orchestration already:
`exec_resolve_from_namespace` resolves a path in the caller's Territory, with the
caller's authority, and hands back a Spoor.

What the measurement found (`usr/netd/src/server.rs`, not scripture):

- Opening `clone` **mints** a connection and rebinds that fid onto its `ctl`;
  that fid holds the connection's only reference (`slot_ref`, 0→1). Drop it before
  another fid binds and the connection is freed.
- Opening a TCP `data` file **defers** the `Rlopen` until ESTABLISHED (#257). So
  `data` cannot be pre-opened at `socket()` — it is openable only after the
  `connect` verb is written.
- **Any** fid bound to a connection holds a reference (`fid_set`/`fid_clunk`, via
  `path_conn_n`), so once `data` is open the `ctl` fid is disposable.
- Reading a `ctl` fid yields the connection number in decimal
  (`file_content`: `FK_CTL => c.push_dec(n)`) — the Plan 9 idiom, live.
- `read`/`write`/`close` are **T1 renumber rows** that fall through to the native
  switch. So if the socket fd *is* the `data` fd, the entire data path needs no
  translation whatsoever.

Those five facts compose into the design: **the socket fd changes identity at
`connect`** (`ctl` → `data`), which is what makes the hot path free, and the only
thing that must survive the `socket()`→`connect()` gap is the tuple
`(proto, N, state)`. `N` is re-readable from `ctl` by the documented protocol;
`proto` is knowable only at `socket()` and cannot be recovered later without
decoding netd's private qid layout — which the kernel must not do, because `/net`
is a mount point and need not be netd. So the state is small, real, and
unavoidable: a per-Proc table, exactly the shape `Proc.sigtab` took at V-6.

**Therefore V-3 stays deferred and V-5 does not build it.** Its fork moves to the
next chunk that genuinely needs a destination, which on present evidence is
**process creation (task #93)** — `clone`/`execve`/`wait4`, where the guest's
request really is "make a *new* process exist", something no in-kernel
translation of the caller's own authority can synthesise from `SYS_SPAWN_*`'s
program-not-continuation shape. The three candidates above stand recorded for it.

The rule this establishes, and which the next chunk should apply before reaching
for a supervisor: **ask first whether the kernel can perform the call with the
caller's own authority.** A supervisor is needed only when the work is something
the caller could not do for itself — not merely when it takes several steps.

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

**CORRECTION to Tier 1, 2026-07-30 (V-6 entry; user-voted).** The clause *"restores
`pstate`/`pc` from user memory at `rt_sigreturn`"* described Linux's mechanism, not
the one Thylacine should build — and measuring the kernel found a strictly safer
shape that this section did not know existed. Thylacine's note delivery already
saves the interrupted user context into **kernel-side per-Thread fields**
(`Thread.note_saved_regs[31]`/`note_saved_sp_el0`/`note_saved_elr`/
`note_saved_spsr`), a deliberate divergence from Plan 9 (whose `notify` pushes a
`Ureg` the handler's `noted` reads back). Those four fields are a field-for-field
match for the register half of Linux's `mcontext_t`. So Tier 1 pushes a frame the
handler can **read**, and restores from the **kernel** copy — which makes the
escalation hazard this section flagged impossible by construction rather than
merely guarded. §6.22 is the design; the audit-trigger row in §8 is restated to
match.

### 5.5 Sockets (R-2)

The Linux socket family is **translated to `/net` file operations** — the same
mapping the pouch boundary-line (`0016`) already implements and proved, relocated
into the kernel phenotype as a T2 family (§4.1.1, user-voted 2026-07-31; the
earlier "forwards to the supervisor under Option C" reading is superseded — there
is no supervisor, and sockets do not need one). `AF_UNIX` maps to the existing
`/srv` byte-mode services (the `0006` patch's proven mapping). `AF_INET6` →
`EAFNOSUPPORT` at v1.0, honestly.

**Why the kernel and not a server.** Every step is something the calling Proc
could already do for itself: `stalk` into its own Territory, read, write, install
an fd in its own handle table. So I-43 holds *by construction* rather than by
review — a translated socket call reaches exactly what the guest could have
reached by opening `/net` by hand, and a container whose territory has no `/net`
gets `ENETDOWN` from the walk, not a privileged bypass. This is the same argument
the diorama rests on (§6.2): a reformatter, never a new authority.

#### 5.5.1 The fd identity change (the load-bearing mechanism)

`/net` splits across three files what Linux fuses into one fd. The resolution:

| Linux | `/net` |
|---|---|
| `socket(AF_INET, SOCK_STREAM)` | open `/net/tcp/clone` → the fid *becomes* `ctl`; read it for `N` |
| `connect(fd, addr)` | write `connect a.b.c.d!port` → `ctl`; open `/net/tcp/N/data` |
| `read`/`write`/`close` | **untranslated** — T1 rows on the `data` fd |
| `shutdown` | write `hangup` → a freshly re-walked `ctl` |
| `getsockname`/`getpeername` | read `/net/tcp/N/local` / `remote` |

The socket fd is the `ctl` fd from `socket()` until `connect()`, and the `data` fd
afterwards. `connect` performs that swap **in place**, via a new
`handle_replace(p, fd, kind, rights, obj)` primitive — `handle_dup` cannot serve,
because it allocates a *new* slot and the guest is holding a specific number.

Three properties make the swap correct, each measured in §4.1.1:

1. The `ctl` fid must be held until `data` binds, or netd frees the connection
   (`slot_ref` 0→1 at the clone-mint). The swap opens `data` **before** releasing
   `ctl`, so the reference count never transits zero.
2. `data` cannot be opened earlier, because netd defers its `Rlopen` to
   ESTABLISHED (#257). So the swap is at `connect` and nowhere else.
3. After the swap the fd needs no further translation forever — which is the
   entire point, and why `read`/`write` must **not** grow a socket check. Putting
   one on the hottest path in the system to serve a phenotype would be the wrong
   trade even if it were correct.

#### 5.5.2 `Proc.socktab` — the state, and why it is unavoidable

`(proto, N, state)` per socket, in a lazily-allocated per-Proc table: the
`Proc.sigtab` shape (V-6), CAS-installed outside every lock, freed at `proc_free`,
not `rfork`-inherited, bounded (`VIV_SOCK_MAX`) so a guest cannot grow kernel
memory without bound — the I-32 posture, with the socket count charged to the
guest's own handle table besides.

It is unavoidable, and the alternatives were measured rather than assumed:

- **`N`** is re-readable from `ctl`, but only while the fd still *is* `ctl`.
- **`proto`** is knowable only at `socket()` (the guest passes `SOCK_STREAM` /
  `SOCK_DGRAM` there and never again). Recovering it later would mean decoding
  netd's qid layout (`CONN_FLAG | proto<<32 | N<<8 | filekind`) — **rejected**:
  `/net` is a mount point, need not be netd, and a foreign server's qid decoded as
  netd's is a silent mistranslation, the one failure mode §6.19's argument-domain
  rule exists to prevent.
- **`Spoor.path`** would carry the name — **forbidden by I-33**, which makes path
  retention explicitly non-load-bearing. Reading it here would convert a cosmetic
  field into a correctness dependency.

So the table is the honest answer, and it is small.

**A SECOND property of the table rather than of the data (V-5d SA-1).** The
socktab keys on the fd NUMBER, so an entry must be dropped whenever that number
is freed — and exactly one place does it, the `close` hook in
`viv_linux_dispatch`. That hook is COMPLETE only because `close` is the ONLY
fd-freeing row. `dup` (23), `dup3` (24) and `close_range` (436) all free an fd
and all decline, which is what makes the single hook sufficient; each is also a
near-trivial renumber, so serving one as an ordinary T1 row is an easy and
invisible mistake that would reintroduce the exact bug the hook prevents — a
freed fd number whose `(proto, N)` entry survives to be handed to the next
fd-creating call, so a later `connect()` writes a dial verb to a stranger's
connection. The three are therefore NAMED and declined explicitly rather than
merely absent, and `vivarium.fd_freeing_rows` asserts it: **an fd-freeing row
lands with its socktab drop, in the same commit.** (Like the threading property
below, this is a fact about the translation table that a future row can
falsify — not an invariant of the code.)

**A thread-safety property that is a property of the TABLE, not of the data.**
`socktab` (like `sigtab`) is read and written without a lock. That is sound today
only because **a `PHENO_LINUX` Proc cannot obtain a PEER THREAD**, so there is no
peer to race. This is *not* a property of the entries being small or of any
atomicity argument.

**The MECHANISM, corrected at #157/#158.** This paragraph used to say the reason
was that `clone`/`clone3` are not table rows — which was true when written and
was falsified by LINEAGE L-3d landing the clone row (L-6a then widened it to the
fork shape), *without anything failing*. The reason it still holds is narrower
and lives one function away: `vivarium_clone_decide` admits exactly two words by
**exact equality** (`SIGCHLD`, and `CLONE_VM|CLONE_VFORK|SIGCHLD`), and neither
carries `CLONE_THREAD` — refusing the thread set is one of the three things that
equality is written to do. A `fork` yields a new *Proc* with its own tables,
which races nothing here.

So the property now **evaporates the moment the clone domain admits the thread
set** — a one-line change with no compiler consequence anywhere near either
table. Both must be re-derived then, and the field comments say so. (The general
shape: a load-bearing sentence must name the mechanism that is *actually*
holding, or nobody can re-check it when that mechanism moves.)
(V-6c left the opposite claim on `sigtab` — that byte-sized entries could not tear
— which was true at V-6b and false once entries widened to 32 bytes; corrected in
the same commit as this section, task #97. And the intra-Proc argument above was
never the whole of it: `sigtab` is read lock-free from OTHER Procs' CPUs too —
`notes_post`'s SIG_IGN hook, `notes_proc_has_live_handler`, the `^Z` fan — which
is the axis aux#254 / main#243 added; that half rests on the table never being
freed while reachable and on every field being accessed as one atomic u64 with
`handler` published last on install and zeroed first on reset — `kernel/vivarium.c`,
"the access discipline". Widening the clone domain re-opens only the intra-Proc
half; the cross-Proc half does not care how many threads the Proc has.)

#### 5.5.3 The server path — `bind` remembered, `listen` spent, `accept` walked

The client path swaps an fd; the server path does three different things, and
each is shaped by something netd does rather than by a choice made here.

**`bind` is remembered, not written.** netd has no `bind` ctl verb at all. A
local endpoint reaches it only as the argument of `announce` — and is simply
unavailable to a client. So `bind()` records the request in the socktab and
`listen()` spends it. The pouch boundary-line arrived at the same shape
independently, which is some evidence it is the shape the server offers rather
than a preference.

Two consequences are listed in §9: a port collision surfaces at `listen` instead
of `bind`, and a *constrained* bind before `connect` is refused outright, because
netd's dial verb carries only the remote endpoint and pretending otherwise would
hand the client an ephemeral source port while telling it it had the one it
asked for. An *unconstrained* bind (`0.0.0.0:0`) asks for nothing netd is not
already doing and proceeds — which is also why the table needs no `bound` flag:
"bound to anything" and "not bound" are the same request everywhere it is read.

**`listen` writes `announce`, and performs no swap.** The fd stays `ctl`. That is
not an omission: `ctl` is the file `accept` re-walks from, and the reference that
keeps the listener alive. So the ctl/data split is `FRESH|LISTENING` vs
`CONNECTED`, not `FRESH` vs everything else.

The wildcard is load-bearing. `0.0.0.0` renders as Plan 9's `announce *!port`, a
concrete address as `announce a.b.c.d!port` — and netd treats them differently:
an explicitly-announced `127.x` listener is migrated onto its loopback stack,
while a `*` listener stays on the NIC and does not span loopback. Rendering one
as the other would silently move the server to a different interface.

**`accept` walks, and swaps nothing.** netd holds the `Rlopen` for
`/net/tcp/N/listen` until a call lands, then rebinds that fid onto the accepted
connection's `ctl` and replies. So:

```
open(/net/tcp/N/listen)  -> BLOCKS; returns a fid that is now M's ctl
read(it)                 -> M
open(/net/tcp/M/data)    -> the fd accept() returns
close(the listen fid)       data holds M's reference now
```

The fd `accept` returns is the one `sys_open_kpath_for_proc` already produced for
`data`, so unlike `connect` there is nothing to move. The listener N is untouched
— netd re-arms it with a fresh socket during the swap — which is what lets a
server accept more than one connection.

**The whole round-trip is provable in ONE single-threaded process** -- which is
how the V-5b gate drives both ends, and was the only shape available when this
landed (the fork-shape `clone`/`execve`/`wait4` rows arrived later at L-6a/L-6b;
a `PHENO_LINUX` Proc can now fork, though a genuinely-concurrent `CLONE_THREAD`
is still refused). It works because TCP establishes in netd's *stack*, not in
`accept()`: the client's `connect()` completes the handshake against the
announced listener, and the server's `accept()` then finds the connection already
waiting. That is precisely what a listen backlog is, and it is why the in-guest
gate can drive server and client from the same thread.

#### 5.5.4 Readiness — the fd that gets polled is not the fd that gets read

`ppoll` is the whole poll family on aarch64: the generic ABI dropped plain
`poll(2)` and `select(2)`, so musl's `poll()` and `select()` both arrive here.

**The pollfd array needs no conversion.** `<thylacine/poll.h>` is deliberately
Linux-shaped — 8 bytes, `fd` at 0, `events` at 4, `revents` at 6, and the same
`POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP` values. So the only translation is the
**fd**, and only for a socket.

**A socket cannot be polled on its own fd.** A `/net` socket's fd names
`/net/<proto>/N/data` — an ordinary dev9p file, and dev9p reports an ordinary
file as POSIX always-ready. That is right for a file and useless for a socket.
netd publishes readiness on a *sibling*, `/net/<proto>/N/ready`, whose qid
carries the reserved `QTPOLL` bit; `dev9p.poll` probes exactly that bit. So
`ppoll` opens the `ready` sibling, polls *that*, and puts the caller's own fd
number back before returning. Polling the socket's own fd would return "ready"
instantly and defeat every wait — the identical bug the pouch boundary-line hit
at net-6b-3, for the identical reason.

**The `ready` fd is opened per call, not cached, and that is a deliberate
trade.** Caching it (what pouch does) would put a handle the guest never asked
for into the guest's *own* fd-number space — where the guest can close it,
leaving a cached number that names whatever was allocated next, and where it
breaks POSIX's lowest-available-fd guarantee. In pouch that hazard does not
exist, because there the ready fd *is* a guest fd its own libc opened and
tracks. Here the guest cannot see it, so it must not outlive the call. The
transient fd is unobservable for exactly the reason the socktab needs no lock —
a `PHENO_LINUX` Proc is single-threaded — and both properties evaporate together
when process creation lands (task #93).

**Readiness is not knowable synchronously, and the fix for that is latency, not
a guess.** netd's probe is asynchronous: `dev9p.poll` *submits* it and answers
from a cache the freshly-opened fd does not yet have. A strict zero-timeout scan
would therefore report "nothing ready" for a socket that is plainly writable —
and a caller polling with timeout 0 in a loop would never make progress at all.
So a caller-supplied timeout of **0** gets a small budget (10 ms) when a socket
is actually in the array. That changes the *latency*, never the *answer*: what
comes back is netd's real verdict. A probe that misses even that budget yields
not-ready and the caller retries — the safe direction. It is a mitigation rather
than a closure; task #98 holds the two real fixes, both of which sit on the
audited net-6b surface.

**One netd change was owed and is paid here (#220).** POSIX defines `POLLIN` on
a *listener* as "a connection is pending — `accept` will not block". netd
computed readiness from `can_recv()`, which is false for a listening socket in
every state, so a `poll(listener, POLLIN)` deferred forever while a real client
sat connected. A server that polls before accepting — the entire point of poll —
could never learn it had a caller. `slot_poll_readable` now reports an announced
slot's readiness with `accept_ready`, the *same* predicate `poll_accepts` uses to
decide a deferred accept may complete, so a poller and an accepter cannot
disagree about whether a call has arrived. The window is not narrow: netd only
swaps a listener when some fid is already blocked in `open(listen)`, so a server
that polls first leaves the established call sitting in the listener's socket
until it chooses to accept.

**`pselect6` is the same readiness, wearing a different shape** (V-5c-2). Three
1024-bit bitmaps in, one pollfd array out, three bitmaps back — the one T2 row
whose translation is a genuine change of *representation* rather than a renumber
or a field copy. The conversion is pure and unit-driven; the shell is uaccess and
the same `viv_poll_translated` core `ppoll` uses, so the socket→`ready` fd swap
above is shared rather than reimplemented.

The prior art is in this tree and it is wrong in four ways. pouch's userspace
`select()` performs this identical translation over native `SYS_POLL`
(`0005-pouch-poll.patch`), and reading it before writing this was worth more than
the writing — each of its defects is a decision point here (task #99):

* **It caps the wrong axis.** It rejects any fd ≥ 64 with `EBADF`, justified as
  "the handle would be unreachable through any Thylacine syscall". That was true
  when `PROC_HANDLE_MAX` was 64; commit `ffcc64b7` split `PROC_HANDLE_MAX` (256,
  the fd *value* ceiling) from `POLL_MAX_NFDS` (64, the pollfd *array* ceiling)
  and made it false. The bound belongs on the count of contributing fds.
* **It maps `exceptfds` to `POLLPRI`**, which native poll cannot report, so the
  request is silently dropped and a pure-`exceptfds` wait blocks forever.
* **It forwards `POLLHUP` into the write set**, commented "(Linux semantics)".
  Linux's `POLLOUT_SET` is `POLLOUT|POLLERR`; `POLLHUP` is in `POLLIN_SET` only.
  The asymmetry is not an oversight — a peer that hung up leaves data readable,
  while writing to it is an error rather than a completion.
* **It counts fds, not bits.** Linux increments `retval` once per *set bit*, so
  an fd ready both ways counts twice and the return can exceed the number of fds
  passed in. A caller looping `while (n-- > 0) find the next set bit` stops one
  short against a per-fd count.

**Both zero-fd forms now sleep.** `select(0, NULL, NULL, NULL, &tv)` is the
classic portable sleep and `poll(NULL, 0, ms)` is its twin. V-5c-1 declined
`ppoll`'s on the grounds that "there is no native sleep syscall to route it to" —
which was true and one layer too high: there is no sleep *syscall*, but there has
always been a sleep *primitive*, `tsleep` with a deadline and a cond that is never
true, which is what poll's own slow path parks on. `sys_poll_sleep_for` makes it
reachable, `sys_poll_for_proc`'s `nfds == 0` rejection is deliberately left alone
(it is a native ABI a native caller may rely on), and the `ppoll` decline is
retired.

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
| ~~`O_CLOEXEC`~~ | ~~Asks that the fd not survive exec. Thylacine has no close-on-exec concept because there is nothing to opt out of: a spawned child "inherits no Spoor handles" (`syscall.h:327`) and `SYS_SPAWN_WITH_FDS` passes an **explicit** list~~ **VOIDED; the flag is now HONOURED FOR REAL — see below (#151)** |
| `O_NOCTTY` | Asks not to acquire a controlling terminal. Thylacine acquires one only via the explicit `SYS_TTY_ACQUIRE` (PTY-1), never implicitly on open — already relied on by the pouch pty patch |
| `O_LARGEFILE` | Asks that >2 GiB offsets be permitted. Every Thylacine offset is 64-bit, exactly as on 64-bit Linux, whose kernel force-sets the bit internally |

> **`O_CLOEXEC`'s entry above is no longer true, and how it stopped being true is
> the point.** Its argument was never "the flag is harmless"; it was the stronger
> and correct claim that *there is nothing to opt out of*, because the only way to
> start a program was `SYS_SPAWN_*`, which endows an **explicit** fd list. Nothing
> survived an exec that the parent had not deliberately handed over, so a flag
> asking "do not let this survive" requested behaviour we already provided
> unconditionally — which is exactly the bar this table sets.
>
> **LINEAGE voided the premise, one commit at a time, and neither commit had any
> reason to look here.** L-2a (`execve`) replaces the image *in a live Proc* and
> leaves the handle table untouched. L-3c-1 gave `rfork` a **copy** of the parent's
> table, so the child sees what the parent sees. Together they are the POSIX
> fork+exec shape, and under it every fd survives exec — so there is now a great
> deal to opt out of, and admitting `O_CLOEXEC` silently promises something the
> kernel does not do.
>
> This is the recurring shape rather than a one-off: **a round is scoped to one
> commit, so a premise that a *later* commit voids is invisible to it.** The
> defence is not a better review of either commit; it is that a fact stated as
> load-bearing must be re-checked when the thing it rests on moves. `#150`
> surfaced it from the other end — measuring which `fcntl` cmds Alpine's busybox
> issues turned up `F_SETFD(FD_CLOEXEC)` and `F_DUPFD_CLOEXEC`, and asking why
> those could not be served led back to this row.
>
> **#151 CLOSED IT, and by building the feature rather than adjusting the claim.**
> `struct HandleTable` gained a `cloexec` bitmap parallel to its slot array —
> parallel rather than a field in `struct Handle` because POSIX close-on-exec is a
> property of the **descriptor**, not of the open file description behind it
> (`dup(fd)` yields a second descriptor onto the same description with the flag
> clear). Linux keeps `close_on_exec` beside `fd[]` in `struct fdtable` for the
> same reason. `execve` consumes the flagged slots after its commit point;
> `rfork` preserves them; `openat` sets the bit after a successful open, since it
> names the resulting descriptor and so cannot ride in the omode.
>
> So `O_CLOEXEC` returns to the table above as genuinely honoured — but the entry
> stays struck through and this note stays beneath it, because the *reasoning*
> that admitted it was wrong even while its conclusion happened to hold. A flag is
> admitted here only when the behaviour it asks for is what we do unconditionally;
> `O_CLOEXEC` no longer qualified, and the fix was to do what it asks.

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

There was no create-by-path syscall in the tree when this correction was
written (the only other cwd-joining site, `exec_resolve_from_namespace`,
resolves a binary to exec). **#50 built it** — `SYS_OPEN_CREATE` = 108 plus
the path-mutation family, section 6.24: the verdict above stands against
ROUTING to `SYS_WALK_CREATE`; the resolution is a new kernel core whose cwd
join is SYS_OPEN's own helper (blocker 3 closed structurally) and whose
create-else-open composition is kernel-side (blockers 1+2 dissolved the
Plan 9 way).

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

### 6.22 Signals — the frame shape (V-6, design)

R-3 (§1.4) called signals "the genuine hard part", and §5.4 sized Tier 1 as
"kernel-built `ucontext` frame on the user stack + `rt_sigreturn`" — the shape
every peer builds. Measuring the kernel at V-6's entry found that Thylacine
already owns most of the mechanism, and owns it in a **safer** shape than the one
scripture specified.

**The substrate.** `SYS_NOTIFY`/`SYS_NOTED` already deliver an asynchronous note
to a user handler and return from it: the EL0-return tail saves the interrupted
context, rewrites the exception frame to land at the handler, and `SYS_NOTED`
restores. That machinery is audited across four rounds. Crucially, the save is
**kernel-side** — into per-`Thread` fields — and those fields are a field-for-field
match for the register half of Linux's `mcontext_t`:

| `Thread` (`thread.h:247`) | Linux `mcontext_t` (aarch64) |
|---|---|
| `note_saved_regs[31]` | `regs[31]` (x0–x30) |
| `note_saved_sp_el0` | `sp` |
| `note_saved_elr` | `pc` |
| `note_saved_spsr` | `pstate` |

Only `fault_address` has no counterpart, and it is derivable (`FAR_EL1` for the
`snare:*` faults, 0 otherwise).

**This is a Plan 9 divergence Thylacine already made, in the safe direction.**
Plan 9's `notify` pushes a `Ureg` onto the user stack and `noted` reads it back;
Thylacine saves in the kernel instead. The capability-microkernel SOTA agrees —
seL4 manipulates a faulting thread's state through its TCB capability, Genode
notifies a handler thread; neither reconstructs registers from a user stack. Only
the Linux *emulators* (gVisor's Sentry, Starnix) rebuild the real frame, because
bug-compatibility is their product.

**DECISION (user-voted 2026-07-30): the frame is pushed for reading; the restore
is from the kernel.** Delivery writes a real `siginfo_t` + `ucontext_t` to the
user stack, so a handler that *reads* them — a crash reporter printing
`uc_mcontext.pc`, anything consulting `si_addr` — works. `rt_sigreturn` then
restores from the `Thread` snapshot and **ignores whatever the handler wrote to
the frame**.

The gain is not a smaller diff, it is a deleted hazard. §8's audit-trigger row
warned that Tier 1 "restores `pstate`/`pc` from user memory at `rt_sigreturn` — a
classic privilege-escalation shape. Must reject any frame that would elevate."
Under this design **no field of the user frame ever reaches `pstate`, `pc`, or
`sp`**, so there is no frame to reject and no validator to get wrong. The row is
restated accordingly: the obligation becomes proving the restore reads only
kernel state, which is a structural property rather than a sanitising pass.

The cost is a fidelity limit, named rather than buried: **writing to
`uc_mcontext` does not change where execution resumes.** That breaks
signal-driven control transfer — Go's `sigpanic` injection and JIT
deoptimisation both rewrite the saved `pc`. Neither reaches this path at v1.0
(the Go fork is native, and an executable anonymous mapping is `CAP_JIT`/I-42
territory that §6.21 already refuses), and the limit belongs in §9's DEGRADED
tier. It costs fidelity, never authority: the frame is the guest's own stack,
every gate is unchanged, and I-43 is untouched.

**The disposition table is new per-Proc state, and it has to be.** Pouch keeps
its sigtab in *userspace* (`__pouch_sigtab`, patch `0007`) because the pouch libc
is ours to patch; a Vivarium guest's libc is not, so the kernel must hold it.
Lazily allocated and freed at `proc_free`, the `p->env`/`debug_hw` pattern.

**The signal↔note map.** Every row is an existing note, so Tier 0 is a decode
rather than new machinery:

| Linux | note | disposition |
|---|---|---|
| `SIGINT`, `SIGTERM` | `interrupt` | catchable; default terminate (LS-5) |
| `SIGKILL` | `kill` | non-catchable both sides (I-19 N-4) |
| `SIGPIPE` | `pipe` | catchable; maskable |
| `SIGCHLD` | `child_exit` | catchable; default ignore |
| `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` | `snare:*` | catchable; default terminate |
| `SIGHUP`/`SIGQUIT`/`SIGWINCH`/`SIGTSTP`/`SIGCONT` | `tty:*` | PTY-1 semantics |
| `SIGALRM`, `SIGUSR1/2`, `SIGABRT` | — | no note exists; see below |

`SIGTERM` sharing `interrupt` with `SIGINT` is inherited from the pouch mapping
and is a stated v1.0 imprecision, not an oversight.

**The argument domains** (V-2b's rule, unchanged):

- **`rt_sigaction` (134)** requires `SA_RESTORER` set when installing a real
  handler. Measured: musl compiles with `-D_XOPEN_SOURCE=700`, which exposes
  `SA_RESTORER` in `arch/aarch64/bits/signal.h`, so `struct k_sigaction` carries
  a `restorer` and `sigaction.c` always fills it with `__restore_rt`
  (`mov x8,#139; svc 0`). **The guest therefore supplies its own return
  trampoline** and Thylacine needs none. The flag is self-describing, so the
  struct's two shapes are distinguishable rather than guessed. A guest that omits
  it (glibc on aarch64 relies on a vDSO trampoline instead) declines — supplying
  one would mean making Thylacine's vDSO page executable, and it is deliberately
  RO+XN.
- **`rt_sigprocmask` (135)** maps onto the existing per-`Thread` `note_mask`,
  which already exists for exactly this reason ("so multi-thread Procs can have
  different threads accept different signals — POSIX `pthread_sigmask`
  semantics", `notes.h:107`). Bits outside the mapped set decline.
- **`kill` (129) / `tkill` (130) / `tgkill` (131)** map to `notes_post_pid`,
  under the *existing* I-26 two-axis gate. A phenotype must not become a way to
  signal a Proc the caller could not otherwise reach.
- **`rt_sigreturn` (139)** is the phenotyped spelling of `SYS_NOTED(NCONT)`.

`sigaltstack` (132), `rt_sigsuspend` (133), `rt_sigpending` (136),
`rt_sigtimedwait` (137), `rt_sigqueueinfo` (138) and `restart_syscall` (128) are
explicit `ENOSYS` rows — Tier 2, and recorded rather than defaulted, per the
file's standard that a number never considered and one considered-and-rejected
are different facts. `SIGALRM`/`SIGUSR1`/`SIGUSR2` have no note to carry them;
`SIGABRT` is reachable only via `raise`, which is `tkill` to self, so it
terminates rather than running a handler.

**`SA_RESTART` and the `EINTR` surface (item 11).** Until item 11 (ARCH §8.8.3,
the caught-note-interruptible sleep) no phenotype syscall could return `EINTR` at
all — a caught note never unwound a blocked wait, so the pouch boundary-line
truthfully recorded "no EINTR retry surface to enable" (patch `0007`). Item 11
*creates* that surface: a blocking syscall interrupted by a deliverable caught
note unwinds and returns `-T_E_INTR` (4), and the tail delivers the handler.
Where the restart-vs-`EINTR` decision is made is the pouch/unmodified split:
- **Pouch guest** (our patched musl): the kernel returns `-EINTR` and delivers
  the frame; musl's cancellation/`__eintr_valid_flag` machinery honours
  `SA_RESTART` in *userspace* — `SA_RESTART` re-issues the syscall, else the
  caller observes `EINTR`. This is the v1.0 path and it fully closes the
  interactive-shell case (item 8): an interactive read's SIGINT handler is
  typically not `SA_RESTART`, so the line is discarded and the prompt reprints.
- **Unmodified Vivarium guest** (libc not ours to patch): a real Linux kernel
  performs the `SA_RESTART` restart itself — rewinding `pc` by 4 and restoring
  the original `x0` so the `svc` re-executes — because the guest libc expects it.
  Thylacine does **not** rewind `pc` at v1.0; the `Thread` snapshot keeps the
  interrupted regs, so the machinery *could*, but the restart-continuation is the
  `restart_syscall` (128) ENOSYS row above. So an unmodified guest that installs
  an `SA_RESTART` handler over a blocking syscall observes a spurious `EINTR`
  rather than a transparent restart — a named DEGRADED-tier fidelity gap (§9),
  costing correctness only for a guest that both runs unmodified *and* relies on
  kernel-side restart, never authority. Kernel-side `SA_RESTART` is the natural
  Tier-2 lift when an unmodified guest needs it.

**AS BUILT (V-6c).** The frame is `siginfo_t` (128) + `ucontext_t` (4560) =
4688 bytes, plus a 16-byte `{fp, lr}` frame record above it so a backtrace from
inside a handler still walks into the interrupted code. Delivery sets the
aarch64 contract -- `x0` signum, `x1` &siginfo, `x2` &ucontext, `x29`
&frame_record, `x30` the guest's own restorer, `sp` the frame, `pc` the handler
-- and `rt_sigreturn` (139) is intercepted in `viv_linux_dispatch` and routed to
`SYS_NOTED(NCONT)`, which restores from the `Thread` snapshot.

Two layout facts that cost a measurement each, recorded so nobody re-derives
them wrong:

- `sizeof(struct sigcontext)` is **4384**, and `__reserved` begins at **288**,
  not 280. musl declares it `long double __reserved[256]`, which is 16 bytes
  per element and 16-ALIGNED on aarch64. Compiling the same declaration with the
  host `cc` on an arm64 Mac gives 2328 -- macOS `long double` is 8 bytes. The
  numbers here were confirmed under `--target=aarch64-linux-gnu` before being
  written down.
- The kernel writes only the first **600** bytes of the frame (siginfo +
  ucontext through the mcontext head) plus an 8-byte `_aarch64_ctx` terminator.
  The remaining ~4 KiB of `__reserved` is left untouched: it is the guest's own
  stack below its own sp, so nothing crosses a boundary, and an empty record
  chain remains the honest report. **CORRECTED at V-8**: the original reason
  was that note delivery did not save Q0-Q31 at all (task #96), so an FPSIMD
  record would have been a lie. Delivery now DOES save and restore them, so the
  guest's FP state is genuinely preserved -- but the kernel still does not
  serialise it into the frame, so the record is absent rather than wrong. A
  guest that only wants its registers back gets them; a guest that wants to
  READ them out of `uc_mcontext` still cannot. Emitting a real `fpsimd_context`
  is the remaining half, and it is now a pure reporting change with the hard
  part (the save) already done.

**A v1.0 Linux guest can only signal ITSELF**, and that shaped the gate. `kill`
and `tkill` are not table rows, and `clone` — which *is* one since LINEAGE L-3d —
admits only the fork and vfork shapes, never `CLONE_THREAD`. So no guest can
generate a signal for another Proc or spawn a peer thread to race its own
disposition table. (Corrected at #158: the reason used to be stated as "`clone`
is not a table row", which the clone row's landing quietly falsified.) The
`viv-pheno-probe` gate therefore uses the one self-inflicted route that exists:
the bundle declares `org.thylacine.sigpipe-selftest`, `viv` hands the entrypoint
fd 0 as the write end of a reader-less pipe, and the guest's own `write()` makes
the kernel post `pipe`. No second Proc times anything. The gate additionally
pins that a BLOCKED signal is *deferred* rather than lost: the handler must not
run while SIGPIPE is masked, and must run at the `rt_sigprocmask` that unblocks
it.

**What Tier 2 still owes**, unchanged from §5.4: queued `siginfo`, `SA_RESTART`
(a restartable syscall needs the EINTR plumbing LS-8 defers) and `SA_ONSTACK`.
Thylacine's `in_handler` blocks *all* delivery for the duration where Linux
blocks only the delivered signal plus `sa_mask` — a stated imprecision in the
conservative direction. **The MASK a handler runs under is Linux's since
2026-08-17** (aux item 7; ARCH §7.6): `note_mask` = pre-handler | `sa_mask` |
sig (omitted under `SA_NODEFER`, which is therefore honoured for the mask), and
`rt_sigreturn` restores the pre-handler mask from the kernel-side save — so a
handler's own `rt_sigprocmask` does not outlive it, and an `execve`/`fork()`
from inside a handler passes on what Linux passes on. The guard above is what
still differs, and it only defers.

---

### 6.23 Signals — handler escape, detected (bug-2, the arm-2 root)

§6.22's `in_handler` guard has a failure mode the frame shape does not, and it
assumes the thing that fails: that every handler which *starts* eventually calls
`rt_sigreturn`. A handler that **escapes** — `siglongjmp` to a `sigsetjmp` point
in the main loop, the canonical way an interactive shell abandons a half-typed
line on Ctrl-C — never returns through the kernel, so `in_handler` (set at
delivery, cleared *only* by `notes_noted_restore` and `exec`) is left **stuck
true**. The N-3 re-entrancy guard (§7.6.7) then refuses every future non-`kill`
caught note: the guest is **permanently signal-deaf** — a second Ctrl-C does
nothing — and undeliverable notes pile up until the queue reaches
`NOTE_QUEUE_DEPTH` and `notes_post` begins to fail. What §6.22 records as a
bounded imprecision (`in_handler` blocks all delivery *for the duration* of a
handler) becomes unbounded: the duration is forever. This is not a fidelity
gap; it is a correctness bug, and it fires on the single most common interactive
idiom a real shell uses.

Linux does not have this failure because it has no `in_handler` flag: it tracks
the blocked **mask**, and `siglongjmp` (via `sigsetjmp(env, savesigs=1)`)
restores the mask that was saved *before* the signal blocked it, so the handled
signal is live again the instant the jump lands. Thylacine's kernel-side
re-entrancy guard is precisely the state that needs an escape signal Linux gets
for free — so we synthesize one.

**The detector (sp-comparison).** The escape is observable from the one fact the
frame layout already pins. At delivery, `note_saved_sp_el0` is the
**pre-handler** `sp` (`notes_deliver_linux_locked`, `t->note_saved_sp_el0 =
ctx->sp`, currently `notes.c:1437`), and the handler is launched with
`ctx->sp = sigframe`, which is strictly **below** it (`notes.c:1494`;
`sigframe = next_frame − VIV_SIGFRAME_SIZE < next_frame < sp0`).
The ARM64 stack grows down, so:

- while a handler genuinely runs (including any syscall it makes),
  `sp < note_saved_sp_el0`;
- after a normal `rt_sigreturn`, `in_handler` is already clear — nothing to
  detect;
- after a `siglongjmp` escape, `sp` is the `sigsetjmp` point in the main loop —
  an **older, higher** frame — so `sp ≥ note_saved_sp_el0`.

The predicate is therefore exact for the case it names:

```
in_handler ∧ proc.phenotype == PHENO_LINUX ∧ ctx.sp ≥ note_saved_sp_el0
    ⟹ the handler has unwound above its own frame ⟹ clear in_handler
```

This is **total discrimination for a guest on a single contiguous stack**, not a
heuristic — but it rests on a stack-identity assumption, made explicit in the
cross-stack limitation below (the audit's F1 sharpened this). On one stack: a
live handler's `sp` is strictly below the saved value on *every* path — a nested
or recursed handler only pushes lower, a deep-stack handler is lower still — so
no running handler is ever mis-flagged. And a `siglongjmp` target *must* be an
**ancestor** frame *on that stack*: jumping to a `sigsetjmp` env whose function
has already returned is undefined behaviour, so a surviving `sigsetjmp` point is
necessarily older than the handler and therefore at a higher address. The escape
thus *always* trips `sp ≥ note_saved_sp_el0` and a live single-stack handler
*never* does. The claim fails only **across** stacks — a `swapcontext` from a
handler to a **higher-addressed separate** stack trips the same `≥` without being
an abandonment; that is the fix's one regression, detailed and bounded below. (Both operands are the saved SP_EL0 bank —
`exception_context.sp` is filled by `KERNEL_ENTRY`'s `mrs x10, sp_el0` and the
kernel itself runs EL1h/SP_EL1, so the comparison is never across register
banks.) Clearing `in_handler` re-arms delivery exactly as Linux's mask-restore
does.

**Two sites, and why EL0-entry is *required*, not merely preferred.** The clear
fires at **EL0-entry** (`viv_linux_dispatch`, after the `rt_sigreturn` intercept)
as the primary, and at **EL0-return** (`notes_deliver_at_el0_return`, immediately
before the N-3 guard) as defense-in-depth. EL0-return *alone* is **insufficient**,
and the reason is the interaction with the bug-1 sleep-predicate fix (§7.6.7). After
the escape, the main loop's next syscall is typically a blocking read. If the clear
waited for EL0-return, that read would *enter* with `in_handler` still stuck → it
parks → the sleep-interrupt predicate `thread_caught_note_deliverable` returns false
for a stuck handler (bug-1's gate) → a second caught signal cannot interrupt the
parked read → the thread never returns to EL0 → the EL0-return check never runs. A
deaf deadlock. EL0-entry breaks it: the escaped main loop's *first* syscall clears
`in_handler` **before** the read parks, so the park is interruptible again and a
second Ctrl-C lands. EL0-return remains as belt-and-suspenders — it self-heals the
guard at the exact point it would otherwise refuse, and covers a
fault-return-before-next-syscall window. Both sites call one pure predicate,
`thread_note_handler_escaped(t, sp)`, so the logic exists once.

**The load-bearing dependency: `sigaltstack` (132) stays `ENOSYS`.** The detector
compares `sp` against a saved `sp` *on the same stack*. If a handler could run on
an alternate stack (`SA_ONSTACK`), its `sp` would be an unrelated address and the
comparison would produce both false negatives (a live alt-stack handler read as
escaped) and false positives. `sigaltstack` is an explicit `ENOSYS` row
(`vivarium.c:256`) and `SA_ONSTACK` is a §9 residual; a `_Static_assert`/comment
at the detector ties it to that row, so anyone who later serves `sigaltstack`
must revisit this subsection. This is the design's single sharpest risk, and it
is enforced at build time rather than trusted to memory.

**What it does not fix, in two directions (the audit's F1).** *Below `sp0`,
benignly:* a `setcontext`/`swapcontext` to a context *below* `sp0` is not
detected — rare, and identical to a handler that legitimately never returns (`sp`
stays low, `in_handler` stays set, exactly as before this change). A handler that
spins deep and never unwinds is likewise undetected and, correctly,
indistinguishable from one still running. *Above `sp0`, harmfully — the fix's one
regression:* a `swapcontext` from *inside* a handler to a **higher-addressed
separate** stack (a live, suspended coroutine — **not** an abandonment) trips
`sp ≥ note_saved_sp_el0` at that coroutine's first syscall, so the detector
**false-clears** `in_handler`. The N-3 guard then admits a nested delivery, which
overwrites the single `note_saved_*` slot; when the original handler is later
resumed and `rt_sigreturn`s, it restores the *overwritten* context — silent
guest-state corruption. This is **worse than pre-`bug-2`**, which left
`in_handler` stuck and safely *deferred* the second note (the §6.22 imprecision).
The `sigaltstack`-`ENOSYS` coupling does **not** cover this: that governs where
the *handler* runs, not where a *swap target* lives. It is **contained** —
`note_saved_sp_el0` is always a validated user VA, so the wrong restore yields a
user `sp` never a kernel one; the damage is confined to a self-corrupting guest,
`in_handler` is per-Thread (no cross-Proc effect), and `kill` bypasses the whole
path. It is also **exotic** — it needs signal-driven cross-stack coroutine
switching to a higher-addressed stack, which no v1.0 target (busybox / Alpine /
Go) does. Recorded as a §9 DEGRADED row; the closing VMA-same-stack hardening
(`vma_lookup(sp) == vma_lookup(note_saved_sp_el0)` before clearing) is tracked
for v1.x. The detector adds a **third** clear edge (beside `rt_sigreturn` and
`exec`); it removes neither, and it never clears `in_handler` for a native
(`PHENO_NATIVE`) Proc — a native handler's longjmp discipline is a separate
question this does not touch.

The net result: the **common** escape case moves *out* of §9's DEGRADED tier — a
phenotyped handler may `siglongjmp` out of itself on one stack, as real shells
do, and the guest stays signal-live — while the **cross-stack** case above moves
*into* a new §9 DEGRADED row (the F1 regression). What remains degraded is
§6.22's genuine imprecision — a *running* handler briefly defers other signals,
now the bounded thing it always claimed to be — plus that one exotic cross-stack
corruption, until the tracked v1.x VMA-same-stack hardening closes it.

**The proof — a fails-without-fix driver, not a regression net.** The
interactive witness `r5f9-ash.exp` (busybox `ash` Ctrl-C) is a *regression net*,
not a control: it passes 6/6 on a kernel **without** the fix, because `ash`
reprompts and never takes a *second* caught signal while the latch is stuck — so
it can only guard against a future regression, never demonstrate the fix. The
deterministic control is legs L245-L248 of `viv-pheno-probe` (the boot-time
`/vivarium/pheno` bundle). It hand-rolls a `setjmp`/`longjmp` pair (no libc), a
`PHENO_LINUX` handler `siglongjmp`s out of itself on the first self-raised
`SIGPIPE` (a one-byte write to the reader-less fd 0), the escaped main loop
unblocks and delivers a **second** `SIGPIPE` across the escape, and L248 asserts
the handler fired **twice**. Without the clears the stuck `in_handler` makes the
N-3 guard refuse the second delivery — the handler fires once, L248 is red, and
joey reports `V-1b linux-phenotype leg FAILED marker=L248` (boot-fatal).
**Measured both ways** (both call-site clears disabled → `marker=L248`; restored
→ `V-1b phenotype ... PASS`), so the two clears now have an in-guest driver that
discriminates the fix from its absence. It exercises the two clears *jointly* —
the EL0-entry clear on the post-escape unblock does the work here, the EL0-return
copy is idempotent behind it; isolating EL0-entry alone would need a park-based
driver (a deaf-deadlock hang rather than a clean marker) and is not built.

### 6.24 Tier 2 — the path-mutation family: `openat(O_CREAT)` + `mkdirat` + `unlinkat` + `renameat`/`renameat2` (#50; design ratified 2026-08-25)

**The problem.** §6.20's Correction 1 proved `O_CREAT` cannot be *routed* to
`SYS_WALK_CREATE` — three independent blockers (shape / semantics / the silent
cwd-sentinel divergence), any one fatal. That verdict stands, and it was a
verdict about **routing**, not about the feature: the kernel half wants a
create-by-**path** primitive that did not exist. This section designs it. The
forcing consumer is git (the arc opened by the operator 2026-08-25): `git init`
alone needs create + mkdir + unlink + rename (`config.lock` → rename-into-place),
and every one of `mkdirat`(34) / `unlinkat`(35) / `renameat`(38) was an unnamed
number → ENOSYS.

**Prior art, which collapses the design space:**
- **Plan 9 (heritage).** `create(2)` *is* path-based; the kernel's
  `namec(Acreate)` walks to the parent, tries the create, and falls back to
  open when the create loses an exists-race. The create-else-open composition
  lives **in the kernel**. Thylacine dropped the path-create half at stalk-1
  in favor of the 9P-shaped single-component `SYS_WALK_CREATE`; this chunk
  restores it. `ARCH §11.2`'s core-syscall table has listed a path-based
  `create(name, mode, perm)` since Phase 0 — the mint **fulfills** standing
  scripture rather than deviating from it.
- **Linux v9fs (SOTA for a 9P-backed FS).** `open(O_CREAT)` without `O_EXCL`
  is a bounded client loop: try open → ENOENT → `Tlcreate` → EEXIST → retry
  open. Close-to-open consistency; the loop is the accepted idiom.
- **Fuchsia (capability SOTA).** Create is an operation on the parent
  *directory connection*, atomic at the server — which is exactly our
  `dev->create`/`Tlcreate` at Stratum.

Both models agree: **exclusive create is atomic at the server; open-if-present
is a bounded client loop.** There is no third idiom.

**The ratified forks (operator, 2026-08-25, AskUserQuestion):**
1. **Full family in one chunk** — `openat(O_CREAT)` + `mkdirat` + `unlinkat` +
   `renameat` ride ONE new primitive and get ONE audit round, rather than
   re-auditing the same helper across sequential chunks.
2. **Mint the native syscall too** — `SYS_OPEN_CREATE = 108` joins the native
   ABI on the same core. The witness adopter is `libthyla-rs`'s
   `open_create_at_path` (`fs/file.rs`), which today hand-rolls the
   split + parent-`T_OPATH` + `WALK_CREATE` dance in userspace — rewiring that
   ONE function adopts every native `File::create` caller (coreutils, ut
   redirects, corvus) at a stroke, and retires its stale create-first
   rationale ("walk_open does not return a distinguishable not-found code" —
   false since the errno rollout gave stalk `T_E_NOENT`).

**The design — one new mechanism, five consumers:**

1. `sys_split_parent_kpath(...)` (kernel/syscall.c) — the missing primitive:
   cwd-join with **SYS_OPEN parity** (`territory_join_cwd`, killing blocker 3
   — the join happens BEFORE the split, so the parent resolves exactly where
   `SYS_OPEN` would resolve the whole path), then a **lexical split at the
   last component only** (the libthyla `split_parent_leaf` rows, #87: the
   split classifies, never resolves — `.`/`..`/root leaves reach the caller's
   POSIX row), then `stalk` the parent prefix walk-only. Containment (I-28)
   and symlink expansion in the prefix are inherited from stalk — no new
   resolution mechanism exists for an audit to find holes in.
2. `spoor_create_in_dir(...)` — the create mechanics **extracted from**
   `sys_walk_create_handler` (dev-slot checks, QTDIR, the A-2d W|X parent
   gate, clone-walk, `dev->create`, gid stamp), one implementation for the
   native handler + the new core — the "extracted rather than duplicated"
   rule the I-43 row already imposes on every T2 shell.
3. `sys_open_create_kpath_for_proc(p, start_fd, path, len, omode, perm)` —
   the loop. Semantics rows:
   - `OEXCL` (the pre-reserved `0x1000` omode bit): create-first, once;
     EEXIST is the honest answer (atomic at the server — git lockfiles get
     real exclusivity).
   - plain create: **open-first** (the common existing-file case pays one
     RPC; `OTRUNC` composes on the open leg and is STRIPPED on the create
     leg — a fresh file is already empty); on `T_E_NOENT` → create; on the
     create losing an exists-race (`EEXIST`) → retry open; **bounded at 2
     rounds** then the last real error, loud.
   - `perm` carries `DMDIR` → mkdir semantics: create-only (EEXIST if
     present), the `OEXCL` arm with a directory — `mkdirat` is this row.
   - EISDIR rows (Linux `open_last_lookups` parity, already libthyla's):
     trailing-slash, `.`/`..`, and root leaves answer EISDIR on any create.
   - `NOFOLLOW` composes (live final symlink → ELOOP, absent → create);
     `OPATH` is **rejected** (a navigation handle cannot want creation).
4. The native `SYS_OPEN_CREATE = 108` handler — the user-buffer front of (3),
   `(start_fd, path_va, path_len, omode, perm)`, mirroring `SYS_OPEN` + perm.
   FROM_ROOT joins cwd; an explicit `O_PATH` dirfd start works as in
   `SYS_OPEN`.
5. The viv rows (decides pure, shells in the socket-row pattern):
   - `openat` gains the `O_CREAT` domain: `AT_FDCWD` only (same narrowing,
     same handle-state reason as the existing row), admitted flags +
     `O_CREAT`/`O_EXCL`/`O_TRUNC`, mode = low-9 bits (07000 bits → decline,
     census-visible).
   - `mkdirat`(34): `AT_FDCWD` + low-9 mode → core with `DMDIR`.
   - `unlinkat`(35): `AT_FDCWD`; flags 0 ↔ file, `AT_REMOVEDIR`(0x200) ↔
     `SYS_UNLINK_REMOVEDIR` — a 1:1 map onto the native unlink mechanics run
     on the split parent.
   - `renameat`(38) + `renameat2`(276, flags==0 only): two parent splits →
     the native rename mechanics (Linux replace-existing atomicity IS
     `SYS_RENAME`'s documented contract — 1:1).

**Documented degradations (loud or cosmetic, none silent-wrong):**
- A **dangling** final symlink + `O_CREAT` answers EEXIST where Linux creates
  the *target* (the open-first leg sees ENOENT, the create sees the link name
  occupied). git never creates through dangling links; recorded, not built.
- No umask: the guest's `umask` syscall is ENOSYS and the kernel applies no
  mask, so modes arrive unmasked (0666 where Linux yields 0644). Cosmetic
  under A-2d; passing through literally beats inventing kernel state.
- `O_APPEND` stays rejected (milestone A runs git with reflogs off);
  `O_DIRECTORY` stays rejected wholesale as today.
- Real (non-`AT_FDCWD`) dirfds stay out — the §6.20 Correction 2 handle-state
  blocker is untouched by this chunk.

**Invariant framing:** I-28 inherited whole (stalk resolves the parent; the
split is lexical-only). I-43 holds by construction (the family runs the SAME
stalk, the SAME A-2d gates, the SAME create mechanics as native callers —
extraction, not duplication; shape conferred, zero new authority). I-22/I-32
untouched. The chunk is **audit-bearing** (a new native syscall + four
phenotype rows on the FS-mutation surface); its row joins
`docs/AUDIT-TRIGGERS.md` + the CLAUDE.md index with the impl commit, and the
holotype round spawns AFTER the whole surface is complete (the curl-chunk
lesson).

**Race honesty:** two racing creators of one leaf converge (loser's EEXIST →
open succeeds); two racing `O_EXCL` creators — exactly one wins (server-
atomic); `O_CREAT|O_TRUNC` racers may truncate each other exactly as on
Linux. The loop bound turns pathological churn into a loud error, never a
spin. Prose-validated (spec-to-code suspension); no new wait/wake, no new
lock — the only serialization is the 9P client's existing per-RPC order.

**Deliberately next, not silent:** `getdents64`(61) (a self-contained
9P-dirent → `linux_dirent64` format row) + `fsync`(82)/`fdatasync`(83)
(trivial fd delegation) are the follow-on chunk; `faccessat`(48) is measured
at git time before deciding.

### 6.25 Tier 2 — `getdents64` (61) + `fsync`/`fdatasync` (82/83) + the `O_DIRECTORY` admission (the §6.24 follow-on; as-built 2026-08-26)

The three rows §6.24 named "deliberately next". The forcing consumer is
unchanged (git: `readdir` over `.git/objects`, `core.fsync` paths), and the
first blocker is upstream of the rows themselves: **musl's `opendir` opens with
`O_RDONLY|O_DIRECTORY|O_CLOEXEC`, so while `O_DIRECTORY` stayed on the V-2b
reject list, `getdents64` was unreachable** — every `ls`/`readdir` died at the
`openat` before the new row could matter.

**`O_DIRECTORY` — admitted as a decide OUTPUT, enforced by the shell.** The
plain-open decide gains a fourth output, `dir_required` (written only on
TRANSLATED — the forwards-leave-outputs-alone contract; a NULL out-pointer is
permitted). The openat shell enforces it as a **postcondition on the minted
Spoor's own qid**: after the open lands, `sys_lookup_spoor` + `QTDIR` check →
on a non-directory, `handle_close` + `ENOTDIR`. No extra RPC and no TOCTOU —
the qid examined is the one the open itself returned, not a re-resolve. The
create decide still declines the flag (`O_CREAT|O_DIRECTORY` is contradictory
on Linux in exactly the way the decline reports), and the §6.20 rejected-flags
narrative carries the row's RETIRED note (the `O_NOFOLLOW` pattern).

**`getdents64` (61) — a pure format row on the existing readdir mechanics.**
The native handler's core is extracted as `spoor_readdir_run` with a
**no-offset-advance contract**: the helper reads raw 9P dirents into a caller
buffer and reports the last cookie, and each caller commits `c->offset` only
after its own copy-out succeeds — so a faulting user buffer cannot advance the
cursor (the F3 fault property; the native `sys_readdir_handler` keeps identical
behavior through the same helper). The phenotype arm then runs a pure
transform, `viv_dirent64_encode_run`: 9P dirent → `linux_dirent64` with
`d_ino ← qid.path`, `d_off ← the resume cookie`, `d_type` passed through
verbatim (9P and Linux share the DT numbering), `d_reclen` 8-aligned
`19 + namelen + 1`. Whole records only: the encoder stops at the first no-fit
and reports the last **emitted** cookie, so the committed cursor never points
past what the guest actually received. Buffer split 2048 raw → 2560 encoded;
the worst-case growth ratio is `align8(20+n)/(24+n)` at `n == 5` (32/29), and
`2048 × 32/29 = 2260 < 2560`, so the encode can never overrun. Contour rows:
`count == 0 → EINVAL`, no-`RIGHT_READ` fd → `EBADF`, a non-QTDIR qid →
`ENOTDIR`, and a first-record no-fit (`emitted == 0` with raw bytes in hand)
→ `EINVAL` — each Linux's own answer.

**`fsync` (82) / `fdatasync` (83) — T2 shells with an explicit datasync
argument.** Both route to the native `sys_fsync_handler(fd, datasync)`; the
shell passes the datasync bit **explicitly** because a T1 renumber would copy
the six argument words verbatim and the native handler would read garbage in
x1. Divergence, documented not silent: the native gate requires `RIGHT_WRITE`,
so an `fsync` on an `O_RDONLY` fd answers `EBADF` where Linux syncs — git
milestone A runs `core.fsync=none`, and the row is revisited if a consumer
actually syncs read-only fds.

**Errno rollouts on the shared native handlers** (the boundary rule: a bare
`-1` crosses the viv boundary as a fabricated `EPERM`): `sys_readdir_handler`
now answers `-T_E_BADF` (CWALKONLY fd), `-T_E_OPNOTSUPP` (no readdir slot),
`-T_E_IO` (malformed server dirent), and passes dev errors verbatim;
`sys_fsync_handler` answers `-T_E_BADF` / `-T_E_OPNOTSUPP` for its two
formerly-bare rows.

**The E2E gained the getdents64 leg — and the leg's ^C neighbor got its race
fixed.** `viv-run.exp`'s pts block now runs `ls /tmp/d50 | tr a-z A-Z` → `G50`
(busybox `ls` → musl `readdir` → the 61 row, as a plain user). Adding that 4th
leg deterministically re-timed the following ^C leg into a failure that a
counter-instrumented hunt (2026-08-26) ran to ground: the ^C was being sent
the instant leg 4's output matched, landing inside busybox-ash's reap window
where the shell still holds `SIGINT=SIG_IGN` — and the §6.19 V-6b ignore-drop
then discards the note at post time, exactly as Linux discards a
generated-while-ignored signal. The scenario now settles before the ^C
(enforcing the leg's stated "^C at the ash prompt" precondition), and the hunt
**measured the whole caught-note chain live** on the way: fan → arm → wake of
the parked elected 9P reader → `SLEEP_NOTEINTR` → `CLIENT_WAIT_NOTEINTR` →
`EINTR` → handler → prompt (the wake-of-a-parked-reader leg had never been
exercised before). No kernel defect; the byte-level capture showed ash's read
returning the post-^C line intact.

---

### 6.26 Tier 2 — the git chunk: `faccessat`/`chdir`/`fchmodat`/`readlinkat` (48/49/53/78) + `geteuid`/`getegid` (175/177) + `getrandom` (278), and the three walls (milestone A: `init` + `add`, as-built 2026-08-26)

The forcing consumer is **git** — a real static aarch64 musl `git 2.51.2` (built
NO_CURL / NO_REGEX=NeedsStartEnd against a static zlib on the thyla-pi silicon
host), driven under the phenotype. Milestone A is `git init` + `git add`
end-to-end; `commit` + `clone` are §6.27 (they need the reflog's `O_APPEND`,
which the phenotype `openat` does not yet admit). Getting `init` + `add` to run
crossed **three walls**, and only the first is a syscall-translation problem.

**Wall 1 — the seven missing numbers.** `git version` aborts before it prints
unless `access(R_OK)` on `/etc/gitconfig` returns something other than `ENOSYS`;
`git init` cannot enter the tree it just made without `chdir`, cannot write
`core.filemode` without `fchmodat`, and dies in its path canonicalizer without
`readlinkat`; `git add` cannot name a temp object without `getrandom`; and both
git and ash read `geteuid`/`getegid` for their "am I root" checks. Each row was
`FORWARD`ing to the supervisor (`ENOSYS`). The seven:

- **`faccessat` (48)** — the raw 3-arg `faccessat(dirfd, path, mode)` (NOT the
  4-arg `faccessat2`/439; musl's `access()` and `faccessat(...,0)` both issue
  the 3-arg number, so there is no flags word). It is `newfstatat`'s front half
  joined to a `perm_check`: resolve the path (follow symlinks — there is no
  `AT_SYMLINK_NOFOLLOW` here), then answer the mode question. `R_OK`=4/`W_OK`=2/
  `X_OK`=1 map 1:1 onto `PERM_R`/`PERM_W`/`PERM_X`; `F_OK`=0 asks only existence
  (the stat succeeding IS the answer); any bit outside `0x7` is `EINVAL`, judged
  in the shell (the mmap-judges-`len` precedent). A resolution failure IS the
  answer — `ENOENT`/`ENOTDIR`/`EACCES` from the walk flow straight back.
- **`chdir` (49)** — measures the length Linux leaves implicit, then delegates
  to the native `SYS_CHDIR` (which reads + validates the path itself). The
  native handler collapses every failure to a bare `-1`; the shell maps that to
  `ENOENT` (the dominant cause; the `ENOTDIR`/`EACCES` collapse is a documented
  fidelity gap for a richer native errno path, and the SUCCESS path — the only
  one milestone A exercises — is exact).
- **`fchmodat` (53)** — opens the path `O_PATH` (chmod needs OWNERSHIP, never
  read, so the `perm_check`-exempt navigation handle is the correct base) and
  applies the mode through the audited `sys_wstat_for_proc`, whose
  `perm_wstat_check` IS the POSIX owner-or-CAP gate. Only the 9 rwx bits
  (`& T_WSTAT_MODE_MASK`); setuid/setgid/sticky are dropped (T_WSTAT rejects
  them at v1.0 — a documented, not silent, gap). The RAW syscall 53 is 3-arg
  (`fchmodat(dirfd, path, mode)`; the flags-bearing variant is `fchmodat2`/452,
  a distinct number), so there is no flags word — `args[3]` is undefined
  register residue and is never read (the F1 audit correction: reading it
  spuriously `EINVAL`'d a valid chmod on any binary that left x3 nonzero; the
  sibling faccessat row makes the same point). The `O_PATH` fd is closed on
  every return path.
- **`readlinkat` (78)** — resolves the path `NOFOLLOW` (the link ITSELF is the
  quarry), checks `QTSYMLINK` + a `.readlink` Dev slot (a non-symlink answers
  `EINVAL`, the POSIX contour git's resolver relies on to treat a component as a
  plain file), reads the target via the slot, and copies `min(target, bufsiz)`
  out — `readlink(2)` does NOT NUL-terminate and returns the byte count. **The
  copy-out validates the exact span with `sys_validate_user_buf(buf_va, n)`
  BEFORE `uaccess_copy_out`** (the getdents64 P0 class: `uaccess_copy_out`'s
  fault fixup engages only for user-half VAs — an unvalidated kernel-half `buf`
  would extinct or corrupt kernel memory).
- **`geteuid` (175) / `getegid` (177)** — the exact twins of `getuid`/`getgid`
  through the same `vivarium_map_uid`/`gid`. Thylacine carries ONE principal per
  Proc (no real-vs-effective split — I-22: authority is the capability set, not
  a uid), so effective == real.
- **`getrandom` (278)** — the native `SYS_GETRANDOM` is shape-identical (buf,
  buflen, flags) and does its own validation + copy-out. It gates on
  `CAP_CSPRNG_READ`, and **that gate is KEPT under I-43**: a phenotype confers
  Linux's numbering and semantics, never authority — a container that draws
  entropy must be granted the capability (see Wall 3).

The **collision arguments**: 48/49/53/78 are below the `VIV_NATIVE_CEILING`
(108), so each owes a per-number paragraph in `vivarium.h`. 48/53/78 collide
with native `SYS_NOTE_MASK`/`SYS_PIVOT_ROOT`/`SYS_PCI_INFO`, and all three share
the **AT_FDCWD gate** (`vivarium_faccessat_decide`, which admits ONLY
`dirfd == -100` as a sign-extended `s32`): the colliding native arg (a note
bitfield, a Spoor fd, a PCI handle) is a small non-negative value, never the
`-100` sentinel, so a mis-declared native caller `FORWARD`s to `ENOSYS` on
shape. 49 is FD-less (no gate), so it carries the getdents64 family's DAMAGE
ENVELOPE instead: `chdir` reads `args[0]` as a path in the CALLER's OWN memory
and at most moves the CALLER's OWN cwd under its OWN identity — a native
`SPAWN_FULL_ARGV` pointer read as a path resolves to garbage and fails, nothing
is spawned, no authority crosses. 175/177/278 are above the ceiling —
collision-free by construction, `_Static_assert`ed in `vivarium.c`.

**Wall 2 — the pool is SYSTEM-owned, so git runs as SYSTEM.** The pool 9P mount
is served by the kernel's single shared connection to stratumd, and that FS is
system-owned (`syscall.h`: "dev9p reports `PRINCIPAL_SYSTEM`"). So every file
ANY container creates on the pool is stamped `PRINCIPAL_SYSTEM`, regardless of
the creating principal — and git's config write chmods its own lockfile, which
requires OWNERSHIP. A container running as a real user (e.g. uid 1000) is denied
the chmod on its own file and `git init` dies. **Per-principal 9P ownership is
A-3, unbuilt at v1.0** (tracked as a separate arc). Milestone A therefore runs
git as a **SYSTEM-principal boot probe** (`do_git_probe_gate` in joey), which
OWNS the SYSTEM-stamped files, so the chmod succeeds. This proves the phenotype
mechanics (the seven rows + Wall 3) end-to-end; git-as-a-real-user waits on A-3.

**Wall 3 — the phenotype fork must INHERIT caps.** With git running as SYSTEM,
`git init` succeeds but `git add` fails at `getrandom` — the forked git has no
`CAP_CSPRNG_READ`. Root cause: Thylacine's `rfork_forked` passes `CAP_NONE`, so
a forked child's caps are `parent_caps & 0 = 0` — **fork zeros caps**. Caps are
conferred only via explicit `rfork_with_caps` at spawn. But git is FORKED (via
`clone`) from the entrypoint shell, so it lost every cap the container was
granted. This is an I-43 fidelity gap: **Linux forks INHERIT the parent's
capabilities; Thylacine's phenotype fork zeroed them.** The fix (new
`rfork_forked_with_caps`, taken by the `PHENO_LINUX` arm of `sys_rfork_core`
with `CAP_ALL` as the mask): a Linux fork inherits, so the phenotype path forks
with a full mask, which `rfork_internal` still intersects with the parent's
actually-held caps and still strips `~CAP_ELEVATION_ONLY` unconditionally —
`child->caps = (parent_caps & CAP_ALL) & ~CAP_ELEVATION_ONLY`. So the child gets
exactly `parent_caps` minus elevation: **I-2** holds (`child <= parent`, never
grown), elevation (HOSTOWNER/DAC_OVERRIDE/CHOWN/KILL) never propagates by
inheritance, and **I-43** is satisfied (the SHAPE is Linux's inherit; the child
gets only authority the parent already held, which its launcher conferred
explicitly). **NATIVE fork is unchanged** — `rfork_forked` keeps `CAP_NONE`
(Thylacine's stronger fork-zeros-caps default; a native program confers caps at
spawn, never by inheritance). This fixes every capability-using phenotype
program forked by a shell, not just git.

The **cap-conferral chain** carries `CAP_CSPRNG_READ` from the trusted boot down
to the forked git, each hop intersecting (I-2, never growing): joey grants it to
`viv` (the git-probe gate's `run_viv_bundle(..., T_CAP_CSPRNG_READ)`); `viv`
confers it on the entrypoint when the bundle sets `org.thylacine.csprng:
granted` (symmetric with the existing `org.thylacine.net` grant — `cap_mask`
masks against viv's OWN caps, so viv can pass on only what its launcher granted
it); the entrypoint git then FORKS children that inherit it (Wall 3). Absent the
annotation the container's cap floor stays 0 (no ambient authority).

**The gate.** `do_git_probe_gate` (joey, SYSTEM, boot-probe-gated) spawns
`viv run /vivarium/git-probe`, whose `/gitprobe.sh` runs `git init` + `git add`
and emits `GITPROBE-INIT` / `GITPROBE-ADD` / `GITPROBE-DONE`. The gate asserts
the terminal `GITPROBE-DONE` and reports the first missing step (a container
that dies at INIT and one that dies at ADD are different bugs). It SOFT-SKIPs
when the static-git tarball is absent (the default build stays hermetic) and is
BOOT-FATAL when present (a gate that cannot redden is a disabled test).
`commit`/`clone` markers are deliberately NOT asserted — they await §6.27.

**Deferred to §6.27 (sub-chunk 2):** `commit` + `clone file://` open the reflog
`.git/logs/HEAD` with `O_APPEND`, which the phenotype `openat` does not admit
(Thylacine has no kernel append mode; pouch ports emulate it in libc, a raw
Linux binary cannot). A phenotype `O_APPEND` (open-at-EOF, sound for the
single-threaded phenotype; for git's absent reflog the open need only
RESOLVE→`ENOENT` instead of FORWARD→`ENOSYS`) is the next chunk.

---

### 6.27 Tier 2 — `O_APPEND` (via FS pass-through) + `pread64`/`pwrite64` (67/68): `git commit` + `clone` (as-built 2026-08-26)

Milestone A stopped at `init` + `add`; this arm makes `git commit` + `git clone
file://` run — the full chain (`init`/`add`/`commit`/`log`/`clone`/`verify`,
reflogs ON) now passes under the phenotype as SYSTEM. Two walls, both small, both
NOT the kernel-append-mode the §6.26 deferral feared.

**Wall 1 — `O_APPEND`, delegated to the FS.** git's ref update creates + appends
the reflog `.git/logs/HEAD` with `O_CREAT|O_WRONLY|O_APPEND`, and the phenotype
`openat` was rejecting `O_APPEND`. The key finding: **Stratum already implements
O_APPEND end-to-end** — its 9P server stores the fid's open flags at `Tlopen` and,
on every `Twrite` to an `O_APPEND` fid, ignores the client offset and writes at
the file's current size (`server.c` `h_write`; `_Static_assert(STM_9P_O_APPEND ==
O_APPEND)`). So the kernel needs no append MODE of its own — it just **passes the
flag through**. The plumbing: a new omode bit `SYS_WALK_OPEN_OAPPEND` (0x40,
inside the widened `SYS_WALK_OPEN_OMODE_VALID` 0xB3→0xF3, pinned by a
`_Static_assert`); `dev9p_open` AND `dev9p_create` map it → `O_APPEND` (02000) in
the `Tlopen`/`Tlcreate` flags; both phenotype openat decides admit `O_APPEND`
(`VIV_OPENAT_ADMITTED += VIV_O_APPEND`) and set the omode bit (the plain decide
drops it under `O_DIRECTORY` — append on a read-only dir is vacuous). This is the
append face of "the filesystem is the OS": **the kernel's write path and cursor
are unchanged; the FS positions the write.** For an append fd the kernel's
`c->offset` is advisory (Stratum ignores it) — exactly correct for a write-only
append (git's reflog), and a mixed read+append on the same fd sees the tracked
cursor, best-effort. The `syscall.h` `SYS_PWRITE` stance note is updated to record
the delegation (the native `SYS_RW` path still carries no append bit, so pouch
ports keep emulating it above that layer).

**Wall 2 — `pread64`/`pwrite64` (67/68), the clone pack read.** With `O_APPEND`
in, `commit` + `log` passed but `clone` failed reading the fetched pack:
`error reading from ...pack: Function not implemented` (ENOSYS). git's
`index-pack` reads the pack via `pread`, and `pread64`(67)/`pwrite64`(68) were
untranslated. Their `(fd, buf, count, offset)` shape matches `SYS_PREAD`(85)/
`SYS_PWRITE`(86) exactly, so they are pure **T1 renumbers** — no shell. They are
sub-ceiling, colliding with the native LOOM pair (67=`SYS_LOOM_REGISTER`,
68=`SYS_LOOM_ENTER`); the collision argument is the `read`/`write` renumbers'
damage-envelope (a renumber runs the native handler with the caller's OWN args,
and a mis-declared LOOM caller's loom handle is not a `RIGHT_WRITE` Spoor, so
`SYS_PWRITE` fails clean — at worst it touches the caller's own file via its own
fd rights). I-43 holds: a renumber confers no authority the native handler does
not already gate.

**What this is NOT:** no kernel append mode, no new write-path mechanism, no ABI
break (the omode bit is additive; native opens that don't set it are unaffected).
The whole arm is "carry two flags/numbers to machinery that already exists" —
Stratum's server-side append and the native pread/pwrite handlers.

---

## 7. The vivarium — the container runner

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

### 7.2.1 The diorama channel — a private pipe pair, no name (as-built 2026-08-17)

**How the runner reaches its diorama.** V-7 as first built had the
per-container diorama post the fixed name `/srv/viv-dio` and `viv` open it.
That cornered the design three ways at once: the boot `SrvRegistry` never
frees a dead entry (#33), so the name had to be fixed, so two concurrent
containers collided (V-8 F3 made the collision fail closed instead of open);
and posting needs `MAY_POST_SERVICE`, which every joey-spawned boot `viv` was
handed and **no session shell's `viv` ever held** — so an interactive
`viv run` failed at its very first spawn (`viv: spawn /bin/diorama`) and no
gate noticed, because every gate ran the privileged twin of the path.

**The resolution is Plan 9's:** a 9P server a process starts for itself is
reached by `mount(fd)` over a pipe; `srv(3)` exists to *publish* an fd to
strangers, and this channel has no strangers. `viv` makes two Plan 9 pipes,
spawns `diorama --vivarium <its pid>` with the server ends as the child's fds
0/1 and nothing else, attaches the client ends with `SYS_ATTACH_9P` (the
Phase-5 `stub-driver` transport, its first production consumer) and mounts the
root at `/dio` exactly as before. The diorama serves that one connection until
EOF. Consequences, each now a gated fact rather than a note:

- **No privilege.** `viv` passes no perm bits and needs none; joey's boot
  `viv run`s pass none either, so the gates run the interactive path.
- **No name, no collision.** Concurrent containers moved from §9's OUT list to
  IN; the V-7 boot leg runs two `viv run /vivarium/probe` at once and each
  probe asserts its pid view is exactly `{self}` — the F3 property (A never
  shows B) proven from the inside, under concurrency.
- **The attach gate is structural.** Nobody but the runner holds an end, so
  the peer-pid check in `h_attach` and the joey `#101` deny leg are gone; what
  the diorama still verifies at startup is its one scoping premise, that the
  argv runner is its parent (membership descends from that pid).
- **`self` is derived, not stamped.** With no `SYS_SRV_PEER` on a pipe, the
  diorama's peer is the runner it was spawned by — pid from argv checked
  against its own ppid, ids its own (`t_getuid`/`t_getgid`, inherited by the
  plain spawn), liveness a native `/proc/<runner>` resolve. Same content the
  stamp gave (the mounter); the #90 caveat is unchanged.
- **^C is the container's, never the runner's** (2026-08-18). An interactive
  `viv run` is the terminal's foreground job, so the pts's `interrupt` reaches
  its whole pgrp — `viv`, its diorama, the container. The container's shell
  handles SIGINT; the two native members used to DIE of it (LS-5's uncaught
  default), orphaning the shell into a terminal it then shared with the outer
  `ut`. `viv` masks `interrupt` (the tty family stays unmasked so ^Z stops it
  with the container and the shell's job control sees the stop); the diorama
  masks `interrupt` and the tty family (a server never dies of a keystroke —
  its lifetime is its channel's). A spawned child starts with a zero mask, so
  the container inherits nothing.

The capability-microkernel reading is the same answer (a component's private
service channel arrives in its startup handles), which is why this went in as
a routine correction rather than a design fork. Residual, recorded rather than
built here: a kernel-internal pipe write to a dead reader posts the writer a
`pipe` note (`kernel/pipe.c`), so a container Proc touching `/proc` after its
diorama has died — an orphan outliving its runner, or a diorama crash — gets a
`SIGPIPE`-shaped note where the `/srv` transport gave only an error; the fix is
a `MSG_NOSIGNAL`-shaped transport write on the Pipe audit surface
(`docs/AUX-ROADMAP.md`). Reference: `docs/reference/145-vivarium.md`, "The
diorama channel is a private pipe pair".

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
| Signal delivery + `rt_sigreturn` (§6.22) | **RESTATED at V-6.** The original hazard — "restores `pstate`/`pc` from user memory, a classic privilege-escalation shape; must reject any frame that would elevate" — describes Linux's mechanism, and §6.22 does not build it: the restore reads the kernel-side `Thread` snapshot, so no user-frame field reaches `pstate`/`pc`/`sp` and there is no validator to get wrong. The obligation becomes proving that structural property holds on every path, plus: delivery is on the **death/notes lineage** (I-9/I-19, #809/#811/LS-5), so prosecute no-lost/no-double delivery, `kill` staying non-catchable through the phenotype, and the frame push failing *closed* (an unwritable user stack must re-enqueue, never half-deliver). `kill`/`tkill`/`tgkill` must not widen I-26's two-axis gate. **The §6.23 handler-escape detector rides this same surface** (V-6d / bug-2): prosecute that the `sp`-comparison never clears `in_handler` for a live handler (no false-clear that would drop the N-3 guard mid-handler and admit a re-entrant delivery), that both operands are the SP_EL0 bank (no cross-bank compare), that clearing `in_handler` cannot itself lose or double a delivery, that the `PHENO_LINUX` gate keeps it off the native path, and that the `sigaltstack`==`ENOSYS` precondition (its load-bearing coupling) holds. |
| Socket translation | Every `/net` op must run with the *guest's* authority, never the supervisor's (I-43 + I-1). |
| The diorama servers | `/proc/<pid>` cross-Proc reads are the #57a-F2 class (UAF/lifetime under `g_proc_table_lock`) and an info-leak surface (KASLR, other principals' data). |

---

## 9. Scope — the fidelity ladder (the WSL1 lesson)

Published honestly, because §2.3.4 says scope discipline is what decides success.

**v1.0 target**: *a pre-built `musl`-static Linux ARM64 binary runs, does file I/O
and network I/O, and exits correctly; an Alpine container runs a shell and a
non-trivial script.* Concretely `curl`, `wget`, `python3`, `busybox`, `redis-cli`.

**Status at the V-8 close — the target's first clause is MET and its second is
NOT, and the honest thing is to say which.** A single-process static Linux binary
runs, opens and reads and writes files through the diorama, completes a TCP
round-trip in both directions, takes a signal through a handler it installed, and
exits through Linux `exit_group`. That is the first clause, and it is boot-gated
on every build. The second clause — *a shell* — is **not met and was never
chunked**: §10's own note records that no V-chunk builds `clone`/`execve`/
`wait4`, and a shell forks before it does anything else. So a guest shell reaches
`ENOSYS` at its first fork.

This is a gap between the arc as chunked and the arc gate, not a defect in
anything built; it is task **#93**, the named next chunk. It is written here
rather than only in §10 because §9 is the document a reader consults to learn
what works, and a scope contract whose headline claim is contradicted three
sections later is exactly the WSL1 failure this section exists to avoid.

- IN: the §11.5 top-50, BSD sockets via `/net`, **static ELF *and everything a
  statically-linked guest shell does with it***, the diorama, `viv`, signals
  Tier 0 **and Tier 1** (V-6 landed both — a real handler installs, runs, and
  returns through `rt_sigreturn`).
- **PROCESS CREATION MOVED OUT → IN at the L-6c gate (`1237dc2f`), and this
  paragraph was flipped at L-7 when the flip was noticed to be owed.** The
  condition this entry set for itself was "an Alpine `/bin/sh` actually running a
  command, **not** any earlier chunk's landing" — because moving it when the
  mechanism merely exists would reproduce the WSL1 failure §9 exists to prevent.
  That condition is met and stays met on every boot: `L6C-A`..`L6C-I` drive a
  real Alpine `busybox sh` through running, exec'ing an external program,
  reporting zero and nonzero status, a pipeline, a command substitution, a loop,
  a nested shell, and a reap — and the gate is boot-fatal, so a regression stops
  the boot rather than quietly reverting this line.
  **Read the qualifier, because it is the whole scope of the claim: the gate's
  binary is `busybox-static`.** What is IN is a statically-linked Linux guest
  doing process creation. A STOCK Alpine rootfs is still two mechanisms away and
  both are OUT below: every stock Alpine binary is `ET_DYN`/PIE with a
  `PT_INTERP` (task #145), and a stock rootfs carries 335 symlinks including
  `/bin/sh` itself, which `stalk` does not resolve (task #146).
- OUT: dynamic linking (`ET_DYN`/PIE + `PT_INTERP` — #145; **the single largest
  remaining gap between "a Linux binary runs" and "a Linux distro runs"**),
  symlink resolution in `stalk` (#146),
  `epoll` (v1.1 candidate), `inotify` (degrade), `io_uring`, `bpf`,
  `perf_event_open`, `ptrace`, glibc-dynamic (best-effort), `AF_INET6`,
  cgroups/seccomp, full signal fidelity (Tier 2), and in-guest OCI image
  acquisition (`viv pull` — registry/TLS/layer-unpack; the v1.0 bundle is
  host-baked, §7.2). **Concurrent containers** were OUT until 2026-08-17 (a
  second simultaneous `viv run` collided on the fixed `/srv/viv-dio` name and
  was refused at the diorama attach — V-8 F3); they are IN since the diorama
  channel became a private pipe pair (§7.2.1), gated by the boot leg that runs
  two `viv run /vivarium/probe` at once.

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
| **`lseek` on a non-seekable fd reports `EPERM`, not `ESPIPE`** (V-8 F1, tasks #100/#106) | The refusal is CORRECT — a pipe, socket or `/proc` file is not seekable and the call must fail — but the code naming it is wrong. `T_E_SPIPE` (29) is not in the errno registry and appending one is signoff-bearing, so `sys_lseek_handler`'s non-seekable arm still returns a bare `-1`, which stock musl's `__syscall_ret` reads as `errno = 1` = `EPERM`. Substituting `EINVAL` would be the "differently wrong" answer #100 explicitly declined: a caller that special-cases `ESPIPE` (the standard "this stream is not seekable, fall back to reading forward" idiom) sees a permission error instead and may report it as one. `pread`/`pwrite` carry the same residual in `spoor_read_common`/`spoor_write_common` but are not table rows yet, so today only `lseek` is reachable from a guest. Every OTHER error on the T1 byte-I/O surface names itself correctly as of #100 |
| **`/proc/self` names the mounter** (task #90) | The per-container diorama reports `viv` rather than the reading Proc |
| **A handler's `ucontext` carries no FPSIMD record** (§6.22, task #96) | **NARROWED at V-8 -- the register corruption it used to describe is FIXED.** Note delivery now saves and restores Q0-Q31 + FPSR + FPCR around a handler (`fp_save_area`/`fp_restore_area`, a 520-byte block on `struct Thread`), so a handler may use floating point and autovectorised routines freely and the interrupted computation resumes intact. This was never an authority question -- the registers are the Proc's own -- but it was silent data corruption, PRE-EXISTING on the native note path and made reachable by ordinary compiled C once the phenotype landed. What REMAINS degraded is only the *reporting*: the frame's `_aarch64_ctx` chain is still terminated immediately rather than carrying an `fpsimd_context`, so a guest that walks `__reserved` looking for its FP state is told the record is absent rather than being handed one. Absent-and-honest, not present-and-wrong; and the state itself is now genuinely preserved underneath |
| **`siginfo` carries the signal number only** (§6.22) | `si_signo`/`si_errno`/`si_code` are filled and the `_sifields` union is zeroed, with `si_code = SI_KERNEL` -- the one value that claims nothing about the union. A note carries a 16-byte name and one u32 arg, so `si_pid`, `si_uid`, `si_status` and `si_addr` have no source. Queued `siginfo` is the Tier-2 item §5.4 already names |
| **A signal handler's `ucontext` is read-only** (§6.22) | The frame is written to the user stack and is accurate to read, but `rt_sigreturn` restores from the kernel-side `Thread` snapshot, so *writing* `uc_mcontext` does not change where execution resumes. Breaks signal-driven control transfer (Go's `sigpanic`, JIT deoptimisation); neither reaches this path at v1.0. Bought deliberately: it is what makes the `rt_sigreturn` escalation surface structurally absent rather than merely guarded |
| **`SA_ONSTACK` / `sigaltstack` answer `ENOSYS`, and that is now load-bearing** (§6.23) | A phenotyped handler always runs on the main stack: `sigaltstack(2)` is an explicit `ENOSYS` row (`vivarium.c:256`) and `SA_ONSTACK` in `rt_sigaction` changes nothing. Beyond the ordinary fidelity cost, this `ENOSYS` is a **precondition of the §6.23 handler-escape detector** — it tells a live handler from a `siglongjmp`'d escape by comparing the interrupted `sp` against the pre-handler `sp` *on the same stack*, which is sound only while a handler cannot run on an alternate one. What *used* to be the unbounded failure here — a handler escaping without `rt_sigreturn` left `in_handler` stuck forever, so the guest went permanently signal-deaf — is now **ENFORCED-correct** (§6.23), not degraded. Serving `sigaltstack` later requires revisiting §6.23; a `_Static_assert` at the detector pins the coupling at build time |
| **`swapcontext` from a signal handler to a higher-addressed stack corrupts** (§6.23, audit F1) | The §6.23 escape-detector **false-clears** `in_handler` when a handler `swapcontext`s to a *higher-addressed separate* stack — a live, suspended coroutine, **not** an abandonment. The higher `sp` trips `sp ≥ note_saved_sp_el0`, the N-3 guard admits a nested delivery, and the single `note_saved_*` slot is overwritten, so the original handler resumes on the *wrong* context at `rt_sigreturn`. **Worse than pre-`bug-2`**, which left `in_handler` stuck and safely *deferred* the second note. Contained: guest-**self**-corruption — `note_saved_sp_el0` is always a validated user VA, so the wrong restore yields a user `sp`, never a kernel one; `in_handler` is per-Thread (no cross-Proc effect); `kill` bypasses the path. Exotic: it needs signal-driven cross-stack coroutine switching to a higher-addressed stack, which no v1.0 target (busybox / Alpine / Go) does. The v1.x hardening is a VMA-same-stack gate — `vma_lookup(sp) == vma_lookup(note_saved_sp_el0)` before clearing, which still detects a same-stack `siglongjmp` but not a cross-stack swap — **tracked, not built** |
| **`bind` reports address collisions late** (§5.5.3, V-5b) | netd has no `bind` ctl verb — a local endpoint reaches it only as the argument of `announce` — so `bind()` is *remembered* and `listen()` spends it. A port already in use therefore succeeds at `bind` and fails at `listen` with `EADDRINUSE`. The error moves; it does not vanish. A server that reports "cannot bind" one line later is the whole visible effect |
| **`listen` will not auto-bind** (§5.5.3, V-5b) | Linux binds an ephemeral port when `listen()` is called on an unbound socket; netd's announce parser rejects port 0, and inventing a port would be a translation the guest did not ask for, so this answers `EOPNOTSUPP`. Harmless in practice because discovering an auto-bound port needs `getsockname`, which is not a row yet — a server that cannot learn its own port cannot use one |
| **`connect` after a *constrained* `bind` is refused** (§5.5.3, V-5b) | netd's dial verb carries the REMOTE endpoint only (its `!local` suffix is parsed and ignored), so a client that bound a specific source port cannot be honoured and gets `EOPNOTSUPP` rather than a silent ephemeral port. An *unconstrained* bind (`0.0.0.0:0`) asks for nothing netd is not already doing and proceeds normally |
| **`listen`'s backlog is netd's** (§5.5.3, V-5b) | The `backlog` argument is dropped: netd owns its accept queue (depth 1 today) and exposes no way to request another. Linux also treats the value as a hint and clamps it to a system maximum, so a caller cannot distinguish this from an ordinary clamp — but a second connection arriving before the first is accepted is refused rather than queued |
| **`accept`'s peer address degrades to `0.0.0.0:0`** (§5.5.3, V-5b) | The address comes from a second read of the connection's `remote` file. If that read fails the accept still succeeds — the peer genuinely is connected, and failing would be worse — and the `sockaddr_in` is written all-zero rather than left holding the caller's stale bytes. Not reachable in normal operation; listed because a caller cannot tell it apart from a genuine `0.0.0.0` peer |
| **A zero-timeout `ppoll` over a socket takes up to 10 ms** (§5.5.4, V-5c, task #98) | Readiness lives in netd, one RPC away, and the probe is asynchronous — so a literal zero-timeout scan would answer "nothing ready" for a plainly writable socket, and a caller looping on timeout 0 would never progress. A requested 0 therefore gets a 10 ms budget when a socket is in the array. The *answer* is netd's real verdict; only the latency differs, and a caller-supplied timeout is never touched. A probe that misses the budget yields not-ready and the caller retries |
| **`ppoll` with a `sigmask` is refused** (§5.5.4, V-5c) | The atomic mask swap is ppoll's entire reason to exist over `poll()`, and doing it non-atomically would re-open the exact race the caller chose ppoll to close. `ENOSYS` rather than an approximation. musl's `poll()` passes NULL, so the common path is unaffected; only a program using ppoll *for its signal semantics* is |
| **`pselect6` with a `sigmask` is refused** (§5.5.4, V-5c-2) | Same reason as `ppoll`'s, and note the sixth argument is a POINTER to `{ss, ss_len}` — aarch64 caps a syscall at six registers — so a non-NULL pair is declined without being dereferenced. A NULL sixth argument is unambiguously "no mask", which is the common path |
| **A set `exceptfds` bit is refused** (§5.5.4, V-5c-2) | Native poll has no `POLLPRI`: the requestable set is `(POLLIN\|POLLOUT)`, full stop. Dropping the bit silently — what pouch's userspace `select` does (task #99 F-b) — turns a *pure* `exceptfds` wait into an infinite block rather than an error, and mapping it to `POLLIN` would report ordinary data as an exception. A NULL or all-zero `exceptfds` is not a request and passes through |
| **More than 64 CONTRIBUTING fds is refused** (§5.5.4, V-5c-2) | `POLL_MAX_NFDS` bounds the pollfd ARRAY. Note this is a bound on the *count*, not on fd *values*: a `select` over fds 200 and 201 is two pollfds and is fine. (pouch's `select` caps the wrong axis and returns `EBADF` for any fd ≥ 64 — task #99 F-a) |
| **An `nfds` above the fd table is CLAMPED, not refused** (§5.5.4, V-5c-2) | Which is Linux's own `if (n > max_fds) n = max_fds`. A bit above the table names an fd that cannot exist, so it is simply not scanned. A bit *below* the clamp naming no open handle still becomes `POLLNVAL` → `EBADF`, which is also Linux |
| **`pipe2` admits `{0, O_CLOEXEC}` and nothing else** (#155) | `O_NONBLOCK` and `O_DIRECT` are flags Linux's `pipe2` genuinely accepts, and both answer `ENOSYS` here — devpipe has no non-blocking read and no packet framing, so admitting either would tell a guest something false about the pipe it just received. An allow-list rather than a deny-list, for the reason V-2d's `mmap` recorded: aarch64 defines flags a deny-list admits by omission. The two admitted values are not a conservative subset — they are what the gate's own busybox issues, measured off the binary (four sites through musl's `pipe()` with a hardcoded `mov x1, #0`, two through `pipe2()` with `mov w1, #0x80000`), and on aarch64 there is no legacy `pipe` number to reach instead |
| **`dup3` DECLINES when the SOURCE is a socket** (#157) | `dup2(sockfd, 0)` — the inetd idiom — answers `ENOSYS`. Thylacine's socktab keys `(proto, N, state)` on the fd NUMBER and is not refcounted, so two descriptors cannot share one socket's state, and both alternatives are wrong rather than merely imperfect: *copying* the entry gives two independent state machines over one connection (a `connect` on the first advances it and swaps its handle `ctl`→`data` while the second still names `ctl` and still believes it is FRESH), and *omitting* it gives an fd that reads and writes correctly but fails `connect`/`bind`/`getsockname` — the silent half-service §6.19's argument-domain rule exists to forbid. Reproducing Linux exactly needs a refcounted socktab entry, a real change to a table V-5 audited. A shell's `dup2` is for files and pipes, so the L-6c gate is unaffected; what this turns away is a server doing `dup2(connfd,0); dup2(connfd,1)`. Note the DESTINATION being a socket is a different question and IS served — dup3 closes it, and the entry keyed on that number is dropped |
| **`dup3` admits `{0, O_CLOEXEC}` and refuses the rest with `EINVAL`, not `ENOSYS`** (#157) | Listed here for contrast rather than as a gap: unlike `pipe2` above, this row's served set is **equal** to Linux's, because `ksys_dup3` refuses everything outside the same pair with `EINVAL`. So a refused flags word is us reproducing Linux exactly, not declining to serve — which is why it must not be collapsed into the ENOSYS decline (V-2d's `munmap` note). The only genuinely degraded thing about this row is the socket case above |

---

## 10. Build arc — V-0..V-8

| # | Chunk | Contents | Gate |
|---|---|---|---|
| V-0 | Scripture | This document; the §4 fork resolved; `ARCH §11.5/§11.6` corrected (R-1, R-2); I-43 minted; NOVEL entry | user signoff |
| V-1 | Phenotype + brand | `Proc.phenotype`, brand detection at exec, the dispatch branch, a native-unchanged proof. **V-1a LANDED** (the field + the advisory `elf_brand_hint`). **V-1b LANDED**: the declaration (`SPAWN_PHENO_LINUX` in `sys_spawn_args.pheno_flags`, consuming the must-be-0 `_pad_allow` slot at offset 92 -> a zero-filled pre-V-1b request still means inherit) + the syscall-entry branch (T1 renumber-in-place then FALL THROUGH to the native switch; the three T2 shells over the V-2 pure translators; FORWARD and ENOSYS kept as separate arms so V-3 is a one-line change) + `sys_fstat_for_proc` extracted so the phenotyped path shares the native core + rule 4's advisory diagnostic (the hint's first caller, on an already-failed load) + `viv`'s `org.thylacine.phenotype` manifest annotation | native suite byte-unchanged (1237/1237); **PASS in-boot on two vantages** -- leg A `viv-pheno-probe native` proves a Linux number is NOT translated without a declaration (`brk` -> -1, not -ENOSYS), leg B `viv run /vivarium/pheno` proves the whole chain with a container entrypoint that speaks only raw Linux numbers and moves real bytes (openat/read/lseek/fstat/newfstatat/write/close, the two stat paths cross-checked on `(st_dev, st_ino)`, the `AT_SYMLINK_NOFOLLOW` reject still rejecting) and dies through Linux `exit_group`; revert-probed |
| V-2 | The translation table | The §4-C stateless 1:1 set; the split rule enforced | a static `hello` (built by *Linux* toolchain) runs and exits 0 |
| V-3 | Supervisor channel | **DEFERRED (user-voted 2026-07-30) — §4.1; and V-5 did NOT claim it (user-voted 2026-07-31) — §4.1.1.** The sketched destination (a ring to a peer Proc) is verified unable to serve the forwarded set: no Proc can mutate another's address space, handle table or process tree, so the servable set is empty. Not "hard"; *empty*. V-5 was expected to decide the shape, and instead **measured that sockets need no supervisor**: every step is work the calling Proc could do for itself, so the kernel performs it with the caller's own authority (§5.5). The fork therefore moves to the next chunk that needs a destination it cannot synthesise — on present evidence **process creation (#93)**. The three candidates + the peer evidence stand recorded in §4.1 for it. `specs/phenotype.tla` lands with whatever that chunk chooses. **RESOLVED 2026-08-01 — the fork does not travel; it dissolves.** #93 was designed (`docs/LINEAGE.md`, user-voted: the full arc through COW fork) and it needs **no supervisor either**, for V-5's reason restated: `execve` mutates the caller's *own* address space, and `rfork`/`fork` create the caller's *own* child — both are work the calling Proc could already authorize, so the kernel performs them with the caller's authority. Two independent chunks have now been expected to claim the supervisor and both measured it unnecessary, which is evidence about the *shape of the phenotype* and not a coincidence: a phenotype confers ABI shape, never authority (I-43), so a translated call never needs a destination with more authority than its caller. `specs/phenotype.tla` accordingly has no owner and is not owed by any planned chunk | (dissolved; no chunk builds it) |
| V-4 | The diorama | `/proc`, `/sys`, `/dev` servers + per-container mounts | `busybox ps`, `ldd`, `/proc/self/exe` |
| V-5 | Sockets | The `/net` translation, **in the kernel phenotype as a T2 family** (§5.5, user-voted 2026-07-31). **V-5a** the substrate + the client path: `handle_replace`, `Proc.socktab`, and `socket`/`connect`/`shutdown`/`getsockname`/`getpeername`/`sendto`/`recvfrom` — `read`/`write`/`close` need no row, which is the design's point. **V-5b** the server path: `bind`/`listen`/`accept`/`accept4` over netd's deferred-accept (LANDED -- section 5.5.3; the in-guest gate drives a full server+client TCP round-trip from ONE single-threaded process). **V-5c** readiness over the `QTPOLL` `ready` file: **V-5c-1 LANDED** -- `ppoll` (which IS the poll family on aarch64) as a T2 row that swaps a socket fd for its `ready` sibling, plus the netd half of #220 (a listener now reports `POLLIN` when a call is pending, via the same `accept_ready` predicate `poll_accepts` uses); the first two rows BELOW the native number ceiling, so §5.5.4 carries the per-number collision re-check the ARCH §25.4 row mandates. **V-5c-2 LANDED** -- `pselect6`'s `fd_set` reshape (three 1024-bit bitmaps in, one pollfd array out, three back), plus the zero-fd sleep that BOTH forms needed: `select(0, NULL, NULL, NULL, &tv)` is the classic portable sleep, so `sys_poll_sleep_for` was added and `ppoll`'s `nfds == 0` decline retired with it. **V-5d** the focused audit + close | **`curl` fetches a URL** (ROADMAP §9.2); V-5a's own gate is an in-guest TCP round-trip over the resident loopback |
| V-6 | Signals | Tier 0, then Tier 1 (audit-bearing). **Frame shape decided 2026-07-30 (§6.22, user-voted)**: the kernel already owns the delivery machinery (`SYS_NOTIFY`/`SYS_NOTED`) and saves the interrupted context *kernel-side*, in fields that field-for-field match Linux's `mcontext_t`. So the frame is pushed for **reading** and `rt_sigreturn` restores from the `Thread` snapshot — which makes §8's escalation hazard structurally absent rather than guarded, at the stated cost that `uc_mcontext` writes are inert. Tier 0 + Tier 1 both land. **V-6a** landed the decode; **V-6b** landed dispositions (`rt_sigaction` for `SIG_DFL`/`SIG_IGN` + `rt_sigprocmask` + the per-Proc `viv_sigtab` + the post-time discard), and corrected two V-6a facts by measuring musl -- the `k_sigaction` layout is arch-fixed at 32 bytes rather than flag-chosen, and SIGTERM had to be evicted from `interrupt` because a shared note cannot carry two independent dispositions (task #95). **V-6c** landed the Tier-1 frame: a real handler installs and RUNS, `rt_sigreturn` is the phenotyped spelling of `SYS_NOTED(NCONT)`, and the sigtab widened to the whole `k_sigaction`. **V-6 IS COMPLETE**. **Refined 2026-08-17 (aux):** the SIG_IGN pending-discard moved to the INSTALL (POSIX 2.4.3 / Linux `flush_sigqueue_mask`): `rt_sigaction` stores, then `notes_discard_name` removes every queued note of that name mask-blind; `notes_post`'s disposition read is under `q->lock`, so no stale ignored note survives and the EL0 tail's SIG_IGN arm is defense-in-depth. In-guest legs L205-L216 (a handler installed after SIG_IGN, still blocked, fires nothing on unblock). **Fork/exec signal state is POSIX (voted 2026-08-17; task #127 both halves):** clone copies the sigtab + the caller's note_mask into the child; execve resets caught rows to SIG_DFL and KEEPS SIG_IGN rows + the mask (native keeps the Plan 9 clear). | Ctrl-C kills a guest; `SIGPIPE`; handler round-trip |
| V-7 | `viv` | bundle-consumer runtime (§7.2): host-baked bundle → territory + per-container diorama + `/dev` binds → #58 spawn. **LANDED**: `usr/viv` + `usr/viv-probe` + the `/vivarium` pool bake (the synthetic probe bundle always; the Alpine bundle stages when a minirootfs tarball is provided) + the boot-fatal joey leg; PGRP_MAX_MOUNTS 20→32 (the container recipe overflowed the territory table) | the native `viv-probe` gate (§7.2) — **PASS in-boot**, revert-probed (an unfiltered diorama fails the pid-enumeration leg); **an Alpine shell runs** is the ARC gate (needs V-1b + V-2 too; ROADMAP §9.2) |
| V-8 | Close | Focused audit (I-43), SMP gate, `docs/reference/NN-vivarium.md`, the fidelity ladder published. **LANDED** — the round covered the arc's genuinely unaudited surface (V-1b + V-2d + V-6a/b/c + V-7, chosen by measuring what the two prior rounds left rather than by assuming), returned **0 P0 / 1 P1 / 2 P2 / 4 P3**, and every finding is closed: F1 `285acd2c` (#100), F2 `ff79386b`, F3 `27a11e2c` (#101), F4-F7 `b3648a2b` (#102); the #96 FP fix and #94 landed in `a39d2c53` ahead of it, and a gate failure that surfaced during it was root-caused to a PRE-EXISTING test race and fixed at `8c1c080e` (#103/#104). §9 now carries the ESPIPE residual and the two OUT items the arc actually leaves | **CLOSED, and not dirty** — see §10.1 |

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
   Its own premise was re-checked at entry the way §4.1 re-checked V-3's, and
   unlike V-3's it **holds**: the notes substrate exists, every row of the
   signal↔note map lands on a live note, and a consumer is available
   (`viv-pheno-probe` issues raw Linux numbers). §6.22 is the design.
3. **V-5** — sockets, which **decides V-3's shape** and then builds it.
4. **V-8** — the I-43 focused audit + close.

**CORRECTED A THIRD TIME 2026-07-31, at V-5's entry — and this time the
correction is that the expected decision did not need making.** Step 3 above
assumed V-5 would pick a supervisor shape and build it. Measuring `/net` first
(§4.1.1) found that sockets are translatable in the kernel with the caller's own
authority, so **V-5 builds no supervisor and V-3 stays deferred**. The order from
here:

1. **V-5a..V-5d** — sockets as a T2 family (§5.5): substrate + client, then
   server, then readiness, then the focused audit.
2. **V-8** — the I-43 focused audit + close, where #93 (process creation) is
   weighed and, if built, inherits V-3's fork.

The generalisable lesson, and the reason this correction reads like the previous
two: **each was a mechanism assumed rather than measured, and each was wrong in
the direction of "more machinery than the tree needs".** V-1b was expected to
precede V-7 and could not; V-3 was expected to precede V-5 and had an empty
servable set; V-5 was expected to need a supervisor and does not. Before building
a channel, check what the existing authority already reaches.

**A gap this arc does not chunk, recorded at V-6's entry (task #93).** The ARC
gate is "an Alpine shell runs", but a shell forks before it does anything else,
and **no V-chunk builds `clone`/`execve`/`wait4`**. §2's table correctly says the
*native* counterparts exist (`rfork`, the `SYS_SPAWN_*` family, `SYS_WAIT_PID`),
but the translation is not a renumber — `SYS_SPAWN_*` takes a program where
`clone` takes a continuation, and `fork()` has no native counterpart at all. So
they are unclassified → `FORWARD` → `ENOSYS`, and a guest shell cannot fork. This
does not block V-6 (signals are needed either way), but it is a hole between the
arc as chunked and the arc gate; it wants its own scripture pass, weighed at V-8
or promoted sooner if the ARC gate is attempted.

---

## 10.1 V-8 — the arc close, and what is deliberately NOT built

V-8 is the close: the focused audit over the arc's unaudited surface, the SMP
gate, the reference doc, the fidelity ladder (section 9), and -- the part that
takes judgement -- a **disposition for every tracked task the arc raised**. A
close that leaves those as an undifferentiated backlog has not closed anything.

**The audit scope was chosen by measuring, not by assuming.** Two prior rounds
exist: V-4c-3 covered the arc through V-4c-2, and V-5d covered the socket
family. Everything between and after was unaudited -- and V-6 (signals) is
marked "kernel and audit-bearing" in this document's own track note and had
never received a round. So V-8's audit is V-1b + V-2d + V-6a/b/c + V-7, not a
formality.

### FIXED in V-8

| Task | Why it was fixed here rather than deferred |
|---|---|
| **#96** FP/SIMD not saved across note delivery | A real correctness defect -- silent corruption of an interrupted computation, PRE-EXISTING on the native path and made reachable by ordinary compiled C once the phenotype landed. Small, well-understood fix; benefits native pouch and the phenotype identically; and it sits squarely on V-8's own audit surface, so fixing it first meant the round prosecuted the final state. `docs/reference/83-pouch-signals.md` |
| **#94** a failing pheno gate extincts naming the wrong thing | Cheap, and it costs a reader the exact wrong minutes when a gate breaks. **The enqueued diagnosis was wrong**: it named the `usr/joey.c` reap-any sites, but measuring found those already converted by the U-7-pre lift and the two that remain are the init orphan-reaper loops, where reap-any is correct. The actual site was `kernel/joey.c` -- the kproc reaping userspace-init, which is also the Proc the orphan rule reparents to, so it could legitimately reap an adopted orphan first and then extinct naming *that* |

### DEFERRED, with the reason

| Task | Disposition |
|---|---|
| **#93** process creation (clone/execve/wait4) | **The named next chunk, and the arc gate's blocker** -- "an Alpine shell runs" needs `fork`. Not a renumber: Linux `clone(flags, stack, ptid, tls, ctid)` versus a `SYS_SPAWN_*` family that takes a *program* rather than a continuation, and `fork()` has no native counterpart at all. Wants its own scripture pass. It also **falsifies two premises the socket family rests on** -- both the socktab's lock-freedom and the transient-fd invisibility assume a `PHENO_LINUX` Proc cannot spawn a thread -- so `viv_sock_connect`'s re-read of `e->proto`/`e->n` after a blocking write is the first line to revisit when it lands |
| **#91** exit status is boolean | Thylacine-wide, not vivarium-specific, and `docs/ERRORS.md` is ABI-bearing -- the exit-status **encoding** needs user signoff before any impl. Touches the death path (#809/#811), so it wants its own chunk with the usual death-lineage care |
| **#95** SIGTERM needs its own note | An I-19 supported-set addition = an ABI change to the notes surface. Signoff |
| **#98** `/net` readiness cannot be answered synchronously | Needs a netd-side or kernel-side readiness change; V-5c mitigates with a 10 ms probe budget and section 9 publishes the residual honestly |
| **#90** container `/proc/self` names the mounter | The remedies section 6.13 names both need a per-op identity channel, which is a new kernel surface |
| **#99** pouch `select(2)`'s four defects | Userspace pouch, and the kernel translator already avoids all four (V-5c-2). Tracked, not arc-blocking |
| **#106** `T_E_SPIPE` (29) is unregistered | Raised BY the round, as F1's deliberate residual. Appending an errno is an `ERRORS.md` change and `ERRORS.md` is ABI-bearing, so it needs signoff -- and the alternative (`EINVAL`) is the "differently wrong" substitution #100 declined. Published in §9's DEGRADED table meanwhile |
| **#107** the flat `-1` sites ADJACENT to the byte-I/O family | F1's fix was scoped to the family the finding named. The same `-1`-means-EPERM defect exists on neighbouring surfaces; ER-3 already ratifies the mapping, so this is rollout rather than design. Deliberately not widened mid-round -- the scope a finding names is the scope a fix should be reviewable against |
| **#108** `#NNN` means two different things across tracks | A bookkeeping hazard the arc created, not a code defect: this tree's task numbers collide with the main tree's `bug_NNN` series, so a `#NNN` read out of a commit message resolves differently depending on which tree you are in. `TaskGet` before acting on one |

The through-line: **#96 and #94 were fixed because they are defects; the rest
are deferred because they are DESIGN, and design that touches an ABI needs a
vote rather than a commit.**

### The close itself — 0 P0 / 1 P1 / 2 P2 / 4 P3, and NOT dirty

Every finding is closed. The P1 and both P2s were live defects with in-guest
consequences; all four P3s were verified against the code before being fixed,
and **two of the four had a stated justification that did not survive that
check** — F4's safety argument was true but spanned two files, and F5's
conclusion held for a reason other than the one it gave.

**A round 2 is NOT owed, and the reasoning is worth keeping because this
document predicted the opposite.** The closed list recorded, at the time F1 was
raised, that fixing it would restructure the central dispatch and therefore make
this a dirty close. That prediction was **false three times over**:

| finding | predicted | actual |
|---|---|---|
| F1 | restructures the central dispatch | dispatch untouched — every `-1` is a LOCAL gate where the reason is already known exactly, so the fix is ER-3 applied to five call sites |
| F3 | a per-runner diorama name | a peer check in one userspace `h_attach`; **kernel byte-unchanged** |
| F4-F7 | (unstated) | two one-line guards and two comments; no mechanism, no wait/wake protocol, no lock order touched |

The dirty-close bar is a P0 return, six or more P1+P2, or a structurally invasive
fix. None applies. Note the *shape* of the error though: the prediction was made
from the finding's own description rather than from the code, which is the arc's
recurring failure mode appearing one more time in the close itself.

### What the round confirmed, so it is not re-derived

The prosecutor independently verified the **I-43 argument-arity property** for
all five T1 rows (checked against handler signatures, not dispatch sites), that
`kill`/`tkill`/`tgkill` are in neither table so **I-26's two-axis gate is
untouched by this arc**, that nothing in `exec.c`/`elf.c` writes the phenotype
(§12.1 rule 4 holds by construction), that the V-2d prot check is a true
allow-list rather than a `PROT_EXEC` deny-list, that SIGKILL is uncatchable **by
construction** rather than by a special case, and that `viv` holds no capability
beyond its invoker's. It also reproduced the #96 FP fix's whole mechanism
independently, including the same five-function sweep the self-audit had reached
by a different method.

### The arc's durable lesson

Across the whole VIVARIUM arc, **six of seven enqueued mechanisms were wrong**
(#79, #80, #82, #84, #101, and #102's two justifications; #87 is the sole
exception) — and every one was wrong in the direction of *more machinery than
the tree needs*. The three sequencing corrections in §10 are the same error at
chunk scale: V-1b was expected to precede V-7 and could not, V-3 was expected to
precede V-5 and had an empty servable set, V-5 was expected to need a supervisor
and does not. **Read the code before implementing from a task's text, and check
what existing authority already reaches before building a channel.**

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

1. The **vivarium manifest** declares a container's phenotype (the `pheno_flags`
   spawn-arg channel). Since the `/viv/bin` extension (§13), a **mount marked
   `MPHENO_LINUX`** is the *second* — and, at v1.0, last — thing that can *set*
   `PHENO_LINUX`: a binary resolved for exec through such a mount is stamped Linux
   even outside any vivarium. Both channels are OR-combined at the single exec-time
   phenotype stamp; no ELF byte, note, or interpreter path ever adds a third.
2. Within a declared-Linux vivarium, every exec is `PHENO_LINUX` unless the binary is
   positively identified as native (a Thylacine-native brand, §12.2).
3. Outside a vivarium **and not resolved through an `MPHENO_LINUX` mount**, the
   phenotype is always `PHENO_NATIVE`. Only a namespace-composed declaration (a
   manifest, or a pheno-mount) ever sets it — never an ELF byte, note, or interpreter
   path. The fail-safe direction is preserved on BOTH channels: a coverage gap in
   either one leaves a binary NATIVE, never silently Linux (§13's fail-safe property).
4. `PT_INTERP` / `EI_OSABI` / `NT_GNU_ABI_TAG` are used only to *warn* on an obvious
   mismatch (a Linux-interp binary exec'd outside a vivarium AND off any pheno-mount
   gets a diagnostic and a clean failure, not a silent mis-decode).

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

## 13. The `/viv/bin` phenotype mount — bare Linux binaries on the PATH

**Status: DESIGN (scripture-first). Operator-requested + operator-voted 2026-08-26,
after the git-under-VIVARIUM arc (§6.26/§6.27) closed. No code yet; this section is
the ratified design the implementation is built against.**

### 13.0 Thesis

git now runs end to end under a vivarium container (§6.27). The operator's next ask:
**ship it as a first-class program** — "a separate bin directory for Linux programs
run via viv, put that bin on the PATH, make it work with ut's autocomplete and path
resolution, so the user has a seamless experience" — plus the question, *"is there a
way to easily/quickly identify a Linux binary from ut?"*

The answer to that question is the whole design. **There is no reliable INTRINSIC ELF
marker for the v1.0 target (a musl-static binary)** — this is the settled Q3
resolution (§12.1): `EI_OSABI` is non-discriminating in both directions and
`PT_INTERP` is absent on a static binary. So a phenotype is never *sniffed*, only
*declared*. The fast, unambiguous, and design-correct identifier is therefore **the
binary's LOCATION**: a curated, trusted directory *is* an external declaration, in
exactly the Q3 spirit. `/viv/bin` is that directory, and the mount it lives on
carries the declaration.

### 13.1 The decision (operator vote, 2026-08-26)

Two votes, both to the recommended option:

- **Declaration mechanism: a kernel mount-flag.** `/viv/bin` is a real bind mount
  carrying a new `MPHENO_LINUX` territory flag; the *kernel* stamps `PHENO_LINUX` on
  any binary exec'd through that mount. The declaration stays kernel-owned and
  namespace-resident — where §12.1 says it belongs — rather than as a hardcoded
  path-prefix string in userspace.
- **Directory: `/viv/bin`** (matches the vivarium naming; system-owned, so only
  trusted shipped binaries live there — which is what makes by-location sound).

### 13.2 Prior art (why a mount flag is the design-correct form)

- **Plan 9** has no binary-compat precedent, but *location-as-namespace-composition*
  is its native idiom, and mounts already carry flags (`MREPL`/`MBEFORE`/`MAFTER`/
  `MCREATE`, and our own `MNOEXEC`). **The phenotype as a mount property is the
  Plan 9-shaped answer** — the same shape as `MNOEXEC` (#217), which is the direct
  implementation template.
- **FreeBSD Linuxulator** brands a binary Linux by its `/compat/linux` interpreter-
  path prefix — literally "by location," system-curated. **Direct precedent for the
  operator's idea**; `/viv/bin` is its Thylacine form.
- **illumos LX brand zones / WSL1 pico processes / gVisor** all have the *zone/
  sandbox/runtime* declare the ABI — which is our §12.1 *manifest* channel (as-built
  V-1b). The SOTA splits into "the container declares it" (our first channel) vs "a
  curated location declares it" (FreeBSD, this second channel).

The synthesis — **the phenotype is a property of a trusted namespace mount, declared
by whoever composes the namespace, never inferred from bytes** — fuses the Plan 9
mount-flag idiom with FreeBSD's `/compat/linux`, at single-binary rather than whole-
container granularity, while keeping the declare-don't-sniff invariant intact.

### 13.3 Mechanism (the `MNOEXEC` sibling)

1. **A new mount flag `MPHENO_LINUX = 0x0020`** in `kernel/include/thylacine/
   territory.h`, the next bit after `MNOEXEC (0x0010)`. A mount marked with it
   declares: binaries resolved for exec through this mount run under the Linux
   phenotype.
2. **A parallel coverage scan `mount_pheno_linux_covers(territory, dc, devno)`** in
   `kernel/territory.c`, the ANY-scan twin of `mount_noexec_covers` — keyed on the
   `(dc, devno)` device instance a file necessarily shares with its mount source
   (`spoor_clone` propagates `devno` through every walk/cross), so one device instance
   mounted twice cannot carry two verdicts.
3. **One OR-combined stamp.** At the exec-time phenotype stamp (today
   `if (pheno_linux) p->phenotype = PHENO_LINUX;`, `kernel/syscall.c` in the spawn
   thunk, right where the resolved binary Spoor `exe` and the child `territory` are
   both in hand), the child is stamped Linux iff the spawn-arg channel declared it
   (`sa->pheno_flags & SPAWN_PHENO_LINUX`) **OR** the resolved binary's mount covers
   it (`mount_pheno_linux_covers(p->territory, exe->dc, exe->devno)`). Two declaration
   channels, one stamp site, no third path.
4. **Observable.** The `/proc/<pid>/ns` and `/dev/ns` renderers print ` pheno-linux`
   next to a covered mount exactly as they print ` noexec` (`kernel/territory.c`
   render path) — a declaration that cannot be observed cannot be audited (the #217
   lesson).

**The fail-safe property — and why `MPHENO_LINUX` needs NO `may_back_exec`-style
floor.** `MNOEXEC` is a RESTRICTION whose coverage gaps fail *open* (a Dev the
`(dc,devno)` key cannot reach — `devenv` stamps the caller's env devno — escapes the
noexec cover), which is precisely why `Dev.may_back_exec` exists as a hard floor
beneath it. `MPHENO_LINUX` is a DECLARATION whose coverage gaps fail *safe*: if the
key misses, the binary runs `PHENO_NATIVE` (rule 3's default) — a Linux binary that
does not get the Linux phenotype merely makes Linux-numbered calls that hit native
handlers and fails cleanly (rule 4's diagnostic path), never a silent privilege gain.
So there is no fail-open class to floor against; the safe direction is structural.

### 13.4 I-43 soundness (shape, never authority)

The mount flag confers ABI **shape** through the namespace and not one bit of
authority (I-43). A Proc stamped `PHENO_LINUX` via the mount gets Linux syscall
numbering/semantics; its capabilities come *solely* from the spawn's `cap_mask` (the
spawner's own held caps, minus `CAP_ELEVATION_ONLY` — the phenotype-fork-inherits-caps
rule of §6.26). The core soundness argument carries over verbatim from the manifest
channel: **every translated Linux number collides with a live native one, so a
mis-declared Proc mis-decodes its own calls behind its own gates and reaches nothing
new** (ARCH §28 I-43; `docs/reference/145-vivarium.md` §3). The mount channel is if
anything *stronger* than the manifest channel (which `viv` sets ungated): composing a
mount is itself a namespace edit, and `/viv/bin` is composed by `PRINCIPAL_SYSTEM` at
boot.

**Open question (for the sub-chunk-A audit, not the operator):** should *setting*
`MPHENO_LINUX` on a mount be capability-gated? `MNOEXEC` is deliberately ungated
(territory.c: "authority conferred by a namespace edit," and a user marking their own
mount noexec only RESTRICTS). `MPHENO_LINUX` EXPANDS (declares Linux) — but I-43 makes
it authority-neutral, so an unprivileged user marking a mount in their OWN namespace
`MPHENO_LINUX` grants their own procs nothing they could not already get via `viv run`.
The lean is therefore **ungated, matching `MNOEXEC`**, with the I-43 argument as the
guard rather than a cap. The holotype prosecutes this explicitly.

### 13.5 Trust model + the file-ownership wall

- **Direct-spawn in the user's namespace, not a container.** ut spawns the `/viv/bin`
  binary directly in the user's territory (the phenotype comes from the mount, not
  from wrapping it in a diorama). That is what makes it *seamless* — git sees the
  user's cwd and files, which an isolated container territory could not. Sound because
  `/viv/bin` is system-owned: only trusted shipped binaries live behind the pheno-mount.
- **The A-3 wall stands (noted, not blocking).** The pool 9P mount is `PRINCIPAL_
  SYSTEM`-owned; git run as a real user hits the chmod wall on the system pool (the
  §6.26 wall). git operating on **user-owned** files (a user's A-5 encrypted home) is
  clean. This is the existing A-3 ownership model, not new debt.

### 13.6 ut integration + cap conferral

- **PATH:** add `/viv/bin` to `resolve_command`'s `$path`
  (`usr/utopia/libutopia/src/eval/stmt.rs`) so `git` resolves bare.
- **Tab completion:** add `/viv/bin` to `refresh_command_index`'s readdir set
  (`usr/utopia/libutopia/src/repl.rs`) so `git` completes.
- **NO phenotype logic in ut.** Because the kernel applies the phenotype at exec via
  the mount flag, ut just spawns normally — the declaration never enters userspace.
- **Caps:** ut already holds `CAP_CSPRNG_READ` (`SHELL_CAPS = LOCK_PAGES | CSPRNG_READ`,
  `usr/login`), so git's `getrandom` works. ut confers its benign user caps to external
  spawns (the sub-chunk-B mechanism decision — uniform conferral of the user's own
  non-elevation caps, NOT location-gated, so no phenotype/location logic re-enters ut).

### 13.7 Deploy

- Stage the sha-pinned static git (2.51.2, `b8c41cfd…4615de9`) at `/viv/bin/git` +
  the dashed `git-upload-pack`/`git-receive-pack` symlinks + `/etc/gitconfig`, in the
  pool (or ramfs), **outside** the container bundle rootfs it currently lives in
  (`tools/build.sh`). Keep the §6.27 git-probe container gate (the O_APPEND witness).
- Compose `/viv/bin` as an `MPHENO_LINUX` bind mount at boot (`joey`/the boot path).

### 13.8 Alternatives considered + rejected

- **B1 — ut path-prefix (the fast form).** ut sets the phenotype when the resolved
  path is under `/viv/bin/`. Rejected: puts the phenotype-declaration *policy* into
  userspace as a hardcoded string (a second declarant that is not the kernel), and
  opens a larger, userspace-shaped hole in rule 3's fail-safe. ~1 day vs the mount-flag
  chunk's multi-day cost — but the standing "highest standard, design for the future"
  bar picks the kernel-owned form.
- **B3 — wrap each invocation in a vivarium.** No change to who declares, but a
  container territory cannot see the user's cwd/files, so it is not seamless, and it
  is heavyweight (a diorama per `git` call). Fails the operator's core requirement.
- **Q3-by-bytes** (sniff `EI_OSABI` / `PT_INTERP` / `NT_GNU_ABI_TAG`). Rejected: the
  settled §12.1 resolution — non-discriminating for the static target, and it violates
  declare-don't-sniff.

### 13.9 Sub-chunk plan

- **A (kernel mechanism).** `MPHENO_LINUX` flag + `mount_pheno_linux_covers` + the
  OR-combined exec stamp + the ns-introspection render + kernel unit tests (a synthetic
  `MPHENO_LINUX` mount → resolved binary → phenotype stamped; and the fail-safe: an
  uncovered binary stays native). Audit-bearing (the "Exec from the namespace" +
  Territory + I-43 surfaces) → holotype. Updates the `sub-kernel-territory` vault note
  (OWNED) + a new `docs/AUDIT-TRIGGERS.md` row + ARCH §28 I-43.
- **B (integration + deploy).** ut `/viv/bin` PATH + completion + uniform benign-cap
  conferral; `build.sh` git-at-`/viv/bin` + symlinks + `/etc/gitconfig` + the boot
  `MPHENO_LINUX` bind; a boot-gate E2E proving **bare `git` from a shell (not a
  container) runs Linux and works** — the end-to-end witness the whole arc exists for.

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
