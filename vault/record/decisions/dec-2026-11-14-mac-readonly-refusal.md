---
id: dec-2026-11-14-mac-readonly-refusal
type: dec
title: "A read-only shared folder refuses an ordinary user's change in the guest, from the modes; the app's EROFS answers what the kernel passes on"
date: 2026-11-14
status: standing
decided-by: user-vote
affects: []
created: 2026-11-14
---
## Fork

`docs/MAC-RUNNER.md` 4.4 had two rules for a read-only shared folder: a modifying operation
answers EROFS, and the owner bits are the host file's with write taken away. Guest run 12
(2026-11-14, cebf236be) showed they meet in the guest's kernel first. `ls-mac-folders` leg 6
created a file in the read-only folder and got `permission denied`, not EROFS, and the app's
record holds no Tlcreate. dev9p enforces rwx (`kernel/dev9p.c` `.perm_enforced = true`, A-3b,
IDENTITY-DESIGN 3.7.1 and 9.7): create, rename and remove need the parent's write and search
bits, an open for writing needs the file's write bit (`kernel/syscall.c`, `perm_check`). The
folder's modes carry no write, so the kernel refuses before it asks the app. Only what
`perm_wstat_check` passes on reaches the app: the owner's change of a mode or of explicit times
(`kernel/include/thylacine/perm.h`), and any change by a CAP_HOSTOWNER holder, whose check
short-circuits.

## Research

- Plan 9: the client kernel checks no modes on a mount; the file server decides, and its error
  is what the caller sees. Thylacine's dev9p checks them in the guest as well (A-3b), a
  deliberate step past the heritage.
- Linux: a mode without write gives an ordinary user EACCES; EROFS comes from a read-only
  mount (`mount -o ro`), whatever the modes say.
- The tree: the guest-visible answer was untested. The scripted guest that stood in for a guest
  had no `perm_enforced` model.

## Options

1. As built: keep both rules. An ordinary user's change reads `permission denied`; EROFS
   answers what reaches the app. MAC-RUNNER 4.4 says so in one sentence.
2. The app keeps the write bits in a read-only folder, so every change reaches it and answers
   EROFS. Modes would then claim a write that cannot happen.
3. A read-only mount flag in the guest, so the kernel answers EROFS itself.

## The call

The operator, 2026-11-14: option 1, as recommended (vote M14).

## Rationale

Modes that tell the truth about what can be done are the Plan 9 answer, and the guest refuses
without a round trip to the Mac. EROFS stays the app's answer for whatever reaches it, so the
app's rule needs no change. The manual and `ls-mac-folders` leg 6 say which answer an ordinary
user sees, and the controls that test the app's own rule use what reaches it (a mode change).
