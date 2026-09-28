---
id: dec-2026-09-24-union-covered-directory
type: dec
title: "Plan 9 unions: an MBEFORE / MAFTER mount keeps the directory it covers"
date: 2026-09-24
status: standing
decided-by: user-vote
affects: [inv-i3, inv-i28, sub-kernel-territory, sub-kernel-stalk, sub-stratum-boot]
created: 2026-09-24
---
## Fork

B-1d's third vote put the native interpreter at `/lib/libc.so`, with `/lib`
"bound from the initrd as `/bin` is" ([[dec-2026-09-24-b1d-loader-shape]];
ARCH 6.5, DISTRO D-4). Building joey's post-pivot bind showed that the plan
could not work in the tree as it stood. The disk's `/lib` holds files the
running system reads: `ndb/local`, `dosbox-x/`, `beacon/verbs`,
`halcyon/renderer` and the `shcompat` shim. A bind at `/lib` would hide all of
them, because Thylacine's union searched only the sources mounted at a point,
never the directory the mount covered. A mount of a directory onto itself is
refused as an I-3 cycle, so the covered directory could not be put back by
hand either. ARCH 9.6.1 says the mount flags "mirror Plan 9", and Plan 9 keeps
the covered directory.

The question went to the operator by blocking question on 2026-09-24 (Opus
5.5, under the away grant's Opus clause): how should `/lib/libc.so` reach the
running system?

## Research

- **Plan 9 keeps the covered directory.** `cmount`
  (`/sys/src/9/port/chan.c`) handles a point with nothing mounted on it: "if
  this is a union mount, add the old node to the mount chain". The old channel
  joins with mount flag 0, so it is searched and read, but never created in
  (it has no `MCREATE`). Per `bind(2)`, `MBEFORE` adds the new directory "so
  its contents appear first in the union", and under `MAFTER` it "goes at the
  end". So `bind -b /386/bin /bin` leaves the old `/bin`'s names visible
  behind the new ones. `MREPL` makes a one-member union and adds nothing.
  `cmount` refuses `MBEFORE` or `MAFTER` when the old file is not a directory
  (`Emount`); `bind(2)`: "Both the old and new files must be directories."
- **The tree as found.** `mount()` recorded only the mounted source at a
  point, so a lone `MBEFORE` made a one-member union (`test_stalk.c` asserted
  "single-member mount is not a union"). The union's walk
  (`stalk_union_member_holding`), readdir and create all iterate the entries
  keyed at the point, so no flag could make the covered directory visible. A
  self-mount (source = point) is refused by `would_create_mount_cycle`
  (`kernel/territory.c`).
- **What reads `/lib` after the pivot.**
  - joey's UM-6 shim mount opens `/lib/shcompat` (`usr/joey/joey.c:7619`).
  - netd's database is `/lib/ndb/local` (`joey.c:8941`; baked by the pool
    populate, `tools/build.sh:3732`).
  - halcyond reads `/lib/beacon/verbs` (`usr/halcyond/src/session.rs:1799`).
  - DOSBox-X's system config lives under `/lib/dosbox-x` (`build.sh:320`).

  Nothing on the device creates directly in `/lib` (only the host-side pool
  populate writes there), so a union with no `MCREATE` member is enough.
- **Linux has no namespace union.** A mount shadows the directory it covers,
  and overlayfs merges trees inside a filesystem, not in the namespace. It is
  not a model here, because in Plan 9 it is the namespace that unions.

## Options

1. **Plan 9 unions (recommended).**
   - What it does: a new `MBEFORE` or `MAFTER` union keeps the covered
     directory as a member, as Plan 9 does. joey binds the initrd's `lib/`
     `MBEFORE` onto `/lib`, so `libc.so` ships with the binaries and the
     disk's `/lib` stays visible.
   - Cost: an audit-bearing change to the union resolver, touching the UM
     row, `territory.tla`, readdir, `MCREATE` and the pivot shed, plus four
     kernel test files. ut's `bind` and `mount` builtins switch to Plan 9
     behaviour. It lands as its own sub-chunk before B-1d.
