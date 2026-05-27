// libthyla-rs::fs::path — Path + PathBuf.
//
// Two types, std::path-shaped:
//
//   `Path` is the borrowed view of a filesystem path. Unsized; lives
//   behind `&Path`. `#[repr(transparent)]` over `str` so the cast
//   between `&str` and `&Path` is a no-op at runtime.
//
//   `PathBuf` is the owned, heap-allocated counterpart. Wraps `String`.
//   `Deref<Target=Path>` lets PathBuf forward every Path method.
//
// Foundation chunk: U-2c-path per docs/UTOPIA-SHELL-DESIGN.md §15
// (split from U-2c into U-2c-path / U-2c-io / U-2c-fs sub-chunks).
//
// PATH ENCODING (v1.0 choice):
//   - Paths are `str` (UTF-8 guaranteed). Plan 9 + Unix tradition is
//     bytes; Thylacine convention is UTF-8 across the board (joey,
//     corvus, stratumd all UTF-8). Storing as `str` simplifies every
//     interop boundary (Display, Debug, From<&str>, From<String>,
//     Hash). Non-UTF-8 paths are a v1.x consideration -- we'd add
//     `PathBytes` / `PathBytesBuf` alongside, like `OsStr` / `OsString`
//     in std.
//
// SEPARATOR:
//   - Forward slash `/`. The only path separator. Plan 9 + Unix; no
//     Windows backslash duality.
//
// COMPONENTS:
//   - A leading `/` is the root component.
//   - `.` and `..` are recognised as `CurDir` / `ParentDir`; other
//     non-empty segments are `Normal`.
//   - Empty segments (consecutive slashes, trailing slash) are
//     skipped by the Components iterator; an absolute path's first
//     non-root component is the directory under root.
//
// API DISCIPLINE:
//   - Mirror std::path API where it makes sense. Method names match
//     std exactly. Differences are documented at the relevant method.
//   - Skip Windows-specific concepts (Prefix component, verbatim
//     paths, drive letters).
//   - Skip OsStr machinery; v1 is str-only.

use alloc_crate::borrow::ToOwned;
use alloc_crate::string::{String, ToString};
use core::borrow::Borrow;
use core::cmp::Ordering;
use core::fmt;
use core::hash::{Hash, Hasher};
use core::ops::Deref;

/// The platform separator. `/` on Thylacine. Always.
pub const SEPARATOR: char = '/';

// =============================================================================
// Path — borrowed.
// =============================================================================

/// A slice of a path (analogous to `str` for `String`).
///
/// `Path` is unsized; it always lives behind `&Path` or in `Box<Path>`.
/// Build a `&Path` from a `&str` via `Path::new(s)`. The cast is free
/// at runtime (`#[repr(transparent)]`).
///
/// Examples:
///
/// ```ignore
/// use libthyla_rs::fs::Path;
/// let p = Path::new("/etc/hosts");
/// assert_eq!(p.file_name(), Some("hosts"));
/// assert_eq!(p.parent().map(|p| p.as_str()), Some("/etc"));
/// ```
#[repr(transparent)]
pub struct Path {
    inner: str,
}

impl Path {
    /// Wraps a string slice as a `&Path`.
    ///
    /// Free at runtime: the returned reference points to the same bytes
    /// as the input. Lifetime of the returned `&Path` is the input's.
    #[inline]
    pub fn new<S: AsRef<str> + ?Sized>(s: &S) -> &Path {
        // SAFETY: `Path` is `#[repr(transparent)]` over `str`, so the
        // pointer cast is sound and preserves the lifetime via the
        // outer reference.
        unsafe { &*(s.as_ref() as *const str as *const Path) }
    }

