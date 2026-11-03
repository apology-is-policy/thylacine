// cp [-aLPpRrvnf] SRC... DST -- copy files (and, with -r, directory trees).
//
// Forms: `cp SRC DST` (DST a file or existing dir) and `cp SRC... DIR`.
// -r / -R: recursively copy a directory. Recursion skips "." / ".." (fs::read_dir
// yields them). A file copy is open(SRC) + create(DST, trunc, SRC's mode under
// the umask) + io::copy.
//
// File identity is (dev, qid path) from a stat that follows links, as the
// opens do. A destination that IS its source is refused before the truncating
// open could empty it, wherever in a tree it occurs; a directory met again
// below itself (a symlink cycle), or the destination tree met inside the
// source, is not descended. -p keeps the source's times, owner and mode (an
// owner or group the caller may not give is left, as POSIX allows); -a is
// -R -p -P. -P would copy a symlink as a symlink, which needs a creation call
// native code does not have yet, so a symlink source under -P is refused.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File, FileTimes, Metadata, OpenOptions, Path};
use libthyla_rs::{eprintln, io, println};

/// Behavior flags threaded through the copy.
struct Opts {
    verbose: bool,
    no_clobber: bool,
    preserve: bool,
    no_deref: bool,
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

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: cp [-aLPpRrvnf] SRC... DST
  Copy files (and, with -r, directory trees).
  -r, -R  copy directories recursively
  -p      keep each source's times, owner and mode
  -a      archive: -R -p -P
  -P, -d  copy a symbolic link as a link (not supported yet: refused)
  -L      follow symbolic links in the source (the default)
  -v      verbose: print each copy
  -n      no-clobber: never overwrite an existing file
  -f      force (accepted; cp always overwrites and never prompts)
  --help  show this help

Examples:
  cp a b                # copy a file
  cp -r dir copy        # copy a directory tree
  cp -p a b             # copy a file, keeping its times
  cp -v *.txt dest/     # report each copy
";

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut recursive = false;
    let mut verbose = false;
    let mut no_clobber = false;
    let mut preserve = false;
    let mut no_deref = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    'v' => verbose = true,
                    'n' => no_clobber = true,
                    'p' => preserve = true,
                    'a' => {
                        recursive = true;
                        preserve = true;
                        no_deref = true;
                    }
                    'P' | 'd' => no_deref = true,
                    'L' => no_deref = false,
                    'f' => {} // force: a no-op (cp always overwrites, never prompts)
                    _ => return usage::die("cp", &format!("invalid option -- '{}'", ch)),
                }
            }
            idx += 1;
        } else {
            break;
        }
    }
    let opts = Opts { verbose, no_clobber, preserve, no_deref };

    let mut ops: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => ops.push(p),
            Err(_) => {
                eprintln!("cp: invalid UTF-8 in operand");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("cp: missing file operand");
        return 1;
    }

    let dest = ops[ops.len() - 1];
    let sources = &ops[..ops.len() - 1];
    let dest_is_dir = fs::is_dir(dest);
    if sources.len() > 1 && !dest_is_dir {
        eprintln!("cp: target '{}' is not a directory", dest);
        return 1;
    }

    let mut status = 0;
    for src in sources {
        if fs::is_dir(src) && !recursive {
            eprintln!("cp: -r not specified; omitting directory '{}'", src);
            status = 1;
            continue;
        }
        let target = if dest_is_dir {
            join(dest, base(src))
        } else {
            String::from(dest)
        };
        // Refuse the plain into-itself copy (`cp -r d d/sub`) before anything
        // is created. Lexical, so a symlink can hide it: the walk's dest_root
        // guard catches that case, after the fact.
        if recursive && fs::is_dir(src) {
            if let (Some(mut prefix), Some(ct)) = (canon(src), canon(target.as_str())) {
                if !prefix.ends_with('/') {
                    prefix.push('/');
                }
                if ct.starts_with(prefix.as_str()) {
                    eprintln!("cp: cannot copy a directory, '{}', into itself, '{}'", src, target);
                    status = 1;
                    continue;
                }
            }
        }
        let mut walk = Walk { ancestors: Vec::new(), dest_root: None };
        if !cp_one(src, target.as_str(), &opts, &mut walk) {
            status = 1;
        }
    }
    status
}

