---
id: abi-caps
type: abi
kind: registry
stability: append-only
title: "The capability registry — CAP_* and the fork-grantable / elevation-only partition"
pinned-by:
  - "_Static_assert (CAP_ALL & CAP_ELEVATION_ONLY) == 0 (kernel/include/thylacine/caps.h)"
  - "_Static_assert (CAP_ALL | CAP_ELEVATION_ONLY) == CAP_DEFINED (kernel/include/thylacine/caps.h)"
  - "specs/handles.tla::CapsCeiling, ElevationOnly"
mirrors: []
created: 2026-08-02
updated: 2026-09-23
---
## The surface

A capability is a per-Proc unforgeable bit in a `u64` gating a privileged
kernel operation. Fifteen are defined; the next free bit is `1 << 15`.

Every bit belongs to exactly one of two classes, and the class is the whole
security story:

**Fork-grantable** — a member of `CAP_ALL`, the ceiling kproc starts with.
`rfork_with_caps` confers `parent->caps & mask`, so these flow *down* a
vetted chain and can only ever narrow (I-2).

| bit | name | gates |
|---|---|---|
| 0 | `CAP_HW_CREATE` | `SYS_MMIO/IRQ/DMA/PCI_CREATE` — claiming hardware |
| 1 | `CAP_LOCK_PAGES` | `SYS_MLOCKALL`; held by corvus + per-user stratumd |
| 2 | `CAP_CSPRNG_READ` | `SYS_GETRANDOM`; granted broadly, exists for future revocation |
| 4 | `CAP_GRANT_HOSTOWNER` | writing the `cap` device's hostowner grant file — corvus alone |
| 5 | `CAP_SET_IDENTITY` | `SPAWN_IDENTITY_SET` — the setuid equivalent; kproc → joey → login |
| 6 | `CAP_GRANT_CLEARANCE` | writing the clearance grant file — corvus alone |
| 14 | `CAP_TCB_DIAL` | open=connect on a TCB **byte** service in `/srv` (U); kproc → joey → login → the per-user home proxy |

**Elevation-only** — a member of `CAP_ELEVATION_ONLY`, deliberately
*excluded* from `CAP_ALL`. No Proc holds one at creation, not even kproc,
and `rfork_internal` ANDs every child's mask with `~CAP_ELEVATION_ONLY`, so
an elevated parent cannot leak elevation across a fork. The only path in is
the `cap` device.

| bit | name | confers |
|---|---|---|
| 3 | `CAP_HOSTOWNER` | the unified admin authority; corvus `ADMIN_ELEVATE` from a console-attached session |
| 7 | `CAP_DAC_OVERRIDE` | the `perm.c` rwx bypass, split out of HOSTOWNER as a finer clearance |
| 8 | `CAP_CHOWN` | chown/chgrp to any owner — the no-give-away authority |
| 9 | `CAP_KILL` | the cross-identity kill axis on `/proc/<pid>/ctl` (I-26) |
| 10 | `CAP_DEBUG` | the cross-identity debug axis on the `/proc/<pid>` debug surface (I-39) |
| 11 | `CAP_JIT` | `SYS_JIT_CREATE` — the only path by which emitted bytes become executable (I-42) |
| 12 | `CAP_AUDIO_GRAPH` | the Nocturne whole-sink authority — cross-owner tap/insert/volume |
| 13 | `CAP_POST_SERVICE` | posting into `/srv` as a capability rather than the spawn-time TCB mark |

The split between "who may *register* a grant" (`CAP_GRANT_*`, ordinary and
fork-grantable, held by corvus) and "who has been *elevated*"
(`CAP_HOSTOWNER` and friends, elevation-only, held by a console session) is
the load-bearing shape. Neither half is useful alone.

`CAP_ELEVATION_ONLY` membership is an **invariant obligation** for `CAP_JIT`
specifically: I-42's own text requires the capability be non-heritable, and
exclusion from `CAP_ALL` is exactly what delivers that.

Reserved for later, one bit per domain: `CAP_NS_MOUNT`, `CAP_NS_BIND`,
`CAP_NET_RAW`, `CAP_TIME_SET`, `CAP_REBOOT`.

