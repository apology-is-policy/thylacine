---
id: chg-2026-09-06-devcap-doc-absorb
type: chg
title: "absorb docs/reference/75-devcap (hostowner-elevation cap device): clean redirect to sub-kernel-caps"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The kernel-side factotum-pattern hostowner elevation (the /cap device). An
I-2/I-22/I-27-adjacent SECURITY surface, so verified atom-by-atom before stubbing.
sub-kernel-caps (audit:hard, owns devcap.c) LAPS the doc -- it carries every atom:

- The two-phase grant gated on different authorities (/grant on CAP_GRANT_
  HOSTOWNER-or-clearance; /use on CONSOLE_ATTACHED + a matching pending grant).
- The pending-grant table + 30s expiry + lazy sweep + re-register-in-place.
- The equality-not-subset redeem rule (line 98: "must be equal | must be a
  subset") + no-consume-on-failed-gate (104, 198) + one-shot consume.
- The stripes binding + cap_proc_exit_notify cleanup.
- The two-trust-domain defense-in-depth (corvus can register arbitrary stripes
  but only a console-attached writer redeems -> a corvus compromise is bounded to
  the local console; corvus.tla HostownerRequiresConsole).
- The seam-devcap-plain-caps-read (register gates read the plain caps word).

Zero-fold. The doc's "pending fixup / P5-hostowner-b-a status" framing is stale
(the userspace consumer landed). Redirect stub -> sub-kernel-caps + sub-kernel-perm.
