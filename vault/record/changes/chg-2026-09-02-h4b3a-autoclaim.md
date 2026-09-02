---
id: chg-2026-09-02-h4b3a-autoclaim
type: chg
title: "H-4b-3a: libtapestry auto-consumes an inherited TAPESTRY_CLAIM on open"
date: 2026-09-02
arc: arc-tapestry
commits: ["75365f95"]
touched: [sub-libtapestry]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-02
---
The client half of layout restore (13.7's opaque cookie). `Surface::open`
auto-consumes an inherited `TAPESTRY_CLAIM` from `/env` (`take_env_claim`,
guest-gated), upgrading a Content mint to `Claim(token)` so a restored program
lands in the tool's target leaf without knowing about placement. One-shot per
process via a `CLAIM_TAKEN` latch, though correctness does not depend on it (the
server-side consume is already one-shot -- a spent/inherited-stale token falls
back to focus). NO-OP for a normally-launched program (no var). Proved a no-op
for existing clients by ls-gfx-panes 33/33 (the battery opens as before). The
restore TOOL + skeleton (H-4b-3b) carry the ACTIVE-path E2E. [[sub-libtapestry]]
gains "The restore auto-claim".
