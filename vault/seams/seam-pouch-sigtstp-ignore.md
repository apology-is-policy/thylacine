---
id: seam-pouch-sigtstp-ignore
type: seam
title: "SIG_DFL `SIGTSTP` is IGNORE, not STOP"
status: open
surface: [sub-pouch-signal, sub-pouch-tty]
opened-by: chg-2026-07-18-pty3
tracker: "PTY-3"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`^Z` on a handler-less pouch program does nothing. Two mechanisms
compose into it: the kernel's pre-delivery stop gate treats a Proc with a
registered notify handler as "caught", and pouch's constructor ALWAYS
registers the bootstrap -- so every pouch Proc is caught and the
`tty:susp` note delivers instead of stopping the Proc; and the kernel's
`SYS_NOTED(NDFLT)` arm TERMINATES (`exits` -> `proc_group_terminate`)
rather than stopping, so the bootstrap cannot re-enter the default
either.

`NCONT` (ignore) is therefore the least-wrong answer available:
`NDFLT` would turn `^Z` into process death, which is the one actively
harmful option. A program with a real SIGTSTP handler is unaffected --
the handler runs, per POSIX.

## The lift

A kernel `NDFLT`-stop arm for `tty:susp` -- `NDFLT` applies the note's
TRUE default, which for the suspend class is stop, not terminate. That
is an ABI-semantics change on the audited notes surface and needs
signoff; it is deferred alongside the `SYS_POSTNOTE` pgrp arm.
