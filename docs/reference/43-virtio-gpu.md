# 43 — virtio-gpu userspace driver (P4-L + scanout) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-virtio-gpu-doc-absorb`).
`/virtio-gpu` — the fourth composed userspace driver (after virtio-blk, the
virtio-net family, and virtio-input) — proves the composed-hw-handle SVC substrate
(MMIO + DMA + IRQ) generalizes to the VIRTIO GPU device class (DeviceID 16) and
drives the full 2D scanout pipeline (the substrate gate for the graphical shell).
Its content lives, code-verified and current, in:

- the **/virtio-gpu probe driver + the composed-hw-handle substrate + the 2D
  scanout pipeline** — the shared MMIO-bank claim / DTB slot match / DMA
  coherence / IRQ delivery the four composed drivers exercise as one boot-gate,
  the flat le32 config-space (`num_scanouts`/`num_capsets`), and the six-OK 2D
  scanout contract (create resource → record backing → bind scanout →
  transfer-to-host → flush, each `OK_NODATA`):

      vault/system/userspace/hardware/sub-virtio-probes.md   (owns usr/virtio-gpu/src/main.rs)

- the **current GPU compositor** that superseded the probe as the production GPU
  driver:

      vault/system/userspace/services/sub-tapestryd.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The P4-L `/virtio-gpu` driver is
  a boot-gate probe now, owned by `sub-virtio-probes` (which carries all four
  composed drivers as the tree's only end-to-end MMIO/DMA/IRQ capability
  exercise); the production GPU path is `sub-tapestryd`'s.
- **Visual verification was never in CI scope** (the doc notes it: `run-vm.sh` runs
  `-nographic`); the six-OK contract is the proxy the probe asserts.
