---
id: sub-kernel-boot-entry
type: sub
parent: moc-kernel-boot
title: "The pre-C stub and the image layout"
code:
  - arch/arm64/start.S
  - arch/arm64/kernel.ld
audit: hard
guarded-by: [inv-i16, inv-i21]
validated-by: [prose, gate-smp, gate-v80-floor]
locks: []
abis: [abi-boot-banner]
design:
  - "docs/ARCHITECTURE.md section 5"
  - "docs/reference/08-exception.md"
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

Take control from a bootloader that knows nothing about Thylacine, and hand a C
function a machine it can run on: a stack, cleared BSS, deterministic per-CPU
registers, PAC keys, a randomized high-VA mapping, and a program counter that has
already moved into it.

The kernel presents itself as a Linux arm64 `Image`, so this stub also *is* the
image header. That choice is load-bearing rather than cosmetic: QEMU's
`load_aarch64_image()` only passes a DTB pointer when it recognizes a Linux-shaped
kernel. Without the header, the ELF path runs, the DTB is never placed in RAM,
and [[sub-kernel-dtb]] has nothing to parse.

## Contract

Entry is the Linux arm64 boot protocol: MMU off, caches off, interrupts masked,
one CPU running, `x0` = DTB physical address. Entry at EL1 or EL2 is supported;
EL3 and EL0 halt silently, because reporting the problem needs a stack and a
UART base neither of which exists yet.

On exit the kernel is executing `boot_main` at a randomized high virtual address
with `SP_EL1` anchored to the same image's high-VA boot stack. `boot_main` never
returns; if it does, the stub falls through to the halt loop.

## Mechanism

**Establish the exception level.** `CurrentEL` selects between proceeding
directly and the canonical EL2→EL1 drop (`HCR_EL2.RW`, timer access, no stage-2,
a known `SCTLR_EL1`, then `eret` with the general registers — including the DTB
pointer — preserved). Which path was taken is recorded and surfaced in the
banner, because on bare-metal firmware that boots at EL2 it is the first thing
worth knowing. On QEMU virt the EL2 arm is dead code, kept exercised by review
rather than by execution.

**Take the stack, in the right bank.** `SPSel` is asserted to 1 *before* the
stack pointer is written, so the write lands in `SP_EL1` regardless of how
firmware left the bank. This is [[inv-i21]] at its origin: the kernel is
uniformly EL1h from its first instruction, so a thread's execution mode is never
a function of its scheduling history.

**Clear BSS, then make the per-CPU registers deterministic.** `TPIDR_EL0`,
`TPIDR_EL1` and `TPIDRRO_EL0` have architecturally UNKNOWN reset values, so "BSS
is zero" does not cover them — a pre-`thread_init` read of `current_thread()`
would otherwise return firmware residue, and `TPIDR_EL0` would leak that residue
into the first thread's saved TLS.

The third register is on a different axis from the other two and is worth
separating. The kernel never writes it, and userspace can always read it — so a
boot leftover there is not a correctness hazard that resolves at the first
thread creation, it is **disclosure that persists for the machine's uptime**.
Two of the three are about being wrong; the third is about being seen.

### The trampoline did not inherit any of it

The secondaries enter through a separate stub in this same file, and for a long
time that stub zeroed none of these registers. The reset values are UNKNOWN at
power-on-secondary entry for exactly the reason they are UNKNOWN at cold boot;
the argument that motivated the primary's fix applied verbatim, and the code
that implemented it was never copied across.

**What kept it invisible is the sharper half.** Every emulated substrate the
project runs on resets these registers to zero, so the thread-pointer read
returned NULL and the NULL guard downstream held — *by accident*, on a value the
platform supplied rather than the code. Real hardware under a hypervisor that
deliberately poisons unknown-reset registers produced a non-zero, non-NULL
pointer instead, and the first dereference walked into the poison.

So the guard was never doing the work it appeared to do, and no amount of
testing on the usual substrates could show that: **a guard that passes because
the environment happens to supply the value it checks for is indistinguishable
from a guard that works, until the environment changes.**

This is the reason the project keeps a real-silicon host in the loop, stated
concretely rather than as a principle.

**And it is the canonical shape of a fix that stops at one site.** The two entry
paths are in the same file, a few hundred lines apart, doing the same job for
different CPUs. The primary's version carried a comment explaining precisely why
the zeroing was necessary — which is what made it feel handled, and what stopped
the question being asked one screen further down.

