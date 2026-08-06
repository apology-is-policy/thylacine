---
id: sub-kernel-vivarium
type: sub
title: "The Linux phenotype: the syscall translation table"
parent: moc-kernel-entry
code: ["kernel/vivarium.c", "kernel/include/thylacine/vivarium.h"]
audit: hard
guarded-by: [inv-i43]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/VIVARIUM.md", "docs/LINEAGE.md"]
created: 2026-08-06
updated: 2026-08-06
---
## Purpose

A Linux binary issues Linux syscall numbers. This layer decides, for each
one, whether Thylacine can serve it **exactly** — and if it cannot, says so
rather than approximating. It is the decision half of the phenotype; the
uaccess, the handle table and the actual dispatch live in
[[sub-kernel-syscall-dispatch]].

The governing property is stated once and every function is shaped by it:

> the translator NEVER silently mistranslates; it either produces an
> exactly-equivalent call or declines.

Declining is always safe — the caller gets ENOSYS or a specific errno and
its own fallback runs. Accepting a flag we cannot honour is the failure
this whole surface exists to prevent, and the file's standard is that
**each admission is a claim about behaviour that has to be justified
individually**. "We ignore it and nothing seems to break" is not a
justification; it is the bug.

The file is **PURE**: no Proc, no user memory, no locks, no allocation, no
globals mutated. That is a constraint on the design, not a description of
what happened to be easy — it is what makes the whole decision layer
unit-testable against synthetic argument vectors with zero kernel
plumbing, and what keeps the auditable part separable from the part that
touches EL0.

## Contract

| Entry | Returns | Notes |
|---|---|---|
| `vivarium_translate(nr, args, out)` | `TRANSLATED` / `FORWARD` / `ENOSYS` / `TIER2` | `out` written **only** on TRANSLATED |
| `vivarium_openat_decide` | verdict + start_fd + omode + cloexec | measures no memory; the shell does the strnlen |
| `vivarium_openat_build` | void | the one place SYS_OPEN's argument order is written down |
| `vivarium_fstatat_decide` | verdict, and nothing else | the emptiness is the finding — see Mechanism |
| `vivarium_mmap_decide` | verdict | `len` deliberately not judged |
| `vivarium_clone_decide` | verdict + `share_mem` | the vfork shape vs the fork shape |
| `vivarium_wait4_decide` | verdict + `viv_wait_opts` | the bit-4 collision |
| `vivarium_{pipe2,dup3,fcntl,writev}_decide` | verdict + params | #150/#151/#155/#157 |
| `vivarium_{socket,listen}_decide`, the sockaddr/ctl codecs | bool + errno | V-5 |
| `vivarium_{sigaction,sigprocmask}_decide`, the note maps | verdict / mask | V-6 |
| `vivarium_stat_to_linux`, `vivarium_build_sigframe`, `vivarium_uname_fill` | void | fully write `out`, pads included |

Every `_decide` **fails closed on a NULL out-param** — returning FORWARD or
ENOSYS, never TRANSLATED — so a caller that ignores the verdict cannot be
handed a dispatchable number.

The three struct-filling functions zero their whole output before writing.
That is an [[inv-i13]] obligation rather than tidiness: each buffer is
copied to a guest, so a word left unwritten ships a slice of the kernel
stack.

## Mechanism

**The admission rule** (VIVARIUM.md section 4, binding). A Linux call may
be a table row iff its translation is *total and stateless*: a pure
renumber plus an argument-order or flag-bit mapping onto exactly one
existing `sys_*_for_proc`, with no new kernel state, no new error
semantics, and no policy.

**"Total" is the word that does the work**, and it is easy to misread as
"the arguments line up". The file carries two worked counterexamples, and
they are the best short statement of what this layer is for:

- `munmap(addr, len)` and `SYS_BURROW_DETACH(vaddr, length)` take the same
  two words in the same order — and burrow_detach requires an **exact VMA
  match** while Linux explicitly permits partial and multi-mapping unmaps
  *and succeeds on an unmapped range*. A renumber is wrong in two
  directions for a legal class of inputs, with no error anywhere.
- `writev(fd, iov, iovcnt)` and `SYS_WRITE(fd, buf, len)` are three
  arguments each — and arg 1 is a **pointer to an array of pointers**, arg
  2 an **entry count**. The renumber would write `iovcnt` bytes of the
  guest's own iovec array to the fd.

`F_DUPFD_CLOEXEC(fd, min)` is the third and the sharpest on authority:
SYS_DUP's second argument is a **rights mask**, so a renumber would read
`10` as capability bits and hand back a descriptor with arbitrary
authority, silently, for a legal input.

