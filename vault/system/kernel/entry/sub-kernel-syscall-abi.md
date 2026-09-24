---
id: sub-kernel-syscall-abi
type: sub
parent: moc-kernel-entry
title: "The syscall ABI — the number space, the argument records, and the three copies of both"
code:
  - kernel/include/thylacine/syscall.h
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libt/include/thyla/poll.h
  - usr/lib/libt/src/start.S
  - usr/lib/libthyla-rs/src/lib.rs
audit: hard
guarded-by: [inv-i5, inv-i13, inv-i32]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: [abi-t-stat, abi-handle-rights, abi-errno]
design:
  - "docs/ARCHITECTURE.md section 13"
created: 2026-08-03
updated: 2026-09-24
---
## Purpose

The contract half of the userspace boundary. [[sub-kernel-exception]] defines
*where* a crossing happens; this defines *what* crosses — the syscall numbers,
the register convention, the argument records copied across, and the bounds
every one of them is checked against.

It is three files saying the same thing in three languages, and nothing in the
build checks that they agree.

## Contract

**`T_CAP_TCB_DIAL` (U, 2026-09-23).** Bit 14 joins the `T_CAP_*` mirror set in
both userspace copies -- `usr/lib/libthyla-rs/src/lib.rs` and
`usr/lib/libt/include/thyla/syscall.h` -- alongside the kernel's `CAP_TCB_DIAL`
in `kernel/include/thylacine/caps.h`. It is a capability constant, not a new
syscall or argument record: the syscall number space and every argument shape
are unchanged, so this is an additive mirror update, not an ABI break. The
authoritative partition (fork-grantable vs elevation-only) lives in [[abi-caps]];
the gate it feeds is in [[sub-kernel-devsrv]].

**`T_SPAWN_PERM_SEAL` ((U) F1, 2026-09-23; renamed from `T_SPAWN_PERM_NOTRACE` and widened to stamp NODUMP as well by F5, 2026-09-24, before the bit was ever pushed; its NODUMP half acquired REAL EFFECT 2026-09-24).**

**What the bit now means for a caller, which is an ABI-doc change and not only an
implementation one.** `NODUMP` stopped being forward-compat scaffolding: while set, it
refuses every `/proc/<pid>` file that hands out something the Proc holds -- environ, maps,
ns, cwd, exe, cmdline, and reads of mem/regs/fpregs/kregs -- to every OTHER Proc,
`CAP_HOSTOWNER` included (DEBUG-FS-DESIGN 3.2; widened to the whole image by the seal's
round-2 re-audit). `status`, `sched` and `imperium` stay readable -- the kernel's record,
not image content. It does NOT refuse control, so it does not stand against a peer that
may still drive the Proc; guarding a secret takes `SYS_SET_TRACEABLE(0)` too. Both
setters and the `SPAWN_PERM_SEAL` stamp write through `proc_seal`, under
`g_proc_table_lock`. So `SYS_SET_DUMPABLE(0)`, which is UNGATED and one-way, is now a
caller choosing permanent opacity of its image to the whole machine rather than only arming a future dump refusal, and TWO callers
(`usr/corvus/src/main.rs`, `usr/login/src/main.rs`) were already invoking it as hygiene
on the strength of the older wording. All four copies of that wording -- this header,
`proc.h`, libt and libthyla-rs -- were corrected in the same commit; the libthyla-rs
`t_set_dumpable` doc was the sharpest, having called a live irreversible switch a no-op.
Note also that `syscall.h`'s reason SEAL is the one UNGATED perm ("strictly REDUCES what
may be done to the child") never weighed that it now also reduces what a THIRD PARTY may
learn; that argument still holds for the child but is no longer the whole story.
 Bit 9 joins the
`T_SPAWN_PERM_*` mirror set in the same two userspace copies, alongside the
kernel's `SPAWN_PERM_SEAL` in `kernel/include/thylacine/syscall.h`, and is
added to `SPAWN_PERM_ALL` -- which is the part that matters for the mirrors,
because a bit outside that mask is rejected outright at the entry gate, so a
kernel that does not know the bit refuses the spawn rather than ignoring it.
Additive: no syscall number moves and no argument record changes shape, and a
parent that never sets the bit is unaffected. The `perm_flags` field is `u32` on
the wire (`TSpawnArgs`) while the Rust mirror types the constant as `u64` and
narrows at the call (`self.perm_flags as u32`), so the mirror has 32 bits of
headroom the ABI does not, so a future bit above 31 is a real hazard -- now
refused rather than truncated (see the next paragraph).
The gate's placement and the reason this one bit is ungated are in
[[sub-kernel-syscall-dispatch]]; what it protects is in [[sub-stratum-session]].

