// A shared folder's tree, reached only beneath its root (4.4): every step of every path is its own
// openat with O_NOFOLLOW_ANY, so no host symbolic link is followed and no `..` is ever sent; a link
// is served as a link. The app does the I/O as the Mac user.
use std::ffi::CString;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::Path;

use super::errno;

/// A path below a share's root: one name per element, none empty, none `.` or `..`, none holding
/// `/` or NUL. Only `name_ok` admits an element.
pub type Rel = Vec<Vec<u8>>;

/// Is `name` one element of a path?
pub fn name_ok(name: &[u8]) -> bool {
    !name.is_empty() && name != b"." && name != b".." && !name.contains(&b'/') && !name.contains(&0)
}

fn cname(name: &[u8]) -> Result<CString, u32> {
    CString::new(name).map_err(|_| ninep::E_INVAL)
}

fn check(rc: libc::c_int) -> Result<libc::c_int, u32> {
    if rc < 0 {
        Err(errno::last())
    } else {
        Ok(rc)
    }
}

fn owned(fd: RawFd) -> Result<OwnedFd, u32> {
    check(fd)?;
    // SAFETY: a descriptor the call just returned, owned by no one else.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

// O_NOFOLLOW_ANY alone: macOS refuses it beside O_NOFOLLOW (EINVAL), and over a one-name path it
// says the same.
const NOFOLLOW: libc::c_int = libc::O_NOFOLLOW_ANY;
const DIR_FLAGS: libc::c_int = libc::O_RDONLY | libc::O_DIRECTORY | NOFOLLOW | libc::O_CLOEXEC;

/// The root of a share, opened once. The share's own path may pass through links: the person chose
/// it. Below it, none is followed.
pub struct Root {
    fd: OwnedFd,
    pub read_only: bool,
}

impl Root {
    pub fn open(path: &Path, read_only: bool) -> Result<Root, u32> {
        let c = CString::new(path.as_os_str().as_encoded_bytes()).map_err(|_| ninep::E_INVAL)?;
        // SAFETY: a NUL-terminated path.
        let fd = owned(unsafe {
            libc::open(
                c.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
            )
        })?;
        Ok(Root { fd, read_only })
    }

    /// The directory `rel` names, opened, one element at a time.
    pub fn dir(&self, rel: &[Vec<u8>]) -> Result<OwnedFd, u32> {
        // SAFETY: dup of a descriptor this Root owns.
        let mut at = owned(unsafe { libc::fcntl(self.fd.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 0) })?;
        for name in rel {
            let c = cname(name)?;
            // SAFETY: a live descriptor and a NUL-terminated name.
            at = owned(unsafe { libc::openat(at.as_raw_fd(), c.as_ptr(), DIR_FLAGS) })?;
        }
        Ok(at)
    }

    /// `rel`'s parent directory, opened, and its last name. None for the root itself.
    pub fn parent<'a>(&self, rel: &'a [Vec<u8>]) -> Result<Option<(OwnedFd, &'a [u8])>, u32> {
        match rel.split_last() {
            None => Ok(None),
            Some((last, up)) => Ok(Some((self.dir(up)?, last))),
        }
    }

    /// `rel`'s status, a link's own (lstat).
    pub fn lstat(&self, rel: &[Vec<u8>]) -> Result<libc::stat, u32> {
        match self.parent(rel)? {
            None => fstat(self.fd.as_raw_fd()),
            Some((dir, name)) => fstatat(dir.as_raw_fd(), name),
        }
    }

    /// Open the file or directory `rel` names with macOS `flags` (O_NOFOLLOW_ANY is added: a link
    /// is never opened through).
    pub fn open_at(&self, rel: &[Vec<u8>], flags: libc::c_int) -> Result<OwnedFd, u32> {
        match self.parent(rel)? {
            None => self.dir(&[]),
            Some((dir, name)) => {
                let c = cname(name)?;
                // SAFETY: a live descriptor and a NUL-terminated name.
                owned(unsafe {
                    libc::openat(
                        dir.as_raw_fd(),
                        c.as_ptr(),
                        flags | NOFOLLOW | libc::O_CLOEXEC,
                    )
                })
            }
        }
    }

    pub fn statfs(&self) -> Result<libc::statfs, u32> {
        // SAFETY: an all-zero statfs is a valid value to be overwritten.
        let mut st: libc::statfs = unsafe { std::mem::zeroed() };
        // SAFETY: a live descriptor and a buffer of the right type.
        check(unsafe { libc::fstatfs(self.fd.as_raw_fd(), &mut st) })?;
        Ok(st)
    }
}