    /// The underlying string slice.
    #[inline]
    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.inner
    }

    /// True iff the path begins with the root separator `/`.
    #[inline]
    #[must_use]
    pub fn is_absolute(&self) -> bool {
        self.inner.starts_with(SEPARATOR)
    }

    /// True iff the path does not begin with the root separator.
    #[inline]
    #[must_use]
    pub fn is_relative(&self) -> bool {
        !self.is_absolute()
    }

    /// `true` for an empty path. (Thylacine convention: the empty path
    /// is distinct from `.`; most ops on an empty path are no-ops.)
    #[inline]
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }

    /// Returns the parent directory's path, or `None` if `self` is the
    /// root or empty.
    ///
    /// Examples:
    ///
    /// - `Path::new("/etc/hosts").parent() == Some(Path::new("/etc"))`
    /// - `Path::new("/etc").parent() == Some(Path::new("/"))`
    /// - `Path::new("/").parent() == None`
    /// - `Path::new("foo").parent() == Some(Path::new(""))`
    /// - `Path::new("").parent() == None`
    #[must_use]
    pub fn parent(&self) -> Option<&Path> {
        if self.is_empty() {
            return None;
        }
        // The root `/` has no parent.
        if &self.inner == "/" {
            return None;
        }
        // Find the last separator. Everything before it is the parent.
        // Trailing separators are stripped before the search.
        let trimmed = self.inner.trim_end_matches(SEPARATOR);
        if trimmed.is_empty() {
            // Was all separators ("///" -> ""): treat as root.
            return None;
        }
        match trimmed.rfind(SEPARATOR) {
            Some(0) => Some(Path::new("/")),
            Some(idx) => Some(Path::new(&trimmed[..idx])),
            None => Some(Path::new("")),
        }
    }

    /// Returns the final component of the path -- the file or
    /// directory name -- as a `&str`, or `None` if the path is empty
    /// or ends in `..`.
    ///
    /// Examples:
    ///
    /// - `Path::new("/etc/hosts").file_name() == Some("hosts")`
    /// - `Path::new("/").file_name() == None`
    /// - `Path::new("foo").file_name() == Some("foo")`
    #[must_use]
    pub fn file_name(&self) -> Option<&str> {
        let trimmed = self.inner.trim_end_matches(SEPARATOR);
        if trimmed.is_empty() {
            return None;
        }
        let name = match trimmed.rfind(SEPARATOR) {
            Some(idx) => &trimmed[idx + 1..],
            None => trimmed,
        };
        if name == ".." || name == "." {
            None
        } else {
            Some(name)
        }
    }

    /// Returns the stem portion of the file name -- everything before
    /// the final `.`, or the whole file name if it begins with `.` or
    /// has no extension.
    ///
    /// Examples:
    ///
    /// - `Path::new("/etc/hosts").file_stem() == Some("hosts")`
    /// - `Path::new("foo.tar.gz").file_stem() == Some("foo.tar")`
    /// - `Path::new(".bashrc").file_stem() == Some(".bashrc")`
    #[must_use]
    pub fn file_stem(&self) -> Option<&str> {
        let name = self.file_name()?;
        match name.rfind('.') {
            // A leading `.` (and no other dots) -> the whole name is
            // the stem.
            Some(0) => Some(name),
            Some(idx) => Some(&name[..idx]),
            None => Some(name),
        }
    }

    /// Returns the extension portion of the file name -- everything
    /// after the final `.`, or `None` if the name has no extension or
    /// begins with `.` with no other dots.
    ///
    /// Examples:
    ///
    /// - `Path::new("/etc/hosts").extension() == None`
    /// - `Path::new("foo.tar.gz").extension() == Some("gz")`
    /// - `Path::new(".bashrc").extension() == None`
    #[must_use]
    pub fn extension(&self) -> Option<&str> {
        let name = self.file_name()?;
        match name.rfind('.') {
            Some(0) | None => None,
            Some(idx) => Some(&name[idx + 1..]),
        }
    }

    /// Joins `self` with `other`, producing a new `PathBuf`.
    ///
    /// If `other` is absolute, it replaces `self` entirely (std::path
    /// semantics). Otherwise a separator is inserted between `self`
    /// and `other` (unless `self` already ends in one or is empty).
    #[must_use]
    pub fn join<P: AsRef<Path>>(&self, other: P) -> PathBuf {
        let mut buf = self.to_path_buf();
        buf.push(other);
        buf
    }

    /// Returns `true` if `self` begins with the components of `base`.
    #[must_use]
    pub fn starts_with<P: AsRef<Path>>(&self, base: P) -> bool {
        let base = base.as_ref().as_str();
        if base.is_empty() {
            return true;
        }
        if let Some(stripped) = self.inner.strip_prefix(base) {
            // The match is component-aligned only if the rest starts
            // with a separator OR is exactly empty (full match) OR
            // base ends in a separator.
            stripped.is_empty()
                || stripped.starts_with(SEPARATOR)
                || base.ends_with(SEPARATOR)
        } else {
            false
        }
    }

    /// Returns `true` if `self` ends with the components of `child`.
    #[must_use]
    pub fn ends_with<P: AsRef<Path>>(&self, child: P) -> bool {
        let child = child.as_ref().as_str();
        if child.is_empty() {
            return true;
        }
        if let Some(stripped) = self.inner.strip_suffix(child) {
            stripped.is_empty() || stripped.ends_with(SEPARATOR)
        } else {
            false
        }
    }

    /// Clones the path into an owned `PathBuf`.
    #[inline]
    #[must_use]
    pub fn to_path_buf(&self) -> PathBuf {
        PathBuf {
            inner: self.inner.to_string(),
        }
    }

    /// Returns an iterator over path components.
    ///
    /// See [`Components`] for the variants. Skips empty segments (so
    /// `///foo//bar/` iterates as `RootDir, Normal("foo"), Normal("bar")`).
    #[inline]
    pub fn components(&self) -> Components<'_> {
        Components::new(&self.inner)
    }

    /// Returns a wrapper that implements `Display`. Equivalent to
    /// formatting `self.as_str()` directly.
    #[inline]
    pub fn display(&self) -> Display<'_> {
        Display { inner: &self.inner }
    }
}

