---
id: chg-2026-09-24-hi0-pointer
type: chg
title: "HI-0: native pointer through Lictor and surface shape preferences"
date: 2026-09-24
arc: arc-halcyon-interaction
commits: ["12af4d53"]
touched: [sub-lictor, sub-tapestryd, sub-libtapestry, sub-libhalcyon, sub-substrate-machine]
established: ["Bounded standard pointer shapes and a private VirtIO cursor lane"]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
The first interaction implementation adds shared scaled pointer geometry,
per-surface preferences and a semantic Lictor cursor request. GPU-only scene
pixels rule out erasing a software cursor from Tapestry's stale CPU mirror.
The VirtIO backend uses its cursor plane; the shared software helper is not a
claim that a future framebuffer backend is already qualified.

The lane permits one outstanding command. Descriptor reuse follows a checked
used entry, not an IRQ. Ambiguous completion retains DMA and prevents trusted
acknowledgement. Every output receives a transparent image then a hide before
trusted scanout, because QEMU VNC ignores visibility changes alone. Restore
invalidates the normal compositor's last-sent cursor state.

Source validation: the latest Pi run passes 274 host tests; affected crates
cross-check and the three guest programs link in release mode. Missing staged
theme fixtures and byte-argv literals were corrected before the green reruns.
The boot image built on reconciled Main `5857b6bf` (`08c26509`). Pointer
runtime passes on QEMU/KVM with both virtio-gpu-pci and virtio-gpu-gl-pci
(VirGL 3D compositor, EGL/VNC). Each verifies five shapes, both edges, the
restored divider, owner exit, SAK transparent exclusion and restoration. Eight
raw scene captures per backend retain the exact flat fixture pixels, with no
cursor trails baked into them. The original final assertion incorrectly expected
an arrow over the restored divider; the corrected assertion and movement into
a tile pass. Evidence is in `work/hi0-{2d,gl}-pass.log`, `hi0-pi-pixels.log`,
and screenshot directories `work/hi0-{2d,gl}`. No complete HI-0 claim is made. Application relative capture is a named follow-up:
the existing code has only divider capture, and relative motion does not
silently hide the cursor. Cursor waits also retain controlq's longer allowance
when a readback holds the shared device loop; latency still needs qualification.

All three accelerated graphical SAK scenarios subsequently passed, including
true expiry, five-failure lockout, held-chord failure/recovery and a fresh grant
after recovery (`work/hi0-pi-sak-gl.log`). The current reconciled source passes
274 host tests again. The separate KVM boot/probe gate passes, with production
compile, external Alpine and clade rows explicitly not covered by that image.