pub fn fstat(fd: RawFd) -> Result<libc::stat, u32> {
    // SAFETY: an all-zero stat is a valid value to be overwritten.
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    // SAFETY: a live descriptor and a buffer of the right type.
    check(unsafe { libc::fstat(fd, &mut st) })?;
    Ok(st)
}

pub fn fstatat(dir: RawFd, name: &[u8]) -> Result<libc::stat, u32> {
    let c = cname(name)?;
    // SAFETY: as fstat.
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    // SAFETY: a live descriptor, a NUL-terminated name and a buffer of the right type.
    check(unsafe { libc::fstatat(dir, c.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) })?;
    Ok(st)
}

pub fn is_dir(st: &libc::stat) -> bool {
    st.st_mode & libc::S_IFMT == libc::S_IFDIR
}

pub fn is_link(st: &libc::stat) -> bool {
    st.st_mode & libc::S_IFMT == libc::S_IFLNK
}

pub fn is_reg(st: &libc::stat) -> bool {
    st.st_mode & libc::S_IFMT == libc::S_IFREG
}

pub fn pread(fd: RawFd, buf: &mut [u8], off: u64) -> Result<usize, u32> {
    let off = libc::off_t::try_from(off).map_err(|_| ninep::E_OVERFLOW)?;
    // SAFETY: a live descriptor and a buffer of buf.len() bytes.
    let n = unsafe { libc::pread(fd, buf.as_mut_ptr().cast(), buf.len(), off) };
    if n < 0 {
        Err(errno::last())
    } else {
        Ok(n as usize)
    }
}

/// A write at `off`, or at the end for a descriptor opened O_APPEND (the offset is then the
/// server's, as 9P2000.L's append is).
pub fn write_at(fd: RawFd, data: &[u8], off: u64, append: bool) -> Result<usize, u32> {
    let n = if append {
        // SAFETY: a live descriptor and data.len() bytes.
        unsafe { libc::write(fd, data.as_ptr().cast(), data.len()) }
    } else {
        let off = libc::off_t::try_from(off).map_err(|_| ninep::E_OVERFLOW)?;
        // SAFETY: as above.
        unsafe { libc::pwrite(fd, data.as_ptr().cast(), data.len(), off) }
    };
    if n < 0 {
        Err(errno::last())
    } else {
        Ok(n as usize)
    }
}

/// One directory entry: its name, its d_type and its inode.
pub struct Entry {
    pub name: Vec<u8>,
    pub dtype: u8,
    pub ino: u64,
}

/// Every entry of the open directory `fd`, `.` and `..` left out (a Plan 9 directory read has
/// neither).
pub fn entries(fd: RawFd) -> Result<Vec<Entry>, u32> {
    // SAFETY: dup of a live descriptor; fdopendir takes the duplicate.
    let dup = check(unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) })?;
    // SAFETY: a fresh descriptor, given to the stream.
    let dir = unsafe { libc::fdopendir(dup) };
    if dir.is_null() {
        let e = errno::last();
        // SAFETY: fdopendir failed, so the duplicate is still ours.
        unsafe { libc::close(dup) };
        return Err(e);
    }
    // SAFETY: a stream fdopendir just made; rewinding reads from the first entry whatever the
    // shared offset was.
    unsafe { libc::rewinddir(dir) };
    let mut out = Vec::new();
    loop {
        // SAFETY: a live stream; the entry is copied before the next call.
        let e = unsafe { libc::readdir(dir) };
        if e.is_null() {
            break;
        }
        // SAFETY: readdir returned a valid entry whose d_name is NUL-terminated within d_namlen.
        let (name, dtype, ino) = unsafe {
            let e = &*e;
            let name =
                std::slice::from_raw_parts(e.d_name.as_ptr().cast::<u8>(), e.d_namlen as usize)
                    .to_vec();
            (name, e.d_type, e.d_ino)
        };
        if name == b"." || name == b".." {
            continue;
        }
        out.push(Entry { name, dtype, ino });
    }
    // SAFETY: the stream is ours and closes its descriptor.
    unsafe { libc::closedir(dir) };
    Ok(out)
}

