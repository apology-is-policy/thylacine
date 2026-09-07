# 42 — virtio-input userspace driver (P4-K + P4-K-events) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-virtio-input-doc-absorb`).
`/virtio-input` — the third composed userspace reference driver (VIRTIO input
class, DeviceID = 18, keyboard), proving the composed-hw-handle SVC substrate
(MMIO + DMA + IRQ) generalizes to a new device class and consumes injected host
events end-to-end. Its content lives, code-verified and current, in:

- the **reference-driver dossier** — the selector-based device-specific config
  space (VIRTIO 1.2 §5.8.4: driver writes `select`/`subsel`, device fills
  `size`/`u`), the RX-only eventq on queue 0, the 8-byte event records, the drain +
  descriptor recycle, the **F217 read barrier** (the LoadLoad barrier after
  observing `used.idx` before reading the used-ring entry and buffer — without it
  an out-of-order Cortex-A core speculatively reads the pre-advance zeroed pool and
  misclassifies the key as a phantom EV_SYN), and the **#362 three-second
  wall-clock poll budget** (replacing the substrate-speed-dependent iteration cap):

      vault/system/userspace/hardware/sub-virtio-probes.md   (audit: light)

- the **gate that keys on it** — `sub-substrate-gates` keys on `virtio-input: SKIP`
  and the QMP key-injection round-trip:

      vault/system/substrate/sub-substrate-gates.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** sub-virtio-probes carries every
  atom, including the two the P4-Z audit hardened: the F217 read-barrier-after-
  used-idx (line 132-133) and the #362 wall-clock poll budget (line 193). The
  selector config, the RX-only eventq, and the can't-hang property are all there.
- **The production keyboard is elsewhere now** — this probe is the P4-era substrate
  scaffold; the live keyboard path is tapestryd's (it gathers `virtio-pci:18`
  alongside the GPU). The doc's own note that it "does NOT close ROADMAP §6.2"
  (keyboard via `/dev/cons`) remains accurate: the reference driver proved the
  device class works, and the compositor owns the production surface. Zero code
  change.
