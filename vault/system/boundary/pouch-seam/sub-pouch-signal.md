---
id: sub-pouch-signal
type: sub
parent: moc-pouch-seam
title: "POSIX signals over kernel notes"
code:
  - usr/lib/pouch/patches/0007-pouch-signals.patch
audit: hard
guarded-by: [inv-i24]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md", "docs/PTY-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

`sigaction` / `raise` / `kill` / `sigprocmask` on a kernel that has
string-named Plan 9 notes instead of numbered signals. pouch keeps the
per-Proc disposition table itself and registers ONE bootstrap handler
with the kernel; the kernel's job is delivery, pouch's is dispatch.

## Contract

- `sigaction(sig, sa, old)` — records into `__pouch_sigtab[sig]`;
  accepts exactly the supported set, `EINVAL` for everything else
  (which subsumes SIGKILL/SIGSTOP, POSIX-correctly).
- `raise(sig)` → `SYS_POSTNOTE(pid=0, name, len)` — the kernel's
  self-post sentinel. `kill(pid, sig)` → the same with the caller's pid.
- `pthread_sigmask` / `sigprocmask` → `SYS_NOTE_MASK`, marshalling
  `sigset_t` ↔ `NOTE_BIT_*`.
- The bootstrap `__pouch_note_handler(name, arg)` never returns
  normally: it ends in `SYS_NOTED(NCONT)` or `SYS_NOTED(NDFLT)`.

The supported set: `SIGINT`/`SIGTERM` → `"interrupt"` (shared),
`SIGPIPE` → `"pipe"`, `SIGCHLD` → `"child_exit"`, `SIGKILL` → `"kill"`
(non-catchable), plus the PTY-3 tty family — `SIGQUIT` → `"tty:quit"`,
`SIGTSTP` → `"tty:susp"`, `SIGCONT` → `"tty:cont"`, `SIGWINCH` →
`"tty:winch"`, `SIGHUP` → `"tty:hup"`.

## Mechanism

**One handler, dispatched by name.** The `.init_array` constructor runs
before `main`, sets `NOTE_BIT_PIPE` in the kernel mask (SIGPIPE
masked-by-default — the modern-daemon posture), then registers
`__pouch_note_handler` via `SYS_NOTIFY`. The kernel calls it Plan-9-style
(`x0` = a pointer to the 16-byte NUL-padded note name on the user stack,
`x1` = the arg), pouch maps the name to a signum with a bounded 16-byte
compare, looks up the disposition, and ends with the right `SYS_NOTED`.
The constructor's ORDER is itself the invariant: mask before notify, so
no note can reach a registered handler over an unset sigtab.

**SIGINT and SIGTERM share one note**, so the dispatcher arbitrates by
handler-presence: prefer SIGINT's if installed, else SIGTERM's, else
SIGINT. A program that installs both gets last-registered-wins semantics
it did not ask for.

**The tty family is RECEIVE-ONLY.** The kernel's POST axis rejects any
userspace `tty:*` post — the I-39 gate that makes a terminal event
unforgeable, since only a pts's minting server may originate one. So
`kill()`/`raise()` of those five signums answer `EPERM` at the pouch
layer (the POSIX-shaped errno; the kernel would refuse with a bare `-1`).
All five also share ONE kernel mask bit (`NOTE_BIT_TTY`), so blocking any
one blocks the family — documented coarseness, made survivable by the
kernel's terminate-class latch (a masked `tty:quit`/`tty:hup` fires on
unmask).

**The SIG_DFL matrix is where the honest seam is.** quit/hup → `NDFLT`
(whole-Proc terminate, the POSIX default); winch/cont → `NCONT` (POSIX
ignore; a cont's RESUME already happened kernel-side at
`SYS_TTY_CONT`); chld → `NCONT`. **susp → `NCONT` (ignore) — not STOP.**
The reason is a composition, not laziness: the kernel's pre-delivery stop
gate treats a Proc with a registered notify handler as "caught", and
pouch's constructor ALWAYS registers one, so every pouch Proc is caught
and the note delivers to the bootstrap instead of stopping the Proc; the
bootstrap then cannot re-enter the default either, because the kernel's
`NDFLT` arm TERMINATES (`exits` → `proc_group_terminate`) and never
stops. `NDFLT` here would turn `^Z` into process death — the one actively
wrong option. The clean fix is a kernel `NDFLT`-stop arm for `tty:susp`,
an ABI-semantics change on the audited notes surface that needs signoff.

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

`__pouch_sigtab[_NSIG]` — per-Proc dispositions (POSIX semantics).
`__pouch_note_mask_shadow` — a `__thread` shadow of the kernel's
per-Thread note mask, needed for `SIG_BLOCK`/`SIG_UNBLOCK`
read-modify-write; pouch is its sole writer.

## Concurrency

The sigtab is written by `sigaction` and read by the bootstrap on
whatever thread takes the note. The struct copy is multi-word, but the
bootstrap reads ONLY `sa_handler` (offset 0, naturally aligned, atomic on
aarch64) — so a torn read is impossible *today*, and any future
enhancement that reads `sa_mask` or `sa_flags` in the bootstrap must
first make the store atomic. The mask shadow is TLS, hence race-free by
construction — and per-Thread, which is where its POSIX divergence comes
from.

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
`kill`/`raise` of the tty family. `EIO` for a genuine `SYS_POSTNOTE`
failure (the flat-`-1` collapse — so `ESRCH` and `EPERM` from the kernel's
own post gate are indistinguishable to a caller).

## Performance

`sigaction` is pure userspace except the SIGPIPE mask adjust. `raise` is
one syscall whose EL0-return tail runs the handler — so by the time
`raise()` returns, the handler has already run.

## Prosecution

- The bootstrap must reach a `SYS_NOTED` on EVERY arm (unknown name
  included), or the Thread wedges with `in_handler` set.
- The constructor's mask-then-notify order.
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
[[seam-pouch-sigtstp-ignore]] (the SIG_DFL `^Z` seam above).

## Caveats

- **`83-pouch-signals.md` (absorbed) still listed "abort() extincts the
  kernel" as a live caveat, proposing as a "v1.x extension" the exact
  override that shipped** in 0011 — documented in a different reference
  doc (`86-pouch-stratumd-boot.md`), so the two never met.
- SIGINT/SIGTERM aliasing; `sigset_t` round-trip is lossy (unsupported
  signums drop) and spuriously additive (reading back a mask containing
  SIGINT also reports SIGTERM).
- No mask inheritance across `pthread_create` (the child starts at 0).
- `SIG_IGN` does not DISCARD pending SIGPIPE notes (POSIX §2.4); the mask
  defers them, and a later real handler sees them retroactively.
- `siglongjmp` does not restore masks; `sigaltstack` / `sigpending` /
  `sigtimedwait` / `sigqueue` / `sigsuspend` / `pthread_kill` are all
  sentinels; no real-time signals, no `siginfo_t` (the fd-shaped
  `SYS_NOTE_OPEN` path carries the analog).
- `raise()` does not coalesce — two raises deliver twice.
- The note-name literals are duplicated between `kernel/notes.c` and
  `_pouch_signal.c` with no shared header.

## Provenance

[[chg-2026-05-24-p6-signals-b]] (0007 + the paired kernel `pid=0`
self-post sentinel; [[adt-signals13b-r1]] DIRTY 2 P1 + 6 P2 →
[[adt-signals13b-r2]] CLEAN) → [[chg-2026-07-18-pty3]] (the tty family).
