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
// Riders, because this is the one native probe that runs on the live FS and
// performs a real chroot -- each is argued where it is defined:
//   N append        an append open carries T_OAPPEND (write-after-seek lands at END)
//   U union-a/-b    a UNION root under chroot, and the dissolved union (ARCH 9.6.10)
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
use libthyla_rs::{t_chroot, t_exits, t_pivot_root, t_putstr};

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
    let _ = fs::remove_file(APPEND_FILE);
    let _ = fs::remove_file(INNER);
    let _ = fs::remove_file(TARGET);
    let _ = fs::remove_dir(SUB);
    let _ = fs::remove_dir(WORK);
}

// ---------------------------------------------------------------------------
// The UNION ROOT stages (mount-table shed, ARCH 9.6.10). Each runs in its own
// child -- a chroot is one-way -- re-spawned from the parent below.
//
// The union is /proc + /ctl mounted on a fresh Stratum directory, and that
// choice is the whole point: the union's POINT is on Stratum while member[0]
// is devproc, so point and members live in DIFFERENT device instances. A union
// composed inside one 9P session passes on a kernel with no union seed at all
// (the point's instance is the root's instance anyway) and proves nothing.
//
//   union-a  chroot ONTO the union handle. The shed's closure must seed the
//            point or it drops the union's own entries: every name under the
//            new root is ENOENT (audit r1 F1). Then DISSOLVE the union with
//            plain unmounts and open "/": it must be member[0], never the
//            Stratum directory the union covered (audit r2 F1, variant A --
//            which needs no shed at all).
//   union-b  hold a union dirfd, chroot ELSEWHERE (into /proc), so the shed
//            drops the union's entries; "." of the dirfd must again be
//            member[0], not the covered directory (variant B).
//
// The covered directory holds one marker file, so "which directory is this"
// is a fact read back, not an inference from errnos.
// Leg N rides here because this is the one NATIVE probe on the live FS: an
// append open must carry T_OAPPEND, not merely start at the end. Until
// 2026-09-21 libthyla-rs emulated append with one seek at open, so a write
// after ANY seek landed mid-file -- and so did the second of two appenders.
// The discriminator is a seek to 0 before the write: with the bit the bytes
// still land at the end; with the emulation they overwrite the first line.
const APPEND_FILE: &str = "/d1-symlink/append.txt";

fn append_leg(c: &mut Checker) {
    use libthyla_rs::io::{Seek, SeekFrom, Write};
    let made = File::create(APPEND_FILE)
        .map(|mut f| f.write_all(b"one\n").is_ok())
        .unwrap_or(false);
    let wrote = fs::OpenOptions::new()
        .write(true)
        .append(true)
        .open(APPEND_FILE)
        .map(|mut f| f.seek(SeekFrom::Start(0)).is_ok() && f.write_all(b"two\n").is_ok())
        .unwrap_or(false);
    c.ok("N append: fixture + append-after-seek write", made && wrote);
    c.ok(
        "N append: the write landed at END despite the seek (T_OAPPEND)",
        read_all(APPEND_FILE).as_deref() == Ok(b"one\ntwo\n".as_slice()),
    );
}

const UNION_DIR: &str = "/d1-union";
const UNION_MARKER: &str = "/d1-union/covered-marker";
// union-c's members: two Stratum directories, so member[0] is a 9P directory and
// its handle a 9P fid -- which refuses a Twalk once opened. /proc and /ctl (the
// other stages' members) are kernel Devs that walk an opened Spoor without
// complaint, which is how a rule wrong for 9P passed both of them.
const UNION_M0: &str = "/d1-union-m0";
const UNION_M1: &str = "/d1-union-m1";

fn opens_under(dirfd: i64, name: &str) -> bool {
    let fd = unsafe {
        libthyla_rs::t_open(dirfd, name.as_ptr(), name.len(), libthyla_rs::T_OPATH)
    };
    if fd >= 0 {
        unsafe { libthyla_rs::t_close(fd) };
    }
    fd >= 0
}

