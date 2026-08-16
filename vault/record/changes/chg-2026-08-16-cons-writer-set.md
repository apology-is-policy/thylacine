---
id: chg-2026-08-16-cons-writer-set
type: chg
title: "A role serializes only against writers who take it"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-cons, abi-boot-banner]
established: []
closed: []
opened: [seam-extinction-line-unserialized]
mirrors-checked:
  - "tools/test.sh"
  - "tools/smp-multiboot.sh"
  - "tools/test-cross-reboot.sh"
  - "tools/test-fault.sh"
  - "tools/ci-idle-gate.sh"
  - "tools/np3-bench.sh"
  - "tools/verify-kaslr.sh"
  - "tools/warp/boot-probe.sh"
  - "tools/interactive/lib.exp"
  - "tools/interactive/dap-nora.exp"
  - "tools/interactive/flood-174.exp"
  - "tools/interactive/freeze-172.exp"
  - "tools/interactive/ls-gfx-font.exp"
  - "tools/warp/quarry-wedge.exp"
  - "tools/stall-watch.py"
depth: rich
created: 2026-08-16
---
Three console commits, and each one falsifies a sentence this vault or the
source was carrying. Two of the three were carried *here*.

## The mutual exclusion that excluded the wrong set

The console has a writer role that makes a whole write atomic against other
writers. The dossier said so, correctly, and stopped one clause short of the
defect: **against other writers who take it.**

The role was given to the device write path and the syscall that routes through
it. The kernel's own direct emitters were never enrolled — and one of them
prints the tooling-ABI line every gate greps for. So a boot where the compositor
wrote the console concurrently produced the success banner woven byte-wise
through the compositor's output. The harness matched nothing and timed out
reporting no boot marker, on a guest whose same log shows login reached, both
users authenticated, and zero extinctions.

**A provably healthy system reported as a boot failure, by a correct string
emitted by the correct code at the correct moment.**

Found by a gate it failed, and read off both halves rather than inferred — the
tear's shape (byte-granular interleave) is exactly what the file's own comment
describes as the bug the role was introduced to fix. **The remedy was in place
and the population it covered was never re-derived.**

## A flush is a point in time; the hazard is an interval

The banner path already flushed the transmit ring before emitting, from an
earlier audit whose comment names this exact tear.

Flushing drains what is *already* queued. A peer that begins writing immediately
afterwards refills the ring while the caller is still mid-emit. **So the
half-fix was aimed at the wrong shape of thing** — it emptied the past when the
requirement was exclusivity across a duration.

Worth keeping because the flush is the intuitive move and it produces a system
that is *better* and still wrong, which is the hardest kind to notice: the tear
becomes rarer, so the next occurrence looks like a new bug.

## Two edges of the fix that bound what it buys

**It excludes the crash path deliberately.** Those emitters run on a dying
machine and must stay lock-free and bounded, so the extinction prefix keeps the
old delivery guarantee — none. **A serialization primitive that can park is
exactly wrong where parking may never return.** The consequence for the ABI note
is that its two frozen strings do not have equal integrity, which nothing there
said.

**It prefers a torn line to a dropped one.** If a death unwind interrupts the
park, the caller emits unserialized rather than losing the line.

## What the ABI note was missing, and why nothing there could have caught it

The boundary note for these strings is thorough about **value**: what may
change, who mirrors it, a derived check that fails rather than warns, a positive
control against an empty hit set.

Every one of those reasons about **who reads the string.** None covers the
string *arriving intact*. Its first paragraph even states the requirement —
"must appear on a line by itself" — and carries no obligation that produces it,
because that sentence reads as a property of the emitter and is not one. **It is
a joint property of the emitter and every concurrent writer of the same device**,
which cannot be established by inspecting the code that prints it.

So the mirror check could not have found this, and neither could a stricter
version of it. Recorded as a delivery obligation in its own right.

## The rule made me enumerate, and the enumeration inverted the result

The linter refused the note until the full mirror set was checked off. I nearly
argued my way past it — no literal changed, so no consumer is affected — and
that reasoning was correct and would have cost the best finding of the batch.

Classifying all fifteen mirrors by which literal each actually matches: **eight
match the now-serialized success line; fourteen match the still-unserialized
crash prefix.** The fix protected the string with roughly *half* the readership
of the one it left alone, and the crash path was excluded on the correct
grounds that a primitive which parks must never run on a dying machine.

The two costs are worse than a missing line, and are recorded on
[[seam-extinction-line-unserialized]]: a torn prefix demotes a real corruption to
the multi-boot classifier's *unclassified* bucket — its most expensive verdict,
reached from outside anything the classifier can see — and a torn message body
makes the fault gate report that a protection **did not fire** on a run where it
fired.

**A check earns its keep by being unskippable in the case where you are sure it
does not apply.** The rule was written for literal changes; the value it
returned here was from a change that touched no literal at all. Had it warned
instead of failed, I would have proceeded, correctly, past it.

## A claim that held because the window was narrow

The control file discards a half-assembled line on a mode change. The source
comment justified it and then asserted: *no current consumer flips mid-line;
the login program flips between completed reads.*

The login passphrase prompt flipped after emitting the prompt. A byte typed into
that window is **echoed** — the pre-flip mode still applies — and then
**discarded** by the very mechanism the sentence was defending. A rendered
prefix of a passphrase, and a silently truncated read, from one race.

**The claim held because the window is narrow, not because it was shut**, and
from inside the code relying on it those are indistinguishable. This dossier had
inherited the sentence verbatim.

The corollary is now stated as a contract the kernel cannot enforce: set the
mode *before* emitting the prompt that invites the input. A flip after the
prompt is a well-formed control write; nothing can reject it.

## The exit whose post-condition differed from its siblings

The receive holdback stranded at exactly one drain-loop exit — the scripture
called that state unreachable and it was reachable, when the last unit of the
budget is spent on a refusal whose recheck then lifts the pause.

What makes it a real defect is **where the byte lives**. A held byte sits in a
software variable, not the hardware queue, so it raises neither the receive
interrupt nor the receive timeout — and the hardware re-fire that covers every
ordinary budget-exhaustion exit does not cover this one. A keystroke that
vanishes and returns when the user next types, with no counter and no log line.

**The other exits were sound for a reason that does not generalize**: three
return with the pause standing, and the fourth is reachable only with nothing
held. So this was not a forgotten exit but an exit with a *different
post-condition from its siblings* — which reading the siblings cannot reveal,
and which is the argument for stating the post-condition per exit rather than
per loop.

## A severity rule, and a testing technique

Two prosecutors found the strand independently and graded it differently: P1
citing the architecture document, P2 citing only the code comment. The higher
grade was taken after verifying the quotation was verbatim. **A contradicted
comment is a bug; a contradicted binding document means the commit's own claim
is false.** The same defect earns a different severity depending on what it
falsifies — which is not obvious, and cuts against the instinct to grade by
consequence alone.

And the regression test is the more interesting artefact. The race is a peer
taking ring room between a lockless pre-check and an under-lock push — not
schedulable single-threaded. The hook does not attempt the timing: it makes the
*room* query report full while the *admission* query, which reads the count
directly, still reports space. **That divergence between two views IS the race.**

The general form: *to test a race you cannot schedule, construct the state
disagreement it produces rather than reproducing the timing.* The cost is a
production-inert hook living inside the machinery it validates — this file now
has two — where a refactor could make it non-inert with no test failing.
