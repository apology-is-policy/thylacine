---
id: chg-2026-08-04-warden-sweep
type: chg
title: "The warden — an unchecked computation whose only audit is a log that under-reports it"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - sub-warden
  - moc-userspace-boot-chain
  - moc-userspace
  - moc-userspace-runtime
  - inv-i34
established:
  - sub-warden
  - moc-userspace-boot-chain
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 48: the warden — the Menagerie hardware broker. One file, 784 lines.
The third slice of 57d, and the consumer of both libdriver halves.

**A NEW AREA, AND THE REASON IT IS A NEW AREA RATHER THAN A FILING
CONVENIENCE.** The warden is a program, so `runtime` (explicitly libraries,
not programs) was wrong; and `services` has an organizing fact — the shared
9P-server template — that the warden does not share, so filing it there would
have been the batch-46 trap exactly: an area's organizing fact failing to
cover the note just placed under it. [[moc-userspace]] has named a boot-chain
area since it was written. It now exists, with the warden as its first
member.

Its organizing fact: **every member is spawned holding an authority whose
whole purpose is to be given away, and is finished when it holds less than it
started with.** The consequence is what makes the area worth reading — *an
area whose job is delegation cannot be judged by what it protects, only by
what it computes.* And it is where the capability rules are weakest by
construction: the kernel bounds a conferral by the conferrer's own holding,
which is vacuous against a conferrer holding everything, and being early is
what makes you such a conferrer. **The fact is written from one member and
says so**; expect it re-derived, not extended, when init, the coordinator and
the login gate land.

**THE THREE QUESTIONS THE HANDOFF CARRIED ALL RESOLVED, AND ONE OF THEM
AGAINST THE DOCUMENTATION.**

- **Is `abi` compared here?** No — and this settles task #134 at its
  consumer rather than merely in the library. The warden parses its manifest
  database and pushes each entry into the bind set without looking at the
  field. Two documents name *the warden specifically* as the thing that
  refuses a stale version: the library's own doc comment, and the design
  document's argument that a stable driver ABI becomes desirable under the
  capability-sandbox inversion. The field is dead end-to-end.
- **Does the gather fold re-check membership?** Yes, and deliberately: the
  library re-tests every folded node against the manifest rather than
  trusting the caller's matching, with a comment saying why. Sound.
- **Is the impure half faithful to the pure contracts?** Yes — confirmed from
  the consuming side this time rather than the library side. The restart
  ladder drives the pure decision function with its published limit; the
  readiness reader sizes its buffer to the line cap and treats garbled as
  give-up.

