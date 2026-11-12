---
id: chg-2026-11-12-aux-display-follow
type: chg
title: "The guest takes a mode for any host size and follows its window"
date: 2026-11-12
arc: arc-mac-runner
commits: ["b8a18d11f", "3f454b436", "580b51d1e", "13c5e2a67", "c697bb6ab", "a48f32411", "7c4c13091", "99cd8d55f", "3d2e4b5ce", "5b73e3d3d", "2929c2a55", "4dd462eb8", "bfda0ba4c"]
touched:
  - sub-tapestryd
  - sub-lictor
  - sub-libhalcyon
  - sub-macrunner-app
  - sub-substrate-gates
established: []
closed: []
opened: []
mirrors-checked:
  - "docs/AUDIT-TRIGGERS.md has the row 'The display follows its host', and docs/agent/AUDIT-TRIGGERS-INDEX.md its line"
  - "docs/AURORA-CONFIG.md 3.4 says what `mode auto` and `mode W H` mean now; docs/GPU-DESIGN.md 4.5.15 says six stall shots"
  - "docs/MAC-RUNNER.md: B5 as built, and M-8 with the guest's half measured"
  - "docs/handoffs/050 is the record for Astra of what was changed in Lictor, with what was measured"
  - "abi-boot-banner lists the two new scenarios among the readers of `EXTINCTION:`"
  - "sub-macrunner-app no longer says the guest does not follow; its caveat on a window the guest cannot make a mode for is the present state"
  - "docs/phase7-status.md and docs/JOURNAL.md carry the chunk"
depth: skeletal
created: 2026-11-12
---
Follows [[chg-2026-11-11-aux-mac-guest-run]], which measured the two defects this closes. The guest's half of the Mac runner's resize station, on [[sub-tapestryd]], [[sub-lictor]] and [[sub-libhalcyon]]: at its start the compositor takes a mode that fits the host's size (the size over the first whole number, up to four, whose screen and full-screen surface can be made) and Lictor's trusted screen a size its renderer can draw; while it runs, Lictor counts the device's display events and the compositor follows them unless its mode is pinned. `docs/AURORA-CONFIG.md` 3.4 was amended for it under the operator-away grant, and Lictor was changed directly on the operator's word of 2026-11-11 with Astra away (`docs/handoffs/050`). [[sub-macrunner-app]] loses its caveat that the guest does not follow.

**Four audit rounds** (holotype-reviewer, Opus 5.5 at max, the same model at start and end): 0/0/1/6, 0/1/2/5, 0/0/1/4, 0/0/0/5 by P0/P1/P2/P3. Round 2's P1: a bind whose deadline passed may still be executed, so a change taken back could leave the device scanning a screen that was given up. What the device scans is now known where every scanout command passes, Lictor's client proxy (`scanout_unsettled`), and a pass binds again (`bind_pump`). Round 4's P3 fixes were not read by a reviewer. The do-not-report set is `audit_display_follow_closed_list.md` in the agents' memory.

**Shown.** On bfda0ba4c (QEMU 11.1.3, HVF, an M2 Air): `ls-display-stall` 7 passes, `ls-display-follow` 7, `ls-gfx-mode` 9; two red controls on images of their own (the pass without `bind_pump`; the proxy's flag never set), each green through the first stalled bind and red at the stalled switch; the bare image at 3456x2160 a session at 1728x1080, the Mac app's window followed with its frame QEMU's dump byte for byte; the kernel suite 2447 tests, none failed. On the two commits before: 640x400 and 5120x2880, each no compositor on the image from before; a told size on the scanout 214 ms and 282 ms later, one sample each; the same resize with the GPU's driver on the interrupt line. Host tests: libhalcyon 247 (10 for the display), lictor 65; 35 mutations, all red. [[sub-substrate-gates]] has what the stall scenario's first guest run met: QEMU's PPM is not a P6 at every width, and `tools/screendump.sh` now hands one out.

**Not in it.** One P3 of round 4 is open and older than this change: a closed surface's last GL frame can stay shown in the off state (it needs a GL host; the agents' OPEN-BUGS, 2026-11-11T23:55Z). The trusted screen knows no display scale (OPEN-BUGS 2026-11-11T18:55Z). No scenario reaches a stall the device stays in, an EDID question that is not answered, the direct state's repair, or an event under a pinned mode. The interrupt-line branch ran once and is in no gate. The full CI gate did not run (held since 2026-11-02).
