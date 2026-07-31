---
id: moc-kernel-ninep
type: moc
title: "Kernel 9P stack"
parent: moc-kernel
created: 2026-07-31
updated: 2026-07-31
---
The kernel-side 9P2000.L stack: wire codec, session state machine, transport
backends, the shared multi-Proc client, the dev9p Dev that mounts it into
namespaces, the dev9p.poll readiness bridge, the Larder guest-side cache, and
the attach/srvconn glue. Every FS mount (Stratum system FS, per-user homes,
netd `/net`, corvus) resolves through this stack.

## Children

Bottom-up through the stack:

- [[sub-kernel-ninep-wire]] — the stateless byte codec (builders/parsers,
  strict framing, the extension-op registry).
- [[sub-kernel-ninep-session]] — the tag/fid state machine + the
  flush/abandon retirement rules (I-10/I-11's enforcement site).
- [[sub-kernel-ninep-transport]] — the frame-aware core + the four
  backends (srvconn production; spoor; loopback + mq test).
- [[sub-kernel-ninep-client]] — the shared elected-reader client
  (pipelining, flow control, frame-atomic recv). **The pilot dossier.**
- [[sub-kernel-ninep-attach]] — mount creation (`p9_attached` +
  `srvconn_attach_dev9p_root`; the refcounted session holder).
- [[sub-kernel-ninep-dev9p]] — the Dev: walk/IO/mutation + the Larder
  policy + write-behind + cached-open + the weft arms.
- [[sub-kernel-ninep-dev9p-poll]] — the readiness bridge + the global
  poll-pump kthread (the net arc's one kernel ABI).
- (sweep-pending: the Larder mechanism (`kernel/larder.c`,
  `docs/reference/132-larder.md`) · netd's server half
  (`docs/reference/121-netd.md`).)

## Cross-cutting

- Invariants: [[inv-i9]] · [[inv-i10]] · [[inv-i11]] · [[inv-i38]].
- Specs: [[spec-9p-client]] · [[spec-reader-frame]] · [[spec-net-poll]] ·
  [[spec-net-poll-teardown]] · [[spec-fs-cache]].
- Lineage: [[lin-9p-client]] — the #841→#90 hardening saga.
- Hazards: [[haz-shared-stream-desync]] · [[haz-single-waiter-rendez]] ·
  [[haz-death-path-wake]].
