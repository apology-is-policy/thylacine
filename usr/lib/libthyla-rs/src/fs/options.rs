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
//
// NOT YET SUPPORTED (kernel surface doesn't exist; methods either
// return error at .open() or warn at compile time of intent):
//   - create -- needs a SYS_TLCREATE-equivalent. Will land alongside
//     the directory-create + file-create surface. Until then, calling
//     `.create(true).open(...)` returns `Error::NotImplemented`.
//   - create_new -- same.
//   - append -- Plan 9's open flags don't include O_APPEND directly;
//     append-mode writes need an explicit lseek-then-write loop or
//     a new kernel flag. Returns `Error::NotImplemented` if requested.
//   - custom permission bits at create -- needs the create surface.

use crate::err::{Error, Result};
use crate::{T_ORDWR, T_OREAD, T_OTRUNC, T_OWRITE};

use super::file::File;
use super::path::Path;

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
}

impl OpenOptions {
    /// A fresh builder with every flag at its default (`false`).
    /// At least one of `read` / `write` must be set before `.open()`.
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

    /// Open `path` using the current settings.
    pub fn open<P: AsRef<Path>>(self, path: P) -> Result<File> {
        // V1.0 unsupported flag rejections.
        if self.create || self.create_new {
            return Err(Error::NotImplemented);
        }
        if self.append {
            return Err(Error::NotImplemented);
        }

        // Read-or-write must be requested.
        let omode = match (self.read, self.write) {
            (true, true) => T_ORDWR,
            (true, false) => T_OREAD,
            (false, true) => T_OWRITE,
            (false, false) => return Err(Error::InvalidArgument),
        };

        // truncate requires write.
        if self.truncate && !self.write {
            return Err(Error::InvalidArgument);
        }

        let omode = if self.truncate {
            omode | T_OTRUNC
        } else {
            omode
        };

        File::open_with_omode(path.as_ref(), omode)
    }
}

impl Default for OpenOptions {
    #[inline]
    fn default() -> OpenOptions {
        OpenOptions::new()
    }
}
