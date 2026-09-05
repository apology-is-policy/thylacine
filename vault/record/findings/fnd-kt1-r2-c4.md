---
id: fnd-kt1-r2-c4
type: fnd
title: "HALCYON.md 14.12 step 4's body still states the retracted principal-keyed trigger; the amendment is appended under the sequencing bullet, so a reader of the model paragraph gets the old rule"
round: adt-kt1-r2
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none (scripture)"
created: 2026-09-05
---
## Prosecution

**File**: docs/HALCYON.md:1620-1640 (step 4: "a SESSION leaf (its owner_principal a real user principal -- the actor() test) outranks SYSTEM leaves ... When a session leaf is visible, the SYSTEM leaves are BACKGROUNDED ... with no session leaf present (every pre-session + gfx-test path) nothing backgrounds"), :1692-1710 (the 2026-09-05 AMENDED paragraph under KT-1.5d-1b)
**Invariant**: scripture consistency (the audit-policy order: spec -> technical reference -> code; a doc that states two rules for one mechanism is the "comment true about the wrong version" class).
**Prosecution**: step 4 is the MODEL section future readers cite; it names `owner_principal` + `actor()` as the trigger and the "session leaf visible" condition, both false since 488cab49 (`has_session_tree` keys on `session_conns`, visibility-independent). The amendment is correct but lives 60 lines down inside the implementation-stage list. A reader implementing against step 4 (e.g. the C-F11 renderer-chrome follow-up) re-introduces the round-1 trigger.
**Suggested fix**: rewrite step 4's trigger sentence to "a DECLARED session conn (`session on`) hosts a leaf" and cross-reference the amendment; keep the amendment as the change record.

## Disposition

Fixed in the round-2 close: HALCYON.md 14.12 step 4's model paragraph now states the DECLARED trigger, the takeover rule, and the undeclared fallback; the d-1b amendment records the round-2 change under the round-1 one.