**The narrowing is now CHECKED, not silent (round 2).** `Command::spawn` refuses
`perm_flags > u32::MAX` instead of truncating. The old `as u32` failed OPEN in the
caller's eyes: a future bit >= 32 would be dropped and the spawn would SUCCEED
without it, so login would believe it had sealed the proxy when it had not. The
kernel cannot catch that on this path either -- its `& ~SPAWN_PERM_ALL` rejection
only ever sees the low 32 bits (the legacy `SYS_SPAWN_WITH_PERMS` handler does check
the full u64, so the two entry points differed). joey's C-side `(unsigned int)` cast
has the same shape and is the remaining instance.

**The bit's own comment was overstated and is now accurate.** It claimed the stamp
makes the debug surface refuse an attach "for the whole of its life". It does not:
`rfork` publishes the child before the thunk runs, so a window exists in which
`proc_flags` is still 0. What closes that window for the case that matters is the
identity/seam ordering described in [[sub-kernel-devproc]], not the stamp's timing
alone. All three copies (kernel header, libt, libthyla-rs) say so now.

**The bit's WHY was stale in the same block, and the audit caught it (F2,
2026-09-24).** It still explained the seal as the answer to a debug surface that
"separates on identity", which stopped being true the moment the owner axis
gained capability cover -- and the commit that added cover said so in its body
while touching neither this header nor `caps.h`. Both now state that the seal is
the SECOND of two independent answers: cover refuses the shell's attach on
authority (it lacks `CAP_TCB_DIAL`), and the seal is kept because it is the one
that still holds between peers of EQUAL authority -- a caps-0 native fork of the
proxy is covered by every same-principal peer while still holding the parent's
handles. Lesson for this dossier's own class of prose: a comment that explains
WHY a bit exists is invalidated by a change to the mechanism it names, and
nothing in the build fails when it rots.


**Imperium integration (2026-09-17).** Reserved numbers 110 and 111 are
now implemented as SYS_CONSOLE_EPISODE and SYS_CAP_GRANT_IMPERIUM. Main's
SYS_DMA_SEGMENTS stays 112; later PCI appends now put SYS__NATIVE_TOP at 124. No existing syscall
was renumbered. Native C and Rust mirrors include the new operations and
CAP_POST_SERVICE at bit 13. The console operation accepts ARM=1 or END=2;
unknown operations fail closed.

**B-1a append (2026-09-23).** `SYS_BURROW_RESERVE` = 124 and
`SYS_BURROW_PROTECT` = 125; `SYS__NATIVE_TOP` is 126 and `VIV_NATIVE_CEILING`
125. Re-measured on this tree, not incremented: **123** live numbers, the span
runs to 125 with the same three holes (26, 30, 43), `syscall_dispatch` has
exactly 123 arms, and both set differences are empty. The section at the end
of this dossier carries the two records.


`x8` carries the syscall number, `x0..x5` the arguments, `x0` the result —
deliberately Linux's AArch64 convention, so a ported libc's syscall stub needs
renumbering and nothing else. Userspace issues `svc #0`; the immediate is
ignored at v1.0 and reserved as a future class selector.

Numbers are **append-only and never reused** — with one bounded exception, a
pre-merge collision between two branches, argued under Mechanism. Three of the
110 slots below the span are holes: `SYS_POST_SERVICE` (26), `SYS_SRV_CONNECT`
(30), and `SYS_POST_SERVICE_BYTE` (43), all retired when `/srv` moved from
dedicated syscalls to ordinary namespace operations (stalk-3c). The
device-class-query slot that the last sweep recorded as reserved-but-unbuilt is
now filled: `SYS_FD_DEVCLASS` (80, H-1a) returns a Dev's class character, so
is-a-terminal is `dc == 'c'` (the console) OR `dc == 't'` (a pts slave, H-4d) --
`libthyla-rs::stdout_is_terminal()` is the one wrapper that folds both, since a
program in a session tile is on a terminal exactly as one on `/dev/cons` is. Each
hole carries a comment naming what used to be there. An unknown number returns `-1` rather than terminating the
caller.

