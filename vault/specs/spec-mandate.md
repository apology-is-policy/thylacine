---
id: spec-mandate
type: spec
title: "mandate.tla — initial authority transaction composition"
models: [sub-corvus-authority, sub-corvus, sub-kernel-caps]
pins: [inv-i35, inv-i25, inv-i27]
cfgs: [mandate.cfg, mandate_buggy_stale_support.cfg, mandate_buggy_fork_admission.cfg, mandate_buggy_no_cascade.cfg, mandate_buggy_preview_change.cfg, mandate_buggy_no_restore.cfg, mandate_buggy_replay.cfg, mandate_buggy_no_audit.cfg]
gate: "Run specs/check-mandate.py when changing authority policy, trusted commit admission, replay or revocation publication. Each mutant must violate its named invariant."
created: 2026-09-24
updated: 2026-09-24
---
## Abstraction
Initial finite safety model: one envelope, one transaction, root/child, one crash.
Split fork-copy/publication and durable restrictive intent/admission closure.
No liveness claim. Does not model the full grant DAG, groups, account state,
separate commit-admission/publication, cryptography, byte codecs, locking,
storage guarantees, driver barriers or physical display. Those require later
model extensions and runtime tests before the corresponding UA stage closes.

## Action-site map
No runtime mapping yet. The pure policy analogues are Prepare/Commit to
`Ledger::check_issue`/`issue`; CloseAdmission to `begin_revoke`; graph invalidity
to `is_live`; Complete to `finish_revoke`. These are proof obligations, not a
claim the pure library implements kernel admission or persistent commits.

## Validation
2026-09-24: clean 154 distinct states; seven deliberately buggy configurations
fail Supported, RevocationComplete, Supported, Bounded,
RestorationBeforeCommit, NoReplayReopen and AtomicAudit respectively.
`check-mandate.py` uses one worker, 512 MiB heap, bounded timeout and exact
TLC violation exit code + invariant name. Logs/trace inputs are retained under
work/ua-model; parser errors are failures, never successful negative evidence.
The first run exposed unassigned Boolean variables in Init; explicit Boolean
assignments repaired this model error before the successful run.

## Transaction detail extension

[[spec-mandate-commit]] now supplies separate admission/publication,
group-dependent preview, suspension/resume and requester/issuer lifetime actions.
The two models retain distinct abstraction limits; no full runtime proof claimed.
