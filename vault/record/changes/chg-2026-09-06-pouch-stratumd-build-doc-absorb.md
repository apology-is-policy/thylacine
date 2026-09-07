---
id: chg-2026-09-06-pouch-stratumd-build-doc-absorb
type: chg
title: "absorb docs/reference/85-pouch-stratumd-build (P6 stratumd cross-build recipe): clean redirect; the UNOWNED cross-toolchain file noted"
date: 2026-09-06
arc: arc-vault
commits: ["b2f1c77c"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
P6 sub-chunk 15 -- cross-compiling stratumd against the pouch sysroot. NOT
audit-bearing (POUCH-DESIGN 14 row 15). A build-recipe + Stratum-coordination doc;
verified atom-by-atom.

WHERE EACH ATOM LIVES (verified, not assumed):
- The build orchestration (all -> kernel -> {..., stratumd, ...}, the sysroot
  caching + the sysroot_is_stale 80-vs-72-byte-buffer lesson, build_stratumd) ->
  sub-substrate-build (:30/:61-79).
- The one Thylacine arm (peer_creds.c __thylacine__ reusing the Linux SO_PEERCRED
  body, which pouch's 0006-pouch-sockets marshals onto SYS_srv_peer) ->
  sub-pouch-net.
- The deployment (joey spawns stratumd + ramfs->/sysroot pivot, the audit-bearing
  sub-chunk 16 follow-up) -> sub-stratum-boot + sub-stratum-session.
- The pouch-ld link-driver rationale (clang mis-selects the host Darwin toolchain
  for unknown OS triples) -> sub-pouch-seam.

Zero-fold. NOTED not folded: cmake/Toolchain-aarch64-pouch.cmake is UNOWNED (the
-D__thylacine__/_GNU_SOURCE/nostdlibinc/pouch-ld toolchain) -- a build-recipe file
whose substantive rationale is in sub-pouch-seam, a candidate for a broader
pouch-build-substrate dossier (one of the uncovered surfaces). The v1.0
SO_PEERCRED-returns-uid=0/gid=0 lossiness is a known limitation. Redirect stub.
