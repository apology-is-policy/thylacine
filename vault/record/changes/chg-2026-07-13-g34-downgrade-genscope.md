---
id: chg-2026-07-13-g34-downgrade-genscope
type: chg
title: "G3 perm-valid attr downgrade + G4 qid-scoped gen guard"
date: 2026-07-13
arc: arc-go-build
commits: ["49d4f9de"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The T4-M-measured wga re-walk band (1104 mid-hop attr misses + 726-886
gen-guard-lost installs per S3 window) closed guest-side: child mutations
DOWNGRADE the parent attr to a perm-servable core (a create cannot edit
the parent's mode/uid/gid -- the X-check stays RPC-free while leaf
consumers refetch), and the install gen guards become qid-scoped via a
128-slot invalidation-event ring (fail-safe global skip on overflow). The
soundness obligation recorded: EVERY mutation logs EVERY qid it stales.
