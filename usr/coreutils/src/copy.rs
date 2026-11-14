// The copy engine cp and mv share: a file or a directory tree duplicated, with
// the source's times, owner and mode kept on request.
//
// A file copy is open(SRC) + create(DST, trunc, SRC's mode under the umask) +
// io::copy. Recursion skips "." / ".." (fs::read_dir yields them).
//
// File identity is (dev, qid path) from a stat that follows links, as the
// opens do. A destination that IS its source is refused before the truncating
// open could empty it, wherever in a tree it occurs; a directory met again
// below itself (a symlink cycle), or the destination tree met inside the
// source, is not descended. Keeping attributes keeps the source's times, owner
// and mode (an owner or group the caller may not give is left, as POSIX
// allows). Copying a symlink as a symlink needs a creation call native code
// does not have yet, so a symlink source is refused when links are not
// followed.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::env;
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File, FileTimes, Metadata, OpenOptions, Path};
use libthyla_rs::{eprintln, io, println};

/// How a copy behaves, and the tool its diagnostics name.
pub struct Opts {
    pub tool: &'static str,
    pub verbose: bool,
    pub no_clobber: bool,
    pub preserve: bool,
    pub no_deref: bool,
    /// An attribute that cannot be kept is said and does not fail the copy, as
    /// POSIX asks of mv.
    pub attrs_soft: bool,
}

/// A file's identity: its Dev instance and the server's qid path.
type Id = (u64, u64);

fn id_of(m: &Metadata) -> Id {
    (m.dev(), m.qid_path())
}

/// One top-level source's traversal: the directories on the current path, and
/// the destination directory it creates or fills.
struct Walk {
    ancestors: Vec<Id>,
    dest_root: Option<Id>,
}

/// Copy `src` to `dst`, a file or a whole directory tree (the caller decides
/// whether a directory may be copied), printing a diagnostic for each failure;
/// false if any. A failed entry does not stop its siblings.
pub fn copy(src: &str, dst: &str, o: &Opts) -> bool {
    let mut w = Walk {
        ancestors: Vec::new(),
        dest_root: None,
    };
    copy_one(src, dst, o, &mut w)
}

fn copy_one(src: &str, dst: &str, o: &Opts, w: &mut Walk) -> bool {
    if o.no_deref {
        match File::open_link(src).and_then(|f| f.metadata()) {
            Ok(m) if m.is_symlink() => {
                eprintln!(
                    "{}: cannot copy symbolic link '{}': creating a link is not supported",
                    o.tool, src
                );
                return false;
            }
            Ok(_) => {}
            Err(e) => {
                eprintln!("{}: cannot stat '{}': {}", o.tool, src, e);
                return false;
            }
        }
    }
    match fs::metadata(src) {
        Ok(m) if m.is_dir() => copy_dir(src, dst, &m, o, w),
        Ok(m) => copy_file(src, dst, &m, o),
        Err(e) => {
            eprintln!("{}: cannot stat '{}': {}", o.tool, src, e);
            false
        }
    }
}

fn copy_dir(src: &str, dst: &str, m: &Metadata, o: &Opts, w: &mut Walk) -> bool {
    let id = id_of(m);
    if w.ancestors.contains(&id) {
        eprintln!(
            "{}: '{}': a directory that contains itself (a symlink cycle); not copied",
            o.tool, src
        );
        return false;
    }
    if w.dest_root == Some(id) {
        eprintln!(
            "{}: cannot copy a directory, '{}', into itself, '{}'",
            o.tool, src, dst
        );
        return false;
    }
    let created = match fs::create_dir(dst) {
        Ok(()) => true,
        Err(Error::Exists) => false,
        Err(e) => {
            eprintln!("{}: cannot create directory '{}': {}", o.tool, dst, e);
            return false;
        }
    };
    let dm = match fs::metadata(dst) {
        Ok(dm) => dm,
        Err(e) => {
            eprintln!("{}: cannot stat '{}': {}", o.tool, dst, e);
            return false;
        }
    };
    if !dm.is_dir() {
        eprintln!(
            "{}: cannot overwrite non-directory '{}' with directory '{}'",
            o.tool, dst, src
        );
        return false;
    }
    if id_of(&dm) == id {
        eprintln!("{}: '{}' and '{}' are the same file", o.tool, src, dst);
        return false;
    }
    if w.ancestors.is_empty() {
        w.dest_root = Some(id_of(&dm));
    }
    if o.verbose && created {
        println!("'{}' -> '{}'", src, dst);
    }

    w.ancestors.push(id);
    let mut ok = true;
    match fs::read_dir(src) {
        Ok(entries) => {
            for ent in entries {
                match ent {
                    Ok(e) => {
                        let name = e.file_name();
                        if name == "." || name == ".." {
                            continue;
                        }
                        if !copy_one(&join(src, name), &join(dst, name), o, w) {
                            ok = false;
                        }
                    }
                    Err(e) => {
                        eprintln!("{}: reading directory '{}': {}", o.tool, src, e);
                        ok = false;
                        break;
                    }
                }
            }
        }
        Err(e) => {
            eprintln!("{}: cannot read directory '{}': {}", o.tool, src, e);
            ok = false;
        }
    }
    w.ancestors.pop();

    // After the entries: creating them moved the directory's mtime, and a
    // read-only mode would have refused them.
    if o.preserve {
        match File::open_with_opath(dst) {
            Ok(f) => ok &= preserve(&f, dst, m, o),
            Err(e) => {
                eprintln!(
                    "{}: cannot open '{}' to preserve its attributes: {}",
                    o.tool, dst, e
                );
                ok &= o.attrs_soft;
            }
        }
    }
    ok
}

