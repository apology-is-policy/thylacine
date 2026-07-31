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

/// Six arguments -- `mmap` is the only caller, and it needs all of them.
#[inline(always)]
#[allow(clippy::too_many_arguments)]
unsafe fn svc6(nr: u64, a0: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64) -> i64 {
    let mut x0: i64 = a0 as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") a1,
        in("x2") a2,
        in("x3") a3,
        in("x4") a4,
        in("x5") a5,
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
// The socket family (V-5). aarch64 has no socketcall; each is its own number.
const NR_SOCKET: u64 = 198;
const NR_CONNECT: u64 = 203;
const AF_INET: u64 = 2;
const AF_INET6: u64 = 10;
const SOCK_STREAM: u64 = 1;
const SOCK_DGRAM: u64 = 2;
const SOCK_NONBLOCK: u64 = 0o4000;
const SOCK_SEQPACKET: u64 = 5;
// POSIX errno values a Linux libc compares against (musl generic bits/errno.h).
const ENOTSOCK: i64 = 88;
const EPROTONOSUPPORT: i64 = 93;
const EAFNOSUPPORT: i64 = 97;
const EISCONN: i64 = 106;
const NR_EXIT_GROUP: u64 = 94;
const NR_BRK: u64 = 214;
const NR_MUNMAP: u64 = 215;
const NR_MMAP: u64 = 222;
const NR_MPROTECT: u64 = 226;
const NR_RT_SIGACTION: u64 = 134;
const NR_RT_SIGRETURN: u64 = 139;
const NR_RT_SIGPROCMASK: u64 = 135;

// ---------------------------------------------------------------------------
// The V-6c signal handler, its trampoline, and the evidence it leaves behind.
//
// WHY THE RESTORER IS OURS. vivarium_sigaction_decide REQUIRES SA_RESTORER for
// a real handler: Thylacine will not synthesise a sigreturn trampoline, because
// the only place to put one is the vDSO page and that page is deliberately
// RO+XN. So the guest supplies it -- which is exactly what musl does, and this
// is the same two instructions musl's __restore_rt compiles to.
//
// NO FP/SIMD ANYWHERE IN HERE. Note delivery does not save Q0-Q31 (task #96),
// so a handler that touched them would corrupt the interrupted computation.
// Integer stores into statics only.
// ---------------------------------------------------------------------------

core::arch::global_asm!(
    ".globl viv_restore_rt",
    ".type viv_restore_rt, @function",
    "viv_restore_rt:",
    "    mov x8, #139",          // __NR_rt_sigreturn
    "    svc #0",
    "    brk #0",                // unreachable: sigreturn does not return
);

extern "C" {
    fn viv_restore_rt();
}

// The frame offsets the handler reads, from the ucontext base (x2).
//   uc_mcontext        @ 176
//   ... .pc            @ 176 + 264
//   ... .__reserved[0] @ 176 + 288   (the _aarch64_ctx terminator)
const UC_MCONTEXT: usize = 176;
const MC_PC: usize = 264;
const MC_RESERVED: usize = 288;

static mut SIG_FIRED: u64 = 0;
static mut SIG_SIGNO: u64 = 0;
static mut SIG_INFO_VA: u64 = 0;
static mut SIG_UC_VA: u64 = 0;
static mut SIG_SI_SIGNO: u32 = 0;
static mut SIG_UC_PC: u64 = 0;
static mut SIG_UC_END_MAGIC: u32 = 0xFFFF_FFFF;

extern "C" fn viv_sig_handler(signo: i32, info: *const u8, uc: *const u8) {
    unsafe {
        SIG_FIRED += 1;
        SIG_SIGNO = signo as u64;
        SIG_INFO_VA = info as u64;
        SIG_UC_VA = uc as u64;
        if !info.is_null() {
            SIG_SI_SIGNO = core::ptr::read_unaligned(info as *const u32);
        }
        if !uc.is_null() {
            SIG_UC_PC = core::ptr::read_unaligned(
                uc.add(UC_MCONTEXT + MC_PC) as *const u64);
            SIG_UC_END_MAGIC = core::ptr::read_unaligned(
                uc.add(UC_MCONTEXT + MC_RESERVED) as *const u32);
        }
    }
}

fn handler_addr() -> u64 { viv_sig_handler as usize as u64 }
fn restorer_addr() -> u64 { viv_restore_rt as usize as u64 }
fn sig_fired() -> u64 { unsafe { core::ptr::read_volatile(&raw const SIG_FIRED) } }
fn sig_signo() -> u64 { unsafe { core::ptr::read_volatile(&raw const SIG_SIGNO) } }
fn sig_info_va() -> u64 { unsafe { core::ptr::read_volatile(&raw const SIG_INFO_VA) } }
fn sig_uc_va() -> u64 { unsafe { core::ptr::read_volatile(&raw const SIG_UC_VA) } }
fn sig_si_signo() -> u32 { unsafe { core::ptr::read_volatile(&raw const SIG_SI_SIGNO) } }
fn sig_uc_pc() -> u64 { unsafe { core::ptr::read_volatile(&raw const SIG_UC_PC) } }
fn sig_uc_end_magic() -> u32 {
    unsafe { core::ptr::read_volatile(&raw const SIG_UC_END_MAGIC) }
}

// V-2d. Values from third_party/musl (generic include/sys/mman.h plus the two
// aarch64 additions in arch/aarch64/bits/mman.h) -- the same source the kernel
// table read, so a drift between the two shows up here as a failed leg.
const PROT_NONE: u64 = 0;
const PROT_READ: u64 = 1;
const PROT_WRITE: u64 = 2;
const PROT_EXEC: u64 = 4;
const MAP_PRIVATE: u64 = 0x02;
const MAP_FIXED: u64 = 0x10;
const MAP_ANON: u64 = 0x20;

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

// V-2d: the T2 shells reproduce Linux's ARGUMENT errors exactly rather than
// collapsing them into a decline, so EINVAL is a distinct expected answer.
const NEG_EINVAL: i64 = -22;

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

    // --- L02: an argument error is Linux's, not a decline -------------------
    // Was `munmap -> ENOSYS` while munmap was a FORWARD row. V-2d makes it T2,
    // and the shell reproduces Linux's OWN answer for len == 0 (EINVAL) rather
    // than collapsing it into the decline. This leg is what caught the change:
    // it failed the moment the disposition moved, which is the point of pinning
    // dispositions in the prover.
    leg!(rep, svc3(NR_MUNMAP, 0, 0, 0) == NEG_EINVAL, b"L02\n");

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

    // --- L16-L23: mmap + munmap (V-2d) --------------------------------------
    // The row a Linux guest cannot reach main() without: musl mmaps for TLS
    // (__init_tls.c:137) and mallocng mmaps every heap area.
    const MAP_LEN: u64 = 8192;
    let m = svc6(
        NR_MMAP,
        0,
        MAP_LEN,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        (-1i64) as u64,
        0,
    );
    // A successful mmap must not land in the errno band a Linux caller checks.
    leg!(rep, m > 0 && !(-4095..0).contains(&m), b"L16\n");

    // The pages are REAL: write a pattern through the mapping and read it back.
    // A translation that returned a plausible-looking address without backing
    // it would pass L16 and fail here, which is why this leg exists separately.
    let p = m as *mut u64;
    p.write_volatile(0x5649564152494f4d);          // "VIVARIOM"
    p.add((MAP_LEN / 8 - 1) as usize).write_volatile(0xA5A5_5A5A_1234_5678);
    leg!(rep, p.read_volatile() == 0x5649564152494f4d, b"L17\n");
    leg!(
        rep,
        p.add((MAP_LEN / 8 - 1) as usize).read_volatile() == 0xA5A5_5A5A_1234_5678,
        b"L18\n"
    );

    // The exact-match subset: unmapping what we mapped succeeds.
    leg!(rep, svc3(NR_MUNMAP, m as u64, MAP_LEN, 0) == 0, b"L19\n");

    // PROT_EXEC is the hard line, not a degradation: an executable anonymous
    // mapping is CAP_JIT / I-42 territory and W^X (I-12) forbids it. Proven
    // from INSIDE the guest, which is the only vantage that shows a Linux
    // binary cannot obtain one.
    leg!(
        rep,
        svc6(NR_MMAP, 0, MAP_LEN, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON, (-1i64) as u64, 0) == NEG_ENOSYS,
        b"L20\n"
    );

    // MAP_FIXED is where `addr` stops being a hint and becomes a requirement.
    leg!(
        rep,
        svc6(NR_MMAP, 0x40000000, MAP_LEN, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, (-1i64) as u64, 0) == NEG_ENOSYS,
        b"L21\n"
    );

    // mprotect is the explicit ENOSYS row. musl DEPENDS on this answer being
    // ENOSYS specifically: mallocng/malloc.c:92 proceeds when mprotect fails
    // with ENOSYS and gives up on any other error, so a different errno here
    // would break malloc rather than degrade it.
    leg!(
        rep,
        svc3(NR_MPROTECT, 0x40000000, 4096, PROT_READ | PROT_WRITE) == NEG_ENOSYS,
        b"L22\n"
    );

    // THE DEGRADATION, pinned deliberately (VIVARIUM.md §9's DEGRADED tier).
    // PROT_NONE is admitted and yields a WRITABLE mapping, because Thylacine
    // anonymous memory is always RW/XN and there is no prot-mutation syscall.
    // This leg asserts the divergence rather than hiding it: should real
    // PROT_NONE ever land, this fails and forces the ladder entry to be
    // updated instead of silently going stale.
    let n = svc6(NR_MMAP, 0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON,
                 (-1i64) as u64, 0);
    leg!(rep, n > 0 && !(-4095..0).contains(&n), b"L23\n");
    (n as *mut u64).write_volatile(0xDEAD_BEEF);   // writable despite PROT_NONE
    let _ = svc3(NR_MUNMAP, n as u64, 4096, 0);

    // --- L24-L31: signals (V-6b) --------------------------------------------
    // musl calls BOTH of these before main(): __init_tls / pthread_create go
    // through rt_sigprocmask, and any signal() is an rt_sigaction. Until this
    // chunk both were ENOSYS.
    //
    // The kernel unit tests cover the DECISION (which requests are in domain);
    // these legs cover the PLUMBING -- the uaccess of two user structs, the
    // lazily-allocated per-Proc table, and the oldact writeback -- which no
    // pure test can see.
    const SIG_BLOCK: u64 = 0;
    const SIG_UNBLOCK: u64 = 1;
    const SIG_DFL: u64 = 0;
    const SIG_IGN: u64 = 1;
    const SIGHUP: u64 = 1;
    const SIGINT: u64 = 2;
    const SIGSEGV: u64 = 11;
    const SIGPIPE: u64 = 13;
    const SIGCHLD: u64 = 17;
    const SIGKILL: u64 = 9;
    const SIGWINCH: u64 = 28;
    const SA_RESTORER: u64 = 0x0400_0000;
    const SA_SIGINFO: u64 = 4;
    let bit = |s: u64| 1u64 << (s - 1);

    // A `struct k_sigaction`: handler, flags, restorer, mask. The 32-byte
    // aarch64 shape -- fixed by the arch, not chosen per call.
    let mut ksa: [u64; 4];
    let mut old: [u64; 4];
    let mut set: u64;
    let mut oldset: u64 = 0;

    // Blocking a signal is accepted at all.
    set = bit(SIGPIPE);
    leg!(
        rep,
        svc4(NR_RT_SIGPROCMASK, SIG_BLOCK, &set as *const u64 as u64,
             &mut oldset as *mut u64 as u64, 8) == 0,
        b"L24\n"
    );

    // ... and it TOOK EFFECT: query the mask back and SIGPIPE is in it. A
    // translation that accepted the call and dropped it would pass L24.
    oldset = 0;
    leg!(
        rep,
        svc4(NR_RT_SIGPROCMASK, SIG_BLOCK, 0, &mut oldset as *mut u64 as u64, 8) == 0
            && (oldset & bit(SIGPIPE)) != 0,
        b"L25\n"
    );

    // THE HONEST OVER-REPORT (§9's DEGRADED tier). The tty family shares ONE
    // note-mask bit, so blocking SIGWINCH really does block SIGHUP -- and the
    // readback SAYS SO rather than showing the guest the tidy answer it asked
    // for. This leg asserts the divergence so it cannot go stale silently.
    set = bit(SIGWINCH);
    oldset = 0;
    let _ = svc4(NR_RT_SIGPROCMASK, SIG_BLOCK, &set as *const u64 as u64, 0, 8);
    leg!(
        rep,
        svc4(NR_RT_SIGPROCMASK, SIG_BLOCK, 0, &mut oldset as *mut u64 as u64, 8) == 0
            && (oldset & bit(SIGWINCH)) != 0
            && (oldset & bit(SIGHUP)) != 0,
        b"L26\n"
    );

    // SIG_IGN on SIGPIPE -- the single most common signal call in real
    // programs. Accepting it allocates the per-Proc table.
    ksa = [SIG_IGN, 0, 0, 0];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGPIPE, &ksa as *const u64 as u64, 0, 8) == 0,
        b"L27\n"
    );

    // The disposition ROUND-TRIPS: query it back and the table returns SIG_IGN.
    // This is the leg that proves the table was allocated, written and read --
    // and that the oldact writeback lands at the right offsets.
    old = [0xDEAD; 4];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGPIPE, 0, &mut old as *mut u64 as u64, 8) == 0
            && old[0] == SIG_IGN
            && old[3] == 0,          // mask zeroed, not left as our sentinel
        b"L28\n"
    );

    // A DIFFERENT signal is unaffected -- per-signal, not a blanket mute.
    old = [0xDEAD; 4];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGINT, 0, &mut old as *mut u64 as u64, 8) == 0
            && old[0] == SIG_DFL,
        b"L29\n"
    );

    // V-6b pinned this leg as DECLINING and said it would invert when the frame
    // landed. It has: a handler WITH a restorer installs, and L32-L35 below
    // prove it actually runs.
    ksa = [0x400000, SA_RESTORER, 0x400100, 0];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGINT, &ksa as *const u64 as u64, 0, 8) == 0,
        b"L30\n"
    );

    // The three permanent declines, each for its own reason: SIGKILL is
    // uncatchable, SIGSEGV's note never reaches a queue (the fault terminates
    // first), and SIGCHLD+SIG_IGN means AUTO-REAP on Linux, which Thylacine
    // cannot do.
    ksa = [SIG_IGN, 0, 0, 0];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGKILL, &ksa as *const u64 as u64, 0, 8) == NEG_ENOSYS
            && svc4(NR_RT_SIGACTION, SIGSEGV, &ksa as *const u64 as u64, 0, 8)
                == NEG_ENOSYS
            && svc4(NR_RT_SIGACTION, SIGCHLD, &ksa as *const u64 as u64, 0, 8)
                == NEG_ENOSYS,
        b"L31\n"
    );

    // --- L32-L36: the Tier-1 frame, delivered for real (V-6c) ---------------
    //
    // Everything above proves the TABLE. These prove the HANDLER RUNS -- which
    // is the whole of V-6c, and the difference between an install and a stored
    // lie.
    //
    // The signal is SELF-INFLICTED and therefore race-free: viv hands this
    // process fd 0 as the write end of a pipe with NO READER, so `write()`
    // makes the kernel post `pipe` synchronously, and it is delivered at that
    // very syscall's return to EL0. No other Proc has to time anything, and
    // the handler is provably installed first because WE install it.
    //
    // If delivery is broken the default action for SIGPIPE is terminate, so a
    // regression does not produce a wrong answer -- it kills this process and
    // joey reports the marker that was current when it died.

    // The disposition round-trips through the table with the restorer intact.
    // (L28 proved that for SIG_IGN; this is the handler shape, where the
    // restorer is the field that matters -- it is the only way back out.)
    ksa = [handler_addr(), SA_RESTORER | SA_SIGINFO, restorer_addr(), 0];
    old = [0xDEAD; 4];
    leg!(
        rep,
        svc4(NR_RT_SIGACTION, SIGPIPE, &ksa as *const u64 as u64,
             &mut old as *mut u64 as u64, 8) == 0
            && old[0] == SIG_IGN,     // L27 left SIGPIPE ignored
        b"L32\n"
    );

    // Fire it. A one-byte write to the reader-less fd 0. The write itself must
    // report the error (that is the EPIPE a program actually reads); the note
    // is the SECOND effect, and the two are separate legs so a failure names
    // which link broke.
    let byte: u8 = b'x';
    let wrc = unsafe { svc3(NR_WRITE, 0, &byte as *const u8 as u64, 1) };
    leg!(rep, wrc < 0, b"L33\n");

    // SIGPIPE is still BLOCKED from L24, and that is deliberate: over-blocking
    // must DEFER a signal, never lose it (the claim viv_notemask_to_sigset's
    // header makes about the honest over-report). So the handler must NOT have
    // run yet, even though the note is queued.
    leg!(rep, sig_fired() == 0, b"L34\n");

    // Unblock, and the deferred note is delivered at THIS syscall's own return
    // to EL0 -- the handler runs, sees its own signal number, and returns
    // through the guest's own trampoline. We are still executing, which is the
    // proof that rt_sigreturn worked: had it not, we would be at whatever
    // address the restorer's `svc` left us.
    set = bit(SIGPIPE);
    let _ = svc4(NR_RT_SIGPROCMASK, SIG_UNBLOCK, &set as *const u64 as u64, 0, 8);
    leg!(rep, sig_fired() == 1 && sig_signo() == SIGPIPE as u64, b"L35\n");

    // The three pointer arguments are the aarch64 delivery contract:
    // x1 = &siginfo, x2 = &ucontext, and the ucontext sits exactly 128 bytes
    // (sizeof siginfo_t) above the siginfo. A wrong frame size shows up here
    // before it shows up as a corrupted guest.
    leg!(
        rep,
        sig_info_va() != 0 && sig_uc_va() == sig_info_va() + 128,
        b"L36\n"
    );

    // The frame CONTENT the handler read out of its own ucontext:
    //   si_signo            == SIGPIPE
    //   uc_mcontext.pc      != 0        (the interrupted PC is real)
    //   the _aarch64_ctx chain terminator is present, so a guest walking
    //   __reserved stops at once instead of following stack garbage.
    leg!(
        rep,
        sig_si_signo() == SIGPIPE as u32
            && sig_uc_pc() != 0
            && sig_uc_end_magic() == 0,
        b"L37\n"
    );

    // rt_sigreturn OUTSIDE a handler must be INTERCEPTED, not forwarded. The
    // two answers are distinguishable and that is the point: -1 means the
    // dispatcher routed it to SYS_NOTED (which refused, correctly, because no
    // handler is running), while -ENOSYS would mean the interception is gone --
    // in which case every real handler would run once and never return.
    leg!(rep, svc4(NR_RT_SIGRETURN, 0, 0, 0, 0) == -1, b"L38\n");

    // --- V-5a: sockets -----------------------------------------------------
    // The container's manifest grants /net, so these run against the LIVE netd
    // through the guest's own territory -- which is the point: a translated
    // socket call reaches exactly what this Proc could reach by opening /net
    // by hand (I-43), and nothing more.

    // The argument domain, refused BEFORE anything touches /net. Each errno is
    // one a Linux program acts on differently, so collapsing them to EINVAL
    // would change behaviour, not just the message.
    leg!(
        rep,
        svc3(NR_SOCKET, AF_INET6, SOCK_STREAM, 0) == -EAFNOSUPPORT,
        b"L39\n"
    );
    leg!(
        rep,
        svc3(NR_SOCKET, AF_INET, SOCK_SEQPACKET, 0) == -EPROTONOSUPPORT,
        b"L40\n"
    );
    // SOCK_NONBLOCK is REFUSED, not silently dropped. A guest that asked for a
    // non-blocking socket and got a blocking one blocks where it expected
    // EAGAIN -- the exact mistranslation the argument domain exists to prevent.
    leg!(
        rep,
        svc3(NR_SOCKET, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0) == -22,
        b"L41\n"
    );

    // A real UDP socket. UDP is the deterministic choice: netd's udp_connect
    // binds a local port and records the remote with NO handshake, so this
    // proves the whole path without needing a peer or a live network.
    let sfd = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    leg!(rep, sfd >= 0, b"L42\n");

    // The fd currently denotes the connection's `ctl` file. Record its qid.
    let mut cst = core::mem::zeroed::<LinuxStat>();
    leg!(
        rep,
        svc3(NR_FSTAT, sfd as u64, &mut cst as *mut LinuxStat as u64, 0) == 0,
        b"L43\n"
    );
    let ctl_ino = cst.st_ino;

    // connect() to 127.0.0.1:9 (discard). sockaddr_in: family LE, port NETWORK
    // order, then the four address octets.
    let sa: [u8; 16] = [
        AF_INET as u8, 0, // sin_family
        0, 9, // sin_port = 9, network order
        127, 0, 0, 1, // sin_addr
        0, 0, 0, 0, 0, 0, 0, 0,
    ];
    leg!(
        rep,
        svc3(NR_CONNECT, sfd as u64, sa.as_ptr() as u64, sa.len() as u64) == 0,
        b"L44\n"
    );

    // THE FD IDENTITY CHANGE, observed. The same fd number now denotes the
    // connection's `data` file -- a DIFFERENT object with a different qid. This
    // is the mechanism the whole design rests on: after this point read/write
    // on this fd are untranslated T1 rows on an ordinary Spoor.
    let mut dst = core::mem::zeroed::<LinuxStat>();
    leg!(
        rep,
        svc3(NR_FSTAT, sfd as u64, &mut dst as *mut LinuxStat as u64, 0) == 0,
        b"L45\n"
    );
    leg!(rep, dst.st_ino != ctl_ino, b"L46\n");

    // A second connect is EISCONN, which means the socktab state advanced --
    // not merely that the fd changed.
    leg!(
        rep,
        svc3(NR_CONNECT, sfd as u64, sa.as_ptr() as u64, sa.len() as u64) == -EISCONN,
        b"L47\n"
    );

    // connect() on an fd that is not a socket is ENOTSOCK, not a dial written
    // to some unrelated file.
    leg!(
        rep,
        svc3(NR_CONNECT, rep as u64, sa.as_ptr() as u64, sa.len() as u64) == -ENOTSOCK,
        b"L48\n"
    );

    // THE CLOSE HOOK, live. Close the socket, then confirm the fd index no
    // longer resolves to a socket. Without the hook the entry would survive,
    // and a later connect() on whatever reused this index would dial a
    // stranger's connection.
    leg!(rep, svc3(NR_CLOSE, sfd as u64, 0, 0) == 0, b"L49\n");
    leg!(
        rep,
        svc3(NR_CONNECT, sfd as u64, sa.as_ptr() as u64, sa.len() as u64) == -ENOTSOCK,
        b"L50\n"
    );

    // And the table recycles: a fresh socket still works after the close.
    let sfd2 = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    leg!(rep, sfd2 >= 0, b"L51\n");
    leg!(rep, svc3(NR_CLOSE, sfd2 as u64, 0, 0) == 0, b"L52\n");

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
