// viv-pheno-probe — the VIVARIUM V-1b gate (docs/VIVARIUM.md §5, §12.1).
//
// Proves the phenotype end to end, from the two vantages that matter:
//
//   `viv-pheno-probe native`  — spawned WITHOUT a declaration. Asserts that a
//                               Linux syscall number is NOT translated. This is
//                               the discriminator: without it every "linux" leg
//                               below could conceivably be passing for some
//                               reason other than the phenotype.
//   `viv-pheno-probe linux`   — running inside a vivarium whose manifest
//                               declares `org.thylacine.phenotype: linux`.
//                               Issues ONLY Linux aarch64 numbers, and moves
//                               real bytes through them.
//
// WHY THIS BINARY TALKS IN RAW `svc` AND EXITS BY HAND. A PHENO_LINUX Proc has
// no native ABI left: every libthyla-rs call would be fed to the translation
// table, and the ones that are not Linux rows come back -ENOSYS. That includes
// the runtime's own exit -- `_start` ends with `mov x8, #0` (T_SYS_EXITS), and
// Linux 0 is `io_setup`, which is not a row, so returning from rs_main in linux
// mode would park forever in _start's defensive `wfe` loop. So the linux path
// allocates nothing, calls nothing, prints nothing, and terminates through
// Linux `exit_group` (94) itself. That constraint is not an inconvenience to
// work around -- it IS the property under test: a Linux binary is a Linux
// binary all the way down, including how it dies.
//
// REPORTING GOES THROUGH A FILE, and the reason is a finding worth stating: a
// PHENO_LINUX Proc has no native SYS_PUTS and no endowed fds, so the exit
// status looks like the only channel back -- but Thylacine's exit status is
// BOOLEAN at v1.0 (sys_exits_handler / sys_exit_group_handler both collapse any
// nonzero to exits("fail") -> 1; task #91). Per-leg exit codes would all arrive
// as 1, which is worse than useless: it names the wrong leg. So the verdict is
// WRITTEN INTO the bundle's `pheno-scratch` file through Linux write(64), and
// joey reads it back from OUTSIDE the container. That makes the report channel
// reliable AND turns the write path's proof from "the probe read its own write"
// into "a different Proc, in a different territory, read the bytes" -- the
// stronger claim. joey stamps the file with a sentinel BEFORE the run, so a
// stale marker from a previous boot cannot pass the gate.

#![no_std]
#![no_main]

// The libthyla-rs convention (every native Rust binary names the allocator,
// because the lib links `alloc` at its root). Declared for the LINK, not for
// use: the linux path below allocates nothing, and must not -- ThylaAlloc's
// first allocation calls SYS_BURROW_ATTACH_LAZY, which is not a Linux row, so
// under PHENO_LINUX it would come back -ENOSYS and the heap would fail to
// initialise. The native path may allocate freely and doesn't need to.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::arch::asm;
use libthyla_rs::{env, t_putstr};

// ---------------------------------------------------------------------------
// Raw Linux aarch64 syscalls. Six argument registers, x8 = number.
// ---------------------------------------------------------------------------

#[inline(always)]
unsafe fn svc3(nr: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    let mut x0: i64 = a0 as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") a1,
        in("x2") a2,
        in("x8") nr,
        options(nostack)
    );
    x0
}

#[inline(always)]
unsafe fn svc4(nr: u64, a0: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    let mut x0: i64 = a0 as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") a1,
        in("x2") a2,
        in("x3") a3,
        in("x8") nr,
        options(nostack)
    );
    x0
}

// Linux aarch64 numbers under test (the kernel/vivarium.c table's own set).
const NR_OPENAT: u64 = 56;
const NR_CLOSE: u64 = 57;
const NR_LSEEK: u64 = 62;
const NR_READ: u64 = 63;
const NR_WRITE: u64 = 64;
const NR_NEWFSTATAT: u64 = 79;
const NR_FSTAT: u64 = 80;
const NR_EXIT_GROUP: u64 = 94;
const NR_BRK: u64 = 214;
const NR_MUNMAP: u64 = 215;

const AT_FDCWD: u64 = (-100i64) as u64;
const AT_SYMLINK_NOFOLLOW: u64 = 0x100;
const O_RDONLY: u64 = 0;
const O_WRONLY: u64 = 1;
const SEEK_SET: u64 = 0;

// The translated-mode answer for a number with no Thylacine counterpart, and
// (until V-3's supervisor exists) for a forwarded one too: -ENOSYS. Natively
// the same number falls to syscall_dispatch's `default:`, which answers -1 --
// and that difference is exactly what leg 1 measures.
const NEG_ENOSYS: i64 = -38;

