---
id: moc-kernel-security
type: moc
title: "Kernel authority substrate"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-02
---
The machinery that decides whether an operation is allowed: the per-Proc
handle table (references and their rights), the capability model and its
elevation path, the hardware allowance, and the identity-axis permission
check. Every privilege gate in the tree resolves through one of these four.

## The organizing fact

**Authority is three orthogonal axes, and the tree's discipline is that they
never substitute for one another.**

| Axis | Question | Carrier | Home |
|---|---|---|---|
| capability | *may this Proc do this kind of thing at all?* | `Proc.caps` | [[sub-kernel-caps]] |
| rights | *may this reference be used this way?* | `Handle.rights` | [[sub-kernel-handle]] |
| identity | *is this principal allowed at this object?* | `Proc.principal_id` + groups | [[sub-kernel-perm]] |

The hardware allowance ([[sub-kernel-allowance]]) is a fourth, narrower axis
layered *under* the capability one: `CAP_HW_CREATE` says a Proc may create
hardware handles at all, the allowance says *which* windows, INTIDs, DMA
sizes and PCI functions it may name.

Orthogonality is enforced, not merely intended, and the enforcement is
visible as a set of deliberate *refusals* to conflate:

- `perm_check` special-cases **no** `principal_id` — not even
  `PRINCIPAL_SYSTEM`. The DAC-override is a capability, never an identity
  ([[inv-i22]]'s statement made mechanical).
- `CAP_DAC_OVERRIDE` is deliberately **not** an axis on the kill or debug
  gates: fs-admin stays orthogonal to process control (Linux's
  `CAP_DAC_OVERRIDE` vs `CAP_KILL` split).
- The handle envelope may never exceed the access the identity check
  validated — `rights_for_omode` and `perm_want_for_omode` are written as a
  matched pair for exactly that reason.
- Elevation-only capabilities never ride identity or inheritance: `rfork`
  strips them unconditionally, so an elevated parent cannot leak elevation
  to a child.

## Children

- [[sub-kernel-handle]] — the per-Proc table: the four-way kind partition
  (what may be duplicated or transferred), the rights ceiling, and the
  `#844` snapshot-with-a-held-ref lifetime discipline.
- [[sub-kernel-caps]] — the capability model: fork-grantable vs
  elevation-only, the two-phase `cap`-device grant, and the legate.
- [[sub-kernel-allowance]] — the hardware allowance: the two-step create
  that closes the revoke-vs-create race.
- [[sub-kernel-perm]] — the identity axis: owner-first POSIX, the
  no-give-away chown, and the omode pairing.

## Cross-cutting

- Invariants: the family this area enforces is I-2 (capability monotonic
  reduction), I-5 (hardware handles non-transferable), I-6 (rights
  monotonic), [[inv-i22]] (no ambient super-authority), I-23 (service FS
  authority bounded by its endowment), I-25 (legate scope) and I-34 (driver
  allowance). This sweep gave all seven a swept `guards` home — the edge that
  makes an invariant note more than a restatement of scripture, and whose
  absence stalled the batch-13 registry pass
  ([[chg-2026-08-02-authority-sweep]]). [[inv-i22]] is minted; the remaining
  six are mintable and unblocked.
- Specs: `specs/handles.tla` (the kind partition + `RightsCeiling`),
  `specs/allowance.tla` (the revoke-vs-create race), `specs/corvus.tla`
  (`HostownerRequiresConsole`). None minted yet — same registry pass.
- The gates that consume this area but live elsewhere are now all swept:
  `/proc/<pid>/ctl` kill ([[inv-i26]]) and the debug surface ([[inv-i39]]) in
  [[sub-kernel-devproc]], and the trusted path ([[inv-i27]]) in
  [[sub-kernel-cons]] + [[sub-kernel-devdev]]. Every invariant this area's
  gates enforce has a `guards` home.
