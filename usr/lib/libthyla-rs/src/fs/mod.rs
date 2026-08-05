// libthyla-rs::fs — filesystem types.
//
// Module layout per docs/UTOPIA-SHELL-DESIGN.md §15.4:
//
//   fs/
//   ├── mod.rs              free functions (metadata, exists, is_file, is_dir)
//   ├── file.rs             File RAII (U-2c-io)
//   ├── metadata.rs         Metadata (U-2c-fs)
//   ├── options.rs          OpenOptions builder (U-2c-fs)
//   ├── path.rs             Path, PathBuf (U-2c-path)
//   └── dir.rs              ReadDir, DirEntry, read_dir (U-6e-b-1, over
//                            SYS_READDIR)
//
// The fs module re-exports its sub-module types at this level so
// callers write `use libthyla_rs::fs::{Path, PathBuf, File, ...}`.

pub mod dir;
pub mod file;
pub mod metadata;
pub mod options;
pub mod path;

pub use self::dir::{read_dir, DirEntry, ReadDir};
pub use self::file::File;
pub use self::metadata::Metadata;
pub use self::options::OpenOptions;
pub use self::path::{Component, Components, Display, Path, PathBuf, SEPARATOR};

// =============================================================================
// Free functions (mirror std::fs's top-level functions).
// =============================================================================

use crate::err::{Error, Result};

/// Fetch `path`'s metadata in ONE syscall (POUNCE SYS_STAT = 88): the
/// resolution's final run is the walk-QUERY, so on the disk FS this is a
/// single fused Twalkgetattr RPC with no handle ever created. The X-search
/// authority is the path-X-only POSIX stat shape -- notably this now
/// succeeds on a file the caller may stat but not READ (the old
/// open-OREAD emulation wrongly required read access).
pub fn metadata<P: AsRef<Path>>(path: P) -> Result<Metadata> {
    metadata::stat_path(path.as_ref())
}

/// Set `path`'s permission bits (chmod). Opens with `T_OPATH` -- a navigation
/// handle, born R|W (so the SYS_WSTAT `RIGHT_WRITE` gate is satisfied) and exempt
/// from the open-time R/W perm check (so it works on a file you own but cannot
/// write, matching POSIX chmod) -- then writes only the mode (`T_WSTAT_MODE`).
/// The kernel re-runs the owner perm_check. `mode` is masked to the 9 rwx bits.
pub fn chmod<P: AsRef<Path>>(path: P, mode: u32) -> Result<()> {
    let f = File::open_stalk(path.as_ref(), crate::T_OPATH)?;
    // SAFETY: t_wstat is the SYS_WSTAT SVC wrapper; f.as_raw_fd() is a live
    // KOBJ_SPOOR; uid/gid are ignored when only T_WSTAT_MODE is set.
    let rc = unsafe {
        crate::t_wstat(
            f.as_raw_fd() as i64,
            crate::T_WSTAT_MODE,
            mode & crate::T_WSTAT_MODE_MASK,
            0,
            0,
        )
    };
    Error::from_syscall_return(rc).map(|_| ())
}

/// `true` iff `path` exists and is reachable to the caller (open
/// succeeds with read access). Returns `false` if any error occurs
/// during the probe (not just `NotFound` -- a permission failure also
/// reports the file as absent from the caller's perspective). Use
/// `metadata(path)` if you need to distinguish the error variants.
pub fn exists<P: AsRef<Path>>(path: P) -> bool {
    File::open(path).is_ok()
}

/// `true` iff `path` exists and is a regular file. Wrapping error +
/// type-bit check in one call. Returns `false` on any probe error.
pub fn is_file<P: AsRef<Path>>(path: P) -> bool {
    metadata(path).map(|m| m.is_file()).unwrap_or(false)
}

/// `true` iff `path` exists and is a directory. Returns `false` on
/// any probe error.
pub fn is_dir<P: AsRef<Path>>(path: P) -> bool {
    metadata(path).map(|m| m.is_dir()).unwrap_or(false)
}

// =============================================================================
// Mutation free functions (mirror std::fs). Each splits `path` lexically at
// its LAST component (`file::split_parent_leaf` -- nothing else is resolved
// in userspace, #87), opens the parent prefix through the kernel stalk
// resolver (`file::with_parent_fd`, T_OPATH, so the parent carries the
// RIGHT_WRITE the SYS_WALK_CREATE / SYS_UNLINK / SYS_RENAME handlers gate
// on and every `.` / `..` / non-directory prefix is gated kernel-side),
// applies its POSIX leaf row (trailing slash / `.` / `..` -- Linux-shaped,
// the #86 per-caller table one layer up), and drives the raw syscall on the
// (parent_fd, leaf) pair. NB: devramfs (the read-only boot FS) leaves the
// .create / .unlink / .rename Dev slots NULL, so these succeed only on the
// Stratum-backed (dev9p) FS reachable after the pivot.
// =============================================================================

