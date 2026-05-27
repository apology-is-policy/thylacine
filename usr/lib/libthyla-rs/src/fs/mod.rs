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
//   └── dir.rs              ReadDir, DirEntry (deferred -- kernel
//                            directory-read mechanism not yet exposed)
//
// The fs module re-exports its sub-module types at this level so
// callers write `use libthyla_rs::fs::{Path, PathBuf, File, ...}`.

pub mod file;
pub mod metadata;
pub mod options;
pub mod path;

pub use self::file::File;
pub use self::metadata::Metadata;
pub use self::options::OpenOptions;
pub use self::path::{Component, Components, Display, Path, PathBuf, SEPARATOR};

// =============================================================================
// Free functions (mirror std::fs's top-level functions).
// =============================================================================

use crate::err::Result;

/// Fetch `path`'s metadata. Opens the file with OREAD, calls fstat,
/// closes the handle. Convenience for callers that don't otherwise
/// need a `File`.
pub fn metadata<P: AsRef<Path>>(path: P) -> Result<Metadata> {
    let file = File::open(path)?;
    file.metadata()
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
