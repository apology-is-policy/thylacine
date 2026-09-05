---
id: fnd-signals13b-r1-f11
type: fnd
round: adt-signals13b-r1
severity: P3
status: fixed
title: "sigaction accepted SIG_ERR as a handler"
surface: [sub-pouch-signal]
threatens: []
fixed-by: chg-2026-05-24-p6-signals-b
created: 2026-08-01
---
## Prosecution

`SIG_ERR` is `(void(*)(int))-1` — the error RETURN from `signal(3)`, not
a valid `sigaction` input. pouch recorded it into the table, so the
bootstrap would later call `h(SIGINT)` at address -1: an EL0 fault, which
at the time extincted the kernel.

A program that forgets to check `signal()`'s return and feeds it back is
enough.

## Fix

Explicit `EINVAL` rejection at `sigaction`. POSIX is silent here; some
implementations reject, and on this kernel rejecting is the only safe
reading.