use self::file::Leaf;
use alloc_crate::string::String;

/// Default POSIX mode for a directory created via `create_dir`: `rwxr-xr-x`.
const CREATE_DIR_MODE: u32 = 0o755;

/// Create the directory `path` (mode 0755). The parent directory must already
/// exist -- there is no `create_dir_all` at v1.0; the `mkdir -p` behavior is a
/// caller-side prefix loop. Mirrors `std::fs::create_dir`.
///
/// Leaf rows (#87, Linux-shaped): a trailing slash is legal and strips (the
/// created leaf IS a directory -- `mkdir("d/")` creates `d`); `mkdir("/")`
/// and a `.` / `..` leaf answer `Exists` (Linux `filename_create` rejects a
/// non-NORM last component with EEXIST).
///
/// Errors: `Exists` if `path` is already present (the kernel create is
/// exclusive); `NotFound` if a parent component is missing; `NotADirectory`
/// if a parent component is not a directory; `PermissionDenied` (or another
/// kernel error) when the parent denies creation or the FS is read-only.
pub fn create_dir<P: AsRef<Path>>(path: P) -> Result<()> {
    let sp = file::split_parent_leaf(path.as_ref())?;
    file::with_parent_fd(sp.parent, |parent| {
        let name = match sp.leaf {
            Leaf::Root | Leaf::Dot | Leaf::DotDot => return Err(Error::Exists),
            Leaf::Name(n) => n,
        };
        // A trailing slash needs no probe: the leaf being created IS a
        // directory (sp.dir_required strips silently).
        // SAFETY: name is a valid &str (ptr+len); parent is FROM_ROOT or an
        // owned dir handle carrying RIGHT_WRITE (T_OPATH). DMDIR selects a
        // directory; OREAD is the create-time omode for a directory (the
        // kernel SYS_WALK_CREATE contract + joey's mkdir_or_open idiom).
        let rc = unsafe {
            crate::t_walk_create(
                parent,
                name.as_ptr(),
                name.len(),
                crate::T_OREAD,
                crate::T_WALK_CREATE_DMDIR | CREATE_DIR_MODE,
            )
        };
        let fd = Error::from_syscall_return(rc)?;
        // mkdir does not keep the new directory open; close the returned fd.
        // SAFETY: fd is a live KOBJ_SPOOR handle this Proc just received.
        unsafe {
            let _ = crate::t_close(fd);
        }
        Ok(())
    })
}

/// Remove the file `path` (a non-directory). Mirrors `std::fs::remove_file`.
/// `NotFound` if absent; the kernel rejects a directory target here (use
/// `remove_dir`).
///
/// Leaf rows (#87, Linux `do_unlinkat`): a `.` / `..` leaf (and `/` itself)
/// answers `IsADirectory` -- the parent open has already answered
/// `NotADirectory` when the prefix walks through a non-directory, matching
/// Linux's parent-walk-first ordering. A trailing slash must NEVER reach
/// SYS_UNLINK (stripped, it would delete the very file the slash asserts is
/// a directory): one stat of the slash-terminated path -- the kernel's
/// audited #82 gate -- discriminates. `Ok` means it IS a directory
/// (`IsADirectory`, the unlink-of-a-directory answer); an error is the real
/// `NotADirectory` / `NotFound` / `PermissionDenied`.
pub fn remove_file<P: AsRef<Path>>(path: P) -> Result<()> {
    let path = path.as_ref();
    let sp = file::split_parent_leaf(path)?;
    file::with_parent_fd(sp.parent, |parent| {
        let name = match sp.leaf {
            Leaf::Root | Leaf::Dot | Leaf::DotDot => return Err(Error::IsADirectory),
            Leaf::Name(n) => n,
        };
        if sp.dir_required {
            return match metadata(path) {
                Ok(_) => Err(Error::IsADirectory),
                Err(e) => Err(e),
            };
        }
        // SAFETY: name is a valid &str (ptr+len); parent is FROM_ROOT or an
        // owned dir handle carrying RIGHT_WRITE.
        let rc = unsafe { crate::t_unlink(parent, name.as_ptr(), name.len(), 0) };
        Error::from_syscall_return(rc).map(|_| ())
    })
}

