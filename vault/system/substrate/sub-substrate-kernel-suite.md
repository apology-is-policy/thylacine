---
id: sub-substrate-kernel-suite
type: sub
parent: moc-substrate
title: "The in-kernel suite — what the boot log says, and what a reader may conclude from it"
code:
  - kernel/test/test.c
  - kernel/test/test.h
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
updated: 2026-10-07
---

## Purpose

`sub-substrate-gates` owns the gate's half of this story: which boot counts as a
pass, how a multi-boot run classifies a non-pass. This dossier owns the other
half -- what the kernel itself EMITS while the suite runs, in what order, and
therefore which readings of a captured serial log are sound.

Every gate in the fleet consumes that text, and so does every ad-hoc parser an
agent writes mid-investigation. It had no owning dossier, and the cost of the
gap is on the record: a parser written against a reasonable belief about the
format read 1749 of 1836 verdicts and reported a test that had FAILED as absent,
while 87 PASSING verdicts in the same green boot had been invisible to the same
pattern all along.

## Contract

What a reader of the boot log is entitled to assume:

- The number of tests is derivable from the source without booting: it is the
  number of registration rows before the table's sentinel.
- Every test that runs produces exactly one verdict -- `PASS`, or `FAIL: ` with
  a message -- AFTER the line that announces its name.
- A verdict belongs to the most recently announced name, not to the line it sits
  on.
- The suite prints a summary tally with its own verdict word, whether it passed
  or failed.
- Every kernel-printed line ends CRLF.

What a reader is NOT entitled to assume: that a name and its verdict share a
line; that a failing suite is cut short before its tally; that an absent
`[skip]` line means the probe set was present; that the first `  tests:` match
in the log carries numbers.

## Mechanism

**The registration table is the denominator.** `g_tests[]` is the single
registration site, declared in `test.h` and defined in `test.c`. The runner
walks it until it meets a row whose function pointer is NULL -- the table ends
in an explicit sentinel -- so the test count is the number of rows before that
sentinel. Derive it; do not carry it in prose. A census that counts
registration rows and compares them with the suite's own total has two
independent routes to one number, which is the only form in which a count is
evidence.

**But counting those rows out of the log needs the conjunction of two markers,
not either one.** Measured on a green boot whose registration count is known:
the `[test] ` marker alone appears 1837 times, because the runner prints a
NON-TEST summary line (the yield-waits report) behind the same marker. The
` ... ` separator alone also appears 1837 times, because a userspace probe line
happens to contain it. Lines carrying BOTH are exactly the registration count.
Two decoys, from unrelated sources, each inflating a census by exactly one --
and off-by-one is the error least likely to be noticed, because the number still
looks right.

**A verdict is a STATE in the log, not a line in it.** The runner prints the
indented `[test] ` marker, the name, and ` ... ` BEFORE calling the test
function. The verdict is printed only after that function returns. Everything
the test emits in between lands between a name and its verdict, and three
sources routinely do:

- `test_fail` records the failure on the current test and then calls
  `sched_dump_runnable` immediately, on the failure path only -- the instrument
  that cracked a scheduler-quiescence bug, and worth its cost. So a FAILING test
  prints its name, then an all-CPU runnable dump, then `FAIL: <msg>`: three
  lines or more, never one.
- The runner itself interposes a note before the verdict when a test leaves a
  counted condition behind, such as the proc-wait timeout report.
- `test_soft_warn` prints a `[SOFT-WARN]` line, with its own newline, from
  inside the test, and does not fail it.

So a reader must track the pending name from the `[test]` marker and resolve it
on the first following verdict.

**The tally is printed by the boot, not by the harness.** `boot_main` emits the
summary; the harness only counts. The prefix `  tests:` appears TWICE in a
passing boot -- first as a bare header before the run, with no numbers after it
-- so a reader that takes the first match gets the header. The tally itself
carries its verdict word: `<passed>/<total> PASS`, with a parenthesised
soft-warn count appended when any fired, or `<passed>/<total> FAIL`. A
production build compiles no suite and prints `  tests: DISABLED` with a reason:
the same prefix, no numbers, no verdict word.

## Data structures

`struct test_case` (in `test.h`) is one row: the name as it appears in the log,
the test function, and then the row's own VERDICT -- a failed flag and a failure
message. Those last two are OUTPUT slots written by the harness, not
configuration: the `false, NULL` that ends every registration row is their
initial value, which is why a new test is registered by extending the array and
needs no constructor and no options. The runner holds a pointer to the row it is
currently running, and that is how `test_fail`, called from arbitrary depth
inside a test, attributes a failure without being passed anything.

