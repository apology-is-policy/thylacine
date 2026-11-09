---
id: dec-2026-11-03-note-dispose-table
type: dec
title: "A native Proc gets the per-note table, written by SYS_NOTE_DISPOSE"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-abi]
created: 2026-11-03
---
## Fork

A blocked Pouch program ignored a caught note until its wait ended. The gate was `PHENO_LINUX` only, and item 11c owed natives a way in (vote 16). Pouch's constructor registers one handler for every program, so `handler_va != 0` reads as caught for every note: a per-Proc opt-in, or Plan 9's rule that catching is the opt-in, would unwind a `read` for a `SIGCHLD` nobody handles and return `EINTR` from it, the class `dec-2026-09-29-caught-signal-slow-calls` closed for the phenotype. The same reading is why a `SIG_DFL` Pouch program blocked in a wait does not die on `^C`: no terminate latch arms. Read in code, not reproduced when the vote was asked.

Options: one per-note table, the phenotype's, with a new syscall; three mask words on the Proc with the same syscall; Plan 9's shape, no syscall, every reader changed first.

## Decision

Vote 18 of 2026-11-03, as recommended: native programs get `struct viv_sigtab`, written by `SYS_NOTE_DISPOSE` = 139, the signoff for that ABI addition. Pouch's `sigaction` writes it and registers its dispatcher only when a real handler is installed, so `SIG_DFL` gets the kernel's default. Only a caught row's note unwinds a wait. Other natives stay as they are until they write a row.

The table already has an audited lifetime: one immortal object per Proc, whole-field cross-Proc reads, fork's copy and exec's reset. Masks would have been a second mechanism beside it, kept in step at every gate. Plan 9 cannot separate a `SIGWINCH` handler from a default `SIGHUP`, because the tty family is one bit and five names, while the table has one row per note.
