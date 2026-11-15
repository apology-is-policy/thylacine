// mv [-vnf] SRC... DST -- rename/move files.
//
// Two forms: `mv SRC DST` (rename) and `mv SRC... DIR` (move each into DIR).
// Within one device a move is libthyla-rs::fs::rename (SYS_RENAME / 9P
// Trenameat -- an atomic replace). Across devices the kernel answers EXDEV, and
// mv copies: the source's tree goes, with its times, owner and mode
// (coreutils::copy), into a temporary name mv creates new beside the
// destination, each file and directory synced; the temporary name is renamed
// over the destination (an atomic replace on that device) and the
// destination's directory synced; only then is the source removed
// (coreutils::rmtree), and only the entries copied, each file as it was
// copied: one that appeared during the copy, or changed after it, is named and
// left. A copy that fails removes the temporary name and leaves the source and
// the destination as they were. A union can make two names one file, or point
// a name at a different file between two lookups, so mv compares identities
// (Dev instance, qid path): a destination that is the source is refused, the
// name renamed into place must be the copy, and a source that no longer names
// the file copied is not removed. A file's removal also compares its qid
// version, which its server moves on every change and gives anew to a file
// that reuses a removed one's number. -n is asked before the move: a
// destination created after that is replaced (no rename here refuses to
// replace).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::copy::{self, base, id_of, join, split, Done, Id, Opts};
use coreutils::rmtree::{self, Rm};
use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File, Metadata, ReadDir};
use libthyla_rs::{eprintln, println, T_ONOFOLLOW, T_OPATH, T_OREAD};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: mv [-vnf] SRC... DST
  Rename or move files. Within one device a move is an atomic rename;
  across devices mv copies (times, owner and mode kept, synced), puts the
  copy in place with an atomic rename, then removes what it copied.
  -v      verbose: print each move
  -n      no-clobber: never overwrite an existing file
  -f      force (accepted; mv never prompts)
  --help  show this help

Examples:
  mv old new            # rename
  mv -v file dir/       # move into a directory
  mv -n a b             # never overwrite b
";

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut verbose = false;
    let mut no_clobber = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'v' => verbose = true,
                    'n' => no_clobber = true,
                    'f' => {} // force: a no-op (mv never prompts)
                    _ => return usage::die("mv", &format!("invalid option -- '{}'", ch)),
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut ops: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => ops.push(p),
            Err(_) => {
                eprintln!("mv: invalid UTF-8 in operand");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("mv: missing destination operand");
        return 1;
    }

    let dest = ops[ops.len() - 1];
    let sources = &ops[..ops.len() - 1];
    let dest_is_dir = fs::is_dir(dest);
    if sources.len() > 1 && !dest_is_dir {
        eprintln!("mv: target '{}' is not a directory", dest);
        return 1;
    }

    let mut status = 0;
    for src in sources {
        let target = if dest_is_dir {
            join(dest, base(src))
        } else {
            String::from(dest)
        };
        if no_clobber && fs::metadata(target.as_str()).is_ok() {
            continue; // -n: never overwrite an existing destination
        }
        match fs::rename(src, target.as_str()) {
            Ok(()) => {
                if verbose {
                    println!("'{}' -> '{}'", src, target);
                }
            }
            Err(Error::CrossesDevices) => {
                if !move_across(src, target.as_str(), verbose) {
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("mv: {} -> {}: {}", src, target, e);
                status = 1;
            }
        }
    }
    status
}

/// What `name` in the directory `dir` is, a link not followed.
fn stat_at(dir: &File, name: &str) -> Result<Metadata, Error> {
    dir.open_at(name, T_OPATH | T_ONOFOLLOW)
        .and_then(|f| f.metadata())
}

/// Whether the directory `name` in `dir` holds no entry.
fn is_empty_dir(dir: &File, name: &str) -> Result<bool, Error> {
    for ent in ReadDir::from_file(dir.open_at(name, T_OREAD | T_ONOFOLLOW)?) {
        let ent = ent?;
        if ent.file_name() != "." && ent.file_name() != ".." {
            return Ok(false);
        }
    }
    Ok(true)
}

