// /symlink-probe -- the DISTRO D-1 gate: symlinks resolve on the REAL FS.
//
// The kernel's stalk.symlink_* tests drive a FIXTURE Dev whose qid.type comes
// from a static table. Nothing there proves a link created by a real 9P server,
// walked over the wire, with QTSYMLINK arriving off dev9p's qid decode, actually
// resolves -- which is the only claim the DISTRO arc rests on. This runs
// post-pivot against the live Stratum pool and gates the boot on it.
//
// SELF-CONTAINED BY CONSTRUCTION. Every link it walks it created itself, one
// boot earlier than it reads them. Two reasons, both learned the hard way here:
// a pool-resident fixture goes STALE under THYLACINE_MKFS_PRESERVE=1 (#126 --
// populate is skipped, so a probe silently tests the previous build), and a leg
// that depends on the bake layout goes VACUOUS when the layout moves (the
// probe79 discipline). Creation is LOOM_OP_SYMLINK: SYS_WALK_CREATE has no
// symlink mode, so the Loom ring IS the v1.0 in-guest creation surface, and this
// is its first consumer -- the op had zero coverage before D-1.
//
// The battery (docs/DISTRO.md section 4.5). Each leg names what a regression
// would look like, because "it passed" is not information unless the leg could
// have failed:
//
//   A follow        a link resolves at all                  -> ENOENT if not
//   B chain         link -> link -> file, 2 expansions       -> the bound is 40
//   C mid-path      a link as a NON-final component          -> the Alpine shape
//   D loop          a self-referential link                  -> ELOOP, not a hang
//   E dangling      a link to nothing                        -> ENOENT, not EIO
//   F stat/lstat    the SAME name, two records               -> different qids
//   G nofollow      O_NOFOLLOW on a link                     -> ELOOP (Linux)
//   H nofollow-neg  O_NOFOLLOW on a NON-link                 -> must still open
//   I slash         trailing '/' on link-to-dir / link-to-file
//   J unlink        removes the LINK, never the target
//   L re-anchor     an absolute target anchors at the ROOT, not at the base
//   K current-root  ...and at the root the caller has NOW (I-28)
//
// Legs L and K together are the I-28 containment proof, and they are two legs
// because a revert probe proved one is not enough.
//
// K is the chroot inversion: `l_abs` -> /<work>/target and `l_root` -> /target,
// measured on both sides of a real chroot into <work>. Before it exactly one
// resolves; after it, exactly the other. Nothing about the links changed, only
// the root did, so a resolver that captured the root once and reused it fails
// the after-legs. That is a real property -- but it is NOT the re-anchor.
//
// Deleting the re-anchor from stalk_expand_link leaves every K leg green,
// because every path here is opened from the Territory root (and the LS-4 join
// makes cwd-relative paths absolute, so those start at the root too). The base
// already IS the root; storing the root over it changes nothing. The one caller
// whose base is NOT the root is SYS_OPEN with an explicit dirfd -- so leg L
// opens through a dirfd, and asserts the CONTENT it reads back. It fails
// closed (ENOENT) the moment the re-anchor goes.
//
// This is the #83 lesson relearned on the same resolver: probe83's own comment
// says it runs from a NON-root cwd because "the trivial case would pass either
// way." A containment leg that starts at the root is that trivial case.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;

use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::Read;
use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe};
use libthyla_rs::{t_chroot, t_exits, t_putstr};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

const WORK: &str = "/d1-symlink";
const TARGET: &str = "/d1-symlink/target";
const SUB: &str = "/d1-symlink/sub";
const INNER: &str = "/d1-symlink/sub/inner";

// Distinct lengths, so a leg that resolved to the WRONG file fails on size
// alone -- a same-length payload would let a mis-resolution read as success.
const TARGET_BODY: &[u8] = b"d1-target-body\n";
const INNER_BODY: &[u8] = b"d1-inner\n";

struct Checker {
    checks: usize,
    fails: usize,
}

impl Checker {
    fn ok(&mut self, label: &str, cond: bool) {
        self.checks += 1;
        if cond {
            t_putstr(&format!("symlink-probe: {} ok\n", label));
        } else {
            self.fails += 1;
            t_putstr(&format!("symlink-probe: {} FAILED\n", label));
        }
    }

