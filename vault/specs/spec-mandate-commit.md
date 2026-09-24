---
id: spec-mandate-commit
type: spec
title: "mandate_commit.tla -- immutable trusted transaction admission"
models: [sub-corvus-authority, sub-corvus, sub-kernel-caps]
pins: [inv-i35, inv-i25, inv-i27]
cfgs: [mandate_commit.cfg, mandate_commit_buggy_no_recheck.cfg, mandate_commit_buggy_group_delta.cfg, mandate_commit_buggy_no_restore.cfg, mandate_commit_buggy_fork_admin.cfg, mandate_commit_buggy_repurpose.cfg, mandate_commit_buggy_no_audit.cfg, mandate_commit_buggy_replay.cfg]
gate: "Run specs/check-mandate.py before changing transaction admission, immutable intent, group-dependent preview, publication/replay or administrative scope inheritance."
created: 2026-09-24
updated: 2026-09-24
---
## Abstraction
One transaction, two incomparable rights, one group revision increase, one crash.
Admit and Publish are separate. Client exit/expiry after Admit can coexist with
publication of that same immutable request; source/issuer/generation invalidation
prevents effective authority. Group changes after preview must be revalidated.
Suspend/resume does not revive an old scope; durable restrictive intent closes
replay admission. No liveness claim. Actual user/group resolution, graph depth,
filesystem/disk durability, physical restoration and locks remain beneath this
model. mandate.tla separately covers member-copy/publication/drain; these are
complementary bounded models, not a proof of the entire runtime composition.

## Action-site map
The isolated issuance approval core now maps Prepare to `PreparedIssue::new`
plus `Ledger::preview_issue`; Show to `visible`; Authenticate to `authenticated`;
Restore to `restored`; Admit to `admit` plus `Ledger::check_issue`. None is wired
to a Corvus endpoint or physical receipt. Publish/replay, account/group mutation
and actual issuer-death/admission machinery remain implementation obligations. The
kernel must supply the live root's non-inherited scope identity at Admit; the
client cannot name that identity as authority. Source recheck at Publish gives
restriction priority without reusing the operator's approval for changed intent.

## Validation
2026-09-24: clean 3768 states, seven named mutants detected: Supported, Bounded,
RestorationBeforeAdmission, TrustedActor, FrozenIntent, AtomicAudit,
NoReplayReopen. Logs and exact inputs retained under
work/ua-model/20260924T110752.927464Z. No runtime implementation claim.