/// The Linux open flags 9P2000.L carries (the kernel sends the access mode, O_TRUNC and O_APPEND)
/// as macOS's.
pub fn mac_open_flags(linux: u32) -> Result<libc::c_int, u32> {
    const L_TRUNC: u32 = 0o1000;
    const L_APPEND: u32 = 0o2000;
    let mut f = match linux & 3 {
        0 => libc::O_RDONLY,
        1 => libc::O_WRONLY,
        2 => libc::O_RDWR,
        _ => return Err(ninep::E_INVAL),
    };
    if linux & L_TRUNC != 0 {
        f |= libc::O_TRUNC;
    }
    if linux & L_APPEND != 0 {
        f |= libc::O_APPEND;
    }
    Ok(f)
}

pub fn mkdirat(dir: RawFd, name: &[u8], mode: u32) -> Result<(), u32> {
    let c = cname(name)?;
    // SAFETY: a live descriptor and a NUL-terminated name.
    check(unsafe { libc::mkdirat(dir, c.as_ptr(), (mode & 0o7777) as libc::mode_t) })?;
    Ok(())
}

pub fn create(dir: RawFd, name: &[u8], flags: libc::c_int, mode: u32) -> Result<OwnedFd, u32> {
    let c = cname(name)?;
    // SAFETY: as mkdirat. O_EXCL: a create of a name that exists answers EEXIST, which the kernel
    // reads as proof the file exists.
    owned(unsafe {
        libc::openat(
            dir,
            c.as_ptr(),
            flags | libc::O_CREAT | libc::O_EXCL | NOFOLLOW | libc::O_CLOEXEC,
            (mode & 0o7777) as libc::c_uint,
        )
    })
}

pub fn symlinkat(target: &[u8], dir: RawFd, name: &[u8]) -> Result<(), u32> {
    let (t, c) = (cname(target)?, cname(name)?);
    // SAFETY: two NUL-terminated strings and a live descriptor.
    check(unsafe { libc::symlinkat(t.as_ptr(), dir, c.as_ptr()) })?;
    Ok(())
}

pub fn readlinkat(dir: RawFd, name: &[u8]) -> Result<Vec<u8>, u32> {
    let c = cname(name)?;
    let mut buf = vec![0u8; libc::PATH_MAX as usize + 1];
    // SAFETY: a live descriptor, a NUL-terminated name and a buffer of buf.len() bytes.
    let n = unsafe { libc::readlinkat(dir, c.as_ptr(), buf.as_mut_ptr().cast(), buf.len()) };
    if n < 0 {
        return Err(errno::last());
    }
    buf.truncate(n as usize);
    Ok(buf)
}

pub fn unlinkat(dir: RawFd, name: &[u8], rmdir: bool) -> Result<(), u32> {
    let c = cname(name)?;
    let flag = if rmdir { libc::AT_REMOVEDIR } else { 0 };
    // SAFETY: a live descriptor and a NUL-terminated name.
    check(unsafe { libc::unlinkat(dir, c.as_ptr(), flag) })?;
    Ok(())
}

pub fn renameat(from_dir: RawFd, from: &[u8], to_dir: RawFd, to: &[u8]) -> Result<(), u32> {
    let (f, t) = (cname(from)?, cname(to)?);
    // SAFETY: live descriptors and NUL-terminated names.
    check(unsafe { libc::renameat(from_dir, f.as_ptr(), to_dir, t.as_ptr()) })?;
    Ok(())
}

