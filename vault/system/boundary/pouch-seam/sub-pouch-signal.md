---
id: sub-pouch-signal
type: sub
parent: moc-pouch-seam
title: "POSIX signals over kernel notes"
code:
  - usr/lib/pouch/patches/0007-pouch-signals.patch
  - usr/lib/pouch/patches/0049-pouch-sigterm-term-note.patch
  - usr/lib/pouch/patches/0069-pouch-note-table.patch
  - usr/lib/pouch/patches/0071-pouch-note-wait.patch
audit: hard
guarded-by: [inv-i24]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md", "docs/PTY-DESIGN.md"]
created: 2026-08-01
updated: 2026-11-03
---
## Purpose

`sigaction` / `raise` / `kill` / `sigprocmask` on a kernel that has
string-named Plan 9 notes instead of numbered signals. pouch keeps the
handlers in its own table and writes each note's disposition in the kernel's
(`SYS_NOTE_DISPOSE`, [[sub-kernel-notes]]). The kernel decides what a note does;
pouch's one dispatcher runs the handler for a caught note.

## Contract

- `sigaction(sig, sa, old)` — records into `__pouch_sigtab[sig]` and writes the
  kernel's row; accepts exactly the supported set, `EINVAL` for everything else
  (which subsumes SIGKILL/SIGSTOP, POSIX-correctly). A refused row puts the old entry
  back and answers the errno.
- `raise(sig)` → `SYS_POSTNOTE(pid=0, name, len)` — the kernel's
  self-post sentinel. `kill(pid, sig)` → the same with the caller's pid.
- `pthread_sigmask` / `sigprocmask` → `SYS_NOTE_MASK`, marshalling
  `sigset_t` ↔ `NOTE_BIT_*`.
- `sigsuspend` / `sigpause` → `SYS_NOTE_WAIT`'s suspend: the mask and the sleep are one
  step. `sigtimedwait` / `sigwait` / `sigwaitinfo` → its take.
- `sigsetjmp(env, 1)` saves the mask and `siglongjmp` restores it.
- The bootstrap `__pouch_note_handler(name, arg, flags)` never returns
  normally: it ends in `SYS_NOTED(NCONT)` or `SYS_NOTED(NDFLT)`.

The supported set: `SIGINT` → `"interrupt"`, `SIGTERM` → `"term"` (its
own note since 0049, 2026-10-13; before it SIGTERM shared `interrupt`),
`SIGPIPE` → `"pipe"`, `SIGCHLD` → `"child_exit"`, `SIGKILL` → `"kill"`
(non-catchable), plus the PTY-3 tty family — `SIGQUIT` → `"tty:quit"`,
`SIGTSTP` → `"tty:susp"`, `SIGCONT` → `"tty:cont"`, `SIGWINCH` →
`"tty:winch"`, `SIGHUP` → `"tty:hup"`.

`SIGABRT` is outside the set: `sigaction(SIGABRT)` answers `EINVAL`, so its
default is its only disposition. `abort()` does not raise it — it executes
the `brk` the kernel ends as `snare:abort` ([[sub-pouch-process]]) — and since
0049 `raise(SIGABRT)` and a `kill` of the caller with it call `abort()`; a
`SIGABRT` to another process answers `EINVAL`, since no note carries it.
`SIGSEGV`, `SIGBUS`, `SIGILL` and `SIGFPE` are outside the set the same way,
and since 0050 (2026-10-14) `raise()` of one, or `kill()` of one to the caller
(pid 0 or `getpid()`), posts the snare it names (`__pouch_self_snare`) to the
caller, and the kernel ends the process with it
([[dec-2026-10-14-self-death-snare]]); to another process they answer
`EINVAL`. The reverse direction
does carry signals since gcpark-b (2026-10-12): the kernel records the
signal of every death by note, a fault (`snare:segv` → `SIGSEGV`, …,
`snare:abort` → `SIGABRT`) or an uncaught terminating note (`interrupt` →
`SIGINT`, `term` → `SIGTERM`, `usr1` → `SIGUSR1`, `usr2` → `SIGUSR2`,
`kill` → `SIGKILL`, `pipe` → `SIGPIPE`,
`tty:quit`, `tty:hup`),
and a parent's `waitpid` reads it as `WIFSIGNALED` / `WTERMSIG`
(docs/ERRORS.md "The wait status"). No handler runs for a fault signal.
The musl `__abort_lock` interlock in `sigaction` is not carried: Pouch's
`abort()` never takes the lock, so it has nothing to guard.