(This dossier's first draft called those two fields "dispatch fields the runner
uses to decide how to invoke it". That was wrong, and it was caught by reading
the header rather than inferring from the registration rows' shape -- a comment,
or a dossier, can be true about the wrong thing.)

## Concurrency

The suite runs on one thread, in table order, in the boot's own context -- there
is no parallel test execution to reason about. Concurrency enters only as
SUBJECT: tests that start threads, and the runnable dump that `test_fail` prints
across all CPUs. Two consequences for a reader. Output from a test's own threads
can interleave with the harness's lines, which is a further reason a verdict
cannot be assumed to sit on its name's line. And the kernel's own CRLF applies
to every line regardless of which CPU printed it.

## Invariants enforced

None of the section-28 invariants is enforced here; the harness is an observer,
not a mechanism. The property it does carry is weaker but load-bearing for every
gate: **one announced name yields exactly one verdict, and the pair is ordered**
-- name first, verdict after. A parser may rely on that order and on nothing
else about adjacency.

## Error paths

`test_fail` is the only failure channel: it marks the row, stores the message,
and dumps the runnable set. A test that returns without calling it has passed,
and that is the harness's single most consequential behaviour -- see Caveats.

A failing suite is NOT cut short. The runner completes the whole table, the
summary prints the full tally with `FAIL`, and only then is `extinction` raised
with the suite's name. So a red boot carries a usable tally, and the kernel's
own failure count can be cross-checked against a reader's parse of the
individual verdicts -- two routes, one number, on the red leg as much as the
green.

## Performance

The failure-path dump costs nothing on a passing run and so cannot detune
timing-sensitive tests. The suite's cost is otherwise the sum of its tests; it
runs inside the boot, so it is on the critical path of every gate boot, which is
why a gate that only needs a verdict reads the log rather than re-running.

## Prosecution

The adversarial questions this surface deserves, each of which has been answered
wrongly here at least once:

- Does the pattern match a name and a verdict on one line? Then it is wrong
  about failures AND about a measured 87 of 1836 passing verdicts.
- Is the pattern `$`-anchored? Then it matches nothing, silently, because every
  line is CRLF.
- Does the reader report a parsed total, and does that total equal the
  registration count? A short parse is a refusal, never a verdict.
- Does an unresolved pending name exist at end of log? That is a reading
  failure, not an absent test.
- Does the reader cross-check the kernel's own tally? It is free, independent,
  and present on red legs too.
- Does the gate assert zero `[skip]` lines? If not, a missing image file turns
  a real test into a green no-op.

## Seams

The log text is the seam, and it is consumed by `tools/test.sh` (the boot
verdict), by the multi-boot classifier, and by any investigation-time parser.
Only the boot banner and the `EXTINCTION:` prefix are binding tooling ABI; the
`[test]`, `[skip]`, `[SOFT-WARN]` and tally lines are prose, so a consumer
should key on their SHAPE -- including the verdict word -- rather than on a
prefix alone, and a change to any of them should be looked for in consumers
before it is made.

## Caveats

**A skip is a pass, and it is a live trap, not a documented convenience.**
Sixteen test files print an indented `[skip]` line and return when a file they
need is absent from the ramfs. A test that returns without failing is counted as
a pass, so a renamed or unstaged binary converts a real test into a green no-op
and the suite's own tally cannot tell you. The skip exists for a fresh checkout
without userspace; a gate image always carries the probe set. A gate that cares
therefore asserts ZERO `[skip]` lines against a baseline that had none -- a
property of the IMAGE, which must be stated as such. Tracked as an open defect.

**This dossier covers the harness, not the tests.** `test.c` and `test.h` are
the harness. The individual test files are a separate and much larger surface --
137 files, of which 26 are claimed by the dossiers of the subsystems they
exercise and the rest are unclaimed. That gap is pre-existing and tracked with
the standing dossier backlog; nothing here closes it, and this dossier must not
be read as covering it.

## Provenance

Authored 2026-10-07 from the source, after the private-owner red legs exposed
that no dossier described the format every gate parses. The specific readings
recorded here -- the eager name print, the failure-path dump between name and
verdict, the 87 split passing verdicts, the CRLF, the tally surviving a failing
suite -- were measured against two real captured boot logs, a green one and a
mutant one, not inferred from the source alone. The skip trap predates this
dossier and is carried from the open-defect record rather than discovered here.
