---
id: chg-2026-09-24-authority-admission-core
type: chg
title: "Bind immutable issuance approval to live administrative proof"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus-authority, inv-i35, spec-mandate-commit]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Implement the pure issuance approval transitions after the checked commit model.
A canonical preview is bound to source, principal/incarnation, revision, receipt,
authentication and successful restoration. Admission consumes the prepared object
and requires a live Administrative proof for exactly that transaction; decoded
client data cannot stand in for trusted kernel/seat inputs. Preview eligibility
is a separate helper, never a fabricated live Activation. No runtime endpoint,
journal or publication implemented by this tranche.

Validation:47 host tests pass, including wrong receipt/episode/source, child
incarnation, execution-scope substitution, policy changes, support invalidation,
weak auth, expiry, cancel and missing restoration. Bare-target check passes with
existing outline-atomics warning; host Clippy -D warnings passes. Both models
(154 /3768 states) and all14 named mutants rechecked. Evidence:
work/ua-policy/transaction-first.log and work/ua-model/20260924T111948.364450Z.
Single-agent self-review only. Runtime source binding, storage/audit reservations,
prepared-transaction quotas and exact protocol/kernel ABI integration remain open.
