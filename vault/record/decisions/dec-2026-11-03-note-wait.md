---
id: dec-2026-11-03-note-wait
type: dec
title: "One mask restore at a handler's return, and SYS_NOTE_WAIT for sigsuspend and sigtimedwait"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-abi, sub-pouch-signal]
created: 2026-11-03
---
## Fork

`sigsuspend` needs a mask and a sleep with no window between them, and `sa_mask` and
`SA_RESETHAND` need a mask restored at a handler's return. The Linux path already saved and
restored; the native path did neither, and Pouch kept its own copy of the mask.

The operator's vote 24: "sigsuspend and sigtimedwait: one chunk after the thread model, with
sa_mask and SA_RESETHAND. One mechanism restores a mask at handler return. A new native
syscall on the next free number (this is the signoff for that ABI addition), both phenotypes.
sigtimedwait is scoped in that chunk."

## Decision

One restore, unconditional, in `notes_noted_restore`, for both phenotypes. One number, 141,
with two forms; `rt_sigsuspend` and `rt_sigtimedwait` sit on the same cores. 139 was
`SYS_ACCOUNT` on corona/async-memory as well as this tree's `SYS_NOTE_DISPOSE`, so corona moved
to 140 (yip call 0279).

Three things this choice decided that the vote did not name, all inside its scope and all on
syscalls that have not reached main:

- `SYS_NOTE_DISPOSE`'s flags word carries `NOTE_DISP_RESETHAND`, `NOTE_DISP_NODEFER` and the
  row's mask in bits 16..31. No fifth register: an old caller's `x4` is not read, and the
  refusal of unknown bits already made the word extensible.
- Pouch's mask shadow goes. A read-modify-write is two swaps.
- A `sigsuspend` in a handler ends at a note its mask admits, and that note waits for the
  handler's return. `in_handler` holds every note, and nesting is a larger change.

## Why not

- A fifth register for the mask: the call has room in its word, and a register is a slot a
  later call cannot take back.
- Keeping the shadow and writing it from the dispatcher: the kernel moves the mask at entry and
  at `SYS_NOTED`, and a copy kept right across a `siglongjmp`, a `sigsuspend` and a peer's fork
  is more code than the one extra syscall a read-modify-write costs.
- The class setting `NOTE_WF_NOHAND`: `native_note_class` runs only for a Proc with a handler or a
  notes fd, and a call with neither must still re-issue after its tail.