## Mechanism

**No handler until a program asks for one (0069, operator vote 18).** The
`.init_array` constructor runs before `main`, sets `NOTE_BIT_PIPE` in the kernel
mask (SIGPIPE masked-by-default — the modern-daemon posture), and writes
`child_exit`'s default row. That row allocates nothing, and it makes the kernel's
table govern from the image's start, so there is no window in which every note
reaches a catch-all, and the kernel discards a dead child's note at the tail, where no
reader would ever take it. The constructor registers no handler. It used to register
one, and to the kernel that read as "caught" for every note: a `SIG_DFL` `SIGINT` armed
no terminate latch, so a program blocked in a wait did not die until the wait ended,
and `^Z` did nothing.

**One handler, dispatched by name.** A `sigaction` with a real handler calls
`SYS_NOTIFY` with `__pouch_note_handler`, then writes a caught row, with
`NOTE_DISP_RESTART` for `SA_RESTART`. It registers on every such install: registering
is idempotent, and no flag in the image can disagree with the kernel in a forked child.
Until the row lands its note takes the default, as POSIX has it before `sigaction`
returns. `SIG_IGN` writes an ignore row and `SIG_DFL` a default one. The kernel calls the
dispatcher Plan-9-style (`x0` = a pointer to the 16-byte NUL-padded note name on the user
stack, `x1` = the arg), pouch maps the name to a signum with a bounded 16-byte compare,
looks up the handler, and ends with the right `SYS_NOTED`.

**The mask is the kernel's alone (0071, operator vote 24).** The kernel moves it at a
handler's entry, at `SYS_NOTED` and across a `sigsuspend` ([[sub-kernel-notes]]), so a copy
here would be wrong in a handler and after one, and the TLS shadow went. `SYS_NOTE_MASK`
swaps and hands back the old mask. `SIG_SETMASK` is one call. Anything that needs the old
mask to compute the new one calls `__pouch_note_mask_hold`, which blocks every note and
returns the old mask, and then names the mask it wants: two calls with nothing deliverable
between them. `pthread_sigmask` reads the caller's set before any swap, so a bad pointer
faults with the mask as it was.

**A caught row carries the rest of the `struct sigaction`.** `SA_RESETHAND`, `SA_NODEFER`
and `sa_mask`, in note bits from bit 16, go in `SYS_NOTE_DISPOSE`'s flags word. The kernel
hands the dispatcher the row's flags in `x2`. Under `NOTE_DISP_RESETHAND` the dispatcher
resets the table with a compare from the handler it read, so a peer's `sigaction` that has
landed since stands. The kernel has reset its own row by then, under the queue's lock, so
two threads cannot both read caught.

**The waits.** `sigsuspend` translates its set to note bits and calls `SYS_NOTE_WAIT`.
`sigtimedwait` translates with `__pouch_sigset_to_take`, which names each tty signal's own
row, and reads the taken note's name back through `__pouch_note_to_sig`, the dispatcher's
table. Upstream's `EINTR` loop stays, because `sigwait` may not read `EINTR`, but a retry
waits only for what is left of the timeout: a steady note would otherwise keep the timeout
from passing. `si_code` is `SI_KERNEL` with a zero union, as on a Linux guest's row.

**Every handler gets the `SA_SIGINFO` arguments.** A handler installed with `SA_SIGINFO`
used to find the dispatcher's own `x1` and `x2` in its arguments, so reading `si_signo` read
through a note arg. The dispatcher now builds a `siginfo_t` for every delivery and passes
`(sig, &si, NULL)`, which a `void (int)` never reads. It does not look at `sa_flags` to
decide, because its read set stays {`sa_handler`}.

