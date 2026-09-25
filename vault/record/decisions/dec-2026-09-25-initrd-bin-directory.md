---
id: dec-2026-09-25-initrd-bin-directory
type: dec
title: "The initrd keeps its programs in bin/"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [inv-i12, inv-i28, sub-kernel-content, sub-substrate-build, sub-stratum-boot]
created: 2026-09-25
---
## Fork

The first boot of the boot-ramfs tree ([[dec-2026-09-25-devramfs-directories]])
failed `devramfs.load_complete` (1718/1719): the load refused one entry,
`env`. The initrd root was two things at once:

- the pre-pivot root, which carries six empty synthetic mount points (`srv`,
  `proc`, `ctl`, `dev`, `hw`, `env`);
- the post-pivot `/bin`, which joey binds from the initrd root `MREPL` (ARCH
  9.6.8).

The native coreutils `env` (LS-3c, 8c8a99ca, 2026-06-09) ships at the root.
G15 (002764b0, 2026-06-23) added the `env` mount point beside it. The flat
walk checked the mount points first ("shadow same-named files (none ship by
those names)"), so from 2026-06-23 `/bin/env` resolved to the `/env`
directory, the utility could not run, and readdir listed `env` twice. No gate
runs `env`. The tree's load rule refuses a root entry with a mount point's
name, which turned the hidden collision into a failing boot.

With `env` left out of an experiment image, B-1d passed on the device
(1719/1719, all seven prover legs). The question went to the operator by
blocking question on 2026-09-25 (Opus 5.5, under the away grant's Opus
clause): how should the initrd lay out its programs?

## Research

- **Inferno's root holds mount points only.** The emu configuration's `root`
  table (`emu/Plan9/emu`, read locally) lists `/dev /fd /prog /net /net.alt
  /chan /nvfs /env`. Programs live in `/dis` (`man/1/emu`: `/dis/emuinit.dis`,
  `/dis/sh.dis`).
- **Plan 9 keeps its boot programs in `#/boot`**, with empty directories at the
  root of `#/` to mount on (recalled, not re-read).
- **Linux's initramfs keeps programs in `/bin`** and `/sbin`, and mount points
  (`/proc`, `/sys`, `/dev`) are directories of their own.
- **The blast radius, measured.**
  - About 36 files hold string literals that name an initrd file by its root
    path. The count includes false positives such as disk paths that share a
    program's name.
  - joey makes 135 spawn calls, and before the pivot they use bare names
    resolved through its working directory.
  - The kernel loads `/joey` by archive name (`kernel/joey.c`).

## Options

1. **Programs under the initrd's `bin/` (recommended).** The root keeps only
   the mount points, `bin/` and `lib/`. `/bin/<name>` is the same path before
   and after the pivot, and `/bin` stops listing the mount points and `lib/`.
   It is done inside B-1d. Cost: the path literals, the kernel's joey lookup,
   the build's staging, and ARCH 9.6.8 and 14.5.
2. **Drop `env` now and move to `bin/` after B-1d.** A build check refuses a
   staged name equal to a mount point. `env` stays unreachable until the later
   chunk, which makes this a deferral.
3. **Retire the native `env`.** Plan 9 ships no `env` command; `/env` is the
   interface. This is the cheapest option, and it drops a POSIX utility.
4. **Keep the shadowing.** The file is loaded but the mount point wins, and
   `load_complete` exempts it. `/bin/env` stays a directory, which is how the
   defect hid.

## The call

**Option 1, programs under the initrd's `bin/`** (operator vote, 2026-09-25
~06:50Z). As built at B-1d:

- **The archive.**
  - `build.sh` stages every program and data file into `ramfs-src/bin/`, and
    the dynamic loader's files into `ramfs-src/lib/`.
  - The initrd root holds nothing else, so no program's name can meet a mount
    point's.
  - `mkcpio --require` names `bin/joey`, and, when the sysroot has `libc.so`,
    `bin/pouch-hello-dlopen`, `lib/libc.so` and `lib/libdlprobe.so`.
- **The kernel.** It loads joey from the archive path `bin/joey`.
- **joey's working directory.**
  - Before its first spawn, joey sets its working directory to `/bin`. Bare
    program names, which joey's pre-pivot spawns and their children use,
    resolve there.
  - After the pivot, joey sets it back to `/`, so the services and sessions it
    starts do not inherit `/bin`.
  - The working directory is a name (LS-4), so `/bin` means the initrd's `bin/`
    before the pivot and the bound `/bin` after it.
- **The `/bin` bind.** After the pivot, joey binds the initrd's `bin/` `MREPL`
  at `/bin`, with a pre-pivot handle as before. It was the initrd root.
  - The UM-6 union keeps `shcompat` `MAFTER` it.
  - `/lib` keeps the initrd's `lib/` `MBEFORE` the disk's.
- **Absolute paths.** A path that named an initrd file from the root
  (`/version`, `/system.key`, `/ambush-child`) now names it under `/bin`, which
  is valid on both sides of the pivot.
- **What it supersedes and corrects in [[dec-2026-09-25-devramfs-directories]].**
  - That note said `/bin`, the initrd root, lists `lib/` beside the six mount
    points. `/bin` is now the initrd's `bin/` and lists programs only.
  - Its load rule reads "an entry whose parent directory is not in the
    archive". As built, the parent must PRECEDE the entry: the load places
    an entry only under a directory it has already placed, as Linux's
    initramfs creates in archive order, so a parent that comes later is
    refused too (`devramfs.tree_load_refusals`, `late/c`).
- **The load rule is unchanged.** It still refuses a root entry with a mount
  point's name. With nothing but `bin/` and `lib/` beside the mount points, the
  archive cannot produce one, and `load_complete` would name it if it did.

## Rationale

- Inferno, Plan 9 and Linux all keep the boot medium's programs in a directory
  of their own, apart from the directories that exist to be mounted on.
  Sharing one directory caused a three-month-old defect, and moving the
  programs removes that class of collision rather than one instance of it.
- `/bin/<name>` now works on both sides of the pivot. `/bin` lists programs
  only, instead of the six mount points and `lib/` beside them.
- Option 2 defers the same work and keeps `env` broken meanwhile. Option 3
  drops a utility to protect a layout. Option 4 keeps the defect.
- The cost is a broad but mechanical change of paths, reviewed in B-1d's
  round.
