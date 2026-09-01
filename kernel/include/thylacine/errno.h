// Thylacine errno registry.
//
// Canonical numeric values for syscall-return error codes. ABI-stable
// from v1.0; appendable; never renumber.
//
// Design + rationale + rollout policy: docs/ERRORS.md.
//
// CONTRACT:
//   - Each value matches the AArch64 POSIX errno number (the value Linux
//     + musl + glibc all agree on). A kernel syscall returning
//     `-T_E_<NAME>` is observed by pouch as the corresponding POSIX
//     `errno` without translation (the boundary-line patch
//     usr/lib/pouch/patches/0001-pouch-syscall-seam.patch's
//     `__syscall_ret` passes [-4095, -2] through to userspace `errno`).
//
//   - Values are pinned by _Static_assert below. Changing any value
//     fails the build -- a deliberate ABI bump must edit the assertion
//     too, surfacing the change in code review.
//
//   - Append-only. New errors get the next-available POSIX errno number
//     they need; reserved slots (rows commented out below) stay
//     reserved -- the slot is NOT reused for a different meaning.
//
// USAGE:
//   #include <thylacine/errno.h>
//   ...
//   if (bad_arg) return -T_E_INVAL;
//   if (no_mem) return -T_E_NOMEM;
//
// Audit-bearing (CLAUDE.md §"Audit-triggering changes" row "Errno ABI
// surface"): adding / removing / renumbering any value triggers a
// focused audit round over every syscall surface that emits or
// interprets the changed value.

#ifndef THYLACINE_ERRNO_H
#define THYLACINE_ERRNO_H

#include <thylacine/types.h>

// Success. Returned only by the syscalls whose contract is "0 on
// success, -T_E_<NAME> on failure" (most of them). Some syscalls
// (e.g. read/write) return a non-negative byte count on success;
// those don't reference T_E_OK explicitly.
#define T_E_OK         0

// Operation not permitted. Use when a capability check (e.g.,
// CAP_HOSTOWNER) refuses the operation regardless of handle rights.
// POSIX: EPERM.
//
// **WARNING — DO NOT RETURN `-T_E_PERM` FROM A SYSCALL HANDLER**:
// the value 1 collides with the pouch boundary-line patch's flat-
// `-1` EIO sentinel. A handler that returns `-T_E_PERM` evaluates to
// `-1`, which `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch`
// (`__syscall_ret`) recognizes as the generic flat-error sentinel
// and maps to `errno = EIO`, NOT `EPERM`. The collision is invisible
// to the kernel-side caller -- userspace just sees the wrong errno.
//
// Until the boundary-line patch is reworked to drop the `-1 -> EIO`
// special case (which requires every existing `-1`-returning kernel
// handler to be upgraded to a specific `-T_E_<NAME>` first, otherwise
// they'd suddenly produce `errno = EPERM` instead of `EIO`), syscall
// handlers that mean "permission denied" should use `T_E_ACCES`
// (POSIX EACCES, value 13 — fits the [-4095, -2] passthrough range
// and produces the correct POSIX errno on the userspace side).
//
// The name remains in the registry because (a) the ABI value 1 = EPERM
// is fixed by POSIX, (b) kernel-side code that translates from
// userspace-side EPERM (e.g., a 9P client receiving Rerror with a
// POSIX errno) needs the symbolic constant. The constraint is on the
// return-from-syscall direction only.
//
// Audit F1 of `audit_p6_pouch_stratumd_boot_16b_gamma_mount_bind_hardening_3a_closed_list.md`.
#define T_E_PERM       1

// No such file or namespace entry. Use when a 9P walk yields no
// match, or a Spoor lookup misses, or a SYS_NOTE_OPEN target Proc
// doesn't exist. POSIX: ENOENT.
#define T_E_NOENT      2

// No such process -- a pid-addressed operation names no live Proc (or,
// for SYS_SETPGID, a pid that is neither the caller nor one of its
// children -- the POSIX setpgid ESRCH contour). Appended for the PTY-1
// session/pgrp syscalls (PTY-DESIGN.md section 4, the signed-off ABI;
// the T_E_NODEV/T_E_CANCELED append-under-ratified-scripture precedent)
// so the PTY-3 pouch boundary-line maps getpgid/getsid/setpgid errnos
// 1:1. POSIX: ESRCH.
#define T_E_SRCH       3

