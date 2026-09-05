---
id: chg-2026-09-05-stalk-union-symlink-pheno
type: chg
title: "sub-kernel-stalk brought current: union resolution, symlink expansion (D-1), the phenotype accumulator (Design D)"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-kernel-stalk
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The third VIVARIUM-D kernel giant this session (after territory + the two
syscall dossiers). sub-kernel-stalk was already deeply detailed and re-swept at
2026-08-16, so its core -- the per-component loop, the POSIX shape gates, mount
crossing, POUNCE, STALK_STAT, the cached-open arm -- is current and was left
intact. Stale were THREE whole features (1251 lines of churn since) the
audit-trigger table names for stalk.c, all verified against the code before
writing:

## Union resolution (the UM arc)

The amode set grew from 4 to 6: `STALK_CREATE`=4 and `STALK_REMOVE`=5. A mount
point with >= 2 members is a union; stalk leaves the POINT as the trail tip (so
`..` lands on it) and iterates members via `stalk_union_child`, returning the
first hit in declared order. Documented: the ATOMIC member snapshot
(`mount_members_snapshot` under one `ns_lock` hold, crossing the EXACT snapshot
source -- UM-8 F4, else a concurrent unmount crosses a member never inspected);
Plan 9 union-skip on every non-fatal outcome (UM-8 F8 -- a member fault must not
hide a later member's entry; only all-miss is ENOENT); create picks the first
MCREATE member, remove returns uncrossed for the caller to select the HOLDER
(UM-7 F3); `stalk_union_has_child` (readdir dedup probe), `union_snap_point_only`
(UM-8c R2-F2 point-only snapshot), the union DIRFD base (UM-8c F5).

## Symlink expansion (DISTRO D-1)

`STALK_NOFOLLOW`=0x100 flag + `STALK_AMODE_MASK`=0xFF + `STALK_MAX_FOLLOWS`=40.
`stalk_expand_link` on a `QTSYMLINK` component: three dispositions, and the
splice-vs-restart split is a SOUNDNESS decision -- absolute target re-anchors at
the caller's OWN Territory root (I-28 containment: a confined Proc's absolute
link stays in its container by construction) + restart; a `..`-bearing relative
target MUST restart (a `..` pop needs a 1:1 trail, only a POUNCE-disabled fresh
resolution guarantees it -- splicing in place is the bug the restart prevents);
a `..`-free relative target splices in place. Hostile-Dev defenses documented
(readlink bound before use, NUL-in-target reject), follows bounded -> T_E_LOOP
(new error path added), intermediate always-follow, trailing-slash override,
mount-membership-wins.

## The phenotype accumulator (Design D)

`crossed_pheno` -- set-only, threaded through the crossing path, recorded BEFORE
the cross can fail (the pheno-mount fact survives a cross failure), winning-union-
member-only. The subtle part: the seed lives AT the `restart:` label and is
`territory_root_pheno`, not false -- a seed hoisted to the first pass only would
let an absolute symlink inside a container drop the declaration and revert its
target to native. `stalk_exec` (the exec-only front end) + `stalk_cross_mounts`
gained the out-param; `stalk_stat` gained a `flags` word (the lstat shape). This
is the stalk half of the same Design D whose dispatch half landed in
[[sub-kernel-syscall-dispatch]] and whose territory half landed in
[[sub-kernel-territory]] this session.

## Sections touched

Purpose (the three widenings named), Contract (amode block + guard + the new
signatures), three new Mechanism sections, Data structures (the expand state +
union snapshot array), Concurrency (`mount_members_snapshot` + the re-anchor ref
both under `ns_lock`), Invariants (I-28 enriched with symlink containment; I-1/I-3
union + I-43 pheno as composed prose, the dossier's established style), Error
paths (T_E_LOOP), Prosecution (7 new obligations), `updated:` -> 2026-09-05.
guarded-by unchanged [inv-i28, inv-i33] -- the composed invariants stay prose, the
I-3/I-22 precedent. The seams (`seam-posix-pathname-form-gates` the aux-2 #79-84
pouch reconciliation, `seam-fid-monotonic-reclaim`, etc.) are a distinct fix
family and were NOT touched -- no unverified merge state asserted.

## Remaining giants

sub-stratum-boot (joey.c ~5659), sub-kernel-vivarium (2671), the proc.c cluster
(proc/jobctl/caps/death ~1097), sub-substrate-build (1713).
