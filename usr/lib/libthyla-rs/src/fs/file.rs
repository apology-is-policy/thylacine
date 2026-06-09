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
//     SYS_WALK_OPEN(FROM_ROOT, "etc",   T_OPATH)  -> etc_fd
//     SYS_WALK_OPEN(etc_fd,    "hosts", omode)    -> hosts_fd
//     close(etc_fd)
//     return hosts_fd
//
//   Intermediate components are walked with T_OPATH -- the directory
//   navigation / capability base, born RIGHT_READ | RIGHT_WRITE (kernel
//   A-3b). Two reasons it must be T_OPATH and not T_OREAD: (1) traversal
//   is the POSIX X-search (T_OPATH is exempt from the R/W perm gate, so a
//   path through a `--x` dir resolves), and (2) the final parent must carry
//   RIGHT_WRITE for a `File::create` at depth >= 2 -- SYS_WALK_CREATE gates
//   the parent on RIGHT_WRITE, which an T_OREAD (RIGHT_READ-only since A-3b)
//   intermediate would fail. The final component takes the requested omode.
//   Intermediate fds are closed before the final fd is returned -- no leakage.
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
// CREATE SEMANTICS (U-6d-b: create-or-open + append landed):
//   - `File::create` is create-or-open-with-truncate (std::fs::File::create
//     semantics): the file is created if absent (0644), truncated if
//     present. Backed by SYS_WALK_CREATE (t_walk_create) -- the FS-alpha
//     kernel surface -- via the open-existing-first / create-on-NotFound
//     dance in `open_create_at_path` (semantics-independent of whether the
//     server's create is exclusive). NB: devramfs is read-only at v1.0
//     (its `.create` returns NULL), so creation only succeeds on the
//     dev9p (Stratum-backed) FS; on devramfs the create leg returns an
//     error, matching the read-only boot FS.
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
    t_close, t_lseek, t_open, t_read, t_walk_create, t_walk_open, t_write, T_OPATH, T_OREAD,
    T_OTRUNC, T_OWRITE, T_SEEK_CUR, T_SEEK_END, T_SEEK_SET, T_WALK_OPEN_FROM_ROOT,
    T_WALK_OPEN_NAME_MAX,
};
use alloc_crate::string::String;
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

    /// Walk `path` and open-or-create its final component.
    ///
    /// The intermediate components are walked with `T_OPATH` (the directory
    /// navigation base; see the module header -- born R|W so a depth >= 2
    /// `create` reaches a `RIGHT_WRITE` parent, and traversal is X-search
    /// only); the final component is opened at
    /// `base_omode`, creating it (mode `perm`) when `create` /
    /// `create_new` request it. `create_new` is exclusive (it always
    /// uses `t_walk_create` and fails if the file exists). `create`
    /// (non-exclusive) opens the existing file first and falls back to
    /// `t_walk_create` only on `NotFound` -- so it is correct whether
    /// or not the underlying server's create is exclusive, and it never
    /// truncates a freshly-created (already-empty) file.
    ///
    /// Path rules + intermediate-fd discipline match `File::open`:
    /// absolute-only, `.` skipped, `..` rejected, intermediate handles
    /// closed before the final fd is returned (no leakage).
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
        // LS-4: a RELATIVE create/open path resolves against the per-Proc cwd
        // (the same absolutize `with_parent_dir` uses for the fs:: mutations).
        // The holder outlives the rest of the call.
        let abs_holder: String;
        let path: &Path = if path.is_relative() {
            abs_holder = cwd_join_clean(path)?;
            Path::new(&abs_holder)
        } else {
            path
        };

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
            // Path was "/" (RootDir + optional CurDirs). The per-component walk
            // has no component to open, but the root IS openable: the `stalk`
            // resolver (SYS_OPEN) returns the Territory root Spoor for "/"
            // (#929 -- File::open("/") / metadata("/") / is_dir("/") now work).
            // Creating or exclusively-creating the root is nonsensical.
            if create || create_new {
                return Err(Error::InvalidArgument);
            }
            return Self::open_stalk(Path::new("/"), base_omode);
        }

        // Walk the parent chain (`names[0..last]`) with T_OPATH. `parent`
        // starts as the FROM_ROOT sentinel; later iterations replace it
        // with the freshly-opened intermediate handle. `parent_owned`
        // tracks whether we must close it (FROM_ROOT is a kernel
        // sentinel and must NOT be closed).
        let mut parent: i64 = T_WALK_OPEN_FROM_ROOT;
        let mut parent_owned: bool = false;
        let last_idx = names.len() - 1;

        for (i, name) in names.iter().enumerate() {
            if name.is_empty() || name.len() > T_WALK_OPEN_NAME_MAX {
                if parent_owned {
                    // SAFETY: parent was returned by a prior SYS_WALK_OPEN
                    // on this Proc; closing it is a valid op.
                    unsafe {
                        let _ = t_close(parent);
                    }
                }
                return Err(Error::InvalidArgument);
            }
            if i == last_idx {
                break; // the final component is handled below
            }

            // SAFETY: name is a valid &str slice (len + ptr); parent is
            // either FROM_ROOT or a Spoor fd this Proc owns; T_OPATH is in
            // the SYS_WALK_OPEN_OMODE_VALID mask. T_OPATH (not T_OREAD) so the
            // final parent carries RIGHT_WRITE for a depth >= 2 create and
            // traversal is X-search only -- see the module header.
            let rc = unsafe { t_walk_open(parent, name.as_ptr(), name.len(), T_OPATH) };

            // Always close the prior owned handle before checking for
            // errors -- it's no longer needed regardless of
            // success/failure on this step.
            if parent_owned {
                unsafe {
                    let _ = t_close(parent);
                }
            }

            let new_fd = Error::from_syscall_return(rc)?;
            parent = new_fd;
            parent_owned = true;
        }

        // Open-or-create the final component under `parent`. Close the
        // owned parent fd before returning (success OR error).
        let last = names[last_idx];
        let final_fd = last_open_or_create(parent, last, base_omode, create, create_new, perm);
        if parent_owned {
            unsafe {
                let _ = t_close(parent);
            }
        }
        let fd = final_fd?;

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

