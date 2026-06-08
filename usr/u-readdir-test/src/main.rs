// /u-readdir-test -- U-6e-b-1 directory-enumeration boot probe.
//
// Runs PRE-pivot (devramfs root). Drives libthyla_rs::fs::read_dir against the
// flat boot ramfs and asserts the contract the kernel devramfs_readdir + the
// ReadDir iterator + the #929 root-open fix establish:
//
//   1. read_dir("/") enumerates the flat root: welcome / version / srv / proc /
//      this binary are all present.
//   2. The synthetic mount-point dirs srv/proc type as directories (is_dir),
//      and welcome types as a regular file (is_file) -- the qid.type byte the
//      kernel emits per dirent, read with no extra fstat.
//   3. #929 is closed: fs::is_dir("/") and fs::metadata("/") resolve the
//      Territory root (via the stalk resolver), no longer InvalidArgument.
//   4. An empty synthetic dir (/srv) enumerates to zero entries (clean EOD).
//   5. read_dir on a regular file errors on the first item (the kernel readdir
//      rejects a non-directory Spoor).
//   6. A missing path fails to open.
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::fs;
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1 + 2. Enumerate the flat boot root; record names + the per-entry type.
    let mut names: Vec<String> = Vec::new();
    let mut srv_is_dir = false;
    let mut proc_is_dir = false;
    let mut welcome_is_file = false;
    match fs::read_dir("/") {
        Ok(rd) => {
            for ent in rd {
                match ent {
                    Ok(e) => {
                        let n = e.file_name();
                        if n == "srv" && e.is_dir() {
                            srv_is_dir = true;
                        }
                        if n == "proc" && e.is_dir() {
                            proc_is_dir = true;
                        }
                        if n == "welcome" && e.is_file() {
                            welcome_is_file = true;
                        }
                        names.push(String::from(n));
                    }
                    Err(_) => return fail("read_dir / yielded an error entry"),
                }
            }
        }
        Err(_) => return fail("read_dir / failed to open the root"),
    }
    if !contains(&names, "welcome") {
        return fail("root missing welcome");
    }
    if !contains(&names, "version") {
        return fail("root missing version");
    }
    if !contains(&names, "srv") {
        return fail("root missing srv");
    }
    if !contains(&names, "proc") {
        return fail("root missing proc");
    }
    if !contains(&names, "u-readdir-test") {
        return fail("root missing self");
    }
    if !srv_is_dir {
        return fail("srv not typed as a directory");
    }
    if !proc_is_dir {
        return fail("proc not typed as a directory");
    }
    if !welcome_is_file {
        return fail("welcome not typed as a file");
    }
    // The live boot corpus is dozens of entries; a tiny count means a parse /
    // pagination bug truncated the listing.
    if names.len() < 10 {
        return fail("root entry count implausibly low");
    }

    // 3. #929: the root resolves through metadata/is_dir now.
    if !fs::is_dir("/") {
        return fail("fs::is_dir(\"/\") false (#929)");
    }
    match fs::metadata("/") {
        Ok(m) => {
            if !m.is_dir() {
                return fail("metadata(\"/\") not a directory");
            }
        }
        Err(_) => return fail("metadata(\"/\") errored (#929)"),
    }

    // (The empty-directory case is covered by the devramfs.readdir_synth_dir_empty
    // kernel test, which drives the bare synthetic Spoor directly. We deliberately
    // do NOT read_dir("/srv") here: by the time this probe runs, joey has mounted
    // the devsrv service registry onto /srv, so a userspace open crosses the mount
    // into devsrv (connect semantics, no directory readdir) -- enumerating a
    // mounted /srv / /proc is a separate devsrv/devproc readdir feature.)

    // 4. read_dir on a regular file: the open succeeds (stalk resolves it) but
    //    the kernel readdir rejects a non-directory -> first item is Err.
    if let Ok(mut rd) = fs::read_dir("/welcome") {
        match rd.next() {
            Some(Err(_)) => {}
            _ => return fail("read_dir on a file should error on the first item"),
        }
    }

    // 5. A nonexistent directory fails to open.
    if fs::read_dir("/no-such-directory-xyz").is_ok() {
        return fail("read_dir on a missing path should fail");
    }

    t_putstr("u-readdir-test: all OK\n");
    0
}

fn contains(v: &[String], want: &str) -> bool {
    v.iter().any(|s| s == want)
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-readdir-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
