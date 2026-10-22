---
id: sub-kernel-perm
type: sub
parent: moc-kernel-security
title: "The identity axis — owner-first rwx and the wstat policy"
code:
  - kernel/perm.c
  - kernel/include/thylacine/perm.h
  - kernel/test/test_perm.c
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/IDENTITY-DESIGN.md section 3.7.1", "docs/IDENTITY-DESIGN.md section 9.6"]
created: 2026-08-02
updated: 2026-10-22
---
## perm_create_mode: the umask, then the directory's bound (2026-10-19, B-3e M8)

`perm_create_mode(mode, is_dir, umask, dir)` returns the rwx bits a create
sends ([[dec-2026-10-19-create-mode-bound-and-umask]], IDENTITY-DESIGN 9.7 M8).
The umask first: `mode & 0777 & ~umask`. Then, when `dir` is non-NULL, Plan 9
open(5)'s bound: a file's rw bits are ANDed with the directory's and its
execute bits pass (`mode & (~0666 | (dir->mode & 0666))`), a directory's rwx
are ANDed with the directory's (`~0777 | (dir->mode & 0777)`). Only the low
nine bits of each input count, and only they come back; the caller keeps the
high bits (DMDIR and the rest) itself. A pure function: no Proc, no lock, no
I/O. Its two callers are `spoor_create_install`
([[sub-kernel-syscall-dispatch]]) and Loom's directory-mutation gate for
`MKDIR` and `MKNOD` ([[sub-kernel-loom]]); each passes the parent's stat only
where the Dev enforces rwx, so a Dev whose modes carry no authority gets the
umask alone. One consequence differs from Linux: an executable asked 0755 in a
0700 directory comes out 0711 (the execute bits pass), where Linux's umask
alone leaves 0755.

`perm.create_mode` pins it row by row, each row one input from a neighbour, so
a dropped umask, a bound applied to the wrong bits, or a missing file/directory
split each fails a row.

## Purpose

Answer *may this principal do this to this object* — the identity half of
authority, orthogonal to capabilities ([[sub-kernel-caps]]) and to handle
rights ([[sub-kernel-handle]]). 132 lines carrying the whole rwx model, the
policy governing who may change a file's owner, group, or mode, and the rule
that sets a new entry's mode.

Thylacine enforces per-file rwx in the **kernel**, at the resolution
chokepoint — the Linux-VFS position. The filesystem server enforces only
dataset scope (see [[sub-stratum-server]]), so this file is the sole place
the question is decided.

## Contract

`perm_check(p, st, want)` returns 0 or -1 for `want ⊆ {R, W, X}` against a
`t_stat`'s uid/gid/mode.

`perm_want_for_omode(omode)` maps an open mode to the access to demand;
`rights_for_omode(omode)` maps the same mode to the handle rights envelope.
**These two are a matched pair and must be read together.**

`perm_wstat_check(p, cur, valid, new_gid)` adjudicates metadata changes for
`{MODE, UID, GID}` and the four time bits, against the file's current
`t_stat` (`cur`). `SIZE` passes through it with no arm.

`perm_create_mode(mode, is_dir, umask, dir)` returns the rwx bits a create
sends: the asked mode less the umask, bounded by the parent's mode when `dir`
is given (the 2026-10-19 section).

## Mechanism

**The DAC-override is a capability and never an identity, and the code is
emphatic about it.** `perm_check` special-cases **no** `principal_id` — not
even `PRINCIPAL_SYSTEM`. Either `CAP_HOSTOWNER` (the unified fs-admin
authority) or `CAP_DAC_OVERRIDE` (the finer clearance-grantable split)
bypasses the rwx check; nothing else does. This is [[inv-i22]] rendered
mechanically: there is no ambient root, so a SYSTEM-identity Proc that has
not been elevated is judged by the same bits as anyone else.

**Owner-first POSIX, with the consequence stated.** An owner is judged on
owner bits *only*, even where group or other would grant more — sound
because an owner can always chmod itself the bit. The branch order is
owner → group → other, exactly one branch taken. A file whose uid is
`PRINCIPAL_NONE` grants its owner bits to every Proc running as none: a file
is a shared object, and Plan 9 keeps none's files shared the same way. The
Proc-to-Proc owner axes in `/proc` are the opposite -- there none owns nothing
but itself ([[dec-2026-10-06-none-owns-nothing]]).

