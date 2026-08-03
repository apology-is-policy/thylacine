---
id: chg-2026-08-03-exec-image-sweep
type: chg
title: "exec and the image cache -- three documents wrong about which code runs, and a checker that could not see the field that was wrong"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-exec
  - sub-kernel-elf
  - sub-kernel-image
  - inv-i36
  - inv-i12
  - arc-revenant
  - moc-kernel-execution
  - moc-kernel-memory
  - sub-kernel-fault
  - sub-stratum-boot
established:
  - sub-kernel-exec
  - sub-kernel-elf
  - sub-kernel-image
  - inv-i36
  - arc-revenant
closed: []
opened: []
depth: skeletal
created: 2026-08-03
---
Batch 31, the third sweep off the census: exec + the ELF loader + the Image
cache -- `kernel/exec.c`, `kernel/elf.c`, `kernel/image.c` and their headers,
~2,100 lines, holding **I-36**. Main unmoved at `c0c76977`; L-1 absent on the
NINETEENTH check. Three dossiers, [[inv-i36]] (deliberately deferred at batch 29
until both halves were read), and [[arc-revenant]].

Every finding this batch is the same question asked of a different noun: **does
this thing exist, and does it run?** Nothing in the build asks it.

**F1 -- THE THIRD DORMANT DECLARATION IN THREE BATCHES.** `elf_brand_hint`
(`elf.c:288`) reports whether a binary looks Linux-shaped. It has **zero
production callers** -- only the test suite. Its header states the purpose: *"a
Linux-interp binary exec'd OUTSIDE a vivarium earns a diagnostic and a clean
failure instead of a silent mis-decode."* No diagnostic exists anywhere; such a
binary gets `ELF_LOAD_HAS_INTERP`, which `exec` collapses to a bare `-1` -- the
silent failure the function exists to prevent. [[arc-vivarium]] is complete.

It differs from its two predecessors in a way worth recording: **no document
claims it is wired.** A search of `docs/` and CLAUDE.md returns nothing. So
unlike `pte_violates_wxe` (named by five) and `NOTE_MASK_SUPPORTED` (whose
comment described an outcome it did not cause), this one misleads no reader --
it is simply finished work with no consumer. Eleven test assertions cover it,
including one that *fails if someone "improves" it* by consulting `EI_OSABI`: a
regression test protecting a deliberate non-decision inside a function that
never runs. Task #62.

**F2 -- TWO DOCUMENTS CALL THE BLOB EXEC PATH TEST-ONLY. IT IS THE BOOT PATH.**
`exec.h:299` says `exec_setup` is *"retained for the kernel test suite"*;
CLAUDE.md's REVENANT row says *"kept test-only"*. `kernel/joey.c:164` loads
**init** through it -- compiled unconditionally (`CMakeLists.txt:67`), zero
`KERNEL_TESTS` guards in the file.

This is not a tidiness complaint. The two paths differ in ways that matter to a
prosecutor: the blob path slurps a whole image, never consults the Image cache
(so init's text is shared with nothing), and reads from memory rather than
through a death-interruptible `dev->read`. **A reader working from either
document skips the code that loads PID 1.** Task #63.

**F3 -- TWO DESIGN DOCUMENTS THAT DO NOT EXIST.** `docs/REVENANT.md` is cited
**six times across five source files** (`exec.c`, `image.c`, `burrow.c`,
`image.h`, `burrow.h`) with section numbers. The sections resolve -- in
`docs/EXEC-LOAD-DESIGN.md`, the name that was actually committed. `git log --all`
confirms no file has ever existed under the cited name.

The second is worse. `docs/reference/88-pouch-stratumd-boot-16c.md` is named
**three times by CLAUDE.md and once by POUCH-DESIGN.md** ("the new ... *once impl
lands*"). The impl landed years of chunks ago; the document never did -- and
`88-ninep.md` has since taken the number, so a reader who goes looking finds an
unrelated document sitting in the slot. Task #64.

**F4 -- A HEADER CALLS ITS OWN PRODUCTION CALLER ABSENT.** `image.h`'s closing
paragraph: *"At R-3 there is NO production caller: exec still slurps (R-4 wires
`image_lookup_or_create` in place of the eager whole-ELF read)."* R-4 landed.
`exec.c:507` is the caller; `main.c:515` calls the initializer. The header names
the exact sub-chunk that would land the consumer, and a reader who trusts it
concludes the file is dead code. Task #65. Batch 29's F3 shape (headers calling
landed work "future"), one repo-layer up.

