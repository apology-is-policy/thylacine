// libthyla-rs::err — Thylacine error type for native userspace.
//
// The foundational error type every libthyla-rs module reports through
// (and every native Thylacine Rust program propagates via `?`). Maps
// the kernel's T_E_* registry — pinned by `_Static_assert` in
// kernel/include/thylacine/errno.h, designed in docs/ERRORS.md — to a
// strongly-typed Rust enum, plus Thylacine-specific variants the
// registry doesn't yet cover.
//
// Foundation chunk: U-2a per docs/UTOPIA-SHELL-DESIGN.md §15. Every
// subsequent libthyla-rs module (t::handle, t::fs, t::process,
// t::notes, ...) reports through this type.
//
// CONVENTIONS:
//   - Each enumerated variant has a POSIX-aligned errno value; values
//     match kernel/include/thylacine/errno.h's `_Static_assert` pins.
//     The mapping is the single source of truth for libthyla-rs → kernel
//     errno round-tripping.
//   - `Other(i32)` carries any errno value not enumerated above so
//     unknown kernel errors stay observable instead of being silently
//     dropped.
//   - `From<i32>` maps a positive errno integer to a variant.
//   - `Error::from_syscall_return(rc: i64) -> Result<i64>` is the
//     canonical syscall-return decoder reused by every typed wrapper
//     module: rc ≥ 0 → Ok(rc); rc < 0 → Err(Error::from(-rc as i32)).
//
// no_std + no_alloc: variants are unit or carry a single i32; the
// Display impl uses static strings; no String, no Box, no heap.
//
// TRAPS:
//   - T_E_PERM (= 1) collides with the pouch boundary-line's flat
//     -1/EIO sentinel on the pouch side (see kernel errno.h header
//     comment + docs/ERRORS.md). Native libthyla-rs callers bypass
//     pouch, so the `NotPermitted` variant is safe to mint and observe
//     directly. The trap only matters when a kernel handler returns
//     `-T_E_PERM` to a pouch-via-musl program.

use core::fmt;

/// A `Result` with `libthyla_rs::err::Error` as the error type.
///
/// Every fallible operation in libthyla-rs returns this alias. New
/// userspace Rust code is encouraged to `use libthyla_rs::err::Result;`
/// at the top of each module and use bare `Result<T>`.
pub type Result<T> = core::result::Result<T, Error>;

/// Errors reported by Thylacine syscalls and libthyla-rs operations.
///
/// Marked `#[non_exhaustive]` so the registry can grow without breaking
/// downstream `match` arms. Pattern-match with a `_ =>` arm to stay
/// forward-compatible.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
#[non_exhaustive]
pub enum Error {
    /// Operation not permitted — capability missing
    /// (e.g., `CAP_HW_CREATE` for `SYS_MMIO_CREATE`).
    /// Maps `T_E_PERM` = 1 (POSIX `EPERM`).
    NotPermitted,

    /// No such file, directory, or namespace entry.
    /// Maps `T_E_NOENT` = 2 (POSIX `ENOENT`).
    NotFound,

    /// I/O error from a transport, block device, or 9P session.
    /// Maps `T_E_IO` = 5 (POSIX `EIO`).
    Io,

    /// Bad handle: handle table slot empty, magic-corrupted, or holds
    /// the wrong `KObj` kind for the requested operation.
    /// Maps `T_E_BADF` = 9 (POSIX `EBADF`).
    BadHandle,

    /// Resource temporarily unavailable (would-block on a non-blocking
    /// read, NoteQueue at depth without coalesce, etc.).
    /// Maps `T_E_AGAIN` = 11 (POSIX `EAGAIN`).
    WouldBlock,

    /// Out of memory: allocator or fixed-size table exhausted.
    /// Maps `T_E_NOMEM` = 12 (POSIX `ENOMEM`).
    NoMemory,

    /// Permission denied at the handle-rights or page-permission level
    /// (the rights mask blocks the op, or a PTE permission check fails).
    /// Distinct from `NotPermitted`: this is per-handle/per-page; that
    /// is Proc-wide capability.
    /// Maps `T_E_ACCES` = 13 (POSIX `EACCES`).
    PermissionDenied,

    /// Bad address: a user VA argument took a translation/permission
    /// fault under the kernel's `uaccess` checks.
    /// Maps `T_E_FAULT` = 14 (POSIX `EFAULT`).
    BadAddress,

    /// Resource busy: per-Proc lock held and the op can't wait, or a
    /// mount-busy rejection.
    /// Maps `T_E_BUSY` = 16 (POSIX `EBUSY`).
    Busy,

    /// Already exists: target path holds a mount or namespace entry.
    /// Maps `T_E_EXIST` = 17 (POSIX `EEXIST`).
    Exists,

    /// Invalid argument: structurally malformed (NULL where non-NULL
    /// required, out-of-range integer, alignment violated, size
    /// exceeds bound).
    /// Maps `T_E_INVAL` = 22 (POSIX `EINVAL`).
    InvalidArgument,

    /// Broken pipe: pipe or socket write hit a closed read end.
    /// Maps `T_E_PIPE` = 32 (POSIX `EPIPE`).
    BrokenPipe,

    /// Numerical result out of range (in-range per type but exceeds
    /// an implementation limit, e.g., handle count over the bound).
    /// Maps `T_E_RANGE` = 34 (POSIX `ERANGE`).
    OutOfRange,

    /// Function not implemented: syscall slot is a placeholder or
    /// the code path is stubbed at v1.0.
    /// Maps `T_E_NOSYS` = 38 (POSIX `ENOSYS`).
    NotImplemented,

