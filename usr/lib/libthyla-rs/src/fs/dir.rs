// libthyla-rs::fs::dir — directory enumeration over SYS_READDIR.
//
// `ReadDir` is the iterator `fs::read_dir` returns (the std::fs::read_dir
// shape). It owns the open directory `File` and streams `DirEntry` items by
// repeatedly calling SYS_READDIR (`t_readdir`), which returns a *run* of
// 9P2000.L dirents and advances the directory's kernel-side resume cursor.
//
// WIRE FORMAT (per entry, matching kernel/syscall.c sys_readdir_handler):
//
//   qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name(name_len)
//
// where qid is type(1) + version(4 LE) + path(8 LE). The `offset` field is a
// RESUME COOKIE that the kernel stores on the Spoor; we never feed it back
// ourselves — successive `t_readdir` calls continue from where the last left
// off (the kernel tracks `c->offset`). `t_readdir` returns the run byte-count
// (> 0), 0 at end-of-directory, or -1 on error.
//
// PARSING DISCIPLINE: we parse COMPLETE ENTRIES ONLY. When the bytes remaining
// in the staging buffer can't hold a whole entry, we refill — discarding the
// partial trailing bytes, which the kernel re-sends in full on the next run
// (its cookie only advances to the last *complete* entry it returned). This
// mirrors the handler's own accounting, so the stream has no duplicated and no
// skipped entry. (In practice no well-behaved Dev returns a partial trailing
// entry — devramfs and the 9P server both stop at whole-entry boundaries — so
// the partial path is defensive.)
//
// LAZINESS: `read_dir` opens the directory and returns the iterator; a target
// that is not a directory (or otherwise unreadable) surfaces as the first
// `next()` yielding `Err`, after which iteration ends. The glob walker treats
// that as "no matches" (rc nullglob).

use crate::err::{Error, Result};
use crate::t_readdir;
use alloc_crate::string::String;
use alloc_crate::vec;
use alloc_crate::vec::Vec;

use super::file::File;
use super::path::Path;

/// 9P qid.type bit for a directory (`P9_QTDIR`).
const QTDIR: u8 = 0x80;

/// The fixed dirent header: qid(13) + offset(8) + type(1) + name_len(2).
const DIRENT_HDR: usize = 24;

/// Staging buffer for one `t_readdir` run. Sized well above one maximal entry
/// (24-byte header + a long name) so the kernel always returns at least one
/// whole entry -- a buffer too small for the first entry returns -1 (a Dev
/// reports "too small", not EOD), which `refill` would surface as an `Err`.
/// 4 KiB holds many short directory entries per syscall.
const READDIR_BUF: usize = 4096;

/// One entry yielded by [`ReadDir`]. Carries the entry name and its 9P qid
/// type, so [`DirEntry::is_dir`] needs no extra `fstat`.
pub struct DirEntry {
    name: String,
    qid_type: u8,
}

impl DirEntry {
    /// The bare file name of this entry (no directory prefix).
    #[inline]
    #[must_use]
    pub fn file_name(&self) -> &str {
        &self.name
    }

    /// Consume the entry, returning the owned name.
    #[inline]
    #[must_use]
    pub fn into_file_name(self) -> String {
        self.name
    }

    /// The raw 9P qid.type byte (`QTDIR` = 0x80, `QTFILE` = 0x00, ...).
    #[inline]
    #[must_use]
    pub fn qid_type(&self) -> u8 {
        self.qid_type
    }

    /// Whether this entry is a directory (per the qid type bit). The glob
    /// walker uses this to decide whether to descend a multi-component pattern.
    #[inline]
    #[must_use]
    pub fn is_dir(&self) -> bool {
        (self.qid_type & QTDIR) != 0
    }

    /// Whether this entry is a non-directory (regular file at v1.0 — Thylacine
    /// has no symlink surface, and Devs mark directories with `QTDIR` and
    /// everything else `QTFILE`).
    #[inline]
    #[must_use]
    pub fn is_file(&self) -> bool {
        (self.qid_type & QTDIR) == 0
    }
}

/// Streaming directory iterator. Owns the open directory `File`; `Drop` closes
/// the handle. Yields `Result<DirEntry>`; a kernel error during a refill yields
/// one `Err` and then ends iteration.
pub struct ReadDir {
    dir: File,
    buf: Vec<u8>,
    filled: usize, // valid bytes in buf[0..filled]
    pos: usize,    // cursor into buf[0..filled]
    done: bool,    // t_readdir returned 0 (EOD) or errored — stop refilling
}

impl ReadDir {
    pub(crate) fn from_file(dir: File) -> ReadDir {
        ReadDir {
            dir,
            buf: vec![0u8; READDIR_BUF],
            filled: 0,
            pos: 0,
            done: false,
        }
    }

    /// Refill the staging buffer with the next run. `Ok(true)` if bytes were
    /// read, `Ok(false)` at EOD, `Err` on a kernel failure.
    fn refill(&mut self) -> Result<bool> {
        // SAFETY: buf is a live, writable, READDIR_BUF-byte allocation; dir is
        // an open KOBJ_SPOOR (RIGHT_READ) this Proc owns. t_readdir writes at
        // most buf.len() bytes and returns the count.
        let rc = unsafe { t_readdir(self.dir.as_raw_fd() as i64, self.buf.as_mut_ptr(), self.buf.len()) };
        let n = Error::from_syscall_return(rc)?;
        if n == 0 {
            return Ok(false); // end-of-directory
        }
        self.filled = n as usize;
        self.pos = 0;
        Ok(true)
    }
}

impl Iterator for ReadDir {
    type Item = Result<DirEntry>;

    fn next(&mut self) -> Option<Result<DirEntry>> {
        loop {
            // Parse a complete entry at `pos` if one is fully present.
            if self.pos + DIRENT_HDR <= self.filled {
                let p = self.pos;
                let qid_type = self.buf[p]; // qid byte 0
                let nlen =
                    (self.buf[p + 22] as usize) | ((self.buf[p + 23] as usize) << 8);
                let end = p + DIRENT_HDR + nlen;
                if end <= self.filled {
                    let name = String::from_utf8_lossy(&self.buf[p + DIRENT_HDR..end]).into_owned();
                    self.pos = end;
                    return Some(Ok(DirEntry { name, qid_type }));
                }
                // Partial trailing entry: fall through to refill (its bytes are
                // re-sent in full on the next run — see the module header).
            }
            if self.done {
                return None;
            }
            match self.refill() {
                Ok(true) => continue,
                Ok(false) => {
                    self.done = true;
                    return None;
                }
                Err(e) => {
                    self.done = true;
                    return Some(Err(e));
                }
            }
        }
    }
}

/// Iterate the entries of the directory at `path`.
///
/// Opens the directory through the kernel `stalk` resolver (`t_open`), so
/// `path` may be the root `/`, multi-component, and contain `.` / `..`. Returns
/// a [`ReadDir`] streaming [`DirEntry`] items. The directory is held open for
/// the lifetime of the `ReadDir`.
///
/// Errors:
///   - the open itself fails (`NotFound`, `PermissionDenied`, ...) -> `Err`
///     returned here;
///   - the path resolves to a non-directory -> the open succeeds but the first
///     `next()` yields `Err` (the kernel readdir rejects a non-directory Spoor).
pub fn read_dir<P: AsRef<Path>>(path: P) -> Result<ReadDir> {
    let dir = File::open_stalk(path.as_ref(), crate::T_OREAD)?;
    Ok(ReadDir::from_file(dir))
}
