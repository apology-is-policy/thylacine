---
id: seam-sak-revoke-note
type: seam
title: "SAK-revoke has no note of its own"
status: open
surface: [sub-kernel-proc]
opened-by: chg-2026-08-01-proc-thread-sweep
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A dedicated note name for "you lost the console" — something like `hangup`
or `console-revoked`. Today the SAK transition posts NOTHING to the Proc it
revokes; the attach-bit clear is its only observable effect on the old
owner.

## What closes it

A new note in the kernel-synthetic family, plus a disposition decision: it
must be informational, since the whole reason the current post was removed
is that a terminate-class note kills the wrong thing.

## Risk while open

Silent revocation. A Proc that held the console attach loses its elevation
authority with no signal.

The history is the useful part: the SAK USED to post `interrupt` as a
courtesy, which was benign until LS-5 made `interrupt` a real
terminate-if-uncaught note. After LS-5 that courtesy TERMINATED a
non-self-managing old owner — joey during bringup, i.e. init — or
spuriously killed a session shell's foreground command. Removing the post
was the correct emergency fix; adding the right note is the deferred one.
