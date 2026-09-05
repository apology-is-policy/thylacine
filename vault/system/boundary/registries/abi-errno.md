---
id: abi-errno
type: abi
kind: registry
stability: append-only
title: "The errno registry — T_E_* and the POSIX-value contract"
pinned-by:
  - "_Static_assert per value (kernel/include/thylacine/errno.h, 20 asserts)"
mirrors:
  - "usr/lib/libthyla-rs/src/err.rs: enum Error + From<i32> + as_errno + Display"
  - "usr/lib/pouch/patches/0001-pouch-syscall-seam.patch: __syscall_ret (range contract, not a value list)"
created: 2026-08-02
updated: 2026-08-02
---
## The surface

Every syscall that fails returns `-T_E_<NAME>`. The registry's whole design
is one sentence: **each value is the AArch64 POSIX errno number**, so a
kernel return crosses into pouch's `errno` with no translation layer
anywhere.

| value | name | POSIX | meaning as used in this tree |
|---|---|---|---|
| 0 | `T_E_OK` | — | success (byte-count returners never name it) |
| 1 | `T_E_PERM` | EPERM | **never returned by a handler** — see the collision below |
| 2 | `T_E_NOENT` | ENOENT | walk miss, Spoor lookup miss, no such target Proc |
| 3 | `T_E_SRCH` | ESRCH | no such process — the `setpgid`/`getpgid`/`getsid` contour |
| 5 | `T_E_IO` | EIO | block-device failure, 9P `Rerror`, transport break |
| 9 | `T_E_BADF` | EBADF | empty slot, bad magic, wrong `KObj` kind for the op |
| 11 | `T_E_AGAIN` | EAGAIN | would block; note queue at depth without coalesce |
| 12 | `T_E_NOMEM` | ENOMEM | allocator or fixed-size table exhausted |
| 13 | `T_E_ACCES` | EACCES | **the "denied" code handlers actually use** |
| 14 | `T_E_FAULT` | EFAULT | a `uaccess` touch faulted on the supplied user VA |
| 16 | `T_E_BUSY` | EBUSY | per-Proc lock held and the op cannot wait; mount busy |
| 17 | `T_E_EXIST` | EEXIST | mount point taken; create collision |
| 19 | `T_E_NODEV` | ENODEV | the backing endpoint went away (Loom device-gone) |
| 22 | `T_E_INVAL` | EINVAL | structurally malformed argument |
| 32 | `T_E_PIPE` | EPIPE | write to a closed read end |
| 34 | `T_E_RANGE` | ERANGE | in-range for the type, past an implementation limit |
| 38 | `T_E_NOSYS` | ENOSYS | dispatch slot exists, handler is a placeholder |
| 95 | `T_E_OPNOTSUPP` | EOPNOTSUPP | reserved; appended at PTY-1d, no current emitter |
| 110 | `T_E_TIMEDOUT` | ETIMEDOUT | a `tsleep`/`torpor_wait` deadline elapsed |
| 125 | `T_E_CANCELED` | ECANCELED | a Loom chain op cancelled by a failed predecessor |

Two distinctions the names do not carry on their face. `T_E_ACCES` is a
**per-handle or per-page** rights failure; `T_E_PERM` is a **Proc-wide
capability** failure — except that the latter is unreturnable, so in
practice capability refusals also come back as `ACCES`. And `T_E_NODEV` is
broader than its name: any clean server-side close qualifies, not only an
actual device removal.

## The `-1` collision — why `T_E_PERM` is defined but forbidden

`T_E_PERM` is 1, so `-T_E_PERM` is `-1` — and `-1` is the tree's *generic*
failure sentinel, the value every un-upgraded handler still returns. The
pouch boundary-line resolves the ambiguity in favour of the sentinel:
`__syscall_ret` tests `r == -1` **before** the range test and maps it to
`EIO`. A handler that returned `-T_E_PERM` meaning "permission denied"
would therefore surface in userspace as `EIO`, silently.

So the registry holds a value that no handler may produce. It stays for two
reasons: the number is fixed by POSIX and cannot be reassigned, and
kernel-side code that *translates inbound* POSIX errnos (a 9P client reading
an `Rlerror`) needs the symbol. **The constraint is on the
return-from-syscall direction only.**

The native mirror reaches the same conclusion independently:
`Error::from_syscall_return` maps `-1` to `Io`, not `NotPermitted`, and says
why — aliasing `-1` onto the loaded `NotPermitted` variant had mislabelled
every flat-error failure (a missing file, a denied write, a bad fd) as a
permission problem.

## Change protocol

**Append-only, and never renumber.** A new error takes the POSIX number it
needs; a retired one keeps its slot reserved rather than being reused for a
different meaning. Adding, removing or renumbering a value is an
audit-bearing change over every syscall surface that emits or interprets it.

The three ranges a return value can fall in, as the seam reads them:

- `>= 0` — success (often a byte count).
- `== -1` — the generic sentinel → `EIO`. Tested first, because `-1` also
  satisfies the range test below.
- `[-4095, -2]` — an explicit errno, passed through untranslated.

## Where the registry is mirrored, and where it has drifted

**`usr/lib/libthyla-rs/src/err.rs`** is the only value-by-value mirror. It
enumerates **15** of the 19 non-zero values. Missing: `T_E_SRCH` (3),
`T_E_NODEV` (19), `T_E_OPNOTSUPP` (95), `T_E_CANCELED` (125) — which is
exactly the set appended *after* the mirror was written (SRCH and OPNOTSUPP
at PTY-1, NODEV at the Menagerie arc, CANCELED at Loom-5).

This does not lose information: `Other(i32)` carries any unenumerated errno
through, deliberately, so unknown kernel errors stay observable. The cost is
that a native program cannot name them. `setpgid` on a stranger's pid and a
cancelled Loom chain op are both live emit sites, and both surface as
`Error::Other(3)` / `Error::Other(125)`, displaying as `kernel error
(errno N)` and matchable only against a magic number.

**Nothing catches the lag.** The kernel's discipline is a `_Static_assert`
per value; the Rust side has no equivalent, and no test compares the two
lists. Each append owed a mirror update by the audit rule above; four went
without one, and the build stayed green every time. Tracked as task #34.

**`usr/lib/libt`** deliberately holds no mirror — the C wrappers return the
raw negative and let the caller interpret it.

**Stratum** does not mirror this registry; it has its own `STM_E*` space and
the 9P layer translates.

## Prosecution

- A handler returning `-T_E_PERM` produces `EIO` in userspace. Use
  `-T_E_ACCES`.
- A handler returning a value below `-4095` falls outside the passthrough
  range; the native side saturates it to `Other(i32::MAX)` rather than
  wrap-casting, so the bug surfaces as a visible outlier instead of aliasing
  onto a real variant. The pouch side has no such guard.
- Renumbering any value breaks the no-translation property silently — the
  build passes, and userspace gets a different error than the kernel meant.
- A new value with no `err.rs` variant is *safe* but *unnameable*. If a
  native consumer must branch on it, the variant is owed.

## Referenced by

[[sub-pouch-seam]] · [[sub-pouch-fs]] · [[sub-kernel-ninep-client]] ·
[[abi-caps]] · [[moc-boundary]].