// A blocking wait was interrupted by a deliverable caught note before its
// condition became true (item 11 / note-interruptible waits). Distinct from
// death: the terminate latch (thread_die_pending) unwinds a wait to TERMINATE
// the Thread; T_E_INTR unwinds a wait so a CAUGHT note (a Linux-phenotype
// sigaction handler, or a native self-managing notes-fd reader) can be
// delivered at the EL0-return tail without the Thread dying. The syscall then
// returns -T_E_INTR: a native caller re-issues the wait after servicing the
// note; a Linux-phenotype caller restarts the syscall iff the handler carried
// SA_RESTART, else observes EINTR (ARCH 8.8.2, VIVARIUM 6.22). Slot 4 was the
// last free low errno (3=ESRCH, 5=EIO); the operator ratified the ABI row.
// POSIX: EINTR.
#define T_E_INTR       4

// I/O error. Use when a block-device read fails (Stratum/bdev), a
// 9P transport returns Rerror, or a hardware operation reports
// failure. POSIX: EIO.
#define T_E_IO         5

// Argument list too long -- the argv/envp a process-creation call asks to
// place on the new image's stack does not fit the bound the frame budget
// allows. Appended for the LINEAGE L-6c envp projection (#140), the commit
// that wires its first consumer, under the 2026-08-03 signoff (#142).
//
// The DISTINCTION from T_E_INVAL is the one a caller acts on: EINVAL says
// the request was malformed and retrying it verbatim is pointless, while
// E2BIG says the request was well-formed and TOO LARGE -- so a shell can
// respond by splitting the command (which is exactly what xargs exists to
// do). Collapsing them, as this path did before the registry carried the
// value, told a guest to give up on a request it could have satisfied in
// two halves.
//
// Emitted today by the environment bounds ONLY (EXEC_ENV_MAX /
// EXEC_ENV_DATA_MAX, in both the /env projection and the vivarium
// envp walk). The neighbouring ARGV bounds still answer T_E_INVAL: that
// is a LANDED native ABI (SYS_EXECVE, L-2a) whose error code is its own
// deliberate decision, reserved to the #142 rollout rather than changed
// as a side effect of adding envp. The asymmetry is tracked, not
// accidental. POSIX: E2BIG.
#define T_E_2BIG       7

// No such device -- the BACKING SERVICE/DEVICE ENDPOINT disappeared. The
// Loom device-gone terminal CQE (MENAGERIE.md section 10, the
// "T_E_DEVGONE" class): when an in-flight Loom op's 9P session dies
// because the served endpoint vanished -- a CLEAN peer-gone EOF (any
// `recv` returning 0 = the server/driver side closed) -- the op completes
// with -T_E_NODEV, distinct from a generic transport -T_E_IO (a recv
// error / malformed frame), so the consumer can tell "the thing serving
// me went away" from a transport hiccup. The trigger is BROADER than
// strict device-removal: any clean server-endpoint close qualifies; for
// the Menagerie warden path that close IS a DeviceRemoved group-
// terminating the driver (audit F4). The signal is threaded to the
// terminal CQE as the death reason. Extends I-29 (Loom completion
// integrity) to "the backing endpoint disappeared." POSIX: ENODEV.
#define T_E_NODEV      19

// Bad handle / fd. Use when a syscall references a handle table slot
// that is empty, magic-corrupted, or holds the wrong KOBJ type for
// the operation. POSIX: EBADF.
#define T_E_BADF       9

// No child processes -- a wait names no matching child. Appended for the
// LINEAGE L-6b `wait4` row (docs/LINEAGE.md section 5.5, the signed-off
// ABI; the T_E_SRCH/T_E_NODEV append-under-ratified-scripture precedent).
//
// IT IS THE TERMINATION CONDITION OF EVERY REAP LOOP, which is why a
// near-miss will not do. `while ((p = waitpid(-1, &st, WNOHANG)) > 0)`
// stops on any non-positive answer, but a shell that waits for a
// SPECIFIC child distinguishes "gone, stop asking" (ECHILD) from every
// other failure -- and a bare -1 reads to a Linux guest as EPERM, which
// is the #100 class of wrong answer (an errno that means something else
// entirely, not merely something vaguer).
//
// wait_pid_for answers -1 for TWO conditions: no matching child, and a
// #811 death-interrupted sleep. Both map here, and the collapse is
// sound rather than a compromise: the death path returns through the
// sync-from-EL0 tail, where el0_return_die_check is NORETURN on the die
// branch (vectors.S), so a group-terminating Thread never carries any
// value back to EL0. There is no observer to tell the two apart.
// POSIX: ECHILD.
#define T_E_CHILD      10

