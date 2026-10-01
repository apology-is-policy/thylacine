---
id: dec-2026-09-29-caught-signal-slow-calls
type: dec
title: "A caught signal interrupts only the calls Linux lets it interrupt (signal(7)'s list)"
date: 2026-09-29
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-fault, sub-kernel-thread, sub-kernel-spoor]
created: 2026-09-29
---
## Fork

NP-5's SMP gate failed 3 boots in 50: the V-1b probe's `socket()` returned
`EINTR` after the kernel logged `9p: op abandoned (tag 0, note, flush sent)`.
The cause predates NP-5. Item 11 (ARCH 8.8.3) lets a caught note unwind any
wait that opted in, and both 9P waits opted in: the client's RPC wait and the
elected reader's receive. For a Linux-phenotype Thread every 9P-backed call is
such a wait. So a `SIGCHLD` handler made `socket()`, `bind()`, `openat()`,
`newfstatat()` and a regular file's `read()` fail with `EINTR`. It also failed
a demand-paged file read, which raises `SIGBUS`. The probe's counting `SIGCHLD`
handler was live while two children were still exiting, and NP-5's new legs
were the first to make 9P calls in that window.

Which calls may a caught signal interrupt?

## Research

- **Linux, the phenotype's oracle (signal(7)).** A handler interrupts only a
  *slow* call that is blocked when it arrives. These are `read`, `readv`,
  `write`, `writev` and `ioctl` on a slow device (a pipe, a socket, a
  terminal); `open` of a FIFO; `wait*`; `accept`, `connect`, `recv*` and
  `send*`; `flock` and `F_SETLKW`; the `poll` family, which is never restarted;
  sleeps; `futex` waits; and `rt_sigsuspend` / `rt_sigtimedwait`. Every other
  call sleeps `TASK_KILLABLE`, including a disk file's I/O and a page fault.
  Only a fatal signal wakes it, and the handler runs when the call returns.
  Linux's own 9P client (v9fs) waits with `wait_event_killable` for this
  reason.
- **Plan 9.** A note interrupts any wait. That is the heritage rule for native
  programs, but Thylacine's natives are not caught-note-interruptible yet:
  `proc_caught_note_eintr_ready` admits only the Linux phenotype, and item 11c
  owes the native opt-in.
- **The tree at 26e8d367.**
  - The unwind has one predicate, `thread_caught_note_deliverable` (notes.c).
    Only the four caught arms of `sleep_common` and `tsleep_common` (sched.c)
    read it.
  - Only two waits opt in: the 9P client's RPC wait (`9p_client.c`) and the
    srvconn receive (`srvconn.c`). A pipe or console read uses plain `sleep`.
  - Every page-in enters through `userland_demand_page` (fault.c). A failed
    file read there is `FAULT_USER_BUS`.

## Options

1. **signal(7)'s list.** Only Linux's interruptible calls unwind for a caught
   note. Every other call, and every page-in, waits it out and can still be
   killed.
2. **Exempt socket setup only** (`socket`, `bind`, `listen`, `setsockopt`).
   The gate's failure goes away, but `openat`, `newfstatat` and regular-file
   I/O still fail with `EINTR`, and the fault path is untouched.
3. **Restart the interrupted call in the kernel** (`ERESTARTNOINTR`). An
   abandoned 9P operation may already have taken effect on the server, since a
   `Tflush` does not undo a completed create or dial. Re-issuing it is not
   idempotent.

## The call

Option 1 (operator, 2026-09-29).

A positive per-Thread flag, `note_interruptible`, gates the unwind. It
defaults to false. The vivarium dispatcher sets it only for a call on the
list, and `syscall_dispatch` clears it on the way out. The list:

- **Always:** `accept`, `accept4`, `connect`, `recvfrom`, `recvmsg`, `sendto`,
  `sendmsg`, `wait4`, `ppoll`, `pselect6`, `futex`, `rt_sigsuspend`,
  `rt_sigtimedwait`, and `fcntl` with `F_SETLKW` or `F_OFD_SETLKW`.
- **On a slow file only:** `read`, `readv`, `write`, `writev`, `pread64`,
  `pwrite64` and `ioctl`. A file is slow if it has a socket-table row, or else
  if its Dev's stat type is `S_IFIFO` or `S_IFCHR`. That answer is learned once
  per open file and cached on the Spoor.
- **Never:** everything else. `userland_demand_page` clears the flag for a
  page-in and restores it afterwards.

Death is untouched. `thread_die_pending` is still checked first at every sleep
site, so a kill or a group exit still unwinds every wait. A caught note that
arrives during a call that is not on the list is delivered when that call
returns to EL0.

## Rationale

It is the phenotype's contract. A Linux program is written against signal(7),
and no Linux program retries `socket()` on `EINTR`.

The flag is positive so that a new row, or a new wait inside an old row, can
only be killed until someone puts it on the list. The failure mode is then a
handler that runs late, never a spurious `EINTR`.

Option 2 fixes the symptom the gate saw and leaves the same defect on every
other 9P call. Option 3 would replay operations that may already have
happened.
