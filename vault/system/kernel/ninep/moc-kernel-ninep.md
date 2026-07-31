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

- [[sub-kernel-ninep-client]] — the shared elected-reader client
  (`9p_client.c` + session + transports). **The pilot dossier.**
- (sweep-pending: wire codec · session · transports · dev9p · dev9p.poll ·
  Larder · attach/srvconn — `docs/reference/44..48, 121, 132` until then.)

## Cross-cutting

- Invariants: [[inv-i9]] · [[inv-i10]] · [[inv-i11]].
- Specs: [[spec-9p-client]] · [[spec-reader-frame]].
- Lineage: [[lin-9p-client]] — the #841→#90 hardening saga.
- Hazards: [[haz-shared-stream-desync]] · [[haz-single-waiter-rendez]].
