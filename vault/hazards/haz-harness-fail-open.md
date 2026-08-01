---
id: haz-harness-fail-open
type: haz
title: "The harness fails open — a gate that cannot see the feature reports success"
applies-to: [sub-substrate-gates, sub-substrate-interactive, sub-substrate-build, sub-substrate-builders]
instances: []
created: 2026-08-01
updated: 2026-08-01
---
## The failure shape

A verification asset stops being able to observe the thing it verifies, and
its output is **indistinguishable** from a run that verified it. Nothing
goes red. The gate keeps reporting PASS, and the PASS is now evidence of
nothing.

It has two directions, and they are not symmetric:

**Read as guest** — the harness's own fault is attributed to Thylacine. A
truncated fixture reads as corruption; a dead serial relay reads as a QEMU
exit; a contaminated config reads as a geometry regression. Costly, but
self-limiting: someone is looking at a red.

**Read as verified** — the strictly worse direction. The feature was never
exercised, or the assertion was satisfied by the broken state. Nobody looks,
because everything is green, and the gate's own history becomes false
evidence retroactively.

## The tells

- A gate passes on a configuration where the feature is *absent*, because
  the consumer "skips when absent by design."
- An assertion that is satisfiable by a state the system reaches when
  broken (a reused fixture already holding the expected bytes).
- A classification bucket anchored on a token present in HEALTHY output.
- A "green" stage whose artifact was never produced (exit status is not
  evidence).
- A positive control that passes without discriminating — the clean leg
  containing none of the thing being detected.

## Worked instances

| | What happened | Direction |
|---|---|---|
| #101 | `THYLACINE_BAKE_CLADE` defaulted to 0, so 40 gate boots ran with `/clade` absent; the probe skips an absent server by design → 40/40 PASS | verified |
| #83 | A `config.cfg` from an earlier run satisfied the assertion trivially; the leg passed for five days after the asserted path stopped being written | verified |
| #87 | A reused `disk.img` already holds pattern B, so a dropped write reads a stale B and PASSES — reusing the fixture disarms the write proof | verified |
| #362 | The TIMING regex matched a PASSING test-name line, making the class a catch-all; 23 of 40 inject-misses buried in it | verified |
| #91 | The v8.0 source check enumerated the build inputs it expected; the offender was found only by measuring the shipped binary | verified |
| #82 | A failed scenario left `mode 1600 900` in the pool; later boots inherited it → false RED blamed on a merge | guest |
| #72 | Five of ten boots lost to the relay dying of SIGPIPE under a LIVE VM, rationalized as "host timing" for three sessions | guest |
| #60 | The relay losing its reader while the VM was alive, indistinguishable from a guest exit at the expect layer | guest |

## The countermeasures

1. **Verify the artifact, not the intent.** Check that `pool.img` exists and
   is large enough; do not trust the flag that requested it.
2. **Revert-probe every gate.** Break the thing it watches and prove it goes
   red. A gate that has never failed has never been shown to work.
3. **Make the control discriminate.** A positive control must contain the
   thing being detected, or its pass is vacuous.
4. **Anchor classification on failure-only text.** Never on a token that
   appears in a healthy log.
5. **Restore fixtures to a pristine twin** per attempt, and fail CLOSED if
   the restore is partial — booting an unknown fixture manufactures the
   corruption the gate exists to detect.
6. **Split attribution with evidence, not inference.** Record the VM's `ps`
   state and the relay's exit record at failure time; both causes look like
   one EOF otherwise.
7. **A retry is a tolerance, never a diagnosis.** Preserve every failed
   attempt's artifact — the no-host-load discipline needs something to look
   at.

## Relation to the forbidden non-explanations

This is the harness-side face of the discipline that forbids "host load",
"timing", and "flake" as explanations. Those phrases are how direction-one
failures get closed; "it passed" is how direction-two failures never get
opened. Both are the same reach for a reason not to look.

## Referenced by

[[moc-substrate]] · [[sub-substrate-gates]] · [[sub-substrate-interactive]] ·
[[sub-substrate-build]] · [[sub-substrate-builders]].
