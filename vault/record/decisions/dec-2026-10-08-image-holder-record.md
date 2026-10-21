---
id: dec-2026-10-08-image-holder-record
type: dec
title: "An address space records the caps and seals of every Proc that has held it, and the I-39 join weighs the record"
date: 2026-10-08
status: standing
decided-by: user-vote
affects: [sub-kernel-proc, sub-kernel-addrspace, sub-kernel-devproc, sub-kernel-caps]
created: 2026-10-08
---
## Fork

I-39 asks every guard on an image (the cover, both seals, the taint) of the
join over the Procs that map it. A Proc that has stopped mapping it is not in
that join, but what it held can still be in the image. `rfork(RFPROC|RFMEM)`
gives a child the parent's address space with the elevation-only caps carved
off (I-2), and the child keeps that space after the parent is reaped. Three
things leaked that way:

- a code region, which is `CAP_JIT`'s authority. B-2b audit r2 closed this one
  alone by counting code aliases (`AddrSpace.code_vmas`);
- an MMIO, DMA or PCI BAR window, which is the I-34 allowance's (B-2b audit r3
  F3). L-3 keeps a shared space's device mapped for the surviving sharer
  (`proc_quiesce_owned_devices` skips the reset when the space is not sole), so
  a capless same-principal peer covers the child and writes the device through
  its `mem`;
- a seal. Seals do not cross fork (DEBUG-FS-DESIGN 3.3), and the join stopped
  reading the parent's bit once the parent was gone, while the secret the seal
  kept is still in the shared bytes.

## Research

- Linux keeps dumpability per address space, not per task: `MMF_DUMPABLE` lives
  in `mm->flags`; `commit_creds` lowers it when a task's credentials gain
  privilege; `__ptrace_may_access` refuses a non-dumpable mm without
  `CAP_SYS_PTRACE`; it stays set for that mm until exec's new mm resets it.
- Plan 9 has no capability cover and nothing comparable to weigh.
- Tree facts: a live Proc's caps only grow (every write is a `fetch_or`); a
  holder leaves a space at exactly three points, its ZOMBIE transition, its exec
  swap, and `proc_free`, and the first two run under `g_proc_table_lock`, the
  lock every join runs under. A COW fork gives the child a new space, so the
  record does not cross it.

## Options

1. **A sticky per-space record (recommended).** The space keeps the OR of every
   departed holder's caps and seal and taint bits, and the join ORs it in.
   Exec's fresh space starts empty. It covers every class at once, including
   residue a dead creator left in the bytes, and replaces the code-alias count.
   Cost: an orphan stays uncoverable after its aliases or windows are gone, and
   a space a more capable Proc once held stays out of a lesser peer's reach until
   exec.
2. **Per-type counts.** Extend B-2b's count: MMIO and DMA windows count as
   `CAP_HW_CREATE`. A GPU host-memory mapping belongs to a client that holds no
   such cap, so it needs a rule of its own; the seal half and residue stay open.
3. **Refuse RFMEM over a device mapping.** Fail closed at fork, as fork refuses
   a code region. A driver then cannot spawn through libthyla-rs's
   `rfork_spawn`; host-memory clients and the seal half stay open.

## The call

Option 1, by the operator's vote on 2026-10-08. The vote named the record of
caps; the seal and taint bits ride the same record, because the vote's
rewording of I-39 ("every Proc that has held it") covers every guard I-39
names, and the seal's leak is the same departure.

## Rationale

The image outlives the Procs that held authority over it, so the authority has
to be recorded where the image lives. A count per class answers one class at a
time and misses the next one; recording at departure answers all of them,
because a Proc's caps and guard bits at its departure are everything it ever
held. Writing the record at the departure points rather than at every grant
keeps it to three sites under the join's own lock, and it also closes the reap
window DEBUG-FS-DESIGN 3.3 named: the ZOMBIE transition records before any
unlink.