**A blocked call unwinds for a caught note, and the kernel re-issues it** (votes 19 and
20). Nothing in pouch or musl re-issues a call; the comment that said `SA_RESTART`
needed nothing was false. The patch's other half makes each blocking wrapper answer
what the kernel now says: `waitpid` surfaces `EINTR` (it flattened every failure to
`ECHILD`), `pause()` sits on `SYS_poll` over no fds (`SYS_ppoll` is an `ENOSYS` sentinel,
so it returned at once), and `poll` and `select` over no fds sleep for their timeout. The
listen open and the data open set `SYS_WALK_OPEN_OINTR`. A `connect` a note unwinds
leaves the slot `POUCH_SOCK_CONNECTING`: the request stands, as POSIX has it, so a retry
waits on the data open and never dials again, and I/O on such a slot waits for the
connection first.

**SIGINT and SIGTERM have a note each** since 0049: the dispatcher maps
`interrupt` to SIGINT and `term` to SIGTERM, and each has its own mask bit
(`POUCH_NOTE_BIT_TERM` is bit 7; the supported mask is 0xaf). Before it they
shared `interrupt`: the dispatcher guessed by handler-presence (SIGINT's if
installed, else SIGTERM's), blocking SIGINT blocked SIGTERM, and a child
killed with SIGTERM read `WTERMSIG == SIGINT`.

**SIGUSR1 and SIGUSR2 have a note each** since 0068 (operator vote 15, 2026-11-02):
`usr1` and `usr2`, bits 8 and 9, which took the supported mask to 0x3af. `sigaction`,
`kill`, `raise` and `pthread_kill` all answered EINVAL for them before it. SIGALRM is
now what a signal with no note means.

**The tty family is RECEIVE-ONLY.** The kernel's POST axis rejects any
userspace `tty:*` post — the I-39 gate that makes a terminal event
unforgeable, since only a pts's minting server may originate one. So
`kill()`/`raise()` of those five signums answer `EPERM` at the pouch
layer (the POSIX-shaped errno; the kernel would refuse with a bare `-1`).
All five also share ONE kernel mask bit (`NOTE_BIT_TTY`), so blocking any
one blocks the family — documented coarseness, made survivable by the
kernel's terminate-class latch (a masked `tty:quit`/`tty:hup` fires on
unmask).

**A signal left at `SIG_DFL` reaches no handler.** The kernel takes the note's
default itself, in a wait as well as at the tail: `SIGTSTP` stops the Proc, and quit,
hup, int, term, usr1 and usr2 end it. The dispatcher's `SIG_DFL` arms remain for a
peer's reset that crosses a delivery: the kernel read the row as caught, and the table
reads default by the time the dispatcher looks. There `NDFLT` applies the note's own
default and `NCONT` is kept for the default-ignored ones.

**The `NDFLT`-refusal fallback is now defense-in-depth only.** At 13b the
kernel refused `NDFLT` in a multi-thread Proc (cross-thread shootdown was
v1.x), and a bootstrap that dropped into `for(;;)` after the refusal
wedged the thread permanently with `in_handler` still true — the second
**P1** of that round ([[fnd-signals13b-r1-f2]]). The fix retries `NCONT`.
Since #809 + the RW-8 fix the kernel's `NDFLT` cascades instead of
refusing, so the fallback no longer fires on the normal path — and
relying on it to swallow a terminating signal would now be the bug.

**The asm restorers are dead but pointed somewhere safe.** pouch's
`sigaction` never installs `sa_restorer`, so `__restore` / `__restore_rt`
are unreferenced; they were still rewritten to call `SYS_NOTED(NCONT)`
instead of the `rt_sigreturn` sentinel, with a `b .` trap after the `svc`
so an impossible return cannot fall off the end into adjacent `.text`.

## Data structures