Return values follow two conventions and the split is per-syscall, not
per-family: older calls return a bare `-1` for every failure; newer ones return
a negative errno. The errno registry deliberately keeps `T_E_PERM` at 1 so that
`-T_E_PERM` and the bare sentinel are the same value — which means an errno-
returning syscall can never distinguish "permission denied" from "generic
failure", and the header says so, steering POSIX-EPERM contours to
`T_E_ACCES` instead.

## Mechanism

### The number space is coherent, and that is verifiable

**123** numbers are live (re-measured 2026-09-23; this section was written at
107 and the appends since are the dated sections at the end);
`syscall_dispatch`'s switch has exactly **123** arms; the two sets are equal
with **both** differences empty. Every mirrored number
agrees across all three copies — there is no case where a name means one number
to the kernel and another to a library. (Re-measured this sweep against the enum
and the dispatch body: each set difference is empty, and no name overlapping a
mirror and the kernel disagrees on its value.)

Compare the *sets*, not the counts. Two lists of equal length can disagree
about their members, so a count match passes vacuously; the empty
difference in each direction is the claim worth making.

That is worth stating precisely because it is *not* guaranteed by anything. It
is the current state, maintained by hand.

The allocated span runs to **125** with **three holes** — 26, 30 and 43, all
`/srv` retirements (it ran to 109 when this section was written; 110..125 are
the Imperium, DMA-segments, PCI, trusted-seat, nonblock and B-1a appends
below).