impl AsRef<Path> for Path {
    #[inline]
    fn as_ref(&self) -> &Path {
        self
    }
}

impl AsRef<Path> for str {
    #[inline]
    fn as_ref(&self) -> &Path {
        Path::new(self)
    }
}

// (`AsRef<Path> for &str` is provided by core's blanket
// `impl<T: ?Sized, U: ?Sized> AsRef<U> for &T where T: AsRef<U>`.
// Declaring it here would conflict.)

impl AsRef<Path> for String {
    #[inline]
    fn as_ref(&self) -> &Path {
        Path::new(self.as_str())
    }
}

impl AsRef<str> for Path {
    #[inline]
    fn as_ref(&self) -> &str {
        &self.inner
    }
}

impl PartialEq for Path {
    #[inline]
    fn eq(&self, other: &Path) -> bool {
        self.inner == other.inner
    }
}

impl Eq for Path {}

impl PartialOrd for Path {
    #[inline]
    fn partial_cmp(&self, other: &Path) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Path {
    #[inline]
    fn cmp(&self, other: &Path) -> Ordering {
        self.inner.cmp(&other.inner)
    }
}

impl Hash for Path {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.inner.hash(state);
    }
}

impl fmt::Debug for Path {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Path({:?})", &self.inner)
    }
}

impl ToOwned for Path {
    type Owned = PathBuf;
    #[inline]
    fn to_owned(&self) -> PathBuf {
        self.to_path_buf()
    }
}

// =============================================================================
// PathBuf — owned.
// =============================================================================

/// An owned, mutable filesystem path (the `String` to Path's `str`).
///
/// `Deref<Target=Path>` forwards every Path method.
#[derive(Clone)]
pub struct PathBuf {
    inner: String,
}

impl PathBuf {
    /// Allocates an empty `PathBuf`.
    #[inline]
    #[must_use]
    pub fn new() -> PathBuf {
        PathBuf {
            inner: String::new(),
        }
    }

