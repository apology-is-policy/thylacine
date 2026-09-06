---
id: chg-2026-09-07-verification-doc-absorb
type: chg
title: "absorb docs/reference/13-verification (Phase-1 verification infra): fold the deliberate-fault matrix anti-DCE + #244 into sub-kernel-boot-sequence, claim fault_test.c + test-fault.sh"
date: 2026-09-07
arc: arc-vault
commits: ["f6bd18e0"]
touched: [sub-kernel-boot-sequence]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The Phase-1 verification cadence (leak checks, boot-time, KASLR variability, UBSan
trap, the deliberate-fault matrix). quaestor owner: fault_test.c / test-fault.sh /
verify-kaslr.sh / test_phys.c ALL UNOWNED (3 of 72 kernel files unclaimed).
Verified atom-by-atom.

THE FOLD (audit-critical gap -> sub-kernel-boot-sequence, depth rich):
- The deliberate-fault matrix was described-by-REFERENCE (sub-substrate-gates
  covers the SMP-CLASSIFIER's assumed-vs-known lesson #143/#234/#212, NOT the fault
  provokers; sub-kernel-boot-sequence only REFERENCED the 7 provokers at :104) and
  OWNED by neither. Folded the load-bearing anti-DCE detail: canary_smash's
  asm("":"+r"(p)) launder (else clang elides the OOB writes at -O2 +
  fstack-protector-strong -> canary never arms), bti_fault's volatile fp (else
  clang's bl devirtualization bypasses BTI -- bl doesn't set PSTATE.BTYPE),
  wxe_violation's kernel-text write, pac_mismatch deferred (v1.0 verifies via
  EnIA+APIA-nonzero + code review + ARM mandate), and the #244 lesson (a silent
  provoker reads identically to a passing boot -- test-fault.sh in no gate is how
  recursive_kernel_fault emitting NOTHING hid ~a month). Claimed fault_test.c +
  tools/test-fault.sh in the code: list. updated: 09-06 -> 09-07.

COVERED-BY-REFERENCE (redirect): the KASLR variability gate (verify-kaslr, I-16
witness) -> abi-boot-banner (the parsed line); the SMP/UBSan/boot harness ->
sub-substrate-gates; the boot-time measurement + in-kernel leak suite ->
sub-kernel-boot-sequence.

RESIDUAL UNOWNED (pre-existing, NOT my regression, content covered): verify-kaslr.sh
+ test_phys.c + test_slub.c stay unclaimed -- a light follow-up claim owed (KASLR
gate -> gates/KASLR home; leak tests -> allocator dossier). Historical/stale doc
content (3-of-8 variant list, *(pending)* hash, KASAN-deferred, "TLA+ Phase-2
onward" predating the 34-module inventory) named as P1-I history. Redirect stub.
Zero code change.
