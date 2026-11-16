// The copy engine cp and mv share: a file or a directory tree duplicated, with
// the source's times, owner and mode kept on request.
//
// A file copy is open(SRC) + create(DST, trunc, SRC's mode under the umask) +
// io::copy. Recursion skips "." / ".." (fs::read_dir yields them).
//
// File identity is (dev, qid path) from a stat that follows links, as the
// opens do. When links are not followed, the walk goes by descriptor, as
// rmtree's does: each entry is examined through a navigation handle that does
// not follow a final link, opened relative to its directory's handle (the top
// operand by its path), a directory's entries are read from a reading open of
// its own handle ("."), and a file is opened relative to its directory without
// following a final link and checked to be the entry examined. No entry is
// reached by a path walked again from the top, so a link swapped in during the
// copy, for a file or for a directory already listed, cannot lead it out of
// the tree. A directory the copy creates is examined through the handle its
// creation returned, so it is the one made. A destination that IS its source is
// refused before the truncating open could empty it, wherever in a tree it
// occurs; a directory met again below itself (a symlink cycle), or the
// destination tree met inside the source, is not descended. Keeping attributes
// keeps the source's times, owner and mode (an owner or group the caller may
// not give is left without a word, as GNU cp and mv leave it for an
// unprivileged caller; the mode carries no setuid or setgid bit, so none
// survives a refused owner). Copying a symlink as a symlink needs a creation
// call native code does not have yet, so a symlink source is refused when
// links are not followed. Every destination file is closed with its error
// checked: a write the server had not yet taken fails the copy there. A file's
// mode, times and identity come from its open handle, the file actually
// copied; a special file is refused on what that handle says (an unconnected
// /srv entry stats as a regular file, and the open that tells connects to the
// service, a connection dropped at once).
//
// A directory's attributes and sync wait until the whole walk is done (its
// entries change it until then, and a kept read-only mode would refuse them),
// and are applied deepest first, each to the directory the copy made, checked
// by identity. mv asks three more things: every entry created new (its
// temporary tree), each file and directory synced, and directory attributes
// kept only when the whole copy succeeded, since a failed one is removed. It
// is told each entry reproduced, a file with the version its open found, so
// that its removal of the source can leave a file changed since.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::env;
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File, FileTimes, Metadata, OpenOptions, Path, ReadDir};
use libthyla_rs::{eprintln, io, println, T_ONOFOLLOW, T_OPATH, T_OREAD};

/// How a copy behaves, and the tool its diagnostics name.
pub struct Opts {
    pub tool: &'static str,
    pub verbose: bool,
    pub no_clobber: bool,
    pub preserve: bool,
    pub no_deref: bool,
    /// An attribute the server refuses to set is said and does not fail the
    /// copy, as POSIX asks of mv.
    pub attrs_soft: bool,
    /// A source that, opened, is neither a regular file nor a directory is
    /// refused: a device's or a service's stream is not its content.
    pub refuse_special: bool,
    /// Every destination entry is created new; one that exists fails the copy.
    pub excl: bool,
    /// Each file is synced before it is closed, and each directory once its
    /// entries and attributes are in.
    pub sync: bool,
    /// Directory attributes are kept only when the whole copy succeeded.
    pub attrs_only_whole: bool,
}

/// A file's identity: its Dev instance and the server's qid path.
pub type Id = (u64, u64);

pub fn id_of(m: &Metadata) -> Id {
    (m.dev(), m.qid_path())
}

/// An entry as it was copied: its identity and, for anything but a directory,
/// its qid version, which the server moves on every change to the file and
/// gives anew to a file that reuses a removed one's number. A directory's
/// entries change it, and one entry added must not stand for every file copied
/// below it, so a directory is its identity alone.
pub type Ver = (u64, u64, Option<u32>);

pub fn ver_of(m: &Metadata) -> Ver {
    let v = if m.is_dir() { None } else { Some(m.qid_vers()) };
    (m.dev(), m.qid_path(), v)
}