**F5 -- A SIXTH DOCUMENT ON I-12, NAMING A SYSCALL THAT DOES NOT EXIST.**
`elf.c`'s file header calls the loader *"one of three layers (PTE bits +
mprotect + ELF loader)."* Searching `kernel/`, `arch/` and `mm/` for `mprotect`
returns **exactly one hit: that comment.** The *absence* of a protection-changing
syscall genuinely is one of the five mechanisms holding [[inv-i12]] -- but an
absence is not a layer, and listing it beside two real checks converts a
strength into a phantom. The same sentence omits `vma_alloc`, making this the
**sixth** document to do so. Folded into task #59.

Six documents enumerate I-12's enforcement. **Every one names something that
cannot fire; none names the single line that always does.** They were written
from the design and never re-derived from the code.

**F6 -- THE ARGV BOUNDS EXIST IN SEVEN PLACES AND ONE IS ALREADY STALE.** Two
authoritative macros; two hardcoded literals in `exec.c` (with a comment saying
they *"mirror"* the macros, kept local to dodge an include cycle); two more
inside `EXEC_INIT_STACK_MAX_SIZE` -- which feeds the `_Static_assert` proving a
Shape-B frame fits under the 1 MiB stack; and two prose statements, of which
**`exec.h:264` says 4096 for a constant that is 65536**, seventy lines below the
same header saying 64 KiB. No assert ties any copy to any other.

The drift already happened, in the copy nobody compiles. The failure mode if it
happens in a copy that *is* compiled: the syscall admits an argc the frame
builder extincts on -- a kernel panic from a one-line constant edit. Task #66.
Batch 26's pattern with the pinning removed entirely.

**AND THE VAULT DID THE SAME THING, IN THE FIELD ITS OWN CHECKER SKIPS.** Batch
29 wrote `docs/REVENANT.md` into [[sub-kernel-fault]]'s `design:` -- copied
faithfully out of the source it swept, never resolved. Batch 28 built
`checkCodePaths` for exactly this failure mode and pointed it at `code:` only.
Both fields hold repo-relative paths; both are resolvable; **one was checked, on
no better grounds than the `models:`-vs-`mirrors:` line the same probe already
criticized.** Extended it; it immediately found the second instance
([[sub-stratum-boot]] carrying the phantom `88-*.md`). Both fixed.

The extension's own first run is the sharper half. It reported **51 failures, 49
of them wrong** -- because `design:` entries are document *references*
("`docs/ARCHITECTURE.md section 5`"), not bare paths, and I checked the field
without reading what the corpus actually puts in it. A claim about a field, true
of the two entries in front of me and false of the other thirty-seven. The fix
validates the leading token only. **The check written to catch an unverified
assumption began by making one.**

**THE COUNTERWEIGHT.** `elf.c` holds this arc's best example of the *inverse*
finding: the W^X check is hoisted **above** the type switch, deliberately, so a
future `PT_*` nobody has heard of inherits it without anyone remembering to add
it -- a guard made wider than the case that motivated it, in the same file whose
header mis-describes the layer it belongs to.

And `image.c`'s eviction-safety argument is the strongest reasoning in the
batch. "Could a mapper be part-way through claiming this entry?" is a race
question, normally answered with more locking. Here it is answered by
**reachability**: claiming requires coming through a function that takes this
lock, so while eviction holds it no such claimant can exist, cannot appear, and
once the entry is detached is reachable by nobody -- so the final unref outside
the lock still cannot race. The same move the MMU makes when it pre-demotes the
allocator zone at boot to make a split race unreachable rather than guarded.

**PATTERN, EIGHT BATCHES.** b24 assertions pin values not their description; b25
models pin mechanisms not their own scope; b26 each copy pinned to itself not to
the others; b27 the guard travelled but not its reason; b28 the ledger pins the
areas not the areas to the tree; b29 the enforcement list names a guard that
cannot fire; b30 plus a justification whose stated and real reasons diverged;
**b31 the documents are wrong about which code runs -- a function nothing calls,
a path called test-only that boots the machine, two design docs never written.**

All three are answerable by one cheap question the build never asks. The vault
now asks it of two frontmatter fields, having just demonstrated why one field
was not enough.

LEDGER. Corpus 806 -> **812**. Coverage 142 -> **148 owned of 421 (35%)**;
`kernel` 39 unowned -> 33. [[inv-i36]] joins at last -- and it is the first
invariant swept here whose **first two conditions have no enforcement site in
this repository**: immutability and integrity are Stratum's, so five of seven
are checkable from the code and two rest on a cross-project contract. Worth
stating, since the usual reading of an invariant note is that everything in it
is checkable from what it points at.
