---
id: chg-2026-09-07-warden-doc-absorb
type: chg
title: "absorb docs/reference/119-warden (the Menagerie hardware broker, I-34): fold the UNOWNED menagerie-probe (the I-34 grant proof) into sub-menagerie-leaves"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-menagerie-leaves]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The warden (TCB hardware broker, I-34 grantor). No new kernel ABI. quaestor owner:
warden -> sub-warden (audit:hard, fresh 2026-09-06); virtio-mmio-source ->
sub-menagerie-leaves; menagerie-probe -> UNOWNED. Verified atom-by-atom.

ALREADY COVERED (verified): the broker engine + #230 unconditional placement +
#160 revoke-first + #926 await_readiness + the I-34 fourth-leg vacuous-conferrer-
comparison reasoning + one-hop MAY_POST_SERVICE -> sub-warden (audit:hard); the
5e-4 F1 partial-line READY-read DoS (a partial line stalls the TCB forever, the
time/liveness DoS distinct from the memory-cap) -> sub-libdriver-discovery; the
5d-4 reconcile trust-boundary (a non-TCB source can't fabricate reg/INTID to
inflate a grant, warden rebuilds from its trusted view) -> sub-menagerie-leaves +
sub-libdriver-grant; the grant arithmetic + page-rounded #140 over-grant ->
sub-libdriver-grant; the I-34-on-PCI proof + confer-at-spawn ABI -> inv-i34.

THE FOLD (genuine gap -> sub-menagerie-leaves, depth rich; updated 08-04 -> 09-07):
- menagerie-probe was UNOWNED and its canonical I-34 mechanics demo detailed
  nowhere (grep menagerie-probe/out-of-grant/0xDEAD -> only sub-warden's fixture
  MENTION). Folded as the fixture THIRD leaf: the positive (map_mmio the granted
  pl061 window OK -- descriptor round-trip + gate admits) + the DELIBERATE negative
  (Mmio::new 0xDEAD_0000 outside the grant -> kernel REJECTS -- the one leaf that
  proves the gate DENIES where the others prove it ADMITS). Added
  usr/menagerie-probe/{src/main.rs,Cargo.toml} to code:; adjusted purpose (the
  "two production leaves + one fixture proof" framing) + Mechanism ("The probe").

Redirect stub. Zero code change.
