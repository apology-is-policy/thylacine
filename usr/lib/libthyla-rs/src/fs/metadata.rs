// libthyla-rs::fs::metadata — Metadata struct backed by SYS_FSTAT.
//
// Mirror of std::fs::Metadata, scoped to what Thylacine's kernel
// surfaces today. Backed by `struct t_stat` (80 bytes, ABI-pinned per
// kernel/include/thylacine/syscall.h).
//
// Foundation chunk: U-2c-fs per docs/UTOPIA-SHELL-DESIGN.md §15
// (third slice of the U-2c split: U-2c-path / U-2c-io / U-2c-fs).
//
// FIELD COVERAGE:
//   - size, nlink, mode (POSIX-shaped bits per T_S_IF*).
//   - 9P qid (path / vers / type) — distinguishing identity per Dev,
//     stable for the file's lifetime.
//   - atime / mtime / ctime as raw u64 epoch seconds. The kernel
//     populates 0 at v1.0 (most Devs don't track timestamps); user
//     code observes 0 there. A future v1.x t::time::SystemTime wraps
//     these into typed instants.
//   - blksize + blocks for stat(2)-style I/O sizing.
//
// SYMLINKS:
//   - v1.0 Thylacine has no symlink surface. `is_symlink()` always
//     returns false; `symlink_metadata()` is the same as `metadata()`.

use crate::err::{Error, Result};
use crate::{
    t_fstat, T_S_IFCHR, T_S_IFDIR, T_S_IFMT, T_S_IFREG,
};
use core::mem::MaybeUninit;

/// File / Spoor metadata returned by `t::fs::File::metadata`,
/// `t::fs::metadata(path)`, and friends.
///
/// 88-byte plain-data struct mirroring the kernel's `struct t_stat`
/// (A-2a appended uid + gid -> 80; #100 appended devno + pad -> 88).
/// Cheap to copy.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct Metadata {
    size: u64,
    qid_path: u64,
    atime_sec: u64,
    mtime_sec: u64,
    ctime_sec: u64,
    mode: u32,
    nlink: u32,
    qid_vers: u32,
    qid_type: u8,
    _pad_qid: [u8; 3],
    blksize: u32,
    _pad_blksize: u32,
    blocks: u64,
    uid: u32,
    gid: u32,
    dev: u32,
    _pad_dev: u32,
}

// Compile-time check that our Rust mirror matches the kernel's ABI pin
// (88 bytes; #100 devno). If a v1.x kernel adds fields to struct t_stat
// without extending Metadata, this assertion fires -- and since the kernel
// writes sizeof(t_stat) bytes into this buffer, a stale size would corrupt.
const _: () = assert!(core::mem::size_of::<Metadata>() == 88);

impl Metadata {
    /// File size in bytes.
    #[inline]
    #[must_use]
    pub const fn len(&self) -> u64 {
        self.size
    }

    /// `true` iff this is a regular file.
    #[inline]
    #[must_use]
    pub const fn is_file(&self) -> bool {
        (self.mode & T_S_IFMT) == T_S_IFREG
    }

    /// `true` iff this is a directory.
    #[inline]
    #[must_use]
    pub const fn is_dir(&self) -> bool {
        (self.mode & T_S_IFMT) == T_S_IFDIR
    }

    /// `true` iff this is a character device.
    #[inline]
    #[must_use]
    pub const fn is_char_device(&self) -> bool {
        (self.mode & T_S_IFMT) == T_S_IFCHR
    }

    /// `true` iff this is a symbolic link. Always `false` at v1.0;
    /// Thylacine has no symlink surface yet.
    #[inline]
    #[must_use]
    pub const fn is_symlink(&self) -> bool {
        false
    }

