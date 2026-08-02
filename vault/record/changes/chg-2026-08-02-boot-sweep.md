---
id: chg-2026-08-02-boot-sweep
type: chg
title: "vault sweep: the boot path -- where the tools' assumptions are not yet true"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-boot-entry
  - sub-kernel-boot-sequence
  - sub-kernel-kaslr
  - sub-kernel-dtb
  - sub-kernel-alternatives
established:
  - inv-i15
  - inv-i16
closed: []
opened:
  - seam-kaslr-link-va-unchecked
  - seam-dtb-blob-internally-trusted
  - seam-hwcap-boot-cpu-only
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 18. Read from code: `arch/arm64/start.S` (704), `kernel.ld` (255),
`kaslr.{c,h}` (390), `alternatives.{c,h}` (177), `atomic_lse.h` (107),
`hwfeat.{c,h}` (293), `lib/dtb.c` (1205), `kernel/main.c` (932), plus the
patching helpers in `mmu.c` and the four boot-area test files. `boot/` had been
declared and empty since commit 0.

WHY THIS BATCH. L-1 still had not reached main (SIXTH check -- `addrspace.h`
absent), so address-space stayed deferred. Of the two remaining empty areas,
`boot/` homes two unminted invariants (I-15, I-16) and carries the alternatives
patcher, which self-modifies `.text` and is the sharpest instance of I-12 outside
the MMU. `devices/` is larger and more heterogeneous -- two or three batches'
worth -- so it waits.

THE ORGANIZING FACT: **boot is the region where the tools' assumptions are not
yet true**, and every local oddity is one of them being worked around.

    an address means what the linker said  -> false until the long branch
    memory is Normal and unaligned-tolerant -> false until the MMU
    a constant can be shared by #include    -> false across asm / C / linker
    a test suite exists to run              -> false by definition

THE FIRST TWO PRODUCED WORKED FAILURES WHERE THE COMPILER DEFEATED THE CODE, both
fixed with `volatile`, both independent, both in this area. (a) Under PIE clang
treats `&_kernel_start` as a link-time constant and folded a store-then-load into
a one-byte "was it set" flag plus that constant -- correct everywhere except
pre-MMU, where PC-relative addressing yields the LOAD PA and the two differ, so
the accessor returned the wrong address. (b) Pre-MMU kernel data accesses are
Device-nGnRnE, which mandates natural alignment; clang was observed fusing two
adjacent 4-byte DTB reads into one 8-byte load, which FAULTS. Neither is a
compiler bug. Both are the same shape: **this code runs on a machine the compiler
is not modelling.**

THE HEADLINE -- TWO HEADERS EACH CLAIM THE OTHER ENFORCES A CHECK THAT NEITHER
PERFORMS. The kernel's link-time virtual base exists as two constants, one in the
linker script and one as a C macro; they must be equal and nothing checks it. The
C header says "the linker ASSERT enforces the C / linker-script values agree";
the linker script says the same and names the WRONG header for the C copy. The
assertion they both point at compares `_kernel_start` against `KERNEL_LINK_VA` --
both linker-side, assigned to each other ELEVEN LINES APART. It is a tautology
and cannot see the C macro. Verified by census: no `_Static_assert`, no `-D` from
the build, no generated header.

What makes it sharp is that **the same file family solves this problem correctly
TWICE, with a named idiom.** Assembly cannot include a C header, so a per-CPU
stack slot size and a CPU-count bound are hardcoded as literals in assembly -- and
in both cases the C header carries a `_Static_assert` pinning ITS OWN constant to
the literal the assembly hardcodes, with a message naming the assembly file. That
does not compare the two languages; it guarantees that changing the C side fails
the build with a pointer at the other copy, which is all that is needed since
silent divergence is the only failure mode. So the gap is not that the problem is
unsolved -- it is that **the one instance that CLAIMS to be solved is the one that
isn't, and the two that claim nothing are.** [[seam-kaslr-link-va-unchecked]],
task #24; fix is the existing one-line idiom.

Consequence if they diverge: a wrong long-branch target, jumping into unmapped
high VA immediately after `mmu_enable` -- with no VBAR installed (that is much
later) and the console not yet remapped. Definite failure, zero diagnostic. The
least debuggable window in the system.

THE STALE-SUMMARY CLASS MOVES INTO SCRIPTURE. Three prior batches found ten
instances, every one in code (file-header prose). This batch's are in BINDING
DOCUMENTS: (a) **the audit-trigger table's boot row names a file that does not
exist -- and the two copies name DIFFERENT nonexistent files** (`init/init.c` in
CLAUDE.md, `init/joey.c` in ARCH section 25.4; the tree has no `init/` directory
at all, and ARCH is the copy the project designates authoritative). The table is
the mechanism that decides what gets an adversarial round, so a prosecutor scoping
this area from it goes looking for a file that was never there. (b) CLAUDE.md's
thematic-naming section still lists the `_hang` -> `_torpor` rename as "held for
explicit signoff"; the tree contains no `_hang` -- the rename landed and the
record of it did not. Both are cutover items rather than tasks. Plus one in code:
the DTB depth cap's comment says an over-deep tree "would panic, not silently
corrupt" -- the safety half is right (no out-of-bounds access anywhere; the index
advances unconditionally but every access is guarded) and the mechanism half is
wrong: it degrades silently to not-found, which is arguably the better of the two.

