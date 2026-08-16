---
id: sub-kernel-boot-sequence
type: sub
parent: moc-kernel-boot
title: "The boot sequence — an order that is the contract"
code:
  - kernel/main.c
  - arch/arm64/hwfeat.c
  - arch/arm64/hwfeat.h
audit: hard
guarded-by: [inv-i15]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: [abi-boot-banner]
design:
  - "docs/TOOLING.md section 10"
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

Bring roughly forty subsystems up in an order where almost every position is
load-bearing, print the banner that is the tooling's contract with the kernel,
run the in-kernel suite, and hand control to init.

The subject of this dossier is the **order**, not any one initializer. Each has
its own home; what lives here is why it sits where it does, and what breaks if it
moves.

## Contract

Entered from [[sub-kernel-boot-entry]] at the randomized high virtual address,
with a stack, cleared BSS, and PAC live. Never returns.

The externally-visible contract is the banner, defined by [[abi-boot-banner]]:
one success line, and a distinct prefix for a fatal fault. Everything else printed
is diagnostic. The success line is **not** printed at the end of this function —
it is printed when init explicitly signals that its own checks passed, which is a
consequence of init having become a process that never exits.

## Mechanism

**The four phases.** Describe the machine (parse the tree, find the console,
print what was found). Build the memory system (physical allocator, then the
allocator that sits on it, then relocate the tree into it). Arm the machine
(exception vectors, interrupt controller, timer, then unmask interrupts). Build
the process world (address-space groups, handle tables, memory objects, the
scheduler, then devices, then drivers).

Then: patch instructions, start secondaries, retire the identity map, install the
idle thread, start the kernel service threads, run the suite, enable SMP
behaviour, and exec init.

**The orderings that matter**, each recorded beside its call:

- **Instruction patching before secondaries.** The pass rewrites code with one
  CPU running and all exceptions masked; a second CPU executing a site mid-rewrite
  is the failure it is placed to avoid. See [[sub-kernel-alternatives]].
- **Identity-map retirement after secondaries.** Their bring-up trampoline runs
  through the identity map until it re-anchors to high addresses. Retiring
  earlier strands them.
- **The wall-clock anchor before SMP.** Written once while one CPU runs, which is
  what makes a single unsynchronized value sufficient for every later reader.
- **The tree relocated after the allocator, before retirement.** It needs
  somewhere to go, and it must be somewhere durable before its original mapping
  is withdrawn.
- **Interrupt reservations before any driver can ask for one.** Kernel-owned
  interrupt numbers and memory ranges are claimed so that a userspace driver's
  later request cannot take one out from under the kernel.
- **The idle thread before the suite**, because tests block, and a blocking
  thread with nothing else runnable needs somewhere to go.
- **Console receive and the manager thread after the scheduler**, because the
  manager is a thread and the interrupt handler that feeds it can only wake
  things.
- **Buffered console output armed only once an interrupt can drain it.** Every
  print before that point takes the direct path; the ring is empty at the moment
  of transition, so the change cannot reorder output.

**Feature detection.** Identifier registers are read once and reduced to a
struct: which hardening features the silicon implements, how many debug
breakpoints it has, and a word in the format userspace expects. The struct is a
*report*, not a gate — the hardening is enabled unconditionally where the
architecture makes that harmless and gated where it does not.

The published word is deliberately Linux-shaped, so ported code's existing
feature detection works unmodified. Two of its fields are inverted sentinels
where zero means present, unlike every neighbouring field, and the code says so
at the site.

**Per-CPU identity is handled differently from features.** The processor
identifier and cache line size are recorded *by each CPU, into its own slot*, at
its own bring-up, with a release store publishing validity last. The header
explains why: both registers genuinely differ on a heterogeneous machine, so a
boot-CPU-only read would be wrong precisely where the values matter.

**Two cache sizes are now decoded, and they answer different questions.** The
minimum data line is the smallest span a level will allocate — the maintenance
number, the one every cache-clean loop strides by. The **writeback granule** is
the largest span one eviction may write back, and it is the number that governs
**false sharing**; the architecture permits them to differ, and only the second
one tells you how far apart two hot per-CPU fields must sit
([[sub-kernel-gic]] is the consumer).

A granule field of zero is decoded as **zero, verbatim** — architecturally it
means "this part provides no writeback-granule information", which is a
different fact from "the granule is four bytes" and a different fact again from
the architectural maximum. Recording the raw distinction is what lets a consumer
apply its own policy instead of inheriting a fabricated one. Full emulation
reports zero.

The padding constant is a **compile-time** value because struct layout is, so it
cannot be the measured number; it is set to twice the largest granule any target
reports, on an asymmetry argument (over-padding wastes a fixed few hundred bytes
once; under-padding silently restores contention with no symptom a test can
catch). The recorded per-CPU value is what keeps that constant honest — a
registered test fails loudly if a target ever reports a granule the constant
does not cover.

**The falsification is recorded in the header and the falsified claim still
stands in the source file.** See Caveats — it is the clearest instance in this
vault of a correction landing only where it was noticed.

**The suite runs in a deliberately UP-like configuration.** Cross-CPU wakeup
notifications and secondary preemption are both off during the tests and enabled
immediately afterwards, because a secondary waking on a timer tick and stealing a
test's thread surfaced as a failure in a scheduler test. This is a real narrowing
of what the suite can observe, taken knowingly, with the multi-boot gate as the
compensating control.

## Data structures

