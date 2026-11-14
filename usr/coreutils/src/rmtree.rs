// Removing a tree by descriptor, for rm -r and mv. Each entry is opened
// relative to its directory's handle without following a final link, and its
// kind and identity are read from that handle; a directory's entries are read
// from a reading open of the same directory (".") and removed relative to it.
// So a link swapped into the tree mid-walk cannot steer the walk out of it, as
// a path walked again from the top could. The unlink itself still names the
// entry (there is no unlink by identity): an entry replaced between its check
// and its unlink is the one removed, and a directory replaced so only if it is
// empty. Recursion depth follows the tree's depth.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::err::Error;
use libthyla_rs::fs::{File, ReadDir};
use libthyla_rs::{eprintln, println, T_ONOFOLLOW, T_OPATH, T_OREAD};

use crate::copy::{id_of, join, Id};

/// How a removal reports, and what it may remove.
pub struct Rm<'a> {
    pub tool: &'static str,
    /// A directory's contents go too; without it a directory is refused.
    pub recursive: bool,
    /// Say each failure and go on (mv); otherwise the first failure ends the
    /// walk and is returned for the caller to say once (rm).
    pub keep_going: bool,
    pub verbose: bool,
    /// An entry already gone counts as removed (rm -f; mv).
    pub missing_ok: bool,
    /// Remove only these entries, sorted (mv: what it copied). Anything else is
    /// named and left, and with it each directory above it.
    pub only: Option<&'a [Id]>,
}

impl Rm<'_> {
    /// A failure at `disp`: Ok(true) for an entry already gone when that is
    /// allowed, Ok(false) once said when going on, else the error.
    fn fail(&self, disp: &str, e: Error) -> Result<bool, Error> {
        if self.missing_ok && matches!(e, Error::NotFound) {
            return Ok(true);
        }
        if self.keep_going {
            eprintln!("{}: cannot remove '{}': {}", self.tool, disp, e);
            return Ok(false);
        }
        Err(e)
    }
}

/// Remove the entry `name` of the directory `parent` (a navigation handle), and
/// with `recursive` everything below it; `disp` names it in messages. `follow`
/// follows a final link (an operand written with a trailing slash names a
/// directory, POSIX 4.13), and `expect` is the identity the entry must still
/// have. Ok(true) when it is gone, Ok(false) when something was left (each
/// said), Err at the first failure when not going on.
pub fn remove_at(
    parent: &File,
    name: &str,
    disp: &str,
    follow: bool,
    expect: Option<Id>,
    r: &Rm,
) -> Result<bool, Error> {
    let omode = if follow {
        T_OPATH
    } else {
        T_OPATH | T_ONOFOLLOW
    };
    let nav = match parent.open_at(name, omode) {
        Ok(f) => f,
        Err(e) => return r.fail(disp, e),
    };
    let m = match nav.metadata() {
        Ok(m) => m,
        Err(e) => return r.fail(disp, e),
    };
    let id = id_of(&m);
    if expect.is_some_and(|x| x != id) {
        eprintln!(
            "{}: '{}' is no longer the file it was; not removed",
            r.tool, disp
        );
        return Ok(false);
    }
    if r.only.is_some_and(|set| set.binary_search(&id).is_err()) {
        eprintln!("{}: '{}' was not copied; left in place", r.tool, disp);
        return Ok(false);
    }
    if !m.is_dir() {
        if follow {
            return r.fail(disp, Error::NotADirectory);
        }
        return match parent.unlink_at(name, false) {
            Ok(()) => {
                if r.verbose {
                    println!("removed '{}'", disp);
                }
                Ok(true)
            }
            Err(e) => r.fail(disp, e),
        };
    }
    if !r.recursive {
        return r.fail(disp, Error::IsADirectory);
    }
    let names = match names_in(&nav) {
        Ok(v) => v,
        Err(e) => return r.fail(disp, e),
    };
    let mut whole = true;
    for n in &names {
        if !remove_at(&nav, n, &join(disp, n), false, None, r)? {
            whole = false;
        }
    }
    if !whole {
        return Ok(false);
    }
    match parent.unlink_at(name, true) {
        Ok(()) => {
            if r.verbose {
                println!("removed directory '{}'", disp);
            }
            Ok(true)
        }
        Err(e) => r.fail(disp, e),
    }
}

/// The names in the directory `nav` is a handle on, "." and ".." left out, read
/// whole before any goes: a server need not keep its listing's place while its
/// entries are removed.
fn names_in(nav: &File) -> Result<Vec<String>, Error> {
    let mut v = Vec::new();
    for ent in ReadDir::from_file(nav.open_at(".", T_OREAD)?) {
        let n = ent?.into_file_name();
        if n != "." && n != ".." {
            v.push(n);
        }
    }
    Ok(v)
}