A FOURTH INSTANCE, AS A DRIFT CASE STUDY: the KASLR entropy figure. The
alignment has been widened twice (each time because a sanitizer image outgrew the
page-grain kernel mapping) and each widening costs a bit. The figure now appears
across the record as **13** (pre-widening), **12** (`05-kaslr.md` + a REFERENCE.md
snapshot), a "future bump to **17**", a row promising **18** -- and **11** in the
code. Five values, four documents, one quantity, none of them current. The
dossier and inv-i16 state the durable fact instead: the number is the accumulated
cost of image growth rather than a security calculation, and the next widening
spends another bit.

MEASURED, NOT COUNTED BY EYE. **Seven registered tests for ~4150 lines** (1 mix
avalanche + 3 DTB lookups + 2 patcher + 1 feature detection). Not a coverage gap
so much as a description: if the ordering is wrong the suite does not run, so
**the boot path's test is the boot** -- every one of the 1200+ downstream tests is
evidence that the ordering worked. The sharp edge is that the evidence is
CONFIGURATION-SPECIFIC: it shows the boot path works on the machine that booted,
in the configuration that booted. Also verified: feature detection has exactly ONE
caller (boot CPU); five link-time assertions in the linker script; 11 bits of
slide entropy at 8 MiB alignment over a 16 GiB window (mask arithmetic checked).

THE PATCHER'S W^X ARGUMENT IS STRUCTURAL, NOT A SHORT WINDOW. The canonical
mapping stays read-execute and the direct-map alias read-only-non-executable; the
write goes through a transient read-write-NON-EXECUTABLE alias at a dedicated
scratch address. No mapping ever carries both permissions -- the property holds at
mapping granularity rather than by keeping a window brief. And the whole design
FAILS SAFE by inversion: the baseline (the multi-instruction exclusive loop) is
the IN-PLACE form and the fast single instruction is what gets copied in, so a
patcher that skips an entry, misreads a feature, or does nothing yields a slower
kernel, never a wrong one.

TWO TARGETS, TWO HALVES. The patcher's count assertion is non-vacuous on BOTH
gates but asserts DIFFERENT things on each (everything-patched on the development
host; nothing-patched on the v8.0 floor), while the hardening test's PAC/BTI
implications are non-vacuous only where the features exist. So [[gate-v80-floor]]
is not optional coverage here -- it is the only instrument that can see a
regression in the unpatched path. Neither gate can see a memory-ORDERING mistake
in a replacement: a relaxed form substituted for an acquire-release one computes
the same values and passes both tests. [[gate-smp]] is the only backstop, and only
probabilistically.

THE THIRD SEAM CHANGED SHAPE WITHOUT ANYONE NOTICING. The feature word is
boot-CPU-derived and feeds both the `.text` patcher and every process's published
capability word; both consumers document the heterogeneous-target fix. What is
new is that **the infrastructure that fix needs now exists and is wired**:
per-CPU IDENTITY (processor id + cache line size) is recorded by each CPU into its
own slot at bring-up, with a header stating the exact argument -- a boot-CPU-only
read would be wrong precisely where the values earn their keep -- three lines from
where the capability read would go. The seam's cost dropped from "add a mechanism"
to "call the existing one", and nothing recorded that. The two halves still differ in
cost, though: the published word only has to be right before the first program
starts, whereas making the PATCHER heterogeneity-correct reverses one of the boot
sequence's deliberate orderings. [[seam-hwcap-boot-cpu-only]], no task
(homogeneous everywhere today; an unimplemented instruction traps rather than
silently misbehaving).

REGISTRY TAIL. Minted this batch's own dependencies and stopped: **inv-i15**
(hardware view from the tree; guards the parser + the sequence) and **inv-i16**
(randomized, never-zero kernel base). **I-12 deliberately NOT minted** despite the
patcher being its sharpest instance -- its enforcement home is the MMU's
page-table construction, unswept, and minting it under the patcher would misfile
it. Recorded in prose as a claim this area upholds. Same treatment as I-5 under
Weft (batch 17) and I-13's separation half (batch 16); third consecutive batch.

SCOPE. Secondary CPU bring-up -- the PSCI trampoline, `per_cpu_main`, the
online/alive handshake, and the dense-index equality assertion -- is already owned
by [[sub-kernel-sched-smp]] and was cross-linked, not restated. `fault.c` and the
MMU table construction stay for the memory area.
