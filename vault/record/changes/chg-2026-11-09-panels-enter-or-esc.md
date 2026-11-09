---
id: chg-2026-11-09-panels-enter-or-esc
type: chg
title: "Trusted panels close on Enter or Esc; the key prompt runs on its own clock (vote 70)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-corvus
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Operator vote 70 ([[dec-2026-11-05-panels-enter-or-esc]]) closes vote 50 audit round 3's F1. Corvus's `console_wait_any_key` became `console_wait_dismiss`: Enter (CR or LF) or Esc closes a panel, the bound closes it too, and every other byte is swallowed, up to 4096 so a broken clock cannot leave the loop unbounded. The key prompt is timed 60 s from the SAK instead of the request's remainder, a SAK past the request's window answers TIMEOUT before the provincia, and the post-verification "request expired" check is gone. provincia.rs and Lictor's render.rs say "press Enter"; IMPERIUM-DESIGN section 11's items 9 and 13 and the manual's section 18 follow. Witnesses, each red on the old code: im1-sak-lever and ls-graphical-sak-states type a letter at a panel and require it to stay, then Enter; im3-lex-curiata's arm 4b presses the SAK 52 s into the request's window and types the key 12 s later.
