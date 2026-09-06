---
id: chg-2026-09-06-netdev-doc-absorb
type: chg
title: "absorb docs/reference/114-netdev (virtio-net transport MMIO+PCI): clean redirect to sub-netdev + sub-netd-nic"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The native virtio-net frame transport (VirtioNet MMIO + VirtioNetPci PCI). A
dedicated dossier sub-netdev (audit:hard, "the NIC transport, the one counterparty
the kernel does not mediate", owns netdev/lib.rs+ring.rs) LAPS the doc; verified
atom-by-atom.

ALREADY COVERED (spot-checked, not assumed):
- The ring/virtio/virtio_pci layering + the send/poll_rx/drain_tx/wait_irq API ->
  sub-netdev (:70-74).
- Every RW-7/RW-8 audit hardening: the desc_id bounds-check ("bounds the
  untruncated value and declines to recycle a bogus one", :38), the used.len clamp
  (:50), the ring back-pressure (:46/:110-112), the device-death quiesce on Drop
  ("the contract worth reading", :56), the pci-3 F2 NotifyRegionTooSmall guard
  (the doorbell arithmetic bounded against the notify region at bring-up, :100-101)
  -> sub-netdev.
- The consumer (netd's VirtioNetPci use, phy tokens, DHCP bring-up, resident serve
  loop) + the I-34/I-5 probe identity gate (the warden-narrowed PCI claim = the
  live I-34-on-PCI proof, :186) -> sub-netd-nic (audit:hard).
- The #140 MMIO co-residency resolution (PCI's page-aligned BAR isolates net from
  blk at the MMU granule) -> sub-substrate-machine.
- The cross-cutting mechanisms cited: virtio_rmb (F217 class, sub-virtio-probes),
  the ISV-safe MMIO accessor (#890, now sub-libthyla-rs).

Zero-fold. Redirect stub.