    /// `true` iff `len() == 0`.
    #[inline]
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.size == 0
    }

    /// POSIX-shaped mode bits (includes the file-type bits per
    /// `T_S_IF*`; mask with `T_S_IFMT` to extract the type).
    #[inline]
    #[must_use]
    pub const fn mode(&self) -> u32 {
        self.mode
    }

    /// Permission bits (mode with the type bits masked out — the
    /// classic `0o644`, `0o755`, etc.). Convenience over `mode()`.
    #[inline]
    #[must_use]
    pub const fn permissions(&self) -> u32 {
        self.mode & !T_S_IFMT
    }

    /// Hard-link count. `1` for regular files at v1.0.
    #[inline]
    #[must_use]
    pub const fn nlink(&self) -> u32 {
        self.nlink
    }

    /// Access time (epoch seconds). `0` at v1.0 (most Devs don't
    /// populate); a v1.x kernel may surface real timestamps.
    #[inline]
    #[must_use]
    pub const fn atime_sec(&self) -> u64 {
        self.atime_sec
    }

    /// Modification time (epoch seconds). `0` at v1.0.
    #[inline]
    #[must_use]
    pub const fn mtime_sec(&self) -> u64 {
        self.mtime_sec
    }

    /// Metadata-change time (epoch seconds). `0` at v1.0.
    #[inline]
    #[must_use]
    pub const fn ctime_sec(&self) -> u64 {
        self.ctime_sec
    }

    /// 9P qid.path — unique identifier within a Dev. Plays the role
    /// of an inode number for code that needs file identity.
    #[inline]
    #[must_use]
    pub const fn qid_path(&self) -> u64 {
        self.qid_path
    }

    /// 9P qid.vers — version counter (`0` at v1.0).
    #[inline]
    #[must_use]
    pub const fn qid_vers(&self) -> u32 {
        self.qid_vers
    }

    /// 9P qid.type — `QTFILE` / `QTDIR` / ... raw byte.
    #[inline]
    #[must_use]
    pub const fn qid_type(&self) -> u8 {
        self.qid_type
    }

    /// Preferred I/O size hint.
    #[inline]
    #[must_use]
    pub const fn blksize(&self) -> u32 {
        self.blksize
    }

    /// Count of 512-byte blocks the file occupies on its backing
    /// store.
    #[inline]
    #[must_use]
    pub const fn blocks(&self) -> u64 {
        self.blocks
    }

    /// Owner principal-id (A-2a). `T_PRINCIPAL_SYSTEM` for boot-FS
    /// (devramfs) files; the server-reported owner for Stratum-backed
    /// (dev9p) files.
    #[inline]
    #[must_use]
    pub const fn uid(&self) -> u32 {
        self.uid
    }

    /// Owning group (A-2a). `T_GID_SYSTEM` for boot-FS files; the
    /// server-reported group for Stratum-backed files.
    #[inline]
    #[must_use]
    pub const fn gid(&self) -> u32 {
        self.gid
    }

    /// Per-instance device number (Plan 9 Chan.dev / POSIX st_dev, #100):
    /// the mount/session identity. `(dev(), qid_path())` uniquely names a
    /// file ACROSS datasets (two datasets can reuse a qid.path); `0` for a
    /// static single-instance Dev like the boot ramfs.
    #[inline]
    #[must_use]
    pub const fn dev(&self) -> u32 {
        self.dev
    }
}

/// Internal: fill a Metadata from the given Spoor fd via SYS_FSTAT.
/// Called by `File::metadata` and `fs::metadata`.
///
/// Returns the populated Metadata on success; any kernel rejection
/// (bad fd, Dev without `stat_native`, etc.) propagates as the matching
/// `Error` variant.
/// Path-stat via SYS_STAT (POUNCE): one syscall, and on a walk_attrs-capable
/// Dev one fused RPC with no handle/Spoor/fid ever created. The hot path
/// under `fs::metadata` / `exists` / `is_file` / `is_dir`.
pub(crate) fn stat_path(path: &super::Path) -> Result<Metadata> {
    let bytes = path.as_str().as_bytes();
    let mut buf: MaybeUninit<Metadata> = MaybeUninit::uninit();
    // SAFETY: as fstat_fd -- the kernel fully initializes the 80 bytes on
    // success; on failure the uninitialized struct is discarded unread.
    let rc = unsafe {
        crate::t_stat_path(bytes.as_ptr(), bytes.len(), buf.as_mut_ptr() as *mut u8)
    };
    let _n = Error::from_syscall_return(rc)?;
    Ok(unsafe { buf.assume_init() })
}

pub(crate) fn fstat_fd(spoor_fd: i32) -> Result<Metadata> {
    // SAFETY: MaybeUninit<Metadata> reserves 80 bytes of stack-aligned
    // memory; t_fstat fully initializes it on success. On failure we
    // discard the (uninitialized) struct without reading it. The
    // pointer cast is safe — Metadata is #[repr(C)] matching the
    // kernel's struct t_stat byte-for-byte.
    let mut buf: MaybeUninit<Metadata> = MaybeUninit::uninit();
    let rc = unsafe {
        t_fstat(spoor_fd as i64, buf.as_mut_ptr() as *mut u8)
    };
    let _n = Error::from_syscall_return(rc)?;
    // Success -- the kernel wrote 80 bytes into buf.
    Ok(unsafe { buf.assume_init() })
}
