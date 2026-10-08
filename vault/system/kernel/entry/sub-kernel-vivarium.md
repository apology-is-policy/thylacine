---
id: sub-kernel-vivarium
type: sub
title: "The Linux phenotype: the syscall translation table"
parent: moc-kernel-entry
code: ["kernel/vivarium.c", "kernel/include/thylacine/vivarium.h", "kernel/test/test_viv_sock.c"]
audit: hard
guarded-by: [inv-i43]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/VIVARIUM.md", "docs/LINEAGE.md"]
created: 2026-08-06
updated: 2026-10-07
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
| `vivarium_{openat_create,mkdirat,unlinkat,renameat}_decide` | verdict + params | #50 path-mutation family (the create/remove decisions) |
| `vivarium_{mmap_file,mmap_fixed_file,mmap_fixed_anon}_decide` | verdict | DISTRO D-3 file-backed mmap; PROT_WRITE **refused** to keep I-36; the fixed-anon arm admits PROT_NONE since B-1a; both fixed arms are confined to the burrow window since B-1a' (`fixed_addr_ok`) |
| `vivarium_mprotect_decide` | verdict | B-1a: the prot word alone; `addr` / `len` are the shell's (a zero length succeeds, an unaligned address is EINVAL) |
| `vivarium_madvise_decide` | verdict + kind | B-1b: the advice word alone -- RELEASE (`DONTNEED` / `FREE`) over the decommit core, HINT (twelve) answers 0 / ENOMEM and changes nothing, everything else declines; `addr` / `len` are the shell's |
| `vivarium_{ppoll,pselect6}_decide` | verdict + params | the poll family; `exceptfds`/POLLPRI is the load-bearing decline (Error paths) |
| `vivarium_clock_nanosleep_decide` | errno + clock + abstime | 6.29: EINVAL exactly where the gettime map refuses (derived), EOPNOTSUPP for MONOTONIC_RAW and both COARSE clocks; only `TIMER_ABSTIME` is read |
| `vivarium_sleep_req_ns`, `vivarium_sleep_verdict` | bool / verdict | 6.29: the request's validity and saturation; at the deadline the clock beats every wait outcome |
| `vivarium_clock_sleep` | 0 / `-EINTR` | 6.29: the one entry here that sleeps; a relative sleep counts on the monotonic clock, an absolute `CLOCK_REALTIME` one hooks the step list |
| `vivarium_{recvfrom,recvmsg,sendto}_decide` | bool + errno | V-5 socket data path |
| `vivarium_{faccessat,ioctl,futex,getsockopt}_decide` | verdict | the 6.26 git batch + misc |
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
  two words in the same order — and until B-1a' burrow_detach required an
  **exact VMA match** while Linux explicitly permits partial and
  multi-mapping unmaps *and succeeds on an unmapped range*, so a renumber
  was wrong in two directions for a legal class of inputs, with no error
  anywhere. Since B-1a' both are the range form over one core, and what
  still makes a renumber wrong is the ERROR CONVENTION (the native answers
  -1, the row must answer Linux's errno) and the WINDOW (a `munmap` below it
  is declined, never faked) -- which is why the row is a tier-2 shell rather
  than a renumber.
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
  DEGRADED tier rather than buried. The one example this bullet carried --
  `PROT_NONE` yields a *writable* mapping, so guard pages are not protective
  under this phenotype -- ENDED at B-1a (2026-09-23): the anon arm now mints
  the Linux prot exactly under an RW ceiling, and the section-9 row reads
  ENDED. The shape of the bullet stands for the next one.

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

**The mmap decision fanned into four arms at D-3, and the file arms are an
allow-list guarding I-36.** `vivarium_mmap_decide` once judged a purely-anonymous
map; the phenotype now also serves file-backed and fixed maps through
`vivarium_mmap_file_decide` / `_fixed_file_decide` / `_fixed_anon_decide`. The
file arms admit a read/exec map that rides a shared `BURROW_TYPE_FILE` Burrow
demand-paged from the file (the I-36 generalization to phenotype mmap-time), and
**refuse `PROT_WRITE` outright** — that Burrow has no write-back path, so a
writable file map would either lose the guest's writes or leak them into the file
and into every other Proc sharing the Image. The refusal keeps "no userspace
writable file mapping exists" true by construction, and it is an allow-list
(`PROT_BTI`/`PROT_MTE`/`PROT_GROWSDOWN` fall outside it unenumerated) for the
same reason the anon arm is; `len` is still deliberately unjudged.

**The two anonymous arms changed at B-1a.** `vivarium_mmap_decide`'s allow-list
(`VIV_MMAP_PROT_ADMITTED` = R|W) is unchanged, but `PROT_NONE` inside it is now
minted EXACTLY rather than degraded: the shell mints the Linux prot through
`sys_burrow_reserve_for_proc` under an RW ceiling, so a `PROT_NONE` mapping
faults until `mprotect` raises it and a `PROT_READ` one is read-only. And
`vivarium_mmap_fixed_anon_decide` ADMITS `PROT_NONE` where it used to decline
-- a FIXED none window over an existing mapping is a guard, and the shell
mints it at none under an RW ceiling. Until the raise existed a decline was the
only honest answer: a writable page where a guard was asked for would have
been a hole, not a degradation. `PROT_NONE` still DECLINES on the FILE arm: a
none file window is a pure reservation with no raise path for file-backed
pages at v1.