// The Linux aarch64 `struct stat` (128 bytes; kernel/include/thylacine/
// vivarium.h `struct viv_linux_stat` is the kernel's copy of this layout).
// Only the fields the legs read are named; the rest is padding by offset.
#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxStat {
    st_dev: u64,
    st_ino: u64,
    st_mode: u32,
    st_nlink: u32,
    st_uid: u32,
    st_gid: u32,
    st_rdev: u64,
    _pad1: u64,
    st_size: i64,
    st_blksize: i32,
    _pad2: i32,
    st_blocks: i64,
    st_atime_sec: i64,
    st_atime_nsec: u64,
    st_mtime_sec: i64,
    st_mtime_nsec: u64,
    st_ctime_sec: i64,
    st_ctime_nsec: u64,
    _unused4: u32,
    _unused5: u32,
}
const _: () = assert!(core::mem::size_of::<LinuxStat>() == 128);

const S_IFMT: u32 = 0o170000;
const S_IFREG: u32 = 0o100000;

// Paths, NUL-terminated because Linux passes a pointer and lets the kernel
// find the end -- which is the whole reason `openat` is a Tier-2 translator
// (SYS_OPEN wants an explicit length, so the kernel must measure).
const SELF_PATH: &[u8] = b"/bin/viv-pheno-probe\0";
const SCRATCH_PATH: &[u8] = b"/pheno-scratch\0";
// The pass marker joey reads back from OUTSIDE the container.
const PASS_MARK: &[u8] = b"OK\n";

#[inline(always)]
unsafe fn linux_exit(code: i64) -> ! {
    asm!(
        "svc #0",
        in("x0") code,
        in("x8") NR_EXIT_GROUP,
        options(noreturn, nostack)
    );
}

// Every leg is `cond or (report, exit)`. The marker goes into the report file
// through Linux write(64) -- the exit status cannot carry it (task #91), so the
// file is what tells joey WHICH property broke. A write failure here leaves the
// sentinel joey stamped, which reads as "died before speaking" -- still a fail,
// never a pass.
macro_rules! leg {
    ($rep:expr, $cond:expr, $mark:expr) => {
        if !($cond) {
            let m: &[u8] = $mark;
            let _ = svc3(NR_WRITE, $rep as u64, m.as_ptr() as u64, m.len() as u64);
            linux_exit(1)
        }
    };
}

