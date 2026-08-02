---
id: chg-2026-08-02-console-sweep
type: chg
title: "vault sweep: the console and its front doors"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-cons
  - sub-kernel-devdev
established:
  - inv-i27
  - spec-cons-poll
  - lock-cons
  - lock-cons-tx
closed: []
opened:
  - seam-devdev-winsize-statless
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 15. Read from code: `kernel/cons.c` (1636 lines) and `kernel/devdev.c`,
plus the console-role primitives and the trusted-path transition in `proc.c`,
the receive back-pressure in `arch/arm64/uart.c`, and the syscall front door.
Two dossiers under `system/kernel/console-gfx/`, which had been declared-and-
empty since commit 0.

WHY THIS BATCH. Batch 14 closed the authority family down to one: **I-27, the
trusted path**, whose enforcement home is `cons.c` and `devdev.c` and nowhere
else. With this batch every invariant the security area's gates enforce has a
swept `guards` home, and the registry pass is genuinely unblocked rather than
blocked-on-a-dependency.

THE ORGANIZING FACT is two facts that meet: **the console's producer is an
interrupt handler that may not do the work, and its consumer is a trust
boundary.**

The first produces the shape. Everything the receive handler wants to do with a
byte is illegal in interrupt context — waking a poller needs a non-irqsave lock
that nests a wake, posting the interrupt note and performing the trusted-path
transition need the process table lock, waiting for room to echo needs to
sleep. So each is flagged under the console lock and relayed to a manager
kthread. That relay occurs FOUR times and is the one part of the console with a
model. Every remaining oddity is the same fact from another angle: the transmit
ring exists because echo may not block; the diagnostic emitters exist because a
bounded spin in a handler is still a spin; the locks are leaves because a leaf
is what a handler can take.

The second produces the rules — three console roles, an attention key that is a
line condition rather than data, and two front doors that gate identically.

THE THREE ROLES, AND WHERE THE RULE WAS LIVING. **attached** conveys elevation
and the right to mint a console fd by name; **owner** is the target of the
interrupt and window-change notes; **renderer** conveys the output drain and the
input feed and nothing else. Holding one never confers another.

What is worth recording is that this rule — the actual content of I-27 — is
stated nowhere in the tree. What exists is three separate comments, each
explaining why one particular collapse was *undone*: attaching the trusted
authority without making it the owner (because an interrupt posted to a
note-less login authority killed the trusted path until reboot); the transition
posting no note at all (because reusing the interrupt note as a courtesy
terminated init once that note became a real signal); the renderer being
refused the console data leaf (because reading input is exactly what the
display must not confer). Three bug fixes, three local rationales, and the
general rule distributed across them. [[inv-i27]] is the first place it is
written down as one statement — which is precisely the vault's purpose, and the
clearest instance so far of a fact that had no home rather than a wrong one.

The **medium** clause is the part most likely to be misread later: the
invariant is stated over an attention key and a trusted sink, not over a serial
port. And the reason the graphical path cannot simply reuse the renderer is
positional — on a graphical backend the renderer decodes the keyboard, so it is
already in the input path and holds every keystroke by construction. That is
why it is untrusted for elevation and why the trusted path stays on serial
until the kernel owns the keyboard.

