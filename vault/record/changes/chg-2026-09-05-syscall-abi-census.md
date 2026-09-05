---
id: chg-2026-09-05-syscall-abi-census
type: chg
title: "sub-kernel-syscall-abi census re-derived by measurement: 107 live / span 109 / 3 holes, spawn's fourth growth, t_stat to 88"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-kernel-syscall-abi
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The second of the VIVARIUM-D kernel giants (territory was the first this session).
sub-kernel-syscall-abi carried a census that was correct at its 2026-08-24 sweep
and had a forward-looking paragraph anticipating the Warp appends -- but four
numbers have since moved it, and the dossier's own discipline (audit:hard,
"measure don't guess") demands the refresh be RE-DERIVED, not incremented. Every
count below was measured against the three files this sweep, not carried forward.

## The number space (measured, not incremented)

- **107 live** (was 103): the kernel enum has 107 members, 107 distinct values.
- **107 dispatch arms**, and the two sets are EQUAL both directions (empty in
  each) -- verified by isolating `syscall_dispatch`'s body [14075..14730] and
  diffing its `case SYS_` set against the enum. The dossier's "compare the sets,
  not the counts" claim re-checked, not assumed.
- **span 109** (was 106), **3 holes {26, 30, 43}** (was 4). All three are the
  stalk-3c `/srv` retirements (POST_SERVICE / SRV_CONNECT / POST_SERVICE_BYTE),
  confirmed from their hole comments.
- The four movers: `SYS_BURROW_FROM_HOSTMEM`=107 (V-2), `SYS_HOSTMEM_REFCOUNT`=108
  (V-3b-1c-2b), `SYS_OPEN_CREATE`=109 (#50 family) -- three appends past the top;
  and `SYS_FD_DEVCLASS`=80 (H-1a @7cd1ab94), which FILLED the reserved slot rather
  than extending the span, so four holes became three. None has a C consumer; the
  Rust mirror carries all three appends, the C mirror none -- the subset rule
  visibly holding.

## The mirrors (subset counts + value-agreement)

- **C mirror 77** (was 75), **Rust mirror 100** (was 95). Both re-derived by
  intersecting each mirror's `T_SYS_` enum/consts against the KERNEL enum by name,
  not by trusting the prefix -- the Rust naive prefix count is now **102**, and the
  2 non-syscalls are `T_SYS_SPAWN_ARGV_MAX` + `T_SYS_SPAWN_ARGV_DATA_MAX` (the
  bounds-const trap the dossier flags, re-confirmed).
- **Value-agreement re-verified on both intersections**: no name that overlaps a
  mirror and the kernel disagrees on its number. This is the invariant that
  matters (not "mirrors complete" but "where they overlap they agree").

## Static-asserts + the "mirror" prose census

- Kernel `_Static_assert` **111** (was 109); C mirror **50** (unchanged); Rust
  **43** (unchanged). The C poll.h still has NO real assertion -- its lone
  `_Static_assert` token is inside the quoted "MUST mirror" COMMENT, so the
  dossier's "contains no assertion of its own" stands (a grep -c would miscount it).
- "mirror" appears on **84** lines (was 80) across the two mirrors + poll header;
  "must mirror" case-insensitive **23** (unchanged); "MUST mirror" case-sensitive
  **11** (unchanged).

## The spawn record's fourth growth -- where the two hazards meet

The spawn args grew a FOURTH time: `pheno_flags` (VIVARIUM V-1b) at offset 96,
taking the struct 96 -> **104 bytes**; **20 offset asserts** now (was 16), 21
total. The rich find: `pheno_flags` was AUTHORED at offset 92 -- the same
`_pad_allow` slot CL-5's `page_budget` already claimed, on a different branch --
so it is the concurrent-allocation collision from the number space replayed at a
STRUCT OFFSET. The aux-2 merge moved it to 96 and opened a fresh pad at 100; the
offset assertion records that verbatim. Documented in the growth section as the
struct-field analog of the number-space collision the Mechanism already carries.

## t_stat, the other growth mode

`t_stat` -> **88 bytes** (72 -> 80 uid+gid A-2a -> 88 devno+pad #100), both
appended PAST THE END (no reserved slot -- a stat result is written into the
caller's buffer). Its size assert names FOUR mirrors that must grow in lockstep
(libt, libthyla-rs, pouch patch 0010, go-thylacine `Stat_t`) -- the widest drift
hazard on the surface, added to the growth section as the append-past-end
counterpart to spawn's reserved-slot reuse.

## Records + frontmatter

Record count still **13** (no new argument structs -- the new numbers take scalar
args). `updated:` -> 2026-09-05. Frontmatter guarded-by/abis unchanged (the abi
notes themselves were not edited, only the dossier prose that describes them).

## The three true giants remain

sub-kernel-{proc 1097, stalk 1251, vivarium 2671} -- each a multi-feature
de-stale, one at a time, fresh context each.
