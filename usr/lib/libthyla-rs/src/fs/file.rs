// libthyla-rs::fs::file — File RAII over the Thylacine kernel's
// Spoor-handle surface.
//
// `File` owns a `Handle` to an open Spoor (the Thylacine analogue of
// a Unix file descriptor). `Drop` closes the handle. Reads, writes,
// and seeks route through the kernel's SYS_READ / SYS_WRITE /
// SYS_LSEEK syscalls.
//
// Foundation chunk: U-2c-io per docs/UTOPIA-SHELL-DESIGN.md §15.
//
// MULTI-COMPONENT WALK:
//   The kernel's SYS_WALK_OPEN walks one path component at a time.
//   `File::open("/etc/hosts")` walks two components:
//
//     SYS_WALK_OPEN(FROM_ROOT, "etc",   OREAD)  -> etc_fd
//     SYS_WALK_OPEN(etc_fd,    "hosts", OREAD)  -> hosts_fd
//     close(etc_fd)
//     return hosts_fd
//
//   Intermediate components are walked with OREAD (since they are
//   directories we traverse); the final component takes the requested
//   omode. Intermediate fds are closed before the final fd is
//   returned to the caller -- no leakage.
//
// PATH SUPPORT (v1 scope):
//   - Absolute paths only. Relative paths return Error::InvalidArgument.
//     The current-directory concept isn't part of v1; callers compose
//     absolute paths.
//   - `.` components are silently skipped.
//   - `..` components return Error::InvalidArgument (no parent
//     traversal in v1; safer to be explicit until we audit the
//     namespace-escape implications).
//   - Empty path -> Error::InvalidArgument.
//   - Path "/" alone -> Error::InvalidArgument (no opener for the
//     pivoted root via this API; use FROM_ROOT directly if you need
//     a Spoor for the root).
//
// CREATE SEMANTICS (v1 scope):
//   - `File::create` opens the existing file in OWRITE | OTRUNC mode.
//     It does NOT create a new file -- SYS_WALK_OPEN cannot create.
//     A future `File::create_new` / `OpenOptions::create(true)` will
//     land at U-2c-fs alongside SYS_TLCREATE (or equivalent kernel
//     surface).
//
// RIGHTS:
//   - The kernel grants RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER on
//     a successful SYS_WALK_OPEN. The server-side fid's omode is what
//     actually gates reads/writes; rights at the handle level are a
//     transfer-control concern, not a read/write gate.

use crate::err::{Error, Result};
use crate::handle::{Handle, Rights};
use crate::io::{Read, Seek, SeekFrom, Write};
use crate::{
    t_close, t_lseek, t_read, t_walk_open, t_write, T_OREAD, T_OTRUNC, T_OWRITE,
    T_SEEK_CUR, T_SEEK_END, T_SEEK_SET, T_WALK_OPEN_FROM_ROOT, T_WALK_OPEN_NAME_MAX,
};
use alloc_crate::vec::Vec;

use super::path::{Component, Path};

/// An open file on Thylacine.
///
/// RAII: `Drop` closes the underlying Spoor handle. Use `File::open`
/// to open for read; `File::create` to open-for-write-with-truncate
/// (the file must already exist at v1 -- see module header).
///
/// Implements `Read`, `Write`, and `Seek`.
pub struct File {
    handle: Handle,
}

