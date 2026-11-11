---
id: chg-2026-11-11-aux-mac-app-audit
type: chg
title: "The Mac app after its first window: the guest's pointer, and two audit rounds"
date: 2026-11-11
arc: arc-mac-runner
commits: ["985bbb42c", "8421fd848"]
touched:
  - sub-macrunner-app
established: []
closed: []
opened: []
mirrors-checked:
  - "docs/AUDIT-TRIGGERS.md's row for the Mac app names the new functions and carries round 2's prosecution addenda"
  - "gate-macrunner-headless: proves, blind-to, method and the list of red controls match the 60-line script"
depth: skeletal
created: 2026-11-11
---
Follows [[chg-2026-11-11-aux-mac-app]]. Two commits on [[sub-macrunner-app]]: the guest's pointer, which the first window did not draw, with audit round 1 (0 P0, 1 P1, 3 P2, 8 P3); then the pointer's alpha (multiplied in: the guest's rasteriser says so, and the app had followed QEMU's Cocoa display) with audit round 2 (0 P0, 0 P1, 2 P2, 7 P3). Every finding is dispositioned; the closed list is in the session memory (`audit_mac_app_closed_list.md`).

**Measured without a guest** (QEMU 10.0.2 and 11.1.3, macOS 27.2.1): 31 unit tests; [[gate-macrunner-headless]] checks the picture, 60 trace lines and the pointer on both pixel paths; 24 red controls are red and the unchanged copy is green.

**Owed:** the guest measurement (`docs/MAC-RUNNER.md` M-3), now with the guest's pointer as a guest defines it; audit round 3 on round 2's fixes, batched with the resize station; a person's look at the glass.