2. **An initrd-owned loader directory.**
   - What it does: PT_INTERP moves to a directory the disk never holds (for
     example `/lib/ld/libc.so`), bound `MREPL` from the initrd like `/bin`, and
     the loader's search path gains it.
   - Cost: no kernel change, but it re-opens the third vote's path (driver,
     build checks, prover and scripture), and the union divergence stays an
     open bug.
3. **The pool carries `libc.so`.**
   - What it does: Linux's shape. The pool populate writes `/lib/libc.so` to
     the disk.
   - Cost: the smallest change, but the disk copy can drift from the initrd's
     binaries (a `PRESERVE=1` bake, a persisted pool), and the loader must
     match the programs it runs.
4. **Defer to B-6.**
   - What it does: B-1d's exit criterion still holds before the pivot. A
     dynamic program started from the shell fails to exec until B-6 settles
     `/lib`.
   - Cost: it defers a dependency that B-1d's deliverable has.

## The call

**Option 1, Plan 9 unions** (operator vote, 2026-09-24 ~18:35Z). As built at
B-1d-u:

- **When a covered member is added.** An `MBEFORE` or `MAFTER` mount at a
  directory that hosts no member adds a second entry: the covered directory,
  `MCOVERED`. The flag is kernel-internal, and `SYS_MOUNT`'s flag mask refuses
  it from EL0.
- **Order.** Order is Plan 9's: `<new, covered>` for `MBEFORE`,
  `<covered, new>` for `MAFTER`. Later ordered mounts at the point go before
  or after all existing members, the covered one included.
- **What adds none.** `MREPL` and a flagless mount add no covered member, and
  `MREPL` replaces the whole group, covered entry included. Whether the point
  hosts a member is judged before the UM-8 F6 reposition. So re-mounting the
  sole member of an `MREPL` group with `MBEFORE` does not grow one
  (`territory.tla` `BUGGY_FRESH_AFTER_REMOVE`).
- **What the entry holds.** Its source is the mount point's own Spoor,
  retained with one reference. This is the one case where the table keeps the
  point's Spoor rather than copying its identity. It carries no other flag.
  It never has `MCREATE`, so a create in a union whose mounted members lack
  `MCREATE` is `EACCES`. It never has `MNOEXEC`: an `MNOEXEC` union restricts
  the instance that was mounted, not the directory it covers.
- **It is never crossed.** The resolver answers the covered member with a
  clone of the point, and a mount-over-mount chain stops at a point whose
  first member is the covered one. So it is not an edge of the mount graph,
  and I-3 holds over the edges that are crossed (`territory.tla`
  `NoSelfMount`, `CoveredIsItsPoint`).
- **Leaving.** It cannot be unmounted by itself: its source is the point,
  which `unmount` never names. It leaves with the last mounted member
  (`NoOrphanCovered`).
- **Capacity.** A union's first mount needs two table slots.
- **A point that is not a directory stays plain.** Nothing can be searched
  there, so the mount installs no covered member (`territory.tla` `FilePaths`,
  `NoCoveredFile`). Plan 9 refuses such a mount outright (`Emount`). Refusing
  it at `SYS_MOUNT` would narrow the syscall, so that choice is left to the
  operator (OPEN-BUGS).
- **Dissolution.** A dissolved union degrades to member[0]. That is the
  covered directory only when the covered directory was first in order when
  the handle was opened, in which case the handle named it (ARCH 9.6.10).
- **The pivot shed.** The shed treats the covered entry as a self-edge: it
  adds nothing to the closure, and it is kept or shed with the members at its
  point.
- **joey after the pivot.** `/lib` is the initrd's `lib/` `MBEFORE` the disk's
  `/lib`. With no `lib/` in the initrd, joey makes no bind.

## Rationale

The flags were documented as mirroring Plan 9, and they did not. This choice
makes that sentence true, where the alternatives would have grown a special
case for `/lib`:

- Option 2 keeps a known divergence and routes around it with a new path.
- Option 3 splits the loader from the programs it loads across two artifacts
  that are baked separately.
- Option 4 defers a dependency the chunk's own deliverable has: a dynamic
  program run from the shell.

The cost, an audit-bearing change to the union mechanism, is paid once. It is
paid under the model: `territory.tla` gains seven invariants and six buggy
configurations, each of which fails its own invariant. It is also paid in
eighteen kernel tests. ut's `bind -b` and `bind -a` now behave as Plan 9's do.
