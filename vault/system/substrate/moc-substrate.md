---
id: moc-substrate
type: moc
title: "The substrate: the machine underneath, and the harness that judges it"
parent: home
created: 2026-08-01
updated: 2026-08-01
---
Everything Thylacine runs *on* and is *judged by*: the emulated board, the
build that produces the image, and the gates that decide whether a boot
counts. Five dossiers:

- **[[sub-substrate-build]]** — `tools/build.sh` and the bake chain. What
  each target actually rebuilds, the ledger that says so, and the two
  staleness checks that make `all` trustworthy.
- **[[sub-substrate-machine]]** — `tools/run-vm.sh`: the QEMU virt board,
  the HVF/TCG matrix, the device wiring whose ORDER is load-bearing, and
  the `nowatchpoint` token whose absence is the safe default.
- **[[sub-substrate-gates]]** — the non-interactive verdicts: `test.sh`,
  `smp-multiboot.sh`, `ci-smp-gate.sh`, `check-v80-floor.py`.
- **[[sub-substrate-interactive]]** — LS-CI: the only harness that can
  type, and the richest failure-attribution taxonomy in the tree.
- **[[sub-substrate-builders]]** — the remote GCP builders, disposable and
  permanent.

## Why this area is `audit: none`, and what it carries instead

No dossier here is audit-bearing: none is a kernel surface, and CLAUDE.md's
audit-trigger table has no `tools/` row. That is not a statement that the
code is low-stakes — it is a statement that the *adversarial audit round* is
the wrong instrument for it. A harness is not attacked by an adversary; it
is defeated by its own optimism.

Its obligation is the **revert-probe**: change the harness, then break the
thing it watches and prove it goes red. Every gate in this area that earned
its keep did so by being probed, and every one that failed did so by passing
a test it could not actually see ([[haz-harness-fail-open]]).

## The distinguishing property: a fault here is attributed to the guest

Kernel and userspace bugs announce themselves. A substrate bug **arrives
wearing the guest's clothes** — a truncated fixture reads as corruption, a
dead serial relay reads as a QEMU exit, an un-baked pool reads as a passing
feature test. Every recurring lesson in this area is a variant of one
sentence: *the harness's own fault got read as a Thylacine defect, or a
Thylacine defect got read as the harness's.*

Both directions are represented, and the second is worse:

- **Read as guest**: #72's five lost boots (the relay dying of SIGPIPE under
  a live VM, rationalized as "host timing" for three sessions); #60's
  relay-cut; a partial fixture restore.
- **Read as harness / not read at all**: #101's 40 boots that verified a
  toolchain that was never baked; #83's leg that passed for five days after
  the path it asserted stopped being written; #87's write-proof disarmed by
  a reused disk.

The countermeasures are recorded on the dossiers, but they reduce to three
rules the code states in its own comments: **verify the artifact, not the
intent**; **a retry is a tolerance, never a diagnosis**; and **a gate that
cannot see the feature reports success identically to one that verified it.**

## What is here that is not a script

[[abi-boot-banner]] — the two strings (`Thylacine boot OK`, `EXTINCTION:`)
that are genuinely kernel ABI, consumed by every gate in this area and
binding on `kernel/main.c`.

Gates: [[gate-smp]] · [[gate-interactive]] · [[gate-v80-floor]].
Hazard: [[haz-harness-fail-open]].

## Not here

The in-kernel test *runner* (`kernel/test/`) is guest code and belongs with
the kernel; this area covers the host-side harness that boots it and reads
its verdict. The boundary is the serial line.
