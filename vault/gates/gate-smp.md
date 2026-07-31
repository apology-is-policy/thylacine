---
id: gate-smp
type: gate
title: "The SMP soundness gate (multi-boot corruption classifier)"
proves: "0-corruption SMP soundness: default+UBSan kernels x smp4/smp8, N>=10 boots each, every boot classified CORRUPTION vs benign host-TIMING; fails iff any boot corrupts. Single boots lie -- multi-boot or it didn't happen."
blind-to: "Races below the sampling rate (N=10 per config is a sample, not a proof); deterministic-interleaving bug classes (the owed multi-in-flight harness, seam-841-mi-harness); accel-specific behavior outside the run's accel (test.sh runs HVF, test-interactive TCG -- different CPUs); anything whose failure mode is a wrong-but-plausible result rather than a corruption signature. It proves absence-of-corruption-in-N-boots, never absence-of-race."
invocation: "tools/ci-smp-gate.sh (make smp-gate); subset via SMP_GATE_CONFIGS=\"default-smp4 ubsan-smp4\" -- the full 4-config matrix sits AT the Bash 600s ceiling, so split it (feedback-smp-gate-split)."
created: 2026-07-31
updated: 2026-07-31
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

CORRUPTION = any EXTINCTION, assert, UAF signature, wrong-result marker.
TIMING = harness-side latency artifacts whose guest end-state is proven
healthy. "Could not classify" is HARD (an unverified pattern check fails
silent; a gate that cannot parse its log fails open).

## History

Caught the #99 F1 negative-dentry race at 2/10 that the single-boot suite
missed. Known self-inflicted traps, all recorded as binding feedback: the
mv-restore mtime trap (a stale object survived rebuilds → 20/20 false FAIL);
the exit-144 mid-matrix cutoff that reads as failure but is the Bash timeout;
`| tail -N` swallowing the gate's exit code.