// Resource temporarily unavailable (would block; queue full). Use
// when a NoteQueue is at NOTE_QUEUE_DEPTH and the userspace
// SYS_POSTNOTE caller can't coalesce, or a non-blocking read sees an
// empty pipe. POSIX: EAGAIN.
#define T_E_AGAIN      11

// Out of memory. Use when an allocator (buddy, slub, magazines) or
// a fixed-size table (handle table, fid pool) is exhausted. POSIX:
// ENOMEM.
#define T_E_NOMEM      12

// Permission denied. Use when a handle's `rights` mask blocks the
// operation (e.g., RIGHT_READ missing on a SYS_READ), or a PTE
// permission check rejects (W^X violation, page not writable).
// Distinct from T_E_PERM: ACCES is a per-handle/per-page rights
// failure; PERM is a Proc-wide capability failure. POSIX: EACCES.
#define T_E_ACCES      13

// Bad address (uaccess fault on a user VA). Use when uaccess_load_*
// or uaccess_store_* takes a translation/permission fault on the
// supplied user-VA argument. POSIX: EFAULT.
#define T_E_FAULT      14

// Resource busy. Use when a per-Proc lock is held and the operation
// can't wait (e.g., concurrent SYS_BURROW_DETACH and SYS_BURROW_ATTACH
// would otherwise race), or a mount-busy attempt. POSIX: EBUSY.
#define T_E_BUSY       16

// Already exists. Use when SYS_MOUNT_ATTACH targets a path that
// already has a mount, or SYS_NAMESPACE_CREATE collides. POSIX:
// EEXIST.
#define T_E_EXIST      17

// Not a directory -- a path prefix component names something that is not
// a directory, so resolution cannot continue THROUGH it (#79). The
// resolver's answer for `/bin/ls/foo`: `ls` exists and was reached, but
// it cannot be searched. Distinct from T_E_NOENT ("no such component"),
// which is what stalk reported for this case before the gate existed --
// and the distinction is load-bearing, because ENOENT is what
// os.IsNotExist / errno == ENOENT branch on to mean "safe to create".
//
// Deliberately NOT a permission failure: the x bit on a non-directory is
// meaningless for traversal, so a 0755 file and a 0644 file are equally
// un-traversable and must answer identically. The gate therefore precedes
// the X-search check (see kernel/stalk.c). POSIX: ENOTDIR.
#define T_E_NOTDIR     20

// Is a directory -- the operation names a directory where a non-directory
// is required. Two producers: (a) the 9P server (Stratum answers EISDIR
// for write-opening a directory; it reached EL0 through the [-4095,-2]
// Rlerror passthrough long before this name existed -- the name was added
// with user signoff at #50, 2026-08-25, and mints no new ABI value);
// (b) SYS_OPEN_CREATE's lexical leaf rows (a create against a trailing-
// slash / "." / ".." / root leaf -- the Linux open_last_lookups rows,
// answered before any RPC). POSIX: EISDIR.
#define T_E_ISDIR      21

// Invalid argument. Use when a syscall argument is structurally
// malformed (NULL where non-NULL required; out-of-range integer;
// alignment violated; size exceeds bound). POSIX: EINVAL.
#define T_E_INVAL      22

// Broken pipe. Use when a pipe/socket write hits a closed read end.
// POSIX: EPIPE.
#define T_E_PIPE       32

// Numerical result out of range. Use when a syscall argument is
// numerically in-range per the type but exceeds an implementation
// limit (e.g., handle count > HANDLE_TABLE_MAX). POSIX: ERANGE.
#define T_E_RANGE      34

// Function not implemented. Use when a syscall slot exists in the
// dispatch table but its handler is a placeholder (returns -ENOSYS),
// or a code path is reachable but stubbed at v1.0. POSIX: ENOSYS.
#define T_E_NOSYS      38

// Too many levels of symbolic links. Two emitters, both in the resolver
// (DISTRO D-1, docs/DISTRO.md section 4): a resolution that exceeds
// STALK_MAX_FOLLOWS (40, Linux parity) symlink expansions -- a cycle, or
// a chain past the bound -- and an open carrying the no-follow flag whose
// FINAL component is a symlink (the Linux O_NOFOLLOW contour: the caller
// asked for not-a-symlink and got one; ELOOP is the errno Linux fixed for
// that answer, so a guest's symlink-attack defenses read correctly).
//
// Registered under the 2026-08-05 DISTRO design signoff + the per-value
// D-1 approval (the append-under-ratified-scripture precedent: T_E_SRCH /
// T_E_NODEV / T_E_CHILD / the V-5 socket family). POSIX: ELOOP.
#define T_E_LOOP       40