(Bits 12 and 13 were added without reaching this table and are recorded above
as of 2026-09-23 -- the same drift this note's own closing section describes.)

## Change protocol

Adding a bit means: define it, and add it to **exactly one** of `CAP_ALL` or
`CAP_ELEVATION_ONLY`. A fork-grantable bit MUST go in `CAP_ALL` or kproc
never holds it and it can never be conferred; an elevation-only bit MUST NOT.

**Both halves are enforced as of 2026-09-23 (U).** The disjointness assert
`(CAP_ALL & CAP_ELEVATION_ONLY) == 0` was always real and fires if a bit lands
in both sets. The COVERAGE half now exists too:

```c
#define CAP_DEFINED (CAP_HW_CREATE | ... | CAP_POST_SERVICE | CAP_TCB_DIAL)

_Static_assert((CAP_ALL | CAP_ELEVATION_ONLY) == CAP_DEFINED, "caps.h drift: ...");
```

Together they pin the partition: disjointness forbids a bit in both classes,
coverage forbids a bit in neither.

## The guard that could not fire (CLOSED 2026-09-23)

`caps.h` used to carry a second assert that read as the coverage check and was
not one:

```c
_Static_assert(CAP_ALL == (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ |
                           CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE),
               "caps.h drift: when adding a new FORK-GRANTABLE CAP_* bit, ...");
```

The right-hand side was the macro's own definition, token for token. The
comparison was `X == X` — true unconditionally, so the drift its comment
described was precisely the drift it did not catch. Measured, not inferred: a
standalone reproduction defining a new fork-grantable bit and deliberately
omitting it from `CAP_ALL` compiled clean.

**Task #35, closed by the (U) chunk**, which added exactly the kind of bit the
guard failed to protect — a new fork-grantable one (`CAP_TCB_DIAL`). It is
replaced by the coverage assert above, whose two sides are INDEPENDENT lists:
omit the new bit from `CAP_ALL` and the union loses it while `CAP_DEFINED` keeps
it; omit it from `CAP_DEFINED` and the union gains a bit the mask lacks. Either
way the comparison is between two different expressions.

Verified the way this project verifies a guard — by sabotage, not by assertion:
the replacement compiles clean as written, and re-running the same standalone
reproduction (a sixteenth bit added to `CAP_DEFINED` but omitted from `CAP_ALL`)
now FAILS the build with the intended message.

Consequence had it bitten: a new fork-grantable capability is simply never
grantable. kproc's initial mask omits it, so `parent->caps & mask` clears it at
every hop and no Proc ever holds it. The gate it guards refuses everyone — a
fail-*closed* outcome, which is why it could sit undetected, and which would
read at runtime as "the feature does not work" rather than as a security hole.

## Where the prose has drifted from the code

Two counts inside the header are stale, both from appends that updated the
macro and not the sentence around it:

- "**All five** are acquired ONLY through the `cap` device" — then lists
  six. `CAP_JIT` was appended without bumping the number.
- `CAP_ALL`'s own comment enumerates the excluded set as "`CAP_HOSTOWNER`,
  `CAP_DAC_OVERRIDE`, `CAP_CHOWN`, `CAP_KILL`" — four of the six.
  `CAP_DEBUG` and `CAP_JIT` are missing.

Neither misleads the compiler; the macros are correct and the disjointness
assert holds. They mislead a reader, and they are the same shape as the
tautology above and as the stale counts in [[abi-handle-rights]]: **the
assertions pin the values, and nothing pins the description of the values.**

`caps.h` also carries a standing forward obligation: when a cap-drop or
rfork-mask syscall lands, it must refuse with `EBUSY` if the drop would
clear `CAP_HW_CREATE` while the Proc still holds any hardware handle —
otherwise the implementation admits a state `handles.tla` forbids.

## Prosecution

- A new bit in neither set would be defined, documented and dead — now a
  BUILD failure (the coverage assert), not a silent one. Prosecute that a new
  bit reached `CAP_DEFINED` *and* exactly one class set.
- A new fork-grantable bit not added to `CAP_ALL` is unreachable. This is the
  #35 defect, closed 2026-09-23; prosecute that the coverage assert's two
  sides stay INDEPENDENT lists — rewriting either side in terms of the other
  restores the tautology.
- Any path that lets a Proc *gain* a bit outside the `cap` device breaks
  I-2 — `rfork`'s mask-AND is the only conferral primitive, and it can only
  narrow.
- `CAP_JIT` must stay out of `CAP_ALL`. Scripture in three places calls it
  "the `CAP_HW_CREATE` class", which read literally is wrong —
  `CAP_HW_CREATE` is fork-grantable, and moving `CAP_JIT` to match would
  contradict I-42's non-heritable clause standing beside it.

## Referenced by

[[sub-kernel-caps]] · [[sub-kernel-handle]] · [[sub-kernel-perm]] ·
[[abi-handle-rights]] · [[moc-boundary]].

I-2 (capabilities only ever reduce) is the invariant this registry exists to
serve, and it has no note yet — it is one of six cited constantly and never
minted. See the note-shortfall list in
[[chg-2026-08-02-registry-pass]].
