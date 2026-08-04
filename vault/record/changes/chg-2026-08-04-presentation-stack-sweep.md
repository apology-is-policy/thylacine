---
id: chg-2026-08-04-presentation-stack-sweep
type: chg
title: "The presentation stack — eighteen tests that cannot compile, and a fix that reached one sibling again"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - sub-aurora
  - sub-libtapestry
  - sub-cornucopia
  - sub-diorama
  - inv-i43
  - moc-userspace
  - moc-userspace-runtime
  - moc-userspace-shell-tui
established:
  - sub-aurora
  - sub-libtapestry
  - sub-cornucopia
  - sub-diorama
  - inv-i43
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 52: the console renderer, its two libraries, and the synthetic Linux
world — 7399 lines across nine files, four dossiers, one invariant minted.

**THE AREA SPLIT THREE WAYS, AND THE HANDOFF'S PREMISE WAS WRONG.** It
recorded diorama as "a demo" and predicted the same plane split batch 51
used. The libraries do split that way — [[sub-libtapestry]] and
[[sub-cornucopia]] are client code over a validated boundary, so they join
the runtime area on the plane rule its organizing fact already states. But
**diorama is not a demo**: it is `/sbin/diorama`, a device-less 9P server
posting `/srv/diorama`, boot-spawned and boot-gated, with a Conn/fid table
and a poll loop — the FIFTH native server, and the userspace area's server
list said four. And [[sub-aurora]] is neither a library nor a service; it
went to the shell/TUI area, whose stated organizing fact is a *range of
risk*, because a console renderer extends that range past the raw-mode
handoff to a console role.

The name was the whole error. Reading the file settled it in nine lines.