fn move_across(src: &str, dst: &str, verbose: bool) -> bool {
    let (Some((sdir, sname)), Some((ddir, dname))) = (split(src), split(dst)) else {
        eprintln!(
            "mv: cannot move '{}' to '{}': {}",
            src,
            dst,
            Error::InvalidArgument
        );
        return false;
    };
    let sparent = match File::open_with_opath(sdir) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}", src, e);
            return false;
        }
    };
    let dparent = match File::open_with_opath(ddir) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("mv: cannot move '{}' to '{}': {}", src, dst, e);
            return false;
        }
    };
    let sm = match stat_at(&sparent, sname) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}", src, e);
            return false;
        }
    };
    if sm.is_symlink() {
        eprintln!("mv: cannot move symbolic link '{}' to another device: creating a link is not supported", src);
        return false;
    }
    let sid = id_of(&sm);
    // What a rename would have replaced: a file by a file, an empty directory by
    // a directory -- never the source itself, which POSIX leaves in place.
    match stat_at(&dparent, dname) {
        Ok(dm) => {
            if id_of(&dm) == sid {
                eprintln!("mv: '{}' and '{}' are the same file", src, dst);
                return false;
            }
            if sm.is_dir() && !dm.is_dir() {
                eprintln!(
                    "mv: cannot overwrite non-directory '{}' with directory '{}'",
                    dst, src
                );
                return false;
            }
            if !sm.is_dir() && dm.is_dir() {
                eprintln!(
                    "mv: cannot overwrite directory '{}' with non-directory '{}'",
                    dst, src
                );
                return false;
            }
            // The rename would refuse it; asked first, it costs no copy.
            if dm.is_dir() && matches!(is_empty_dir(&dparent, dname), Ok(false)) {
                eprintln!(
                    "mv: cannot move '{}' to '{}': {}",
                    src,
                    dst,
                    Error::DirectoryNotEmpty
                );
                return false;
            }
        }
        Err(Error::NotFound) => {}
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}", dst, e);
            return false;
        }
    }
    let tname = match temp_name(&dparent) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("mv: cannot move '{}' to '{}': {}", src, dst, e);
            return false;
        }
    };
    let tpath = join(ddir, &tname);
    let o = Opts {
        tool: "mv",
        verbose: false,
        no_clobber: false,
        preserve: true,
        no_deref: true,
        attrs_soft: true,
        refuse_special: true,
        excl: true,
        sync: true,
        attrs_only_whole: true,
    };
    let mut done = Done::default();
    let copied = copy::copy_tracked(src, &tpath, &o, &mut done);
    if copied && done.src_top != Some(sid) {
        eprintln!("mv: '{}' changed during the copy; not moved", src);
    }
    if !copied || done.src_top != Some(sid) {
        undo(&dparent, &tname, &tpath, done.dst_top, src);
        return false;
    }
    // The copy was made by path; the rename goes through the directory mv
    // opened, so the entry it renames must be the copy.
    match stat_at(&dparent, &tname) {
        Ok(tm) if Some(id_of(&tm)) == done.dst_top => {}
        Ok(_) | Err(Error::NotFound) => {
            eprintln!(
                "mv: '{}' is no longer the copy mv made; '{}' is kept",
                tpath, src
            );
            return false;
        }
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}; '{}' is kept", tpath, e, src);
            return false;
        }
    }
    if let Err(e) = dparent.rename_at(&tname, &dparent, dname) {
        eprintln!("mv: cannot move '{}' to '{}': {}", src, dst, e);
        undo(&dparent, &tname, &tpath, done.dst_top, src);
        return false;
    }
    // The new name, durable before the source goes.
    if let Err(e) = dparent.open_at(".", T_OREAD).and_then(|d| d.sync_all()) {
        eprintln!("mv: cannot sync '{}': {}; '{}' is kept", ddir, e, src);
        return false;
    }
    let r = Rm {
        tool: "mv",
        recursive: true,
        verbose: false,
        missing_ok: true,
        only: Some(done.copied.as_slice()),
    };
    if !rmtree::remove_at(&sparent, sname, src, false, Some(sid), &r) {
        return false;
    }
    if verbose {
        println!("'{}' -> '{}'", src, dst);
    }
    true
}

/// A name in `dir` that names nothing now: ".mv-<pid>-<n>". The copy creates it
/// new, so a name taken in between fails the copy rather than being reused.
fn temp_name(dir: &File) -> Result<String, Error> {
    let pid = libthyla_rs::identity::pid();
    for n in 0..64 {
        let t = format!(".mv-{}-{}", pid, n);
        match stat_at(dir, &t) {
            Err(Error::NotFound) => return Ok(t),
            Ok(_) => {}
            Err(e) => return Err(e),
        }
    }
    Err(Error::Exists)
}

/// Remove what a failed move made at the temporary name, if it is still what
/// the copy created; the source and the destination are not touched. Without
/// the identity the copy recorded (its own error named the path), nothing is.
fn undo(dparent: &File, tname: &str, tpath: &str, made: Option<Id>, src: &str) {
    let Some(id) = made else {
        return;
    };
    let r = Rm {
        tool: "mv",
        recursive: true,
        verbose: false,
        missing_ok: true,
        only: None,
    };
    if !rmtree::remove_at(dparent, tname, tpath, false, Some(id), &r) {
        eprintln!("mv: the copy '{}' is left; '{}' is whole", tpath, src);
    }
}