/// What a copy did, for a caller that removes its source or undoes it (mv).
#[derive(Default)]
pub struct Done {
    /// The top source entry, as the copy found it.
    pub src_top: Option<Id>,
    /// The top destination entry, when the copy created it.
    pub dst_top: Option<Id>,
    /// Every source entry reproduced, as it was when copied, sorted.
    pub copied: Vec<Ver>,
}

/// A directory the copy made or filled, whose attributes and sync wait for the
/// end of the walk.
struct Made {
    path: String,
    id: Id,
    src: Metadata,
}

/// One top-level source's traversal: the directories on the current path, the
/// destination directory it creates or fills, the directories left to finish,
/// and what it did.
struct Walk {
    ancestors: Vec<Id>,
    dest_root: Option<Id>,
    made: Vec<Made>,
    done: Done,
}

/// Where a source entry is found: by its path (the top operand, and every
/// entry when links are followed), or by name in its directory's handle.
#[derive(Clone, Copy)]
enum At<'a> {
    Path,
    In(&'a File, &'a str),
}

/// Copy `src` to `dst`, a file or a whole directory tree (the caller decides
/// whether a directory may be copied), printing a diagnostic for each failure;
/// false if any. A failed entry does not stop its siblings.
pub fn copy(src: &str, dst: &str, o: &Opts) -> bool {
    copy_tracked(src, dst, o, &mut Done::default())
}

/// `copy`, recording what it did in `done`.
pub fn copy_tracked(src: &str, dst: &str, o: &Opts, done: &mut Done) -> bool {
    let mut w = Walk {
        ancestors: Vec::new(),
        dest_root: None,
        made: Vec::new(),
        done: Done::default(),
    };
    let walked = copy_one(src, dst, At::Path, o, &mut w);
    let finished = finish(&w.made, walked, o);
    w.done.copied.sort_unstable();
    w.done.copied.dedup();
    *done = w.done;
    walked && finished
}

fn copy_one(src: &str, dst: &str, at: At, o: &Opts, w: &mut Walk) -> bool {
    // Not following links, the entry is examined through a navigation handle
    // that does not follow a final link; a directory's entries are found from
    // that handle.
    let examined = if o.no_deref {
        let nav = match at {
            At::Path => File::open_link(src),
            At::In(dir, name) => dir.open_at(name, T_OPATH | T_ONOFOLLOW),
        };
        nav.and_then(|f| Ok((f.metadata()?, Some(f))))
    } else {
        fs::metadata(src).map(|m| (m, None))
    };
    match examined {
        Ok((m, _)) if m.is_symlink() => {
            eprintln!(
                "{}: cannot copy symbolic link '{}': creating a link is not supported",
                o.tool, src
            );
            false
        }
        Ok((m, nav)) if m.is_dir() => copy_dir(src, dst, &m, nav.as_ref(), o, w),
        Ok((m, _)) => copy_file(src, dst, &m, at, o, w),
        Err(e) => {
            eprintln!("{}: cannot stat '{}': {}", o.tool, src, e);
            false
        }
    }
}

