---
id: dec-2026-09-28-t-stat-devno-u64
type: dec
title: "t_stat.devno is 64 bits, widened in place over _pad_dev"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [abi-t-stat, sub-kernel-syscall-abi, sub-kernel-spoor]
created: 2026-09-28
---
## Fork

The kernel's device number (`spoor_next_devno`, Plan 9 `Chan.dev`) was a
monotonic `u32` with no refusal at wrap, and every Env and every dev9p / devsrv
attach mints one, so an unprivileged fork loop drives it. Three kernel identity
keys assume a live instance's number is unique: the mount key, `MNOEXEC`
coverage and the Image cache key. Making the kernel's number 64 bits and never
reused fixes all three with no ABI change. It leaves `t_stat.devno`, the field
userspace keys file identity on together with `qid_path` (Go's `sameFile`,
gopls's `robustio` FileID, musl's loader dedupe, `find`'s cycle check), at 32
bits: two instances 2^32 mints apart would still read as one file to
userspace.

## Research

- **Plan 9.** `Chan.dev` is a `ulong` (32 bits on the 386), and 9P2000's stat
  `dev[4]` is 32 bits; the counters wrapped and nothing relied on them staying
  unique.
- **Linux.** `st_dev` is a 64-bit `dev_t` on 64-bit targets; anonymous device
  numbers come from an ID allocator that reuses freed numbers.
- **Fuchsia.** Kernel object ids are 64-bit, monotonic and never reused.
- **The tree.** `t_stat` carries a 32-bit `_pad_dev` right after `devno`, so
  the field can widen over it with offset 80 and the 88-byte size unchanged;
  on a little-endian machine an old reader of the low 32 bits at 80 reads what
  it read before. The record has seven mirrors ([[abi-t-stat]]); six name the
  field and change, and pouch `0024` reads only `mode`.

## Options

1. **Widen in place to 64 bits.** Absorb `_pad_dev`; libt, libthyla-rs, pouch
   patches `0010` / `0019` / `0021` and the go-thylacine `Stat_t` change in
   the same commit; the Linux phenotype's `st_dev` gets the full width.
2. **Keep 32 bits, display only.** No ABI change; userspace identity can alias
   after 2^32 mints in one boot, documented as a known limit.

Rejected for the kernel's own number, before the vote: refusing an attach at
the wrap (an unprivileged fork loop would then deny every later attach on the
system), and reusing freed numbers (an Image cache entry outlives the Spoor it
was keyed from, so a recycled number could match a dead instance's pages).

## The call

Option 1 (operator, 2026-09-28, asked with the B-2 forks). The kernel's device
number is 64 bits and never reused, and `t_stat.devno` carries all of it at
offset 80; a width assertion in `kernel/include/thylacine/syscall.h` pins it.
