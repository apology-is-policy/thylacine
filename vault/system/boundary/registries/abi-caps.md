---
id: abi-caps
type: abi
kind: registry
stability: append-only
title: "The capability registry — CAP_* and the fork-grantable / elevation-only partition"
pinned-by:
  - "_Static_assert (CAP_ALL & CAP_ELEVATION_ONLY) == 0 (kernel/include/thylacine/caps.h)"
  - "specs/handles.tla::CapsCeiling, ElevationOnly"
mirrors: []
created: 2026-08-02
updated: 2026-08-02
---
## The surface

A capability is a per-Proc unforgeable bit in a `u64` gating a privileged
kernel operation. Twelve are defined; the next free bit is `1 << 12`.

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

The split between "who may *register* a grant" (`CAP_GRANT_*`, ordinary and
fork-grantable, held by corvus) and "who has been *elevated*"
(`CAP_HOSTOWNER` and friends, elevation-only, held by a console session) is
the load-bearing shape. Neither half is useful alone.

`CAP_ELEVATION_ONLY` membership is an **invariant obligation** for `CAP_JIT`
specifically: I-42's own text requires the capability be non-heritable, and
exclusion from `CAP_ALL` is exactly what delivers that.

Reserved for later, one bit per domain: `CAP_NS_MOUNT`, `CAP_NS_BIND`,
`CAP_NET_RAW`, `CAP_TIME_SET`, `CAP_REBOOT`.

## Change protocol

Adding a bit means: define it, and add it to **exactly one** of `CAP_ALL` or
`CAP_ELEVATION_ONLY`. A fork-grantable bit MUST go in `CAP_ALL` or kproc
never holds it and it can never be conferred; an elevation-only bit MUST NOT.

**One half of that is enforced and the other half is not.** The disjointness
assert `(CAP_ALL & CAP_ELEVATION_ONLY) == 0` is real and fires if a bit
lands in both sets. There is **no coverage assert** — nothing checks that a
newly defined bit landed in *either*. Today the two sets happen to partition
all twelve bits completely (six and six), but that is a property nothing
holds in place.

## The guard that cannot fire

`caps.h` carries a second assert that reads as the missing coverage check
and is not one:

```c
#define CAP_ALL (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ | \
                 CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE)

_Static_assert(CAP_ALL == (CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ |
                           CAP_GRANT_HOSTOWNER | CAP_SET_IDENTITY | CAP_GRANT_CLEARANCE),
               "caps.h drift: when adding a new FORK-GRANTABLE CAP_* bit, "
               "update CAP_ALL so kproc's initial mask reflects it.");
```

The right-hand side is the macro's own definition, token for token. The
comparison is `X == X`. It is true unconditionally and **cannot fail** — so
the drift its comment describes is precisely the drift it does not catch.

Measured, not inferred: a standalone reproduction defining a thirteenth
fork-grantable bit and deliberately omitting it from `CAP_ALL` — the exact
mistake — compiles clean. Tracked as task #35; the fix is a real coverage
assert of the shape `handle.h` already uses for `kobj_kind`, comparing the
union of the two sets against the defined-bit mask.

Consequence if it bites: a new fork-grantable capability is simply never
grantable. kproc's initial mask omits it, so `parent->caps & mask` clears it
at every hop and no Proc ever holds it. The gate it guards refuses
everyone — a fail-*closed* outcome, which is why it could sit undetected,
and which would read at runtime as "the feature does not work" rather than
as a security hole.

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

- A new bit in neither set is defined, documented, and dead. Nothing warns.
- A new fork-grantable bit not added to `CAP_ALL` is unreachable, and the
  assert that claims to catch this does not.
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
