---
id: dec-2026-09-25-devramfs-directories
type: dec
title: "The boot ramfs serves directories"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [inv-i12, inv-i28, inv-i36, sub-kernel-content, sub-substrate-build, sub-stratum-boot]
created: 2026-09-25
---
## Fork

B-1d's device witness never ran. The native interpreter lives at
`/lib/libc.so` ([[dec-2026-09-24-b1d-loader-shape]], the third vote), and
after the pivot joey binds the initrd's `lib/` `MBEFORE` the disk's `/lib`
([[dec-2026-09-24-union-covered-directory]]). Both votes assumed the initrd
could hold a `lib/` directory. It could not:

- `tools/mkcpio.py` packs only the top-level files of its source directory
  ("Subdirectories are NOT recursed at v1.0 — flat layout only").
- `kernel/devramfs.c` serves one flat directory plus six empty synthetic
  mount points ("Flat file layout (no directories inside the archive)").

So the build staged `lib/libc.so` and `lib/libdlprobe.so` in the ramfs source
tree, and neither reached `ramfs.cpio` (232 entries, none with a `/`). On the
device joey printed `pouch-hello-dlopen SKIPPED (no /lib/libc.so in this
image)` and `B-1d /lib union SKIPPED (no lib/ in the initrd)`. `test.sh`
still went green at 1712/1712. A witness that skips when its subject is
missing is a negative that a broken fixture satisfies.

The question went to the operator by blocking question on 2026-09-25 (Opus
5.5, under the away grant's Opus clause): how should the initrd carry `lib/`?

## Research

- **Inferno's root device serves a static tree.** In `emu/port/devroot.c`
  (read locally), each entry's `Rootdata` records `dotdot`, its parent's
  `qid.path`. Walk, stat, open and read index the parent's child table
  (`rootdata[p].ptr`, `rootdata[p].size`). The table is generated when the
  emulator is built.
- **Plan 9's root device** (`port/devroot.c`) keeps a small fixed tree: `#/`,
  with a `boot/` directory that the kernel configuration fills. This is
  recalled, not re-read.
- **Linux unpacks the whole archive.** `init/initramfs.c` creates the
  directories, files and links that a newc archive names, in archive order,
  inside the root filesystem.
- **The target always had directories.** ARCH 14.5 described the early-boot
  ramfs with a "name → inode map per directory". The flat table was P4-E's
  scope, and the `/lib` votes outgrew it.
- **The tree as found.** `walk_one` answers `..` with the root from anywhere
  and looks names up only at the root. `readdir` lists every file, plus the
  six mount points, at the root, and nothing anywhere else. The table holds
  256 entries, and the image uses 232 of them.

## Options

1. **devramfs serves directories (recommended).**
   - What it does: `mkcpio.py` recurses and emits directory entries. devramfs
     builds a static tree when it loads: each entry keeps its parent, and
     walk, `..`, readdir and stat work per directory. The six synthetic mount
     points stay as they are.
   - Cost: roughly 150 kernel lines plus kernel tests. It is audit-bearing:
     devramfs backs exec (I-12, I-36), and the X-search now crosses real
     directories in it (I-28).
2. **One synthetic `lib/` level.**
   - What it does: only the archive names under `lib/` form a synthetic
     directory, one level deep.
   - Cost: barely smaller than option 1, because walk, `..` and readdir
     change the same way. It is a special case for one name.
3. **No kernel change.**
   - What it does: `libc.so` stays flat in the initrd root, and joey binds the
     whole initrd root `MAFTER` the disk's `/lib`.
   - Cost: the disk wins every name clash (`aurora/` and `dosbox-x/` are also
     initrd binaries), and `/lib` lists every initrd binary. A `libc.so` on
     the disk would shadow the loader. That is the drift between the loader
     and its programs that the union vote was chosen to avoid.

## The call

**Option 1, devramfs serves directories** (operator vote, 2026-09-25
~05:00Z). As built at B-1d:

- **The archive.** `mkcpio.py` walks its source tree in sorted order, so the
  archive's bytes are deterministic. Each directory is emitted before its
  contents, as a newc entry with `S_IFDIR`, its permission bits and size 0.
  Names are relative (`lib/libc.so`). Regular files keep their permission
  bits as before. Symlinks and special files are still skipped.
- **The table.** devramfs keeps one entry for each archive file or directory.
  Each entry records its parent (the root or a directory entry) and its last
  component. The load skips and counts any entry it cannot place:
  - a name that is absolute, or has an empty, `.` or `..` component;
  - an entry whose parent directory is not in the archive (every directory
    must have an entry of its own);
  - a second entry with the same path;
  - a root entry that has the name of a synthetic mount point;
  - any type other than a regular file or a directory.
- **Qids.** The encoding is unchanged. The root is 0. An entry is its index
  plus one, and a directory's qid type is `QTDIR`. The synthetic mount points
  sit above `RAMFS_QID_SYNTH_BASE`.
- **Walk.** A step from a directory finds the child with that name. `..` goes
  to the parent. The root's `..` is the root, and so is a synthetic mount
  point's. A step from a file fails, `..` included, as a 9P walk from a
  non-directory does.
- **Readdir.** A directory lists its children. The root also lists the six
  mount points, which stay empty. The resume cookie is the entry's ordinal in
  one global order, so a resumed read neither repeats nor skips an entry.
- **Stat and read.** A directory reports `S_IFDIR`, the archive's permission
  bits and size 0, and is owned by SYSTEM like every entry. Reading a
  directory's bytes fails, because readdir is how a directory is listed.
- **Kernel lookups.** `devramfs_lookup`, which loads `/joey` before any
  namespace exists, matches a full archive path and finds only regular files.
- **The witness fails closed.**
  - The prover (`/pouch-hello-dlopen`) is built only when `libc.so` is. An
    image that ships the prover must carry `/lib/libc.so`. Otherwise joey
    fails its smoke check before the pivot and its `/lib` bind after it.
  - `build.sh` fails the image when the sysroot has `libc.so` but the archive
    lacks `lib/libc.so`, `lib/libdlprobe.so` or the prover.
  - Only an image built without the LLVM fork carries none of these, and only
    that image skips.
  - This supersedes the union decision's "With no `lib/` in the initrd, joey
    makes no bind."
- **What the namespace shows.**
  - Before the pivot, `/lib/libc.so` resolves in the initrd itself. After the
    pivot, the `/lib` union serves it. Both `/lib` votes hold as written.
  - `/bin`, the initrd root bound `MREPL` after the pivot, lists `lib/` beside
    the six mount points it already listed.

## Rationale

- ARCH 14.5 always gave the early-boot filesystem per-directory maps.
  Inferno's root device and Linux's initramfs both serve a tree from the boot
  archive. Option 1 makes the boot filesystem match them, and makes both
  `/lib` votes true as written.
- Option 2 is the same mechanism restricted to one name. It saves little, and
  the next directory would need it again.
- Option 3 reverses the union vote's order. A disk `libc.so` would shadow the
  loader, which is the drift that vote was chosen to prevent.
- The cost is an audit-bearing change to a Dev on the exec path. It is
  reviewed in B-1d's round.
- The soft skip is how a missing directory passed the gate. The witness now
  fails whenever the build shipped the thing it proves.
