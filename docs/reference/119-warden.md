# 119 — warden: the Menagerie hardware broker [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-warden-doc-absorb`).
The **warden** — the TCB component that turns raw hardware-discovery sources into
capability-sandboxed driver Procs: it reads the device inventory for
*information*, intersects each node's resources with the matched manifest's
`needs` to compute the **narrowed allowance** (the auditable I-34 grant), and
spawns the driver with exactly that allowance (the *authority*, which the kernel
enforces — a driver fabricating a PA outside its allowance is rejected by the I-34
gate, not the warden). No new kernel ABI. Its content lives, code-verified and
current, in:

- **the broker engine** — parse manifests, discover via sources, match+grant,
  confer+run, supervise (the bounded restart-on-crash `next_step`, the
  DeviceRemoved revoke-first-then-terminate teardown, the DMA-safe
  quiesce-before-block, the #926 `await_readiness`-without-EOF, the #230
  unconditional boot placement, the one-hop `MAY_POST_SERVICE` delegation, and
  the I-34 fourth-leg reasoning — the leg the kernel cannot check because its
  own conferrer-comparison is *vacuous* for the unnarrowed broker):

      vault/system/userspace/boot-chain/sub-warden.md   (audit: hard, I-34)

- **the leaves the warden hands a device to** — `virtio-mmio-source` (the
  sandboxed non-TCB `DeviceID`-poke enumerator + the reconcile trust-boundary: a
  compromised source can mis-identify a slot but **never** fabricate a reg/INTID to
  inflate a grant, the warden rebuilding from its own trusted view), `netdev-driver`
  (the long-lived serve → READY → teardown lifecycle), and — **now folded** —
  `menagerie-probe` (the fixture I-34 proof, positive map + negative
  `0xDEAD` out-of-grant reject):

      vault/system/userspace/hardware/sub-menagerie-leaves.md   (I-34)

- **the framework the broker + leaves are built on** — the grant arithmetic
  (`resolve`/`to_allowance`/`to_descriptor`, the page-rounded sub-page MMIO grant +
  the #140 co-residency over-grant) and the discovery/supervise/reconcile pure
  logic (incl. the 5e-4 F1 bounded-not-byte-blocking READY read — a partial-line
  driver stalling the TCB forever was a boot-availability DoS):

      vault/system/userspace/runtime/sub-libdriver-grant.md
      vault/system/userspace/runtime/sub-libdriver-discovery.md

- **the kernel I-34 gate + the invariant** — the confer-at-spawn ABI, the
  `SYS_PCI_CLAIM` bdf-axis gate (the live I-34-on-PCI proof: `netdev-pci-driver`
  narrowed to just its `(bus,dev,fn)` + INTID, no MMIO axis), the revoke-first
  atomicity (#160):

      vault/invariants/inv-i34.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One genuine gap, now folded — `menagerie-probe` was UNOWNED.** The canonical
  I-34 mechanics demo (the grant round-trip positive + the `0xDEAD_0000`
  out-of-grant *deliberate* negative — the one leaf that proves the gate *denies*
  where the others prove it *admits*) was detailed nowhere, and the file was in no
  code list. Now the fixture third leaf in `sub-menagerie-leaves`
  (`chg-2026-09-07-warden-doc-absorb`).
- **Everything else was covered** — the 5e-4 F1 partial-line DoS
  (sub-libdriver-discovery), the 5d-4 reconcile trust-boundary
  (sub-menagerie-leaves + sub-libdriver-grant), the #230/#160/#926/PCI atoms
  (sub-warden + inv-i34). The doc's "no useful driver yet through 5d-2" and
  "compiled-in bind DB / the sig ladder unbuilt" are as-built history and live
  seams the dossiers carry. Zero code change.