// Operation not supported. Appended under the PTY-1 ABI signoff (PTY-1d, the
// T_E_SRCH precedent -- it originally gated SYS_TTY_SIGNAL's TTY_SIG_TSTP
// class until PTY-1f made the job-stop disposition live). RESERVED /
// append-only now: a legitimate POSIX EOPNOTSUPP, kept for future
// not-yet-supported syscall dispositions. POSIX: EOPNOTSUPP.
#define T_E_OPNOTSUPP  95

// Operation timed out. Use when a tsleep / torpor_wait with a
// timeout reaches the deadline without satisfying the wait
// condition. POSIX: ETIMEDOUT.
#define T_E_TIMEDOUT   110

// Operation cancelled. The Loom LINK cancel-cascade posts this as a
// completion result for a chain op whose linked predecessor failed:
// the op never ran, but it is not silently dropped (it posts exactly
// one terminal CQE, the io_uring -ECANCELED convention). POSIX:
// ECANCELED.
#define T_E_CANCELED   125

// ---------------------------------------------------------------------------
// The socket family (VIVARIUM V-5, docs/VIVARIUM.md section 5.5).
//
// Appended under the precedent this file already records twice -- T_E_NODEV
// (Menagerie build-arc 4) and T_E_CANCELED (Loom-5): a sub-chunk appends a
// POSIX-FIXED errno when it is executing already-ratified scripture. The
// socket family was user-voted 2026-07-31, and section 5.5 has carried
// "AF_INET6 -> EAFNOSUPPORT at v1.0, honestly" since V-0's signoff, so the
// name was ratified before the number was needed.
//
// The VALUES are not a choice: every one is read from
// third_party/musl/arch/generic/bits/errno.h, which is what a Linux guest's
// libc will compare against. They are pinned below like every other entry.
//
// Why the phenotype needs its own errnos at all: a Linux program distinguishes
// these, and collapsing them (to EINVAL, say) would make a guest retry a
// connection that can never succeed, or treat a full table as a bad argument.
// The whole point of the boundary-line is that the guest's own error handling
// keeps working.
// ---------------------------------------------------------------------------

// Too many open files -- the per-Proc socket table (VIV_SOCK_MAX) is full.
// POSIX: EMFILE.
#define T_E_MFILE      24

// The fd is not a terminal for the requested control op. Returned by the
// phenotype ioctl shell (C2-k1b) for a TC*/TIOC* request on a non-tty fd, and
// for any request the terminal surface does not serve. isatty() reads this (via
// a failed TIOCGWINSZ) as "not a tty". POSIX: ENOTTY.
#define T_E_NOTTY      25

// The fd is not a socket. Returned when a socket call names an fd with no
// socktab entry -- a plain file, or a socket whose entry was dropped.
// POSIX: ENOTSOCK.
#define T_E_NOTSOCK    88

// Protocol not supported: a SOCK_* type or IPPROTO_* the /net tree has no
// directory for (SOCK_SEQPACKET, SOCK_RAW, a mismatched protocol number).
// POSIX: EPROTONOSUPPORT.
#define T_E_PROTONOSUPPORT 93

// Address family not supported -- AF_INET6 and everything that is not
// AF_INET. POSIX: EAFNOSUPPORT.
#define T_E_AFNOSUPPORT 97

// Address already in use -- netd refused an `announce` for this port. The
// V-5b listen() reports it, not bind(): bind is remembered rather than
// written, so the collision surfaces where the announce actually happens.
// POSIX: EADDRINUSE.
#define T_E_ADDRINUSE  98

// The connection was aborted before it could be handed back -- an accept()
// whose listen fid produced a connection number the data open then refused.
// POSIX: ECONNABORTED.
#define T_E_CONNABORTED 103

// The socket is already connected: a second connect() on a CONNECTED entry.
// POSIX: EISCONN.
#define T_E_ISCONN     106

// The socket is not connected -- a getpeername()/shutdown() on an entry still
// in FRESH. POSIX: ENOTCONN.
#define T_E_NOTCONN    107

// Connection refused. netd rejected the dial, or the data open failed after
// the connect verb was accepted. POSIX: ECONNREFUSED.
#define T_E_CONNREFUSED 111

