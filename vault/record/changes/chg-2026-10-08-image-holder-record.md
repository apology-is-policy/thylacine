---
id: chg-2026-10-08-image-holder-record
type: chg
title: "An address space records every holder that has left it, and the I-39 image join weighs the record; the code-alias count retires"
date: 2026-10-08
arc: arc-boosty
commits: ["95ec416cf", "9ce22d6bf", "04d32797b"]
touched:
  - sub-kernel-proc
  - sub-kernel-death
  - sub-kernel-jobctl
  - sub-kernel-caps
  - sub-kernel-vma
  - sub-kernel-addrspace
  - sub-kernel-devproc
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-08
---
A soundness chunk between B-2b and B-2c, by the operator's vote of 2026-10-08
([[dec-2026-10-08-image-holder-record]]). It closes the finding B-2b audit r3 F3
left enqueued at the B-2 land ([[chg-2026-10-07-b2-jit]], Residuals): an `RFMEM`
child keeps its driver's MMIO, DMA or host-memory window after the driver is
reaped, and a capless same-principal peer then covered the child and wrote the
device through its `mem`.

**What** ([[sub-kernel-addrspace]], [[sub-kernel-proc]], [[sub-kernel-death]]).
`struct AddrSpace` carries `caps_ever` and `guards_ever` (80 bytes, asserted):
the OR of the caps, and of the NODUMP, NOTRACE and DEBUG_TAINTED bits
(`PROC_IMAGE_GUARDS`), of every Proc that has left the space.
`addrspace_record_holder` writes them at each departure: the ZOMBIE transition
and the exec swap, both under `g_proc_table_lock`, and `proc_free`, which in
production only repeats an OR (every `proc_free` outside the reap frees a
rollback no one ever saw). `proc_image_join_locked` ORs both into the join
before its sole-mapper fast path, so every consumer weighs the departed: the
debug cover ([[sub-kernel-devproc]]), both seals, and the elevation taint
([[sub-kernel-caps]]). Nothing clears the record; exec's fresh space and a COW
child's space start without one. B-2b's code-alias count (`AddrSpace.code_vmas`,
[[sub-kernel-vma]]) is gone: a code region's creator held `CAP_JIT`, so the
record carries it.

**Why at departure, not at every grant.** A live Proc's caps only grow (every
write is a `fetch_or`, and a legate scope ends by tearing its members down, not
by clearing bits), so the caps a holder carries as it leaves are all it ever
held. Three sites under the join's own lock replace five grant sites, each of
which would have needed an ordering argument, and the ZOMBIE-site write runs
before any reap can unlink the Proc, which closes the reap-window miss
DEBUG-FS-DESIGN 3.3 had listed as open.

**What it closes.** B-2b r3 F3; the seal half, found while writing the scripture
(seals do not cross fork, so a sealed parent's `RFMEM` child read unsealed once
the parent was reaped); and the reap window.

**Costs, as voted.** An orphan stays uncoverable after its aliases or windows
are gone. A space that a more capable Proc once held stays out of a lesser
peer's reach until exec. An orphan of a NOTRACE creator is refused the
NOTRACE-seamed debug files even of itself, the shape the join already had for a
live sharer.

**Verification.** Audit r1 (Fable 5.1, start == end): 0/0/1/3, clean, no round
2 owed. Five witnesses: `devproc.debug_cover_weighs_departed`,
`seal_outlives_its_holder`, `taint_outlives_its_holder`,
`zombie_records_departure`, `exec_records_departure`.
@@FILL-AT-LAND@@

## Corrections (the record plane is append-only, so they live here)

**[[dec-2026-10-08-image-holder-record]]'s Research overstated Linux.** It said
`commit_creds` lowers dumpability "when a task's credentials gain privilege" and
that the bit "stays set for that mm until exec's new mm resets it". Read in
Linux's tree on 2026-10-08: `commit_creds` lowers it to `suid_dumpable` whenever
the effective or filesystem uid or gid changes, a drop included, or the new
permitted caps are not a subset of the old (`!cred_cap_issubset(old, new)`);
`prctl(PR_SET_DUMPABLE, 1)` raises it again; and the current tree keeps it in a
`task_exec_state` that `CLONE_VM` siblings refcount-share (`copy_exec_state`), a
fork copies and exec replaces, no longer in `mm->flags`. Linux's bit is a
current setting; the record is a history, which makes it stricter. The scripture
commit (95ec416cf) had called the two equivalent; audit r1 F3 caught it, and
the r1 fix (04d32797b) said `commit_creds` lowers the bit "on any credential
change", itself too broad. DEBUG-FS-DESIGN 3.3 and the ARCH I-39 cell were
corrected in place at the land.

**[[chg-2026-10-07-b2-jit]]** describes the join counting a code alias as
`CAP_JIT` through `AddrSpace.code_vmas`, and leaves the I-34 orphan as a
Residual, saying "the count trick does not transfer, because an allowance's
authority is per allowance". Both were true when written. The record retires the
count and closes the residual: it weighs the caps the driver held, not its
windows, so the per-allowance shape no longer matters.
