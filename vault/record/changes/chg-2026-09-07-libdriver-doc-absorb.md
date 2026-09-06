---
id: chg-2026-09-07-libdriver-doc-absorb
type: chg
title: "absorb docs/reference/118-libdriver (the Menagerie driver framework, I-34): clean redirect to sub-libdriver-grant + sub-libdriver-discovery (grant AHEAD of the doc)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
The libdriver framework crate (manifest/resource/dtb/source/supervise/readyline/
driver). quaestor owner: manifest+resource -> sub-libdriver-grant (audit:hard,
2026-09-06); source+supervise+dtb+readyline -> sub-libdriver-discovery (audit:hard,
2026-08-03). Verified atom-by-atom (much already verified in the 119-warden pass).

ALREADY COVERED (verified, grant is AHEAD): resolve's grant-never-exceeds-node
I-34 property + the descriptor codec + the ;-delimiter-injection guard + the
page-rounded MMIO allowance (#140 over-grant) + the authority-vs-information split
(kernel gate is the boundary, not the codec) + the PCI axis (PciNeed::Node -> bdf,
no MMIO, push_pci) -> sub-libdriver-grant [AHEAD: carries a multi-bdf "gathered
extra bdfs" grant the doc's single-bdf codec lacked]; the typed DeviceId
(VirtioPci-distinct-from-Virtio, disjoint claim paths + prefixes) + best_match +
DtbSource/PciSource + the BE cell-width DTB decode + next_step restart state machine
+ feed_ready_line (5e-4 F1 pure half) + reconcile_reported_node (5d-4 trust
boundary, the "three layers each trusting its predecessor less" spine) ->
sub-libdriver-discovery; the consumers -> sub-warden + sub-menagerie-leaves; I-34
-> inv-i34.

Zero-fold. Redirect stub. MSI-carried / whole-node-MMIO / sig-carried seams are live
v1.x seams the dossiers hold. Zero code change.
