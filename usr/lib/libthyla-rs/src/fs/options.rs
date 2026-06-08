// libthyla-rs::fs::options — OpenOptions builder.
//
// Composable file-open mode selection. Mirror of std::fs::OpenOptions
// scoped to what Thylacine's SYS_WALK_OPEN omode field accepts at v1.0.
//
// Foundation chunk: U-2c-fs per docs/UTOPIA-SHELL-DESIGN.md §15.
//
// V1.0 CAPABILITY SURFACE:
//   - read=true alone -> OREAD
//   - write=true alone -> OWRITE (plus OTRUNC if truncate=true)
//   - read=true + write=true -> ORDWR (plus OTRUNC if truncate=true)
//   - truncate=true requires write=true (POSIX semantics)
//   - create / create_new -> SYS_WALK_CREATE (the FS-alpha surface) via
//     File::open_create_at_path (U-6d-b). `create` is create-or-open;
//     `create_new` is exclusive. The new file's POSIX mode is `.mode(m)`
//     (default 0644).
//   - append -> the file is opened/created for write and positioned at
//     end (Plan 9 omode has no O_APPEND; this seek-to-end-at-open is the
//     single-writer approximation -- exact for the shell-redirect case,
//     concurrent atomic-append is a v1.x kernel surface).
//
// CONSTRAINTS:
//   - truncate / append / create_new require write.
//   - append + truncate together is rejected (contradictory).
//   - NB: devramfs is read-only at v1.0, so create only succeeds on the
//     dev9p (Stratum-backed) FS; on devramfs the create leg errors.

use crate::err::{Error, Result};
use crate::{T_ORDWR, T_OREAD, T_OTRUNC, T_OWRITE};

use super::file::File;
use super::path::Path;

/// Default POSIX mode for a file created via `OpenOptions` when
/// `.mode()` is not called: `rw-r--r--`.
const DEFAULT_CREATE_MODE: u32 = 0o644;

/// Builder for fine-grained file-open semantics.
///
/// Construct with `OpenOptions::new()`, chain setters, then call
/// `.open(path)`. Mirror of `std::fs::OpenOptions`.
///
/// Example:
///
/// ```ignore
/// use libthyla_rs::fs::OpenOptions;
///
/// let f = OpenOptions::new()
///     .read(true)
///     .write(true)
///     .open("/etc/hosts")?;
/// ```
#[derive(Copy, Clone, Debug)]
pub struct OpenOptions {
    read: bool,
    write: bool,
    truncate: bool,
    append: bool,
    create: bool,
    create_new: bool,
    mode: u32,
}

impl OpenOptions {
    /// A fresh builder with every flag at its default (`false`) and the
    /// create mode at 0644. At least one of `read` / `write` must be set
    /// before `.open()`.
    #[inline]
    #[must_use]
    pub const fn new() -> Self {
        OpenOptions {
            read: false,
            write: false,
            truncate: false,
            append: false,
            create: false,
            create_new: false,
            mode: DEFAULT_CREATE_MODE,
        }
    }

    /// Open for read access.
    #[inline]
    #[must_use]
    pub const fn read(mut self, b: bool) -> Self {
        self.read = b;
        self
    }

    /// Open for write access. (Plan 9's OWRITE doesn't grant read; use
    /// `.read(true).write(true)` for read+write access.)
    #[inline]
    #[must_use]
    pub const fn write(mut self, b: bool) -> Self {
        self.write = b;
        self
    }

    /// Truncate to zero length on open. Requires `write(true)`;
    /// otherwise `.open()` returns `Error::InvalidArgument`.
    #[inline]
    #[must_use]
    pub const fn truncate(mut self, b: bool) -> Self {
        self.truncate = b;
        self
    }

    /// Append-mode writes (each write goes to the end of file).
    ///
    /// **V1.0: not implemented at the kernel layer.** Returning
    /// `Error::NotImplemented` from `.open()` if set. Documented
    /// here so callers can plan for it.
    #[inline]
    #[must_use]
    pub const fn append(mut self, b: bool) -> Self {
        self.append = b;
        self
    }

    /// Create the file if it doesn't exist (opens otherwise).
    ///
    /// **V1.0: not implemented (no kernel `SYS_TLCREATE`).** Returns
    /// `Error::NotImplemented` from `.open()` if set. Method is
    /// available for forward-compat so callers can write the
    /// canonical create-or-open pattern today.
    #[inline]
    #[must_use]
    pub const fn create(mut self, b: bool) -> Self {
        self.create = b;
        self
    }

    /// Create the file only if it doesn't already exist; error if it
    /// does. Same v1.0 limitation as `create`.
    #[inline]
    #[must_use]
    pub const fn create_new(mut self, b: bool) -> Self {
        self.create_new = b;
        self
    }

    /// Set the POSIX mode bits for a file created via `create` /
    /// `create_new`. Ignored when no creation happens. Default 0644.
    #[inline]
    #[must_use]
    pub const fn mode(mut self, m: u32) -> Self {
        self.mode = m;
        self
    }

    /// Open `path` using the current settings.
    pub fn open<P: AsRef<Path>>(self, path: P) -> Result<File> {
        // Read-or-write must be requested.
        let omode = match (self.read, self.write) {
            (true, true) => T_ORDWR,
            (true, false) => T_OREAD,
            (false, true) => T_OWRITE,
            (false, false) => return Err(Error::InvalidArgument),
        };

        // truncate / append / create_new require write; append+truncate
        // is contradictory.
        if self.truncate && !self.write {
            return Err(Error::InvalidArgument);
        }
        if self.append && !self.write {
            return Err(Error::InvalidArgument);
        }
        if self.create_new && !self.write {
            return Err(Error::InvalidArgument);
        }
        if self.append && self.truncate {
            return Err(Error::InvalidArgument);
        }

        // append never truncates (it positions at end below).
        let base_omode = if self.truncate {
            omode | T_OTRUNC
        } else {
            omode
        };

        let mut file = File::open_create_at_path(
            path.as_ref(),
            base_omode,
            self.create,
            self.create_new,
            self.mode,
        )?;

        if self.append {
            // Position at end so subsequent writes append. (Plan 9 omode
            // has no O_APPEND; this seek-to-end-at-open is the
            // single-writer approximation -- module header.)
            use crate::io::{Seek, SeekFrom};
            file.seek(SeekFrom::End(0))?;
        }

        Ok(file)
    }
}

impl Default for OpenOptions {
    #[inline]
    fn default() -> OpenOptions {
        OpenOptions::new()
    }
}