    /// Constructs a `PathBuf` from the given string-like value.
    ///
    /// Indirect via `From<S>` where `S: Into<String>`; provided as a
    /// method for ergonomic call sites.
    #[inline]
    #[must_use]
    pub fn from_str<S: Into<String>>(s: S) -> PathBuf {
        PathBuf { inner: s.into() }
    }

    /// Allocates a `PathBuf` with `capacity` bytes pre-reserved.
    #[inline]
    #[must_use]
    pub fn with_capacity(capacity: usize) -> PathBuf {
        PathBuf {
            inner: String::with_capacity(capacity),
        }
    }

    /// Appends `path` to `self`.
    ///
    /// If `path` is absolute, replaces `self` entirely. Otherwise
    /// inserts a separator (unless `self` is empty or already ends in
    /// one) and then the path's string.
    pub fn push<P: AsRef<Path>>(&mut self, path: P) {
        let path = path.as_ref().as_str();
        if path.is_empty() {
            return;
        }
        if path.starts_with(SEPARATOR) {
            // Absolute -> replace.
            self.inner.clear();
            self.inner.push_str(path);
            return;
        }
        if !self.inner.is_empty() && !self.inner.ends_with(SEPARATOR) {
            self.inner.push(SEPARATOR);
        }
        self.inner.push_str(path);
    }

    /// Removes the last component. Returns `false` if there was no
    /// component to remove (path was empty or root).
    pub fn pop(&mut self) -> bool {
        // Locate the parent via Path::parent; if it differs from
        // self, truncate to the parent's length.
        let parent_len = match Path::new(&self.inner).parent() {
            Some(p) => p.inner.len(),
            None => return false,
        };
        self.inner.truncate(parent_len);
        true
    }

    /// Replaces the final file name with `file_name`. If self has no
    /// file name, behaves like `push`.
    pub fn set_file_name<S: AsRef<str>>(&mut self, file_name: S) {
        if self.as_path().file_name().is_some() {
            self.pop();
        }
        self.push(Path::new(file_name.as_ref()));
    }

    /// Sets the extension. Returns `false` if there is no file_name
    /// to attach an extension to.
    ///
    /// If `extension` is empty, the trailing `.` is also removed.
    pub fn set_extension<S: AsRef<str>>(&mut self, extension: S) -> bool {
        let ext = extension.as_ref();
        let stem_len = {
            let p = self.as_path();
            match (p.file_name(), p.extension()) {
                (Some(_name), Some(old_ext)) => {
                    // Length of name without ".<old_ext>".
                    self.inner.len() - old_ext.len() - 1
                }
                (Some(_name), None) => self.inner.len(),
                (None, _) => return false,
            }
        };
        self.inner.truncate(stem_len);
        if !ext.is_empty() {
            self.inner.push('.');
            self.inner.push_str(ext);
        }
        true
    }

    /// Returns a borrowed `&Path` view of this PathBuf.
    #[inline]
    #[must_use]
    pub fn as_path(&self) -> &Path {
        Path::new(self.inner.as_str())
    }

    /// Consumes the PathBuf and returns the underlying `String`.
    #[inline]
    #[must_use]
    pub fn into_string(self) -> String {
        self.inner
    }

    /// Truncates the PathBuf to zero length, releasing no capacity.
    #[inline]
    pub fn clear(&mut self) {
        self.inner.clear();
    }
}

impl Default for PathBuf {
    #[inline]
    fn default() -> PathBuf {
        PathBuf::new()
    }
}

impl Deref for PathBuf {
    type Target = Path;
    #[inline]
    fn deref(&self) -> &Path {
        self.as_path()
    }
}

impl Borrow<Path> for PathBuf {
    #[inline]
    fn borrow(&self) -> &Path {
        self.as_path()
    }
}

impl AsRef<Path> for PathBuf {
    #[inline]
    fn as_ref(&self) -> &Path {
        self.as_path()
    }
}

