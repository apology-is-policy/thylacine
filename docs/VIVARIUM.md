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
ELF_LOAD_BAD_OSABI;` (widened to accept `ELFOSABI_GNU` during the Clade arc), and
`PT_INTERP` is already parsed at `elf.c:179`.

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

## 4. THE ARCHITECTURAL FORK (open — needs the user's vote)

Where does the Linux ABI live?

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
- **`/dev`** — Linux-shaped: `null`, `zero`, `full`, `random`, `urandom`, `tty`,
  `pts/`, `ptmx`, `fd/`, `std{in,out,err}`. Most of these already exist behind
  Thylacine's `/dev` (devdev) and `/dev/pts` (ptyfs); the diorama is largely a
  **re-presentation**, not a reimplementation.

Because each is a per-container mount, `uname`/`hostname`/`meminfo` can differ per
container **without any namespacing machinery** — the property Linux needed six
namespace types to get.

---

## 7. The vivarium — the container runner

`thylacine-run` from `ROADMAP §9.1`, named `viv` (§11). Userspace; no new kernel
surface beyond §4's.

1. Fetch/unpack an OCI image (layers → a Stratum dataset; the reflink/snapshot
   machinery makes layering natural).
2. Build the territory: the image root as `/`, the diorama mounts, `/net` if the
   manifest grants it, the resource floor (I-32) and hardware allowance (I-34).
3. Set the phenotype (§5.2).
4. Spawn the entrypoint via the #58 namespace-exec path.

"No cgroups, no seccomp at v1.0; territory isolation is the boundary" (ROADMAP
§9.1) — which is exactly right, because I-32/I-34 already provide the resource and
hardware bounds cgroups/seccomp were retrofitted onto Linux to provide.

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
  (best-effort), `AF_INET6`, cgroups/seccomp, and full signal fidelity (Tier 2).

A Linux binary needing anything in the OUT list gets a clean `ENOSYS`, never a silent
wrong answer. **`ENOSYS` is a supported outcome; a lie is not.**

---

## 10. Build arc — V-0..V-8

| # | Chunk | Contents | Gate |
|---|---|---|---|
| V-0 | Scripture | This document; the §4 fork resolved; `ARCH §11.5/§11.6` corrected (R-1, R-2); I-43 minted; NOVEL entry | user signoff |
| V-1 | Phenotype + brand | `Proc.phenotype`, brand detection at exec, the dispatch branch, a native-unchanged proof | native suite byte-unchanged; a branded no-op binary reaches the Linux path |
| V-2 | The translation table | The §4-C stateless 1:1 set; the split rule enforced | a static `hello` (built by *Linux* toolchain) runs and exits 0 |
| V-3 | Supervisor channel | The forward mechanism + `specs/phenotype.tla` model-first (if B/C) | spec TLC-green + park/wake audit |
| V-4 | The diorama | `/proc`, `/sys`, `/dev` servers + per-container mounts | `busybox ps`, `ldd`, `/proc/self/exe` |
| V-5 | Sockets | The `/net` translation | **`curl` fetches a URL** (ROADMAP §9.2) |
| V-6 | Signals | Tier 0, then Tier 1 (audit-bearing) | Ctrl-C kills a guest; `SIGPIPE`; handler round-trip |
| V-7 | `viv` | OCI unpack → territory + diorama + phenotype → spawn | **an Alpine shell runs** (ROADMAP §9.2) |
| V-8 | Close | Focused audit (I-43), SMP gate, `docs/reference/NN-vivarium.md`, the fidelity ladder published | clean close |

Sequencing note: V-1..V-3 are kernel-track (main); V-4/V-5/V-7 are userspace and
aux-shaped; V-6 is kernel and audit-bearing. The arc can therefore run split across
both tracks after V-3.

---

## 11. Naming

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

## 12. Open questions (user signoff)

- **Q1 — the §4 fork.** In-kernel (A), userspace supervisor (B), or hybrid (C)?
  *Recommendation: C*, on the split rule in §4.
- **Q2 — v1.0 or v1.1?** Vivarium is a large arc (V-0..V-8) sitting beside Halcyon
  (G-8/G-9) in the endgame, and `ROADMAP §11.5` already makes v1.0-rc.1 the shippable
  fallback with Halcyon as the v1.1 candidate. Both cannot obviously fit. *This is
  the real strategic question this document exists to make answerable.*
- **Q3 — the `ELFOSABI_GNU` collision.** `ELFOSABI_LINUX == ELFOSABI_GNU == 3`, and
  the Clade arc widened the loader's accept-list to 3 for a *native* toolchain
  binary. Brand inference must not mis-brand Clade's output. Options: prefer
  `PT_INTERP` + the manifest and treat OSABI as a weak hint (recommended); or brand
  Thylacine-native output distinctly.
- **Q4 — naming** (§11): confirm Vivarium / phenotype / diorama.

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
