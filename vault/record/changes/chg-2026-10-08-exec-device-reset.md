---
id: chg-2026-10-08-exec-device-reset
type: chg
title: "Exec resets the image's devices; the last reference resets what is mapped"
date: 2026-10-08
arc: arc-boosty
commits: ["4a8689a09", "0b95a9daa", "3ee7d75e5", "8fbaa39d0"]
touched:
  - sub-kernel-death
  - sub-kernel-proc
  - sub-kernel-addrspace
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-08
---
Taken by main after weftpark, from vmaguard's audit r2 (OPEN-BUGS 10-08 (a)).
Exec drained the old address space with no device reset: a driver's DMA buffer
whose descriptor it closed after mapping is held only by the mapping, so the
drain freed it while the device that had been handed its address could still be
armed -- RW-7 R3-F1 on exec instead of death. The operator voted (2026-10-08)
that exec resets the image's devices.

**What** ([[sub-kernel-death]], [[sub-kernel-proc]], [[sub-kernel-addrspace]]).
`proc_quiesce_owned_devices` splits into its two walks: `quiesce_fd_devices`
(the handle table) and `addrspace_quiesce_mapped_devices` (the devices a named
space maps, after a take-and-drop of its lock for the weft reaper).
`addrspace_unref` becomes `addrspace_release` (drop one reference; true when it
was the last) and `addrspace_destroy` (the mapped-device walk, the drain, the
free). `proc_exec_replace` calls the two halves itself and runs the handle-table
walk exactly when its own drop is the last, before the drain; sys exec's
close-on-exec sweep runs after the replace, so the walk still sees descriptors
about to close. A descriptor that survives exec reaches the new image reset: a
virtio-mmio device re-initializes from status 0; a PCI function is revoked, and
its claim frees for a new one once the new image closes that descriptor and
every IRQ descriptor of the function and no hostmem client maps a BAR.

**Why at the last reference.** Each dying holder reads the space's count at its
exit close and its reap and drops its reference only at the reap, so two holders
leaving at once could each read the other's reference and both skip the
mapped-device walk -- two concurrent reaps already could, and exec added a third
path. The last reference is the one point no interleaving skips, the same
argument that put the drain there (L-3). The exit close keeps its sole-gated walk
for a buffer held only by a handle, which frees at that close.

**Residual, by the vote.** An exec whose drop is not the last resets nothing (an
unreaped zombie sibling counts as a holder -- a deliberate over-approximation):
a buffer held only by a close-on-exec descriptor frees at the exec's own sweep,
and one left mapped frees at the last holder's drain, with a descriptor-claimed
device still armed. Death has no such gap. It is the R3-F1 trust-envelope
family; the R3-F8 device-session model closes it.

**Verification.** Audits by Fable 5.1 (start == end each round): r1 0/0/0/6 (F1
the release/destroy split, F2 the order witness); r2 0/0/0/3 on the restructure
(F-A the residual's true trigger). Witnesses `virtio.exec_quiesces_devices`
(three execs by a child on itself: shared -> nothing; sole with a descriptor
claim -> exec's walk, before the drain by the teardown stamp; sole with a
mapping-only claim -> the drain's walk) and
`virtio.last_unref_quiesces_mapped_device` (a holder that saw a second reference
skips; a bare last drop resets). Gates on 8fbaa39d0 (rebased on weftpark 41ad15031): RED 6/6 as predicted (S1/S2/S6 at their predicted lines; S3-S5 after the tests were made to release their fixtures before asserting, with no driver collateral; base and green 1965/1965 pre-rebase); suite 1966/1966; test-fault 8/8 PASS; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8, no corruption).
