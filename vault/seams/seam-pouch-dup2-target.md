---
id: seam-pouch-dup2-target
type: seam
title: "`dup2`/`dup3` onto a chosen target fd has no kernel primitive"
status: open
surface: [sub-pouch-process]
opened-by: chg-2026-07-23-cl1b-process
tracker: "CL-1b"
created: 2026-08-01
updated: 2026-10-15
---
## Owed

`handle_dup` allocates the lowest free slot — that is `dup()`. There is
no dup-onto-a-chosen-fd, which is what `dup2` is DEFINED by. The
toolchain never needs it (posix_spawn resolves its file_actions
statically into the positional spawn fd list, and popen/system route
through that resolver), so the call fails loud with `ENOSYS`.

Left non-functional: `freopen(filename, …)` (reopen a stream onto a
fixed fd), `login_tty`, `daemon`, `wordexp`.

## The lift

A kernel dup-onto-target primitive — a new syscall, hence an ABI
addition to escalate when a real workload needs one. Native internal
`handle_dup_to` now exists for Vivarium, but has no native syscall exposure;
lowest-free `SYS_DUP` cannot implement atomic replacement.

Patch0054 closes only the self-alias gap: `old == new` now checks native129
metadata or the Pouch socket slot under its owner lookup. The earlier
implementation returned success without validating nonnegative descriptors.
Self-dup preserves flags and performs no close/reopen. Distinct-target support
remains open; this note does not claim it was delivered.