**The four verdicts.** TRANSLATED (dispatch `out`), TIER2 (admissible but
needs a named translator — the dispatcher must invoke it), ENOSYS (no
counterpart exists at all), FORWARD (needs state or policy the kernel does
not own). Unclassified numbers default to **FORWARD**, not ENOSYS:
claiming "this does not exist" about a call nobody has reached yet would
be a lie the guest cannot distinguish from a real one.

**The reject table is data, deliberately.** A number never considered and
a number considered and rejected are different facts; collapsing them
loses the analysis. Each ENOSYS row carries its own reason, and the rule
for TIER2 is that **a row lands with its shell in the same commit** —
a TIER2 row whose shell is missing declares a capability the code does not
have, which the dispatcher's default arm treats as a table/shell
disagreement and fails closed.

**The argument domain** is V-2b's refinement and the tool the rest of the
surface is built from. A flag map is inherently partial, so a T2 row is
admitted over a *stated domain* and anything outside it declines. This is
stricter than the rule it refines, not looser: it replaces "openat is a
table row" with a per-call check.

Each admission belongs to exactly one of two shapes, and the file is
explicit about which:

- **the flag requests behaviour we already provide unconditionally**, so
  honouring it is both a no-op and correct — `O_NOCTTY` (a controlling
  terminal is only ever acquired through the explicit SYS_TTY_ACQUIRE),
  `O_LARGEFILE` (every offset is 64-bit), `AT_NO_AUTOMOUNT` (a Plan 9
  namespace is composed explicitly; nothing mounts as a side effect of
  traversal, and that is a property of the model rather than a v1.0 gap);
- **a stated fidelity degradation**, published in VIVARIUM.md section 9's
  DEGRADED tier rather than buried — `PROT_NONE` yields a *writable*
  mapping, so guard pages are not protective under this phenotype.

`O_NOFOLLOW` and `AT_SYMLINK_NOFOLLOW` are the instructive rejects:
ignoring them is harmless **today** because symlinks do not exist, and
would become wrong the moment they land with nothing to catch it. *A flag
whose correctness depends on a feature being absent is a trap*, and
`AT_SYMLINK_NOFOLLOW` is rejected even though it costs every `lstat()`.

**`vivarium_fstatat_decide` returns a verdict and nothing else, and that
emptiness is a finding.** `openat` computes a rewritten start_fd because
SYS_OPEN takes one; SYS_STAT does not take a base at all — it is hardcoded
to the AT_FDCWD rule, and `sys_stat_for_proc` and `sys_open_handler`
perform that join through the same `territory_join_cwd` call, so the
correspondence is one implementation rather than two that agree. The
consequence cuts both ways: AT_FDCWD is free, and a real dirfd is not
merely unimplemented but **inexpressible**.

**The collision re-check.** A row's Linux number can equal an assigned
native number. Above `VIV_NATIVE_CEILING` the argument is discharged by
construction; below it, each row owes a per-number paragraph. The first
half is shared — a PHENO_LINUX Proc **cannot reach a native number at
all**, since every number it issues goes through this table — and the
second half asks what a *native program mis-declared as PHENO_LINUX* now
reaches. Every answer lands on "the caller's own memory, its own fds,
bounds-checked; never authority", which is the I-43 shape.

**The fd-freeing obligation** is the sharpest bug this family can have.
The socktab keys on the fd *number*, so a freed index whose `(proto, N)`
survives is handed to the next fd-creating call and a later `connect()`
writes its dial verb to a **stranger's connection**. It is discharged in
two different places, and the difference is not stylistic:

- `close` pays it in the **entry hook**, because a close whose fd carries
  an entry always proceeds;
- `dup3` pays it **inside its shell**, after every refusal and immediately
  before the install — it can be refused while `new` is a live socket, and
  an unconditional entry-time drop would destroy socket state on a call
  that failed.

`dup` and `close_range` are still FORWARD and still owe it, and each still
looks like a trivial renumber.

## Data structures

`struct viv_row` {linux_nr, thyla_nr, nargs} — `nargs` is never used to
copy (the whole six-word vector is copied verbatim, since a Linux caller
may leave unused words as garbage exactly as a native one does); it
records **which words the equivalence claim covers**, and the tests assert
on it.

`struct viv_sock` (16 B, pinned) — fd, `/net` connection number, the
remembered bind, proto, state. `proto` is knowable only at `socket()` and
never mentioned again, and recovering it later would mean decoding netd's
qid layout — refused, because `/net` is a mount point that need not be
netd. **Remembering it is the whole reason the table exists.** There is
deliberately no `bound` flag: an unbound socket and one bound to
`0.0.0.0:0` are indistinguishable in every path the table feeds.

`struct viv_socktab` / `struct viv_sigtab` — per-Proc, lazily allocated,
CAS-installed, freed at `proc_free`, **not** rfork-inherited. `viv_sigtab`
is indexed by *note kind*, not by signal number, which is legitimate only
because `viv_signal_owns_note_exclusively` gates every write.

