---
id: seam-pouch-forkpty
type: seam
title: "`forkpty` and `login_tty` are structurally dead"
status: open
surface: [sub-pouch-tty]
opened-by: chg-2026-07-18-pty3
tracker: "PTY-3"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`forkpty` needs `fork()`, which pouch declines by design (POUCH-DESIGN
8.3) -- `__NR_fork` / `__NR_clone` are sentinels, so musl's `forkpty`
fails honestly at its `fork()` call. `login_tty` needs dup2-onto-target
([[seam-pouch-dup2-target]]).

Not a pty gap: a pouch program gets a pty via `openpty` + `posix_spawn`
with the slave in the child's fd list, which is the Thylacine-shaped way
to do the same thing.

## The lift

Nothing for `forkpty` itself -- it is the wrong primitive on a
fork-less system. `login_tty` follows from the dup2 seam. What a ported
program actually wants is a `forkpty`-shaped convenience over
`openpty` + spawn, which belongs in the port, not the libc.