    // An expected-failure leg. Reports the errno actually seen, because
    // "FAILED" without the observed value costs a whole boot to diagnose.
    fn errs(&mut self, label: &str, got: Result<()>, want: Error) {
        self.checks += 1;
        match got {
            Err(e) if e == want => {
                t_putstr(&format!("symlink-probe: {} ok\n", label));
            }
            Err(e) => {
                self.fails += 1;
                t_putstr(&format!(
                    "symlink-probe: {} FAILED (errno {} want {})\n",
                    label,
                    e.as_errno(),
                    want.as_errno()
                ));
            }
            Ok(()) => {
                self.fails += 1;
                t_putstr(&format!("symlink-probe: {} FAILED (succeeded)\n", label));
            }
        }
    }
}

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

fn read_all(path: &str) -> Result<Vec<u8>> {
    let mut f = File::open(path)?;
    let mut out = Vec::new();
    let mut buf = [0u8; 256];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n]);
    }
    Ok(out)
}

// Reduce an open to just its error, for the expected-failure legs.
fn open_err(path: &str) -> Result<()> {
    File::open(path).map(|_| ())
}

/// Mint `name` -> `target` under the registered O_PATH directory (handle 0).
///
/// The kernel splits ONE pinned span at `name_len`, so both strings are copied
/// adjacently into the registered buffer and the split is handed over in the
/// SQE. It validates `0 < split < total` at submit -- neither half may be empty.
fn mklink(ring: &Ring, buf: &mut RegisteredBuffer, name: &str, target: &str) -> Result<()> {
    let n = name.len();
    let t = target.len();
    {
        let slice = buf.as_mut_slice();
        if n + t > slice.len() {
            return Err(Error::InvalidArgument);
        }
        slice[..n].copy_from_slice(name.as_bytes());
        slice[n..n + t].copy_from_slice(target.as_bytes());
    }
    let sqe = Sqe::symlink(0, 0, 0, n as u32, (n + t) as u32, 0, 0xD1_0000 + n as u64);
    ring.submit_one_wait(&sqe)?.ok().map(|_| ())
}

