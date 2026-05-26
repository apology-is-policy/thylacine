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

// I/O error. Use when a block-device read fails (Stratum/bdev), a
// 9P transport returns Rerror, or a hardware operation reports
// failure. POSIX: EIO.
#define T_E_IO         5

// Bad handle / fd. Use when a syscall references a handle table slot
// that is empty, magic-corrupted, or holds the wrong KOBJ type for
// the operation. POSIX: EBADF.
#define T_E_BADF       9

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

// Operation timed out. Use when a tsleep / torpor_wait with a
// timeout reaches the deadline without satisfying the wait
// condition. POSIX: ETIMEDOUT.
#define T_E_TIMEDOUT   110

// ABI pins: changing a value here is an ABI break. The boundary-line
// patch (and any future POSIX-aware userspace) depends on these
// matching the AArch64 POSIX errno numbers.
_Static_assert(T_E_OK        == 0,   "T_E_OK ABI pin");
_Static_assert(T_E_PERM      == 1,   "T_E_PERM ABI pin (POSIX EPERM)");
_Static_assert(T_E_NOENT     == 2,   "T_E_NOENT ABI pin (POSIX ENOENT)");
_Static_assert(T_E_IO        == 5,   "T_E_IO ABI pin (POSIX EIO)");
_Static_assert(T_E_BADF      == 9,   "T_E_BADF ABI pin (POSIX EBADF)");
_Static_assert(T_E_AGAIN     == 11,  "T_E_AGAIN ABI pin (POSIX EAGAIN)");
_Static_assert(T_E_NOMEM     == 12,  "T_E_NOMEM ABI pin (POSIX ENOMEM)");
_Static_assert(T_E_ACCES     == 13,  "T_E_ACCES ABI pin (POSIX EACCES)");
_Static_assert(T_E_FAULT     == 14,  "T_E_FAULT ABI pin (POSIX EFAULT)");
_Static_assert(T_E_BUSY      == 16,  "T_E_BUSY ABI pin (POSIX EBUSY)");
_Static_assert(T_E_EXIST     == 17,  "T_E_EXIST ABI pin (POSIX EEXIST)");
_Static_assert(T_E_INVAL     == 22,  "T_E_INVAL ABI pin (POSIX EINVAL)");
_Static_assert(T_E_PIPE      == 32,  "T_E_PIPE ABI pin (POSIX EPIPE)");
_Static_assert(T_E_RANGE     == 34,  "T_E_RANGE ABI pin (POSIX ERANGE)");
_Static_assert(T_E_NOSYS     == 38,  "T_E_NOSYS ABI pin (POSIX ENOSYS)");
_Static_assert(T_E_TIMEDOUT  == 110, "T_E_TIMEDOUT ABI pin (POSIX ETIMEDOUT)");

#endif  // THYLACINE_ERRNO_H
