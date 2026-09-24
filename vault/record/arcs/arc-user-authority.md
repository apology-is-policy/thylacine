---
id: arc-user-authority
type: arc
title: "Scoped user administration and durable delegation through Imperium"
status: active
design: [docs/USER-AUTHORITY-DESIGN.md, docs/MANDATE-DESIGN.md, docs/IMPERIUM-DESIGN.md]
chunks: [chg-2026-09-24-user-authority-spec, chg-2026-09-24-user-authority-ratification]
follow-ons: []
exit-criteria:
  - "[ ] UA-P0: integrate and verify the prior-debug taint gate at elevation redemption (Aux kernel repair)"
  - "[ ] UA-P1: enforce administrative/scoped-use authority coverage at debug access; keep the authority ledger unsealed"
  - "[ ] Ratify detailed specification and reserve exact ABI/model contracts"
  - "[ ] Implement policy records, typed administrative scopes and principal admission barriers"
  - "[ ] Implement durable transactions, audit, crash replay and resource-owner revocation"
  - "[ ] Deliver Imperium administration commands and trusted transaction confirmation"
  - "[ ] Deliver namespace mandates, installer migration and recovery"
  - "[ ] Qualify resource-scoped grants, including derived-handle revocation"
  - "[ ] Complete model, runtime, SMP and graphical qualification; update manual and Vault"
created: 2026-09-24
---
## Goal

The operator requested an implementation specification following the September
24 research on use, identity administration and delegation. Imperium is the
userspace driver; Corvus is the policy authority. Durable administration is
eligibility, exercised only through a live scoped legate. The implementation
specification is written for review; no numeric ABI or behavior is changed.

## Planned chunks

USER-AUTHORITY-DESIGN sections 8 and 16 own the detailed UA-P0/P1 prerequisites
and UA-0..UA-7 sequence. UA-P0 was reported by Aux on Yip 0115 and confirmed by
source inspection of the missing prior-debug gate; no new exploit was executed.
A debugger-modified precursor must not receive later authority. UA-P1 prevents
zero-operational-bit Admin authority from escaping the existing flat cap cover.
These are open blocking prerequisites, not assertions of safety today.

The registry/headroom prerequisite remains owned by arc-astra-halcyon-followup.
No new heavy host work or resource lease is needed for the specification.

## Close summary
(written at status flip to complete)

## Implementation authorization

The operator approved the complete specification after reviewing 44d158c3.
[[dec-2026-09-24-user-authority-implementation]] records the decision. Work and
evidence are tracked in `docs/USER-AUTHORITY-STATUS.md`; no implementation gate
is claimed by approval.
