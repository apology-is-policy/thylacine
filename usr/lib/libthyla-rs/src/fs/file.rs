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
// PATH RESOLUTION (#87 -- the kernel resolves; userspace only splits):
//   - A PLAIN open hands the WHOLE path (absolute or relative) to the
//     kernel stalk resolver in one SYS_OPEN: the cwd join for a relative
//     path (#83, verbatim), real `.` / `..` resolution with I-28
//     containment, the #79/#81/#82 ENOTDIR gates, per-component X-search
//     (#84), and the real -errno all come from the kernel.
//   - The CREATE legs hand the kernel the WHOLE path too, since #50:
//     SYS_OPEN_CREATE (t_open_create) joins the cwd, splits the leaf, and
//     applies the Linux O_CREAT leaf rows kernel-side (the same
//     kpath_split_leaf discipline: classify, never resolve). `create_dir`
//     rides the same syscall (DMDIR). The remaining fs:: mutations
//     (remove_file / remove_dir / rename) still hand the kernel a
//     (parent_fd, leaf) pair (SYS_UNLINK / SYS_RENAME are parent-fd +
//     leaf-name primitives), so THEIR paths are split LEXICALLY AT THE
//     LAST COMPONENT ONLY (`split_parent_leaf`): the parent prefix --
//     verbatim, slash-terminated by construction -- is opened via the same
//     stalk resolver (`with_parent_fd`, T_OPATH), and the leaf rows
//     (trailing slash, `.`, `..`) are applied per POSIX by each caller,
//     whose rows are FINER than the kernel path-shells' (remove_dir's
//     Busy/EINVAL/ENOTEMPTY trio). Nothing in userspace pops a `..` or
//     drops a dot: pre-resolving here bypassed the kernel gates (#87 --
//     the userspace twin of the kernel #83 join fix and the pouch #86
//     splitter fix).
//   - The parent is opened T_OPATH -- the navigation / capability base,
//     born RIGHT_READ | RIGHT_WRITE (kernel A-3b). Two reasons it must be
//     T_OPATH and not T_OREAD: (1) traversal is the POSIX X-search
//     (T_OPATH is exempt from the R/W perm gate, so a path through a
//     `--x` dir resolves), and (2) the parent must carry RIGHT_WRITE for
//     a create/unlink/rename -- those handlers gate the parent on
//     RIGHT_WRITE, which a T_OREAD (RIGHT_READ-only since A-3b) handle
//     would fail.
//   - Empty path -> Error::InvalidArgument.
//
// CREATE SEMANTICS (U-6d-b create-or-open + append; #50 one-syscall):
//   - `File::create` is create-or-open-with-truncate (std::fs::File::create
//     semantics): the file is created if absent (0644), truncated if
//     present. Backed by SYS_OPEN_CREATE (t_open_create) since #50 -- the
//     open-first / create-on-NOENT / bounded-retry composition runs in the
//     KERNEL, and `create_new` is a single server-atomic T_OEXCL create.
//     NB: devramfs is read-only at v1.0 (its `.create` returns NULL), so
//     creation only succeeds on the dev9p (Stratum-backed) FS; on devramfs
//     the create leg returns an error, matching the read-only boot FS.
//   - `OpenOptions::create` / `create_new` / `append` are honored (see
//     options.rs). `append` is a seek-to-end-at-open approximation (Plan 9
//     omode has no O_APPEND; the single-writer shell-redirect case is
//     exact, concurrent atomic-append is a v1.x kernel surface).
//
// RIGHTS:
//   - Since A-3b the kernel DERIVES the handle rights from the open mode
//     (rights_for_omode): OREAD->RIGHT_READ, OWRITE->RIGHT_WRITE,
//     ORDWR->R|W (+RIGHT_TRANSFER for a normally-opened handle). So a
//     RIGHT-bearing dev9p Dev now gates reads/writes by the handle RIGHT in
//     addition to the server-side omode -- an OREAD handle is RIGHT_READ-only.

