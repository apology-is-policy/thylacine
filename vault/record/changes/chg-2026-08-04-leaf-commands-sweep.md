---
id: chg-2026-08-04-leaf-commands-sweep
type: chg
title: "The leaf commands — one rule enforced by construction, one delegated by convention"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - moc-userspace-tools
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
  - sub-prowl
  - sub-net-clients
  - sub-mechanism-drivers
  - moc-userspace
established:
  - moc-userspace-tools
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
  - sub-prowl
  - sub-net-clients
  - sub-mechanism-drivers
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 53: the fifty-one coreutils and their library, the process monitor,
the standalone network clients, and two one-file mechanism drivers —
13,057 lines across 72 files, six dossiers, one new area. **The last
userspace batch**: after it `usr/` is swept but for the ports plane, the
prover corpus, and two stubs.

**THE AREA IS NEW AND THE ORGANIZING FACT IS MEASURED, NOT ASSUMED.** The
scope note asked whether the shell/TUI area owns these or a new one does.
Neither shape decides it — what decides it is that these programs are
**leaves**: each does one bounded job and holds nothing afterwards. No
session, no console role, no device, no capability, no peer outliving the
process. The shell/TUI area's members *mediate* — the shell between a
person and everything, the editor between a person and a file, the
renderer between a person and the console. So [[moc-userspace-tools]] is
its own area, and its dossiers say "composes with" in every invariant
section and "enforces" in none: the least privileged code in the tree is
also the code a person touches most.

That makes the interesting question not soundness but **which disciplines
propagated across fifty-odd independent implementations of one shape** —
and this area answers it both ways at once, from the same library, in the
same crate.

**IT PROPAGATED PERFECTLY WHERE IT WAS STRUCTURAL.** The rule
([[sub-coreutils-lib]]) is that colour belongs on presentation and
diagnostics and never on a payload another program reads, because a
coloured payload corrupts `tool | tool`. Measured across all fifty-one
binaries: exactly fifteen link the colour modules and exactly thirty-six
do not. The partition is *exact* — and it holds because a program that
never names the module cannot emit an escape byte. Authority by absence,
applied to output cleanliness, and stronger than a check: a gate can be
forgotten at one call site, a missing import cannot compile.

**AND IT FAILED COMPLETELY WHERE IT WAS DELEGATED (task #156).** The same
library hands "is stdout a terminal" to each binary, correctly, because
answering it needs a syscall the pure modules do not have. All fifteen
callers then wrote

    fn stdout_is_console() -> bool { true }

Fifteen identical copies. So `--color=auto` — the one flag whose entire
purpose is to enforce the rule above — means "always", everywhere, and the
fix now costs fifteen edits.

The blocking reason two of them cite is a device-class syscall reserved at
slot 80 and never built. But the mechanism shipped under another name: the
console gained a stat contract with its own identifying bit, deliberately
disjoint from the pseudoterminal's, and **the shell already performs this
exact probe** — stat the descriptor, check the character-device mode, test
the bit. Thirty lines, one crate away. The same shape as ut declining
`export` for want of an environment array while the environment is a
filesystem: a feature parked on a mechanism that arrived under a different
name.

The pair is the lesson worth keeping: same library, same authors, one rule
enforced by construction and one by convention, with exactly the outcomes
those two choices predict.

**A LIMIT NO CLIENT CAN SEE, RECORDED IN A FIELD AND THEN DISCARDED (task
#158).** [[sub-prowl]] prints `procs: N`, the count it parsed, read
naturally as the count that exists. The kernel's process-table renderer
stops when its 2 KiB buffer fills, at roughly thirty processes — and it
*knows*: `struct procs_fmt_state` carries a `bool overflow` set at FIFTEEN
distinct points, one per field that would not fit. Then `format_procs`
returns the byte count and drops it. Fifteen writes, zero reads.

So a truncated read is byte-indistinguishable from a complete one — no
marker, no count, no short read — and **prowl cannot do better**; the fix
is not in this area. prowl's own comment attributes the truncation
correctly to a kernel pagination seam, which is right and does not close
it. The workload that exceeds thirty processes is a parallel build, which
is exactly when a person opens a process monitor: it under-reports
precisely under the load it exists to observe.

**THREE MIRRORS OF ONE LIST, TWO MISSING THE SAME ENTRY (task #159).** The
shell resolves a bare command against `["/bin/", "/", "/goroot/bin/"]`.
login seeds the environment variable with `/bin:/goroot/bin` — under a
comment naming both endpoints and silently dropping the middle — and the
shell's own completion index omits it too (already filed). `which` reads
that variable, and its header says outright that the two "stay in sync ...
drift is a bug".

Reachable today: root-level binaries exist (the boot probes are spawned by
root-anchored path, which is why the resolver lists `/` at all), so typing
`storm` WORKS, `which storm` says not found, and Tab colours it red as
unknown. One truth, two lies, three tools — and every copy documents
itself as mirroring the others, which is exactly what made the drift
invisible.

**A TEST RECIPE THAT FAILS, IN THE FILE THAT TEACHES IT (task #157).**
coreutils' crate front door gives its host-test command without the flag
that drops the runtime dependency; run as written it compiles the runtime
for the host and dies in the startup assembly. With the flag it passes —
fifteen tests over the four pure modules, verified. The manifest one file
away describes the same procedure correctly. Batch 52's aurora finding at
lower stakes, and the same instruction: **a source comment's claim about
its own test story needs checking, and the sibling is often the one to
trust.** (Same file, second stale claim: "a 16-tool suite"; there are 51.)

**WHAT WAS SOUND, AND A FOURTH ANSWER TO "HOW IS THIS PROVED".** The
network crates ([[sub-net-clients]]) each carry a deterministic,
peer-independent self-test that gates the boot while the live path only
logs — because whether a real server answers is host-dependent and
asserting on it would flake. The time client's is the sharpest in the
tree: spawned unelevated, it asserts that stepping the clock is DENIED,
and a non-denial fails the boot. A privilege-regression detector written
as a positive assertion.

[[sub-mechanism-drivers]] gives the fourth answer: two programs whose
*execution is the assertion* — a pseudoterminal session host and the
concurrent ring stress harness five audit closes owed, run for real under
the multi-processor matrix. Here the absence of unit tests is a correct
judgement rather than a gap, and the distinction is worth having stated.

So this area displays all four proof positions at once: tests that run
(the pure library half), tests that cannot compile (every binary),
boot-gating self-tests (the network crates), and running-for-real (the
drivers). The divergence the sweep has found crate by crate since batch 51
is not a scatter of accidents — it is a spread, and one line of crate
configuration is the whole cause.

Two smaller notes kept in the dossiers rather than filed: `grep` gates
colour two different ways in one file (the working one gates the *data*,
so its unconditional emitter is safe but cannot express its own
precondition), and the ring stress program calls itself a harness in its
first line while the coverage census counts it as production — the visible
edge of an exclusion rule that is name-shaped rather than purpose-shaped.

LEDGER, read off the rendered view. Corpus 870 -> **878**. Coverage
290 -> **362 owned of 426**, 68% -> **84%**; unswept lines
24351 -> **11294**.

The baseline was re-read after merging main, which had added four files
and 848 lines underneath batch 52's closing numbers — so carrying those
forward would have been wrong by exactly that, before this batch moved
anything. Read, not predicted, for the fourth consecutive batch; and the
closing percentage came out one point below the figure the prose was about
to assert, which is the rule earning its keep in miniature.
