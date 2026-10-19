---
id: chg-2026-10-19-mount-table-dynamic
type: chg
title: "A dynamic mount table, mount errnos, a create's group from its directory, and a windowed /proc/<pid>/ns (B-3 votes 5-7)"
date: 2026-10-19
arc: arc-boosty
commits: ["d9e3d796c", "21072f317", "9f6215ba6", "d1f93655e", "00ae50bb2", "0ef2b7a39", "95b114442", "018aa73cf", "693f29020", "af75de851", "1c9158633", "b114ab6bb"]
touched:
  - sub-kernel-territory
  - sub-kernel-stalk
  - lock-territory-ns-lock
  - spec-territory
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - abi-errno
  - sub-kernel-dev
  - sub-kernel-ninep-dev9p
  - sub-kernel-loom
  - sub-kernel-devproc
  - sub-kernel-death
  - sub-substrate-build
  - sub-libthyla-rs
  - sub-viv
  - sub-haul
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/errno.h, docs/ERRORS.md, kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/err.rs, usr/lib/libthyla-rs/src/territory.rs, usr/lib/libthyla-rs/src/loom.rs]
depth: rich
created: 2026-10-19
---
Votes 5, 6 and 7 of B-3e (operator votes 2026-10-19), with the work they
pulled forward. The mount table takes Plan 9's Mhead/Mount shape under a
DoS cap ([[dec-2026-10-19-mount-table-dynamic]]); `SYS_MOUNT` and
`SYS_UNMOUNT` answer an errno per refusal ([[dec-2026-10-19-mount-errnos]]);
a created file's group is its directory's
([[dec-2026-10-19-create-group-from-parent]]). Once a namespace could hold
1024 entries, `/proc/<pid>/ns` could no longer be cut at devproc's 2 KiB
buffer, so it now serves windows of the whole listing (5d). The same
scripture commit recorded votes 8 (Loom's create refusals are EACCES,
[[dec-2026-10-19-loom-create-refusal-eacces]]) and 9 (corona's
`SYS_SRV_REGISTRY_NEW` is 134, [[dec-2026-10-19-syscall-numbers-across-tracks]]).
The scope also carries two inputs: the P3 fixes of the holotype round on M7
and M8 (the create-mode bound and umask), whose two open items became votes
7 and 8, and the wiring of the eight host boundary checkers, which nothing
had run.

**What.** [[sub-kernel-territory]]: a `PgrpMount` per mount under a
`PgrpMhead` per mount point. Points hash on their (dc, devno, qid.path) key
into 64 chains; each point keeps its members in search order; a table-order
list reproduces the old array's order, so `/proc/<pid>/ns` prints what it
printed. `PGRP_MOUNT_CAP` = 1024 replaces the 32-entry array. It is a DoS
bound, not a usage bound: 128 KiB of slab at the cap, 1/32 of
`PROC_VMA_MAX`'s 4 MiB. `mount()` allocates its worst case before `ns_lock`
(-4 is ENOMEM, before any edit) and releases what an edit removes after it.
`territory_clone` counts under the parent's lock, allocates outside it, and
tops up if a peer grew the table. The cycle check and the shed walk the
points with an intrusive visit mark and worklist. The nine arrays sized from
the cap are gone. [[sub-kernel-stalk]]: a union snapshot holds 16 members
inline and retakes a larger union into a heap array, all or nothing.
[[lock-territory-ns-lock]]: never held across an allocation or a free, and
never taken under `g_proc_table_lock`.

[[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]], [[abi-errno]]:
`SYS_MOUNT` answers EINVAL for its flags, EBADF for the source, the path
checks `SYS_OPEN_CREATE` makes, the resolver's errno, ENOTDIR, ELOOP for a
cycle, ENOSPC at the cap and ENOMEM. `SYS_UNMOUNT` answers EINVAL when
nothing is mounted. The source index is bounded at full width before its
cast to `hidx_t`. `T_E_NOSPC` (28) gains its first emitter.
[[sub-libthyla-rs]] gains `Error::NoSpace`, and `territory::unmount` maps
each value. [[sub-viv]] and [[sub-haul]] name each errno.

[[sub-kernel-dev]], [[sub-kernel-ninep-dev9p]], [[sub-kernel-loom]]: every
sync create sends the parent's stat gid instead of the creator's primary
group; a parent that reports no group passes `GID_INVALID` on; a caped
session still sends `P9_NOGID`. Loom's MKDIR, MKNOD and SYMLINK resolve a
gid of 0 to the parent's group, and an explicit gid keeps the chgrp rule. A
create on a Dev with no `stat_native` (devcap's `/cap`, whose create always
refused) answers -EIO instead of -1. From the M7/M8 round: an MKNOD whose
type is a directory takes the directory's mode bound.

