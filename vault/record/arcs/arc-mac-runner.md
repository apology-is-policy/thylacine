---
id: arc-mac-runner
type: arc
title: "Thylacine for Mac: a Dock icon, a window, a hidden patched QEMU under HVF"
status: active
design: ["docs/MAC-RUNNER.md"]
chunks:
  - chg-2026-11-10-aux-mmio-forms
  - chg-2026-11-10-aux-device-windows
  - chg-2026-11-11-aux-mac-app
follow-ons: []
exit-criteria:
  - "[ ] a stranger installs it from the DMG and uses Halcyon for an hour, retina-crisp, copy and paste both ways (MAC-RUNNER.md section 8, Alpha)"
created: 2026-11-10
---
## Goal

Thylacine useful on macOS the way a desktop VM product is: a DMG, a Dock icon, a window that goes
fullscreen, and the Mac served to the guest as a 9P tree. A Rust app over a hidden, patched QEMU
under HVF ([[dec-2026-11-10-mac-runner]]; `docs/MAC-RUNNER.md` holds the research, the design and
the measurement plan).

## Planned chunks

The phasing is `docs/MAC-RUNNER.md` section 8 (Alpha: window, 2D, input, resize, clipboard, one
shared folder; Beta: 3D, audio, suspend, updates; Product: Venus, pacing, policy). The first two
chunks are kernel fixes the arc's first measurement found: the guest's own bug that kept GICv3
from booting under HVF, and the device-window holes its audit led to. The third is the app's first
window ([[sub-macrunner-app]]), attached to a QEMU somebody else started.

## Close summary
(written at status flip to complete)
