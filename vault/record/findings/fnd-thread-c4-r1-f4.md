---
id: fnd-thread-c4-r1-f4
type: fnd
round: adt-thread-c4-r1
severity: P3
status: fixed
title: "A caught direct note made fatal after the post woke nobody"
surface: [sub-kernel-notes]
threatens: [inv-i9]
fixed-by: chg-2026-11-02-thread-model-c4
regression: "rendez.direct_note_turns_fatal_when_the_handler_goes; K25, K26, K33"
created: 2026-11-02
---
## Prosecution

ARCH 7.6.4's 'wakes any wait' held only at the post. `SYS_NOTIFY(0)` and `rt_sigaction` SIG_DFL left a peer in a hold wait asleep, because the unregistering thread's tail scans only its own word. Fixed: `proc_note_direct_rouse` from `notes_set_handler(0)` and `viv_sigaction_settled`. Applying it found a third store the finding did not name: `SA_RESETHAND` at a delivery, fixed by `notes_handler_resets`.
