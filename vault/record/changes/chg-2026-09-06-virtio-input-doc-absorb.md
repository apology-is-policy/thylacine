---
id: chg-2026-09-06-virtio-input-doc-absorb
type: chg
title: "absorb docs/reference/42-virtio-input (P4-K virtio-input reference driver): clean redirect to sub-virtio-probes + sub-substrate-gates"
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
The third composed userspace reference driver (VIRTIO input class, DeviceID=18,
keyboard). sub-virtio-probes (audit:light, owns virtio-input/main.rs) LAPS the
doc; verified atom-by-atom.

ALREADY COVERED (spot-checked, not assumed):
- The selector-based config space (VIRTIO 1.2 5.8.4) + the RX-only eventq on
  queue 0 -> sub-virtio-probes:105.
- The F217 P1 read barrier: the LoadLoad barrier after observing used.idx before
  reading the used-ring entry + buffer (without it an out-of-order Cortex-A
  speculatively reads the pre-advance zeroed pool -> phantom EV_SYN misclass) ->
  sub-virtio-probes:129-133 ("A read barrier after observing the used index. Every
  drain reads the used index, then issues the barrier, then reads the used-ring
  entry and the buffer").
- The #362 three-second wall-clock poll budget (replacing the substrate-speed-
  dependent iteration cap) -> sub-virtio-probes:193; the can't-hang property :242.
- The gate keying on "virtio-input: SKIP" + the QMP key-injection round-trip ->
  sub-substrate-gates:74.

Zero-fold. The production keyboard is tapestryd's now (it gathers virtio-pci:18);
this probe is the P4-era substrate scaffold. The doc's "does NOT close ROADMAP
6.2" (keyboard via /dev/cons) note remains accurate. Redirect stub.