    /// Operation timed out: a `tsleep` or `torpor_wait` deadline
    /// elapsed without satisfying the wait condition.
    /// Maps `T_E_TIMEDOUT` = 110 (POSIX `ETIMEDOUT`).
    TimedOut,

    /// Pass-through for errno values not enumerated above. The carried
    /// integer is the positive errno reported by the kernel (i.e.,
    /// `-rc` where `rc` was the syscall return). Lets unknown errors
    /// propagate instead of being dropped to a synthetic variant.
    Other(i32),

    // ---- library-only variants (no POSIX errno mapping) ----

    /// `read` returned `Ok(0)` (EOF) before `read_exact` could fill
    /// its buffer. Library-only; not produced by any kernel syscall.
    /// `as_errno()` returns `0`. Added at U-2c-io.
    UnexpectedEof,

    /// `write` returned `Ok(0)` before `write_all` could drain its
    /// buffer (the underlying writer stopped making progress while
    /// claiming success). Library-only; `as_errno()` returns `0`.
    /// Added at U-2c-io.
    WriteZero,
}

impl Error {
    /// Returns the POSIX-aligned errno integer for this error.
    ///
    /// Inverse of `From<i32>`. Useful when crossing a foreign interface
    /// (Linux syscall shim, JSON payload, etc.) that wants the raw
    /// errno number.
    #[inline]
    #[must_use]
    pub const fn as_errno(self) -> i32 {
        match self {
            Error::NotPermitted     => 1,
            Error::NotFound         => 2,
            Error::Io               => 5,
            Error::BadHandle        => 9,
            Error::WouldBlock       => 11,
            Error::NoMemory         => 12,
            Error::PermissionDenied => 13,
            Error::BadAddress       => 14,
            Error::Busy             => 16,
            Error::Exists           => 17,
            Error::InvalidArgument  => 22,
            Error::BrokenPipe       => 32,
            Error::OutOfRange       => 34,
            Error::NotImplemented   => 38,
            Error::TimedOut         => 110,
            Error::Other(e)         => e,
            // Library-only variants have no POSIX errno mapping.
            Error::UnexpectedEof    => 0,
            Error::WriteZero        => 0,
        }
    }

    /// Decode a raw syscall return value into a `Result<i64>`.
    ///
    /// The Thylacine syscall ABI returns non-negative values on
    /// success and `-T_E_<NAME>` (a small negative integer in the
    /// `[-4095, -1]` range) on failure. This helper does the dispatch
    /// in one place; every typed wrapper module in libthyla-rs reuses
    /// it so the convention is enforced from one call site, not
    /// scattered.
    ///
    /// Example (sketch):
    ///
    /// ```ignore
    /// use libthyla_rs::err::{Error, Result};
    ///
    /// fn read(fd: i32, buf: &mut [u8]) -> Result<usize> {
    ///     let rc = unsafe { t_read(fd as i64, buf.as_mut_ptr(), buf.len()) };
    ///     let n = Error::from_syscall_return(rc)?;
    ///     Ok(n as usize)
    /// }
    /// ```
    #[inline]
    pub fn from_syscall_return(rc: i64) -> Result<i64> {
        if rc >= 0 {
            Ok(rc)
        } else {
            // POSIX errnos fit in i32. A kernel handler returning a
            // wildly out-of-range negative value (well below -i32::MAX)
            // is a bug; saturate to i32::MAX rather than wrap-cast so
            // the bug surfaces as a distinct (and visible) `Other(MAX)`
            // instead of silently aliasing onto a real variant.
            let errno = if rc < -(i32::MAX as i64) {
                i32::MAX
            } else {
                // rc is in [i32::MIN..0); -rc fits in i32 by the bound.
                (-rc) as i32
            };
            Err(Error::from(errno))
        }
    }
}

impl From<i32> for Error {
    #[inline]
    fn from(errno: i32) -> Self {
        match errno {
            1   => Error::NotPermitted,
            2   => Error::NotFound,
            5   => Error::Io,
            9   => Error::BadHandle,
            11  => Error::WouldBlock,
            12  => Error::NoMemory,
            13  => Error::PermissionDenied,
            14  => Error::BadAddress,
            16  => Error::Busy,
            17  => Error::Exists,
            22  => Error::InvalidArgument,
            32  => Error::BrokenPipe,
            34  => Error::OutOfRange,
            38  => Error::NotImplemented,
            110 => Error::TimedOut,
            n   => Error::Other(n),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let msg: &'static str = match self {
            Error::NotPermitted     => "operation not permitted",
            Error::NotFound         => "no such file or namespace entry",
            Error::Io               => "I/O error",
            Error::BadHandle        => "bad handle",
            Error::WouldBlock       => "resource temporarily unavailable",
            Error::NoMemory         => "out of memory",
            Error::PermissionDenied => "permission denied",
            Error::BadAddress       => "bad address",
            Error::Busy             => "resource busy",
            Error::Exists           => "already exists",
            Error::InvalidArgument  => "invalid argument",
            Error::BrokenPipe       => "broken pipe",
            Error::OutOfRange       => "result out of range",
            Error::NotImplemented   => "function not implemented",
            Error::TimedOut         => "operation timed out",
            Error::Other(_)         => "kernel error",
            Error::UnexpectedEof    => "unexpected end of file",
            Error::WriteZero        => "writer made no progress",
        };
        f.write_str(msg)?;
        if let Error::Other(n) = self {
            // Surface the raw errno so the caller can still see what
            // the kernel produced; without this the catch-all variant
            // would erase the distinguishing information.
            write!(f, " (errno {})", n)?;
        }
        Ok(())
    }
}
