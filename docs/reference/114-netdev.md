# 114 — netdev (the virtio-net frame transport: MMIO + PCI) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-netdev-doc-absorb`).
The native (libthyla-rs, `no_std`) Ethernet frame transport: `VirtioNet` (MMIO) +
`VirtioNetPci` (PCI), the reusable NIC driver `netd` owns and smoltcp wraps. Its
content lives, code-verified and current, in:

- the **transport itself** — the `ring`/`virtio`/`virtio_pci` layering (the kaua
  pattern: pure split-virtqueue arithmetic + device glue), the `send`/`poll_rx`/
  `drain_tx`/`wait_irq` API, and every RW-7/RW-8 audit hardening: the `desc_id`
  bounds-check before it scales the RX-pool base (the OOB-read guard — "bounds the
  untruncated value and declines to recycle a bogus one"), the `used.len` clamp,
  the ring back-pressure (never more than `QUEUE_SIZE` in flight), the device-death
  **quiesce** on Drop (`QUEUE_READY=0` + reset before the DMA pages free — "the
  contract worth reading"), and the pci-3 F2 `NotifyRegionTooSmall` guard (the
  doorbell arithmetic bounded against the notify region at bring-up):

      vault/system/userspace/runtime/sub-netdev.md   (audit: hard)

- the **consumer + the bring-up + I-34** — `netd`'s use of `VirtioNetPci`, the phy
  tokens, the DHCP bring-up, the resident serve loop, and the I-34/I-5 probe
  identity gate (the warden-narrowed PCI claim — the live I-34-on-PCI proof):

      vault/system/userspace/services/sub-netd-nic.md   (audit: hard)

- the **#140 MMIO co-residency resolution** — a PCI function's page-aligned BAR
  isolates net from blk at the MMU granule, dissolving the shared-virtio-mmio-page
  contention a long-lived netd would hit:

      vault/system/substrate/sub-substrate-machine.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold onto a dedicated audit:hard dossier.**
  sub-netdev carries every atom, including the audit hardenings the RW-7/RW-8 and
  pci-3 rounds produced (the `desc_id` OOB guard, the quiesce, the notify-region
  bound). The shared cross-cutting mechanisms it cites are home too: the
  `virtio_rmb` LoadLoad barrier (the F217 class, sub-virtio-probes) and the
  ISV-safe MMIO accessor (#890, now sub-libthyla-rs). The #140 resolution is
  sub-substrate-machine's transport-pivot. Zero code change.