**Derive and apply PAC keys.** Keys come from the counter, are derived once, and
are stored so every CPU can apply the *same* ones. Per-CPU keys would be
stronger and would break thread migration: a return address signed on one CPU
must authenticate on whichever CPU resumes the thread. Both routines are leaf
functions with no compiler prologue, because the routine that enables PAC cannot
itself have signed a return address with a key that is not yet loaded.

**Randomize and relocate**, then enable the MMU — see [[sub-kernel-kaslr]].

**Re-anchor and long-branch.** The stack pointer taken earlier is a load PA;
after the MMU is on, it must move to the high VA of the same physical memory,
before the identity map is retired later in boot. Then the target address of
`boot_main` is converted from PA to high VA and reached with `blr` — deliberately
`blr` and not `br`, because a call sets a different branch-type state than a
jump, and the compiler emits a call-shaped landing pad.

## Data structures

The image header is the first 64 bytes of `.text`: a branch, a nop, load offset,
image size, flags, and the magic. `image_size` covers BSS, per the boot protocol,
so a bootloader reserving that many bytes has room for the kernel's whole
footprint. The assembler cannot subtract two external symbols, so the linker
computes it as an absolute symbol.

The linker script fixes the layout the MMU later enforces: each section starts on
a page boundary so `.text` can be RX, `.rodata` R, `.data`/`.bss` RW with no
spillover — I-12 (W^X) begins as a *layout* property here, before any PTE
exists; its enforcement home is the MMU, unswept. The boot stack lives in BSS with a guard page immediately below it, so an
overflow takes a translation fault instead of quietly eating BSS.

Read-only metadata tables — the uaccess fixup table, the alternatives table and
its replacements — are placed inside `.rodata` with bracketing symbols. All three
use PC-relative offsets rather than absolute addresses, which is what keeps them
free of relocations and therefore independent of the slide: entry and target move
together, so the stored delta stays valid.

## Concurrency

None, and that is the point. One CPU executes this file's primary path; the
secondary trampoline in the same file runs one CPU at a time under a serialized
bring-up owned by [[sub-kernel-sched-smp]]. Every subsequent design that relies
on "single-CPU at this moment" is relying on where its call sits relative to
this.

**The trampoline's stores are not coherent with the primary's view, and that is
architectural rather than incidental.** It executes with the MMU off, so its
writes are Device-typed and go straight to the point of coherency, while the
primary is reading the same locations through a cacheable mapping. The two do
not see each other without explicit maintenance, so the online handshake is a
deliberate protocol — the flag isolated to its own cache line and published with
an explicit clean — rather than an ordinary shared variable. The subsystem's own
notes had recorded this as a caveat before it caused anything; it later did.

The same hazard bounds what the trampoline may *call*. A helper invoked across
the point where the MMU is enabled would push its call frame as a Device write
and pop it as a cacheable read of a possibly-stale line, so a routine used there
must be genuinely leaf, and the constraint is documented at the call rather than
assumed — with the compiler's output checked, since leaf-ness is a property of
what was generated and not of what was written.

**The vector base is installed here, before any C runs.** It had previously been
left at its UNKNOWN reset value until later in bring-up, which meant a fault
taken by a secondary in that window vectored somewhere arbitrary — the failure
mode being silence, since a wrong vector base does not report anything.

## Invariants enforced

- **[[inv-i21]]** — `SPSel` is set to 1 before the first stack write, on the
  primary and on every secondary, and `SP_EL0` is zeroed as a non-current bank.
  There is no path that runs kernel code at EL1t.
- **[[inv-i16]]** — the stub calls the slide chooser and long-branches to the
  result; the mechanism is [[sub-kernel-kaslr]]'s.
- **I-12** — established as section-aligned layout here; enforced as page
  permissions by the MMU. Recorded as a claim this area upholds rather than one
  it owns: the enforcement home is `arch/arm64/mmu.c`, not yet swept.

## Error paths

Deliberately mute. A wrong entry EL, an invalid secondary index, and a
`boot_main` that returns all reach the same halt loop with no output, because
every reporting channel needs something not yet established. The halt loop
carries a branch-target landing pad even though every current reach is a direct
branch — one instruction against the class of bug where a future indirect
dispatch into it faults.

