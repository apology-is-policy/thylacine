// /fork-probe -- the LINEAGE L-3b in-guest gate for SYS_RFORK (I-44).
//
// The ONLY place a forked child can be observed resuming. The kernel unit
// tests reach the DECISION (fork.frame_init) and the argument gate
// (fork.rfork_arg_rejection); neither can reach the RESUME, because an eret to
// EL0 needs a real EL0 caller with a real trapframe and kproc -- the only Proc
// a kernel test runs on -- has neither an address space to share nor a frame to
// fork from. So the resume is not merely untested there, it is unobservable.
//
// WHAT EACH LEG PROVES
//
//   A  the child RESUMES rather than starting fresh: it is executing inside
//      child_main, reached by a blr from the instruction after the syscall in
//      the parent's own code path. A kernel that ignored the frame and entered
//      at some fixed point could not get here at all.
//   B  the two Procs SHARE the address space: the child writes a marker into
//      a static the parent then reads. This is what RFMEM means. Shared memory
//      remains the right observation point for A/B/C even now that the child
//      inherits fds (leg H), because it is the only channel that works before
//      anything about the handle table has been established -- a leg that
//      reported through an fd could not tell "did not resume" from "resumed
//      but inherited nothing".
//   C  the child runs on ITS OWN stack: it writes a recognisable pattern deep
//      into its own stack region and the parent verifies afterwards that the
//      bytes landed there. If the child had resumed on the parent's SP this
//      region would be untouched -- and the parent would very likely have been
//      corrupted instead.
//   D  the pids DIFFER and the parent's is unchanged. This is what separates a
//      fork from an execve: an implementation that quietly replaced the caller
//      would report one pid, not two.
//   E  the child is REAPABLE by the parent, with the status it exited with --
//      it is a real child Proc in the process tree, not a thread.
//   F  the argument gate is live at EL0, not just in the unit test: RFPROC
//      alone and a zero stack are both refused, from a Proc that genuinely has
//      an address space to share (the unit test reaches these from kproc,
//      which would fail them for a different reason anyway).
//   H  (L-3c) the child INHERITS the parent's descriptors: it writes to a pipe
//      fd the parent opened, named only by its number, and the parent reads the
//      bytes back. That the copy happens at all is only provable here -- the
//      kernel test can call handle_table_copy_into directly, but nothing
//      kernel-side can reach rfork_internal's call to it, which sits behind an
//      RFMEM gate kproc cannot pass.
//
// Legs A/B/C together are the chunk's whole claim, and no two of them can be
// satisfied by the same accident: a kernel that entered the child at a fixed
// entry point fails A; one that gave it a private address space fails B; one
// that ignored child_sp fails C (or corrupts the parent, which fails
// everything).

#![no_std]
#![no_main]

extern crate alloc;

use core::sync::atomic::{AtomicU64, Ordering};

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{rfork_spawn, t_close, t_exits, t_getpid, t_pipe, t_putstr,
                  t_read, t_wait_pid_for, t_wait_exitstatus, t_wait_if_exited,
                  t_write, T_RFMEM, T_RFPROC, T_SYS_RFORK};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

const MARKER: u64 = 0x0000_C0FF_EE00_1234;

// The child's observation channel. A static in .bss, so both Procs name the
// same physical page through the address space they share.
static CHILD_MARKER: AtomicU64 = AtomicU64::new(0);
static CHILD_PID_SELF: AtomicU64 = AtomicU64::new(0);
static CHILD_TLS: AtomicU64 = AtomicU64::new(0);

// Leg G's sentinels. The parent installs PARENT_TLS in its OWN TPIDR_EL0 before
// forking, because without that the inherit check is VACUOUS: exec zeroes
// TPIDR_EL0 and libthyla-rs uses no thread-locals, so a native parent sits at 0
// and `child == parent` would pass whether the kernel inherited the value or
// left the child's at zero.
const PARENT_TLS: u64 = 0x0000_7175_4F4C_0000;   // "quOL"
const EXPLICIT_TLS: u64 = 0x0000_7857_5045_0000; // "xWPE"