[[sub-kernel-devproc]], [[sub-kernel-death]]: `territory_format_ns(t, off,
buf, cap)` copies a window of the listing that ends at a line end. The ns
read takes a Territory ref under `g_proc_table_lock`, renders after the
walk with IRQs on (asserted), and then walks again to re-ask the dump seal
and the none wall of the Proc still holding the rendered Territory.
Exiting reads as the empty file; gone reads as -1. A reader whose ref
outlives the target's exit performs the Territory's final release (SA-5,
below).

[[sub-substrate-build]]: the eight `tools/test-<name>.py` host checkers run
on every build: the native three before any target, the five Pouch ones at
`build_sysroot` step 2b. Both sets are derived from `tools/host-tests/`.
`tools/test-host-boundaries.sh` runs every checker under every mutant it
declares (8 checkers, 50 legs). Each driver refuses to run under `-O` or
`PYTHONOPTIMIZE`.

**Witnesses.**
- Vote 5: `territory_mount.nodes_follow_every_edit`,
  `territory_mount.refuses_enomem`, `sys_mount.enomem_answers_enomem`,
  `stalk.union_past_the_inline_snapshot`, and four tests ported to the real
  cap (`table_full`, `covered_needs_two_slots`,
  `shed_full_table_boundary`, `full_table_answers_enospc`).
- Vote 6: `sys_mount.cycle_answers_eloop`; every refusal assert in
  `test_sys_mount` names its errno; alloc-smoke's unmount, missing-point and
  wide-index legs.
- Vote 7: `dev9p.create_group_from_parent`, `9p_client.loom_create_gid`,
  `loom_mkdir_e2e`, symlink-probe leg O.
- M7/M8 fixes: the MKNOD-directory leg of `9p_client.loom_create_mode`, and
  `dev9p.create_mode_on_the_wire` w7 (the no-bound arm).
- 5d and its close: the window legs of `territory_mount.format_ns`,
  `devproc.ns_reads_past_the_buffer`, and
  `devproc.ns_admission_after_render` (a window hook between the two walks:
  control, seal, exit through the real release, unlink; every leg checks
  the reader dropped its ref, and the exit leg that its drop freed the
  Territory).
- The checker gate: 50 legs, each checker clean and red under each mutant.

**Audit.** Opus reviewing Opus, MODEL start == end in both rounds. Round 1: 0 P0, 0 P1, 1 P2, 4 P3. The P2 was global: the ns read
rendered under `g_proc_table_lock` with IRQs masked, waiting on an
`ns_lock` that any Proc can hold for about a million steps per shed (1023
points in one instance, looped `pivot_root`). Fixed by rendering after the
walk, with an IRQ assert as its red check. The fix broke the dump seal's
ordering, which was a lock (SA-4, self-found at the close): a Proc could
seal itself between a reader's admission and its render. The second walk
fixed it. The shed's quadratic cost and the unkeyed mount hash are enqueued
P3s. Round 2 (the close changed a lock nesting): 0 P0, 0 P1, 0 P2, 4 P3,
three fixed. Its F1 is SA-5: a reader holding the last Territory ref runs
the release, and a walked 9P source's clunk there may wait, killably, on a
server the dead Proc controlled. Tracked in OPEN-BUGS with its fix
(strong and weak Territory refs), which lands with vote 2's chunk and its
round. Closed list: auto-memory `audit_b3e_votes567_closed_list.md`.

**Land gates.** Lease 3, CI image of 693f29020 in the b3e worktree: suite
2026/2026 with no FAIL in the boot log; test-fault 8/8. Twelve sabotages in
four red kernels (the M7/M8 round's F3 and F5, vote 6, vote 7's sync and
Loom creates, seven vote 5 and 5d mechanisms) turned every designated
witness red with its predicted message. One of them, a leaked unmount ref,
then hung the suite in `userspace.stratumd_stub_round_trip`, because the 9P
session never closes. The restored image's suite was green. All 23 buggy
configs of `territory.tla` and `territory_shed.tla` violate the invariant
each names. ls-libs bound all eight user layers (19 to 35 mount entries):
libxml2 21 of 23 (runtest and testparser) and freetype 4 of 5, all
enqueued. The first Pouch red moved a number that an earlier checker pins,
so the step-2b call site never ran. The second wrote the kill-probe
checker's own mutant into patch 0057, and step 2b refused it by name.
af75de851 on the same lease: suite 2026/2026, and its red (the render put
back in the callback) extinct at `devproc.read_ns_format` with the IRQ
assert's message.

Owed before B-3 lands on main: 1c9158633 and b114ab6bb are compiled and
link-checked, not built. Their suite (2027) and SA-4's red (remove the
second walk: the seal leg must read 9 bytes) run at vote 2's lease; the
clean `territory` and `territory_shed` configs were not re-run; runtest's
fix (libxml2's directory listed writable) waits for the next ls-libs run.