// Best-effort teardown of a previous boot's tree. The pool PERSISTS across
// reboots, so a run that died mid-way leaves the shape behind; every name this
// probe owns is removed before it builds, and errors are ignored because
// "absent" is the expected case on a clean pool.
fn pre_clean() {
    for name in [
        "l_simple", "l_chain", "l_dir", "l_abs", "l_root", "l_self", "l_dead", "l_nolink",
    ] {
        let _ = fs::remove_file(&format!("{}/{}", WORK, name));
    }
    let _ = fs::remove_file(INNER);
    let _ = fs::remove_file(TARGET);
    let _ = fs::remove_dir(SUB);
    let _ = fs::remove_dir(WORK);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("symlink-probe: starting (DISTRO D-1 gate; live FS)\n");
    let mut c = Checker { checks: 0, fails: 0 };

    pre_clean();

    // --- the fixture, built fresh on the real FS -------------------------
    if fs::create_dir(WORK).is_err() {
        fail("symlink-probe: FAIL -- create_dir work\n");
    }
    if fs::create_dir(SUB).is_err() {
        fail("symlink-probe: FAIL -- create_dir sub\n");
    }
    for (path, body) in [(TARGET, TARGET_BODY), (INNER, INNER_BODY)] {
        match File::create(path) {
            Ok(mut f) => {
                use libthyla_rs::io::Write;
                if f.write_all(body).is_err() {
                    fail("symlink-probe: FAIL -- write fixture body\n");
                }
            }
            Err(_) => fail("symlink-probe: FAIL -- create fixture file\n"),
        }
    }

    // --- the Loom ring, and the links ------------------------------------
    let ring = match Ring::setup(8, 0) {
        Ok(r) => r,
        Err(_) => fail("symlink-probe: FAIL -- Ring::setup\n"),
    };
    let mut buf = match RegisteredBuffer::new(4096) {
        Ok(b) => b,
        Err(_) => fail("symlink-probe: FAIL -- RegisteredBuffer::new\n"),
    };
    if ring.register_buffers(&[buf.buf_reg()]).is_err() {
        fail("symlink-probe: FAIL -- register_buffers\n");
    }
    // O_PATH on the work directory: born RIGHT_READ|RIGHT_WRITE (the create-
    // from-an-O_PATH-base pattern), which is exactly what LOOM_OP_SYMLINK's
    // submit-time RIGHT_WRITE gate wants. Held for the ring's lifetime.
    let dir = match File::open_with_opath(WORK) {
        Ok(f) => f,
        Err(_) => fail("symlink-probe: FAIL -- O_PATH open of the work dir\n"),
    };
    if ring.register_handles(&[dir.as_raw_fd()]).is_err() {
        fail("symlink-probe: FAIL -- register_handles\n");
    }

    // Relative targets resolve against the link's OWN directory; absolute ones
    // re-anchor at the caller's Territory root (leg K).
    let links: [(&str, &str); 7] = [
        ("l_simple", "target"),
        ("l_chain", "l_simple"),
        ("l_dir", "sub"),
        ("l_self", "l_self"),
        ("l_dead", "no-such-name"),
        ("l_abs", TARGET), // resolves ONLY before the chroot
        ("l_root", "/target"), // resolves ONLY after it
    ];
    for (name, target) in links {
        if let Err(e) = mklink(&ring, &mut buf, name, target) {
            t_putstr(&format!(
                "symlink-probe: FAIL -- LOOM_OP_SYMLINK {} -> {} errno {}\n",
                name,
                target,
                e.as_errno()
            ));
            unsafe { t_exits(1) }
        }
    }
    t_putstr("symlink-probe: 7 links minted via LOOM_OP_SYMLINK\n");

    // --- A: a link resolves ----------------------------------------------
    c.ok(
        "A follow",
        read_all(&format!("{}/l_simple", WORK)).as_deref() == Ok(TARGET_BODY),
    );

    // --- B: link -> link -> file. Two expansions in one resolution, so a
    //     resolver that expanded only the FIRST link would answer ENOENT here
    //     while leg A stayed green.
    c.ok(
        "B chain",
        read_all(&format!("{}/l_chain", WORK)).as_deref() == Ok(TARGET_BODY),
    );

    // --- C: a link as a NON-final component. This is the shape that actually
    //     dominates a stock rootfs (/usr/lib -> /lib and friends); the final-
    //     component legs above would all pass with mid-path expansion missing.
    c.ok(
        "C mid-path",
        read_all(&format!("{}/l_dir/inner", WORK)).as_deref() == Ok(INNER_BODY),
    );

    // --- D: the bound. A cycle must terminate as ELOOP -- the failure mode
    //     this guards against is not a wrong errno, it is a kernel that never
    //     returns.
    c.errs(
        "D loop",
        open_err(&format!("{}/l_self", WORK)),
        Error::SymlinkLoop,
    );

    // --- E: a dangling link is ENOENT (the link exists; its target does not).
    c.errs(
        "E dangling",
        open_err(&format!("{}/l_dead", WORK)),
        Error::NotFound,
    );

    // --- F: the same name, two records. Sizes AND qids must differ: a link's
    //     own size is its target string's length, and it is a distinct object
    //     from what it points at. Comparing only the type bits would pass on a
    //     kernel that reported the right mode for the wrong file.
    let simple = format!("{}/l_simple", WORK);
    match (fs::metadata(&simple), File::open_link(&simple).and_then(|f| f.metadata())) {
        (Ok(st), Ok(lst)) => {
            c.ok("F stat follows", st.is_file() && !st.is_symlink());
            c.ok("F stat size", st.len() == TARGET_BODY.len() as u64);
            c.ok("F lstat is a link", lst.is_symlink() && !lst.is_file());
            c.ok("F lstat size", lst.len() == "target".len() as u64);
            c.ok("F distinct qids", st.qid_path() != lst.qid_path());
        }
        _ => {
            c.checks += 5;
            c.fails += 5;
            t_putstr("symlink-probe: F stat/lstat FAILED (a stat errored)\n");
        }
    }

    // --- G/H: O_NOFOLLOW. Positive AND negative -- a kernel that answered
    //     ELOOP for every NOFOLLOW open would pass G alone.
    c.errs(
        "G nofollow on a link",
        File::open_nofollow(&simple).map(|_| ()),
        Error::SymlinkLoop,
    );
    c.ok(
        "H nofollow on a non-link",
        File::open_nofollow(TARGET).is_ok(),
    );

    // --- I: trailing '/'. On a link to a directory it must FOLLOW and succeed;
    //     on a link to a file it is ENOTDIR (POSIX 4.13 -- the slash asserts a
    //     directory, and that assertion is about the TARGET).
    c.ok(
        "I slash on link-to-dir",
        File::open(&format!("{}/l_dir/", WORK)).is_ok(),
    );
    c.errs(
        "I slash on link-to-file",
        open_err(&format!("{}/l_simple/", WORK)),
        Error::NotADirectory,
    );
    // The slash also OVERRIDES O_NOFOLLOW: it can only be checked by following.
    c.ok(
        "I slash overrides nofollow",
        File::open_link(&format!("{}/l_dir/", WORK))
            .and_then(|f| f.metadata())
            .map(|m| m.is_dir() && !m.is_symlink())
            == Ok(true),
    );

    // --- J: unlink removes the LINK. The survival half is the load-bearing
    //     one: a resolver that followed before unlinking would delete the
    //     TARGET and this leg would still see l_simple gone.
    c.ok("J unlink the link", fs::remove_file(&simple).is_ok());
    c.ok(
        "J target survives",
        read_all(TARGET).as_deref() == Ok(TARGET_BODY),
    );
    c.errs("J link is gone", open_err(&simple), Error::NotFound);

    // --- L: the RE-ANCHOR, walked from a DIRFD. This leg exists because a
    //     revert probe proved leg K below does not discriminate it.
    //
    //     Every open above -- and every cwd-relative one, since the LS-4 join
    //     makes those absolute and resolves them from the root too -- starts
    //     resolution AT the Territory root. So the base is already the root and
    //     "re-anchor at the root" is a no-op there: deleting the re-anchor from
    //     stalk_expand_link leaves all 23 other legs green. The ONLY caller
    //     whose base is not the root is a walk from an explicit directory
    //     handle, so that is the only shape that can see the store happen.
    //
    //     The vehicle is SYS_OPEN with an explicit dirfd -- NOT t_walk_open,
    //     which is a single-hop twin that bypasses stalk entirely and does not
    //     expand at all (task #184; a first draft of this leg used it and
    //     passed against a kernel with the re-anchor deleted, because opening
    //     the LINK also returns a handle).
    //
    //     `l_abs` from the WORK dirfd; target /d1-symlink/target. Correct:
    //     re-anchor at the root, resolve, read TARGET_BODY. Without the
    //     re-anchor: resolve d1-symlink/target under WORK -> ENOENT.
    //
    //     Assert the CONTENT. A bare "it opened" is satisfiable by resolving
    //     something else entirely, which is the trap this whole leg exists to
    //     escape.
    {
        let name = b"l_abs";
        let fd = unsafe {
            libthyla_rs::t_open(
                dir.as_raw_fd() as i64,
                name.as_ptr(),
                name.len(),
                libthyla_rs::T_OREAD,
            )
        };
        let mut body = [0u8; 64];
        let n = if fd >= 0 {
            unsafe { libthyla_rs::t_read(fd, body.as_mut_ptr(), body.len()) }
        } else {
            -1
        };
        if fd >= 0 {
            unsafe { libthyla_rs::t_close(fd) };
        }
        c.ok(
            "L dirfd-based open re-anchors",
            n == TARGET_BODY.len() as i64 && &body[..TARGET_BODY.len()] == TARGET_BODY,
        );
    }

    // --- K: the CURRENT root, under a real chroot. Measure BOTH links on BOTH
    //     sides of the pivot; the verdicts must invert.
    //
    //     What this proves, precisely, is that the root an absolute target
    //     anchors at is the caller's root AS OF THIS RESOLUTION -- a kernel
    //     that captured the root once and reused it would keep the before-
    //     verdicts and fail the after-legs. It does NOT prove the re-anchor
    //     itself; leg L above does that. Two claims, two legs (the #83 lesson,
    //     relearned on the same resolver).
    let abs_before = File::open(&format!("{}/l_abs", WORK)).is_ok();
    let root_before = File::open(&format!("{}/l_root", WORK)).is_ok();
    c.ok("K before: l_abs resolves", abs_before);
    c.ok("K before: l_root does not", !root_before);

    let jail = match File::open_with_opath(WORK) {
        Ok(f) => f,
        Err(_) => fail("symlink-probe: FAIL -- O_PATH for chroot\n"),
    };
    if unsafe { t_chroot(jail.as_raw_fd() as i64) } != 0 {
        fail("symlink-probe: FAIL -- chroot\n");
    }

    // Same two links, same directory, new root.
    let abs_after = File::open("/l_abs").is_ok();
    let root_after = File::open("/l_root").is_ok();
    c.ok("K after: l_abs no longer resolves", !abs_after);
    c.ok("K after: l_root resolves", root_after);
    c.ok(
        "K after: content is the caller's own target",
        read_all("/l_root").as_deref() == Ok(TARGET_BODY),
    );

    // No teardown past this point -- the chroot is one-way and the pool tree is
    // reclaimed by the next boot's pre_clean.

    t_putstr(&format!(
        "symlink-probe: {} checks, {} failures\n",
        c.checks, c.fails
    ));
    if c.fails != 0 {
        t_putstr("symlink-probe: FAIL\n");
        return 1;
    }
    t_putstr("symlink-probe: PASS\n");
    0
}
