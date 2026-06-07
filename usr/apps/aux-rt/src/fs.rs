// aux-rt::fs -- the file-creation / mutation / directory shim the safe
// libthyla-rs::fs layer does not provide (DOC-GAP G09). Thin wrappers over
// the raw t_open / t_walk_create / t_unlink / t_rename / t_readdir / t_fsync
// SVC wrappers (which DO exist), plus an `OwnedFd` that wraps a raw fd as
// io::Read/Write/Seek -- the `File::from_raw_fd` libthyla-rs withholds (G05;
// File::from_raw_handle is pub(crate)).
//
// All paths are ABSOLUTE (no cwd at v1.0; G07). Mutation ops resolve the
// parent directory (O_PATH, walk-only) then act on the final component by
// name, mirroring the kernel's (parent_fd, name) syscall shape.

use crate::alloc_crate::string::{String, ToString};
use crate::alloc_crate::vec::Vec;

use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::Path;
use libthyla_rs::io::{Read, Seek, SeekFrom, Write};
use libthyla_rs::{
    t_close, t_fsync, t_lseek, t_open, t_read, t_readdir, t_rename, t_unlink, t_walk_create,
    t_write, T_OPATH, T_OREAD, T_OWRITE, T_SEEK_CUR, T_SEEK_END, T_SEEK_SET, T_UNLINK_REMOVEDIR,
    T_WALK_CREATE_DMDIR, T_WALK_OPEN_FROM_ROOT, T_WALK_OPEN_NAME_MAX,
};

const FROM_ROOT: i64 = T_WALK_OPEN_FROM_ROOT;

/// An owned file descriptor (raw KOBJ_SPOOR handle). `Drop` closes it.
/// Implements io::Read/Write/Seek -- the read+write+seek handle the safe fs
/// layer cannot give you for an arbitrary raw fd.
pub struct OwnedFd {
    fd: i64,
}

impl OwnedFd {
    /// Wrap a raw fd. The caller transfers ownership (Drop will close it).
    pub fn from_raw(fd: i64) -> OwnedFd {
        OwnedFd { fd }
    }

    /// The underlying raw handle index.
    pub fn raw(&self) -> i64 {
        self.fd
    }

    /// Durability barrier (SYS_FSYNC, full).
    pub fn fsync(&self) -> Result<()> {
        let rc = unsafe { t_fsync(self.fd, 0) };
        Error::from_syscall_return(rc).map(|_| ())
    }
}

impl Drop for OwnedFd {
    fn drop(&mut self) {
        // Best-effort close; nothing useful to do with a close error here.
        unsafe {
            let _ = t_close(self.fd);
        }
    }
}

