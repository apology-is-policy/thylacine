---
id: chg-2026-08-15-interactive-gating
type: chg
title: "LS-CI re-swept: the gate went parallel, and its own default is stated three times"
date: 2026-08-15
arc: arc-vault
commits: ["355ffa3e"]
touched: [sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: [tools/test-interactive.sh, tools/interactive/lib.exp, tools/interactive/serial-bridge.py, tools/interactive/serial-listen.py, tools/interactive/test-serial-bridge.py]
depth: rich
created: 2026-08-15
---
~518 lines across the harness since the dossier, and the substance of it is
the **gating overhaul** — G-1 (time it), G-2 (run N at once), G-3 (pick each
scenario's accel). The dossier's Concurrency section, one sentence long, said
*"One VM at a time within a run."* That is now wrong by a factor of three, and
the sentence was load-bearing for how a reader budgets a run.

## The overhaul, in the order it had to happen

The sequencing is the part worth keeping, because it is the opposite of the
obvious one. **The instrument came first on purpose.** G-1 built a
per-scenario/per-attempt timings TSV before either optimisation landed,
because G-2 needs a per-scenario cost to pack slots sensibly *and* a
before/after to prove it gained anything, and G-3 allocates a scarce, riskier
resource **by** cost. Measured result of the pair: 4908 s serial → 2925 s
wall on a full green 34/34.

And the timings table reads its accel **out of the boot artifact**, not out
of the harness's own environment, because 14 scenarios override it — *"a
timings table whose accel column is wrong is worse than one with no accel
column, because it invites exactly the tcg-vs-hvf comparison"* it exists to
prevent. That is #222's rule under a different hat: make the producer report
the fact rather than inferring it from what you think you asked for.

## Going parallel meant finding everything that was implicitly serial

The pool fixture, the QMP socket and the reaper all became per-slot. The
reaper is the interesting one, and the comment records that it was
**measured, not assumed**: bash resets *signal* traps in a subshell, but the
`EXIT` trap still fires when that subshell exits — so a forked scenario
inherits the tree-wide reaper and runs it on the way out, taking down every
neighbour. That is #59's cross-tree shootout reproduced *inside* one tree,
and it presents as "qemu GONE, guest healthy" — indistinguishable from a
guest fault, and precisely the shape this project keeps mistaking for load.

The budget scaling is the other half, and its reasoning points the same way
as [[sub-substrate-interactive]]'s existing #74 material: under contention a
*healthy* guest is legitimately slower (each VM gets 4 vCPUs; N VMs
oversubscribe an 8-core host and TCG is CPU-bound), measured at ~190 s
serial → ~400 s each at three-up. Against a fixed 300 s budget that is a
**harness fault reported as a guest regression**. Erring generous is the safe
direction: an over-large budget only delays declaring a wedged guest dead,
while an undersized one reports failures that do not exist.

## G-3's anchor set is the best structure in the file

Accel is not a speed knob: `run-vm.sh` derives the CPU model *and* the GIC
version from it, so hvf means `-cpu host` + GICv2 and tcg means `-cpu max` +
GICv3 (HVF cannot do v3 here at all). Every scenario moved to HVF therefore
stops covering GICv3 and `-cpu max` — and **#166 is the standing proof that a
scenario can go inert under HVF while still reporting PASS**, the worst
outcome available.

Seven scenarios stay on TCG and the list is **mechanical rather than
"whatever nobody got round to flipping"**: each has a recorded reason, and
several are the only surviving coverage of an open bug — a TCG-only
watchpoint livelock, a bug that reproduces only under TCG gate load, two
whose trigger *is* TCG's serialized vCPU.

**And it is enforced.** An hvf directive inside an anchor's `.exp` is refused
with a message naming the coverage it would drop. The comment states the
principle exactly: *"Enforced, not documented… a coverage rule that depends
on nobody editing the wrong file is not a rule."*

## The finding is that same principle, unapplied 300 lines away

`LS_CI_JOBS`'s default appears three times:

| where | says | |
|---|---|---|
| the usage block, `:25` | 1 | **stale** |
| the code, `:339` | 3 | true |
| the policy comment, `:626` | *"stays 1 until a parallel run has been proven green"* | **stale** |

G-2 landed all three consistent at 1. G-3 flipped the code and updated
neither prose copy — so the surviving policy sentence names a precondition as
*unmet*, three hundred lines below the comment citing the very run that met
it ("proven by a full green 34/34 run").

Not cosmetic, because the parallel branch changes behaviour a reader would
not go looking for: an unconfigured run actually gets a **900 s** boot budget
rather than 300 and **90 s** per command rather than 30, and starts three VMs
on a host the reader believes is running one. The file itself notes that a
swapping host *"makes every timeout in the suite marginal — which would then
get read as guest flakiness"*, which is the exact failure this dossier exists
to keep legible. Task #183.

**A method note that cost me a wrong first answer.** `git log -S
'LS_CI_JOBS:-'` does not find the flip: `-S` counts *occurrences* of the
string, and `:-1` → `:-3` leaves the prefix count unchanged, so the commit
that made the change is invisible to the search most likely to be reached
for. It reported one commit — G-2, the one that introduced the line — which
reads exactly like "nothing has changed it since." Same family as the day's
other pattern slips: **the search returned a confident wrong answer rather
than an error.** The control that caught it was free and structural: G-2's
own text said "the default stays 1 *until proven*", which is a promise, and a
promise in a file is a thing to go and check the far side of.

## One thing the dossier had right and the code has since narrowed

The `#217` refusal — the intra-tree half of #59 — closes the case the
existing reaping paragraph left open, and by **refusal rather than
narrowing**: the `EXIT` trap is still tree-wide, so a VM this script never
started would die uncatchably on the way out, its log simply stopping. In-tree
concurrency is unsafe for a second reason anyway (both gates restore the same
`pool.img`, and a restore under a live VM manufactures exactly the corruption
the gates exist to detect), so a named operator error beats a silent
mutual-corruption race.

The ordering carries a scar worth preserving: **the check must precede the
trap install**, because install-then-refuse fires the reaper on the refusal's
own exit — killing the VM it had just declined to disturb.
