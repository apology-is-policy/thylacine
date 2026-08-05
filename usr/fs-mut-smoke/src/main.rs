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
//   - NotFound on a removed path,
//   - the #87 resolution rows: trailing-slash / `.` / `..` leaves answer
//     their POSIX errnos via the kernel gates (the parent prefix resolves
//     kernel-side -- nothing lexical survives in libthyla-rs), with
//     survival assertions that a strip-only or pop-only regression would
//     delete the wrong object.
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
// The pool PERSISTS across boots, so every name ANY leg can leave (including
// a mid-leg FAIL on broken code) is reclaimed here -- the shared-fixture
// lesson: stale residue must not fail (or falsely pass) the NEXT boot.
fn teardown() {
    let _ = fs::remove_file(A);
    let _ = fs::remove_file(B);
    let _ = fs::remove_file("/fs-mut-smoke/rel.txt");
    let _ = fs::remove_file("/fs-mut-smoke/rel2.txt");
    // #87 section scratch. x87/z87/gone87 exist only if a BROKEN rename
    // moved something there -- and broken code can leave them as either
    // kind (the pre-#87 code renamed the DIRECTORY d87 to z87 via the
    // dropped `.`), so both removal forms are tried.
    let _ = fs::remove_file("/fs-mut-smoke/f87.txt");
    for stray in ["/fs-mut-smoke/x87", "/fs-mut-smoke/z87", "/fs-mut-smoke/gone87"] {
        let _ = fs::remove_file(stray);
        let _ = fs::remove_dir(stray);
    }
    let _ = fs::remove_dir("/fs-mut-smoke/d87");
    let _ = fs::remove_dir("/fs-mut-smoke/d87b");
    let _ = fs::remove_dir("/fs-mut-smoke/d87c");
    let _ = fs::remove_dir("/fs-mut-smoke/d87y");
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

    // --- LS-4: per-Proc cwd (relative paths resolve against dot) ---
    // chdir into the dir we own, then prove a RELATIVE create + open + a
    // "../"-bearing mutation all resolve against the cwd (not the root).
    c.ok("set_current_dir(ROOT)", libthyla_rs::env::set_current_dir(ROOT).is_ok());
    c.ok(
        "current_dir == ROOT",
        libthyla_rs::env::current_dir().ok().as_deref() == Some(ROOT),
    );
    // relative create -> /fs-mut-smoke/rel.txt (the with_parent_dir absolutize).
    let rel_wrote = match File::create("rel.txt") {
        Ok(mut f) => f.write_all(PAYLOAD).is_ok(),
        Err(_) => false,
    };
    c.ok("File::create relative + write", rel_wrote);
    // relative open -> resolves via the kernel SYS_OPEN cwd-join.
    c.ok(
        "relative read-back matches",
        read_all("rel.txt").as_deref() == Some(PAYLOAD),
    );
    // ...and it landed at the expected ABSOLUTE path.
    c.ok(
        "relative create landed at /fs-mut-smoke/rel.txt",
        read_all("/fs-mut-smoke/rel.txt").as_deref() == Some(PAYLOAD),
    );
    // a "../"-bearing relative mutation cleans against the cwd: from
    // /fs-mut-smoke/sub, "../rel2.txt" -> /fs-mut-smoke/rel2.txt.
    c.ok("set_current_dir(SUB)", libthyla_rs::env::set_current_dir(SUB).is_ok());
    let dd_wrote = match File::create("../rel2.txt") {
        Ok(mut f) => f.write_all(PAYLOAD).is_ok(),
        Err(_) => false,
    };
    c.ok("File::create ../rel2.txt + write", dd_wrote);
    c.ok(
        "dotdot-relative landed at /fs-mut-smoke/rel2.txt",
        read_all("/fs-mut-smoke/rel2.txt").as_deref() == Some(PAYLOAD),
    );
    // restore cwd to root + reclaim the relative scratch (so ROOT can rmdir).
    let _ = libthyla_rs::env::set_current_dir("/");
    let _ = fs::remove_file("/fs-mut-smoke/rel.txt");
    let _ = fs::remove_file("/fs-mut-smoke/rel2.txt");
    // --- end LS-4 ---

    // --- #87: the kernel resolves; userspace only splits ---
    // The former cwd_join_clean popped `..` and dropped `.` / trailing
    // separators lexically, so the stalk gates (#79/#81/#82/#84) never saw
    // them: remove_file("nope/../x") removed x, remove_file("f/") deleted
    // the very file the slash asserts is a directory, and an absolute
    // path's `..` was rejected outright. Now the parent prefix resolves
    // kernel-side and each caller applies its POSIX leaf row. Every leg
    // asserts the EXACT Error variant, and the survival legs assert the
    // file is still there -- which a strip-only or pop-only wrong fix
    // would fail.
    {
        use libthyla_rs::err::Error;
        c.ok("87 chdir ROOT", libthyla_rs::env::set_current_dir(ROOT).is_ok());
        c.ok("87 mkdir d87", fs::create_dir("d87").is_ok());
        let f87_wrote = match File::create("f87.txt") {
            Ok(mut f) => f.write_all(PAYLOAD).is_ok(),
            Err(_) => false,
        };
        c.ok("87 create f87.txt", f87_wrote);

        // unlink("f/") must NOT delete f (the anti-strip leg -- the old
        // code deleted it and returned Ok).
        c.ok(
            "87 remove_file(f87.txt/) -> NotADirectory",
            fs::remove_file("f87.txt/") == Err(Error::NotADirectory),
        );
        c.ok(
            "87 f87.txt SURVIVES its slash-unlink",
            read_all("f87.txt").as_deref() == Some(PAYLOAD),
        );
        c.ok(
            "87 remove_file(d87/) -> IsADirectory",
            fs::remove_file("d87/") == Err(Error::IsADirectory),
        );
        c.ok(
            "87 remove_file(nope87/) -> NotFound",
            fs::remove_file("nope87/") == Err(Error::NotFound),
        );
        // A `.` / `..` leaf: the parent resolves FIRST (Linux ordering) --
        // a dir parent reaches the EISDIR dot row; a FILE parent answers
        // ENOTDIR from the kernel #82 gate before any row runs.
        c.ok(
            "87 remove_file(d87/..) -> IsADirectory",
            fs::remove_file("d87/..") == Err(Error::IsADirectory),
        );
        c.ok(
            "87 remove_file(d87/.) -> IsADirectory",
            fs::remove_file("d87/.") == Err(Error::IsADirectory),
        );
        c.ok(
            "87 remove_file(f87.txt/..) -> NotADirectory",
            fs::remove_file("f87.txt/..") == Err(Error::NotADirectory),
        );
        // The #83-twin leg: a `..` after a MISSING component must fail
        // resolution -- the old code popped nope87 lexically and REMOVED
        // f87.txt.
        c.ok(
            "87 remove_file(nope87/../f87.txt) -> NotFound",
            fs::remove_file("nope87/../f87.txt") == Err(Error::NotFound),
        );
        c.ok(
            "87 f87.txt SURVIVES the phantom-dir unlink",
            read_all("f87.txt").as_deref() == Some(PAYLOAD),
        );
        // A REAL `..` in the prefix resolves kernel-side -- including the
        // ABSOLUTE spelling the old arm rejected with InvalidArgument.
        c.ok("87 create_dir(d87/../d87b)", fs::create_dir("d87/../d87b").is_ok());
        c.ok("87 d87b landed beside d87", fs::is_dir("/fs-mut-smoke/d87b"));
        c.ok(
            "87 absolute open through ..",
            File::open("/fs-mut-smoke/d87/../f87.txt").is_ok(),
        );
        // Plain opens ride the kernel gates too (open_stalk, #82).
        c.ok(
            "87 File::open(f87.txt/) -> NotADirectory",
            File::open("f87.txt/").err() == Some(Error::NotADirectory),
        );
        // O_CREAT rows: a trailing slash / dot leaf -> IsADirectory
        // unconditionally; the target stays intact.
        c.ok(
            "87 File::create(f87.txt/) -> IsADirectory",
            File::create("f87.txt/").err() == Some(Error::IsADirectory),
        );
        c.ok(
            "87 File::create(d87/.) -> IsADirectory",
            File::create("d87/.").err() == Some(Error::IsADirectory),
        );
        c.ok(
            "87 f87.txt intact after the create rows",
            read_all("f87.txt").as_deref() == Some(PAYLOAD),
        );
        // mkdir: a trailing slash creates (the leaf IS a directory).
        c.ok("87 create_dir(d87c/)", fs::create_dir("d87c/").is_ok());
        c.ok("87 d87c exists", fs::is_dir("/fs-mut-smoke/d87c"));
        // rmdir rows: slash strips (REMOVEDIR enforces -- a file answers
        // ENOTDIR); `.` is the POSIX EINVAL; `..` is Linux's ENOTEMPTY.
        c.ok(
            "87 remove_dir(f87.txt/) -> NotADirectory",
            fs::remove_dir("f87.txt/") == Err(Error::NotADirectory),
        );
        c.ok(
            "87 remove_dir(.) -> InvalidArgument",
            fs::remove_dir(".") == Err(Error::InvalidArgument),
        );
        c.ok(
            "87 remove_dir(d87/..) -> DirectoryNotEmpty",
            fs::remove_dir("d87/..") == Err(Error::DirectoryNotEmpty),
        );
        c.ok("87 remove_dir(d87c/)", fs::remove_dir("d87c/").is_ok());
        // rename rows: the do_renameat2 trailing-slash rule -- a file
        // source fails ENOTDIR whichever side carries the slash; an
        // absent source is ENOENT first; a dir source with a slash
        // proceeds; a `.` / `..` leaf is Linux's EBUSY.
        c.ok(
            "87 rename(f87.txt/, x87) -> NotADirectory",
            fs::rename("f87.txt/", "x87") == Err(Error::NotADirectory),
        );
        c.ok(
            "87 rename(f87.txt, gone87/) -> NotADirectory",
            fs::rename("f87.txt", "gone87/") == Err(Error::NotADirectory),
        );
        c.ok(
            "87 rename(nope87/, x87) -> NotFound",
            fs::rename("nope87/", "x87") == Err(Error::NotFound),
        );
        c.ok(
            "87 rename(d87/., z87) -> Busy",
            fs::rename("d87/.", "z87") == Err(Error::Busy),
        );
        c.ok("87 rename(d87b/, d87y) dir-source slash", fs::rename("d87b/", "d87y").is_ok());
        c.ok("87 d87y exists", fs::is_dir("/fs-mut-smoke/d87y"));
        // Section teardown (assertive -- these prove the plain rows still
        // work after everything above), then restore the cwd.
        c.ok("87 remove_dir(d87y)", fs::remove_dir("d87y").is_ok());
        c.ok("87 remove_file(f87.txt)", fs::remove_file("f87.txt").is_ok());
        c.ok("87 remove_dir(d87)", fs::remove_dir("d87").is_ok());
        let _ = libthyla_rs::env::set_current_dir("/");
    }
    // --- end #87 ---

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