**Both fixed arms are confined to the burrow window (B-1a').** `fixed_addr_ok`
refuses, beside NULL and a misaligned address, any `addr` outside
`[EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP)`: the `munmap` row is
window-confined, so a fixed mapping placed below the window could never be
unmapped and leaked for the life of the process (pheno-probe L21, every boot,
until this chunk). musl's `map_library` overlays land inside the reservation it
just made, which is in the window, so nothing served is lost; a request below
it is declined honestly (`VIV_FORWARD` -> ENOSYS + the unserved line) instead
of half-served, and the kernel's `mmap_fixed_window` bounds the same window
again ([[sub-kernel-syscall-dispatch]]). `vivarium.mmap_fixed_domain` pins both
arms' decline at `0x40001000`, and the arms-disjoint sweep gained
`0x100001000` beside it so a real page inside the window still reaches
admission.

**The #50 path-mutation family is the create/remove half of the layer.**
`openat`'s `O_CREAT` (`vivarium_openat_create_decide`), `mkdirat`, `unlinkat` and
`renameat` each map the Linux flag/mode word onto the native create/remove parent
resolution — the same "total or decline" bar as every other row, now applied to
the mutation verbs the coreutils and git issue. They are ordinary instances of
the admission rule, called out here only because the Contract table would
otherwise look frozen at the read-only surface it had before the
git-under-VIVARIUM work.

## Data structures

`struct viv_row` {linux_nr, thyla_nr, nargs} — `nargs` is never used to
copy (the whole six-word vector is copied verbatim, since a Linux caller
may leave unused words as garbage exactly as a native one does); it
records **which words the equivalence claim covers**, and the tests assert
on it.

`struct viv_sock` (32 B, pinned: 16 B until N-2a's recorded remote made it 24,
and the u64 claim epoch, the keyed writers' identity key, made it 32) — fd,
`/net` connection number, the remembered bind, the recorded remote, the epoch,
proto, state. `proto` is knowable only at `socket()` and
never mentioned again, and recovering it later would mean decoding netd's
qid layout — refused, because `/net` is a mount point that need not be
netd. **Remembering it is the whole reason the table exists.** There is
deliberately no `bound` flag: an unbound socket and one bound to
`0.0.0.0:0` are indistinguishable in every path the table feeds.

`struct viv_socktab` / `struct viv_sigtab` — per-Proc, lazily allocated,
CAS-installed, freed at `proc_free` **and nowhere else**, **not**
rfork-inherited. Since NP-5 the socktab also holds `ready[VIV_SOCK_MAX]`, each
row's cached readiness Spoor, beside the rows rather than inside `viv_sock` so
that no snapshot or copy of a row can carry a borrowed pointer; each row owns
one reference, a FREE row's is NULL, rows naming one connection (a socket and
its dups) share one Spoor, and the cache (not the table) is released at exit
(below). That sentence was incomplete when written — the table was also
freed at exec, unmentioned — and it became true by the other free site being
*deleted* rather than by the prose being edited (see Concurrency). `viv_sigtab`
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

**No lock, and no longer "no mutation".** There is still no lock in this
file and no lock ordering to state, which is the point of the decide/build
split: the part that can race is the shell's, and it lives next door. But
the claim that every function here is *pure* is now false in one place —
`viv_sigtab_reset` mutates a table and carries a release fence. The
narrowing matters because purity was doing argumentative work below.

### The lock-free argument was refuted, and not at the edge it guarded

The recorded argument was a property of the clone row's argument domain
rather than of the tables: the clone decision admits exactly two flag words
by *exact equality*, neither carrying the thread-sharing bit, so a
Linux-phenotype process cannot obtain a peer thread and there is no peer to
race. The caveat attached to it said the argument *"evaporates the moment
the domain admits the thread set"*, and told a future reader to re-derive
it if the domain widened.

**The domain never widened, and the argument was refuted anyway.** The
readers that actually raced were never peer *threads* — they were other
**processes**, reaching in through the note-post path, which takes the
posting process as an explicit parameter and loads the target's table twice
with a bare acquire and no lock: once in the ignore-disposition hook, once
through the live-handler query on the interrupt-terminate arm. So an exec
racing a note post from *any* other process freed the table under a live
reader.

No-thread-sharing was never sufficient. The argument was already
insufficient the day it was written, and its stated trip-wire watches an
axis the defect does not travel on.

Enumerating the readers by *enclosing function* rather than by grep hit
finds **four** lock-free loads in the notes layer, not two: the two
cross-process ones above, plus the Linux delivery path's reset-handler arm
and the return-to-user-mode delivery entry. Both of those are genuinely
same-process, so the exec-alone gate really does cover them and the
cross-process count stands at two.

Stating the total matters more than it looks. A reader who greps finds four
and, handed an enumeration of two or three, cannot tell which of the
unaccounted sites was evaluated and which was missed — and **the one most
easily dropped is the one the original false comment was about**, since the
pre-fix text named the return-to-user-mode delivery entry as the sigtab's
"only reader". That is the shape that gets a closed finding re-opened.

### The same sentence is sound at one site and unsound at another

The delivery entry carries, in capitals, directly above its load:
*widening that domain to admit the thread-sharing flag voids this argument*
— the identical trip-wire as the refuted paragraph, on the identical axis.
**And there it is correct**, because that reader really is the target's own
thread, so peer threads really are the only racers and bounding them really
does the job.

So the generalization is stronger than "an argument can be precise about the
wrong scope":

> **The identical argument is sound at one site and unsound at another, so
> soundness cannot be inherited by copying the sentence — it has to be
> re-derived per reader.** Several copies are not evidence the claim was
> checked several times; they are several chances for one to be wrong, and
> the wrong one is indistinguishable from the right ones.

The prediction that follows — look for *other* sites restating the same
bound — was run, and it found two more, one of them live. The field's own
**declaration site** still carries the refuted sentence verbatim, and its
header records that the same paragraph has already been corrected twice
before (once for a wrong claim, once for a reason that expired while the
claim stayed). The socket table beside it then inherits its safety *by
pointing at that paragraph* — though its conclusion happens to survive for
a different and stronger reason: every one of its readers is same-process
**by construction**, because unlike the note-post path there is no
cross-process entry point to it at all. Task #179.

**The generalizable half:** *a safety argument can be precise about the
wrong scope.* The gate it rests on is real and correctly derived — the
clone domain genuinely does exclude peer threads — but it bounds *threads*
and was used to prove a claim about *processes*. A correct premise, a
correct derivation from it, and a conclusion that does not follow. That is
harder to catch than a wrong premise, because everything checkable checks
out. ([[sub-kernel-death]] records the same shape found independently on
the other side of the same fix, forty lines from a *valid* use of the same
gate.)

### The severity was escalated and then withdrawn, and the reporter was right

Worth recording because the correction runs **downward**, which almost never
gets written down.

The defect arrived from the other track as a **use-after-free read**. Verifying
it locally turned up two writer call sites, and on their strength the severity
was raised to "use-after-free *writes* — heap corruption".

**That escalation was wrong and was withdrawn.** Both writers run on a thread of
the target process, exactly like the two same-process readers, so the
exec-alone gate really does exclude them. The reporter's narrower original read
was the correct one, and the ceiling is **a wrong disposition, not corruption**.

Two things generalize. **Finding more call sites is not the same as finding more
exposure** — the count went up and the reachable set did not, because the new
sites sat inside the gate that was already holding. And **a reporter who scoped
their own claim carefully deserves to have that scope tested before it is
widened**, since the widening here came from the verifier, not from the report.

The scope check itself was right to run: the report flagged one reader and said
plainly it had not audited this tree for others. This tree has seven sites.
Checking rather than trusting is what found the four readers enumerated above —
and also what produced the withdrawn escalation. **The same diligence produced
the real finding and the false one.**

### The fix, and why it is written per field

The exec path no longer frees the table; it **zeroes it in place**, so the
allocation lives until reap — the lifetime it had before the free was moved
forward to exec. Every accessor is null-safe, so an all-default table and a
null one answer identically, which is exactly why the free looked harmless.

The reset writes **per field** rather than as a block, and that is measured
rather than stylistic: under the kernel's freestanding, no-builtin build the
compiler cannot form a block store from a byte loop, and a byte loop emits
half-word stores — **torn writes under a lock-free cross-process reader**.
The store width is an ABI property here, not an optimisation detail.

The measured artefact is worth stating exactly, because the reasoning is the
reusable part. The byte loop compiled to an **unroll-by-two emitting 2-byte
stores** — precisely *because* `-ffreestanding -fno-builtin` are what stop the
compiler recognising the loop as a block fill. So each eight-byte handler was
written as **four independent halfword stores**, and a concurrent reader could
observe a handler value **no code ever wrote** — half an old address, half zero
— and pass the validity gate on it.

**The flags that make this a kernel are the flags that make the idiom unsafe.**
A byte loop is a perfectly good block fill in a hosted build, where the compiler
recycles it into one; here it is guaranteed not to be, which inverts the usual
intuition about which spelling is conservative.

A field-wise struct assignment gives paired register stores instead —
single-copy-atomic at the eight-byte granule, which is the granule every
accessor actually reads.

**What it deliberately does NOT promise**: a reader still sees an arbitrary
*mix* of pre- and post-reset entries. That is the POSIX exec-versus-signal race
and is fine, because every entry it sees is one that was genuinely installed or
the default. **The guarantee is per-field integrity, not a snapshot** — and
saying so is the difference between a bounded claim and one a later reader will
over-read into atomicity the code never had.

The memory-safety half is split into its own function so it can be unit
tested, with the exec-alone precondition stated on it.

### The test asserts pointer identity, and that is the only observable

Because the accessors are null-safe, no end-to-end test can distinguish the
fix from the defect: a freed-and-nulled table and a zeroed one behave
identically at every call site. **Behavioural invisibility.** So the
regression asserts that the table pointer is *unchanged* across the reset
(plus that every byte is zero), which looks like testing an implementation
detail and is not — it is testing the only thing that separates the two
states.

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
- [[inv-i36]] — the D-3 file-backed mmap arms refuse `PROT_WRITE`, which is what
  keeps "no userspace writable file mapping exists" true on the phenotype path:
  the shared `BURROW_TYPE_FILE` Burrow they ride is demand-paged from the file
  with no write-back, so a writable map would lose or leak the guest's writes
  (and corrupt every other Proc sharing the Image). Read/exec file maps are
  admitted (I-36 generalized to mmap-time); the refusal is by allow-list, not by
  naming `PROT_WRITE`.
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

- **ENOSYS** — the surface is absent. `brk` (no break pointer to move), a
  `munmap` or a fixed `mmap` below the burrow window (a mapping there is not
  the phenotype's to place or unmap; B-1a'), `sigaltstack`,
  `setsockopt`, and `getsockopt` beyond `(SOL_SOCKET, SO_ERROR)` (`/net`
  exposes no option surface; answering "success" to a TCP_NODELAY the stack
  ignores is the silent lie).
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

Two linear scans over small constant tables (11 T1 rows, 73 reject rows)
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
- **`VIV_NATIVE_CEILING`'s declaration comment used to repeat the number the
  symbol exists to stop repeating, and went stale seven times.** The constant
  is now **127** (`SYS_JIT_CREATE_SEALED`, B-2b). The remedy was never going to be
  a person remembering: since the 2026-09-17 PCI rewrite the assert is pinned
  to the `SYS__NATIVE_TOP - 1` sentinel, which the compiler recomputes on every
  append, and the declaration comment narrates that drift history instead of a
  number -- so B-1a's append moved the sentinel, the assert failed until the
  constant was bumped with it, and no prose lagged. The `pselect6 72, ppoll 73`
  paragraph is still a two-row sample of a larger set: **forty-three**
  `VIV_LINUX_*` enum values lie below 125. Task #164's substance (the
  self-repeating number) is closed by the sentinel; the sample-size wording is
  what remains of it.
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