/// Remove the empty directory `path`. Mirrors `std::fs::remove_dir`. The
/// directory must be empty (the kernel/9P rmdir rejects a non-empty dir);
/// recursive removal is the caller's walk (e.g. a coreutil `rm -r`).
///
/// Leaf rows (#87): `rmdir("/")` -> `Busy` (Linux EBUSY); a `.` leaf ->
/// `InvalidArgument` (POSIX-mandated EINVAL); a `..` leaf ->
/// `DirectoryNotEmpty` (Linux ENOTEMPTY). A trailing slash strips silently:
/// REMOVEDIR itself enforces dir-ness (the server answers `NotADirectory`
/// for a file; #80 carries the real errno across).
pub fn remove_dir<P: AsRef<Path>>(path: P) -> Result<()> {
    let sp = file::split_parent_leaf(path.as_ref())?;
    file::with_parent_fd(sp.parent, |parent| {
        let name = match sp.leaf {
            Leaf::Root => return Err(Error::Busy),
            Leaf::Dot => return Err(Error::InvalidArgument),
            Leaf::DotDot => return Err(Error::DirectoryNotEmpty),
            Leaf::Name(n) => n,
        };
        // SAFETY: name is a valid &str (ptr+len); parent is FROM_ROOT or an
        // owned dir handle carrying RIGHT_WRITE.
        let rc = unsafe {
            crate::t_unlink(parent, name.as_ptr(), name.len(), crate::T_UNLINK_REMOVEDIR)
        };
        Error::from_syscall_return(rc).map(|_| ())
    })
}

/// Rename `from` to `to`, atomically replacing an existing `to` (POSIX rename
/// / 9P Trenameat). Mirrors `std::fs::rename`.
///
/// Both paths must resolve within the SAME Dev (one 9P server): the kernel
/// rejects a cross-Dev rename (a 9P renameat is within a single session). At
/// v1.0 the whole pivoted FS is one Stratum Dev, so any in-tree rename
/// qualifies; a cross-Dev `mv` is the caller's copy+remove fallback. `from`'s
/// and `to`'s parent directories are held open simultaneously for the rename.
///
/// Leaf rows (#87, Linux `do_renameat2`): renaming `/` or a `.` / `..` leaf
/// (either side) answers `Busy` (the LAST_NORM rule). A trailing slash on
/// EITHER name rides the verbatim rule -- "unless the source is a directory,
/// trailing slashes give -ENOTDIR" -- via ONE stat of the slash-terminated
/// SOURCE (the kernel's audited #82 gate): `Ok` means the source is a
/// directory, so the stripped names proceed; an error is the real
/// `NotADirectory` (a file) / `NotFound` (absent, which precedes the
/// slash verdict, as in Linux) / `PermissionDenied`.
pub fn rename<P: AsRef<Path>, Q: AsRef<Path>>(from: P, to: Q) -> Result<()> {
    let from = from.as_ref();
    let f_sp = file::split_parent_leaf(from)?;
    let t_sp = file::split_parent_leaf(to.as_ref())?;
    file::with_parent_fd(f_sp.parent, |old_parent| {
        let old_name = match f_sp.leaf {
            Leaf::Root | Leaf::Dot | Leaf::DotDot => return Err(Error::Busy),
            Leaf::Name(n) => n,
        };
        file::with_parent_fd(t_sp.parent, |new_parent| {
            let new_name = match t_sp.leaf {
                Leaf::Root | Leaf::Dot | Leaf::DotDot => return Err(Error::Busy),
                Leaf::Name(n) => n,
            };
            if f_sp.dir_required || t_sp.dir_required {
                let mut probe = String::from(from.as_str());
                if !probe.ends_with(SEPARATOR) {
                    probe.push(SEPARATOR);
                }
                metadata(&probe)?;
            }
            // SAFETY: both names are valid &str (ptr+len); both parents are
            // FROM_ROOT or owned dir handles carrying RIGHT_WRITE, held open
            // across the rename (the inner with_parent_fd nests inside the
            // outer's still-open parent).
            let rc = unsafe {
                crate::t_rename(
                    old_parent,
                    old_name.as_ptr(),
                    old_name.len(),
                    new_parent,
                    new_name.as_ptr(),
                    new_name.len(),
                )
            };
            Error::from_syscall_return(rc).map(|_| ())
        })
    })
}