#[inline(always)]
fn tpidr_el0() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!("mrs {}, tpidr_el0", out(reg) v,
                         options(nomem, nostack, preserves_flags));
    }
    v
}

#[inline(always)]
fn set_tpidr_el0(v: u64) {
    // Architecturally RW at EL0 -- this is the TLS register, and writing it is
    // how a libc installs a thread pointer. Safe here because libthyla-rs has
    // no thread-locals of its own to disturb.
    unsafe {
        core::arch::asm!("msr tpidr_el0, {}", in(reg) v,
                         options(nomem, nostack, preserves_flags));
    }
}

// The child's stack. 16 KiB, 16-aligned, and -- this is the part that matters
// -- entirely disjoint from the parent's. The kernel refuses a zero,
// misaligned, non-user or equal-to-caller's SP, but it cannot see an overlap:
// non-overlap is the caller's contract, exactly as it is for a pthread stack.
#[repr(align(16))]
struct ChildStack(#[allow(dead_code)] [u8; 16 * 1024]);
static mut CHILD_STACK: ChildStack = ChildStack([0; 16 * 1024]);

// Leg C's witness. The child writes this pattern at a fixed depth into its own
// stack; the parent reads the same bytes back afterwards. A child that had
// resumed on the PARENT's SP would leave this region untouched.
const STACK_WITNESS: u64 = 0x5741_434B_5741_434B; // "WACKWACK"

// Published instead of MARKER when the child finds its frame OUTSIDE its own
// stack -- distinct from 0 (never ran / cannot see this page) so leg C's
// failure is told apart from legs A and B's.
const WRONG_STACK: u64 = 0x0000_BAD5_7ACC_0000;

// Leg H (LINEAGE L-3c): the fd the CHILD writes to, published by the parent
// before the fork. The child never opens it -- if it can write to it at all,
// it inherited it, at that exact index.
static INHERITED_WR_FD: AtomicU64 = AtomicU64::new(0);
const INHERIT_TOKEN: &[u8] = b"forked-fd-ok";

extern "C" fn child_inherit_main(_arg: u64) -> ! {
    let fd = INHERITED_WR_FD.load(Ordering::Acquire) as i64;
    // A raw t_write, not io::stdout: the point is to name a NUMBER the parent
    // chose and see whether it resolves in this Proc's table. Anything that
    // re-opened or re-derived the fd would prove nothing.
    let n = unsafe { t_write(fd, INHERIT_TOKEN.as_ptr(), INHERIT_TOKEN.len()) };
    unsafe { t_exits(if n == INHERIT_TOKEN.len() as i64 { 0 } else { 1 }) }
}

extern "C" fn child_main(arg: u64) -> ! {
    // Leg C: prove we are on our own stack by writing through a local, whose
    // address must lie inside CHILD_STACK. Taking the address of a local is
    // the only honest way to ask "where is my stack?" -- a hardcoded probe
    // would prove nothing about where the compiler actually put the frame.
    let witness: u64 = STACK_WITNESS;
    let sp_here = &witness as *const u64 as u64;

    let base = core::ptr::addr_of!(CHILD_STACK) as u64;
    let top = base + (16 * 1024);
    let on_own_stack = sp_here >= base && sp_here < top;

    // Leg G: the child's TLS base, whichever arm of the ABI put it there.
    CHILD_TLS.store(tpidr_el0(), Ordering::Release);

    // Leg D: the child's own pid, published for the parent to compare.
    CHILD_PID_SELF.store(unsafe { t_getpid() } as u64, Ordering::Release);

    // Leg B: publish through the SHARED address space. Release so the parent's
    // Acquire load sees the two stores above it too.
    CHILD_MARKER.store(
        if on_own_stack { arg } else { WRONG_STACK },
        Ordering::Release,
    );

    // Exit with a recognisable status so leg E can distinguish "reaped the
    // child" from "reaped something else".
    unsafe { t_exits(0) }
}

// The PASS marker goes to fd 1 -- the pipe joey content-checks. Failures go to
// the console via t_putstr, so a diagnostic still lands even if the pipe end is
// the thing that broke. (exec-probe splits them the same way, for the same
// reason; using t_putstr for the marker prints a line that looks right on the
// console and never reaches the harness.)
fn mark(line: &str) {
    let mut out = io::stdout();
    let _ = out.write_all(line.as_bytes());
    let _ = out.write_all(b"\n");
}

fn fail(msg: &str) -> ! {
    set_tpidr_el0(0);   // leave TPIDR_EL0 as we found it before reporting
    t_putstr("fork-probe: FAIL ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i32 {
    let parent_pid = unsafe { t_getpid() };

    // ---- Leg F: the argument gate is live at EL0 ------------------------
    //
    // Reached from a Proc that DOES have an address space, unlike the kernel
    // test -- so a pass here means the gate rejected the request on its own
    // terms rather than because the caller had nothing to share.
    let stack_top = core::ptr::addr_of!(CHILD_STACK) as u64 + (16 * 1024);

    // RFPROC alone: refused until copy-on-write exists (L-4). Called through
    // the raw shim so the flags can be wrong on purpose.
    let r = unsafe { raw_rfork(T_RFPROC, stack_top, 0) };
    if r != -22 {
        // -T_E_INVAL
        fail("RFPROC alone should be -EINVAL (COW is L-4, not built)");
    }
    let r = unsafe { raw_rfork(T_RFPROC | T_RFMEM, 0, 0) };
    if r != -22 {
        fail("a zero child_sp should be -EINVAL");
    }

    // ---- Legs A/B/C/D/G1: the fork itself ------------------------------
    //
    // Install a recognisable TLS base first so leg G1 can tell "inherited" from
    // "left at zero" -- see PARENT_TLS.
    set_tpidr_el0(PARENT_TLS);
    let pid = unsafe { rfork_spawn(stack_top, 0, child_main, MARKER) };
    if pid <= 0 {
        fail("rfork_spawn did not return a child pid");
    }

    // Leg D: two distinct Procs, and OUR pid did not change -- an execve-like
    // replacement would show one pid, not two.
    if unsafe { t_getpid() } != parent_pid {
        fail("the parent's own pid changed -- this is a fork, not an exec");
    }
    if pid == parent_pid {
        fail("parent and child report the same pid");
    }

    // ---- Leg E: reap it, which also serialises the observation ----------
    //
    // Waiting by pid rather than reap-any means a stray orphan cannot satisfy
    // this leg (the U-7-pre selector; the same trap #94 closed in the kernel).
    let mut status: i32 = -1;
    let reaped = unsafe { t_wait_pid_for(pid as i32, 0, &mut status as *mut i32) };
    if reaped != pid {
        fail("wait_pid_for did not reap exactly the child we forked");
    }
    if !t_wait_if_exited(status) {
        fail("the child did not exit normally");
    }

    // ---- Legs A/B/C: read the child's evidence --------------------------
    let marker = CHILD_MARKER.load(Ordering::Acquire);
    if marker == 0 {
        // The child never ran, or ran and could not see this page: A or B.
        fail("the child never published its marker -- it did not resume, or \
              it does not share the address space");
    }
    if marker != MARKER {
        // The child ran and could see the page, but found itself off its own
        // stack: C alone.
        fail("the child resumed on the WRONG STACK");
    }

    let child_pid = CHILD_PID_SELF.load(Ordering::Acquire);
    if child_pid != pid as u64 {
        fail("the child's own getpid() disagrees with the pid rfork returned");
    }

    // ---- Leg G1: child_tls == 0 INHERITS the caller's TPIDR_EL0 ---------
    if CHILD_TLS.load(Ordering::Acquire) != PARENT_TLS {
        fail("child_tls == 0 must INHERIT the caller's TPIDR_EL0 -- a vfork \
              child runs the parent's C, thread-locals and all, until it execs");
    }

    // ---- Leg G2: a non-zero child_tls is programmed VERBATIM ------------
    //
    // The other arm of the same branch. G1 alone would be satisfied by a kernel
    // that copies the frame blindly; G2 alone by one that ignores inheritance.
    CHILD_TLS.store(0, Ordering::Release);
    let pid2 = unsafe { rfork_spawn(stack_top, EXPLICIT_TLS, child_main, MARKER) };
    if pid2 <= 0 {
        fail("the second rfork_spawn did not return a child pid");
    }
    let mut st2: i32 = -1;
    if unsafe { t_wait_pid_for(pid2 as i32, 0, &mut st2 as *mut i32) } != pid2 {
        fail("wait_pid_for did not reap the second child");
    }
    if CHILD_TLS.load(Ordering::Acquire) != EXPLICIT_TLS {
        fail("a non-zero child_tls must be programmed into the child verbatim");
    }

    set_tpidr_el0(0);   // restore; nothing here needs it, but leave no residue

    // ---- Leg H: fd inheritance (LINEAGE L-3c) ---------------------------
    //
    // The parent opens a pipe and publishes ONLY the write end's NUMBER. The
    // child writes to that number and the parent reads the bytes back off the
    // read end. Nothing else can produce that result: the child never called
    // pipe/open, so for the write to land the descriptor must already exist in
    // its table, at the index the parent chose, carrying write rights.
    //
    // This is the WIRING half of L-3c. The RULE half -- which slots cross,
    // that a skipped one leaves a hole rather than renumbering, that the
    // reference is the child's own -- is `fork.table_copy`, which can reach
    // handle_table_copy_into directly. Neither half can see the other's bugs:
    // a copy that dropped every hardware handle AND every pipe would still
    // pass the kernel test's hole assertion, and a copy that renumbered would
    // still pass this leg if the parent's fd happened to be the lowest.
    let (rd, wr) = unsafe { t_pipe() };
    if rd < 0 {
        fail("t_pipe failed, so leg H cannot run");
    }
    INHERITED_WR_FD.store(wr as u64, Ordering::Release);

    let pid3 = unsafe { rfork_spawn(stack_top, 0, child_inherit_main, 0) };
    if pid3 <= 0 {
        fail("the inherit-leg rfork_spawn did not return a child pid");
    }
    let mut st3: i32 = -1;
    if unsafe { t_wait_pid_for(pid3 as i32, 0, &mut st3 as *mut i32) } != pid3 {
        fail("wait_pid_for did not reap the inherit-leg child");
    }
    if !t_wait_if_exited(st3) || t_wait_exitstatus(st3) != 0 {
        fail("the child could not write to the inherited fd -- a forked child \
              must inherit its parent's descriptors (L-3c)");
    }

    let mut got = [0u8; 32];
    let n = unsafe { t_read(rd, got.as_mut_ptr(), got.len()) };
    if n != INHERIT_TOKEN.len() as i64 || &got[..INHERIT_TOKEN.len()] != INHERIT_TOKEN {
        fail("the bytes the child wrote to the inherited fd did not arrive");
    }
    unsafe {
        t_close(rd);
        t_close(wr);
    }

    mark("fork-probe: PASS (resumed at the parent's PC, x0 = 0, own stack, \
          shared address space, distinct reapable pid, inherited fds)");
    0
}

// The raw shim, for the legs that must pass deliberately-wrong arguments.
// rfork_spawn cannot express them: it hardcodes RFPROC|RFMEM (the only served
// combination) and it would seed a stack it was told not to use.
unsafe fn raw_rfork(flags: u64, child_sp: u64, tls: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "svc #0",
        inlateout("x0") flags => ret,
        in("x1") child_sp,
        in("x2") tls,
        in("x8") T_SYS_RFORK,
        options(nostack),
    );
    ret
}