impl Read for OwnedFd {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let rc = unsafe { t_read(self.fd, buf.as_mut_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
}

impl Write for OwnedFd {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let rc = unsafe { t_write(self.fd, buf.as_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

impl Seek for OwnedFd {
    fn seek(&mut self, pos: SeekFrom) -> Result<u64> {
        let (whence, off) = match pos {
            SeekFrom::Start(p) => {
                if p > i64::MAX as u64 {
                    return Err(Error::InvalidArgument);
                }
                (T_SEEK_SET, p as i64)
            }
            SeekFrom::Current(o) => (T_SEEK_CUR, o),
            SeekFrom::End(o) => (T_SEEK_END, o),
        };
        let rc = unsafe { t_lseek(self.fd, off, whence) };
        Error::from_syscall_return(rc).map(|n| n as u64)
    }
}

// Parent directory handle for a mutation op: either the territory root
// sentinel or an owned O_PATH handle (closed on Drop).
enum Parent {
    Root,
    Owned(OwnedFd),
}

impl Parent {
    fn raw(&self) -> i64 {
        match self {
            Parent::Root => FROM_ROOT,
            Parent::Owned(f) => f.raw(),
        }
    }
}

// Split an ABSOLUTE path into (parent dir handle, final component name).
// The parent is opened O_PATH (walk-only) unless it is the root. The
// returned name borrows `path`. Rejects relative paths, "/", ".", "..".
fn resolve_parent(path: &str) -> Result<(Parent, &str)> {
    let p = Path::new(path);
    if p.is_relative() {
        return Err(Error::InvalidArgument);
    }
    let name = p.file_name().ok_or(Error::InvalidArgument)?; // None for "/", ".", ".."
    if name.is_empty() || name.len() > T_WALK_OPEN_NAME_MAX {
        return Err(Error::InvalidArgument);
    }
    let parent = match p.parent() {
        None => return Err(Error::InvalidArgument), // path was "/"
        Some(pp) if pp.as_str().is_empty() || pp.as_str() == "/" => Parent::Root,
        Some(pp) => {
            let fd = unsafe { t_open(FROM_ROOT, pp.as_str().as_ptr(), pp.as_str().len(), T_OPATH) };
            Parent::Owned(OwnedFd::from_raw(Error::from_syscall_return(fd)?))
        }
    };
    Ok((parent, name))
}

/// Open an absolute path for reading via the kernel stalk resolver
/// (SYS_OPEN / t_open). Exercises a different code path from
/// libthyla-rs::fs::File::open (which loops per-component over t_walk_open).
pub fn open(path: &str) -> Result<OwnedFd> {
    if Path::new(path).is_relative() {
        return Err(Error::InvalidArgument);
    }
    let fd = unsafe { t_open(FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    Ok(OwnedFd::from_raw(Error::from_syscall_return(fd)?))
}

/// Create (or truncate) a regular file and open it for writing. `perm`'s low
/// 9 bits are the POSIX mode. This is what `OpenOptions::create` lacks (G09).
pub fn create(path: &str, perm: u32) -> Result<OwnedFd> {
    let (parent, name) = resolve_parent(path)?;
    let fd = unsafe { t_walk_create(parent.raw(), name.as_ptr(), name.len(), T_OWRITE, perm) };
    Ok(OwnedFd::from_raw(Error::from_syscall_return(fd)?))
}

/// Create a directory with mode `perm`.
pub fn mkdir(path: &str, perm: u32) -> Result<()> {
    let (parent, name) = resolve_parent(path)?;
    let fd = unsafe {
        t_walk_create(
            parent.raw(),
            name.as_ptr(),
            name.len(),
            T_OREAD,
            perm | T_WALK_CREATE_DMDIR,
        )
    };
    let fd = Error::from_syscall_return(fd)?;
    // We do not need the directory's open handle.
    unsafe {
        let _ = t_close(fd);
    }
    Ok(())
}

/// Unlink a non-directory.
pub fn remove_file(path: &str) -> Result<()> {
    let (parent, name) = resolve_parent(path)?;
    let rc = unsafe { t_unlink(parent.raw(), name.as_ptr(), name.len(), 0) };
    Error::from_syscall_return(rc).map(|_| ())
}

/// Remove an empty directory.
pub fn remove_dir(path: &str) -> Result<()> {
    let (parent, name) = resolve_parent(path)?;
    let rc = unsafe { t_unlink(parent.raw(), name.as_ptr(), name.len(), T_UNLINK_REMOVEDIR) };
    Error::from_syscall_return(rc).map(|_| ())
}

/// Atomically rename/move. Both ends must resolve on the same Dev/session
/// (the kernel enforces this; cross-session rename returns an error).
pub fn rename(from: &str, to: &str) -> Result<()> {
    let (fp, fname) = resolve_parent(from)?;
    let (tp, tname) = resolve_parent(to)?;
    let rc = unsafe {
        t_rename(
            fp.raw(),
            fname.as_ptr(),
            fname.len(),
            tp.raw(),
            tname.as_ptr(),
            tname.len(),
        )
    };
    Error::from_syscall_return(rc).map(|_| ())
}

/// 9P2000.L dirent type byte value for a directory (Linux DT_DIR).
pub const DT_DIR: u8 = 4;
// 9P qid.type bit for a directory (QTDIR), the high byte of the qid.
const QTDIR: u8 = 0x80;

/// One directory entry from `read_dir`.
pub struct DirEntry {
    pub name: String,
    /// The standalone dirent `type` byte (9P2000.L; Linux DT_* family).
    pub d_type: u8,
    /// The qid type byte (9P; QTDIR=0x80 high bit set => directory).
    pub qid_type: u8,
}

impl DirEntry {
    /// Robustly classify a directory using BOTH the dirent type byte and the
    /// qid type bit, because the exact encoding of the t_readdir `type` byte
    /// is not pinned by the wrapper contract (DOC-GAP G10).
    pub fn is_dir(&self) -> bool {
        self.d_type == DT_DIR || (self.qid_type & QTDIR) != 0
    }
}

/// List an absolute directory path. Parses the 9P2000.L dirent stream that
/// t_readdir returns: per entry, qid(13) + offset(8 LE) + type(1) +
/// name_len(2 LE) + name (per the t_readdir wrapper contract). Reads until
/// end-of-directory (a t_readdir returning 0).
pub fn read_dir(path: &str) -> Result<Vec<DirEntry>> {
    let dir = open(path)?; // OREAD handle on the directory
    let mut out = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = unsafe { t_readdir(dir.raw(), buf.as_mut_ptr(), buf.len()) };
        let n = Error::from_syscall_return(n)? as usize;
        if n == 0 {
            break; // end of directory
        }
        let mut off = 0usize;
        // Header is qid(13) + offset(8) + type(1) + name_len(2) = 24 bytes.
        while off + 24 <= n {
            let qid_type = buf[off]; // first byte of the 13-byte qid
            let d_type = buf[off + 21]; // after qid(13) + offset(8)
            let name_len = u16::from_le_bytes([buf[off + 22], buf[off + 23]]) as usize;
            let name_start = off + 24;
            if name_start + name_len > n {
                break; // truncated entry (kernel returns whole entries; defensive)
            }
            let name = core::str::from_utf8(&buf[name_start..name_start + name_len])
                .unwrap_or("")
                .to_string();
            out.push(DirEntry {
                name,
                d_type,
                qid_type,
            });
            off = name_start + name_len;
        }
    }
    Ok(out)
}