/// chmod of `name` in `dir`, a link's own mode for a link; `fd` instead when the file is open.
pub fn chmod(at: Target<'_>, mode: u32) -> Result<(), u32> {
    let m = (mode & 0o7777) as libc::mode_t;
    match at {
        // SAFETY: a live descriptor.
        Target::Fd(fd) => check(unsafe { libc::fchmod(fd, m) })?,
        Target::Name(dir, name) => {
            let c = cname(name)?;
            // SAFETY: a live descriptor and a NUL-terminated name.
            check(unsafe { libc::fchmodat(dir, c.as_ptr(), m, libc::AT_SYMLINK_NOFOLLOW) })?
        }
    };
    Ok(())
}

/// Set the access and modification times; `None` leaves one as it is, `Some(None)` sets it to
/// now.
pub fn utimes(
    at: Target<'_>,
    atime: Option<Option<(u64, u64)>>,
    mtime: Option<Option<(u64, u64)>>,
) -> Result<(), u32> {
    fn ts(t: Option<Option<(u64, u64)>>) -> Result<libc::timespec, u32> {
        Ok(match t {
            None => libc::timespec {
                tv_sec: 0,
                tv_nsec: libc::UTIME_OMIT,
            },
            Some(None) => libc::timespec {
                tv_sec: 0,
                tv_nsec: libc::UTIME_NOW,
            },
            Some(Some((s, ns))) => {
                if ns >= 1_000_000_000 {
                    return Err(ninep::E_INVAL);
                }
                libc::timespec {
                    tv_sec: libc::time_t::try_from(s).map_err(|_| ninep::E_INVAL)?,
                    tv_nsec: ns as libc::c_long,
                }
            }
        })
    }
    let times = [ts(atime)?, ts(mtime)?];
    match at {
        // SAFETY: a live descriptor and two timespecs.
        Target::Fd(fd) => check(unsafe { libc::futimens(fd, times.as_ptr()) })?,
        Target::Name(dir, name) => {
            let c = cname(name)?;
            // SAFETY: a live descriptor, a NUL-terminated name and two timespecs.
            check(unsafe {
                libc::utimensat(dir, c.as_ptr(), times.as_ptr(), libc::AT_SYMLINK_NOFOLLOW)
            })?
        }
    };
    Ok(())
}

pub fn ftruncate(fd: RawFd, size: u64) -> Result<(), u32> {
    let size = libc::off_t::try_from(size).map_err(|_| ninep::E_FBIG)?;
    // SAFETY: a live descriptor.
    check(unsafe { libc::ftruncate(fd, size) })?;
    Ok(())
}

pub fn fsync(fd: RawFd) -> Result<(), u32> {
    // SAFETY: a live descriptor.
    check(unsafe { libc::fsync(fd) })?;
    Ok(())
}

/// Where a metadata change lands: an open descriptor, or a name in a directory.
#[derive(Clone, Copy)]
pub enum Target<'a> {
    Fd(RawFd),
    Name(RawFd, &'a [u8]),
}

