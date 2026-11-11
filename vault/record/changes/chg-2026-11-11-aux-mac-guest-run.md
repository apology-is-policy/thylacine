---
id: chg-2026-11-11-aux-mac-guest-run
type: chg
title: "The Mac app measured with a guest, and a boot that has no rule for the host's size"
date: 2026-11-11
arc: arc-mac-runner
commits: ["pending"]
touched:
  - sub-macrunner-app
established: []
closed: []
opened: []
mirrors-checked:
  - "docs/MAC-RUNNER.md M-3 has the figures with a guest; M-8 has the size at boot and the measured defect"
  - "sub-macrunner-app: Performance has the guest's figures; the caveat on a window the guest cannot start at names what was measured and what was read"
  - "docs/phase7-status.md and docs/JOURNAL.md carry the run"
depth: skeletal
created: 2026-11-11
---
Follows [[chg-2026-11-11-aux-mac-app-size]]. No code in this change: the guest measurement that the last three changes on [[sub-macrunner-app]] were waiting for, run under one lease of 16 minutes (five boots of the Halcyon session image, QEMU 11.1.3 under HVF, an M2 Air on macOS 27.2.1, the app at `c045b8f24`).

**What holds with a guest.** The app's picture is QMP `screendump` byte for byte at 2560x1600 on both pixel paths, in the product posture, and in a boot the app itself sized; the Mac's pointer wears the guest's (64x64, hot spot 4,4); an upload averages 1.2 ms in a settled session; from a key to the guest's next damage the median is 11.7 ms over 18 keys. A size told before the guest starts is the size it starts at, and the guest takes 200% from the millimetres with nothing on its command line.

**What does not.** A guest started at 3456x2160 has no session and no display: the first full-screen surface is three buffers, 89.6 MB, against the kernel's 64 MiB a weave, and the boot (unlike tapestryd's `mode`) has no rule for a size it cannot make. Read, not run: Lictor's probe fails under 800x720 or over 4096x2160 pixels in all. The app can reach both, and so can QEMU's own windows. Both are owned and are the guest's to fix, in the resize station's guest half.

**Not in it:** the fix; an audit of anything (round 3 on the app reported after the run: no P0, P1 or P2, seven P3 to dispose of); a look at the panel; the CPU the app and QEMU use.