fn union_stage(stage: &[u8]) -> i64 {
    use libthyla_rs::territory::{mount, unmount, MountFlags};
    let mut c = Checker { checks: 0, fails: 0 };
    let me = format!("{}", unsafe { libthyla_rs::t_getpid() });

    if stage == b"union-c" {
        union_c(&mut c);
        t_putstr(&format!("symlink-probe: union-c {} checks, {} failures\n", c.checks, c.fails));
        return if c.fails != 0 { 1 } else { 0 };
    }

    let (proc_h, ctl_h) = match (File::open_with_opath("/proc"), File::open_with_opath("/ctl")) {
        (Ok(p), Ok(k)) => (p, k),
        _ => fail("symlink-probe: FAIL -- union: O_PATH /proc + /ctl\n"),
    };
    if mount(&proc_h, UNION_DIR, MountFlags::REPL).is_err()
        || mount(&ctl_h, UNION_DIR, MountFlags::AFTER).is_err()
    {
        fail("symlink-probe: FAIL -- union: mount /proc + /ctl on the Stratum dir\n");
    }
    // Controls, before any chroot: the union is live and both members answer.
    c.ok("U control: a devproc name resolves through the union",
         File::open_with_opath(&format!("{}/{}", UNION_DIR, me)).is_ok());
    c.ok("U control: a devctl-only name resolves through the union",
         File::open_with_opath(&format!("{}/kernel-base", UNION_DIR)).is_ok());
    c.ok("U control: the covered marker is hidden by the union",
         File::open_with_opath(UNION_MARKER).is_err());

    let uh = match File::open_with_opath(UNION_DIR) {
        Ok(f) => f,
        Err(_) => fail("symlink-probe: FAIL -- union: O_PATH on the union\n"),
    };

    if stage == b"union-a" {
        if unsafe { t_chroot(uh.as_raw_fd() as i64) } != 0 {
            fail("symlink-probe: FAIL -- union-a: chroot onto the union handle\n");
        }
        c.ok("U-a: member[0]'s name resolves under the union root",
             File::open_with_opath(&format!("/{}", me)).is_ok());
        c.ok("U-a: member[1]'s name resolves under the union root (the seed)",
             File::open_with_opath("/kernel-base").is_ok());
        // Dissolve: unmount("/") keys the union's point; one call per member.
        let mut gone = 0;
        while gone < 4 && unmount("/").is_ok() {
            gone += 1;
        }
        c.ok("U-a: both members unmounted", gone == 2);
        match File::open_with_opath("/") {
            Ok(root) => {
                let fd = root.as_raw_fd() as i64;
                c.ok("U-a dissolved: \"/\" is NOT the covered directory",
                     !opens_under(fd, "covered-marker"));
                c.ok("U-a dissolved: \"/\" is member[0]", opens_under(fd, &me));
            }
            Err(_) => c.ok("U-a dissolved: \"/\" opens", false),
        }
        // A NAME off the dissolved root, not only "/": the first component is
        // walked from member[0], and member[1]'s names are gone.
        c.ok("U-a dissolved: member[0]'s name resolves off the root",
             File::open_with_opath(&format!("/{}", me)).is_ok());
        c.ok("U-a dissolved: member[1]'s name is gone",
             File::open_with_opath("/kernel-base").is_err());
    } else {
        if unsafe { t_chroot(proc_h.as_raw_fd() as i64) } != 0 {
            fail("symlink-probe: FAIL -- union-b: chroot into /proc\n");
        }
        let ufd = uh.as_raw_fd() as i64;
        // Positive control that the chroot's shed DISSOLVED the union: without
        // it, every leg below still holds on a live union (shed audit round 3).
        c.ok("U-b shed: member[1]'s name is gone off the dirfd",
             !opens_under(ufd, "kernel-base"));
        c.ok("U-b shed: member[0]'s name resolves off the dirfd",
             opens_under(ufd, &me));
        let dot = unsafe {
            libthyla_rs::t_open(ufd, b".".as_ptr(), 1, libthyla_rs::T_OPATH)
        };
        c.ok("U-b: \".\" of the union dirfd opens after the shed", dot >= 0);
        if dot >= 0 {
            c.ok("U-b shed: \".\" is NOT the covered directory",
                 !opens_under(dot, "covered-marker"));
            c.ok("U-b shed: \".\" is member[0]", opens_under(dot, &me));
            unsafe { libthyla_rs::t_close(dot) };
        }
    }

    t_putstr(&format!(
        "symlink-probe: {} {} checks, {} failures\n",
        core::str::from_utf8(stage).unwrap_or("?"), c.checks, c.fails
    ));
    if c.fails != 0 { 1 } else { 0 }
}

