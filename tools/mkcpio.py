#!/usr/bin/env python3
# tools/mkcpio.py — build a cpio newc archive from a directory.
#
# Per Phase 4 P4-E ramfs. The archive is loaded by QEMU via -initrd
# at boot; the kernel parses it via kernel/cpio.c and populates the
# devramfs file table. Output format: cpio newc ("070701" magic) —
# the standard Linux initramfs format.
#
# Usage: mkcpio.py <srcdir> <outfile> [--require <name>]...
#
# The tree under <srcdir> is walked in sorted pre-order, so each directory
# is emitted before its contents and the archive's bytes are deterministic.
# Names are relative ("lib/libc.so"); the kernel's devramfs serves the tree
# (ARCH 14.5, dec-2026-09-25-devramfs-directories) and refuses a child whose
# directory has not come first. Special files (sockets, devices, FIFOs) are
# skipped, and so are symlinked directories (a link to a file packs the
# file's bytes). --require names a regular file the written archive must
# hold; a miss deletes the archive and exits 1, so a staging slip fails the
# build instead of booting an image without it.
#
# Each entry's mode preserves the SOURCE's permission bits (S_IFREG or
# S_IFDIR | the low-9 rwx bits) so a chmod 0755 binary is marked
# executable -- the kernel's exec-from-namespace X-search (#58) reads
# this mode via devramfs_stat_native, and a 0644 (no-x) binary would
# be unspawnable. setuid/setgid/sticky are dropped. uid/gid/mtime/
# inode all zero; per-file `c_check` is zero (newc has no checksum).

import os
import sys


def emit_entry(out, name: str, data: bytes, mode: int = 0o100644) -> None:
    """Emit one cpio newc entry: header + name + padding + data + padding."""
    name_bytes = name.encode("utf-8") + b"\x00"
    namesize = len(name_bytes)
    filesize = len(data)

    # newc header: 6-byte magic + 13 8-char hex fields.
    hdr = b"070701"
    fields = [
        0,         # c_ino
        mode,      # c_mode
        0,         # c_uid
        0,         # c_gid
        1,         # c_nlink (1 for regular files)
        0,         # c_mtime
        filesize,  # c_filesize
        0,         # c_devmajor
        0,         # c_devminor
        0,         # c_rdevmajor
        0,         # c_rdevminor
        namesize,  # c_namesize (includes trailing NUL)
        0,         # c_check (newc doesn't compute; crc format does)
    ]
    for v in fields:
        hdr += b"%08x" % v
    assert len(hdr) == 110, f"newc header should be 110 bytes, got {len(hdr)}"

    out.write(hdr)
    out.write(name_bytes)

    # Pad after name to 4-byte boundary (counting from start of header).
    pad = (4 - (len(hdr) + namesize) % 4) % 4
    out.write(b"\x00" * pad)

    out.write(data)
    pad = (4 - filesize % 4) % 4
    out.write(b"\x00" * pad)


def emit_trailer(out) -> None:
    """Emit the cpio newc trailer entry (name 'TRAILER!!!', size 0)."""
    emit_entry(out, "TRAILER!!!", b"", mode=0)


def collect(srcdir: str) -> list:
    """(name, data, mode) for every directory and regular file under srcdir,
    in sorted pre-order: a directory precedes its contents."""
    entries = []

    def walk(rel: str) -> None:
        base = os.path.join(srcdir, rel) if rel else srcdir
        for fname in sorted(os.listdir(base)):
            path = os.path.join(base, fname)
            name = f"{rel}/{fname}" if rel else fname
            perm = os.stat(path).st_mode & 0o777 if os.path.exists(path) else 0
            if os.path.isdir(path):
                if os.path.islink(path):
                    continue
                entries.append((name, b"", 0o040000 | perm))
                walk(name)
            elif os.path.isfile(path):
                with open(path, "rb") as f:
                    data = f.read()
                # #58: preserve the source's permission bits so a chmod 0755
                # binary carries the execute bit the kernel's exec X-search
                # requires (a 0644 binary would be unspawnable).
                entries.append((name, data, 0o100000 | perm))

    walk("")
    return entries


def read_names(path: str) -> dict:
    """{name: mode} for every entry in the newc archive at path, parsed from
    the written bytes (the artifact, not the list it was built from)."""
    names = {}
    with open(path, "rb") as f:
        blob = f.read()
    off = 0
    while off + 110 <= len(blob):
        if blob[off:off + 6] != b"070701":
            raise ValueError(f"bad newc magic at offset {off}")
        mode = int(blob[off + 14:off + 22], 16)
        filesize = int(blob[off + 54:off + 62], 16)
        namesize = int(blob[off + 94:off + 102], 16)
        name = blob[off + 110:off + 110 + namesize - 1].decode("utf-8")
        if name == "TRAILER!!!":
            return names
        names[name] = mode
        data_off = (off + 110 + namesize + 3) & ~3
        off = (data_off + filesize + 3) & ~3
    raise ValueError("no newc trailer")


def main() -> int:
    args = sys.argv[1:]
    required = []
    while "--require" in args:
        i = args.index("--require")
        if i + 1 >= len(args):
            print("error: --require needs a name", file=sys.stderr)
            return 2
        required.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(f"usage: {sys.argv[0]} <srcdir> <outfile> [--require <name>]...",
              file=sys.stderr)
        return 2

    srcdir, outpath = args
    if not os.path.isdir(srcdir):
        print(f"error: {srcdir} is not a directory", file=sys.stderr)
        return 1

    entries = collect(srcdir)
    with open(outpath, "wb") as out:
        for name, data, mode in entries:
            emit_entry(out, name, data, mode)
        emit_trailer(out)

    ndirs = sum(1 for _, _, mode in entries if mode & 0o170000 == 0o040000)
    print(f"mkcpio: wrote {len(entries)} entries ({len(entries) - ndirs} files, "
          f"{ndirs} directories) to {outpath}")

    if required:
        names = read_names(outpath)
        missing = [r for r in required
                   if names.get(r, 0) & 0o170000 != 0o100000]
        if missing:
            os.remove(outpath)
            print(f"error: {outpath} lacks required file(s): {' '.join(missing)} "
                  f"-- archive deleted", file=sys.stderr)
            return 1
        print(f"mkcpio: required present: {' '.join(required)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
