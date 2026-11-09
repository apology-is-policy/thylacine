---
id: dec-2026-11-05-panels-enter-or-esc
type: dec
title: "Trusted panels close only on Enter or Esc, and the key prompt is timed from the SAK"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-corvus, sub-lictor, sub-imperium]
created: 2026-11-05
---
## Fork

Vote 50's audit round 3 (F1, P1, inherited, read in the code) found that a program in the focused pane could make the rest of a person's imperium key reach it. Its request's 60-s window ran on into the key prompt, which got only the window's remainder; the requester could ask the person to press the chord just before the deadline, and the timeout verdict then came up mid-typing. Any key closed a panel, so the next character ended the episode and the rest of the key went to the focused surface, the requester, which lost nothing by timing out and could ask again. On serial there was not even a wait.

## Decision

Operator vote 70: panels (a verdict, nothing pending, locked) close only on Enter or Esc, or their bound, and swallow every other byte; the key prompt gets a fixed 60 s from the SAK. Serial and graphical alike. The alternatives were Enter or Esc while keeping the window's remainder for the prompt, and any key with a quiet interval of about a second after a panel.

## Consequences

The request's window now covers only the request to the SAK: a SAK past it answers `Timeout` with no prompt, and the old check after verification ("request expired") is gone, so a key entered within the prompt's own time is not refused for the window having passed meanwhile. Arrow keys on serial begin with Esc, so one closes a panel and the rest of its sequence goes to the restored surface; no secret is typed with them. The manual's "press a key" became "press Enter". Witnesses: im1-sak-lever and ls-graphical-sak-states type a letter at a panel and require it to stay, then Enter; im3-lex-curiata's late-SAK arm confers a key typed past the request's window.
