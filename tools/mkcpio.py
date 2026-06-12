#!/usr/bin/env python3
# tools/mkcpio.py — build a cpio newc archive from a directory.
#
# Per Phase 4 P4-E ramfs. The archive is loaded by QEMU via -initrd
# at boot; the kernel parses it via kernel/cpio.c and populates the
# devramfs file table. Output format: cpio newc ("070701" magic) —
# the standard Linux initramfs format.
#
# Usage: mkcpio.py <srcdir> <outfile>
#
# Files in <srcdir> are added at the archive root (no path prefix).
# Subdirectories are NOT recursed at v1.0 — flat layout only.
# Special files (sockets, devices, FIFOs) are skipped.
#
# Each entry's mode preserves the SOURCE file's permission bits
# (S_IFREG | the low-9 rwx bits) so a chmod 0755 binary is marked
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


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <srcdir> <outfile>", file=sys.stderr)
        return 2

    srcdir, outpath = sys.argv[1], sys.argv[2]
    if not os.path.isdir(srcdir):
        print(f"error: {srcdir} is not a directory", file=sys.stderr)
        return 1

    entries = []
    for fname in sorted(os.listdir(srcdir)):
        path = os.path.join(srcdir, fname)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        # #58: preserve the source's permission bits so a chmod 0755 binary
        # carries the execute bit the kernel's exec X-search requires (a 0644
        # binary would be unspawnable). S_IFREG | low-9 perm bits.
        mode = 0o100000 | (os.stat(path).st_mode & 0o777)
        entries.append((fname, data, mode))

    with open(outpath, "wb") as out:
        for name, data, mode in entries:
            emit_entry(out, name, data, mode)
        emit_trailer(out)

    print(f"mkcpio: wrote {len(entries)} entries to {outpath}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