use crate::err::{Error, Result};
use crate::handle::{Handle, Rights};
use crate::io::{Read, Seek, SeekFrom, Write};
use crate::{
    t_close, t_lseek, t_open, t_read, t_write, T_ONOFOLLOW, T_OPATH,
    T_OREAD, T_OTRUNC, T_OWRITE, T_SEEK_CUR, T_SEEK_END, T_SEEK_SET, T_WALK_OPEN_FROM_ROOT,
    T_WALK_OPEN_NAME_MAX,
};
use super::path::Path;

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
    ///     `..`, or a component exceeds `T_WALK_OPEN_NAME_MAX` (255).
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

    /// Open the LINK named by `path` rather than what it points at -- the v1.0
    /// `lstat` spelling (DISTRO D-1). `T_OPATH | T_ONOFOLLOW`: the walk expands
    /// every intermediate component as usual and stops at the final one, so
    /// `.metadata()` on the result reports the link's own record
    /// (`is_symlink()` true, `len()` the target string's length).
    ///
    /// On a path whose last component is NOT a link this is an ordinary O_PATH
    /// open -- the flag narrows what the quarry may be, it does not require one.
    /// A trailing '/' defeats it (POSIX 4.13 asserts a directory, which can only
    /// be checked by following).
    #[inline]
    pub fn open_link<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_with_omode(path.as_ref(), T_OPATH | T_ONOFOLLOW)
    }

    /// Open `path` as a NAVIGATION handle (`T_OPATH`): walked, never opened.
    ///
    /// Not a byte-I/O channel -- read/write/readdir on it are refused. It is the
    /// base for creating/walking/renaming/unlinking children, a valid
    /// `t_chroot` target, and the handle a Loom ring registers for the
    /// directory-child ops. Born `READ | WRITE` and exempt from the R/W
    /// permission gate, because traversal authority is the X-search on the path,
    /// not R or W on the directory.
    #[inline]
    pub fn open_with_opath<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_with_omode(path.as_ref(), T_OPATH)
    }

    /// Open `path` for reading, refusing a symlink as the FINAL component
    /// (`T_OREAD | T_ONOFOLLOW`) -- Linux `O_NOFOLLOW`, which answers
    /// `Error::SymlinkLoop` rather than opening the link.
    ///
    /// Intermediate components still expand: the flag says "the thing I name
    /// must not be a link", not "resolve nothing". To open the link ITSELF, use
    /// [`File::open_link`]. A trailing '/' defeats it (POSIX 4.13).
    #[inline]
    pub fn open_nofollow<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_with_omode(path.as_ref(), T_OREAD | T_ONOFOLLOW)
    }

    /// Create-or-open `path` for writing, truncating on open
    /// (`std::fs::File::create` semantics). The file is created (mode
    /// 0644) if absent, truncated if present. `OWRITE | OTRUNC` +
    /// create-on-NotFound. See the module header's CREATE SEMANTICS.
    #[inline]
    pub fn create<P: AsRef<Path>>(path: P) -> Result<File> {
        Self::open_create_at_path(path.as_ref(), T_OWRITE | T_OTRUNC, true, false, 0o644)
    }

    /// The underlying raw handle index. For interop with code that
    /// passes raw `fd`s through other syscalls; libthyla-rs callers
    /// should prefer `Read`/`Write`/`Seek`.
    #[inline]
    pub fn as_raw_fd(&self) -> i32 {
        self.handle.raw()
    }

    /// The rights the kernel granted on this File's handle. Since A-3b the
    /// kernel derives these from the open mode (`rights_for_omode`):
    /// `open` (OREAD) -> `READ`; `create` (OWRITE|OTRUNC) -> `WRITE`; ORDWR ->
    /// `READ | WRITE` (+ `TRANSFER` for a normally-opened handle). On a
    /// RIGHT-enforcing Dev (dev9p) the handle RIGHT gates read/write in
    /// addition to the server-side omode.
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

    /// Adopt an already-open raw fd (a KOBJ_SPOOR / pipe-end index this
    /// Proc owns) as an OWNING `File`: `Drop` will `t_close(fd)`. `rights`
    /// records the capability axis for the userspace `Handle` (the kernel
    /// enforces the real rights; this is the local hint -- see the module
    /// header's RIGHTS note). Closes DOC-GAP G05/G09: lets a caller wrap a
    /// fd returned by a raw wrapper (`t_walk_create`, `t_pipe`) and drive it
    /// through `Read`/`Write`/`Seek`.
    ///
    /// Do NOT pass an inherited standard fd (0/1/2) here -- the resulting
    /// File would close it on drop. For fd 0/1/2 use the non-owning
    /// `io::{stdin, stdout, stderr}` handles.
    ///
    /// # Safety
    /// `fd` must be a live handle index this Proc owns and that no other
    /// `File`/owner will also close (single-owner, to avoid a double-close).
    #[inline]
    pub unsafe fn from_raw_fd(fd: i32, rights: Rights) -> File {
        File { handle: Handle::from_raw(fd, rights) }
    }

    /// Open `path` at the given omode without creating (the read /
    /// open-existing path). Equivalent to `open_create_at_path` with
    /// `create == create_new == false`.
    pub(crate) fn open_with_omode(path: &Path, omode: u32) -> Result<File> {
        Self::open_create_at_path(path, omode, false, false, 0)
    }

    /// Open `path` via the kernel `stalk` resolver (SYS_OPEN) in one syscall,
    /// from the Territory root. Unlike the per-component `t_walk_open` walk in
    /// `open_create_at_path`, stalk resolves the full multi-component path
    /// including the root `/` and `.` / `..` segments (kernel-side containment
    /// keeps `..` from escaping the root). The kernel derives the handle rights
    /// from `omode` (`rights_for_omode`, identical to SYS_WALK_OPEN), so the
    /// userspace `Rights` here are the same hint the walk path records.
    ///
    /// `fs::read_dir` uses this to open a directory (the root and multi-
    /// component paths both work); the root case of `open_create_at_path`'s
    /// plain-open path routes here too (#929).
    pub(crate) fn open_stalk(path: &Path, omode: u32) -> Result<File> {
        let s = path.as_str();
        // SAFETY: s is a valid &str (ptr+len); FROM_ROOT resolves from the
        // Territory root; omode is within the SYS_OPEN omode-valid mask.
        let rc = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, s.as_ptr(), s.len(), omode) };
        let fd = Error::from_syscall_return(rc)?;
        Ok(File {
            handle: Handle::from_raw(fd as i32, Rights::READ | Rights::WRITE | Rights::TRANSFER),
        })
    }

    /// Open-or-create `path`'s final component.
    ///
    /// A PLAIN open (no `create` / `create_new`) hands the whole path to
    /// the kernel stalk resolver (`open_stalk`) -- see the module header:
    /// the cwd join, `.`/`..` resolution, and the ENOTDIR / trailing-slash
    /// gates are all kernel-side, so `File::open("a/../f")` resolves for
    /// real and `File::open("f/")` answers `NotADirectory` (#87).
    ///
    /// The CREATE legs are ONE syscall since #50 (`t_open_create`,
    /// SYS_OPEN_CREATE): the kernel joins the cwd (SYS_OPEN parity), splits
    /// the leaf, applies the Linux O_CREAT leaf rows (a trailing slash or a
    /// `.` / `..` / root final component answers `IsADirectory`, existing
    /// or not -- Linux `open_last_lookups`), and composes create-else-open
    /// bounded. This retired the userspace split + parent-`T_OPATH` +
    /// `t_walk_create` dance AND its create-first rationale, which had gone
    /// stale ("walk_open does not return a distinguishable not-found code"
    /// -- false since the errno rollout gave stalk `T_E_NOENT`).
    ///
    /// `create_new` maps to `T_OEXCL`: ONE create, atomic at the server --
    /// real exclusivity, which the old create-first dance could not promise
    /// (its EEXIST and its open-fallback raced). `create` (non-exclusive)
    /// is kernel-side open-first / create-on-NOENT / bounded-retry; OTRUNC
    /// is stripped on the create leg kernel-side, so a freshly-created
    /// (already-empty) file is still never truncated.
    pub(crate) fn open_create_at_path(
        path: &Path,
        base_omode: u32,
        create: bool,
        create_new: bool,
        perm: u32,
    ) -> Result<File> {
        if path.is_empty() {
            return Err(Error::InvalidArgument);
        }
        if !create && !create_new {
            return Self::open_stalk(path, base_omode);
        }

        let s = path.as_str();
        let omode = if create_new { base_omode | crate::T_OEXCL } else { base_omode };
        // SAFETY: s is a valid &str (ptr+len); FROM_ROOT joins a relative
        // path against the cwd exactly as SYS_OPEN; omode is within the
        // SYS_OPEN_CREATE mask; perm's low 9 bits are the POSIX mode.
        let rc = unsafe {
            crate::t_open_create(T_WALK_OPEN_FROM_ROOT, s.as_ptr(), s.len(), omode, perm)
        };
        let fd = Error::from_syscall_return(rc)?;

        // The final fd is a live KOBJ_SPOOR slot in this Proc; rights
        // match SYS_WALK_OPEN / SYS_WALK_CREATE's documented envelope
        // (the kernel enforces the real, omode-derived rights since
        // A-3b; the userspace Handle rights are a hint).
        let handle = Handle::from_raw(
            fd as i32,
            Rights::READ | Rights::WRITE | Rights::TRANSFER,
        );
        Ok(File { handle })
    }
}

