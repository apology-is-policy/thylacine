---
id: chg-2026-08-03-syscall-dispatch-sweep
type: chg
title: "the dispatcher — the fix carried its own bug report, and landed on one of the two places that had the bug"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-syscall-dispatch
  - moc-kernel-entry
established:
  - sub-kernel-syscall-dispatch
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 36, the eighth sweep off the census, and the direct successor to batch 35:
`kernel/syscall.c`, 8178 lines, the largest file in the kernel and the one batch
35 returned to unowned after finding a boot dossier had been counted as its
sweep. Main unchanged at `10b1bbb2`. L-1 absent on the TWENTY-FOURTH check.

Taken before the userspace sub-arc (#57) deliberately: batch 35's dossier makes
several checkable claims about this file's behaviour, and #57 is nine areas that
should not start while the kernel's largest file sits unclaimed.

**THE SUBJECT IS STATED NARROWLY, WHICH IS THE POINT.** The file holds all 100
handlers, and most of a handler is policy belonging to its own subsystem --
roughly thirty dossiers already own those. [[sub-kernel-syscall-dispatch]] owns
what SURROUNDS them: the dispatch switch, the user-pointer validator, the two
staging tiers and their budget, the error conventions, and the handler/inner
split. A handler's semantics are its subsystem's; a handler's SHAPE is this
dossier's. Batch 28 is why that sentence is in the note rather than assumed --
"the claim's subject was narrower than the claim" is the defect this arc keeps
finding, and the fix is to write the subject down.

**MEASURED SOUND.** 100 live numbers, exactly 100 switch arms, sets equal both
ways. No arm falls through. Across all 100 arms the only registers read are
`x0..x5` plus `x8` -- the declared argument window, nothing past it (three
syscalls use the sixth argument; most use one or two). All four byte-I/O staging
sites are structurally identical and their charge/uncharge balances -- which
reads wrong at a glance, because the length variable is REASSIGNED to the stack
bound when the heap tier is not taken; it is sound because the reassignment
happens only when no allocation is held and every uncharge is guarded on holding
one.

**F1 -- THE FIX CARRIED ITS OWN BUG REPORT AND LANDED ON ONE OF TWO IDENTICAL
SITES.** Two syscalls have the same shape: copy a bounded kernel string into a
user buffer, return its length. `SYS_GETCWD` and `SYS_FD2PATH`.

The getcwd one computes the string FIRST, then validates and copies exactly the
bytes it will write, and says why in a comment that is unusually specific about
what went wrong:

> POSIX getcwd(buf, size) accepts ANY buffer large enough for the cwd -- do NOT
> reject an oversized one. The pre-fix `buf_len_raw > SYS_OPEN_PATH_MAX+1 -> -1`
> broke every caller passing a PATH_MAX (4096) buffer -- GNU make, clang, git,
> configure scripts, the near-universal `getcwd(buf, PATH_MAX)` idiom (surfaced
> by the CL-1c make oracle; `make: getcwd: I/O error`). ... compute it FIRST,
> then validate + copy EXACTLY len+1 bytes -- never the whole caller buffer.

It is a two-part rule with its rationale and a named field failure. **`SYS_FD2PATH`,
twenty lines below, still has both halves of what that rule removed**: it rejects
`buf_len_raw > SYS_OPEN_PATH_MAX + 1`, and it validates the caller's WHOLE buffer
rather than the bytes it writes. Neither is load-bearing -- the real fit check
happens twenty lines later against the actual string length, and the validator is
overflow-safe without an upper bound. All three copies of the ABI (kernel header,
C mirror, Rust mirror) document only the too-SMALL failure; none mentions
too-large. Task #89.

Unreached today: the only caller is a boot probe with a 64-byte buffer. The caller
class it would meet is precisely the one the fix's own comment enumerates -- the
Plan 9 `fd2path(fd, buf, sizeof buf)` idiom is the same shape as
`getcwd(buf, PATH_MAX)`, and the planned `/proc/fd` consumer (#66c) is a candidate.

**F2 -- ONE AUTHORITY GATE SITS ABOVE A SEPARATELY-CALLABLE INNER.** Forty-five
syscalls split into a `_handler` (raw registers, thread resolution, pointer
validation, staging) and a `_for_proc` inner (explicit process, kernel buffers).
The inner is the testable half, and eight are called from production kernel code.
That split implies a rule: **the handler may own only what is about userspace, and
every authority gate belongs in the inner** -- because an inner is separately
callable, so a gate above it is a gate some caller does not pass.

The rule holds nearly everywhere. The exception is `sys_console_open`: the I-27
console-attach gate (the A-5a F2 fix -- only the console-trust anchor may take the
single-reader console) is in the HANDLER, and `sys_console_open_for_proc` is
non-static. No production caller uses the inner; the kernel test suite calls it
three times, which is legitimate and is also the demonstration that the bypass
exists. Task #90.

**F3 -- THE VALIDATOR'S ZERO-LENGTH PASS IS A RANGE PROPERTY, NOT A POINTER
PROPERTY.** `sys_validate_user_buf(0, 0)` returns true. Correct -- nothing is
dereferenced -- and every one of its 49 callers pairs it with the length it will
actually touch. But the name reads as pointer validation, and a caller that
validated with zero and then dereferenced would be unprotected. Documented in the
dossier rather than filed: there is no wrong site today. Task #91 tracks the
naming.

**THE COUNTERWEIGHTS, AND THE BEST ONE IS A DESIGN RULE WORTH COPYING.** The JIT
surface checks its capability exactly ONCE, at create. Publish and teardown
re-check nothing -- publish instead requires its range to lie inside a live VMA
whose Burrow is of the code type, and only the capability-gated create mints one.
**The authority is carried by kernel-minted OBJECT TYPE, not by a capability
re-check at every touch.** That is strictly stronger than re-checking: a re-check
has to be remembered at each new operation and forgetting one is silent, whereas
a new operation on a code region inherits the gate by construction. The teardown
non-gating is argued explicitly too -- releasing memory you already own is not an
exercise of the emit authority, and gating it would turn a capability expiry into
a leak.

Second: the console write stages its bytes into kernel memory BEFORE claiming the
console writer role, because faulting a user page can sleep and holding the
console across an unbounded page-in would stall every other writer. Third, and
this one is the arc's own lesson written by someone else: the JIT teardown
validates its second alias' geometry before touching either mapping, and says

> Unreachable today ... But that is an UNASSERTED invariant of this function, and
> the neighbouring unreachability claim just below (that nothing else can remove
> one alias) was FALSE until this round's detach gate landed. Assert it rather
> than inherit it.

That is exactly "a stated reason expired under a gate that could not see it",
caught by the CL-7k audit and written into the code as a standing correction.

**PATTERN, THIRTEEN BATCHES.** b32 the guard is right about the case it was
written for; b33 the reason was never written; b34 the reason was written but not
as a precondition on the helper; b35 the doc described a stronger guarantee than
the code; **b36 the fix was written WITH its bug report -- the failing callers
named, the error message quoted, the rule stated in two parts -- and applied to
one of the two sites that had the bug.**

That is the most legible failure mode yet, and the least excusable in principle:
nothing was undocumented, nothing was subtle, and the sibling was twenty lines
away in the same file. What it shows is that a fix's blast radius is decided by
whoever writes it, at the moment they write it, and no later process re-asks the
question -- an audit round is scoped to the change, and a change scoped to one
call site produces a review scoped to one call site. The two-part rule with the
worked failure is exactly the kind of thing that SHOULD have propagated, and its
quality is what makes the non-propagation notable rather than the reverse.

LEDGER, read off the rendered view rather than predicted. Corpus 824 -> **826**.
Coverage 169 -> **170 owned of 421**, still 40% by file count -- but unswept
lines fall 105542 -> **97364**, the whole 8178 back in one motion. This is the
mirror image of batch 35, where five small files arrived as one large one left
and the percentage rose while the line count worsened; here one large file
arrives, the percentage does not move, and the volume drops by 8%. Two batches,
opposite directions on the same pair of numbers -- the file-weighted headline is
simply not measuring the same thing as the line count, and the pair is only
legible read together.

**And the kernel is now essentially swept: 110 owned / 22 unowned, with 2863
unswept lines across those 22** -- about 130 lines each, all small headers and
leftovers ([[chg-2026-08-03-crash-debug-sweep]]'s uart and psci pairs among
them, plus the three orphans of #32). `arch` unchanged at 34/4. So of the 97364
lines still unswept, roughly 93700 are userspace: **#57 is now very nearly all
of what remains**, which is worth knowing before it starts, because it is a
sub-arc rather than a batch and its shape should be chosen deliberately.
