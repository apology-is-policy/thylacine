---
id: gate-smp
type: gate
title: "The SMP soundness gate (multi-boot corruption classifier)"
proves: "0-corruption SMP soundness: default+UBSan kernels x smp4/smp8, N>=10 boots each, every boot sorted into FIVE classes down an ordered ladder (CORRUPTION, EXTERNAL-KILL, INJECT-MISS, TIMING, OTHER); fails iff any boot is CORRUPTION, EXTERNAL-KILL or OTHER -- NOT 'iff any boot corrupts', which this field claimed until 2026-08-16. Single boots lie -- multi-boot or it didn't happen."
blind-to: "Races below the sampling rate (N=10 per config is a sample, not a proof); deterministic-interleaving bug classes (the owed multi-in-flight harness, seam-841-mi-harness); accel-specific behavior outside the run's accel (test.sh runs HVF, test-interactive TCG -- different CPUs); anything whose failure mode is a wrong-but-plausible result rather than a corruption signature. It proves absence-of-corruption-in-N-boots, never absence-of-race."
invocation: "tools/ci-smp-gate.sh (make smp-gate); subset via SMP_GATE_CONFIGS=\"default-smp4 ubsan-smp4\" -- the full 4-config matrix sits AT the Bash 600s ceiling, so split it (feedback-smp-gate-split)."
created: 2026-07-31
updated: 2026-08-16
---
## Method

Builds default + UBSan kernels, multi-boots each at `-smp 4` and `-smp 8`,
greps each boot log (always `grep -a` — logs carry binary spill) for the
corruption signatures vs the benign-timing set, and fails the gate iff any
boot classifies CORRUPTION. Every timing-classified boot must be
ground-truthed to a healthy guest end-state (boot OK + full suite + login +
0 EXTINCTION) before the run is called clean — "host load" is a forbidden
non-explanation, so an unexplained red boot is a race to hunt, never a wave.

## Classification rules

**Five classes, and three of them fail the gate.** The ladder is ORDERED, so
each row also means "nothing above it matched":

| Class | Fails? | Anchored on |
|---|---|---|
| CORRUPTION | yes | EXACT extinction strings — `invalid prev state`, `stack canary mismatch`, `kernel stack overflow`, `already on_cpu`, `#860`, `not RUNNABLE-and-off-cpu`, `corrupted current`, `sched: deadlock` |
| EXTERNAL-KILL | **yes** | QEMU's own `terminating on signal N from pid M` (#88) |
| INJECT-MISS | no | the full green-guest proof (all five conjuncts, below) |
| TIMING | no | EMITTED warn strings only — `[SOFT-WARN]`, the irq-bench CI-budget text |
| OTHER | **yes** | nothing; an unclassified nonzero exit |

OTHER failing is the load-bearing choice: an unexplained red is surfaced,
never absorbed. There is deliberately no bucket for "probably fine", and
"could not classify" is HARD — an unverified pattern check fails silent, and
a gate that cannot parse its log fails open.

**And that is not sufficient by itself.** #88's incident was an OTHER that
failed the gate correctly and told the operator nothing: one boot of forty,
`<unclassified>`, guest provably healthy, QEMU's last line announcing an
outside signal the classifier was discarding. A failing catch-all buries real
failures exactly as a benign one does — by silence in the benign case, by
making the red routine in the failing case. EXTERNAL-KILL still fails; what
changed is that the verdict now says why.

Its soundness is negative space, not content: a guest cannot signal its own
hypervisor, and the harness's only kill is `kill -KILL`, which is uncatchable
— so QEMU prints **nothing** for it. The one killer that could make the line
ambiguous cannot write it.

**Three counts of this note were wrong for two weeks and the body was right.**
The 2026-08-01 sweep corrected the table from two classes to four and left the
`proves:` frontmatter stating the two-class version *and a false exit
condition* ("fails iff any boot corrupts" — OTHER has failed it since #362).
`proves:` is the field rendered into the views, so the wrong version had the
wider readership. Corrected 2026-08-16 in both places, and the failure is
recorded rather than quietly fixed because **a correction that updates the
prose and not the summary is the normal way this drifts** — the prose is where
you are reading when you notice, and the summary is somewhere else.

**INJECT-MISS requires proving the guest GREEN, not merely proving the
injection missed**: the `AWAITING_QMP_KEY` sentinel present, a clean
`virtio-input: SKIP`, the banner present, no `EXTINCTION:`, and no suite
FAIL line. A boot that merely *also* missed injection stays CORRUPTION or
OTHER.

**Two precision rules, both learned by being wrong.** CORRUPTION uses exact
strings because a bare `canary` matched the benign `canaries` hardening
banner and the `canary: initialized` boot line — a false positive on every
healthy boot. TIMING is anchored on emitted warn text and **never on test
names**: the pre-#362 pattern contained `stalk.*lifetime`, which matched the
PASSING line `[test] stalk.lifetime_no_leak ... PASS` present in every log,
making TIMING a catch-all that absorbed any nonzero exit. It buried 23 of 40
inject-misses — "and a real unclassified failure would have been too."

Every boot restores `pool.img` from its baked snapshot first (go4c probes
age the fixture with ~6x CoW amplification; a long matrix would otherwise
drift toward the timeout and eventually ENOSPC into false reds), and the key
twin is compared before the restore so only the pool matching the live key
can be installed. Both the guest serial log AND the harness stdout are
captured on every non-PASS — a post-banner verdict step leaves no trace in
the serial log, which is why one 2026-07-19 OTHER was undiagnosable.

## History

Caught the #99 F1 negative-dentry race at 2/10 that the single-boot suite
missed. Known self-inflicted traps, all recorded as binding feedback: the
mv-restore mtime trap (a stale object survived rebuilds → 20/20 false FAIL);
the exit-144 mid-matrix cutoff that reads as failure but is the Bash timeout;
`| tail -N` swallowing the gate's exit code.

#88's original sender was **never identified** — the two known QEMU-killers in
the tree were checked and falsified. The class exists as a standing trap with
a `ps` at classify time, so a recurrence names it. An open question wearing a
label is still open; the label only stops it costing a fresh investigation
each time.