`__pouch_sigtab[_NSIG]` — per-Proc dispositions (POSIX semantics). The mask has no
userspace copy. The kernel starts a new thread with its creator's mask, which
`pthread_create` has set to every supported note across the spawn, and the thread's first
act, `start()`/`start_c11()`, restores the creator's own mask through `__restore_sigs`
(0065). No note can run a handler in that window, since every one is blocked.

## Concurrency

The sigtab is written by `sigaction` and read by the bootstrap on
whatever thread takes the note. The struct copy is multi-word, but the
bootstrap reads ONLY `sa_handler` (offset 0, naturally aligned, atomic on
aarch64) — so a torn read is impossible *today*, and any future
enhancement that reads `sa_mask` or `sa_flags` in the bootstrap must
first make the store atomic. The table's reset is a compare on that one word. A
`sigaction` that lands between the kernel's reset of its row and the dispatcher's read
of `h` is read as the handler to run, and the compare then resets the table it wrote while
its row stays caught: the next delivery reads default. That needs a `sigaction` racing a
one-shot delivery, and it is stated here rather than fixed.

## Invariants enforced

- **[[inv-i24]] (consumer side)** — a SIG_DFL terminating disposition
  reaches `NDFLT` → `exits` → `proc_group_terminate`, so an uncaught
  fatal signal terminates the whole Proc, matching POSIX.
- **N-3 (kernel-side re-entrancy)** shapes the design: the kernel keeps
  `in_handler` true until `SYS_NOTED`, so the bootstrap MUST reach one on
  every path — which is why each arm ends with a `SYS_NOTED` + an
  unreachable `for(;;)`.
- **P-3** — SIG_ERR is rejected at `sigaction` (otherwise the bootstrap
  would call address -1 and, pre-`snare:*`, extinct the kernel).

## Error paths

`EINVAL` for an unsupported signum or `SIG_ERR`. `EPERM` for a
`kill`/`raise` of the tty family. Since 0050 a refused `SYS_POSTNOTE` reads
as the kernel names it ([[dec-2026-10-14-postnote-errnos]]): `ESRCH` for a
missing pid, `EAGAIN` for a full note queue, `EINVAL` for a name the kernel
refuses, and `EPERM` for a process that is not the caller's child -- the
kernel answers `-EACCES`, and `kill()` sets `EPERM` itself, because `-EPERM`
is `-1`, which the seam reads as `EIO`. Until 0050 every one of these read
`EIO`.

## Performance

`sigaction` is pure userspace except the SIGPIPE mask adjust. A `pthread_sigmask`
is one syscall for `SIG_SETMASK` and two for the rest, where the shadow made it one or
none. `raise` is
one syscall whose EL0-return tail runs the handler — so by the time
`raise()` returns, the handler has already run.

## Prosecution

- The bootstrap must reach a `SYS_NOTED` on EVERY arm (unknown name
  included), or the Thread wedges with `in_handler` set.
- The constructor's row: without it a program with no handler keeps a slot per dead
  child, and a first `sigaction` opens a catch-all window.
- The tty EPERM gate must cover every tty literal (all are ≥ 4 chars and
  no non-tty name collides with the `tty:` prefix) while still forwarding
  `SIGKILL` → `"kill"`.
- The 16-byte bounded name compare (the kernel pushes exactly
  `NOTE_NAME_MAX` NUL-padded bytes; an unterminated name must not run
  off).
- The bootstrap's read set must stay {`sa_handler`} unless the store is
  made atomic.

## Seams

