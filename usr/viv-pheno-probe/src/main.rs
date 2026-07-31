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
const NR_BIND: u64 = 200;
const NR_LISTEN: u64 = 201;
const NR_ACCEPT: u64 = 202;
const NR_CONNECT: u64 = 203;
const NR_ACCEPT4: u64 = 242;
// Readiness (V-5c). aarch64 has no plain poll(2)/select(2), so ppoll IS poll --
// and 73 collides with the NATIVE SYS_GETUID, which is safe only because a
// PHENO_LINUX Proc can never reach a native number (kernel/vivarium.h).
const NR_PPOLL: u64 = 73;
const NR_PSELECT6: u64 = 72;
const AF_INET: u64 = 2;
const AF_INET6: u64 = 10;
const SOCK_STREAM: u64 = 1;
const SOCK_DGRAM: u64 = 2;
const SOCK_NONBLOCK: u64 = 0o4000;
const SOCK_SEQPACKET: u64 = 5;
// POSIX errno values a Linux libc compares against (musl generic bits/errno.h).
const ENOSYS: i64 = 38;
const ENOTSOCK: i64 = 88;
const EPROTONOSUPPORT: i64 = 93;
const EOPNOTSUPP: i64 = 95;
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
        // Task #96: be a handler that touches FP/SIMD, which is what ordinary
        // compiled C does the moment it uses a float or an autovectorised
        // memcpy. Explicit asm so the clobber is guaranteed -- a handler that
        // happened not to touch V registers would let the L40 check below pass
        // on a kernel with no FP save at all. (Legs L155-L157, numbered at the END
        // of the space rather than here: L39-L41 were already taken just below,
        // and renumbering 100+ existing markers to keep these positional would
        // be a far larger change than an out-of-order triple.)
        core::arch::asm!(
            "movi v0.16b,  #0x11", "movi v1.16b,  #0x11",
            "movi v2.16b,  #0x11", "movi v3.16b,  #0x11",
            "movi v4.16b,  #0x11", "movi v5.16b,  #0x11",
            "movi v6.16b,  #0x11", "movi v7.16b,  #0x11",
            "movi v8.16b,  #0x11", "movi v9.16b,  #0x11",
            "movi v10.16b, #0x11", "movi v11.16b, #0x11",
            "movi v12.16b, #0x11", "movi v13.16b, #0x11",
            "movi v14.16b, #0x11", "movi v15.16b, #0x11",
            "movi v16.16b, #0x11", "movi v17.16b, #0x11",
            "movi v18.16b, #0x11", "movi v19.16b, #0x11",
            "movi v20.16b, #0x11", "movi v21.16b, #0x11",
            "movi v22.16b, #0x11", "movi v23.16b, #0x11",
            "movi v24.16b, #0x11", "movi v25.16b, #0x11",
            "movi v26.16b, #0x11", "movi v27.16b, #0x11",
            "movi v28.16b, #0x11", "movi v29.16b, #0x11",
            "movi v30.16b, #0x11", "movi v31.16b, #0x11",
            out("v0") _,  out("v1") _,  out("v2") _,  out("v3") _,
            out("v4") _,  out("v5") _,  out("v6") _,  out("v7") _,
            out("v8") _,  out("v9") _,  out("v10") _, out("v11") _,
            out("v12") _, out("v13") _, out("v14") _, out("v15") _,
            out("v16") _, out("v17") _, out("v18") _, out("v19") _,
            out("v20") _, out("v21") _, out("v22") _, out("v23") _,
            out("v24") _, out("v25") _, out("v26") _, out("v27") _,
            out("v28") _, out("v29") _, out("v30") _, out("v31") _,
            options(nostack),
        );
    }
}