/// Open-or-create a single component `last` inside the already-opened
/// directory `parent`. Returns the raw fd. See `open_create_at_path`
/// for the create / create_new contract.
///
/// The create path is CREATE-FIRST (mirroring joey's `mkdir_or_open`):
/// `SYS_WALK_CREATE` is exclusive (it fails if the target exists), and
/// the kernel's walk_open does not return a distinguishable "not found"
/// code, so the robust create-or-open is "try create; on failure open
/// the existing file". This never depends on a missing-file error code.
fn last_open_or_create(
    parent: i64,
    last: &str,
    base_omode: u32,
    create: bool,
    create_new: bool,
    perm: u32,
) -> Result<i64> {
    // A new file is created empty, so OTRUNC is meaningless on the
    // create leg (the truncate only matters when opening an existing
    // file below).
    let create_omode = base_omode & !T_OTRUNC;

    if create_new {
        // Exclusive create: always go through t_walk_create; it fails
        // if the file already exists.
        // SAFETY: last is a valid &str (ptr+len); parent is FROM_ROOT
        // or a Spoor this Proc owns; create_omode is OREAD/OWRITE/ORDWR.
        let rc = unsafe {
            t_walk_create(parent, last.as_ptr(), last.len(), create_omode, perm)
        };
        return Error::from_syscall_return(rc);
    }

    if create {
        // Create-first. On success the (absent) file is created + opened
        // empty. On failure (it already exists, OR the parent denies
        // creation) fall back to opening the existing file -- with
        // truncate when requested. If the file genuinely cannot be
        // reached, the open carries the real error.
        let cf = unsafe {
            t_walk_create(parent, last.as_ptr(), last.len(), create_omode, perm)
        };
        if cf >= 0 {
            return Ok(cf);
        }
        let rc = unsafe { t_walk_open(parent, last.as_ptr(), last.len(), base_omode) };
        return Error::from_syscall_return(rc);
    }

    // Plain open (no create).
    let rc = unsafe { t_walk_open(parent, last.as_ptr(), last.len(), base_omode) };
    Error::from_syscall_return(rc)
}

/// Walk `path`'s parent-directory chain and invoke `f(parent_fd, basename)`
/// with the parent dir handle and the final path component. The foundation
/// for the path-based mutation free-functions (`fs::create_dir` /
/// `remove_file` / `remove_dir` / `rename`), which act on a parent dir fd +
/// a single-component name -- the SYS_WALK_CREATE / SYS_UNLINK / SYS_RENAME
/// shape -- rather than on an already-open File.
///
/// Intermediate directories are walked with `T_OPATH` (the navigation /
/// capability base, born `RIGHT_READ | RIGHT_WRITE` since kernel A-3b), the
/// same discipline `open_create_at_path` uses: `parent_fd` then carries the
/// `RIGHT_WRITE` those mutation syscalls gate the parent on, and every
/// intermediate is walkable (`RIGHT_READ`); a `T_OREAD` (`RIGHT_READ`-only)
/// parent would be rejected. Per-component the kernel checks search (X)
/// permission only (POSIX traversal), not read.
///
/// LS-4: join a relative path onto the per-Proc cwd and lexically clean it into
/// an owned absolute String. Mirrors the kernel `cwd_lexical_resolve` (the
/// kernel joins SYS_OPEN paths, but the single-hop mutation walk starts from a
/// fd, so the join is done here). `.` skipped, `//` collapsed, `..` pops the
/// last component (clamped at `/`). Reads the cwd via `env::current_dir`
/// (SYS_GETCWD), so it sees the same dot the kernel resolves opens against.
fn cwd_join_clean(rel: &Path) -> Result<String> {
    let cwd = crate::env::current_dir()?; // an absolute, already-clean path
    let mut stack: Vec<&str> = Vec::new();
    for c in Path::new(&cwd).components() {
        if let Component::Normal(s) = c {
            stack.push(s);
        }
    }
    for c in rel.components() {
        match c {
            Component::Normal(s) => stack.push(s),
            Component::ParentDir => {
                stack.pop();
            }
            Component::CurDir | Component::RootDir => {}
        }
    }
    let mut out = String::new();
    if stack.is_empty() {
        out.push('/');
    } else {
        for s in &stack {
            out.push('/');
            out.push_str(s);
        }
    }
    Ok(out)
}

