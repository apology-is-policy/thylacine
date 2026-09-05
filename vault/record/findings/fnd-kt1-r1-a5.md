---
id: fnd-kt1-r1-a5
type: fnd
title: "the two audit-trigger surfaces' as-built references and the trigger table do not carry the new arm; two loop comments describe the mechanism that was removed"
round: adt-kt1-r1
severity: P3
status: fixed
surface: [sub-kernel-poll, sub-kernel-loom]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none (documentation)"
created: 2026-09-05
---
## Prosecution

**File**: docs/reference/107-loom.md (no `loom_poll` / `pollable` / KT-1.5 mention -- grep), docs/reference/72-poll.md (no KOBJ_LOOM mention), docs/AUDIT-TRIGGERS.md (no row; HALCYON.md:1492-1497 promised one "at implementation"), kernel/include/thylacine/poll.h:81 ("The two REAL registering paths are both KOBJ_SPOOR" -- now three), usr/halcyond/src/main.rs:734-737 ("this WAITS on the menu's ring" -- `MenuSet::service` is non-blocking, menuset.rs:115-160) and menuset.rs:109-110 ("the loop's `EventRing::wait` wakes for its keys" -- the loop no longer calls `wait`)
**Invariant**: CLAUDE.md doc-update-per-PR ("Missing docs are reverted along with their code"); the trigger table is cumulative scripture
**Prosecution**:
1. 15796866 updated HALCYON.md + JOURNAL; a85c94e4 updated HALCYON.md + 150-halcyond.md; ARCHITECTURE.md:3863 carries the pollable-Loom paragraph. Neither the Loom reference (107) nor the poll reference (72) describes `loom_poll`, the KOBJ_LOOM arm, the keep_out loom-ref retention, or the SQPOLL-only usefulness; AUDIT-TRIGGERS.md has no KT-1.5 row although 14.11.7a names it as owed.
2. The poll.h preamble's lifetime argument enumerates exactly two registering paths and derives the "transitive Spoor ref" safety from that enumeration; the Loom path is safe for a different reason (a direct `loom_ref`), which the text does not say.
**Suggested fix**: add the 107/72 sections + the AUDIT-TRIGGERS row in the KT-1 close; fix the three stale comments.

## Disposition

Fixed in 062efe18: the three stale comments (poll.h's registering-path list, halcyond main.rs's menu-wait comment, menuset.rs) rewritten; the two AUDIT-TRIGGERS rows (the pollable Loom; the kaua-term seam + the session compositor) appended with the CLAUDE.md index lines.
