---
id: moc-kernel-namespace
type: moc
title: "Kernel namespace / pathname resolution"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
Pathname resolution — the Plan 9 `namec` heritage: one per-Proc resolver
(`stalk`) turns a `/`-separated path into a Spoor under I-28 containment,
crossing the per-Territory mount table on descent and batching component
runs through the POUNCE fused walk. Every absolute path in the OS resolves
here: `SYS_OPEN`/`SYS_STAT`, exec-from-namespace, `SYS_CHDIR`,
`SYS_MOUNT`/`UNMOUNT`, and the single-hop walk syscalls that share its
crossing primitive. The mount TABLE itself (with the cwd string and the
binds) lives on the Territory — that dossier pends the territory sweep.

## Children

- [[sub-kernel-stalk]] — the resolver: trail lifetime, mount crossing,
  the per-component X-search, the POUNCE post-scan, the cached-open arm.
- [[sub-kernel-path]] — the refcounted copy-on-walk namespace-name
  substrate (`struct Path`, I-33; the Plan 9 `Chan.path`).

## Cross-cutting

- Invariants: [[inv-i28]] (containment + per-component X-search) ·
  [[inv-i33]] (name retention is non-load-bearing).
- Arcs: [[arc-identity-detour]] (stalk-1/2, the single-hop cross) ·
  [[arc-holotype-rw]] (RW-4's resolver/namespace round, #66, the June #81
  CWALKONLY block) · [[arc-go-build]] (the ER-1 errno keystone, the
  POUNCE, #100).
- Adjacent areas: [[moc-kernel-ninep]] (dev9p implements `Dev.walk` /
  `walk_attrs` / `open_cached` — the wire half of every resolution over
  the FS) · [[moc-kernel-srv]] (devsrv's open=connect is the one Dev
  whose open REPLACES the quarry).