The ABI mirrors are byte-pinned with `_Static_assert` on size and every
offset: `viv_linux_stat` (128), `viv_ksigaction` (32), `viv_linux_siginfo`
(128), `viv_linux_mcontext_head` (296), `viv_linux_ucontext_head` (472),
`viv_sigframe_head` (600), `viv_linux_utsname` (390), `viv_linux_iovec`
(16). The mcontext offset carries its own warning, because it cost a
measurement: `sigcontext.__reserved` is 16-aligned so it begins at **288,
not 280**, and the layout is the *target's* — the same probe compiled with
the host cc gives 2328 where `--target=aarch64-linux-gnu` gives 4384.

## Concurrency

**None, by construction.** Every function here is pure. There is no lock
in this file and no lock ordering to state, which is the point of the
decide/build split: the part that can race is the shell's, and it lives
next door.

The two per-Proc tables are read and written **lock-free**, and the
argument is a property of the clone row's argument domain rather than of
the tables: `vivarium_clone_decide` admits exactly two words by *exact
equality*, neither carrying CLONE_THREAD, so a PHENO_LINUX Proc cannot
obtain a peer thread and there is no peer to race. A `fork` yields a new
Proc with its own table.

**That argument evaporates the moment the domain admits the thread set** —
a one-line change with no compiler consequence anywhere near either
struct. The comment recording it was already corrected once at #157, when
the stated mechanism ("clone is not a table row") expired without anything
failing. Widening the domain means re-deriving this.

## Invariants enforced

- [[inv-i43]] — a phenotype confers ABI *shape*, never authority. Every
  per-number collision paragraph is an instance: what a mis-declared Proc
  reaches is always its own memory and its own descriptors.
- [[inv-i22]] — `vivarium_map_uid` reports PRINCIPAL_SYSTEM as 0 because
  raw pass-through would show `(uid_t)-2`, the historic *nobody*, which
  inverts the fact being asked about. It is safe by construction
  (PRINCIPAL_INVALID and GID_INVALID are both 0, so 0 is not assignable to
  any real principal) and **confers nothing**: every authority decision
  reads the real `principal_id` through perm_check or a CAP_* gate, so a
  container shell that believes it is root attempts privileged operations
  and is refused at the real gates exactly as before.
- [[inv-i12]] — `PROT_EXEC` is refused rather than degraded, as an
  allow-list of two bits rather than "everything except PROT_EXEC"
  (measured: aarch64 musl also defines PROT_BTI/PROT_MTE, and generic musl
  PROT_GROWSDOWN/GROWSUP, none honourable either).
- [[inv-i13]] — every struct copied to a guest is zeroed whole before
  fill. The signal frame's 4088 untouched `__reserved` bytes are the
  guest's *own* stack below its own sp, so nothing crosses a boundary.
- [[inv-i19]] — the signal layer is a decode onto notes that already
  exist. `viv_signote_is_deliverable` is measured against `g_known_notes`,
  not assumed: the `snare:*` family is absent because
  `proc_fault_terminate` calls `exits()` directly without `notes_post`, so
  a SIGSEGV handler is refused rather than stored where nothing reads it.

## Error paths

The disposition is itself a decision, and the file distinguishes three:

- **ENOSYS** — the surface is absent. `brk` (no break pointer to move),
  `mprotect` (no prot-mutation syscall exists at all), `sigaltstack`,
  `setsockopt`/`getsockopt` (`/net` exposes no option surface; answering
  "success" to a TCP_NODELAY the stack ignores is the silent lie).
- **A reproduced Linux errno** — where our domain *equals* Linux's, an
  out-of-domain value is refused exactly as Linux refuses it. `dup3`'s
  flags word is EINVAL, not ENOSYS, because `ksys_dup3` rejects the same
  set; replacing a specific errno with "this surface is absent" would be
  false.
- **EPERM with one exception** — `setuid`/`setgid`. ENOSYS would be false
  (Thylacine has a full identity model, just not a mutable one), and
  `setuid(getuid())` **succeeds on Linux**, so the identity-preserving
  no-op succeeds and every other call is EPERM. The comparison is made in
  the *guest's* number space, because that is the only value it has been
  shown.

`exceptfds` is the one where declining is load-bearing: native poll has no
POLLPRI, so dropping the bit silently turns a pure-exceptfds wait into an
**infinite block**, and treating it as POLLIN would report ordinary data
as an exception. Both dishonest options are worse than the error.

## Performance

Two linear scans over small constant tables (6 T1 rows, ~50 reject rows)
per translated syscall; no allocation, no lock, no memory access outside
the caller's frame. The socktab scan is bounded by `VIV_SOCK_MAX` (64,
chosen to keep the table under a page while staying generous against
PROC_HANDLE_MAX; exhaustion is EMFILE, not an extinction).

