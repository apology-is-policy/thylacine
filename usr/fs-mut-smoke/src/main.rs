// /fs-mut-smoke -- LS-3b runtime verification of the libthyla-rs fs-mutation
// surface against the live (post-pivot) Stratum FS. joey spawns it once,
// post-pivot, and gates the boot on a 0 exit.
//
// What it proves (the first runtime exercise of these libthyla-rs APIs):
//   - fs::create_dir at depth (a 2-level mkdir exercises with_parent_dir's
//     T_OPATH intermediate walk + create-under-an-owned-parent),
//   - File::create at depth >= 2 (the OREAD->T_OPATH parent-walk fix: a
//     RIGHT_WRITE parent is required by SYS_WALK_CREATE) + write + read-back,
//   - fs::rename (POSIX atomic-replace; same-Dev),
//   - fs::remove_file + fs::remove_dir,
//   - NotFound on a removed path.
//
// IDEMPOTENT: the Stratum pool PERSISTS across reboots, so a crashed prior run
// could leave a stale /fs-mut-smoke tree. The scratch shape is fixed and
// owned by this binary, so we best-effort tear down the known leaves first,
// then build fresh, then tear down at the end. A residual tree from an
// abnormal exit is reclaimed by the next boot's pre-clean.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;

use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

const ROOT: &str = "/fs-mut-smoke";
const SUB: &str = "/fs-mut-smoke/sub";
const A: &str = "/fs-mut-smoke/sub/a.txt";
const B: &str = "/fs-mut-smoke/sub/b.txt";
const PAYLOAD: &[u8] = b"life-support-3b\n";

struct Checker {
    fails: usize,
    checks: usize,
}

impl Checker {
    fn ok(&mut self, label: &str, cond: bool) {
        self.checks += 1;
        if cond {
            t_putstr(&format!("fs-mut-smoke: {} ok\n", label));
        } else {
            self.fails += 1;
            t_putstr(&format!("fs-mut-smoke: {} FAILED\n", label));
        }
    }
}

// Best-effort removal of the known scratch tree (deepest first). Errors are
// ignored -- this is the idempotent pre-clean / post-clean, not an assertion.
fn teardown() {
    let _ = fs::remove_file(A);
    let _ = fs::remove_file(B);
    let _ = fs::remove_dir(SUB);
    let _ = fs::remove_dir(ROOT);
}

fn read_all(path: &str) -> Option<Vec<u8>> {
    let mut f = File::open(path).ok()?;
    let mut v = Vec::new();
    f.read_to_end(&mut v).ok()?;
    Some(v)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut c = Checker { fails: 0, checks: 0 };

    // Reclaim any stale tree from a prior abnormal exit (see module header).
    teardown();

    // mkdir /fs-mut-smoke (1-level, under the pivot root via FROM_ROOT).
    c.ok("create_dir root", fs::create_dir(ROOT).is_ok());
    // mkdir /fs-mut-smoke/sub (2-level: with_parent_dir walks ROOT as T_OPATH,
    // then creates under the RIGHT_WRITE-bearing parent).
    c.ok("create_dir sub (depth 2)", fs::create_dir(SUB).is_ok());

    // create-new must fail on an existing dir (exclusive create).
    c.ok("create_dir existing -> err", fs::create_dir(ROOT).is_err());

    // File::create at depth 3 (the OREAD->T_OPATH parent-walk fix) + write.
    let wrote = match File::create(A) {
        Ok(mut f) => f.write_all(PAYLOAD).is_ok(),
        Err(_) => false,
    };
    c.ok("File::create depth 3 + write", wrote);

    // read-back round-trip.
    c.ok(
        "read-back matches",
        read_all(A).as_deref() == Some(PAYLOAD),
    );

    // rename a.txt -> b.txt (same dir, same Dev): b appears, a is gone.
    c.ok("rename", fs::rename(A, B).is_ok());
    c.ok("renamed content intact", read_all(B).as_deref() == Some(PAYLOAD));
    c.ok("rename source gone", File::open(A).is_err());

    // remove the file, then the (now-empty) dirs.
    c.ok("remove_file", fs::remove_file(B).is_ok());
    c.ok("removed file gone", File::open(B).is_err());
    c.ok("remove_dir sub", fs::remove_dir(SUB).is_ok());
    c.ok("remove_dir root", fs::remove_dir(ROOT).is_ok());
    // remove of an absent dir is an error (NotFound), not a silent success.
    c.ok("remove_dir absent -> err", fs::remove_dir(ROOT).is_err());

    // Defensive: leave nothing behind even if an assertion above bailed early.
    teardown();

    if c.fails == 0 {
        t_putstr(&format!("fs-mut-smoke: all OK ({} checks)\n", c.checks));
        0
    } else {
        t_putstr(&format!(
            "fs-mut-smoke: {} of {} checks FAILED\n",
            c.fails, c.checks
        ));
        1
    }
}