**F1 — THE AUDIT LINE REPORTS A COUNT FOR EVERY AXIS GATHERING CAN GROW,
EXCEPT THE ONE IT DOES GROW (task #140).** Each bind is logged with its
granted resources. Three axes print a *length* — honest under folding. The
fourth prints a scalar, the primary bus function, and the remainder is
printed nowhere in the program.

Measured on the reference machine: the compositor gathers four bus functions
and the line names one. The only trace that anything was folded is that its
interrupt count reads four; the device label printed is the first node's, so
the line is indistinguishable in shape from a single-device bind.

The grant itself is correct in both directions — the kernel receives all four
and the driver is told about all four. **What is wrong is the record**, and
the record is not decoration here: this is the one computation nothing
downstream re-derives, the file header calls it "the auditable grant", and
the design document promises the user sees exactly what a driver was given.
The root is inheritance: the format string was written for the per-node path,
where that axis is at most one, and carried into the gather path unchanged —
which is also why the under-report is exactly co-extensive with gathering,
since the folded field has one writer.

**F2 — TWO OF FIVE TERMINAL OUTCOMES ARE ABSENT FROM THE SUMMARY, NOT EVEN AS
FAILURES (task #141).** The closing line reports four buckets and invites the
reading that bound equals the sum of the rest — which holds only because the
bound counter increments *after* a successful grant. A device that matched a
driver and whose grant could not be computed logs one line and moves no
counter, on either the per-node or the gathered path.

Not reachable on the reference machine, where no device is resource-rich
enough to overflow a grant. Reachable **by configuration** on the gather
path, which accumulates across devices: attaching enough matching functions
overflows the fold, and the compositor silently never starts behind an
entirely green summary and a zero exit.

The disposition is genuinely arguable — an ungrantable device may well be
soft, like a driver that exhausted its restarts. What is not arguable is
being outside the tally, which makes the summary say something *untrue*
rather than something debatable.

**THE TWO FINDINGS ARE ONE SHAPE, AND IT IS THE SHAPE THIS AREA WILL KEEP
PRODUCING.** Both are the report being narrower than the action. On a plane
where nothing re-derives the arithmetic, the log is not a diagnostic — it is
the audit — so a gap in it is a gap in the invariant's only external
evidence. That is now written into [[inv-i34]] alongside the fourth leg it
belongs to.

**THE COUNTERWEIGHTS ARE A STRUCTURE INSTEAD OF A CHECK, AND A DISTINCTION
BETWEEN TWO KINDS OF HARDWARE FAILURE.** What makes the unchecked grant
trustworthy is not a guard anywhere but a shape: one value computed per bind,
with both the kernel authority and the driver's description of it derived
from that one value, recomputed per restart. Authority and account cannot
drift because there is only one of them — which is the *reason* F1 stings,
since the one place the account is re-typed by hand is the one place it is
wrong.

And the soft/hard split is the right call, made explicitly: a driver that
crashed out its restarts leaves its own device unavailable while the system
is fine, and must not fail the boot; only a structural failure — a missing
binary, an unspawnable child — is fatal. Init keys the boot verdict on the
exit code, so this distinction is load-bearing rather than cosmetic.

**THREE THINGS RECORDED AS CAVEATS RATHER THAN FILED, EACH BECAUSE
MEASUREMENT SAID SO.**

The bus enumerator's granted window is a **convex hull** over every slot, and
its stated safety argument is about capacity (the slots outnumber the windows
an allowance can hold) rather than containment. Anything lying *between* two
slots would fall inside the grant — to the least-trusted recipient of any
grant this program makes. On the reference machine the slots are exactly
contiguous and the hull is exact, verified against a boot log, so this is
written down rather than filed; a board that interleaves them would want the
cheap check that no other discovered device's window intersects the hull.

The **readiness contract has a second consequence it does not state**: after
readiness the broker closes its read end and walks away, so a persistent
driver writing a second line writes into a pipe with no reader. The stated
reason for the contract covers only output *before* readiness. Every driver
today honours it — checked, the network daemon signals last and says nothing
after — so it has never bitten.

And one failure classification **has two producers with a comment for one**:
the unobservable-driver case is documented as unreachable because every
driver gets a pipe, which is true of one way in and not of the other.

Not filing these is the batch-47 discipline applied in the same direction it
was learned: the hole I was confident of last batch was not there, and
checking is why I did not file it. Here three candidates were checked and
each dissolved to "true today, worth knowing why".

LEDGER, read off the rendered view rather than predicted. Corpus 850 ->
**853** (a dossier, an area MOC, and this note). Coverage 265 -> **266 owned
of 421**, 62% -> **63%**; unswept lines 44264 -> **43480**.

**The unswept delta is 784 — exactly the file's own length, to the line, with
no residue.** Main moved between batches for the third time in nine, but its
commit touched only in-kernel test files, which sit in the harness-excluded
set; so for the first time the ledger moved by the sweep alone. That is
itself a check passing: the exclusion is a claim about which files the
coverage figure is *about*, and this is the first batch where a main-track
commit could falsify it and did not. Fourth batch running this arithmetic,
fourth distinct behaviour — matched, disagreed-and-explained, matched,
moved-by-the-sweep-alone.

**AND THE MERGE-FALSEHOOD DUTY RAN, WITH A THIRD OUTCOME.** Main's arriving
commit was the bounded-wait conversion across the in-kernel test suite. It
touched no file this vault owns, so nothing on the Present plane could have
become false — but it *did* write 62 further lines into a reference document
this branch has stubbed, which is now the **third** occurrence of that
tripwire in four days (task #119). Folded into the stub's pending block as
items (4) and (5), and the task rewritten: at three occurrences the stopgap
is the thing generating the work, and the honest fix is to give the in-kernel
runner a dossier rather than to keep waiting for the kernel sweep to reach
it. Both new items are cross-cutting test *methodology*, and one of them —
that a bounded wait must match its enclosing function's failure convention —
also records a kernel fact that belongs with the process model.
