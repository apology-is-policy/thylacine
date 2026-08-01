# 83 — pouch signals [ABSORBED INTO THE VAULT]

Absorbed at the pouch sweep (`chg-2026-08-01-pouch-sweep`). Its content
now lives, code-verified and current, in:

    vault/system/boundary/pouch-seam/sub-pouch-signal.md

(the bootstrap dispatch, the constructor ordering, the SIG_DFL matrix
including the SIGTSTP seam, the tty family's receive-only gate, and the
per-Thread mask shadow.)

**What this file got WRONG by the time it was absorbed.** Its "Known
caveats" still listed `abort()` extincting the kernel as a live
limitation, and proposed as a "v1.x extension" — "(1) override pouch's
`abort.c` to `_Exit(127)` directly, bypassing `a_crash`" — the EXACT
change that shipped in `0011-pouch-abort.patch`, documented in a
different reference doc (`86-pouch-stratumd-boot.md`), so the proposal
and its implementation never met.

Its `fstat`-on-a-notes-fd section (#97) belongs to the kernel's notes
surface rather than to pouch, and lands with that sweep.

Binding design (unchanged): `docs/POUCH-DESIGN.md` §6.4, ARCH §7.6,
`docs/PTY-DESIGN.md` §7.
