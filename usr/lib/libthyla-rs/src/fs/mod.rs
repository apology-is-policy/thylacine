// libthyla-rs::fs — filesystem types.
//
// Module layout per docs/UTOPIA-SHELL-DESIGN.md §15.4:
//
//   fs/
//   ├── mod.rs              File, Metadata, OpenOptions (U-2c-io / U-2c-fs)
//   ├── path.rs             Path, PathBuf (U-2c-path -- this chunk)
//   └── dir.rs              ReadDir, DirEntry (U-2c-fs)
//
// The fs module re-exports its sub-module types at this level so
// callers write `use libthyla_rs::fs::{Path, PathBuf, File, ...}`.

pub mod file;
pub mod path;

pub use self::file::File;
pub use self::path::{Component, Components, Display, Path, PathBuf, SEPARATOR};