**Two fail-closed defaults that exist to defend against future callers, not
present ones.**

- `want == 0` returns -1. Without it, `(bits & 0) == 0` is *vacuously true*
  and a check for no specific permission would short-circuit to ALLOW. No
  caller passes 0 today; the guard hardens the default polarity of a
  security gate.
- `perm_wstat_check` rejects any `valid` bit outside the set it actually
  adjudicates, rather than trusting the syscall layer to have filtered
  unknown bits upstream. A future `T_WSTAT_*` addition cannot pass ungated
  merely because someone else happened to be checking.

**A uid the filesystem could not vouch for is `PRINCIPAL_INVALID` (0), and
that is a real principal's non-match rather than a wildcard.** When dev9p
cannot establish ownership it fails closed to this value; no real principal
equals it, so the owner branch is simply not taken and the check falls
through to group/other.

**The omode pair is where an execute-to-read leak was closed.** `OEXEC`
mints a `RIGHT_READ` handle, because the kernel loads a binary by reading
it and there is no `RIGHT_EXEC`. So `perm_want_for_omode(OEXEC)` must demand
`PERM_R | PERM_X`, not `PERM_X` alone — otherwise execute-only (`--x`)
permission would mint a read-capable handle. The rule the pair maintains:
**the granted rights must never exceed the access the identity check
validated.** `OTRUNC` adds `PERM_W` on one side and `RIGHT_WRITE` on the
other, in step. The `rights_for_omode` envelope in full:

| omode | handle rights |
|---|---|
| `OREAD` | `RIGHT_READ` |
| `OWRITE` | `RIGHT_WRITE` |
| `ORDWR` | `RIGHT_READ \| RIGHT_WRITE` |
| `OEXEC` | `RIGHT_READ` (read-implied; there is no `RIGHT_EXEC`) |
| `+OTRUNC` | adds `RIGHT_WRITE` |

Two rights this map deliberately does **not** set are *caller policy*, not
`omode`-derived, and the code says so explicitly: `RIGHT_TRANSFER` (a
normally-opened handle gets it at the syscall walk-open site) and the
`T_OPATH` navigation handle's born-`R|W` base with **no** `RIGHT_TRANSFER`
(the A-1.7/F5 confined storage-capability navigation base). `rights_for_omode`
is the pure `omode -> RIGHT_*` half; the transfer bit and the `T_OPATH`
exception live in `sys_walk_open_handler` ([[sub-kernel-syscall-dispatch]]).

**The wstat policy is four different authorities, not one.**

| Op | Who may |
|---|---|
| chmod (`MODE`) | the owner, or `CAP_HOSTOWNER` |
| chown (`UID`) | **only** `CAP_HOSTOWNER` or `CAP_CHOWN` |
| chgrp (`GID`) | chown-any authority, or the owner *to a group they belong to* |
| a time (an explicit one, or one time alone) | the owner, or `CAP_HOSTOWNER` |
| the touch (`ATIME` and `MTIME`, both "now", no `_SET` bit) | the owner, `CAP_HOSTOWNER`, or anyone `perm_check` grants W |

chown is **no-give-away**: an owner may not hand a file to another
principal. That is the Plan 9 fileserver-owner rule and Linux's `CAP_CHOWN`,
and it is why chown is the one op with no owner branch at all.

chmod deliberately has no finer split — there is no `CAP_FOWNER` at v1.0, so
chmod-by-non-owner stays inside the unified `CAP_HOSTOWNER`. The clearance
set (`DAC_OVERRIDE`, `CHOWN`, `KILL`) contains nothing that grants chmod.

The time rows are POSIX utimensat's (G12, 2026-10-22). An explicit time can
rewrite a file's history, which a backup, a build or a font cache trusts, so
it takes the owner or chmod-any authority, as chmod does; write permission
and `CAP_DAC_OVERRIDE` do not suffice (Linux asks `CAP_FOWNER`). The touch,
both times to "now", only does what a write and a read would do, so write
permission also admits it, and `perm_check` brings its own DAC override
(`CAP_HOSTOWNER` or `CAP_DAC_OVERRIDE`) with it, as Linux's
`inode_permission` does. Only that exact request is the touch: one time
alone, even at "now", or an explicit time anywhere in the mask, is the
owner's, as POSIX has it and Linux's `ATTR_TIMES_SET` does. Witness:
`perm.wstat_times`, each refusal one variable from an allowed control.

