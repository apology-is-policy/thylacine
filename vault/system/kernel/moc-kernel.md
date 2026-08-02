---
id: moc-kernel
type: moc
title: "Kernel"
parent: home
created: 2026-07-31
updated: 2026-08-02
---
The Thylacine kernel tree: boot, memory, execution, entry, namespace, 9P,
IPC/wake, devices, async (Loom/Weft), console/graphics, security,
introspection. Orientation only — the facts live in the `sub-*` dossiers.

## Children

- [[moc-kernel-ninep]] — the 9P stack (the pilot area; other area MOCs land
  with the per-subsystem sweep).
- [[moc-kernel-srv]] — the `/srv` service layer (registry + per-connection
  transport; the 9P stack's production carrier).
- [[moc-kernel-namespace]] — pathname resolution (the stalk resolver, the
  POUNCE fused walk, the Path name-retention substrate).
- [[moc-kernel-execution]] — Procs, Threads, and the death path (the
  lifecycle, the cascade, and the tree's most bug-prone lineage).
- [[moc-kernel-scheduling]] — dispatch, the SMP protocol, and the
  wait/wake primitive (what decides where a Thread runs, and what
  happens when it stops).
- [[moc-kernel-memory]] — the physical allocator stack (phys/buddy/
  magazines + SLUB; where every kernel byte comes from).
- [[moc-kernel-ipc-wake]] — the wake consumers (poll, pipe, torpor;
  [[inv-i9]] instantiated three ways over the scheduling area's
  primitive).
- [[moc-kernel-security]] — the authority substrate (handles and their
  rights, capabilities and the legate, the hardware allowance, the
  identity-axis rwx check; where every privilege gate resolves).
- [[moc-kernel-introspection]] — `/proc` and `/ctl` (process state, the
  debugger's control surface, machine-wide stats; visibility widened
  without authority, gated at the read site rather than the mode bit).
- [[moc-kernel-console-gfx]] — the console and `/dev` (the line discipline,
  the transmit ring, the renderer backend, and the trusted path; an
  interrupt handler that may not do the work, and a consumer that is a
  trust boundary).
- [[moc-kernel-entry]] — the EL0 boundary (the vector table, the return
  tails where the kernel acts on a thread before letting it run again, and
  the fixup table that makes a kernel touch of a user address recoverable).
- [[moc-kernel-async]] — rings and shared pages (the request that stops being
  a syscall, the payload that stops being a copy, and the rule that keeps
  memory the other side can write from ever locating a kernel access).
- [[moc-kernel-boot]] — from the bootloader's first instruction to the banner
  (the image header, the randomized base, the device tree everything is derived
  from, the boot-time rewrite of the atomic baseline, and the ordering that
  composes them; the region where the tools' assumptions are not yet true).

## Cross-cutting

- Registries: `vault/invariants/` · `vault/locks/` · `vault/lineages/` ·
  `vault/hazards/` · `vault/gates/` · `vault/seams/`.
- Until the sweep completes, `docs/reference/NN-*.md` remains the reference
  for unmigrated kernel subsystems.