impl File {
    /// Open `path` for reading.
    ///
    /// Errors:
    ///   - `Error::InvalidArgument`: path is empty, relative, contains
    ///     `..`, or a component exceeds `T_WALK_OPEN_NAME_MAX` (64).
    ///   - `Error::NotFound`: a component along the walk doesn't
    ///     exist.
    ///   - `Error::PermissionDenied`: rights/mode insufficient at
    ///     some step.
    ///   - Other variants pass through from the kernel's per-step
    ///     SYS_WALK_OPEN return.
    #[inline]
    pub fn open<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_with_omode(path.as_ref(), T_OREAD)
    }

    /// Open `path` for writing with truncate-on-open.
    ///
    /// The file MUST already exist (v1 limitation — no creation
    /// surface yet; see module header). `OWRITE | OTRUNC` semantics
    /// match POSIX `O_WRONLY | O_TRUNC`.
    #[inline]
    pub fn create<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_with_omode(path.as_ref(), T_OWRITE | T_OTRUNC)
    }

    /// The underlying raw handle index. For interop with code that
    /// passes raw `fd`s through other syscalls; libthyla-rs callers
    /// should prefer `Read`/`Write`/`Seek`.
    #[inline]
    pub fn as_raw_fd(&self) -> i32 {
        self.handle.raw()
    }

    /// The rights the kernel granted on this File's handle. Always
    /// `READ | WRITE | TRANSFER` at v1 (the kernel's SYS_WALK_OPEN
    /// envelope); the underlying fid's open-mode is what actually
    /// gates read/write success.
    #[inline]
    pub fn rights(&self) -> Rights {
        self.handle.rights()
    }

    /// Fetch the file's metadata via SYS_FSTAT.
    ///
    /// Returns size, type (file/dir/char-device), mode, link count,
    /// timestamps, 9P qid, blksize, blocks. See `t::fs::Metadata`.
    #[inline]
    pub fn metadata(&self) -> Result<super::Metadata> {
        super::metadata::fstat_fd(self.handle.raw())
    }

    /// Construct a File from an existing Handle. `pub(crate)` so
    /// other libthyla-rs modules (t::process::pipe, t::process's
    /// Stdio::Piped handling) can wrap kernel-returned pipe fds as
    /// Files. External callers obtain Files via File::open / create
    /// / OpenOptions::open.
    #[inline]
    pub(crate) fn from_raw_handle(handle: Handle) -> File {
        File { handle }
    }

    pub(crate) fn open_with_omode(path: &Path, omode: u32) -> Result<File> {
        if path.is_empty() {
            return Err(Error::InvalidArgument);
        }
        if path.is_relative() {
            // V1: only absolute paths. Future File::open_at(&Spoor, name)
            // will let callers walk from a held Spoor.
            return Err(Error::InvalidArgument);
        }

        // Collect Normal components; reject ParentDir; skip CurDir +
        // the leading RootDir.
        let names: Vec<&str> = path
            .components()
            .filter_map(|c| match c {
                Component::Normal(s) => Some(Ok(s)),
                Component::ParentDir => Some(Err(Error::InvalidArgument)),
                Component::RootDir | Component::CurDir => None,
            })
            .collect::<Result<Vec<&str>>>()?;

        if names.is_empty() {
            // Path was "/" with only RootDir + (optional) CurDirs.
            return Err(Error::InvalidArgument);
        }

        // Walk per-component. `current` starts as the FROM_ROOT
        // sentinel; later iterations replace it with the freshly-
        // opened intermediate handle. `current_is_owned` tracks
        // whether we need to close it (FROM_ROOT is a kernel sentinel
        // and must NOT be closed).
        let mut current: i64 = T_WALK_OPEN_FROM_ROOT;
        let mut current_is_owned: bool = false;
        let last_idx = names.len() - 1;

        for (i, name) in names.iter().enumerate() {
            if name.is_empty() || name.len() > T_WALK_OPEN_NAME_MAX {
                if current_is_owned {
                    // SAFETY: current was returned by a prior SYS_WALK_OPEN
                    // on this Proc; closing it is a valid op.
                    unsafe {
                        let _ = t_close(current);
                    }
                }
                return Err(Error::InvalidArgument);
            }

            let step_omode = if i == last_idx { omode } else { T_OREAD };

            // SAFETY: name is a valid &str slice (len + ptr); current
            // is either FROM_ROOT or a Spoor fd this Proc owns; omode
            // is in the SYS_WALK_OPEN_OMODE_VALID mask (0x13).
            let rc = unsafe {
                t_walk_open(current, name.as_ptr(), name.len(), step_omode)
            };

            // Always close the prior owned handle before checking
            // for errors -- it's no longer needed regardless of
            // success/failure on this step.
            if current_is_owned {
                unsafe {
                    let _ = t_close(current);
                }
            }

            let new_fd = Error::from_syscall_return(rc)?;
            current = new_fd;
            current_is_owned = true;
        }

        // current is the freshly-opened final fd; rights match
        // SYS_WALK_OPEN's documented envelope. Handle::from_raw's
        // contract (informal, pub(crate)) is satisfied: the fd is a
        // live KOBJ_SPOOR slot in this Proc.
        let handle = Handle::from_raw(
            current as i32,
            Rights::READ | Rights::WRITE | Rights::TRANSFER,
        );
        Ok(File { handle })
    }
}

impl Read for File {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        // SAFETY: buf is a valid user-VA byte slice; handle.raw() is
        // a live KOBJ_SPOOR in this Proc.
        let rc = unsafe {
            t_read(
                self.handle.raw() as i64,
                buf.as_mut_ptr(),
                buf.len(),
            )
        };
        let n = Error::from_syscall_return(rc)?;
        Ok(n as usize)
    }
}

impl Write for File {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let rc = unsafe {
            t_write(
                self.handle.raw() as i64,
                buf.as_ptr(),
                buf.len(),
            )
        };
        let n = Error::from_syscall_return(rc)?;
        Ok(n as usize)
    }

    fn flush(&mut self) -> Result<()> {
        // Kernel writes through SYS_WRITE are not userspace-buffered
        // (no stdio layer at this point in the stack). Flush is a
        // no-op for raw File; buffered writers like BufWriter (v1.x)
        // override.
        Ok(())
    }
}

impl Seek for File {
    fn seek(&mut self, pos: SeekFrom) -> Result<u64> {
        let (whence, offset) = match pos {
            SeekFrom::Start(p) => {
                // The kernel takes a signed i64 offset; reject u64 values
                // that don't fit (>= 2^63) at the boundary rather than
                // wrap-cast into a negative offset the kernel would reject.
                if p > i64::MAX as u64 {
                    return Err(Error::InvalidArgument);
                }
                (T_SEEK_SET, p as i64)
            }
            SeekFrom::Current(o) => (T_SEEK_CUR, o),
            SeekFrom::End(o) => (T_SEEK_END, o),
        };
        let rc = unsafe { t_lseek(self.handle.raw() as i64, offset, whence) };
        let n = Error::from_syscall_return(rc)?;
        Ok(n as u64)
    }
}

impl crate::poll::AsFd for File {
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        self.handle.raw()
    }
}