[[seam-pouch-sigmask-per-thread]] (a `sigaction(SIGPIPE)` on one thread
updates only that thread's kernel mask; POSIX wants Proc-wide) ·
[[seam-native-exec-drops-sig-ign]] (a native exec drops an ignored row). Closed by
0069: [[seam-pouch-sigtstp-ignore]].

## Caveats

- **`83-pouch-signals.md` (absorbed) still listed "abort() extincts the
  kernel" as a live caveat, proposing as a "v1.x extension" the exact
  override that shipped** in 0011 — documented in a different reference
  doc (`86-pouch-stratumd-boot.md`), so the two never met. That override
  (`_Exit(127)`) was itself replaced at gcpark-b by the `snare:abort` death.
- **A raised fault signal ends the process at once, even with it blocked.**
  Since 0050 `raise(SIGSEGV)` (and `SIGBUS`, `SIGILL`, `SIGFPE`) dies of the
  snare; on Linux a blocked one stays pending. Pouch's mask has no bit for
  these signals and `sigaction` refuses all four, so no program can block or
  catch one first. (Until 0050 they answered `EINVAL` and ran on.)
- **`kill(pid, 0)`, the existence probe, answers `EINVAL`**: no signal 0
  mapping and no native probe arm (OPEN-BUGS, 2026-10-14).
- `sigset_t` round-trip is lossy (unsupported signums drop) and spuriously
  additive within the tty family, which shares one bit (reading back a mask
  containing SIGHUP also reports SIGQUIT, SIGTSTP, SIGCONT and SIGWINCH).
  SIGINT and SIGTERM stopped aliasing at 0049.
- `SIG_IGN` does not DISCARD pending SIGPIPE notes (POSIX §2.4); the mask
  defers them, and a later real handler sees them retroactively.
- **A handler that `longjmp`s out never reaches `SYS_NOTED`**, so `in_handler` stays
  set: no later note reaches a handler in that thread, and waits ride a caught note out
  (OPEN-BUGS). A fatal note with a default row still ends the program.
- **No handler nests.** `in_handler` holds every note for a handler's duration, so
  `sa_mask` and `SA_NODEFER` change what a handler reads and passes on, and not what can
  interrupt it. A `sigsuspend` in a handler ends at a note its mask admits, and that note
  runs at the handler's return (OPEN-BUGS).
- **What a handler does to the mask ends at its return**, `sigaction(SIGPIPE)`'s bit
  included, and Pouch's SIGPIPE default rides on that bit. A program that installs a SIGPIPE
  handler from inside another handler keeps SIGPIPE masked on that thread. One that goes back
  to `SIG_DFL` or `SIG_IGN` from inside a handler is worse: the row is default and the bit
  clear, so its next write to a closed pipe ends it instead of answering `EPIPE`
  (OPEN-BUGS, with the row-based default that would close this and
  [[seam-pouch-sigmask-per-thread]] together).
- `siglongjmp` out of a handler restores the mask and leaves `in_handler` set, as above.
- `sigaltstack` / `sigpending` / `sigqueue` are sentinels; no real-time signals. An
  `SA_SIGINFO` handler's `siginfo_t` names the signal, with `SI_KERNEL` and a zero union,
  and its `ucontext_t *` is NULL (the fd-shaped `SYS_NOTE_OPEN` path carries the arg and
  the sender).
- `pselect` and `ppoll` ignore their sigmask (OPEN-BUGS).
- `raise()` does not coalesce — two raises deliver twice.
- The note-name literals are duplicated between `kernel/notes.c` and
  `_pouch_signal.c` with no shared header.

## Provenance

[[chg-2026-05-24-p6-signals-b]] (0007 + the paired kernel `pid=0`
self-post sentinel; [[adt-signals13b-r1]] DIRTY 2 P1 + 6 P2 →
[[adt-signals13b-r2]] CLEAN) → [[chg-2026-07-18-pty3]] (the tty family) →
gcpark-b (2026-10-12: the patch prose for `abort()` and the dropped
`__abort_lock` interlock; deaths by note report their signal) → thread-model
C7 (2026-10-29, operator vote: 0065, a new thread starts with its creator's
mask; the kernel half is `SYS_THREAD_SPAWN` inheriting `note_mask`) → item 11c
(2026-11-03, votes 18 to 20: 0069, the kernel's table, restart and interruptible opens)
→ [[chg-2026-11-03-note-wait]] (vote 24: 0071, the kernel owns the mask, `sigsuspend`,
`sigtimedwait`, `sa_mask` and `SA_RESETHAND`).