Not a measured hot path. If it becomes one, the T1 table is a candidate
for a direct-indexed array — the numbers are dense enough below 300 —
but that would trade the reject table's explicitness for speed, which is
the wrong trade while the surface is still growing.

## Prosecution

What a change must re-establish:

- **the per-number collision argument, for its own number.** The ceiling
  argument is not transferable; the file says so and a new row below the
  ceiling owes its own paragraph;
- **the fd-freeing obligation, in the arm its refusal structure demands** —
  not by copying whichever site is nearer;
- **that a TIER2 row lands with its shell**, in one commit;
- **purity.** A `_decide` that reads user memory, takes a lock, or touches
  a Proc has moved into the shell's job and taken the shell's hazards with
  it;
- **the lock-free argument for the two per-Proc tables**, which is a
  statement about `vivarium_clone_decide`'s admitted words and must be
  re-derived if those change;
- **allow-lists, not deny-lists**, for every flag word. Measured, aarch64
  defines flags a deny-list silently admits.

The strongest existing evidence is the file's own negative space: the
tests assert the *rejects* as well as the rows, so a row promoted without
its reasoning fails a test rather than passing quietly.

## Seams

- **`kill` / `tkill` / `tgkill`** are deliberately unlisted. They are the
  only signal rows that are an *authority* question rather than a
  disposition one — they name another Proc — so they must reuse an
  existing cross-Proc gate verbatim (SYS_POSTNOTE's parent-only check, or
  I-26's two-axis one), never invent a third.
- **SIGTERM has no note.** `interrupt` belongs to SIGINT alone since V-6b,
  because a shared note cannot carry two independent dispositions and
  `sigaction(SIGINT, SIG_IGN)` with SIGTERM at SIG_DFL is *unrepresentable*
  rather than merely approximate. A real SIGTERM wants its own supported
  note name, which is an addition to I-19's closed set and needs signoff.
  It becomes load-bearing the day `kill` lands.
- **`dup3` on a socket declines** rather than half-serving. Reproducing
  Linux needs a *refcounted* socktab entry, a real change to a table V-5
  audited; the idiom turned away is the inetd shape.
- **CLONE_THREAD** has a correct target already (SYS_THREAD_SPAWN) and
  should arrive with its own reasoning — and it invalidates the lock-free
  table argument when it does.

## Caveats

- **The header's opening block describes a system that no longer exists,
  and it is the first thing a reader of this surface sees.** Its "WHAT IS
  DELIBERATELY ABSENT" paragraph states that nothing here is wired into
  `syscall_dispatch`, that nothing can set `Proc.phenotype` to
  PHENO_LINUX, that `PHENO_LINUX` is referenced nowhere outside its own
  enum, and therefore that "the dispatch branch would today be branching
  on a field that is provably always 0". All four are false: `syscall.c`
  branches on the phenotype and calls `viv_linux_dispatch`, the spawn
  thunk assigns PHENO_LINUX from the `SPAWN_PHENO_LINUX` ABI bit, notes.c
  reads the field, and **the same header's own body references
  `viv_linux_dispatch` twice**. V-1b and V-7 both landed. The sharp
  consequence is not untidiness: an auditor reading the top of an
  I-43-bearing file is told the surface is unreachable dead code. Tracked
  as task #163.
- **`VIV_NATIVE_CEILING`'s declaration comment repeats the number the
  symbol exists to stop repeating — and it is already stale.** The
  constant is 105 (`SYS_RFORK`); the paragraph declaring it says "a new
  native syscall above **102**". The enum block a few lines up explains
  that the ceiling had already moved 100 → 102 while *four separate
  comments still said 100*, "which is why VIV_NATIVE_CEILING below exists
  as a symbol rather than as a number repeated in prose". The same
  paragraph also says "the two rows below it (pselect6 72, ppoll 73)";
  there are **seventeen** rows below 105. Tracked as task #164.
- **A dossier's file-level claims about wiring should be read against
  `syscall.c`, not against this file's prose.** Two of the three
  discrepancies above are of the same kind — a V-2-era snapshot preserved
  in a header whose body moved on — and the pattern is now well enough
  attested here to expect more.
- The `err` local in `vivarium_socket_decide` is assigned, never read, and
  suppressed with `(void)err`. Harmless; noted so a reader does not go
  looking for the path that consumes it.
- **`uname` claims release 4.4.0, and the choice is a real one.** No number
  is honest — ours is a subset of every version's — so the question is
  which direction to be wrong in, and low is safer. 4.4 is the newest
  kernel that promises nothing we lack (it predates statx, io_uring,
  clone3, openat2, faccessat2 and close_range) while clearing glibc's
  3.2 minimum, below which a glibc binary aborts before `main()`. The
  `version` field carries "Thylacine" on purpose, because programs do not
  parse it.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