/// The final component of a split path (`split_parent_leaf`).
///
/// The lexical split classifies -- it never resolves. A `Dot` / `DotDot` /
/// `Root` leaf reaches the CALLER, which applies its own POSIX row (the
/// kernel mutation syscalls reject `.` / `..` names outright, so a leaked
/// dot would fail closed with a generic EINVAL -- the rows exist to answer
/// the REAL errno first). #87: the pre-split code popped `..` and dropped
/// `.` lexically, acting on the wrong object entirely.
pub(crate) enum Leaf<'a> {
    /// An ordinary final component (non-empty, within
    /// `T_WALK_OPEN_NAME_MAX`).
    Name(&'a str),
    /// The final component was `.`.
    Dot,
    /// The final component was `..`.
    DotDot,
    /// The path was all separators (`/`, `//`, ...): it names the root,
    /// and there is no leaf.
    Root,
}

/// A path split lexically at its LAST component -- and nowhere else.
pub(crate) struct SplitPath<'a> {
    /// The parent-directory prefix, verbatim, INCLUDING the final
    /// separator: `"a/b/"` for `"a/b/f"`, `"/"` for `"/f"`, `""` for a
    /// bare relative `"f"` (the parent is the cwd). Handed to the kernel
    /// resolver untouched -- the trailing slash makes the kernel's #82
    /// gate answer `NotADirectory` when a prefix component is not a
    /// directory (exactly Linux's parent-walk semantics), and any `.` /
    /// `..` INSIDE the prefix resolves kernel-side with I-28 containment.
    pub parent: &'a str,
    pub leaf: Leaf<'a>,
    /// The original path carried a trailing separator run (`"f/"`,
    /// `"d//"`): POSIX 4.13 -- the path asserts its leaf names a
    /// directory. Each caller applies its row (#86's per-caller table,
    /// one layer up).
    pub dir_required: bool,
}

