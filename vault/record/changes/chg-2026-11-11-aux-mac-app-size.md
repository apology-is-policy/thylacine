---
id: chg-2026-11-11-aux-mac-app-size
type: chg
title: "The Mac app tells the guest the window's size"
date: 2026-11-11
arc: arc-mac-runner
commits: ["*(pending)*"]
touched:
  - sub-macrunner-app
established: []
closed: []
opened: []
mirrors-checked:
  - "docs/AUDIT-TRIGGERS.md's row for the Mac app names UiInfo::of, pixels, tell_size and size_changed, and carries the size telling's prosecution addendum"
  - "gate-macrunner-headless: proves, blind-to, invocation and method describe the third leg; the list of red controls has the eight new ones"
  - "docs/MAC-RUNNER.md M-8 has what was measured of B4 with no guest"
depth: skeletal
created: 2026-11-11
---
Follows [[chg-2026-11-11-aux-mac-app-audit]]. The resize station's half in the app, on [[sub-macrunner-app]]: `Console.SetUIInfo` with the view's pixels and the millimetres of as many points at 96 to the inch, told as the app attaches and a quarter of a second after the view's size last changed, never twice running and never for a view with no size.

**The witness has no guest.** `macrunner/app/test/as_guest.py` reads QEMU's virtio GPU over the qtest socket as a driver would (`GET_DISPLAY_INFO`, `GET_EDID`, the display event), and [[gate-macrunner-headless]] gains a third leg on it: a paused QEMU made 1024x768 answers 2560x1600 and 339x212 mm with the event pending after the app attaches, and 2000x1280 and 265x169 mm after a resize in three steps that is told once. Red on the commit before. QEMU 10.0.2 and 11.1.3, macOS 27.2.1; 32 unit tests; 34 red controls red, ten of them new (eight fixes switched off, two seen by both the script and the unit test), the unchanged copy green.

**Not in it:** an audit (round 3 reads this with round 2's fixes); a guest (whether Lictor boots at the size, and the scale it takes); the guest's half of a resize while it runs, since Lictor does not follow the display event; what a window larger than 3840x2160 should give a guest.