// union-c: a union of two STRATUM directories over a Stratum directory, held
// through an OPENED (OREAD) handle -- the handle class whose member[0] is an
// opened 9P fid. Live, a `..` back to the base must walk member[0] unopened;
// dissolved, "." and member[0]'s names must still resolve, member[1]'s must not,
// and the covered directory must never answer.
fn union_c(c: &mut Checker) {
    use libthyla_rs::territory::{mount, unmount, MountFlags};
    let (m0, m1) = match (File::open_with_opath(UNION_M0), File::open_with_opath(UNION_M1)) {
        (Ok(a), Ok(b)) => (a, b),
        _ => fail("symlink-probe: FAIL -- union-c: O_PATH on the member directories\n"),
    };
    if mount(&m0, UNION_DIR, MountFlags::REPL).is_err()
        || mount(&m1, UNION_DIR, MountFlags::AFTER).is_err()
    {
        fail("symlink-probe: FAIL -- union-c: mount the Stratum members\n");
    }
    c.ok("U-c control: member[0]'s name resolves through the union",
         File::open_with_opath(&format!("{}/zero-only", UNION_DIR)).is_ok());
    c.ok("U-c control: member[1]'s name resolves through the union",
         File::open_with_opath(&format!("{}/one-only", UNION_DIR)).is_ok());
    c.ok("U-c control: the covered marker is hidden by the union",
         File::open_with_opath(UNION_MARKER).is_err());

    let uh = match File::open(UNION_DIR) {
        Ok(f) => f,
        Err(_) => fail("symlink-probe: FAIL -- union-c: OREAD open of the union\n"),
    };
    let ufd = uh.as_raw_fd() as i64;
    c.ok("U-c live: member[1]'s name off the OPENED dirfd", opens_under(ufd, "one-only"));
    c.ok("U-c live: back at the base, member[0] is walked unopened",
         opens_under(ufd, "sub/../zero-only"));

    let mut gone = 0;
    while gone < 4 && unmount(UNION_DIR).is_ok() {
        gone += 1;
    }
    c.ok("U-c: both members unmounted", gone == 2);
    let dot = unsafe { libthyla_rs::t_open(ufd, b".".as_ptr(), 1, libthyla_rs::T_OPATH) };
    c.ok("U-c dissolved: \".\" of the OPENED dirfd opens", dot >= 0);
    if dot >= 0 {
        c.ok("U-c dissolved: \".\" is NOT the covered directory",
             !opens_under(dot, "covered-marker"));
        c.ok("U-c dissolved: \".\" is member[0]", opens_under(dot, "zero-only"));
        unsafe { libthyla_rs::t_close(dot) };
    }
    c.ok("U-c dissolved: member[0]'s name off the dirfd", opens_under(ufd, "zero-only"));
    c.ok("U-c dissolved: member[1]'s name is gone", !opens_under(ufd, "one-only"));
    c.ok("U-c dissolved: the covered marker never answers", !opens_under(ufd, "covered-marker"));
}

// Build the covered directory + its marker, run one stage in a child, reap it.
fn run_union_stage(c: &mut Checker, stage: &str) {
    use libthyla_rs::process::Command;
    let _ = fs::remove_file(UNION_MARKER);
    let _ = fs::remove_dir(UNION_DIR);
    let built = fs::create_dir(UNION_DIR).is_ok()
        && File::create(UNION_MARKER)
            .map(|mut f| {
                use libthyla_rs::io::Write;
                f.write_all(b"covered\n").is_ok()
            })
            .unwrap_or(false);
    if !built {
        fail("symlink-probe: FAIL -- union: build the covered directory\n");
    }
    if stage == "union-c" {
        remove_union_c_members();
        let members = fs::create_dir(UNION_M0).is_ok()
            && fs::create_dir(&format!("{}/sub", UNION_M0)).is_ok()
            && File::create(&format!("{}/zero-only", UNION_M0)).is_ok()
            && fs::create_dir(UNION_M1).is_ok()
            && File::create(&format!("{}/one-only", UNION_M1)).is_ok();
        if !members {
            fail("symlink-probe: FAIL -- union-c: build the member directories\n");
        }
    }
    // joey spawns this probe with no fds, so there is no 0/1/2 to inherit and a
    // default Command refuses: hand the child three real ones. It reports
    // through t_putstr like its parent, so nothing is lost to the bit bucket.
    use libthyla_rs::process::Stdio;
    let null = || fs::OpenOptions::new().read(true).write(true).open("/dev/null");
    let ok = match (null(), null(), null()) {
        (Ok(i), Ok(o), Ok(e)) => {
            match Command::new("/bin/symlink-probe")
                .arg(stage)
                .stdin(Stdio::File(i))
                .stdout(Stdio::File(o))
                .stderr(Stdio::File(e))
                .spawn()
            {
                Ok(mut child) => child.wait().map(|st| st.success()).unwrap_or(false),
                Err(e) => {
                    t_putstr(&format!("symlink-probe: {} spawn errno {}\n", stage, e.as_errno()));
                    false
                }
            }
        }
        _ => {
            t_putstr("symlink-probe: /dev/null would not open\n");
            false
        }
    };
    c.ok(&format!("{}: the child stage passed", stage), ok);
    // The mounts died with the child's Territory; the directory is ours again.
    let _ = fs::remove_file(UNION_MARKER);
    let _ = fs::remove_dir(UNION_DIR);
    if stage == "union-c" {
        remove_union_c_members();
    }
}