**`T_WSTAT_SIZE` has deliberately no policy arm here.** A truncate is a
*content* mutation: its authority is the fd's `RIGHT_WRITE` plus the
open-time W check — the POSIX `ftruncate` model — not an identity policy
question. The self-defend mask admits it, and it passes through untainted
while metadata policy still applies to any bits combined with it.

## Data structures

None owned. Reads `Proc.principal_id`, `primary_gid`, `supp_gids[]`,
`supp_gid_count`, `caps`; and the `t_stat` uid/gid/mode filled by the Dev's
`stat_native` (`perm_wstat_check` reads `cur->uid`, and for the touch the whole
stat through `perm_check`). Loom's create-gid rule asks the chgrp question of
a stat whose only set field is the creator as owner.

## Concurrency

No locks. `p->caps` is read with an **acquire** load at both sites, because
`proc_become_legate` is a cross-thread writer since A-4a and a plain load
would be C11-racy (RW-5 F2).

The identity fields need no synchronisation: identity is immutable on a
running Proc. `proc_apply_identity` runs in the *child*, before it enters
EL0, so a plain read is sound by construction rather than by luck.

`proc_in_group` clamps `supp_gid_count` to its maximum before iterating, so
a corruption-induced overlarge count cannot walk off the array — the same
defensive clamp the rfork inherit applies on the way in.

## Invariants enforced

[[inv-i22]] (no identity carries ambient super-authority) — this is its
enforcement site, and the absence of a `PRINCIPAL_SYSTEM` branch is the
enforcement.

Feeds **I-23** (a service's FS authority is bounded by its endowed storage
capability): the endowment bounds *which subtree* is nameable, this file
bounds *what may be done* once there.

Neither is minted yet; this sweep unblocks them.

## Error paths

Every denial is `-1`. There are no extinctions and no partial results — the
function is total over its inputs, with NULL arguments failing closed.

## Performance

A handful of comparisons plus a bounded loop over at most 16 supplementary
gids. Called per path component during resolution, so it is on the hot walk
path — which is why it does no allocation and takes no lock.

## Prosecution

- No `principal_id` may ever be special-cased here. Adding a
  "`PRINCIPAL_SYSTEM` bypasses" branch would reintroduce ambient root and
  break [[inv-i22]] directly.
- `want == 0` must keep failing closed.
- The two omode mappings must stay in step: any new mode must demand at
  least the access its rights envelope confers. The `OEXEC` case is the
  worked example of getting this wrong.
- A new `T_WSTAT_*` bit must be adjudicated here or explicitly justified as
  content-not-metadata, as `SIZE` is; the self-defend mask fails closed
  until then. Bit 6 (the 9P CTIME bit) is the one hole in the set; the
  self-defend witness uses it and bit 9.
- An explicit time must never be granted on write permission or the DAC
  override alone.
- The chown arm must stay owner-less — no-give-away is the property.
- `p->caps` reads must stay atomic.

## Seams

Per-file enforcement on the dev9p tree is gated behind the Dev's
`perm_enforced` flag, which is a one-line policy switch rather than a
mechanism here. A finer `CAP_FOWNER` split for chmod is unbuilt and would
belong in the clearance set.

## Caveats

- `perm_check` trusts the `t_stat` its caller supplies; the fail-closed
  posture for an unvouched uid lives in the Dev, not here.
- Group membership is a linear scan, so a Proc with many supplementary
  groups pays per component. Bounded at 16, so it does not matter.
- The header's summary of the override says `CAP_HOSTOWNER` short-circuits
  to 0; the implementation also honours `CAP_DAC_OVERRIDE`. The code is the
  authority.

## Provenance

[[chg-2026-08-02-authority-sweep]] · [[chg-2026-09-06-9p-identity-absorb]]
(the F1 `rights_for_omode` table + the caller-policy disclaim, folded at the
docs/reference retirement).
