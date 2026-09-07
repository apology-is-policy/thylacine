# 118 — libdriver: the Menagerie driver framework crate [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-libdriver-doc-absorb`).
The scaffold that makes a Thylacine driver *droppable* and its I-34 grant
*auditable* — `probe()` + device logic + a served file, with the node-INTERSECT-needs
grant computation in one host-tested place. `BoundResources` is the shared currency:
one value the warden computes+encodes and the driver decodes, so the authority the
kernel enforces and the resources the driver maps never diverge. Its content lives,
code-verified and current — across two audit:hard dossiers — in:

- **the grant + the codec + `to_allowance`** (`manifest`, `resource`) — `resolve`'s
  auditable I-34 property (the manifest *selects* an axis, the node *supplies* the
  values, so a grant can never exceed the device; `resolve_grant_never_exceeds_node`),
  the one-argv-slot descriptor codec (strict parse, the `;`-delimiter-injection
  guard), the page-rounded MMIO allowance (the #140 co-residency over-grant), the
  authority-vs-information split (the kernel I-34 gate is the boundary, not the
  codec — libdriver is *not* a privilege surface), and the PCI axis (`PciNeed::Node`
  → the bound function's bdf, no MMIO, the 6a `push_pci` gate):

      vault/system/userspace/runtime/sub-libdriver-grant.md   (audit: hard, I-34)

- **the discovery / supervise / readiness layers** (`source`, `dtb`, `supervise`,
  `readyline`) — the typed `DeviceId` (`VirtioPci` deliberately distinct from
  `Virtio` — disjoint claim paths, disjoint prefixes, so `parse` cannot confuse them
  and a manifest binds exactly one), `best_match`, the `DtbSource`/`PciSource`, the
  big-endian cell-width DTB decode, the `next_step` restart-vs-settle state machine
  with exponential back-off, `feed_ready_line` (the 5e-4 F1 pure half — a partial
  line never blocks the reader mid-line), and the **`reconcile_reported_node` trust
  boundary** (a non-TCB source supplies identity, the warden rebuilds resources from
  its own trusted view — a source can mis-identify but never fabricate a reg/INTID to
  inflate a peer's allowance; the "three layers each trusting its predecessor less"
  spine):

      vault/system/userspace/runtime/sub-libdriver-discovery.md   (audit: hard, I-34)

- **the consumers + the kernel gate** — the warden (the grant computation's live
  driver), the leaves (`menagerie-probe`/`netdev-driver`/`virtio-mmio-source`), and
  the kernel I-34 gate the allowance feeds:

      vault/system/userspace/boot-chain/sub-warden.md
      vault/system/userspace/hardware/sub-menagerie-leaves.md
      vault/invariants/inv-i34.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold onto two audit:hard dossiers, one
  AHEAD of the doc.** The 5e-4 F1 partial-line DoS (readyline), the 5d-4 reconcile
  trust-boundary, the grant-never-exceeds-node property, the page-round/#140
  over-grant, the delimiter-injection guard, the `VirtioPci`-distinct PCI axis, and
  the supervise state machine are all home. `sub-libdriver-grant` is *ahead* — it
  carries a multi-bdf grant ("the gathered extra bdfs") the doc's single-bdf codec
  did not describe. The MSI-carried-not-resolved, whole-node-MMIO-selection, and
  `sig`-carried-not-verified seams are live v1.x seams the dossiers hold. Zero code
  change.