## Native ceiling 114 (2026-09-17)

The PCI mapping API raises the native ceiling to 114. Linux clock_gettime 113
now overlaps native PCI_MAP_WINDOW, so its old above-ceiling proof no longer
applies. The existing Tier-2 row consumes the Linux syscall and invokes the
CLOCK_GETTIME handler directly. A compile-time equality pin records this specific
collision; all remaining above-ceiling assertions still compile. [[abi-pci-windows]].

## Native ceiling 125 and the mprotect row (2026-09-23, B-1a)

`VIV_NATIVE_CEILING` is 125 (`SYS_BURROW_PROTECT`), pinned to
`SYS__NATIVE_TOP - 1` by `vivarium.c`'s static assert, so the append moved the
sentinel and the constant with it. Linux `mprotect` (226) is above the
ceiling; its collision argument is discharged by construction.

`{ VIV_LINUX_MPROTECT, VIV_TIER2 }` lands with its shell (the tier-2 rule):
`vivarium_mprotect_decide(addr, len, prot)` judges the prot word ALONE -- an
allow-list of `PROT_READ | PROT_WRITE | PROT_EXEC`, so `PROT_BTI` /
`PROT_MTE` / `PROT_GROWSDOWN` / `PROT_GROWSUP` decline without being
enumerated -- while `addr` / `len` are semantic questions the shell answers
exactly (a zero length succeeds having touched nothing; an unaligned address
is the target's EINVAL). `PROT_EXEC` is INSIDE the domain although it is
always refused: the refusal is the target's stated EACCES ("X is never a
target"), which a guest must see as that errno and not as ENOSYS. The two
prot words agree by `_Static_assert` (`BURROW_PROT_* == VIV_PROT_*` in
`kernel/syscall.c`), which is what lets the shell hand the word through. The
shell is [[sub-kernel-syscall-dispatch]]'s. `vivarium.mprotect_domain` pins the
domain; the two `test_vivarium` asserts that pinned 226 at ENOSYS now expect
TIER2, and the fixed-anon `PROT_NONE` assert that pinned a decline now expects
TRANSLATED -- the old legs were deliberate tripwires for the degradation, and
they fired as designed on the chunk's first boot.

The header's `mprotect` paragraph (the old "ENOSYS; musl tolerates it, glibc
would not") and the 6.21 degradation prose are rewritten; the "WHAT IS
DELIBERATELY ABSENT" block (task #163, the first caveat above) is not.

## B-1a': the fixed arms are window-confined; the munmap row is the range form (2026-09-23)

`fixed_addr_ok` gained the window test (Mechanism, above), and `vivarium.c`
includes `exec.h` for the two bounds. The `VIV_LINUX_MUNMAP` row's shell
([[sub-kernel-syscall-dispatch]]) now returns `sys_munmap_range_for_proc`'s
value as is after Linux's two argument errors: the native core's `-T_E_INVAL`
/ `-T_E_ACCES` / `-T_E_NOMEM` reach the guest as EINVAL / EACCES / ENOMEM, and
a range below the window as ENOSYS -- where the row used to answer ENOSYS for
every refusal including a boundary straddle, which Linux serves and the core
now serves too. The EL0 witnesses in `viv-pheno-probe` (`run_linux`): L21 maps
`MAP_FIXED | MAP_ANON` at `0x1_4000_0000` (a GiB into the window, above
anything the guest has mapped) and L21b writes and reads back through its last
page; L21c asserts its `munmap` answers 0 -- asserted rather than swallowed,
since a fixed mapping that cannot be unmapped is a leak per call; L21d asserts
a fixed request at `0x40000000` answers ENOSYS. The mprotect legs' "never
mapped" range stays `0x50000000` -- a range no leg has ever mapped is the only
honest unmapped. `test_vivarium.c`'s `mmap_fixed_domain` adds the two
below-window declines and its arms-disjoint sweep the in-window page. The
probe itself remains unowned ([[sub-kernel-protect-witness]] Seams).

## The madvise row (2026-09-23, B-1b)

`{ VIV_LINUX_MADVISE, VIV_TIER2 }` (233) lands with its shell, so a Linux
guest's allocator returns memory the way Pouch's and the native one do (ARCH
6.5 "Capacity"; VIVARIUM 6.28). `vivarium_madvise_decide(advice, &kind)` judges
the advice word ALONE, narrowed to 32 bits as the mprotect row's prot is:
`MADV_DONTNEED` (4) and `MADV_FREE` (8) are RELEASE; the twelve pure hints
(NORMAL, RANDOM, SEQUENTIAL, WILLNEED, HUGEPAGE, NOHUGEPAGE, DONTDUMP, DODUMP,
COLD, PAGEOUT, POPULATE_READ, POPULATE_WRITE) are HINT; the fork-semantic
quartet, KSM's pair, REMOVE, the poison testers and any unknown value decline
(the tier's rule: none can be given Linux's meaning here, and a false 0 is a
lie a later fork would expose). The shell ([[sub-kernel-syscall-dispatch]])
keeps Linux's argument order -- the advice first, an unaligned address EINVAL,
a zero length 0 -- and hands a RELEASE to `sys_burrow_decommit_core` (the
-errno form of `SYS_BURROW_DECOMMIT`, which the native 84 flattens to -1): a
hole ENOMEM, a mapping that is not plain anonymous memory of this space EINVAL.
The core is window-confined like the range detach, and the shell refines its
`-T_E_NOSYS` for a range outside the window with `vma_range_is_mapped_in`: an
UNMAPPED range is still ENOMEM (a hole is a hole wherever it lies -- the first
B-1b boot failed pheno-probe L23m, a hole at `0x50000000` answered ENOSYS), a
MAPPED one is ENOSYS with its bytes untouched (a false ENOMEM would tell an
allocator its own `.bss` is unmapped). A HINT is 0 over a mapped range and
ENOMEM over a hole. Three divergences are recorded, not served (holotype F6;
VIVARIUM 6.28): a RELEASE over a private FILE window is EINVAL (Linux drops
the private pages); a range mixing mapped and unmapped parts releases nothing
(Linux releases the mapped parts, then answers ENOMEM); a length past the
window is EINVAL before any hole is looked for (Linux: ENOMEM). I-43 holds: the
row confers no authority the native decommit does not gate. Witnesses: `vivarium.madvise_domain` (the three sets,
the narrowing, a NULL out-parameter declining), the tier asserts at both
sites, `vma.range_is_mapped`, and pheno-probe legs L23j-L23t (a written page
released and re-read zero; a hole ENOMEM for both kinds; DONTFORK ENOSYS; an
unaligned start EINVAL; a zero length 0; the probe's own data page declined
and intact). The sabotage `nodecommit` (the arm answers 0 without the core)
reddens L23l.

## Native ceiling 126 (2026-09-24, B-1d)

`VIV_NATIVE_CEILING` is 126 (`SYS_BURROW_MAP_FILE`), again moved by the
sentinel rather than by hand. No vivarium row lies at 126 or 127, and the
lowest row the ceiling argues for is `restart_syscall` (128), so the move voids
no row's argument. The phenotype's file-backed `mmap` rows and the new native
number now call the same three D-3 cores ([[sub-kernel-syscall-dispatch]]):
each entry decides its own word and hands the cores the same prot encoding, so
the phenotype's deciders did not change.

## Native ceiling 127 (2026-10-07, B-2b)

`VIV_NATIVE_CEILING` is 127 (`SYS_JIT_CREATE_SEALED`), moved by the sentinel.
Of the table's 100 Linux numbers none lies between 120 and 127, so the move
voids no row's argument. 127 is the last number below `restart_syscall`
(128): the NEXT native append lands on that row and owes it a per-number
collision paragraph, as `pselect6` and `ppoll` have, before the ceiling can
move again. A sealed JIT region is native-only; the phenotype's `PROT_EXEC`
mappings stay readable, because `vma_alloc` promotes EXEC-alone to
`READ | EXEC` everywhere but a code Burrow ([[sub-kernel-vma]]).

## A zero-timeout ppoll is no longer widened (2026-09-28, #98 NP-4c)

`VIV_PPOLL_PROBE_MS` (10 ms) is gone from `vivarium.h`. It was the budget
`viv_poll_translated` gave a guest `ppoll` or `pselect6` whose timeout was 0
when a `/net` socket was in the set: the poll core answered a socket from a
cache that a freshly opened `ready` file did not have yet, so a strict
zero-timeout scan reported a writable socket not ready, and a guest polling
with timeout 0 in a loop made no progress. The core now asks netd for a
snapshot, which netd answers at once ([[sub-kernel-poll]],
[[sub-kernel-ninep-dev9p-poll]]), so the guest's 0 passes through unchanged and
still gets netd's verdict. The cost moved rather than vanished: every pass over
a socket is a netd round trip, timeout 0 included (VIVARIUM.md's DEGRADED row).
The deciders did not change. Two costs stay until NP-5 keeps ready Spoors
outside the guest's fd table: the Twalk+Tlopen+Tclunk each polled socket costs
per call, and the guest fd number each one borrows (V-5d F6). (Both closed by
NP-5, the next section.) The witness is
viv-pheno-probe L113, a ready socket polled at timeout 0.

## The readiness cache, private socket files, and netd's nonblocking mode (2026-09-29, NP-5)

**NP-5, the readiness cache.** `viv_poll_translated` (kernel/syscall.c) no
longer opens `/net/<proto>/N/ready` as a guest fd per socket per call. A
socket's first poll resolves it as a private Spoor (`sys_resolve_kpath_for_proc`,
the resolve half of `sys_open_kpath_for_proc`) and offers it to the row
(`viv_socktab_ready_install`, keyed on fd + epoch like every keyed write; a row
closed, recycled or filled meanwhile refuses, and the Spoor then serves that
call only). Later polls take a new reference from the row
(`viv_socktab_ready_get`). ONE Spoor per connection per table (NP-5 round-1
F1): a row whose connection another row caches -- a `dup` of a polled socket --
shares that Spoor with a reference of its own (`ready_get`'s sibling scan,
`socktab_conn_ready_locked`), and an install for a connection a sibling cached
first is refused and shares instead. Cached per ROW, as first written, one Proc
could hold 64 netd fids by polling a socket's 63 dups, and netd's fid table is
one pool for the whole box ([[sub-netd-server]]); the probe's L299/L300 are
that case. The poll core receives the Spoors pre-resolved
(`sys_poll_for_proc_spoors`, [[sub-kernel-poll]]); `kfds[i].fd` keeps the
guest's own number throughout, and V-5d F1's compaction moves the borrowed
`pre[]` with `kfds[]` while the owning `ready[]` stays in caller order. A
resolve that fails `T_E_NOMEM` fails the call ENOMEM; any other failure marks
that entry POLLNVAL. A full netd fid table lands in the second arm, because
stalk reports a failed device walk as ENOENT (a queued P3).

**Release.** Every path that clears a row detaches its Spoor under the leaf
spinlock and clunks it after the unlock (a last clunk is a Tclunk): drop (the
close hook), claim's replace-on-claim, an alias onto a cached number, reset
(native exec, and now exit), drop_cloexec (exec-alone, no lock) and free
(`proc_free`). A fork child's rows and a new alias row start uncached (the
alias then shares its source's Spoor on its first poll; a fork child opens its
own, so a socket polled by P Procs holds P readiness fids). The exit
reset (`proc_close_handles_at_exit`, [[sub-kernel-death]]) is load-bearing: fds
close at exit, and a cached ready fid holds netd's slot N (every fid under
`/net/<p>/N/` refs it), so a cache released only at reap kept a forked worker's
accepted connection open to its peer until the parent reaped.

