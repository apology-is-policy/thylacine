---
id: chg-2026-08-03-syscall-abi-sweep
type: chg
title: "the syscall ABI — the rule is stated twice in one file and the exception is the entropy source"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-syscall-abi
  - sub-stratum-boot
  - moc-kernel-entry
established:
  - sub-kernel-syscall-abi
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 35, the seventh sweep off the census: the syscall ABI surface -- the
kernel's declaration header (2732 lines), the C mirror and its two companions
(2157 + 28 + 68), and the Rust mirror (2991). Main had moved to `10b1bbb2`;
merged before starting. L-1 absent on the TWENTY-THIRD check.

One dossier, [[sub-kernel-syscall-abi]], filed under [[moc-kernel-entry]], and
the filing is a claim: a privilege boundary has two independent descriptions.
The entry area already held the *mechanism* -- where a crossing may happen. This
is the *contract* -- what crosses. They fail differently, which is the argument
for keeping them separate rather than folding one into the other: a mechanism
defect is a fault on one machine, an ABI defect is two programs each behaving
correctly by its own copy of the rules.

**WHAT IS SOUND, MEASURED RATHER THAN ASSUMED.** 100 syscall numbers are live;
`syscall_dispatch` has exactly 100 arms; the sets are equal both ways. Every
number that appears in more than one of the three files agrees in all of them --
there is no name that means one number to the kernel and another to a library.
The four holes in the space (three retired when `/srv` moved to ordinary
namespace operations, one reserved) each carry a comment saying what was there.
The spawn argument record has grown three times, 56 to 96 bytes, without
breaking a caller -- twice by appending, and once by claiming a reserved slot
that was already required to be zero, so the struct did not grow at all and
every zero-filling caller kept the old behaviour by construction.

None of that is enforced by anything. The mirrors are subsets by design (74 and
92 of 100), they are pinned only to themselves (100 assertions in the kernel, 46
and 33 in the mirrors, every one constraining its own file), and `tools/build.sh`
does not mention either mirror. The word "mirror" appears on 73 lines across the
two, 22 of them "MUST mirror". That is the mechanism.

**F1 -- THE FILE STATES THE RULE TWICE AND THE EXCEPTION IS THE CSPRNG.**

Oversize arguments get one of two dispositions. Byte I/O clamps to the transfer
maximum and returns the count, which is correct: a short read is a complete
answer POSIX promises and the caller loops. Everything all-or-nothing refuses.
And the header does not leave that implicit -- it argues it, twice, in two
chunks separated by many months:

> RW-3 R2-F1: reject len > SYS_RW_STACK -- **do NOT silently cap**. For a
> secret-scrub primitive, capping + returning success would silently retain the
> tail of the buffer; the libthyla-rs wrapper documents -1 on oversize, and
> SYS_PUTS/SYS_READDIR reject oversize the same way.

> Anything over PROC_PAGE_HARD_MAX is REFUSED outright (**never clamped** -- a
> silent clamp would hand back a budget the caller did not ask for and hide the
> misconfiguration).

Twenty-eight lines after the first of those, `sys_getrandom_handler` does
`if (len > SYS_RW_STACK) len = SYS_RW_STACK;`. The transposition is exact: the
scrub's failure is that you think you wiped a secret and the tail survived; the
CSPRNG's is that you think you filled a buffer with entropy and the tail is
whatever was there before. It is the file's *other* secret-handling primitive,
and it is not in the enumeration of siblings that reject.

**And four documents describe the behaviour the comment assumes rather than the
one the kernel has.** The kernel header states the bound as a precondition
without saying what violating it does. The C wrapper lists the -1 causes and
omits oversize -- accidentally the most accurate. The Rust raw wrapper says
outright "-1 on ... oversized len". The Rust safe wrapper repeats that claim in
a comment *and separately enforces the bound itself* before calling. So the
guarantee is real; it is implemented one layer above the kernel that is credited
with it. Note the first comment above cites the Rust wrapper's documented
behaviour as part of its own justification -- the reasoning leaned on a claim
that is false for the call it is being applied to. Task #84.

Nothing is broken. The only native caller of the raw wrapper is the TLS entropy
backend, and it checks `n as usize != buf.len()` as well as `n < 0`. That check
is **redundant against the documentation and load-bearing against the kernel**,
and it is the only thing between a large request and a partially-randomized
buffer feeding rustls. To a reader who trusts the wrapper's doc it looks like
dead code -- so the documentation actively invites deleting the defence that
makes it true.

**F2 -- A CONSTANT THAT IS RIGHT, CITING A SOURCE THAT IS WRONG, IN THE
DIRECTION THAT PUNISHES CHECKING.** The Rust CSPRNG module's per-call limit is
4096, correct, documented twice as mirroring `SYS_RW_MAX`. `SYS_RW_MAX` is
128 KiB and has been since the bulk-I/O lift; the bound actually mirrored is
`SYS_RW_STACK`. Anyone verifying the mirror against the constant it names raises
it to 131072 and lands in F1's silent-cap path. Task #86.

