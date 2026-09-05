---
id: chg-2026-07-22-55c-cons-winsize
type: chg
title: "#55c: the console arm of the ioctl dispatcher"
date: 2026-07-22
arc: arc-tapestry
commits: ["3ca21d5d"]
touched:
  - sub-pouch-tty
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Pre-#55 a cons fd was STATLESS, so the dispatcher folded the failed
fstat to `ENOTTY`, `isatty()` was false on the console, and stdio ran
fully buffered -- a latent POSIX defect with no interactive victim until
graphics. The kernel half gave cons a `stat_native` with an `S_IFCHR` +
bit-41 marker (disjoint from ptyfs's bit 40); this patch adds the
`cons_resolve` arm on the pts miss.

`TIOCGWINSZ` succeeds even at 0x0 or with `/dev/winsize` unreachable,
because that call IS `isatty()` and a cons fd is a terminal.
`TIOCSWINSZ` is `EPERM`: the console geometry is physical.