// ABI pins: changing a value here is an ABI break. The boundary-line
// patch (and any future POSIX-aware userspace) depends on these
// matching the AArch64 POSIX errno numbers.
_Static_assert(T_E_OK        == 0,   "T_E_OK ABI pin");
_Static_assert(T_E_PERM      == 1,   "T_E_PERM ABI pin (POSIX EPERM)");
_Static_assert(T_E_NOENT     == 2,   "T_E_NOENT ABI pin (POSIX ENOENT)");
_Static_assert(T_E_SRCH      == 3,   "T_E_SRCH ABI pin (POSIX ESRCH)");
_Static_assert(T_E_INTR      == 4,   "T_E_INTR ABI pin (POSIX EINTR)");
_Static_assert(T_E_OPNOTSUPP == 95,  "T_E_OPNOTSUPP ABI pin (POSIX EOPNOTSUPP)");
// The V-5 socket family. Values read from musl's generic bits/errno.h, which
// is what a Linux guest's libc compares against.
_Static_assert(T_E_MFILE          == 24,  "T_E_MFILE ABI pin (POSIX EMFILE)");
_Static_assert(T_E_NOTTY          == 25,  "T_E_NOTTY ABI pin (POSIX ENOTTY)");
_Static_assert(T_E_NOTSOCK        == 88,  "T_E_NOTSOCK ABI pin (POSIX ENOTSOCK)");
_Static_assert(T_E_PROTONOSUPPORT == 93,  "T_E_PROTONOSUPPORT ABI pin (POSIX EPROTONOSUPPORT)");
_Static_assert(T_E_AFNOSUPPORT    == 97,  "T_E_AFNOSUPPORT ABI pin (POSIX EAFNOSUPPORT)");
_Static_assert(T_E_ADDRINUSE      == 98,  "T_E_ADDRINUSE ABI pin (POSIX EADDRINUSE)");
_Static_assert(T_E_CONNABORTED    == 103, "T_E_CONNABORTED ABI pin (POSIX ECONNABORTED)");
_Static_assert(T_E_ISCONN         == 106, "T_E_ISCONN ABI pin (POSIX EISCONN)");
_Static_assert(T_E_NOTCONN        == 107, "T_E_NOTCONN ABI pin (POSIX ENOTCONN)");
_Static_assert(T_E_CONNREFUSED    == 111, "T_E_CONNREFUSED ABI pin (POSIX ECONNREFUSED)");
_Static_assert(T_E_IO        == 5,   "T_E_IO ABI pin (POSIX EIO)");
_Static_assert(T_E_2BIG      == 7,   "T_E_2BIG ABI pin (POSIX E2BIG)");
_Static_assert(T_E_NODEV     == 19,  "T_E_NODEV ABI pin (POSIX ENODEV)");
_Static_assert(T_E_BADF      == 9,   "T_E_BADF ABI pin (POSIX EBADF)");
_Static_assert(T_E_CHILD     == 10,  "T_E_CHILD ABI pin (POSIX ECHILD)");
_Static_assert(T_E_AGAIN     == 11,  "T_E_AGAIN ABI pin (POSIX EAGAIN)");
_Static_assert(T_E_NOMEM     == 12,  "T_E_NOMEM ABI pin (POSIX ENOMEM)");
_Static_assert(T_E_ACCES     == 13,  "T_E_ACCES ABI pin (POSIX EACCES)");
_Static_assert(T_E_FAULT     == 14,  "T_E_FAULT ABI pin (POSIX EFAULT)");
_Static_assert(T_E_BUSY      == 16,  "T_E_BUSY ABI pin (POSIX EBUSY)");
_Static_assert(T_E_EXIST     == 17,  "T_E_EXIST ABI pin (POSIX EEXIST)");
_Static_assert(T_E_NOTDIR    == 20,  "T_E_NOTDIR ABI pin (POSIX ENOTDIR)");
_Static_assert(T_E_ISDIR     == 21,  "T_E_ISDIR ABI pin (POSIX EISDIR)");
_Static_assert(T_E_INVAL     == 22,  "T_E_INVAL ABI pin (POSIX EINVAL)");
_Static_assert(T_E_PIPE      == 32,  "T_E_PIPE ABI pin (POSIX EPIPE)");
_Static_assert(T_E_RANGE     == 34,  "T_E_RANGE ABI pin (POSIX ERANGE)");
_Static_assert(T_E_NOSYS     == 38,  "T_E_NOSYS ABI pin (POSIX ENOSYS)");
_Static_assert(T_E_LOOP      == 40,  "T_E_LOOP ABI pin (POSIX ELOOP)");
_Static_assert(T_E_TIMEDOUT  == 110, "T_E_TIMEDOUT ABI pin (POSIX ETIMEDOUT)");
_Static_assert(T_E_CANCELED  == 125, "T_E_CANCELED ABI pin (POSIX ECANCELED)");

#endif  // THYLACINE_ERRNO_H
