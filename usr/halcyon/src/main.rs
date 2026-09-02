// halcyon (the binary) -- the syscalling half of the session tool (H-4a-2).
//
// `halcyon layout save <name>` reads the live compositor tree from
// /dev/tapestry (the `layout` dump + each leaf's `pane/<id>/tag`), folds it
// into a libhalcyon::layout tree, serializes it to `halcyon-layout v1`, and
// writes it durably into the SESSION tier ($HOME/lib/halcyon/layouts/<name>) --
// the user's own namespace, run as the user (HALCYON.md 13.7, the D decision:
// no CAP, no server verb). Restore lands at H-4b.
//
// All decision logic (dispatch, name validation, path building) lives in the
// pure lib (src/lib.rs); this file is I/O only.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;
use alloc::vec::Vec;

use halcyon::{parse_cmd, session_dir_chain, session_layout_path, Cmd, CmdError};
use libhalcyon::layout;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{self, Write};
use libthyla_rs::{env, eprintln, t_fsync};

const TAPESTRY_LAYOUT: &str = "/dev/tapestry/layout";
/// The compositor tree dump is a few KB; cap generously against a runaway file.
const LAYOUT_READ_CAP: usize = 128 * 1024;
/// A tag file is one command line plus a trailing '\n'.
const TAG_READ_CAP: usize = layout::MAX_TAG_LEN + 8;

const USAGE: &str = "\
usage: halcyon layout save <name>
  Save the current Halcyon pane layout to $HOME/lib/halcyon/layouts/<name>.
  <name> is one path component: letters/digits/._- , no leading dot.

  halcyon layout restore <name>   (H-4b -- not yet implemented)
  halcyon --help
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}

fn run() -> i64 {
    let args = env::args();
    let mut toks: Vec<&str> = Vec::new();
    for i in 1..args.len() {
        match args.get_str(i) {
            Some(s) => toks.push(s),
            None => {
                eprintln!("halcyon: argument {} is not UTF-8", i);
                return 2;
            }
        }
    }
    match parse_cmd(&toks) {
        Ok(Cmd::Help) => {
            io::out(USAGE.as_bytes());
            0
        }
        Ok(Cmd::LayoutSave { name }) => layout_save(name),
        Ok(Cmd::LayoutRestore { .. }) => {
            eprintln!("halcyon: layout restore is not yet implemented (H-4b)");
            1
        }
        Err(e) => {
            report_cmd_error(e);
            2
        }
    }
}

fn report_cmd_error(e: CmdError) {
    match e {
        CmdError::UnknownCommand => eprintln!("halcyon: unknown command (try `halcyon --help`)"),
        CmdError::BadLayoutVerb => eprintln!("halcyon: layout: expected `save` or `restore`"),
        CmdError::MissingName => eprintln!("halcyon: layout: missing <name>"),
        CmdError::ExtraOperand => eprintln!("halcyon: layout: too many operands"),
        CmdError::BadName => {
            eprintln!("halcyon: invalid layout name (letters/digits/._- , no leading dot)")
        }
    }
}

fn layout_save(name: &str) -> i64 {
    let home = match env::var("HOME") {
        Some(h) => {
            let h = h.trim();
            if h.is_empty() {
                eprintln!("halcyon: $HOME is empty");
                return 1;
            }
            String::from(h)
        }
        None => {
            eprintln!("halcyon: $HOME is unset -- run me from a logged-in session");
            return 1;
        }
    };

    // The compositor tree dump. The offset-0 read snaps a consistent tree
    // (server-side), so a mutation cannot straddle the read.
    let render = match read_capped(TAPESTRY_LAYOUT, LAYOUT_READ_CAP) {
        Ok(bytes) => match String::from_utf8(bytes) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("halcyon: {}: not valid UTF-8", TAPESTRY_LAYOUT);
                return 1;
            }
        },
        Err(e) => {
            eprintln!("halcyon: {}: {}", TAPESTRY_LAYOUT, e);
            return 1;
        }
    };

    // Fold it into a layout tree, resolving each leaf's tag from pane/<id>/tag.
    let tree = match layout::from_render_text(&render, read_tag) {
        Ok(t) => t,
        Err(_) => {
            eprintln!("halcyon: could not parse the compositor layout");
            return 1;
        }
    };
    let text = layout::serialize(&tree);

    // Durable write into the session tier ($HOME/lib/halcyon/layouts/<name>).
    if !mkdir_p(&home) {
        eprintln!("halcyon: could not create {}/lib/halcyon/layouts", home);
        return 1;
    }
    let path = session_layout_path(&home, name);
    if !durable_write(&path, text.as_bytes()) {
        eprintln!("halcyon: could not write {}", path);
        return 1;
    }
    0
}

/// Resolve a leaf pane's tag (its command line) from `pane/<id>/tag`.
/// Fail-soft: any read/UTF-8 failure yields an empty tag, so a save never
/// aborts on one unreadable leaf -- it becomes an empty placeholder.
fn read_tag(id: u32) -> String {
    let path = alloc::format!("/dev/tapestry/pane/{}/tag", id);
    match read_capped(&path, TAG_READ_CAP) {
        // The tag file appends exactly one '\n'; strip only the trailing
        // newline(s), never internal/leading bytes (a tag is a command line
        // whose spaces are meaningful).
        Ok(bytes) => match String::from_utf8(bytes) {
            Ok(s) => String::from(s.trim_end_matches(['\n', '\r'])),
            Err(_) => String::new(),
        },
        Err(_) => String::new(),
    }
}

fn read_capped(path: &str, cap: usize) -> Result<Vec<u8>> {
    let mut f = File::open(path)?;
    io::slurp_capped(&mut f, cap)
}

/// mkdir -p `<home>/lib/halcyon/layouts`, ignoring already-existing components
/// (the kernel create is exclusive; a re-save must not fail on the second run).
fn mkdir_p(home: &str) -> bool {
    for dir in session_dir_chain(home) {
        match fs::create_dir(&dir) {
            Ok(()) | Err(Error::Exists) => {}
            Err(_) => return false,
        }
    }
    true
}

/// The aurora-config durability discipline (gfx-status cfg-2a): write-tmp,
/// content fsync, atomic rename, then a STRICT metadata fsync on the SAME
/// OWRITE fd -- the fid follows the file across the rename, and a fresh OREAD
/// reopen would fail SYS_FSYNC's RIGHT_WRITE gate. A crash before the
/// post-rename barrier rolls back to the old file, never a torn one.
fn durable_write(path: &str, bytes: &[u8]) -> bool {
    let mut tmp = String::from(path);
    tmp.push_str(".tmp");
    let mut f = match File::create(&tmp) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(bytes).is_err() {
        return false;
    }
    let _ = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
    if fs::rename(&tmp, path).is_err() {
        return false;
    }
    let ok = unsafe { t_fsync(f.as_raw_fd() as i64, 0) == 0 };
    drop(f);
    ok
}