**F3 -- A FLAGS ARGUMENT DOCUMENTED THREE WAYS AND READ BY NOTHING.** The kernel
header describes `SYS_GETRANDOM`'s flags as zero-blocks-until-seeded,
one-returns-immediately. The handler range-checks the argument to 32 bits and
never reads it again; an unseeded generator always fails. The handler's own
comment is honest ("effectively v1.0's only mode"); the header, which is what a
userspace author reads, describes a blocking mode that does not exist. The
constant is defined in two headers, described three incompatible ways across
four files, and has zero readers. Task #85.

**F4 -- three enum comments** state a binary-name bound of 64 bytes. It has been
256 since the on-device toolchain needed absolute paths. Both mirrors carry 256
correctly; the stale copies are the kernel header's own prose about the calls the
bound governs. Task #87. Same family as #66 (argv bounds, one copy stale by 16x),
different constant.

**THE COUNTERWEIGHTS.** The reserved-slot growth pattern worked, and worked
twice, which is rare enough to name: a forward-compatibility field that is
*required to be zero* is claimable later without an ABI break, and the CL-5 page
budget did exactly that, leaving the struct at 96 bytes and its assertion message
spent on explaining the reuse rather than restating the offset. Second, the errno
registry deliberately keeps `T_E_PERM` at 1 so `-T_E_PERM` aliases the bare `-1`
sentinel -- and rather than paper over it, the header names the collision and
steers POSIX-EPERM contours to `T_E_ACCES`. Third, and best in shape: the poll
mirror's header says, correctly and completely,

> MUST mirror the kernel side; the kernel's `_Static_assert`s pin the layout
> **there**. Drift here would surface as a SYS_POLL ABI mismatch at runtime --
> keep them in lockstep.

It names the asymmetry, names the consequence, names the remedy as an
instruction to a person, and contains no assertion of its own. **The mirror
knows.** That is not ignorance; it is a documented acceptance that was never
revisited -- and it is the honest version of what the other 72 "mirror" lines
imply.

**PATTERN, TWELVE BATCHES.** b32 the guard is right about the case it was written
for and silently wrong about the adjacent one; b33 the reason was never written;
b34 the reason WAS written but not as a precondition on the helper, so the second
consumer could not inherit it; **b35 the documentation describes a STRONGER
guarantee than the code provides, the one consumer that needs it defends itself,
and the defence therefore reads as redundant.**

Every drift finding in this arc so far has been a document lagging its code.
This one leads it. The doc is not stale -- it describes a disposition that was
reasoned about carefully, applied next door, and never applied here. And because
the safe wrapper enforces it in userspace, the system behaves exactly as
documented for every consumer that exists, which is precisely why nothing has
failed and nothing has asked.

**AND THE REFLEXIVE FINDING IS BIGGER THAN LAST BATCH'S, AND SUBTRACTS FROM A
NUMBER I HAVE BEEN REPORTING AS PROGRESS.** [[sub-stratum-boot]] -- a dossier
titled "Bringup: spawn, wait for an event, attach, pivot", whose subject is
joey's boot sequence -- listed four files in `code:`: the init program it
describes, plus `kernel/syscall.c`, `kernel/territory.c` and
`kernel/9p_srvconn_transport.c`. It *traverses* those three. It documents none of
their internals. Two have real owners elsewhere and lost nothing. The third,
`kernel/syscall.c` at 8178 lines -- **the largest file in the kernel** -- had no
other claimant, and so has counted as swept since batch 12 on the strength of a
boot narrative naming two of its handlers.

The coverage ledger predicted this failure in its own text ("**this ledger
measures assertions, and a wrong assertion is what went wrong upstream of it**")
and demonstrated it deliberately, by making a dossier claim `mmu.c`. This is the
accidental instance, and it was already in the corpus when the warning was
written. The gap it exposes is that `code:` is doing two jobs -- *I describe this
file* and *I depend on this file* -- and a bare path cannot distinguish them.

Corrected: `sub-stratum-boot` now claims `joey.c` alone, with a Caveat recording
what it dropped and why. **Coverage goes DOWN by one owned file and UP by 8178
unswept lines.** The dispatcher wants a real sweep of its ABI-mechanism layer --
argument marshalling, the user-buffer validator, the errno clamp, the number
space -- distinct from the per-subsystem handler policy that ~30 dossiers already
cover. Task #88.

LEDGER, read off the rendered view rather than predicted. Corpus 822 -> **824**.
Coverage 165 -> **169 owned of 421**, 39% -> **40%**. `usr/lib` gains its first
three owned files and `usr/libthyla-rs` its first one, so the next sweep (#57)
starts from a foothold rather than zero. `arch` unchanged at 34/4.

**And the two metrics moved in opposite directions, which is the ledger telling
on itself.** Five files gained (7976 lines), one lost (8178), so owned-file count
rose by four and the headline percentage improved -- while **unswept lines went
UP by 202**. `kernel` shows it cleanly: 109 owned and 23 unowned both unchanged,
because syscall.h arrived as syscall.c left, and its unswept total went 5595 ->
**11041**. The percentage is file-weighted, so it is indifferent to whether a
file is thirty lines or eight thousand, and this batch is the case that
separates them: a correction that makes the ledger more honest also makes its
headline number look better. Both numbers are already rendered side by side --
the fix is to read the second one, not to add a third.