/// The linux-mode body: raw Linux numbers only, no allocation, never returns.
unsafe fn run_linux() -> ! {
    // The report channel comes FIRST, because a verdict we cannot deliver is
    // not a verdict. joey truncated this file to a sentinel before the run, so
    // "still the sentinel" is itself a readable outcome (the probe died before
    // it could speak) rather than a silent pass.
    let rep = svc4(NR_OPENAT, AT_FDCWD, SCRATCH_PATH.as_ptr() as u64, O_WRONLY, 0);
    // `>= 0`, NOT `> 0`: a container entrypoint spawned by a FD-LESS runner
    // (joey's boot daemons carry no fds, so neither does viv, so neither do we)
    // starts with an empty handle table -- its first open really is fd 0.
    if rep < 0 {
        linux_exit(1);
    }

    // --- L01: we really are translated --------------------------------------
    // `brk` is the table's one explicit ENOSYS row (the heap is Burrow-based;
    // there is no break pointer to move). Natively 214 is unassigned and the
    // dispatcher's default answers -1, so this single comparison separates
    // "the phenotype is live" from "the phenotype did nothing".
    leg!(rep, svc3(NR_BRK, 0, 0, 0) == NEG_ENOSYS, b"L01\n");

    // --- L02: a FORWARD row answers honestly too ----------------------------
    // munmap is rejected for a REASON (burrow_detach refuses a partial detach
    // while Linux permits one), so it must not be silently mistranslated.
    // Until V-3's supervisor exists it shares brk's wire answer.
    leg!(rep, svc3(NR_MUNMAP, 0, 0, 0) == NEG_ENOSYS, b"L02\n");

    // --- L03-L06: openat + read + lseek move real bytes ---------------------
    let fd = svc4(NR_OPENAT, AT_FDCWD, SELF_PATH.as_ptr() as u64, O_RDONLY, 0);
    leg!(rep, fd >= 0, b"L03\n");

    let mut magic = [0u8; 4];
    leg!(rep, svc3(NR_READ, fd as u64, magic.as_mut_ptr() as u64, 4) == 4, b"L04\n");
    // Our own ELF header: proof the bytes are the file's, not an artifact.
    leg!(
        rep,
        magic[0] == 0x7f && magic[1] == b'E' && magic[2] == b'L' && magic[3] == b'F',
        b"L05\n"
    );

    leg!(rep, svc3(NR_LSEEK, fd as u64, 0, SEEK_SET) == 0, b"L06\n");

    // --- L07-L09: fstat, the 128-byte struct conversion ---------------------
    let mut st = core::mem::zeroed::<LinuxStat>();
    leg!(
        rep,
        svc3(NR_FSTAT, fd as u64, &mut st as *mut LinuxStat as u64, 0) == 0,
        b"L07\n"
    );
    leg!(rep, st.st_size > 0, b"L08\n");
    leg!(rep, st.st_mode & S_IFMT == S_IFREG, b"L09\n");

    // --- L10-L12: newfstatat, the same conversion by path -------------------
    let mut st2 = core::mem::zeroed::<LinuxStat>();
    leg!(
        rep,
        svc4(
            NR_NEWFSTATAT,
            AT_FDCWD,
            SELF_PATH.as_ptr() as u64,
            &mut st2 as *mut LinuxStat as u64,
            0
        ) == 0,
        b"L10\n"
    );
    // Two independent stat paths -- one by fd, one by name -- must agree on
    // the file's identity. (devno, qid.path) IS Thylacine's file identity
    // (#100), so this cross-check is meaningful, not tautological.
    leg!(rep, st2.st_ino == st.st_ino, b"L11\n");
    leg!(rep, st2.st_dev == st.st_dev, b"L12\n");

    // --- L13: the documented reject stays rejected --------------------------
    // AT_SYMLINK_NOFOLLOW is what lstat() compiles to. It is refused ON
    // PURPOSE: stat == lstat holds at v1.0 only because symlinks are ABSENT,
    // and admitting it would silently start reporting targets instead of links
    // the day they land. Asserting the refusal keeps a future "optimisation"
    // from quietly deleting the safeguard.
    leg!(
        rep,
        svc4(
            NR_NEWFSTATAT,
            AT_FDCWD,
            SELF_PATH.as_ptr() as u64,
            &mut st2 as *mut LinuxStat as u64,
            AT_SYMLINK_NOFOLLOW
        ) == NEG_ENOSYS,
        b"L13\n"
    );

    leg!(rep, svc3(NR_CLOSE, fd as u64, 0, 0) == 0, b"L14\n");

    // --- the verdict, which is also the write leg ---------------------------
    // Linux write(64) puts these bytes in the file; joey reads them from its
    // own territory. If the renumber were wrong the bytes would not be there,
    // so "joey sees OK" is simultaneously the pass signal and write's proof.
    if svc3(NR_WRITE, rep as u64, PASS_MARK.as_ptr() as u64, PASS_MARK.len() as u64)
        != PASS_MARK.len() as i64
    {
        // write failed: say so through the one channel left.
        linux_exit(1);
    }
    leg!(rep, svc3(NR_CLOSE, rep as u64, 0, 0) == 0, b"L15\n");

    // --- the last leg is the exit itself ------------------------------------
    // exit_group is a Tier-1 row. If 94 were NOT translated it would reach
    // native SYS_TTY_SIGNAL, this process would not die, and joey's by-pid
    // wait would never return -- so a clean reap IS the assertion.
    linux_exit(0)
}

/// The native-mode body: the discriminator, plus proof the native ABI is
/// untouched by the branch's existence.
fn run_native() -> i64 {
    // Un-translated, `brk`'s number is simply unknown to the native dispatcher,
    // which answers -1. Seeing -1 here and -ENOSYS in linux mode is what proves
    // the phenotype -- and only the phenotype -- changed the answer.
    let brk = unsafe { svc3(NR_BRK, 0, 0, 0) };
    if brk != -1 {
        t_putstr("viv-pheno-probe: FAIL native brk not -1\n");
        return 1;
    }
    // A native Proc still has its native ABI: this call is the proof, and it
    // is also how the result gets reported at all.
    t_putstr("viv-pheno-probe: native PASS\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mode: &[u8] = env::args().nth(1).unwrap_or(&[]);
    if mode == b"linux".as_slice() {
        unsafe { run_linux() }
    }
    if mode == b"native".as_slice() {
        return run_native();
    }
    t_putstr("viv-pheno-probe: usage: viv-pheno-probe native|linux\n");
    2
}