**NP-5b, the private socket files.** The same by-number class lived in four
more arms: `connect` parked the data file on a temporary fd before
`handle_replace`; `accept` opened `listen`, `remote` and `data` as guest fds;
an unconnected UDP `sendto` and `recvmsg` each opened `data` as a transient fd.
Each is now a private Spoor (`viv_sock_resolve`), and its I/O goes through
`spoor_read_on` / `spoor_write_on` (the Spoor halves of `spoor_read_common` /
`spoor_write_common`, so a Spoor no fd names meets the same gates in the same
order) and `sys_write_staged` (SYS_WRITE's bounce and copy-in tail). accept's
final install answers a full table EMFILE, as Linux does (it was ECONNABORTED).

**NP-5c, the nonblocking mode reaches netd.** Found hunting the hang the NP-5b
probe legs first caused, where a blocking UDP recvmsg with nothing in flight
parked at netd (correct for a blocking socket). `O_NONBLOCK` on a socket set
only the Spoor's CNONBLOCK, which dev9p never reads, and netd parks an empty
`data` read unless its `nonblock` ctl verb has been written (#52). recvmsg's
N-1b mapping, 0 bytes -> EAGAIN on a nonblocking socket, rested on the pre-#52
premise that netd answered 0 on an empty socket; 0 is only end of stream, so a
closed peer read as EAGAIN while an empty socket blocked. Now `socket()`'s
SOCK_NONBLOCK and `F_SETFL` write the verb through a private ctl Spoor
(`viv_sock_sync_nonblock`). The verb carries the flag as read back after each
write lands and repeats while a peer thread moved it, so the Spoor's bit and
netd's mode agree once the setters stop, with no lock held across the RPC. A
failed verb puts the bit back (`F_SETFL`) or unwinds the socket (`socket()`),
answering ENOMEM for a shortage and EIO otherwise. recvmsg returns 0 for 0 bytes
whatever the mode. A nonblocking `connect` or `accept` still blocks (VIVARIUM.md
ceilings; queued).

**Witnesses.** Unit: `vivarium.socktab_ready_cache` (since round 1 also the
per-connection sharing: a dup's row shares, a racing install is refused and
shares, another n or the other protocol does not, a drop keeps the Spoor for the
rows left), `vivarium.socktab_ready_release_paths` and
`poll.pre_resolved_spoor`, each run red under its own sabotage. On device,
viv-pheno-probe's Linux run: L297/L298 (a socket behind a caller-disabled entry
waits, and answers at its own index), L299/L300 (a listener's 62 dups polled at
once all report POLLIN -- one readiness fid, where one per row overflows netd's
table and returns POLLNVAL); L278-L285
(while a peer thread blocks in ppoll, the lowest free fd stays free and a dup3
onto it survives the poll's end); L286-L290 (a nonblocking socket at end of
stream reads 0, and an empty one answers EAGAIN, whether born nonblocking or
made so by F_SETFL); L291-L296 (a peer thread takes the lowest free number over
and over while this thread sends, receives, sets the mode and dials, and every
copy lands on the same number).

## Only signal(7)'s slow calls are note-interruptible (2026-09-29)

`viv_linux_dispatch` now decides, once per call and from the Linux number
before it is renumbered, whether a caught note may interrupt it, and stores the
answer in the thread's `note_interruptible` ([[sub-kernel-notes]]).
`vivarium_intr_class` is the pure half, a switch beside the tables: accept,
accept4, connect, recvfrom, recvmsg, sendto, sendmsg, wait4, ppoll, pselect6,
futex, rt_sigsuspend and rt_sigtimedwait always; fcntl only for F_SETLKW and
F_OFD_SETLKW; read, readv, write, writev, pread64, pwrite64 and ioctl only on a
slow file; everything else never. A row added later defaults to never, so the
failure is a handler that runs late, never a spurious `EINTR`.
`viv_fd_is_slow` is the dispatcher's half: a socktab row is slow; any other file
is slow when its Dev's stat type is `S_IFIFO` or `S_IFCHR`. The answer is cached
per open file in two Spoor flag bits (`CSLOWKNOWN`, `CSLOW`,
[[sub-kernel-spoor]]), so a 9P file costs one Tgetattr per open file. A failed
stat answers "not slow" and is not cached, so a stat that a group exit cut short
cannot pin a fork-shared pts as uninterruptible. Before this every 9P-backed call
was interruptible, and NP-5's SMP gate caught `socket()` failing with `EINTR`
(3 boots in 50). Witnesses: the kernel tests `vivarium.intr_class` and
`vivarium.fd_is_slow_cached`, and viv-pheno-probe L301-L310 -- a socket read
that must return `EINTR` (the positive control, L305) and a loop of socket,
openat, read and newfstatat calls that must not while children exit on a
stagger (L309a-d). L305's interrupt is a child's exit, so it also rests on the
caught-note wake at the `child_exit` post ([[sub-kernel-death]]); without that
wake the read was interrupted only when unrelated traffic woke it, and L305
failed about half its boots. Pre-existing and owned: an EINTR'd 9P op still
discards a late original reply, where flush(5) says it must be honoured -- a
fix for the 9P client's abandon path
([[dec-2026-09-29-caught-signal-slow-calls]]).

The socket calls keep Linux's shape at the boundary: a signal interrupts the
call's wait and nothing else. `accept`'s wait is netd's held `listen` open. Once
it returns, the connection is the guest's, and `viv_wait_is_over` clears the flag
so the reads and the `data` open that follow cannot hang it up. `connect`'s wait
is TCP's handshake, the held `data` open. `viv_sock_connect_dial` holds the note
off across the dial verb (`viv_note_hold`), because a `Twrite` abandoned for a
note may already have dialed, and gives it back for the `data` open only for TCP;
a UDP connect never waits. An interrupted handshake returns `-T_E_INTR` and
leaves the row `VIV_SOCK_CONNECTING`: `viv_socktab_begin_connect` recorded the
dialed peer when netd accepted the verb, so a retry waits on the dial already
made instead of writing the verb again, which netd refuses for a slot that has
dialed. The retry's own address is ignored, as Linux ignores it in
`SS_CONNECTING`. A failed `data` open resets the row to `FRESH`
(`viv_socktab_abort_connect`) and reports netd's verdict -- `ETIMEDOUT` for a
timed-out dial, `ECONNREFUSED` for the rest. A datagram `sendto` passes
`-T_E_INTR` through from its destination write, whose re-point is idempotent on
the retry, and a `CONNECTING` row refuses a `sendto` with an address (`EISCONN`).
Witnesses: the `vivsock.*` kernel tests (`kernel/test/test_viv_sock.c`) drive the
real shells through dev9p and stalk against a loopback `/net` whose responder
records the caller's flag at each 9P op and fails a `data` open with a chosen
errno; `vivarium.socktab_connecting` pins the two row writers.

**An interrupted connect finishes on the socket's next use (2026-09-30).** POSIX
has a connection a signal interrupted "established asynchronously", so a guest
may poll for it and then use the socket without calling `connect()` again --
CPython does exactly that after `EINTR` (PEP 475). Linux's send and recv wait for
the handshake before they move data (`sk_stream_wait_connect`). So every call
that uses a `CONNECTING` socket finishes the connect first:
`viv_sock_finish_connect` runs the dial's wait half (the `data` open, the swap
onto data, `CONNECTED`), interruptible exactly when the calling call is, and
gives the call its flag back afterwards for the call's own wait. The send and
recv shells (`sendto`, `recvfrom`, `recvmsg`) call it after their row check.
`read`, `write`, their vector forms and the positioned pair reach whatever file
the fd names, which for a `CONNECTING` socket is still `ctl`, so
`viv_linux_dispatch` finishes the connect on their fd before it renumbers them
(`viv_sock_finish_before_io`, [[sub-kernel-syscall-dispatch]]); Linux answers
`ESPIPE` for positioned I/O on any socket, which waits on `T_E_SPIPE` being
registered (ERRORS.md ER-3's residual). `getsockopt(SO_ERROR)` must never block,
so on a `CONNECTING` row it reads netd's `status` file first -- through a private
Spoor (`viv_kpath_read`: the resolve core and `spoor_read_on` above, as every
/net file since NP-5b), never a descriptor of the guest's, because a full table
would otherwise read as a handshake in flight and a peer thread could see the
transient fd: `Syn-Sent` or
`Syn-Received`, or a file that cannot be read, is a handshake in flight and
answers 0, as Linux does while a connect is in progress; any other state means
the dial has resolved, the `data` open answers at once, and finishing the connect
turns a failed dial into the `ECONNREFUSED` or `ETIMEDOUT` a poll-then-`SO_ERROR`
caller must see. Before this arc a signal in the handshake laundered into
`ECONNREFUSED`, so the `CONNECTING` state -- and every way of using it but a
`connect()` retry or a poll -- is new with it. Witnesses:
`vivsock.send_recv_finish_connect`, `vivsock.read_write_finish_connect` and
`vivsock.positioned_io_finishes_connect` (through `viv_linux_dispatch` itself),
`vivsock.so_error_reports_the_dial` and `vivsock.so_error_needs_no_descriptor`
(the descriptor table filled to its ceiling). Owned and
open: a second thread connecting the same socket reads `ECONNREFUSED` where Linux
waits; `connect(AF_UNSPEC)` is unserved in every state; and `read` or `write` on a
socket that never connected still reaches `ctl`, where Linux says `ENOTCONN` or
`EPIPE`.

## The listed calls' own waits end for a caught note (2026-10-05)

`note_interruptible` is necessary, not sufficient: the kernel wait a listed call
blocks in must opt in too, and until [[chg-2026-10-05-signal7-list]] only the
two 9P waits did. A Linux `read` or `write` on a pipe or the console, `ppoll`,
`pselect6`, `pause()` (musl's `ppoll(NULL, 0, NULL, NULL)`), `wait4` and a
`FUTEX_WAIT` all rode a caught note out, so a `SIGCHLD` or `SIGALRM` handler ran
only when the call returned on its own, and `pause()` never returned for one.
Each of those waits now opts in ([[sub-kernel-notes]] lists them) and returns
`-EINTR` with nothing consumed; a pipe or console write that moved bytes returns
its count. `viv_wait4` maps `WAIT_PID_NOTEINTR` before its `ECHILD` line
([[sub-kernel-syscall-dispatch]]); the futex's `TORPOR_ERR_EINTR` is already
`-EINTR` numerically. The kernel restarts none of them: a guest that installed
its handler with `SA_RESTART` sees `EINTR` where Linux would restart `wait4`, a
pipe or a futex -- the DEGRADED row of 6.22.

Witnesses: viv-pheno-probe L311-L318, which run under the counting `SIGCHLD`
handler L301-L310 installed (no `SA_RESTART`), block in each listed call -- a
pipe read, a write into a full pipe, `ppoll` with no fds and with one,
`pselect6`, `wait4` by pid, `FUTEX_WAIT` -- while a child exits mid-wait, and
require `EINTR` with the handler run. Every wait is bounded (its own timeout, or
a rescuer child that ends it after 10 s), so a kernel that rides the signal out
fails the leg instead of hanging the probe, and the legs report together
(`L31a`..`L31g`, each with a class letter for what came back). On the pre-change
kernel all seven fail. The same handler now interrupts the probe's own blocking
`wait4` calls when ANOTHER child exits first, which is Linux's behaviour too, so
every one of them goes through `wait4_r`, a bounded retry on `EINTR`; a probe
that called `wait4` once would read a sibling's exit as a failure of the leg.

## The sleep rows (2026-10-05, VIVARIUM 6.29)

`{ VIV_LINUX_NANOSLEEP, VIV_TIER2 }` (101) and `{ VIV_LINUX_CLOCK_NANOSLEEP,
VIV_TIER2 }` (115) land with their shells ([[sub-kernel-syscall-dispatch]]).
Until them both numbers FORWARDed to ENOSYS, and musl's `sleep()`, `usleep()`
and `nanosleep()` -- all three reach 101 on aarch64, and none checks the error
-- returned at once: `busybox sleep 1` did not sleep. Both numbers are below the
native ceiling (101 is `SYS_JIT_CREATE`, 115 `SYS_PCI_IRQ_CREATE`), so
`vivarium.h` carries their per-number collision paragraph.

The pure half. `vivarium_clock_nanosleep_decide(clk, flags, &clk_out, &abs)`
judges the clock in Linux's order: EINVAL for a clock
`vivarium_clock_gettime_map` refuses -- derived from that map, so the two calls
cannot disagree about which clocks exist -- and EOPNOTSUPP for MONOTONIC_RAW and
both COARSE clocks, which Linux reads but keeps no sleep for. Only
`TIMER_ABSTIME` is read from `flags`. The gettime map now reads the clock id's
low 32 bits (`clockid_t` is an int, and Linux reads it so), and the sleep's
decide inherits that. `vivarium_sleep_req_ns` is Linux's `timespec64_valid`
plus a length that saturates rather than wraps short.
`vivarium_sleep_verdict(ts, deadline, now, &rem)` is the deadline rule: the
clock first -- once `now` has reached the deadline the answer is 0, whatever
ended the wait -- then tsleep's outcome: a caught note before the deadline is
EINTR with what was left, and so is a death: its terminate latch can be revoked
before the thread's tail (a peer installs a handler or ignores the note), and a
thread that survives must not read a short sleep as a full one. Anything else
sleeps again. It is pure so
that the race a running kernel cannot be made to hit on demand, a note that
lands after the deadline and before the sleeper runs, is decided where a unit
test reaches it.

`vivarium_clock_sleep(wall, abstime, req_ns, &rem)` is the sleep, on the
calling thread -- the one entry in this file that sleeps. A relative sleep
counts on the monotonic clock whatever its clock: tsleep with a condition that
is never true, toward now plus the request. A deadline already past returns 0
before any wait, even with a note pending -- the opposite of
`sys_poll_sleep_for`, which `ppoll` with no fds reaches and which answers a
pending note with EINTR -- and a deadline of 0, tsleep's no-deadline sentinel,
never reaches tsleep. Every 0 for a deadline is the clock's verdict after the
wait, not tsleep's TIMEDOUT, because tsleep rounds the deadline down to a
counter value. An absolute sleep on `CLOCK_REALTIME` hooks the wall clock's
step list ([[sub-kernel-timer]]) before it reads the offset, sleeps toward the
derived monotonic deadline or a step (the condition is the hook's `ready`),
unhooks, and asks the verdict on the wall clock; a step wakes it to derive
again, so a step past the instant ends the sleep and a step back lengthens it.
Both rows join `vivarium_intr_class`'s ALWAYS arm. signal(7) lists the sleep
interfaces among the calls never restarted, so the missing kernel-side
`SA_RESTART` (the DEGRADED row of 6.22) costs them nothing.

Witnesses: `vivarium.nanosleep_domain` (the clock verdicts against the gettime
map over 21 ids, the narrowing on both maps, the flag, the request's validity
and saturation, the verdict for every outcome), the tier asserts and
`vivarium.intr_class`, `clock.nanosleep_caught_note` (EINTR with the time left;
the native control rides the note out; a zero sleep with a note pending is 0;
an absolute deadline of 0 is not slept on), `clock.nanosleep_wall_step` (a step
past the instant ends the sleep, a step back does not, a relative sleep ignores
both), and viv-pheno-probe L319-L328, which report together: on a kernel
without the rows every leg reads ENOSYS.
