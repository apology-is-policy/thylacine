---
id: moc-userspace
type: moc
title: "Userspace"
parent: home
created: 2026-07-31
updated: 2026-07-31
---
The native + ported userspace tree: the boot chain (joey, corvus, login,
warden), the services (netd, stratumd-facing proxies, ptyfs, tapestryd),
the shell/TUI stack (ut, kaua, nora, aurora), the runtime libraries
(libthyla-rs, libdriver, netdev, tls), and the ports plane (pouch + the
Go fork + clade). Orientation only — the facts live in the `sub-*`
dossiers.

## Children

- [[moc-userspace-netd]] — the network daemon (the first userspace area;
  other area MOCs land with the per-subsystem sweep).

## Cross-cutting

- The native/ported split (ARCHITECTURE.md section 3.5): authored-within
  → libthyla-rs, ported → pouch. Every service here is native unless its
  dossier says otherwise; the ported half's translation layer is
  [[moc-pouch-seam]], which lives on the boundary plane because both
  halves must agree on it.
- Kernel boundaries these Procs stand on: [[moc-kernel-srv]] (post/
  connect), [[moc-kernel-ninep]] (the mounts that reach them).