NEW SEAM: [[seam-devdev-winsize-statless]] (task #19). `devdev_stat_native`
enumerates twelve of the thirteen leaf kinds; the geometry leaf falls to the
default arm, so `fstat("/dev/winsize")` returns -1. Three statements in the tree
say that is wrong — the Dev's vtable comment states the intent as *every* leaf
answering; the rationale beside the switch says a stat failure with the wrong
errno is FATAL to a real toolchain (the compiler's standard-fd fixup dies on it,
which is the defect that put the slot there); and the stat test's header claims
it covers "every leaf shape" while enumerating a hardcoded list of five that
predates this leaf. One statement says it is right: the geometry leaf's own test
comment records the absence as though it were the design. **The tree states the
rule and its exception in two comments that cannot both be true.**

Bounded today — nothing stats it, and the leaf is newer than the toolchain fix.
But the population it exists FOR is the one most likely to trip it: the leaf is
ungated precisely so an ordinary program that cannot mint a control fd can read
the terminal size, and ordinary programs stat what they open. The two prior
instances of this class in this tree, a pipe and a notes fd, were both silent
until a workload happened to put one on a standard descriptor, and one of them
killed concurrent build jobs with no diagnostic at all.

ONE ARC, TWO STATEMENTS LEFT BEHIND. The stat gap and the second finding are
not unrelated nits — they are the same arc's tail in opposite directions.

The window-size work added a verb to the mode-line renderer whose bound check
reserves the worst case unconditionally, raising the minimum caller buffer from
34 bytes to **54**. The header contract still says 34. A caller that allocates
exactly the documented minimum gets zero bytes back, every time, forever. It is
fail-safe (nothing truncates; the one production caller passes a comfortable
buffer), and what makes it worth recording is that **the test already encodes
the correct behaviour** — it asserts that a 40-byte buffer yields zero — so the
header and the suite state contradictory contracts and only the header is what
a new caller reads. The standalone geometry renderer has the same drift one byte
wide: header says at most 21, its own guard and comment say exactly 20.

So: adding a verb grew a documented contract past its statement, and adding a
leaf missed a table. Both are downstream statements that did not follow their
arc, and both are invisible to the build.

THE QUIET REGISTRATION. `devdev`'s leaf set has four registrations that fail
LOUDLY (the kind, the name table, the read dispatch, the write dispatch) and one
that fails QUIETLY (the stat switch). A leaf missing from read or write is dead
on arrival; a leaf missing from stat walks, opens, reads and writes perfectly
and fails only when someone stats it. That is how this one got in, and it is the
same shape batch 14 recorded for the introspection Dev's read whitelist, where a
missing registration leaves a file that resolves fine and reads -1 forever. Two
consecutive batches, two Devs, the same silent-registration class.

THREE SINGLE-WAITER RENDEZ, THREE DIFFERENT ARGUMENTS. The console has three,
and none of them is safe for the same reason: the two readers by an explicit
busy guard that REFUSES a second reader rather than parking it (refusing beats
racing into the extinction), and the transmit room by the writer role's
exclusivity — only the role holder pushes-with-wait, so only one thread can ever
wait. The code says outright that if a second waiter is ever introduced it must
become a wait list. Linked to the standing hazard note rather than restated.

THE HEADER BLOCK IS ARCHAEOLOGY. `cons.c` opens by describing a write-only
console whose reads return end-of-file and whose control file is "held until a
later phase" — the state of the world several arcs ago; all of it has since
landed. The per-section comments throughout the file are current and unusually
good. Recorded because the pattern is now familiar: the drift lives in the
oldest, most-summarizing prose, not in the comments beside the code.

REGISTRY TAIL. Per batch 14's finding that the registry pass IS the sweep's
tail, this batch minted its own dependencies and stopped: [[inv-i27]],
[[spec-cons-poll]], [[lock-cons]], [[lock-cons-tx]]. The two lock notes earn
their place because the console's whole soundness argument IS its lock
discipline — that both locks are leaves is not a detail, it is the deferred
design restated. The drain lock is deliberately NOT minted: it is the console
lock's discipline applied to the output ring, so it is described there rather
than forked into a second note (R1).

I-2, I-5, I-6, I-23, I-25 and I-34 remain mintable and remain deliberately
untouched — the same scope line as batch 14. With I-27 minted, the security
area's cross-cutting list is complete and the registry pass has no remaining
dependency on the sweep.

LAYOUT. `console-gfx/` was declared-and-empty since commit 0, so both dossiers
went there with no schema change. `devdev` sits here rather than in the
unswept `devices/` because the file's substance is the console's gate story,
and splitting that across two areas would fork it; the MOC carries an explicit
scope note saying so, and pointing the hardware drivers at `devices/` and the
compositor at userspace.
