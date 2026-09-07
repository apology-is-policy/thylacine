---
id: chg-2026-09-06-admin-elevate-doc-absorb
type: chg
title: "absorb docs/reference/76-admin-elevate (corvus ADMIN_ELEVATE + C-22 gating): clean redirect to sub-corvus + sub-kernel-caps, superseded hardcoded-passphrase named"
date: 2026-09-06
arc: arc-vault
commits: ["fc9e3c46"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The userspace half of hostowner elevation (corvus ADMIN_ELEVATE verb + joey
redemption + C-22 admin-verb gating). A partially-updated doc; verified atom-by-
atom against the code + sub-corvus.

ALREADY COVERED (verified, not assumed):
- ADMIN_ELEVATE (token -> live console re-query -> the REAL Argon2id + AEGIS
  system-passphrase unwrap -> t_cap_grant), the C-22 gating (fresh t_srv_peer per
  admin call, fail-closed on dead/failed query; the snapshot deliberately does not
  authorize), the first-user bootstrap exception (a corrupt db aborts the boot,
  not re-bootstrap) -> sub-corvus (:144/:150-160/:166/:242-244/:457).
- SYS_CAP_GRANT=32 / SYS_CAP_USE=33 + T_CAP_HOSTOWNER (1<<3) / T_CAP_GRANT_
  HOSTOWNER (1<<4) + the two-trust-domain devcap gate -> sub-kernel-caps.
- The spec (corvus.tla AdminElevate / HostownerRequiresConsole /
  AdminRequiresProcCap).

SUPERSEDED CLAIM NAMED: the doc is partially updated -- sections 48-50 + caveat
214 say SYSTEM_PASSPHRASE is the hardcoded "thylacine" byte string, but the doc's
own line 69 notes A-5c-b replaced the byte-compare with a real Argon2id + AEGIS
unwrap, and the current code carries NO such constant (verified: 0 hits for
SYSTEM_PASSPHRASE/b"thylacine" in usr/corvus/src). The "anyone with source access
can elevate" caveat died with the byte-compare. sub-corvus:244 documents the live
Argon2id + AEGIS mechanism.

Zero-fold. Redirect stub.