/// Split `path` at its last component. Pure and lexical: no syscall, no
/// cleaning, no dot resolution -- the ONLY lexical act is locating the
/// final separator (and trimming the trailing run into `dir_required`).
/// Errors: empty path or a `Name` leaf over `T_WALK_OPEN_NAME_MAX` ->
/// `InvalidArgument`.
pub(crate) fn split_parent_leaf(path: &Path) -> Result<SplitPath<'_>> {
    let s = path.as_str();
    if s.is_empty() {
        return Err(Error::InvalidArgument);
    }
    let trimmed = s.trim_end_matches(super::path::SEPARATOR);
    let dir_required = trimmed.len() != s.len();
    if trimmed.is_empty() {
        // All separators: the root.
        return Ok(SplitPath { parent: "/", leaf: Leaf::Root, dir_required });
    }
    let (parent, name) = match trimmed.rfind(super::path::SEPARATOR) {
        // The parent slice comes from the ORIGINAL string so it keeps its
        // own trailing separator; `name` is non-empty because `trimmed`
        // does not end in a separator.
        Some(idx) => (&s[..idx + 1], &trimmed[idx + 1..]),
        None => ("", trimmed),
    };
    let leaf = match name {
        "." => Leaf::Dot,
        ".." => Leaf::DotDot,
        n => {
            if n.len() > T_WALK_OPEN_NAME_MAX {
                return Err(Error::InvalidArgument);
            }
            Leaf::Name(n)
        }
    };
    Ok(SplitPath { parent, leaf, dir_required })
}

/// Open the parent-directory prefix from `split_parent_leaf` and invoke
/// `f(parent_fd)`. The foundation for the path-based mutation
/// free-functions (`fs::create_dir` / `remove_file` / `remove_dir` /
/// `rename`) and the create legs of `open_create_at_path`, which act on a
/// parent dir fd + a single-component name -- the SYS_WALK_CREATE /
/// SYS_UNLINK / SYS_RENAME shape.
///
/// The prefix is resolved by the KERNEL stalk resolver (one SYS_OPEN,
/// T_OPATH -- see the module header for why T_OPATH): a relative prefix
/// joins the per-Proc cwd kernel-side (#83, verbatim), `.` / `..` resolve
/// with I-28 containment, and the slash-terminated prefix carries the #82
/// dir-ness gate, so `f` only ever runs under a real, searchable parent
/// directory. `""` (a bare relative leaf) opens `"."` -- the cwd, which
/// must still exist and be searchable (#81's dot gate). An all-separator
/// prefix (`"/"`) uses the `T_WALK_OPEN_FROM_ROOT` sentinel directly (the
/// mutation syscalls resolve it to the Territory root; nothing to open).
///
/// The opened fd is closed after `f` returns -- on success OR error -- so
/// `f` must not retain it past its own return.
pub(crate) fn with_parent_fd<T>(
    parent: &str,
    f: impl FnOnce(i64) -> Result<T>,
) -> Result<T> {
    if !parent.is_empty() && parent.bytes().all(|b| b == b'/') {
        return f(T_WALK_OPEN_FROM_ROOT);
    }
    let pstr: &str = if parent.is_empty() { "." } else { parent };
    // SAFETY: pstr is a valid &str (ptr+len); FROM_ROOT is the kernel
    // sentinel start; T_OPATH is in the SYS_OPEN omode-valid mask.
    let rc = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, pstr.as_ptr(), pstr.len(), T_OPATH) };
    let parent_fd = Error::from_syscall_return(rc)?;
    let result = f(parent_fd);
    // SAFETY: parent_fd is a live Spoor handle this Proc owns.
    unsafe {
        let _ = t_close(parent_fd);
    }
    result
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