Four numbers moved the census since the 103-live sweep, and this refresh folded
them in by **re-measuring**, not incrementing — the same "measure, don't guess"
discipline that caught the Rust `T_SYS_` bounds-constant trap below. Three were
appends past the top: **107** (`SYS_BURROW_FROM_HOSTMEM`, V-2) and **108**
(`SYS_HOSTMEM_REFCOUNT`, V-3b-1c-2b; a read-only VA-keyed query returning
[[sub-kernel-burrow]]'s `burrow_total_refs`) from the Warp host-visible-ring arc,
then **109** (`SYS_OPEN_CREATE`, the #50 path-mutation family). The fourth did
not extend the span at all: `SYS_FD_DEVCLASS` (H-1a) took the reserved slot at
**80**, so what the last sweep recorded as four holes is now three. None of the
three appends has a C consumer; the Rust mirror carries all three, the C mirror
none — the subset rule below, visibly holding rather than asserted.

### The libraries are subsets, not copies

Neither mirror carries the whole set, and neither is expected to: the C library
exposes **77** numbers, the Rust library **100**, and each omits what its
consumers do not call. So the invariant that matters is not "the mirrors are
complete" but "where they overlap, they agree" — which holds (re-verified this
sweep on both intersections: no name means one number to a library and another
to the kernel).

**Counting the Rust mirror has a trap in it, and it caught this sweep.** The
Rust side spells *bounds constants* with the same `T_SYS_` prefix as syscall
numbers — the argv count and data caps sit in the same namespace as the calls —
so a census keyed on the prefix returns **102** and two of those —
`T_SYS_SPAWN_ARGV_MAX` and `T_SYS_SPAWN_ARGV_DATA_MAX` — are not syscalls.
The kernel keeps the two categories apart by **form** (an enum for numbers, a
`#define` for bounds); the Rust mirror flattens both into `pub const`, and the
prefix no longer discriminates. Intersect against the kernel's enum rather than
trusting the prefix.

### Nothing pins a mirror to the kernel

The kernel header carries **111** compile-time assertions; the C mirror **50**;
the Rust mirror **43**. Every one of them constrains **its own file**. Across
the mirrors the word "mirror" appears on **84** lines, **23** of them as some
casing of "must mirror". Neither phrase is a mechanism.

(State the method with the figure: those counts are case-insensitive line
counts across the two mirrors plus the poll header. A case-*sensitive* count of
`MUST mirror` gives 11, which read against a case-insensitive predecessor looks
like the phrase halving. It did not — nothing in this census shrank.)

### And the hazard is not only drift; it is concurrent allocation

The sharpest demonstration is a collision that did happen. Two branches
independently allocated the same two numbers — one for the process-creation
pair, one for the JIT pair — both live, both with real consumers.

**Duplicate enum values are legal C.** The merge would have compiled *silently*
and stayed silent until two dispatch cases collided; and fixing the collision is
not fixing the bug, because every mirror carries its own copy of the number.

Which side moved was decided on measured edit cost, not seniority: one side
embedded the literal once, in-tree; the other embedded it three times inside
**patch files** against an out-of-tree dependency rebuilt remotely, each beside
a comment naming the syscall it would no longer be. Editing a patch file is the
riskiest edit in this tree — `patch` trusts the hunk header and silently drops
added lines past it.

**It took five sites, and the fifth is the one that generalizes.** Four were
findable: the kernel enum, the two Rust constants, and a naked `mov x8, #N` in
assembly. The fifth was a constant defined as *"the highest assigned native
syscall number"* — and **it contains no syscall number to grep for.** It is a
*semantic* mirror, invalidated by a renumber of the top because the renumber
moves what it is defined against. Both agents' censuses missed it, and neither
was looking for that kind of thing.

The consequence would have been silent and security-shaped: the phenotype
collision argument is keyed to that ceiling, so a stale ceiling voids the
argument for every row at or below the new value, with nothing failing.

**What caught it was a `_Static_assert` written at the point of the hazard**,
whose message says what to do and why — left by someone who had already lived
the same failure, since the header records that this constant "was previously
written out as a literal in four places and was stale in all four." The
re-check the message demands is itself mechanized: the ceiling-dependent rows
each assert individually, so the compiler adjudicates a bump rather than a hand
scan. This is the one place on this surface where the enforcement is a
mechanism rather than an instruction to a person.

**And the append-only rule survives the apparent violation.** A renumber is
exactly what that rule forbids — but append-only is a property of the
**shipped** number space, and two unmerged branches do not have one shipped
space between them. The rule binds allocation *from* a released ABI; it cannot
adjudicate two branches that allocated concurrently from the same free list.
Nothing prevents the recurrence except that the free list is now shorter.

There is no generator, no shared header, and no build step that reads one file
and checks the other — `tools/build.sh` never mentions either mirror. The
enforcement is that a human wrote MUST in a comment.

The clearest statement of this is the poll ABI's slim header, which is worth
quoting because it is entirely correct and draws no conclusion:

> MUST mirror the kernel side; the kernel's `_Static_assert`s pin the layout
> **there**. Drift here would surface as a SYS_POLL ABI mismatch at runtime —
> keep them in lockstep.

It names the asymmetry ("there"), names the consequence (runtime, not build),
and names the remedy as an instruction to a person. The file contains no
assertion of its own. The Rust mirror's `pollfd` says the same thing in the same
shape — the kernel's asserts "pin the kernel side" — and also carries none. Both
happen to be correct today because both spell the fields with types that give
the same C layout.

### Growth is by appended field into a reserved slot, and it has worked

The spawn argument record has grown four times — an identity block, a hardware
allowance block, a page budget, a phenotype-flags word — from 56 bytes to 104,
and every existing caller kept working, because each growth either appended past
the end or claimed a field that was already reserved and required to be zero. The
page budget is the best case: it took over the tail padding slot (`_pad_allow`,
offset 92), so the struct did **not** grow, and every caller that zero-fills the
struct gets the historical behaviour by construction rather than by a
compatibility branch. Twenty offset assertions pin the result, and the one on the
reused slot spends its message explaining that reuse rather than restating the
offset.

**The fourth growth is where the two hazards on this surface meet.** The
phenotype-flags word (VIVARIUM V-1b) was authored at offset 92 — the *same*
`_pad_allow` slot the page budget had already claimed — on a different branch.
That is the concurrent-allocation collision from the number space replayed one
level down, at a struct offset: two branches drawing the last reserved field from
the same free slot, both compiling, the merge the place it surfaces. It was
resolved the same way the number collision was — the aux-2 merge moved the
phenotype word to 96 (growing the struct to 104) and opened a fresh forward-compat
pad at 100 — and the offset assertion on it records that history verbatim, so a
reader is not surprised by a struct that is 104 rather than 96. A `_Static_assert`
at the point of the hazard, again, is the whole mechanism.

`t_stat` is the same story in the other growth mode. It has grown twice — uid+gid
(A-2a) took it from 72 to 80, then a per-instance device number plus pad (#100)
from 80 to 88 — both **appended past the end**, because a stat result is written
into the caller's buffer and there was no reserved slot to reuse. Its size
assertion is unusually loud about the consequence: the kernel writes `sizeof(88)`
bytes, so a mirror left at 80 *overflows the caller's buffer*, and the message
names all four copies that must grow in lockstep — libt, libthyla-rs, the pouch
stat patch, and the go-thylacine `Stat_t`. Four mirrors, not two: the drift hazard
is wider here than anywhere else on the surface, and nothing but that comment binds
them.

### The all-or-nothing rule, stated twice and broken once

Oversize arguments get one of two dispositions, and the file is explicit about
which is right for which kind of call.

**Clamp** is correct for byte I/O: read, write, pread and pwrite clamp to the
128 KiB transfer maximum and return the count, because a short transfer is what
POSIX promises and the caller loops.

**Refuse** is correct where a short result is meaningless or dangerous, and the
header argues this twice, in two separate chunks, about two different fields.
The secret-scrub primitive rejects an oversize length with a comment that says
"do NOT silently cap — for a secret-scrub primitive, capping and returning
success would silently retain the tail of the buffer", and names its precedent
set: the console write and the directory read, which reject the same way. The
page budget refuses an over-cap request "never clamped — a silent clamp would
hand back a budget the caller did not ask for and hide the misconfiguration".

The CSPRNG read clamps. See Caveats.

## Data structures

Thirteen argument and result records cross the boundary, each pinned by size and
per-field offset assertions on the kernel side: the spawn arguments (twenty
offset assertions plus the size assertion, the most-grown record at 104 bytes),
the stat result (88 bytes after two growths), the hardware allowance descriptor,
the PCI info block and its two sub-records, the debug register frames, the peer
identity record, a timespec, and a JIT region descriptor.

One is pinned only transitively. The hardware window — a base/size pair — has no
assertion naming it, but the descriptor that contains an array of eight of them
asserts that the following field sits at offset 128, which forces each window to
be exactly 16 bytes. That is real pinning, just indirect.

The bounds constants are the other half of the ABI: a 128 KiB transfer maximum,
a 4 KiB stack-scratch bound that three calls are deliberately held at, a 1 KiB
path maximum, a 256-byte binary name, 512 argv entries in 64 KiB of data, 16
inheritable descriptors.

## Concurrency

None owned. These are declarations; the file defines no state and takes no
locks. Concurrency lives in the handlers.

The one ABI-level concurrency statement is negative and worth keeping: the
argument records are copied out of user memory before validation, so a
concurrent peer thread scribbling the same buffer cannot make a checked field
change afterwards. That property belongs to the handlers, but it is what makes
these records safe to describe as records rather than as pointers.

## Invariants enforced

None directly — a header enforces nothing. It *declares* the shapes through
which four invariants are enforced elsewhere:

**I-2** (capability monotonic reduction — still unminted as a note, cited bare
here as [[sub-kernel-caps]] cites it) — the spawn record's capability mask is
documented as advisory: the kernel ANDs it with the parent's own set, so a mask
requesting more than the parent holds is clamped rather than refused, and
monotonic reduction holds structurally rather than by a check. Note that this is
a *deliberate* clamp, in a file that argues twice against silent clamping: it is
sound because the clamp is toward less authority, which is the direction the
invariant wants, and because the caller cannot have been relying on the excess.

**[[inv-i5]]** — the descriptor-inheritance list is documented as accepting only
file handles, precisely because hardware handles must not cross a process
boundary.

**[[inv-i13]]** — every user pointer in every record is a value to be validated,
never dereferenced from the declaration.

**[[inv-i32]]** — the page budget field is the per-process page cap's spawn-time
entry point.

## Error paths

Two conventions, per-syscall. The bare `-1` set predates the errno set and has
not been converted; the header documents which each call uses, individually, in
its enum comment. A caller cannot tell from the number which convention applies.

The unknown-number arm returns `-1` and lets userspace decide — the header notes
that a signal-equivalent note is the eventual behaviour and was not built.

## Performance

Not a runtime surface. The one performance-relevant ABI decision is the 128 KiB
transfer maximum, raised from 4 KiB when the measured read ceiling turned out to
be one 4 KiB staging buffer per round trip.

## Prosecution

- **A new syscall appends. It never fills a hole.** The three retired numbers
  and the one reserved number stay unallocated; reuse would silently redirect a
  stale binary's call.
- **A new argument record field appends, or claims a reserved slot that is
  already required to be zero.** Both are proven patterns here; anything else
  breaks a caller that zero-fills.
- **Every appended field gets an offset assertion, not just a size assertion.**
  A size assertion alone passes on a field reorder.
- **A mirror change must be made in all three files in the same commit**, because
  nothing else will catch it. The mirrors are subsets, so "not present" is
  legitimate and indistinguishable from "forgotten".
- **Enumerate mirrors by what they MEAN, not by what they CONTAIN.** A census
  that greps for the value cannot find a constant that holds the value only by
  *definition* — "the highest assigned number", "one past the last", "the same
  as X". Those are mirrors and they carry no token to search for. The five-site
  renumber found four by grep and the fifth by an assert.
- **A number allocated on an unmerged branch is not allocated.** Duplicate enum
  values compile, so two branches drawing from the same free list collide
  silently and the merge is where it surfaces — after both sides have
  consumers. Check the far branch's tip before taking the next number.
- **Count against the kernel enum, not against the `T_SYS_` prefix.** The Rust
  mirror puts bounds constants in that namespace too.
- **A new oversize bound must choose clamp or refuse deliberately**, and the
  choice is semantic: clamp iff a short result is a complete answer the caller
  can loop on. Refuse otherwise.
- **A bound documented in a mirror must be cited by the right constant name.**
  See Caveats — one is not, and correcting the mirror by looking up the name it
  cites would change the value by 32x.

## Seams

- **The two return conventions are not converging.** New syscalls use errno, old
  ones use `-1`, and there is no migration underway. A caller handling both must
  know which is which per call.
- **The `svc` immediate is unused.** Reserved as a class selector; nothing reads
  it.
- **Environment pass-through at spawn was reserved and never built.** The
  reserved slot is still there, still required to be zero — the environment
  arrived instead as a per-process filesystem, so the slot now guards a design
  that was superseded rather than deferred.
- **The blocking mode of the CSPRNG read does not exist.** See Caveats.

## Caveats

- **The CSPRNG read is the one call that silently caps, and four documents say
  otherwise.** An oversize request is clamped to the 4 KiB scratch bound and
  returns 4096; it is not refused. The header states the bound as a precondition
  without saying what violating it does. The C mirror lists the failure causes
  and omits oversize — accidentally the most accurate of the four. The Rust raw
  wrapper states outright that the kernel returns -1 on an oversized length, and
  the safe wrapper repeats that claim in a comment while separately enforcing the
  bound itself before the call. So the guarantee the documentation describes is
  real — it is just implemented in the library, one layer above the kernel that
  is credited with it.

  What makes this more than a documentation slip is the neighbourhood. The
  secret-scrub primitive twenty-eight lines earlier refuses an oversize length
  and carries a comment explaining that capping a security primitive and
  returning success is the wrong disposition — a comment that cites *the Rust
  wrapper's documented behaviour* as part of its justification, and enumerates
  the sibling calls that reject the same way. The CSPRNG read is not in that
  enumeration, and it is the file's other secret-handling call. The page-budget
  field, added by a much later chunk, argues the same principle again from
  scratch. The rule is stated twice in this one file and the exception is the
  entropy source.

  Nothing is broken today. The only native consumer that calls the raw wrapper
  is the TLS entropy backend, and it checks that the returned count equals the
  requested length — a check that is redundant *against the documentation* and
  load-bearing *against the kernel*. It is the only thing between a large request
  and a partially-randomized buffer, and to a reader who trusts the wrapper's
  documentation it looks like dead code.

- **A constant is correct and its stated source is wrong, in the direction that
  punishes checking.** The Rust CSPRNG module's per-call limit is 4096, which is
  right, and is documented twice as mirroring the transfer maximum, which is
  128 KiB. The bound it actually mirrors is the stack-scratch constant. Anyone
  verifying the mirror against the constant it names would raise it to 131072 and
  land squarely in the silent-cap path above.

- **The blocking mode of the CSPRNG read is documented and unimplemented.** The
  header describes a flags argument where zero blocks until the generator is
  seeded and one returns immediately. The handler range-checks the argument and
  never reads it; an unseeded generator always fails. The handler's own comment
  is honest about this; the header, which is what a userspace author reads, is
  not. The non-blocking constant is defined in two headers, described three
  incompatible ways across four files, and read by nothing.

- **Three enum comments state a name bound of 64 bytes; it has been 256 since
  the on-device toolchain needed absolute paths.** Both mirrors carry 256
  correctly. The stale copies are in the kernel header's own prose, describing
  the calls the bound applies to.

## Provenance

[[chg-2026-08-03-syscall-abi-sweep]], [[chg-2026-09-05-syscall-abi-census]].

2026-09-16: `SYS_BURROW_DETACH`'s documented `-1` list (syscall.h and the
libthyla-rs `t_burrow_detach` mirror) now states the window refusal it always
had, and the identity arm ARCH 6.5 added: a DMA- or MMIO-backed mapping is
detachable wherever the driver placed it. No number, argument or record changed;
the result for that class of call went from `-1` to `0`.

## Protected PCI windows (2026-09-17)

[[abi-pci-windows]] appends `PCI_MAP_WINDOW` 113 and `PCI_WINDOWS` 114 in kernel,
C and Rust. The native ceiling is 114. Linux clock_gettime 113 is handled by the
existing phenotype Tier-2 translator before native dispatch; its former ceiling
assertion is replaced by an explicit collision pin. The old PCI_INFO record and
four-argument whole-BAR map remain unchanged; protected whole-BAR maps now fail.

## PCI endpoint append (2026-09-17)

[[abi-pci-irq]] appends calls 115..120; native ceiling is 123 after the trusted-seat/nonblocking append, with SYS__NATIVE_TOP
124. Kernel/C/Rust records pin event 32 bytes and info 48 bytes. Clock_gettime's
113 collision remains consumed by its Tier-2 phenotype row. Remaining ceiling
assertions compile. MSI-X mode currently fails ENODEV; backend implementation
must precede any support claim.

## Trusted-seat and nonblocking append

[[abi-trusted-seat]] pins native 121/122 and [[abi-native-nonblock]] pins 123.
The ceiling is 123, SYS__NATIVE_TOP is 124. Existing call numbers and Linux
phenotype translation retain their meanings. The 2026-09-21 review changed no
number and no envelope: `seat.h` gained names for the six chord key codes, and
RESTORED is now also accepted in the failed phase ([[abi-trusted-seat]]).

## Behaviour changes on existing numbers (B-0, 2026-09-21)

No number, record or errno changed; three existing calls changed what they
DO, and `syscall.h`'s comments say so. `SYS_CHROOT` and `SYS_PIVOT_ROOT` now
REMOVE every mount entry whose point the new root cannot reach, under the same
lock hold (the #80 mount-table shed, ARCH 9.6.10, [[sub-kernel-territory]]):
nothing resolved from the new root changes, an fd-relative walk from a
directory fd opened before the swap can. Both refuse a non-directory with the
flat -1 -- `SYS_CHROOT` newly (a bad pivot used to wedge resolution until the
caller pivoted back; with the shed it would strip the table for good) -- and
`SYS_PIVOT_ROOT` also refuses a caller with no current root, which is
`SYS_CHROOT`'s to install. The `SYS_PWRITE` O_APPEND note changed with pouch
0040: ports pass the kernel's `OAPPEND` omode bit instead of emulating append
with one seek at open.

## B-1a: SYS_BURROW_RESERVE 124 and SYS_BURROW_PROTECT 125 (2026-09-23)

Two new numbers rather than flags on `SYS_BURROW_ATTACH_LAZY`, per the
2026-06-23 blast-radius precedent; both answer `-T_E_*`, never a bare -1. The
ceiling is 125 and `SYS__NATIVE_TOP` 126 (`vivarium.c`'s static assert pins
`VIV_NATIVE_CEILING == SYS__NATIVE_TOP - 1`). Linux `mprotect` is 226, above
the ceiling, so its collision argument is discharged by construction.

`SYS_BURROW_RESERVE(length x0, prot x1, align_log2 x2) -> vaddr / -errno`: a
demand-zero anonymous reservation in the burrow-attach window, minted at
`prot` in {none, R, RW} under a ceiling of RW, its base aligned to
`2^align_log2` (0 = page; else 12..30, `BURROW_RESERVE_ALIGN_MIN/MAX_LOG2`).
Same page rounding and I-32 posture as `SYS_BURROW_ATTACH_LAZY` (pages charged
at fault, the VMA count at reserve; `BURROW_RESERVE_MAX` = 1 GiB). Refused: a
prot with X (`-EACCES`, first), W-without-R / other bits / an alignment out of
range / length 0 (`-EINVAL`), over the max / no aligned gap / OOM / the VMA cap
(`-ENOMEM`).

`SYS_BURROW_PROTECT(vaddr x0, length x1, prot x2, flags x3) -> 0 / -errno`:
move every page of the range to `prot` in {none, R, RW} under each mapping's
ceiling; `BURROW_PROTECT_SEAL` (the only flag) also lowers the ceiling to
`prot`, irrevocably. No window confinement and no capability: what may be
protected is decided by the mapping, not by its address or the caller's caps
(attenuation creates no authority; `CAP_JIT` gates the creation of code).
All-or-nothing across several mappings. Refused: X (`-EACCES`, before any
lookup -- so `protect(X)` over an unmapped range is EACCES where `protect(R)`
is ENOMEM, which is how the boundary order is observable), other bits /
W-without-R / unknown flags / an unaligned `vaddr` / length 0 (`-EINVAL`), a
hole or a guard in the range or no room for the split (`-ENOMEM`), a shared-in
/ CODE / hardware mapping or a prot above the ceiling (`-EACCES`).

The prot word is `BURROW_PROT_NONE/READ/WRITE/EXEC` = 0/1/2/4 -- the kernel's
own `VMA_PROT_*` and, by construction, Linux's `PROT_*`; `kernel/syscall.c`
pins all three equal with a `_Static_assert`, which is what lets the phenotype
`mprotect` row pass its word straight through. `EXEC` is named only so its
refusal can be spelled.

Mirrors: the Rust mirror carries both numbers and the constants
(`T_SYS_BURROW_RESERVE` / `T_SYS_BURROW_PROTECT`,
`T_BURROW_PROT_NONE/READ/WRITE`, `T_BURROW_PROTECT_SEAL`; `t_burrow_reserve` /
`t_burrow_protect` in `usr/lib/libthyla-rs/src/lib.rs`), deliberately WITHOUT a
`T_BURROW_PROT_EXEC` -- a constant nobody may pass is not worth mirroring, and
the probe that watches X refused spells it as a literal. The C mirror carries
neither (no C consumer; the subset rule above, still holding visibly).
Consumers: `/protect-probe`, `/protect-guard-child`
([[sub-kernel-protect-witness]]) and the phenotype `mmap` / `mprotect` rows,
which are the first production callers.

## The identity cape: an x5 flags word and one perm bit ((L), 2026-09-23)

No number changed and no record grew; the operator voted the additive shape
(IDENTITY-DESIGN 3.2, HAUL-DESIGN 4.7). The native ceiling stays 125.

- `SYS_ATTACH_9P` (13) reads **x5 flags**, under the #112 discipline: every
  caller passes it, and the wrappers take it explicitly (libt
  `t_attach_9p(tx, rx, aname, len, n_uname, flags)`, libthyla-rs the same
  with `in("x5")`), so a stale caller cannot leave a register's garbage in
  it silently. The one bit is `SYS_ATTACH_9P_CAPE` (0x2, mirrored as
  `T_ATTACH_9P_CAPE` in both libraries): the session reports the attaching
  principal as every file's owner and its primary gid as the group, keeps
  the server's mode, and sends nothing identity-bearing. `SYS_ATTACH_9P_LOOSE`
  (0x1) stays `SYS_ATTACH_9P_SRV`-only and is refused here with the flat -1,
  like any unknown bit. Every in-tree caller was checked, and no sibling
  tree (the Go port, the libc port, pouch's patches) issues the call raw.
- `SYS_ATTACH_9P_SRV` (52) refuses `SYS_ATTACH_9P_CAPE` in its x4 like any
  unknown bit and admits LOOSE alone: over `/srv` the cape is the poster's
  decision, and only a conn from a DMSRVCAPE service is caped. (B,
  2026-09-24: an operator decision taken before the flag was ever pushed. It
  had been admitted beside LOOSE for a day, and no caller passed it.)
- `SYS_WALK_CREATE_DMSRVCAPE` (0x00800000, bit 23; libthyla-rs
  `T_WALK_CREATE_DMSRVCAPE`) marks a `/srv` service post caped, and is
  admitted ONLY beside `DMSRVBYTE`. `SYS_WALK_CREATE_DMSRV_BITS` (BYTE | BULK
  | CAPE) is the derived mask all three refusals share -- the post branch's
  "only DMSRV bits", and the fd-based and path-based creates' "no DMSRV bit
  on a regular create" (-EINVAL) -- and `SYS_WALK_CREATE_PERM_VALID` derives
  from it, so a fourth bit cannot reach one site and miss another. A static
  assert pins that bit 23 collides with no other perm bit.
- The rules live in two tested predicates beside
  `sys_attach_9p_ends_are_pipes`: `sys_attach_9p_flags_ok(flags, srv)` and
  `sys_srv_post_perm_ok(perm)` (`srv_client.cape_admission` drives both bit
  by bit, including a bit above 32 that a truncation would lose).
- Two handler inners joined `sys_open_create_kpath_for_proc` as
  test-callable entries, the handler thinning to its user-copy:
  `sys_walk_create_kname_for_proc` and `sys_attach_9p_srv_for_proc`. Their
  checks repeat the handlers', so the syscall's answers and precedence are
  unchanged ([[sub-kernel-syscall-dispatch]]).