/// `parent_fd` is the `T_WALK_OPEN_FROM_ROOT` sentinel for a single-component
/// path (the mutation syscalls resolve it to the caller's Territory root);
/// otherwise it is an owned handle this function closes after `f` returns --
/// on success OR error -- so `f` must not retain it past its own return.
/// `basename` is a `Normal` component (never the root, `.`, or `..`).
///
/// Path rules match `File::open`: absolute-only; `.` skipped; `..` rejected;
/// an empty path or one with no final name component (e.g. `/`) ->
/// `InvalidArgument`; a component over `T_WALK_OPEN_NAME_MAX` ->
/// `InvalidArgument`.
pub(crate) fn with_parent_dir<T>(
    path: &Path,
    f: impl FnOnce(i64, &str) -> Result<T>,
) -> Result<T> {
    if path.is_empty() {
        return Err(Error::InvalidArgument);
    }
    // LS-4: a RELATIVE mutation path resolves against the per-Proc cwd. Join +
    // lexically clean against current_dir() into an owned absolute String, then
    // proceed exactly as for an absolute path -- so ".." in the relative part is
    // resolved HERE, not rejected by the `..` filter below. (The kernel SYS_OPEN
    // join handles relative *opens*; the single-hop mutation walk starts from a
    // fd, so the join is done here. Both consult the same kernel cwd.) The holder
    // outlives the rest of the call; the kernel re-validates rights per walked dir.
    let abs_holder: String;
    let path: &Path = if path.is_relative() {
        abs_holder = cwd_join_clean(path)?;
        Path::new(&abs_holder)
    } else {
        path
    };
    // Collect the Normal components; reject `..`; skip `.` and the leading
    // root separator (mirrors open_create_at_path).
    let names: Vec<&str> = path
        .components()
        .filter_map(|c| match c {
            Component::Normal(s) => Some(Ok(s)),
            Component::ParentDir => Some(Err(Error::InvalidArgument)),
            Component::RootDir | Component::CurDir => None,
        })
        .collect::<Result<Vec<&str>>>()?;
    if names.is_empty() {
        // "/" (root + optional "."): there is no final name to act on.
        return Err(Error::InvalidArgument);
    }
    // Validate every component length up front so a reject needs no cleanup.
    for name in &names {
        if name.is_empty() || name.len() > T_WALK_OPEN_NAME_MAX {
            return Err(Error::InvalidArgument);
        }
    }
    let last_idx = names.len() - 1;

    // Walk the parent chain with T_OPATH. `parent` starts at the FROM_ROOT
    // sentinel (a kernel sentinel, never closed); each step replaces it with
    // the freshly-walked intermediate, closing the prior owned handle first
    // (regardless of this step's success).
    let mut parent: i64 = T_WALK_OPEN_FROM_ROOT;
    let mut parent_owned = false;
    for name in &names[..last_idx] {
        // SAFETY: name is a valid &str (ptr+len); parent is FROM_ROOT or a
        // Spoor fd this Proc owns; T_OPATH is in the SYS_WALK_OPEN omode mask.
        let rc = unsafe { t_walk_open(parent, name.as_ptr(), name.len(), T_OPATH) };
        if parent_owned {
            // SAFETY: prior parent was a Spoor fd this Proc owns.
            unsafe {
                let _ = t_close(parent);
            }
        }
        parent = Error::from_syscall_return(rc)?;
        parent_owned = true;
    }

    let result = f(parent, names[last_idx]);
    if parent_owned {
        // SAFETY: parent was returned by a prior SYS_WALK_OPEN on this Proc.
        unsafe {
            let _ = t_close(parent);
        }
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