// Remove union-c's member directories (idempotent: a leftover from a prior boot
// on a preserved pool must not fail the build step).
fn remove_union_c_members() {
    let _ = fs::remove_file(&format!("{}/zero-only", UNION_M0));
    let _ = fs::remove_dir(&format!("{}/sub", UNION_M0));
    let _ = fs::remove_dir(UNION_M0);
    let _ = fs::remove_file(&format!("{}/one-only", UNION_M1));
    let _ = fs::remove_dir(UNION_M1);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    if let Some(stage) = libthyla_rs::env::args().nth(1) {
        return union_stage(stage);
    }
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

    // --- M: the SINGLE-HOP TWIN refuses a symlink (audit F2, the P1).
    //     `t_walk_open` does not enter stalk and cannot expand -- but it must
    //     not hand the link to Dev.open either. Two reasons the audit made
    //     concrete: the DAC check there reads the LINK's mode, which is 0777
    //     by POSIX convention and so passes for everyone; and what the server
    //     does with a Tlopen on a symlink fid is a SERVER property (Stratum
    //     accepts it unless O_TRUNC, so the writes land in the link's inode --
    //     silent loss; a path-based server would open the TARGET).
    //
    //     So: without O_PATH it is ELOOP, with O_PATH it is the link itself.
    //     Before the fix the first of these returned a usable write handle.
    {
        let name = b"l_dir"; // a link, and to a DIRECTORY -- the shape most
                             // likely to be mistaken for a walkable base
        let dfd = dir.as_raw_fd() as i64;
        let bare = unsafe {
            libthyla_rs::t_walk_open(dfd, name.as_ptr(), name.len(), libthyla_rs::T_OREAD)
        };
        if bare >= 0 {
            unsafe { libthyla_rs::t_close(bare) };
        }
        c.ok(
            "M twin refuses a link",
            bare == -(Error::SymlinkLoop.as_errno() as i64),
        );

        let opath = unsafe {
            libthyla_rs::t_walk_open(dfd, name.as_ptr(), name.len(), libthyla_rs::T_OPATH)
        };
        c.ok("M twin O_PATH returns the link", opath >= 0);
        if opath >= 0 {
            unsafe { libthyla_rs::t_close(opath) };
        }
    }

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

    // A root must be a DIRECTORY, on BOTH doors. A non-directory root wedges
    // every later resolution, and since the mount-table shed (ARCH 9.6.10) a
    // pivot onto one also strips the table for good -- pivot had no such gate
    // until the shed's audit. Deny-path legs: a boot that merely succeeds says
    // nothing about whether the gate is wired.
    match File::open_with_opath(TARGET) {
        Ok(f) => {
            let fd = f.as_raw_fd() as i64;
            c.ok("K gate: chroot onto a file is refused", unsafe { t_chroot(fd) } != 0);
            c.ok("K gate: pivot_root onto a file is refused", unsafe { t_pivot_root(fd) } != 0);
            c.ok("K gate: the namespace is intact afterwards", File::open(TARGET).is_ok());
        }
        Err(_) => fail("symlink-probe: FAIL -- O_PATH on the target file\n"),
    }

    append_leg(&mut c);

    // The union-root stages, each in its own child (see union_stage).
    run_union_stage(&mut c, "union-a");
    run_union_stage(&mut c, "union-b");
    run_union_stage(&mut c, "union-c");

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
