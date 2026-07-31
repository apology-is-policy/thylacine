---
id: moc-kernel
type: moc
title: "Kernel"
parent: home
created: 2026-07-31
updated: 2026-07-31
---
The Thylacine kernel tree: boot, memory, execution, entry, namespace, 9P,
IPC/wake, devices, async (Loom/Weft), console/graphics, security,
introspection. Orientation only — the facts live in the `sub-*` dossiers.

## Children

- [[moc-kernel-ninep]] — the 9P stack (the pilot area; other area MOCs land
  with the per-subsystem sweep).
- [[moc-kernel-srv]] — the `/srv` service layer (registry + per-connection
  transport; the 9P stack's production carrier).

## Cross-cutting

- Registries: `vault/invariants/` · `vault/locks/` · `vault/lineages/` ·
  `vault/hazards/` · `vault/gates/` · `vault/seams/`.
- Until the sweep completes, `docs/reference/NN-*.md` remains the reference
  for unmigrated kernel subsystems.