fn copy_file(src: &str, dst: &str, m: &Metadata, o: &Opts) -> bool {
    if let Ok(dm) = fs::metadata(dst) {
        // -n: never overwrite an existing destination (skip silently).
        if o.no_clobber {
            return true;
        }
        if id_of(&dm) == id_of(m) {
            eprintln!("{}: '{}' and '{}' are the same file", o.tool, src, dst);
            return false;
        }
    }
    let mut r = match File::open(src) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{}: cannot open '{}' for reading: {}", o.tool, src, e);
            return false;
        }
    };
    // The source's mode for a file this creates (the umask applies); an
    // existing destination keeps its own unless attributes are kept.
    let mut wr = match OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(m.permissions() & 0o777)
        .open(dst)
    {
        Ok(wr) => wr,
        Err(e) => {
            eprintln!("{}: cannot create regular file '{}': {}", o.tool, dst, e);
            return false;
        }
    };
    if let Err(e) = io::copy(&mut r, &mut wr) {
        eprintln!("{}: error copying '{}' to '{}': {}", o.tool, src, dst, e);
        return false;
    }
    if o.preserve && !preserve(&wr, dst, m, o) {
        return false;
    }
    if o.verbose {
        println!("'{}' -> '{}'", src, dst);
    }
    true
}

// Keeping attributes on the open destination: times first (the owner may set
// them, and the handle that wrote flushes before they apply), then owner and
// group, then the mode. False only when a failure fails the copy.
fn preserve(f: &File, dst: &str, m: &Metadata, o: &Opts) -> bool {
    let times = FileTimes::new()
        .set_accessed(libthyla_rs::time::SystemTime::from_unix(m.atime_sec(), 0))
        .set_modified(libthyla_rs::time::SystemTime::from_unix(m.mtime_sec(), 0));
    if let Err(e) = f.set_times(times) {
        eprintln!("{}: preserving times for '{}': {}", o.tool, dst, e);
        return o.attrs_soft;
    }
    // Each separately, so a refused owner does not cost the group. A refusal
    // leaves the destination's own, which POSIX permits without a diagnostic.
    if let Ok(cur) = f.metadata() {
        if cur.uid() != m.uid() {
            let _ = f.chown(Some(m.uid()), None);
        }
        if cur.gid() != m.gid() {
            let _ = f.chown(None, Some(m.gid()));
        }
    }
    if let Err(e) = f.chmod(m.permissions()) {
        eprintln!("{}: preserving permissions for '{}': {}", o.tool, dst, e);
        return o.attrs_soft;
    }
    true
}

/// Canonicalize lexically: anchor a relative path against the per-Proc cwd
/// (LS-4), collapse `.`/`..`/`//`. None only if the cwd is unreadable for a
/// relative input. Not file identity -- it cannot see through a symlink.
pub fn canon(path: &str) -> Option<String> {
    let abs = if path.starts_with('/') {
        String::from(path)
    } else {
        let mut cwd = env::current_dir().ok()?;
        if !cwd.ends_with('/') {
            cwd.push('/');
        }
        cwd.push_str(path);
        cwd
    };
    let mut stack: Vec<&str> = Vec::new();
    for comp in abs.split('/') {
        match comp {
            "" | "." => {}
            ".." => {
                stack.pop();
            }
            c => stack.push(c),
        }
    }
    let mut out = String::from("/");
    out.push_str(&stack.join("/"));
    Some(out)
}

/// The last component of `path`.
pub fn base(path: &str) -> &str {
    Path::new(path).file_name().unwrap_or(path)
}

/// `dir` and `name` joined with one slash.
pub fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