// Task #96 sentinel buffers for the phenotype-path FP check (L39-L41).
static mut FP_SENT: [u8; 512] = [0; 512];
static mut FP_SEEN: [u8; 512] = [0; 512];

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

    // --- Task #96: FP/SIMD survives a handler, on the PHENOTYPE path --------
    //
    // A handler runs on the SAME thread with no context switch, so
    // cpu_switch_context's eager FP save never fires and the handler's V
    // registers would otherwise leak back into the interrupted computation.
    // The pouch prover covers the NATIVE delivery site
    // (notes_deliver_at_el0_return); this covers notes_deliver_linux_locked,
    // the other save site. The restore is shared, so between them every part
    // of the fix is exercised in-guest -- and it matters most HERE, because
    // this is the path an ordinary compiled-C guest reaches.
    //
    // Block, queue a note, then deliver it from inside ONE asm block. The
    // block is not stylistic: a Rust-level svc4() is an ordinary call, and
    // AAPCS64 lets a call clobber V0-V7 and V16-V31, so a check written
    // around the call could not tell the bug apart from the ABI.
    unsafe {
        for k in 0..32usize {
            for j in 0..16usize { FP_SENT[k * 16 + j] = (0x40 + k) as u8; }
        }
    }
    set = bit(SIGPIPE);
    let _ = svc4(NR_RT_SIGPROCMASK, SIG_BLOCK, &set as *const u64 as u64, 0, 8);
    let fired_before = sig_fired();
    let wrc2 = unsafe { svc3(NR_WRITE, 0, &byte as *const u8 as u64, 1) };
    // Queued but blocked: the handler must NOT have run yet, or the sentinel
    // was never at risk and L40 would prove nothing.
    leg!(rep, wrc2 < 0 && sig_fired() == fired_before, b"L155\n");

    unsafe {
        let sp = &raw const FP_SENT as *const u8;
        let dp = &raw mut FP_SEEN as *mut u8;
        core::arch::asm!(
            "ldp q0,  q1,  [{s}, #0]",   "ldp q2,  q3,  [{s}, #32]",
            "ldp q4,  q5,  [{s}, #64]",  "ldp q6,  q7,  [{s}, #96]",
            "ldp q8,  q9,  [{s}, #128]", "ldp q10, q11, [{s}, #160]",
            "ldp q12, q13, [{s}, #192]", "ldp q14, q15, [{s}, #224]",
            "ldp q16, q17, [{s}, #256]", "ldp q18, q19, [{s}, #288]",
            "ldp q20, q21, [{s}, #320]", "ldp q22, q23, [{s}, #352]",
            "ldp q24, q25, [{s}, #384]", "ldp q26, q27, [{s}, #416]",
            "ldp q28, q29, [{s}, #448]", "ldp q30, q31, [{s}, #480]",
            "svc #0",                    // handler delivered at this eret edge
            "stp q0,  q1,  [{d}, #0]",   "stp q2,  q3,  [{d}, #32]",
            "stp q4,  q5,  [{d}, #64]",  "stp q6,  q7,  [{d}, #96]",
            "stp q8,  q9,  [{d}, #128]", "stp q10, q11, [{d}, #160]",
            "stp q12, q13, [{d}, #192]", "stp q14, q15, [{d}, #224]",
            "stp q16, q17, [{d}, #256]", "stp q18, q19, [{d}, #288]",
            "stp q20, q21, [{d}, #320]", "stp q22, q23, [{d}, #352]",
            "stp q24, q25, [{d}, #384]", "stp q26, q27, [{d}, #416]",
            "stp q28, q29, [{d}, #448]", "stp q30, q31, [{d}, #480]",
            s = in(reg) sp,
            d = in(reg) dp,
            in("x8") NR_RT_SIGPROCMASK,
            inout("x0") SIG_UNBLOCK => _,
            in("x1") &set as *const u64 as u64,
            inout("x2") 0u64 => _,
            inout("x3") 8u64 => _,
            out("v0") _,  out("v1") _,  out("v2") _,  out("v3") _,
            out("v4") _,  out("v5") _,  out("v6") _,  out("v7") _,
            out("v8") _,  out("v9") _,  out("v10") _, out("v11") _,
            out("v12") _, out("v13") _, out("v14") _, out("v15") _,
            out("v16") _, out("v17") _, out("v18") _, out("v19") _,
            out("v20") _, out("v21") _, out("v22") _, out("v23") _,
            out("v24") _, out("v25") _, out("v26") _, out("v27") _,
            out("v28") _, out("v29") _, out("v30") _, out("v31") _,
        );
    }

    // The handler ran (so the registers really were exposed to it) ...
    leg!(rep, sig_fired() == fired_before + 1, b"L156\n");
    // ... and every V register came back exactly as it went in.
    leg!(
        rep,
        unsafe { (0..512).all(|i| FP_SEEN[i] == FP_SENT[i]) },
        b"L157\n"
    );

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

    // --- V-5b: the server path ---------------------------------------------
    // THE WHOLE ROUND-TRIP RUNS IN THIS ONE SINGLE-THREADED PROCESS, which is
    // not a shortcut but the only shape available: a PHENO_LINUX Proc cannot
    // spawn a thread (clone is not a table row) and cannot fork. It works
    // because TCP establishes in netd's STACK, not in accept(): the client's
    // connect() completes the handshake against the announced listener, and
    // the server's accept() then finds the connection already waiting. That is
    // exactly what a listen backlog is.

    // The refusals first, none of which touch /net.
    leg!(rep, svc3(NR_BIND, rep as u64, 0, 0) == -ENOTSOCK, b"L53\n");
    leg!(rep, svc3(NR_LISTEN, rep as u64, 1, 0) == -ENOTSOCK, b"L54\n");
    leg!(rep, svc3(NR_ACCEPT, rep as u64, 0, 0) == -ENOTSOCK, b"L55\n");

    // A UDP socket cannot listen: netd's walk has no `listen` file outside
    // /net/tcp, and ctl_announce refuses a non-TCP slot outright.
    let ufd = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    leg!(rep, ufd >= 0, b"L56\n");
    leg!(rep, svc3(NR_LISTEN, ufd as u64, 1, 0) == -EOPNOTSUPP, b"L57\n");
    leg!(rep, svc3(NR_CLOSE, ufd as u64, 0, 0) == 0, b"L58\n");

    // The server. bind() is REMEMBERED, not written -- netd has no bind verb,
    // so nothing has reached it yet at this point.
    let srv = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, srv >= 0, b"L59\n");

    // An unbound TCP socket cannot listen either: Linux would auto-bind an
    // ephemeral port, netd's announce parser rejects port 0, and inventing one
    // would be a translation the guest did not ask for. So it DECLINES.
    leg!(rep, svc3(NR_LISTEN, srv as u64, 1, 0) == -EOPNOTSUPP, b"L60\n");

    // 127.0.0.1:7789. The address must be EXPLICIT loopback, not INADDR_ANY:
    // netd migrates an explicitly-announced 127.x listener onto its loopback
    // stack, while a `*` listener stays on the NIC and never sees this
    // connect. The wildcard/concrete split in the announce builder is what
    // makes that reachable.
    let srv_sa: [u8; 16] = [
        AF_INET as u8, 0, // sin_family
        0x1E, 0x6D, // sin_port = 7789, network order
        127, 0, 0, 1, // sin_addr
        0, 0, 0, 0, 0, 0, 0, 0,
    ];
    leg!(
        rep,
        svc3(NR_BIND, srv as u64, srv_sa.as_ptr() as u64, srv_sa.len() as u64) == 0,
        b"L61\n"
    );

    // accept() before listen() is EINVAL -- and, critically, does NOT block.
    leg!(rep, svc3(NR_ACCEPT, srv as u64, 0, 0) == -22, b"L62\n");

    leg!(rep, svc3(NR_LISTEN, srv as u64, 1, 0) == 0, b"L63\n");
    // A repeat listen() is a POSIX success, not an error.
    leg!(rep, svc3(NR_LISTEN, srv as u64, 5, 0) == 0, b"L64\n");

    // accept4 with flags is REFUSED for the same reason socket() refuses
    // SOCK_NONBLOCK: a guest that asked for a non-blocking accepted socket and
    // got a blocking one blocks where it expected EAGAIN.
    leg!(
        rep,
        svc4(NR_ACCEPT4, srv as u64, 0, 0, SOCK_NONBLOCK) == -22,
        b"L65\n"
    );

    // The client. A CONSTRAINED bind before connect() must be DECLINED rather
    // than silently ignored: netd's dial verb carries only the remote endpoint,
    // so honouring a source port is impossible and pretending to would be the
    // mistranslation the argument domain forbids.
    let cbad = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, cbad >= 0, b"L66\n");
    let cb_sa: [u8; 16] = [AF_INET as u8, 0, 0x30, 0x39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    leg!(
        rep,
        svc3(NR_BIND, cbad as u64, cb_sa.as_ptr() as u64, cb_sa.len() as u64) == 0,
        b"L67\n"
    );
    leg!(
        rep,
        svc3(NR_CONNECT, cbad as u64, srv_sa.as_ptr() as u64, srv_sa.len() as u64)
            == -EOPNOTSUPP,
        b"L68\n"
    );
    leg!(rep, svc3(NR_CLOSE, cbad as u64, 0, 0) == 0, b"L69\n");

    // The real client. This connect completes the handshake against the
    // listener, so the server's accept below has a connection waiting.
    let cli = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, cli >= 0, b"L70\n");
    leg!(
        rep,
        svc3(NR_CONNECT, cli as u64, srv_sa.as_ptr() as u64, srv_sa.len() as u64) == 0,
        b"L71\n"
    );

    // THE ACCEPT. Returns a NEW fd -- unlike connect, which swaps in place --
    // and fills the peer address as a value-result parameter.
    let mut peer: [u8; 16] = [0xAA; 16];
    // NOT 16. addrlen is value-result, so seeding it with the answer would make
    // the "plen == 16" leg below satisfiable by a kernel that never writes it.
    // 0xFFFF is a large-enough capacity for the full copy AND a value only the
    // kernel can turn into 16.
    let mut plen: u32 = 0xFFFF;
    let afd = svc3(
        NR_ACCEPT,
        srv as u64,
        peer.as_mut_ptr() as u64,
        &mut plen as *mut u32 as u64,
    );
    leg!(rep, afd >= 0, b"L72\n");
    leg!(rep, afd != srv && afd != cli, b"L73\n");
    // The peer address was written: AF_INET, and the loopback address the
    // client dialled from. Its PORT is ephemeral, so only the family and
    // address are asserted -- asserting a port netd chose would be asserting
    // netd's allocator, not this translation.
    leg!(rep, plen == 16, b"L74\n");
    leg!(rep, peer[0] == AF_INET as u8 && peer[1] == 0, b"L75\n");
    leg!(
        rep,
        peer[4] == 127 && peer[5] == 0 && peer[6] == 0 && peer[7] == 1,
        b"L76\n"
    );

    // THE BYTES CROSS. Both fds now denote `data` files, so these are
    // untranslated T1 read/write -- which is the point of the whole design:
    // once a socket is connected, the hot path has no socket code in it.
    let msg = b"v5b";
    leg!(
        rep,
        svc3(NR_WRITE, cli as u64, msg.as_ptr() as u64, msg.len() as u64) == 3,
        b"L77\n"
    );
    let mut got = [0u8; 8];
    let n = svc3(NR_READ, afd as u64, got.as_mut_ptr() as u64, got.len() as u64);
    leg!(rep, n == 3, b"L78\n");
    leg!(rep, got[0] == b'v' && got[1] == b'5' && got[2] == b'b', b"L79\n");

    // And back the other way, so the accepted fd is proven writable too.
    let msg2 = b"ok";
    leg!(
        rep,
        svc3(NR_WRITE, afd as u64, msg2.as_ptr() as u64, msg2.len() as u64) == 2,
        b"L80\n"
    );
    let mut got2 = [0u8; 8];
    let n2 = svc3(NR_READ, cli as u64, got2.as_mut_ptr() as u64, got2.len() as u64);
    leg!(rep, n2 == 2, b"L81\n");
    leg!(rep, got2[0] == b'o' && got2[1] == b'k', b"L82\n");

    // The accepted fd IS a tracked socket, in CONNECTED state. EINVAL (a
    // connected socket cannot listen) rather than ENOTSOCK (no entry at all)
    // is what distinguishes the two -- and it is the ONLY leg that would catch
    // accept() forgetting to claim its socktab entry, since read/write on the
    // returned fd work either way (they are untranslated T1 rows on a real
    // Spoor, indifferent to whether the socket table knows about it).
    leg!(rep, svc3(NR_LISTEN, afd as u64, 1, 0) == -22, b"L83\n");

    // The listener SURVIVED the accept -- netd re-armed N with a fresh socket
    // during the swap, so it is still ANNOUNCED. If accept had consumed the
    // listener, a server could accept exactly one connection ever.
    leg!(rep, svc3(NR_LISTEN, srv as u64, 1, 0) == 0, b"L84\n");

    // ...and prove it by ACCEPTING A SECOND CONNECTION, which is the claim
    // `listen() == 0` only gestures at: that asserts the socktab state, this
    // asserts netd actually re-armed the listening socket.
    let cli2 = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, cli2 >= 0, b"L85\n");
    leg!(
        rep,
        svc3(NR_CONNECT, cli2 as u64, srv_sa.as_ptr() as u64, srv_sa.len() as u64) == 0,
        b"L86\n"
    );

    // A SHORT addrlen, to pin the value-result truncation: the address is
    // clipped to the caller's buffer, but *addrlen reports the FULL size, so a
    // Linux caller can tell it was truncated. Byte 8 must stay untouched.
    let mut peer2: [u8; 16] = [0xBB; 16];
    let mut plen2: u32 = 8;
    let afd2 = svc3(
        NR_ACCEPT,
        srv as u64,
        peer2.as_mut_ptr() as u64,
        &mut plen2 as *mut u32 as u64,
    );
    leg!(rep, afd2 >= 0, b"L87\n");
    leg!(rep, plen2 == 16, b"L88\n");
    leg!(rep, peer2[0] == AF_INET as u8 && peer2[4] == 127, b"L89\n");
    leg!(rep, peer2[8] == 0xBB, b"L90\n");
    leg!(rep, svc3(NR_CLOSE, afd2 as u64, 0, 0) == 0, b"L91\n");
    leg!(rep, svc3(NR_CLOSE, cli2 as u64, 0, 0) == 0, b"L92\n");

    leg!(rep, svc3(NR_CLOSE, afd as u64, 0, 0) == 0, b"L93\n");
    leg!(rep, svc3(NR_CLOSE, cli as u64, 0, 0) == 0, b"L94\n");
    leg!(rep, svc3(NR_CLOSE, srv as u64, 0, 0) == 0, b"L95\n");
    // The close hook covers the accepted fd too: its socktab entry was claimed
    // by accept(), so it must be released by close() like any other. Paired
    // with L83, this brackets the entry's whole life: EINVAL while open,
    // ENOTSOCK once closed.
    leg!(rep, svc3(NR_LISTEN, afd as u64, 1, 0) == -ENOTSOCK, b"L96\n");

    // --- V-5c: readiness ----------------------------------------------------
    // A Linux `struct pollfd` -- and the native one is byte-identical, which is
    // why only the FD needs translating.
    #[repr(C)]
    #[derive(Clone, Copy)]
    struct PollFd {
        fd: i32,
        events: i16,
        revents: i16,
    }
    const POLLIN: i16 = 0x001;
    const POLLOUT: i16 = 0x004;

    // The argument domain first, since these need no sockets at all. Each is a
    // shape this kernel cannot serve, and each says so with its own code.
    let mut pfd = [PollFd { fd: rep as i32, events: POLLIN, revents: 0 }];
    let ts_zero: [i64; 2] = [0, 0];

    // nfds == 0 is Linux for "sleep for the timeout", and it is SERVED -- this
    // leg asserted -ENOSYS until V-5c-2 taught the zero-fd forms to route to a
    // real timed sleep. A zero timeout makes it a no-op that returns 0.
    //
    // THIS LEG PROVES PLUMBING, NOT BEHAVIOUR: a kernel that returned 0 without
    // ever waiting would pass it, because nothing here can read a clock (the
    // phenotype has no clock_gettime row). That the sleep actually SLEEPS is
    // measured in `poll.sleep_for_waits`, where timer_now_ns() is reachable.
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 0, ts_zero.as_ptr() as u64, 0, 8, 0) == 0,
        b"L97\n"
    );
    // Over the native cap: a genuinely out-of-range argument, so EINVAL.
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 65, ts_zero.as_ptr() as u64, 0, 8, 0) == -22,
        b"L98\n"
    );
    // A sigmask asks for an ATOMIC mask swap, which is ppoll's entire reason to
    // exist over poll() and has no counterpart here. Declined, never approximated.
    let sigmask: [u64; 1] = [0];
    leg!(
        rep,
        svc6(
            NR_PPOLL,
            pfd.as_mut_ptr() as u64,
            1,
            ts_zero.as_ptr() as u64,
            sigmask.as_ptr() as u64,
            8,
            0
        ) == -ENOSYS,
        b"L99\n"
    );
    // A malformed timespec is Linux's own EINVAL, not ours.
    let ts_bad: [i64; 2] = [0, 1_000_000_000];
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_bad.as_ptr() as u64, 0, 8, 0) == -22,
        b"L100\n"
    );

    // A NON-socket fd passes through untranslated. The report file is a regular
    // file, and a regular file is always ready -- so this also proves the
    // translation is not applied indiscriminately.
    pfd[0] = PollFd { fd: rep as i32, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_zero.as_ptr() as u64, 0, 8, 0) == 1,
        b"L101\n"
    );
    leg!(rep, pfd[0].revents & POLLIN != 0, b"L102\n");
    // ...and the caller's own fd number came back unchanged. The kernel polled a
    // different handle underneath; if that leaked into the array the guest would
    // be holding a readiness-file fd where it wrote a socket.
    leg!(rep, i64::from(pfd[0].fd) == rep, b"L103\n");

    // Now the real thing: a fresh server + client pair, polled at both ends.
    let srv3 = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, srv3 >= 0, b"L104\n");
    // A DIFFERENT port from the V-5b pair above (7789): those listeners are
    // closed by now, but netd's announce would refuse a live collision, and a
    // port clash would read as a poll bug rather than the setup error it is.
    let srv3_sa: [u8; 16] = [
        AF_INET as u8, 0, // sin_family
        0x1E, 0x6C, // sin_port = 7788, network order
        127, 0, 0, 1, // sin_addr
        0, 0, 0, 0, 0, 0, 0, 0,
    ];
    leg!(
        rep,
        svc3(NR_BIND, srv3 as u64, srv3_sa.as_ptr() as u64, srv3_sa.len() as u64) == 0,
        b"L105\n"
    );
    leg!(rep, svc3(NR_LISTEN, srv3 as u64, 1, 0) == 0, b"L106\n");

    // THE #220 LEG, half one: an announced listener with NO call pending must
    // NOT report POLLIN. If it did, a select()-loop server would spin calling
    // accept() on nothing.
    //
    // ON ITS OWN THIS LEG PROVES NOTHING -- a kernel that never reported
    // readiness for anything would pass it. It is meaningful only PAIRED with
    // L110 below, which shows the same fd DOES report POLLIN once a call
    // arrives: together they say the signal fires when it should and not when
    // it should not. (Nor is a real timeout optional here: with a zero timeout
    // this leg would pass because netd's probe had not answered yet, which is
    // the same nothing wearing a different disguise -- task #98.)
    let ts_200ms: [i64; 2] = [0, 200_000_000];
    pfd[0] = PollFd { fd: srv3 as i32, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_200ms.as_ptr() as u64, 0, 8, 0) == 0,
        b"L107\n"
    );

    let cli3 = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, cli3 >= 0, b"L108\n");
    leg!(
        rep,
        svc3(NR_CONNECT, cli3 as u64, srv3_sa.as_ptr() as u64, srv3_sa.len() as u64) == 0,
        b"L109\n"
    );

    // THE #220 LEG, half two, and the reason this chunk touches netd at all.
    // POSIX defines POLLIN on a listener as "a connection is pending -- accept
    // will not block". netd computed readiness from can_recv(), which is false
    // for a listening socket in EVERY state, so this returned 0 forever while a
    // real client sat connected. A server that polls before accepting -- the
    // whole point of poll -- could never learn it had a caller.
    let ts_2s: [i64; 2] = [2, 0];
    pfd[0] = PollFd { fd: srv3 as i32, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_2s.as_ptr() as u64, 0, 8, 0) == 1,
        b"L110\n"
    );
    leg!(rep, pfd[0].revents & POLLIN != 0, b"L111\n");

    let afd3 = svc3(NR_ACCEPT, srv3 as u64, 0, 0);
    leg!(rep, afd3 >= 0, b"L112\n");

    // POLLOUT FIRST, and the order is the point: an accepted socket is writable
    // at once, so this establishes that readiness for THIS fd is being answered
    // truthfully before anything below asks it to stay silent.
    //
    // It also pins the zero-timeout mitigation. netd's readiness probe is
    // ASYNCHRONOUS -- the first poll of a freshly-opened `ready` fd submits it
    // and cannot answer it -- so a literal zero-timeout scan would report
    // not-ready for a plainly writable socket. viv_ppoll gives a caller-supplied
    // 0 a small budget for the probe to land (task #98), and this leg is what
    // says so: remove the budget and it fails.
    pfd[0] = PollFd { fd: afd3 as i32, events: POLLOUT, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_zero.as_ptr() as u64, 0, 8, 0) == 1,
        b"L113\n"
    );
    leg!(rep, pfd[0].revents & POLLOUT != 0, b"L114\n");

    // NOW the silence means something. A connected socket with an empty receive
    // buffer must TIME OUT on POLLIN -- and this is THE leg that proves the
    // readiness-file translation, because the socket's own fd names
    // /net/tcp/N/data, an ordinary dev9p file, and an ordinary file is
    // ALWAYS-READY. Without the swap to the QTPOLL `ready` sibling this would
    // return 1 immediately and every poll-driven read would spin.
    let ts_100ms: [i64; 2] = [0, 100_000_000];
    pfd[0] = PollFd { fd: afd3 as i32, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_100ms.as_ptr() as u64, 0, 8, 0) == 0,
        b"L115\n"
    );
    leg!(rep, pfd[0].revents == 0, b"L116\n");

    // And once the peer sends, POLLIN arrives -- through the DEFERRED path,
    // since the bytes are not there when the poll parks.
    leg!(
        rep,
        svc3(NR_WRITE, cli3 as u64, b"hi".as_ptr() as u64, 2) == 2,
        b"L117\n"
    );
    pfd[0] = PollFd { fd: afd3 as i32, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd.as_mut_ptr() as u64, 1, ts_2s.as_ptr() as u64, 0, 8, 0) == 1,
        b"L118\n"
    );
    leg!(rep, pfd[0].revents & POLLIN != 0, b"L119\n");
    let mut got3 = [0u8; 8];
    leg!(
        rep,
        svc3(NR_READ, afd3 as u64, got3.as_mut_ptr() as u64, got3.len() as u64) == 2,
        b"L120\n"
    );
    leg!(rep, got3[0] == b'h' && got3[1] == b'i', b"L121\n");

    leg!(rep, svc3(NR_CLOSE, afd3 as u64, 0, 0) == 0, b"L122\n");
    leg!(rep, svc3(NR_CLOSE, cli3 as u64, 0, 0) == 0, b"L123\n");
    leg!(rep, svc3(NR_CLOSE, srv3 as u64, 0, 0) == 0, b"L124\n");

    // --- V-5c-2: pselect6, the fd_set reshape -------------------------------
    // Three 1024-bit bitmaps in, one pollfd array out, three bitmaps back. The
    // conversion itself is unit-driven (vivarium.fdset_*); these legs prove the
    // PLUMBING -- that x0..x5 arrive where the translator expects them, that the
    // sets are read from and written back to the caller's own memory, and that
    // the declines are reachable through a real syscall rather than only in a
    // direct call to the pure function.
    //
    // fd_set is 128 bytes = 16 u64 words; bit `fd` lives in word fd/64.
    let mut rdset: [u64; 16] = [0; 16];
    let mut wrset: [u64; 16] = [0; 16];
    let mut exset: [u64; 16] = [0; 16];

    // A zero timeout, so every leg below returns without waiting.
    let ts0: [i64; 2] = [0, 0];

    // The rep fd is an ordinary open file, which POSIX says is ALWAYS ready --
    // both for reading and for writing. So asking about it both ways must come
    // back with BOTH bits set, and the return must be 2: the count is of BITS,
    // not of fds, which is the contract a caller's "while (n--) find next bit"
    // loop depends on.
    rdset[(rep as usize) / 64] = 1u64 << ((rep as usize) % 64);
    wrset[(rep as usize) / 64] = 1u64 << ((rep as usize) % 64);
    let n_ready = svc6(
        NR_PSELECT6,
        (rep + 1) as u64,
        rdset.as_mut_ptr() as u64,
        wrset.as_mut_ptr() as u64,
        0,
        ts0.as_ptr() as u64,
        0,
    );
    leg!(rep, n_ready == 2, b"L125\n");
    leg!(
        rep,
        rdset[(rep as usize) / 64] & (1u64 << ((rep as usize) % 64)) != 0,
        b"L126\n"
    );
    leg!(
        rep,
        wrset[(rep as usize) / 64] & (1u64 << ((rep as usize) % 64)) != 0,
        b"L127\n"
    );

    // THE SETS ARE OVERWRITTEN, NOT MERGED. A bit the caller set for an fd that
    // did NOT become ready must come home CLEAR -- that is how select reports.
    // Bit 0 is asked about and is not a live fd here... which would be POLLNVAL
    // and EBADF, so instead assert the property on a set the kernel never
    // reports into: a zeroed exceptfds passed alongside a ready read.
    rdset = [0; 16];
    wrset = [0; 16];
    exset = [0; 16];
    rdset[(rep as usize) / 64] = 1u64 << ((rep as usize) % 64);
    let n_ex = svc6(
        NR_PSELECT6,
        (rep + 1) as u64,
        rdset.as_mut_ptr() as u64,
        0,
        exset.as_mut_ptr() as u64,
        ts0.as_ptr() as u64,
        0,
    );
    leg!(rep, n_ex == 1, b"L128\n");
    leg!(rep, exset.iter().all(|w| *w == 0), b"L129\n");

    // A SET exceptfds bit declines. There is no POLLPRI to map it to, and the
    // alternative -- dropping it and polling the rest -- turns a pure exceptfds
    // wait into an infinite block instead of an error.
    exset = [0; 16];
    exset[(rep as usize) / 64] = 1u64 << ((rep as usize) % 64);
    leg!(
        rep,
        svc6(
            NR_PSELECT6,
            (rep + 1) as u64,
            0,
            0,
            exset.as_mut_ptr() as u64,
            ts0.as_ptr() as u64,
            0,
        ) == -ENOSYS,
        b"L130\n"
    );

    // A sigmask declines for ppoll's reason -- the atomic mask swap has no
    // counterpart. Note the sixth argument is a POINTER TO A PAIR, not a mask.
    let sigpair: [u64; 2] = [0, 8];
    leg!(
        rep,
        svc6(
            NR_PSELECT6,
            (rep + 1) as u64,
            rdset.as_mut_ptr() as u64,
            0,
            0,
            ts0.as_ptr() as u64,
            sigpair.as_ptr() as u64,
        ) == -ENOSYS,
        b"L131\n"
    );

    // A negative nfds is Linux's own EINVAL.
    leg!(
        rep,
        svc6(NR_PSELECT6, (-1i64) as u64, 0, 0, 0, ts0.as_ptr() as u64, 0) == -22,
        b"L132\n"
    );

    // Every set NULL is the zero-fd form: `select(0, NULL, NULL, NULL, &tv)`,
    // the classic portable sleep. With a zero timeout it is a no-op returning 0.
    // (Like L97, this proves the ROUTING, not that the sleep sleeps -- that is
    // measured in poll.sleep_for_waits, where a clock is reachable.)
    leg!(
        rep,
        svc6(NR_PSELECT6, 0, 0, 0, 0, ts0.as_ptr() as u64, 0) == 0,
        b"L133\n"
    );

    // An absurd nfds is CLAMPED, not refused -- Linux does the same, because a
    // bit above the fd table names an fd that cannot exist. With no sets passed
    // there is nothing to find, so this is the sleep form again.
    leg!(
        rep,
        svc6(NR_PSELECT6, 100000, 0, 0, 0, ts0.as_ptr() as u64, 0) == 0,
        b"L134\n"
    );

    // --- V-5d SA-2: pselect6 over a REAL SOCKET -----------------------------
    // Every leg above uses `rep`, an ordinary file -- which is never translated,
    // so `opened[i]` stays -1 and the fd in the pollfd array is never rewritten.
    // That makes all of them blind to the restore in viv_poll_translated, and
    // L103 above (which claims to test it) blind for the same reason: it also
    // polls a regular file. Delete the restore loop and the whole gate stays
    // green -- which is why these legs exist.
    //
    // A socket fd IS translated: the kernel opens the connection's `ready`
    // sibling and polls THAT. For ppoll the substitution is invisible (only
    // `revents` is written back), but pselect6 uses the pollfd's fd as the BIT
    // INDEX, so an unrestored array reports the READINESS handle's number as
    // the ready fd -- a number the guest never opened.
    let psock = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    leg!(rep, psock >= 0, b"L135\n");
    leg!(
        rep,
        svc3(NR_CONNECT, psock as u64, sa.as_ptr() as u64, sa.len() as u64) == 0,
        b"L136\n"
    );

    // A real timeout, not zero: readiness for a /net socket is one RPC away, so
    // a zero-timeout answer would be "not yet" rather than the truth (task #98,
    // and the same reason L107 spends 200ms).
    let ts_200: [i64; 2] = [0, 200_000_000];
    rdset = [0; 16];
    wrset = [0; 16];
    wrset[(psock as usize) / 64] = 1u64 << ((psock as usize) % 64);
    leg!(
        rep,
        svc6(
            NR_PSELECT6,
            (psock + 1) as u64,
            0,
            wrset.as_mut_ptr() as u64,
            0,
            ts_200.as_ptr() as u64,
            0,
        ) == 1,
        b"L137\n"
    );
    // THE RESTORE PROOF. The bit that came back must be the SOCKET's, not the
    // readiness handle's.
    leg!(
        rep,
        wrset[(psock as usize) / 64] & (1u64 << ((psock as usize) % 64)) != 0,
        b"L138\n"
    );

    // THE OVERWRITE PROOF, which L129 could not give: a bit set GOING IN for an
    // fd that does not become ready must come home CLEAR. A connected UDP socket
    // with nothing sent to it is writable but not readable, so asking about the
    // read side alone is a wait that times out -- and the bit must be gone.
    rdset = [0; 16];
    rdset[(psock as usize) / 64] = 1u64 << ((psock as usize) % 64);
    leg!(
        rep,
        svc6(
            NR_PSELECT6,
            (psock + 1) as u64,
            rdset.as_mut_ptr() as u64,
            0,
            0,
            ts_200.as_ptr() as u64,
            0,
        ) == 0,
        b"L139\n"
    );
    leg!(rep, rdset.iter().all(|w| *w == 0), b"L140\n");
    leg!(rep, svc3(NR_CLOSE, psock as u64, 0, 0) == 0, b"L141\n");

    // --- V-5d SA-3: a bit ABOVE nfds is cleared too -------------------------
    // Linux copies FDS_BYTES(n) bytes back out of a buffer it zeroed, so a bit
    // above nfds but inside the same 8-byte unit is in range of the COPY even
    // though it was out of range of the SCAN -- and comes home clear. nfds = 1
    // examines fd 0 only, so bit 5 is never looked at, and the call is the
    // no-fd sleep form returning 0.
    rdset = [0; 16];
    rdset[0] = 1u64 << 5;
    leg!(
        rep,
        svc6(NR_PSELECT6, 1, rdset.as_mut_ptr() as u64, 0, 0, ts0.as_ptr() as u64, 0) == 0,
        b"L142\n"
    );
    leg!(rep, rdset[0] == 0, b"L143\n");

    // --- V-5d F1: a NEGATIVE fd is INERT ------------------------------------
    // poll(2): "If fd is negative, then the corresponding events field is
    // ignored and the revents field returns zero." It contributes nothing to
    // the count -- that is how a fixed-array event loop disables a slot without
    // compacting. Thylacine's NATIVE poll says the opposite and documents it
    // (negative => POLLNVAL, and poll_scan_one counts it READY), which is a fine
    // native ABI and is not Linux's.
    //
    // So the translator cannot pass these through: any disabled slot would make
    // the native fast path fire, and a ppoll asked to block FOREVER would return
    // AT ONCE, every time. This leg is the one that catches that, and it needs a
    // real timeout to do it -- with ts0 a correct kernel and a broken one both
    // return promptly, and the leg would prove nothing.
    let mut pfd2 = [
        PollFd { fd: -1, events: POLLIN, revents: 0 },
        PollFd { fd: -1, events: POLLIN, revents: 0 },
    ];
    leg!(
        rep,
        svc6(NR_PPOLL, pfd2.as_mut_ptr() as u64, 1, ts_200.as_ptr() as u64, 0, 8, 0) == 0,
        b"L144\n"
    );
    leg!(rep, pfd2[0].revents == 0, b"L145\n");

    // And MIXED: an always-ready regular file beside a disabled slot must report
    // exactly one ready fd -- not two, and not the file's readiness lost to the
    // disabled slot's early return.
    pfd2[0] = PollFd { fd: rep as i32, events: POLLIN, revents: 0 };
    pfd2[1] = PollFd { fd: -1, events: POLLIN, revents: 0 };
    leg!(
        rep,
        svc6(NR_PPOLL, pfd2.as_mut_ptr() as u64, 2, ts_200.as_ptr() as u64, 0, 8, 0) == 1,
        b"L146\n"
    );
    leg!(
        rep,
        pfd2[0].revents & POLLIN != 0 && pfd2[1].revents == 0 && pfd2[1].fd == -1,
        b"L147\n"
    );

    // --- V-5d F2: a failed accept must not keep the connection --------------
    // By the time accept writes the peer address it has fully committed: the
    // accepted fd is open, its socktab entry is claimed, and netd's connection
    // is live and held by that fd ALONE. A bare EFAULT there hands the guest
    // three resources and tells it nothing -- the fd number was the return
    // value it just lost -- so they are reclaimable only by Proc death. netd's
    // slot pool is shared across every /net client on the box, which is what
    // lifts this above a self-inflicted leak.
    //
    // MEASURING IT NEEDS A RUN OF fds, NOT ONE. accept internally opens listen,
    // remote and data and closes two of them, so a single probe fd lands on the
    // same number whether or not the third leaked. Comparing a RUN taken before
    // against the same run taken after is independent of that internal
    // ordering: a leak anywhere in the low range shifts the tail.
    let srv4 = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(rep, srv4 >= 0, b"L148\n");
    let srv4_sa: [u8; 16] = [
        AF_INET as u8, 0, // sin_family
        0x1E, 0x6E, // sin_port = 7790, network order
        127, 0, 0, 1, // sin_addr
        0, 0, 0, 0, 0, 0, 0, 0,
    ];
    leg!(
        rep,
        svc3(NR_BIND, srv4 as u64, srv4_sa.as_ptr() as u64, srv4_sa.len() as u64) == 0,
        b"L149\n"
    );
    leg!(rep, svc3(NR_LISTEN, srv4 as u64, 1, 0) == 0, b"L150\n");

    // THE CLIENT COMES FIRST, and the ordering is the whole measurement. Taking
    // `before` while cli4 did not yet exist would compare a landscape without
    // the client against one with it -- the two runs would differ by one fd for
    // a reason that has nothing to do with a leak, and the leg would fail
    // against correct code. (It did, on the first run of this probe.)
    let cli4 = svc3(NR_SOCKET, AF_INET, SOCK_STREAM, 0);
    leg!(
        rep,
        cli4 >= 0
            && svc3(NR_CONNECT, cli4 as u64, srv4_sa.as_ptr() as u64, srv4_sa.len() as u64)
                == 0,
        b"L151\n"
    );

    let mut before = [0i64; 3];
    for k in 0..3 {
        before[k] = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    }
    for k in 0..3 {
        svc3(NR_CLOSE, before[k] as u64, 0, 0);
    }
    leg!(rep, before[0] >= 0 && before[1] >= 0 && before[2] >= 0, b"L152\n");

    // A call is pending, so accept commits -- and then fails on the address
    // write-back, because addrlen names an address that cannot be read. That is
    // the FIRST of the four arms, reached before anything is written.
    let mut peer_sa = [0u8; 16];
    leg!(
        rep,
        svc4(
            NR_ACCEPT4,
            srv4 as u64,
            peer_sa.as_mut_ptr() as u64,
            1, /* an unreadable addrlen pointer */
            0,
        ) == -14, /* EFAULT */
        b"L153\n"
    );

    let mut after = [0i64; 3];
    for k in 0..3 {
        after[k] = svc3(NR_SOCKET, AF_INET, SOCK_DGRAM, 0);
    }
    leg!(
        rep,
        after[0] == before[0] && after[1] == before[1] && after[2] == before[2],
        b"L154\n"
    );
    for k in 0..3 {
        svc3(NR_CLOSE, after[k] as u64, 0, 0);
    }
    svc3(NR_CLOSE, cli4 as u64, 0, 0);
    svc3(NR_CLOSE, srv4 as u64, 0, 0);

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
