---
id: view-closed-sub-kernel-poll
type: view
title: "Do-not-re-report preamble — sub-kernel-poll"
query: closed:sub-kernel-poll
---
# Do-not-re-report preamble — sub-kernel-poll

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-poll`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH two standing facts about this surface:

- **The F3 → 2C-F1 pair is the surface's history lesson**: a P1
  closed by documenting a single-thread precondition, voided by the
  multi-thread lift, detonated at RW-2, closed structurally by the
  retain. Any disposition of the form "safe because only one thread
  does X" on this surface must name the tripwire that fires when X
  stops being true.
- **The retain has a known-inert kind**: KObj_Srv listener polls pin
  nothing ([[seam-poll-srv-registry-retain]]) — safe only while the
  boot registry is immortal. A prosecutor finding this again has
  found the seam, not a new bug.

<!-- generated:begin -->
7 closed findings on [[sub-kernel-poll]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-kt1-r1-a5]] [P3] the two audit-trigger surfaces' as-built references and the trigger table do not carry the new arm; two loop comments describe the mechanism that was removed (fixed) — Fixed in 062efe18: the three stale comments (poll.h's registering-path list, halcyond main.rs's menu-wait comment, menuset.rs) rewritten; the two AUDIT-TRIGGERS rows (the pollable Loom; the kaua-term seam + the session compositor) appended with the CLAUDE.md index lines.
- [[fnd-poll-r1-f1]] [P2] A client polling its own srv connection got the SERVER endpoint's revents (fixed)
- [[fnd-poll-r1-f2]] [P2] Teardown latched the two EOF flags under separate locks — a poll between saw half a hangup (fixed)
- [[fnd-poll-r1-f3]] [P1] The handle-slot borrow across the scan — doc-fixed on a precondition the lift later voided (fixed)
- [[fnd-poll-r1-f4]] [P1] A NULL-obj KOBJ_SPOOR slot reached the Dev dispatch (fixed)
- [[fnd-rw2-2cf1]] [P1] A registered poll waiter outlives the obj ref — sibling-close mid-sleep frees the hook list (fixed)
- [[fnd-rw2-r2poll-f1]] [P3] The retain is INERT for KObj_Srv — listener-poll safety rests on the boot registry's immortality (documented) — The overclaiming comment fixed; the obligation tracked as
<!-- generated:end -->
