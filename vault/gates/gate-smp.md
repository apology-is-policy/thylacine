---
id: gate-smp
type: gate
title: "The SMP soundness gate (multi-boot corruption classifier)"
proves: "0-corruption SMP soundness: default+UBSan kernels x smp4/smp8, N>=10 boots each, every boot classified CORRUPTION vs benign host-TIMING; fails iff any boot corrupts. Single boots lie -- multi-boot or it didn't happen."
blind-to: "Races below the sampling rate (N=10 per config is a sample, not a proof); deterministic-interleaving bug classes (the owed multi-in-flight harness, seam-841-mi-harness); accel-specific behavior outside the run's accel (test.sh runs HVF, test-interactive TCG -- different CPUs); anything whose failure mode is a wrong-but-plausible result rather than a corruption signature. It proves absence-of-corruption-in-N-boots, never absence-of-race."
invocation: "tools/ci-smp-gate.sh (make smp-gate); subset via SMP_GATE_CONFIGS=\"default-smp4 ubsan-smp4\" -- the full 4-config matrix sits AT the Bash 600s ceiling, so split it (feedback-smp-gate-split)."
created: 2026-07-31
updated: 2026-08-01
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

**Four classes, and two of them fail the gate** (corrected 2026-08-01
against `tools/smp-multiboot.sh`; this note previously described two):

| Class | Fails? | Anchored on |
|---|---|---|
| CORRUPTION | yes | EXACT extinction strings — `invalid prev state`, `stack canary mismatch`, `kernel stack overflow`, `already on_cpu`, `#860`, `not RUNNABLE-and-off-cpu`, `corrupted current`, `sched: deadlock` |
| INJECT-MISS | no | the full green-guest proof (all five conjuncts, below) |
| TIMING | no | EMITTED warn strings only — `[SOFT-WARN]`, the irq-bench CI-budget text |
| OTHER | **yes** | nothing; an unclassified nonzero exit |

OTHER failing is the load-bearing choice: an unexplained red is surfaced,
never absorbed. There is deliberately no bucket for "probably fine", and
"could not classify" is HARD — an unverified pattern check fails silent, and
a gate that cannot parse its log fails open.

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