impl AsRef<str> for PathBuf {
    #[inline]
    fn as_ref(&self) -> &str {
        self.inner.as_str()
    }
}

impl From<&str> for PathBuf {
    #[inline]
    fn from(s: &str) -> PathBuf {
        PathBuf {
            inner: s.to_string(),
        }
    }
}

impl From<String> for PathBuf {
    #[inline]
    fn from(s: String) -> PathBuf {
        PathBuf { inner: s }
    }
}

impl From<&Path> for PathBuf {
    #[inline]
    fn from(p: &Path) -> PathBuf {
        p.to_path_buf()
    }
}

impl PartialEq for PathBuf {
    #[inline]
    fn eq(&self, other: &PathBuf) -> bool {
        self.inner == other.inner
    }
}

impl Eq for PathBuf {}

impl PartialEq<Path> for PathBuf {
    #[inline]
    fn eq(&self, other: &Path) -> bool {
        self.as_path() == other
    }
}

impl PartialEq<PathBuf> for Path {
    #[inline]
    fn eq(&self, other: &PathBuf) -> bool {
        self == other.as_path()
    }
}

impl PartialOrd for PathBuf {
    #[inline]
    fn partial_cmp(&self, other: &PathBuf) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for PathBuf {
    #[inline]
    fn cmp(&self, other: &PathBuf) -> Ordering {
        self.inner.cmp(&other.inner)
    }
}

impl Hash for PathBuf {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.inner.hash(state);
    }
}

impl fmt::Debug for PathBuf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PathBuf({:?})", &self.inner)
    }
}

// =============================================================================
// Components iterator.
// =============================================================================

/// A path component.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub enum Component<'a> {
    /// The leading `/` of an absolute path.
    RootDir,
    /// `.`
    CurDir,
    /// `..`
    ParentDir,
    /// An ordinary name segment.
    Normal(&'a str),
}

impl<'a> Component<'a> {
    /// Returns the textual form of this component.
    #[must_use]
    pub fn as_str(&self) -> &'a str {
        match *self {
            Component::RootDir => "/",
            Component::CurDir => ".",
            Component::ParentDir => "..",
            Component::Normal(s) => s,
        }
    }
}

/// Iterator over a path's components.
pub struct Components<'a> {
    rest: &'a str,
    // Whether we have yet to yield the leading RootDir component (for
    // absolute paths).
    pending_root: bool,
}

impl<'a> Components<'a> {
    fn new(s: &'a str) -> Components<'a> {
        if let Some(rest) = s.strip_prefix(SEPARATOR) {
            Components {
                rest,
                pending_root: true,
            }
        } else {
            Components {
                rest: s,
                pending_root: false,
            }
        }
    }
}

impl<'a> Iterator for Components<'a> {
    type Item = Component<'a>;

    fn next(&mut self) -> Option<Component<'a>> {
        if self.pending_root {
            self.pending_root = false;
            return Some(Component::RootDir);
        }
        loop {
            if self.rest.is_empty() {
                return None;
            }
            let (segment, rest_after) = match self.rest.find(SEPARATOR) {
                Some(idx) => (&self.rest[..idx], &self.rest[idx + 1..]),
                None => (self.rest, ""),
            };
            self.rest = rest_after;
            if segment.is_empty() {
                // Consecutive or trailing separators; skip.
                continue;
            }
            return Some(match segment {
                "." => Component::CurDir,
                ".." => Component::ParentDir,
                s => Component::Normal(s),
            });
        }
    }
}

// =============================================================================
// Display wrapper.
// =============================================================================

/// Wrapper that implements `Display` for a `Path`. Returned by
/// `Path::display`.
pub struct Display<'a> {
    inner: &'a str,
}

impl<'a> fmt::Display for Display<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.inner)
    }
}

// =============================================================================
// Display for PathBuf delegates through Path.
// =============================================================================

impl fmt::Display for Path {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.inner)
    }
}

impl fmt::Display for PathBuf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.inner)
    }
}
