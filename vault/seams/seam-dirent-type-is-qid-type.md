---
id: seam-dirent-type-is-qid-type
type: seam
title: "devproc and devramfs emit the qid type as the dirent type byte; getdents64 forwards it as d_type"
status: open
surface: [sub-kernel-devproc]
opened-by: fnd-thread-c7-r1-f5
tracker: "OPEN-BUGS 2026-10-29T09:22:54Z"
created: 2026-10-29
updated: 2026-10-29
---
## Owed

A directory entry's type byte is the qid type (QTDIR, 0x80) in devproc's and
devramfs's dirents, and the getdents64 re-encode forwards it as Linux `d_type`,
where a directory is `DT_DIR` (4). A pouch or phenotype program reading `/proc`
or a ramfs directory with getdents64 sees 0x80.

## What closes it

The re-encode maps qid type to `d_type` (QTDIR -> DT_DIR, QTSYMLINK -> DT_LNK,
plain -> DT_REG, unknown -> DT_UNKNOWN), or each Dev emits a d_type; a test
listing `/proc` through getdents64 asserts DT_DIR.

## The risk while open

Wrong file-type hints: a program that trusts `d_type` mistakes every directory
for an unknown type (a `find`-like walker falls back to stat, or skips it). No
authority or memory-safety exposure. Pre-existing, found by C7's audit.