**F1 — EIGHTEEN TESTS CANNOT COMPILE, AND THE FILE WITH HALF OF THEM SAYS
THEY RUN (task #153).** aurora is unconditionally no-standard-library, so
its suite has no harness. Ground truth rather than inference:

    $ cargo test -p aurora --no-run
    error[E0463]: can't find crate for `test`  --> aurora/src/vt.rs:1161:5
    error: could not compile `aurora` due to 19 previous errors

Three of its four modules say exactly that — and two name the fourth
explicitly ("alongside vt.rs's module", "like render.rs/vt.rs"). A fourth
statement comes from outside the crate: [[sub-cornucopia]], which DOES gate
the attribute on not-under-test, names aurora as its counter-example. So
four independent statements agree, and the dissenter is the interpreter,
holding nine of the eighteen over the most exposed surface in the batch —
the byte machine that eats every byte any program writes to the console.

Two of those nine are named security regressions that have never executed:
the fix for an embedded newline laundering a compositor-tier setting past a
single-token allowlist, and the fix for an out-of-bounds erase at a
deferred wrap — reachable from any console writer, and under abort-on-panic
a dark console.

**F2 — A FIX REACHED ONE SIBLING AND STOPPED, FOR THE THIRD TIME IN THIS
SWEEP.** A full connection table plus a pending connection keeps a listener
perpetually readable, so an accept loop that merely skips the accept turns
at full CPU forever. [[sub-diorama]] drops the listener from its poll set
while full and cites the audit finding that taught it. [[sub-corvus]] does
not, and its comment calls the situation a benign deferral — filed
yesterday as task #149, and today's reading upgrades it from a hypothesis
to a known-and-fixed bug class one server still carries.

**F3 — A CLEANUP HELPER SKIPPED EXACTLY ONCE (task #152).**
[[sub-libtapestry]]'s construction path defines a closure whose only job is
closing the descriptors opened so far, uses it on every failure but one,
and that one sits BETWEEN two that use it — a question-mark operator where
its neighbours call the helper. Five descriptors leak, and with them the
weave mapping, whose lifetime is a descriptor that is never clunked. The
caller amplifies: aurora's bounded connect retry runs it twenty-five times.

**F4 — A SAFETY COMMENT NAMES THE ONE FIELD ITS PARSER SKIPS (task #154).**
The same crate's only unsafe slice construction states "slot stride >=
width * height * 4" as established. The geometry reply carries five fields;
the parser validates four of them, and the fifth is that one. The property
holds — the compositor computes a page-rounded row span — so it is a
trust-the-server fact written as a checked one, which is [[sub-netdev]]'s
shape at a different joint.

**ONE NEAR-FINDING DISSOLVED AND ITS TWIN SURVIVED, WHICH IS THE USEFUL
PART.** ARCHITECTURE section 28 runs I-40 then I-42, and diorama's whole
design rests on I-43. Two apparent gaps — and the closing paragraph of that
section explains ONE of them: I-41 "remains reserved in its own doc, its
row landing at AG-2's SB-1 — so the table above jumps I-40 to I-42".
Deliberate, documented, scheduled. It says nothing about I-43, which is not
reserved but ENFORCED: stated in VIVARIUM section 6.2, restated in the
server's first eight lines, and formally prosecuted by the vivarium audit
round, which concluded it holds structurally. So the register that
enumerates the proof obligations omits a live audited invariant while
explicitly accounting for the neighbour whose absence is intentional (task
#155). Checking both is what separated them; assuming either would have
been wrong.

**AND CHECKING MY OWN FILED TASK CORRECTED IT — the batch-50 discipline,
second outing.** Task #144 claimed the compositor's visible-only frame
gating is "documented nowhere — not in source, not in the dossier". Half
false: aurora's own bounded-wait helper documents it thoroughly, as the
stated reason for a design decision, including the rejected alternative.
The producer still carries no comment, so the residual gap is real and
sharper than filed — **the constraint is written down by the party that
suffers from it, not the party that causes it**, so a change at the
producer has no local warning and the reasoning lives one crate away.
The task now says that.

**[[inv-i43]] IS MINTED HERE**, because diorama is its worked example and
its only enforcer — the batch-51 pattern (mint when the sweep reaches the
first enforcer). Its content is worth the note: a compatibility layer may
confer a foreign system's ABI *shape* and nothing else, and the failure
mode is the confused deputy, which is easy to reach by accident because
every individual step toward it looks like an improvement — a file that
would be empty gets filled, a value with no source gets a reasonable
default, a caller that wants to ask about another process gets to name one.
Its `blind-to` clause is the honest part: nothing mechanical checks the
*provenance* of a render, so a new file that quietly read something its
client could not would pass every test.

**WHAT WAS SOUND.** [[sub-diorama]] carries the strongest proof position in
the userspace tree, and it is neither of the two answers the sweep has been
finding: ~500 lines of assertions that run before the service is posted and
GATE THE BOOT — the tree walk, three qid families and their mutual
non-aliasing, every parser, the bounded renderer, the maps translation, the
dead-peer renders, and the negative assertions protecting the invariant
(including one whose failure string names the leak it prevents). No VM
state, no harness, no CI. Its security reasoning is equally good: the one
file whose authority would diverge from its client's is ABSENT rather than
gated, and the rejected alternative — replicating the kernel's owner check
— is recorded with its reason, that it would turn a component whose whole
design property is having no policy into a policy point.

So this batch holds three different answers to "how is this proved": tests
that run (cornucopia), tests that cannot compile (aurora, libtapestry), and
a boot-gating in-guest selftest (diorama). The proof-position divergence
first noted at batch 51 is not a two-crate accident; it is a spread.

LEDGER, read off the rendered view. Corpus 864 -> **869**. Coverage 281 ->
**290 owned of 422**, 66% -> **68%**; unswept lines 30902 -> **23503**.

Both deltas are exact: +9 files is the batch's nine source files, and
-7399 lines is their line count. That is the second batch running where
the line delta equals the batch's own work, and the reason is the same —
the baseline was read after confirming the tree was level with main, so no
merge moved it underneath. The numbers were read, not predicted; after four
consecutive batches of predicting them wrong for four different reasons,
the rule needs no further demonstration.
