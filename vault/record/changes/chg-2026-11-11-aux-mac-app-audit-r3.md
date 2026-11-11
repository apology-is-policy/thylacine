---
id: chg-2026-11-11-aux-mac-app-audit-r3
type: chg
title: "The Mac app: audit round 3's seven findings"
date: 2026-11-11
arc: arc-mac-runner
commits: ["*(pending)*"]
touched:
  - sub-macrunner-app
established: []
closed: []
opened: []
mirrors-checked:
  - "docs/AUDIT-TRIGGERS.md's row for the Mac app carries round 3's addendum: what the guard in UiInfo::of is for, the two ways QEMU's EDID carries a size, the modifier rule's guess, the settling of an event over the bars"
  - "gate-macrunner-headless: proves, blind-to, invocation and method match the 73-line script and the size leg's second run (4200x700 points, then a point wider and back); the list of red controls has round 3's"
  - "docs/MAC-RUNNER.md B4 has QEMU's large-mode EDID; M-8 has it measured with no guest"
  - "sub-macrunner-app: Contract, Mechanism, Prosecution and Caveats say what was shown and what was read"
depth: skeletal
created: 2026-11-11
---
Follows [[chg-2026-11-11-aux-mac-guest-run]]. Audit round 3 on [[sub-macrunner-app]] (Opus 5.5 at max, the same model at start and end, on a copy of `c045b8f24`): no P0, P1 or P2, and seven P3, all of them a document, a comment or a test that said more than was shown. This change disposes of the seven and of three items found beside them. One behaviour was weighed and left as it is, with the reason written down; nothing the app sends to QEMU changed.

**What changed.** The dossier no longer says QEMU puts millimetres in every EDID: for a mode of 4096 pixels a side, or from 6.25 megapixels, QEMU has whole centimetres and no detailed timing, and the size leg now reads one (4200x700 points told, `111x18 cm` answered, on QEMU 10.0.2 and 11.1.3). The unit test for what a guest makes of a size told compiles the guest's own `usr/lib/libhalcyon/src/scale.rs` in, over a model of both of QEMU's encodings: exact from 22 points a side to two metres at one and two pixels to a point, a side under a centimetre tried only against one of no more than 640 points. The script grew from 60 lines of trace to 73: a modifier whose release never came is let go of by a move, by a click over the bar and by a scroll over the bar, the last two reported by the view because the trace cannot show them; and the two Shifts without bits of their own, where a release with the twin down is sent as a press. That last is the rule's guess, kept: the other guess loses a press that was real. A size that changes and comes back is shown not to be told again by the script itself, not by what AppKit happens to do at setup.

**Controls** (in [[gate-macrunner-headless]]): 34 fixes switched off, 37 red results, the unchanged copy green in both kinds.

**Not in it:** the guest's half of the resize; a look at the glass; a person at the keyboard.