// Copy `src` to `dst`, printing a diagnostic for each failure; false if any.
// A directory here always means recursive mode (the top level gated the
// dir-without-r case). A failed entry does not stop its siblings.
fn cp_one(src: &str, dst: &str, o: &Opts, w: &mut Walk) -> bool {
    if o.no_deref {
        match File::open_link(src).and_then(|f| f.metadata()) {
            Ok(m) if m.is_symlink() => {
                eprintln!("cp: cannot copy symbolic link '{}': creating a link is not supported", src);
                return false;
            }
            Ok(_) => {}
            Err(e) => {
                eprintln!("cp: cannot stat '{}': {}", src, e);
                return false;
            }
        }
    }
    match fs::metadata(src) {
        Ok(m) if m.is_dir() => copy_dir(src, dst, &m, o, w),
        Ok(m) => copy_file(src, dst, &m, o),
        Err(e) => {
            eprintln!("cp: cannot stat '{}': {}", src, e);
            false
        }
    }
}

fn copy_dir(src: &str, dst: &str, m: &Metadata, o: &Opts, w: &mut Walk) -> bool {
    let id = id_of(m);
    if w.ancestors.contains(&id) {
        eprintln!("cp: '{}': a directory that contains itself (a symlink cycle); not copied", src);
        return false;
    }
    if w.dest_root == Some(id) {
        eprintln!("cp: cannot copy a directory, '{}', into itself, '{}'", src, dst);
        return false;
    }
    let created = match fs::create_dir(dst) {
        Ok(()) => true,
        Err(Error::Exists) => false,
        Err(e) => {
            eprintln!("cp: cannot create directory '{}': {}", dst, e);
            return false;
        }
    };
    let dm = match fs::metadata(dst) {
        Ok(dm) => dm,
        Err(e) => {
            eprintln!("cp: cannot stat '{}': {}", dst, e);
            return false;
        }
    };
    if !dm.is_dir() {
        eprintln!("cp: cannot overwrite non-directory '{}' with directory '{}'", dst, src);
        return false;
    }
    if id_of(&dm) == id {
        eprintln!("cp: '{}' and '{}' are the same file", src, dst);
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
                        if !cp_one(&join(src, name), &join(dst, name), o, w) {
                            ok = false;
                        }
                    }
                    Err(e) => {
                        eprintln!("cp: reading directory '{}': {}", src, e);
                        ok = false;
                        break;
                    }
                }
            }
        }
        Err(e) => {
            eprintln!("cp: cannot read directory '{}': {}", src, e);
            ok = false;
        }
    }
    w.ancestors.pop();

    // After the entries: creating them moved the directory's mtime, and a
    // read-only mode would have refused them.
    if o.preserve {
        match File::open_with_opath(dst) {
            Ok(f) => ok &= preserve(&f, dst, m),
            Err(e) => {
                eprintln!("cp: cannot open '{}' to preserve its attributes: {}", dst, e);
                ok = false;
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
            eprintln!("cp: '{}' and '{}' are the same file", src, dst);
            return false;
        }
    }
    let mut r = match File::open(src) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("cp: cannot open '{}' for reading: {}", src, e);
            return false;
        }
    };
    // The source's mode for a file this creates (the umask applies); an
    // existing destination keeps its own unless -p.
    let mut wr = match OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(m.permissions() & 0o777)
        .open(dst)
    {
        Ok(wr) => wr,
        Err(e) => {
            eprintln!("cp: cannot create regular file '{}': {}", dst, e);
            return false;
        }
    };
    if let Err(e) = io::copy(&mut r, &mut wr) {
        eprintln!("cp: error copying '{}' to '{}': {}", src, dst, e);
        return false;
    }
    if o.preserve && !preserve(&wr, dst, m) {
        return false;
    }
    if o.verbose {
        println!("'{}' -> '{}'", src, dst);
    }
    true
}

// -p on the open destination: times first (the owner may set them, and the
// handle that wrote flushes before they apply), then owner and group, then
// the mode.
fn preserve(f: &File, dst: &str, m: &Metadata) -> bool {
    let times = FileTimes::new()
        .set_accessed(libthyla_rs::time::SystemTime::from_unix(m.atime_sec(), 0))
        .set_modified(libthyla_rs::time::SystemTime::from_unix(m.mtime_sec(), 0));
    if let Err(e) = f.set_times(times) {
        eprintln!("cp: preserving times for '{}': {}", dst, e);
        return false;
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
        eprintln!("cp: preserving permissions for '{}': {}", dst, e);
        return false;
    }
    true
}

// Canonicalize lexically: anchor a relative path against the per-Proc cwd
// (LS-4), collapse `.`/`..`/`//`. None only if the cwd is unreadable for a
// relative input. Mirrors the realpath coreutil. Not file identity -- it cannot
// see through a symlink -- so it serves only the early into-itself refusal.
fn canon(path: &str) -> Option<String> {
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

fn base(path: &str) -> &str {
    Path::new(path).file_name().unwrap_or(path)
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