fn copy_dir(
    src: &str,
    dst: &str,
    m: &Metadata,
    nav: Option<&File>,
    o: &Opts,
    w: &mut Walk,
) -> bool {
    let id = id_of(m);
    if w.ancestors.is_empty() {
        w.done.src_top = Some(id);
    }
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
    // Kept attributes widen the mode at the end; until then, and if that is
    // refused, only the owner reaches the copy. A directory created here is
    // examined through the handle its creation returned; an existing
    // destination may be a link to the directory to fill.
    let mode = if o.preserve { 0o700 } else { 0o755 };
    let (created, stat) = match fs::create_dir_open(dst, mode) {
        Ok(d) => (true, d.metadata()),
        Err(Error::Exists) if !o.excl => (false, fs::metadata(dst)),
        Err(e) => {
            eprintln!("{}: cannot create directory '{}': {}", o.tool, dst, e);
            return false;
        }
    };
    let dm = match stat {
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
        if created {
            w.done.dst_top = Some(id_of(&dm));
        }
    }
    if o.verbose && created {
        println!("'{}' -> '{}'", src, dst);
    }
    w.done.copied.push(ver_of(m));

    w.ancestors.push(id);
    let mut ok = true;
    match list(src, nav, o) {
        Some(entries) => {
            for ent in entries {
                match ent {
                    Ok(e) => {
                        let name = e.file_name();
                        if name == "." || name == ".." {
                            continue;
                        }
                        let at = match nav {
                            Some(dir) => At::In(dir, name),
                            None => At::Path,
                        };
                        if !copy_one(&join(src, name), &join(dst, name), at, o, w) {
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
        None => ok = false,
    }
    w.ancestors.pop();
    if o.preserve || o.sync {
        w.made.push(Made {
            path: String::from(dst),
            id: id_of(&dm),
            src: *m,
        });
    }
    ok
}

/// The entries of the directory `src`; when links are not followed, from a
/// reading open of its navigation handle ("."), the directory examined itself.
fn list(src: &str, nav: Option<&File>, o: &Opts) -> Option<ReadDir> {
    let listed = match nav {
        Some(dir) => dir.open_at(".", T_OREAD).map(ReadDir::from_file),
        None => fs::read_dir(src),
    };
    match listed {
        Ok(entries) => Some(entries),
        Err(e) => {
            eprintln!("{}: cannot read directory '{}': {}", o.tool, src, e);
            None
        }
    }
}

fn copy_file(src: &str, dst: &str, m: &Metadata, at: At, o: &Opts, w: &mut Walk) -> bool {
    if !o.excl {
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
    }
    let opened = match at {
        At::In(dir, name) => dir.open_at(name, T_OREAD | T_ONOFOLLOW),
        At::Path if o.no_deref => File::open_nofollow(src),
        At::Path => File::open(src),
    };
    let mut r = match opened {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{}: cannot open '{}' for reading: {}", o.tool, src, e);
            return false;
        }
    };
    let sm = match r.metadata() {
        Ok(sm) => sm,
        Err(e) if o.refuse_special => {
            eprintln!("{}: cannot stat '{}': {}", o.tool, src, e);
            return false;
        }
        Err(_) => *m,
    };
    if o.refuse_special && !sm.is_file() {
        eprintln!("{}: cannot copy special file '{}'", o.tool, src);
        return false;
    }
    // Not following links, the file opened must be the one the stat found: a
    // link swapped in is refused by the open, another entry here. (A service's
    // stream has its own identity; it is refused above when that is asked.)
    if o.no_deref && (sm.is_file() || sm.is_dir()) && id_of(&sm) != id_of(m) {
        eprintln!("{}: '{}' changed during the copy; not copied", o.tool, src);
        return false;
    }
    if w.ancestors.is_empty() {
        w.done.src_top = Some(id_of(&sm));
    }
    // The source's mode for a file this creates (the umask applies); an
    // existing destination keeps its own unless attributes are kept.
    let mut wr = match OpenOptions::new()
        .write(true)
        .create(true)
        .create_new(o.excl)
        .truncate(!o.excl)
        .mode(sm.permissions() & 0o777)
        .open(dst)
    {
        Ok(wr) => wr,
        Err(e) => {
            eprintln!("{}: cannot create regular file '{}': {}", o.tool, dst, e);
            return false;
        }
    };
    if o.excl && w.ancestors.is_empty() {
        match wr.metadata() {
            Ok(dm) => w.done.dst_top = Some(id_of(&dm)),
            Err(e) => {
                eprintln!("{}: cannot stat '{}': {}", o.tool, dst, e);
                return false;
            }
        }
    }
    if let Err(e) = io::copy(&mut r, &mut wr) {
        eprintln!("{}: error copying '{}' to '{}': {}", o.tool, src, dst, e);
        return false;
    }
    if o.preserve && !preserve(&wr, dst, &sm, o) {
        return false;
    }
    if o.sync {
        if let Err(e) = wr.sync_all() {
            eprintln!("{}: cannot sync '{}': {}", o.tool, dst, e);
            return false;
        }
    }
    if let Err(e) = wr.close() {
        eprintln!("{}: error writing '{}': {}", o.tool, dst, e);
        return false;
    }
    w.done.copied.push(ver_of(&sm));
    if o.verbose {
        println!("'{}' -> '{}'", src, dst);
    }
    true
}

// The directories' attributes and syncs, once the walk is done: deepest first,
// each checked to be the directory the copy made. A reading open is taken
// before the attributes, whose mode may refuse one, so the sync covers them.
// Nothing is finished for a failed copy whose attributes wait for a whole one:
// it is removed. False when anything failed the copy.
fn finish(made: &[Made], whole: bool, o: &Opts) -> bool {
    if !whole && o.attrs_only_whole {
        return true;
    }
    let mut ok = true;
    for d in made.iter() {
        let nav = match File::open_link(d.path.as_str()) {
            Ok(f) => f,
            Err(e) => {
                eprintln!("{}: cannot open '{}': {}", o.tool, d.path, e);
                ok &= !o.sync && soft(o, &e);
                continue;
            }
        };
        match nav.metadata() {
            Ok(cur) if id_of(&cur) == d.id => {}
            Ok(_) => {
                eprintln!(
                    "{}: '{}' was replaced during the copy; not finished",
                    o.tool, d.path
                );
                ok = false;
                continue;
            }
            Err(e) => {
                eprintln!("{}: cannot stat '{}': {}", o.tool, d.path, e);
                ok = false;
                continue;
            }
        }
        let rd = if o.sync {
            match nav.open_at(".", T_OREAD) {
                Ok(f) => Some(f),
                Err(e) => {
                    eprintln!("{}: cannot open '{}' to sync it: {}", o.tool, d.path, e);
                    ok = false;
                    None
                }
            }
        } else {
            None
        };
        if o.preserve {
            ok &= preserve(&nav, &d.path, &d.src, o);
        }
        if let Some(rd) = rd {
            if let Err(e) = rd.sync_all() {
                eprintln!("{}: cannot sync '{}': {}", o.tool, d.path, e);
                ok = false;
            }
        }
    }
    ok
}

// Keeping attributes on the open destination: times first (the owner may set
// them, and the handle that wrote flushes before they apply), then owner and
// group, then the mode. A soft refusal is said and the rest are still tried;
// any other failure (a change flushes writes the server had not taken) fails
// the copy. False only when a failure fails the copy.
fn preserve(f: &File, dst: &str, m: &Metadata, o: &Opts) -> bool {
    let times = FileTimes::new()
        .set_accessed(libthyla_rs::time::SystemTime::from_unix(m.atime_sec(), 0))
        .set_modified(libthyla_rs::time::SystemTime::from_unix(m.mtime_sec(), 0));
    if let Err(e) = f.set_times(times) {
        eprintln!("{}: preserving times for '{}': {}", o.tool, dst, e);
        if !soft(o, &e) {
            return false;
        }
    }
    // Each separately, so a refused owner does not cost the group. A refusal
    // leaves the destination's own, without a word (see the header).
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
        return soft(o, &e);
    }
    true
}

/// A refused attribute that does not fail a soft copy: the server will not set
/// it. Anything else is a failure.
fn soft(o: &Opts, e: &Error) -> bool {
    o.attrs_soft
        && matches!(
            e,
            Error::PermissionDenied
                | Error::NotPermitted
                | Error::ReadOnlyFilesystem
                | Error::Unsupported
        )
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

/// `path` as its directory and last name, for a call relative to the
/// directory: trailing slashes dropped, "." for a bare name. None for a path
/// with no last name to act on: "", "/", or one ending in "." or "..".
pub fn split(path: &str) -> Option<(&str, &str)> {
    let t = path.trim_end_matches('/');
    let (dir, name) = match t.rfind('/') {
        Some(0) => ("/", &t[1..]),
        Some(i) => (&t[..i], &t[i + 1..]),
        None => (".", t),
    };
    if name.is_empty() || name == "." || name == ".." {
        return None;
    }
    Some((dir, name))
}