An unsupported relocation type is the one loud failure: a breakpoint instruction,
chosen because there is no console and no exception handler yet, so the only
available report is to stop in a way a debugger can see.

## Performance

Not a factor; the whole stub is a few hundred instructions. The boot-time budget
is measured from a counter read taken as the very first act of `_real_start`,
before BSS is cleared — which is why the value is stashed in a callee-saved
register and written to memory only after the clear.

## Prosecution

- **The eret window.** Any hand-rolled path that sets `ELR_EL1` and returns to
  EL0 must mask all of DAIF across the window; this file's EL2 drop and the
  userspace-entry paths are the tree's instances. The rule's full statement lives
  with [[sub-kernel-exception]].
- **Bank confusion.** Every `mov sp` in this file must be preceded by an asserted
  `SPSel`, and every `msr sp_el0` must be executing at `SPSel=1` where `SP_EL0` is
  the non-current bank. A write to the wrong bank is silent.
- **PAC key uniformity.** Keys must be derived once and applied identically
  everywhere. A change that makes them per-CPU breaks migration in a way that
  surfaces as a corrupted return address, far from here.
- **PC-relative discipline.** Everything reached before the long branch must be
  addressed PC-relative; an absolute reference resolves to a link-time VA that is
  not mapped yet.
- **The stack re-anchor must precede identity-map retirement.** Both are in the
  sequence, in different files.
- **Every determinism step owed by the primary is owed by the trampoline.** They
  are two entry paths to the same state, in one file, and the trampoline is the
  one that gets forgotten. Anything zeroed, installed or asserted before C runs
  on the boot CPU must be checked against the secondary path explicitly — a
  comment on the primary explaining *why* it is needed is the thing most likely
  to make the omission feel already handled.
- **A guard whose passing depends on the substrate is not a guard.** The NULL
  check downstream of the thread pointer held on every emulator and on no real
  machine. When a check tests for a value the platform happens to supply,
  nothing in the test suite distinguishes it from a working one.
- **Nothing reached before the MMU is enabled may make a non-leaf call.** A frame
  pushed as a Device write and popped as a cacheable read is a silent corruption,
  and leaf-ness must be verified in the generated code rather than inferred from
  the source.

## Seams

- Cross-language constants are duplicated by necessity and frozen by convention —
  see [[seam-kaslr-link-va-unchecked]] for the one place the freeze is claimed
  but absent.
- The EL2 entry path is unexercised on every current target.

## Caveats

**Two literals are hardcoded in assembly because assembly cannot include a C
header** — the secondary stack slot size and the CPU-count bound. Both are
handled correctly, by the only technique available: the C header carries a
`_Static_assert` pinning its own constant to the literal the assembly hardcodes,
with a message naming the assembly file. That does not compare the two languages,
but it does guarantee that changing the C side fails the build with a pointer at
the other copy. This is the tree's idiom for the problem, and it is worth knowing
by name — because one place claims it and does not do it.

**`_torpor` is the halt loop's name**, part of the project's thematic naming.
`CLAUDE.md` still lists the rename from `_hang` as a proposal "held for explicit
signoff"; the tree contains no `_hang` at all. The rename landed and the record
of it did not.

**The first real-silicon failure was four stacked defects, and this file owns
only the root.** The uninitialized thread pointer is here; the recursion that
turned one bad dereference into physical-memory corruption is bounded in
[[sub-kernel-exception]]; the bring-up timeout that had never fired in its entire
history — and so froze rather than reporting — is in
[[sub-kernel-sched-smp]], which also records the decision to treat a partial
bring-up as fatal rather than continue on fewer CPUs than were asked for. A
reader tracing that failure needs all three; no single dossier holds it, and this
note exists so the trail does not end here.

## Provenance

Read from `arch/arm64/start.S` (704 lines) and `arch/arm64/kernel.ld` (255) in
full, 2026-08-02, during the boot sweep. Re-read 2026-08-16 at `efd83109` —
`start.S` now 798, the linker script unchanged; the delta is the secondary
trampoline's determinism fix and its instrumentation
([[chg-2026-08-16-boot-entry-trampoline]]). The linker script carries five
build-time assertions — link address, boot stack size, guard page size, and two
about the kernel image fitting inside the fixed page-grain mapping without
straddling a table boundary. Those two are the only mechanical protection against
a layout change that would silently place kernel bytes outside the region the MMU
maps at page granularity.