/// The time a stat records, as (seconds, nanoseconds).
pub fn time(sec: libc::time_t, nsec: libc::c_long) -> (u64, u64) {
    (sec.max(0) as u64, nsec.clamp(0, 999_999_999) as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::symlink;

    fn scratch(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("thylacine-folder-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn rel(p: &str) -> Rel {
        p.split('/')
            .filter(|s| !s.is_empty())
            .map(|s| s.as_bytes().to_vec())
            .collect()
    }

    #[test]
    fn a_name_is_one_element() {
        for good in [&b"a"[..], b"...", b"a b", b".hidden"] {
            assert!(name_ok(good), "{good:?}");
        }
        for bad in [&b""[..], b".", b"..", b"a/b", b"a\0"] {
            assert!(!name_ok(bad), "{bad:?}");
        }
    }

    #[test]
    fn a_link_inside_is_seen_as_a_link_and_never_passed_through() {
        let d = scratch("link");
        fs::create_dir_all(d.join("in/deep")).unwrap();
        fs::write(d.join("in/deep/f"), b"x").unwrap();
        let outside = scratch("outside");
        fs::write(outside.join("secret"), b"s").unwrap();
        symlink(&outside, d.join("in/out")).unwrap();
        symlink("deep", d.join("in/near")).unwrap();
        let r = Root::open(&d, false).unwrap();
        assert!(is_dir(&r.lstat(&rel("in/deep")).unwrap()));
        assert!(is_link(&r.lstat(&rel("in/out")).unwrap()));
        assert!(
            is_link(&r.lstat(&rel("in/near")).unwrap()),
            "a link that stays inside is a link too"
        );
        assert_eq!(r.lstat(&rel("in/out/secret")).err(), Some(ninep::E_LOOP));
        assert_eq!(r.lstat(&rel("in/near/f")).err(), Some(ninep::E_LOOP));
        assert_eq!(
            r.open_at(&rel("in/out"), libc::O_RDONLY).err(),
            Some(ninep::E_LOOP)
        );
        assert_eq!(
            readlinkat(r.dir(&rel("in")).unwrap().as_raw_fd(), b"near").unwrap(),
            b"deep"
        );
        let _ = fs::remove_dir_all(&d);
        let _ = fs::remove_dir_all(&outside);
    }

    #[test]
    fn a_root_that_is_a_link_is_followed_once_because_the_person_chose_it() {
        let d = scratch("rootlink");
        fs::create_dir_all(d.join("real")).unwrap();
        fs::write(d.join("real/f"), b"x").unwrap();
        symlink(d.join("real"), d.join("chosen")).unwrap();
        let r = Root::open(&d.join("chosen"), false).unwrap();
        assert!(is_reg(&r.lstat(&rel("f")).unwrap()));
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn entries_leave_out_dot_and_dotdot_and_carry_types() {
        let d = scratch("entries");
        fs::create_dir(d.join("sub")).unwrap();
        fs::write(d.join("file"), b"x").unwrap();
        symlink("file", d.join("ln")).unwrap();
        let r = Root::open(&d, false).unwrap();
        let fd = r.dir(&[]).unwrap();
        let mut e: Vec<(Vec<u8>, u8)> = entries(fd.as_raw_fd())
            .unwrap()
            .into_iter()
            .map(|e| (e.name, e.dtype))
            .collect();
        e.sort();
        assert_eq!(
            e,
            vec![
                (b"file".to_vec(), libc::DT_REG),
                (b"ln".to_vec(), libc::DT_LNK),
                (b"sub".to_vec(), libc::DT_DIR)
            ]
        );
        // A second read starts from the first entry again.
        assert_eq!(entries(fd.as_raw_fd()).unwrap().len(), 3);
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn open_flags_carry_the_access_mode_trunc_and_append() {
        assert_eq!(mac_open_flags(0), Ok(libc::O_RDONLY));
        assert_eq!(
            mac_open_flags(1 | 0o1000),
            Ok(libc::O_WRONLY | libc::O_TRUNC)
        );
        assert_eq!(
            mac_open_flags(2 | 0o2000),
            Ok(libc::O_RDWR | libc::O_APPEND)
        );
        assert_eq!(mac_open_flags(3), Err(ninep::E_INVAL));
    }

    #[test]
    fn create_is_exclusive_and_append_writes_at_the_end() {
        let d = scratch("create");
        let r = Root::open(&d, false).unwrap();
        let dir = r.dir(&[]).unwrap();
        let f = create(
            dir.as_raw_fd(),
            b"n",
            libc::O_WRONLY | libc::O_APPEND,
            0o644,
        )
        .unwrap();
        assert_eq!(
            create(dir.as_raw_fd(), b"n", libc::O_WRONLY, 0o644).err(),
            Some(ninep::E_EXIST)
        );
        write_at(f.as_raw_fd(), b"ab", 0, true).unwrap();
        write_at(f.as_raw_fd(), b"cd", 0, true).unwrap();
        assert_eq!(fs::read(d.join("n")).unwrap(), b"abcd");
        let _ = fs::remove_dir_all(&d);
    }
}