Almost none of its own: a one-shot completion flag, a painted boot stack for
depth measurement, and the feature struct plus the per-CPU identity array.

The stack painting is a nice small technique — fill the unused stack below the
live frame with a sentinel, scan afterwards for the deepest overwrite, and report
the high-water mark. It distinguishes a genuine depth overflow from a wild stack
pointer landing in the guard page, which look identical from the fault alone.

## Concurrency

Single CPU for most of its length. After secondaries start, this function is
still the only thing driving boot; the parallelism it enables is used by threads
it created.

The completion flag is a one-shot atomic exchange, so a second signal is a no-op.
The signalling syscall additionally requires the caller to hold the console, so a
spawned child cannot emit a premature success line — which would be a false pass
for every tool watching for it.

## Invariants enforced

- **[[inv-i15]]** — every hardware fact this function acts on comes from
  [[sub-kernel-dtb]]; the feature word is derived from identifier registers,
  which is the same principle applied to the CPU.

## Error paths

Almost every initializer that can fail is fatal, and deliberately so: there is no
degraded mode for a kernel without a physical allocator or an interrupt
controller. The exceptions are the ones with a defined fallback — no
power-management interface means running with one CPU, no entropy device means
keeping the boot seed, no real-time clock means an epoch of zero and a clock that
reads uptime.

The suite failing is fatal. Init exiting non-zero before signalling completion is
fatal, inside the init runner.

## Performance

Boot duration is measured from the counter read taken in the stub and reported as
a greppable number, along with a work-conservation summary for the boot itself —
how much idle time was spent parked while work was queued elsewhere. Both exist
because a boot-time regression is otherwise invisible until someone notices the
loop feels slow.

## Prosecution

- **Every reordering is a potential correctness change**, and the dependencies are
  recorded only as comments beside the calls. There is no graph and nothing
  mechanical. The five orderings listed above are the ones where a move is
  silently wrong rather than immediately fatal.
- **The success line and the fault prefix are ABI.** Changing either without the
  coordinated tooling update breaks every gate at once; see [[abi-boot-banner]].
- **The completion signal must stay one-shot and console-gated.** Both properties
  exist to prevent a false success.
- **The suite's UP-like configuration must stay bounded to the suite.** The two
  enables sit immediately after it; moving either earlier changes what the tests
  observe, and moving them later leaves the system without work-stealing.
- **The published feature word must be truthful.** A set bit whose instructions
  fault on this CPU is a delayed crash in every consumer; a clear bit is a
  fallback. It fails safe in one direction only.

## Seams

- [[seam-hwcap-boot-cpu-only]] — features are read from the boot CPU and used
  system-wide, while per-CPU identity is read per CPU.

## Caveats

**A measured falsification and its own falsified claim ship in the same commit,
in two files.** The header's constant records, honestly and in detail, that an
earlier draft asserted the development host's silicon reports a 128-byte
writeback granule, that **one boot falsified it** — the granule equals the
minimum line, at 64, under both hardware virtualization on that host and full
emulation — and that the constant now rests on a margin argument instead.

The decode site in the source file still says the opposite, as a parenthetical
supporting the (true) general point that the two sizes may differ: *a part may
allocate 64-byte lines while its coherency protocol moves 128 — this silicon
does exactly that.*

The general claim is correct and the instance cited is the one that was
measured and refuted. Worse, the header scopes its own correction to *"an
earlier draft of **this comment**"*, which reads as though the claim were
handled — and the surviving copy is a **different** comment that made the
**same** claim.

Two further points bound how it should be fixed:

- **A delete is not obviously right.** The measurement was taken *through* a
  hypervisor, and whether that path presents the silicon's own cache register
  or a synthesized one is itself unverified. So the parenthetical is not
  merely refuted — the claim is **unverified in both directions**, and the
  honest replacement says so rather than asserting the opposite.
- **Nothing depends on it.** The constant is a margin, the recorded value is
  raw, and the geometry test compares them. This is a reader hazard, not a
  correctness one — which is exactly why it survived: the sentence it lives in
  is load-bearing and true, and only its example is wrong.

[[seam-cwg-parenthetical-refuted]].

**The boot path's test is the boot.** Seven registered tests cover this whole area
— one for the entropy mix, three for the tree parser, two for the patcher, one for
feature detection — against roughly four thousand lines. That is not a coverage
gap so much as a description of the situation: if the ordering is wrong, the suite
does not run, so the suite running at all is the evidence. The consequence worth
holding onto is that **the evidence is configuration-specific**. It shows the boot
path works on the machine that booted, in the configuration that booted, and says
nothing about the paths that machine does not take.

**The audit-trigger table names a file that does not exist.** The boot row cites
a path under an `init/` directory in both copies of the table — and the two copies
name *different* files, neither of which is in the tree, which has no `init/`
directory at all. The initial-userspace process lives elsewhere and has done since
it was rewritten as a persistent supervisor. The table is the mechanism that
decides what gets an adversarial round, so a prosecutor scoping this area from it
would go looking for a file that was never there.

## Provenance

Read from `kernel/main.c` (932 lines) and `arch/arm64/hwfeat.c` (199) in full,
2026-08-02, during the boot sweep. The feature-detection call has exactly one
caller — this function, on the boot CPU — verified by census across the tree.

The writeback-granule decode, the compile-time padding constant, and the
refuted parenthetical are [[chg-2026-08-16-boot-cwg-parenthetical]]. The banner
emitter's writer role — the delivery half of [[abi-boot-banner]] — is
[[chg-2026-08-16-cons-writer-set]].
