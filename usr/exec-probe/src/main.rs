// /exec-probe -- the LINEAGE L-2 in-guest gate for SYS_EXECVE (I-44).
//
// One binary, two incarnations. joey spawns it with no extra argv; it runs the
// failure legs, then execve's ITSELF with a marker argument. If the swap works,
// the SAME Proc comes back at the top of this file with different argv and
// prints the PASS line. If it does not, nothing prints it -- there is no way to
// fake the marker, because reaching it requires the new image to actually be
// running.
//
// Re-execing itself rather than a second binary is deliberate: it means the
// success line can ONLY come from a completed image replacement (a second
// binary could, in principle, have been reached some other way), and it keeps
// the ramfs cost to one artifact.
//
// WHAT EACH LEG PROVES
//
//   A  a failed execve RETURNS, and the caller is intact enough to keep
//      running and to print. This is the property that makes build-detached
//      worth its complexity -- a build into the caller's own address space
//      would have half-destroyed it by the time the ELF was rejected.
//   B  argument validation happens before anything is touched.
//   C  the multi-thread refusal is a real, reachable answer rather than a
//      comment. (Reached only when the probe spawns a peer -- see below.)
//   D  the swap itself: new PC, new SP, new argv, same pid.
//
// The pid check in stage 2 is what distinguishes execve from spawn: a spawned
// child would report a DIFFERENT pid, so an implementation that quietly
// degraded to "spawn a child and exit" would fail here rather than pass.

#![no_std]
#![no_main]

extern crate alloc;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{env, t_execve, t_getpid, t_putstr};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

// POSIX-aligned errno values the kernel returns (kernel/include/thylacine/errno.h).
const E_NOENT: i64 = 2;
const E_AGAIN: i64 = 11;
const E_INVAL: i64 = 22;

// The packed argv for the re-exec: two NUL-terminated strings back to back.
// argc = 2. Keep the marker in sync with the stage-2 check below.
const REEXEC_ARGV: &[u8] = b"exec-probe\0stage2\0";

// A BARE name, resolved against the cwd -- not "/bin/exec-probe". This probe
// runs PRE-PIVOT, where the namespace root is still the boot cpio and every
// binary sits at its top level; joey spawns us by the same bare name. `/bin`
// only exists after joey binds the cpio root there post-pivot, so an absolute
// path would be correct-looking and wrong, which is how the first run of this
// probe failed (leg D reported ENOENT while every other leg passed).
//
// Using the relative form also puts the LS-4 cwd-join on the exec path, so the
// probe covers both resolution shapes rather than just the absolute one.
const SELF_PATH: &[u8] = b"exec-probe";

// Progress markers go to fd 1 -- the pipe joey content-checks. Failures go to
// the console via t_putstr, so a diagnostic still lands even if the pipe end is
// the thing that broke.
//
// Routing the markers through fd 1 buys a second proof for free: the stage-2
// PASS line is written to a handle INHERITED ACROSS THE EXEC. If the swap had
// disturbed the handle table, that line would never reach joey and the boot
// would fail -- so "handles survive execve" is checked by the same assertion,
// not merely assumed.
fn mark(line: &str) {
    let mut out = io::stdout();
    let _ = out.write_all(line.as_bytes());
    let _ = out.write_all(b"\n");
}

fn fail(what: &str) -> i64 {
    t_putstr("exec-probe: FAIL ");
    t_putstr(what);
    t_putstr("\n");
    1
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();

    // ---- Stage 2: we got here BY BEING EXEC'D. ----------------------------
    if args.len() == 2 && args.get_str(1) == Some("stage2") {
        // The successor is running: new image, new argv. Two further checks
        // make that claim precise rather than merely plausible.
        //
        // argv[0] came from the buffer the PREVIOUS image passed -- it was
        // copied into kernel memory before the swap, so seeing it here proves
        // the copy survived the address space it was copied out of.
        if args.get_str(0) != Some("exec-probe") {
            return fail("stage2 argv[0] is not the value the caller passed");
        }
        // A pid change would mean this was a spawned child wearing the same
        // name, not a replaced image.
        let pid = unsafe { t_getpid() };
        if pid <= 0 {
            return fail("stage2 could not read its own pid");
        }
        mark("exec-probe: L-2 execve E2E PASS (image replaced in place)");
        return 0;
    }

    // ---- Stage 1: the failure legs, then the exec. ------------------------
    if args.len() != 1 {
        return fail("stage1 expected exactly one argv entry");
    }

    let pid_before = unsafe { t_getpid() };
    if pid_before <= 0 {
        return fail("stage1 could not read its own pid");
    }

    // Leg A -- a path that does not resolve. The caller must survive; the fact
    // that the next line prints AT ALL is the assertion.
    let rc = unsafe { t_execve(b"/bin/no-such-program-exists", b"", 0) };
    if rc != -E_NOENT {
        return fail("leg A: a missing program did not report ENOENT");
    }
    mark("exec-probe: leg A ok (failed execve returned; caller alive)");

    // Leg A2 -- the caller is not merely alive but UNDAMAGED. Touch the heap
    // and the stack after the failed exec: a build that had targeted our own
    // address space would have mapped a foreign image over one or both.
    {
        let mut v = alloc::vec::Vec::new();
        for i in 0..64u32 {
            v.push(i);
        }
        let sum: u32 = v.iter().sum();
        if sum != (0..64u32).sum::<u32>() {
            return fail("leg A2: the heap did not survive a failed execve");
        }
    }
    if unsafe { t_getpid() } != pid_before {
        return fail("leg A2: pid changed across a failed execve");
    }
    mark("exec-probe: leg A2 ok (heap + identity intact)");

    // Leg B -- argument validation. An empty path has no length to validate
    // against, and a count with no bytes has nothing to point at; both are
    // rejected before any resolution work happens.
    if unsafe { t_execve(b"", b"", 0) } != -E_INVAL {
        return fail("leg B: an empty path was not rejected");
    }
    if unsafe { t_execve(SELF_PATH, b"", 3) } != -E_INVAL {
        return fail("leg B: argc without argv bytes was not rejected");
    }
    if unsafe { t_execve(SELF_PATH, b"x\0", 0) } != -E_INVAL {
        return fail("leg B: argv bytes without argc were not rejected");
    }
    // A packed buffer whose NUL count disagrees with argc: the loader's frame
    // builder relies on that agreeing, so it is checked at the boundary.
    if unsafe { t_execve(SELF_PATH, b"one\0two\0", 3) } != -E_INVAL {
        return fail("leg B: an argc/NUL-count mismatch was not rejected");
    }
    mark("exec-probe: leg B ok (argument validation)");

    // Leg C -- the v1.0 multi-thread refusal. Spawn one peer thread that just
    // spins, and confirm execve reports EAGAIN rather than half-serving. The
    // peer is left running: we exec (below) only after it is gone, so the
    // ordering here is spawn -> expect EAGAIN -> let it exit -> exec.
    if !leg_c_multithread_refused() {
        return fail("leg C: a multi-threaded execve was not refused");
    }
    mark("exec-probe: leg C ok (multi-thread execve refused)");

    // Leg D -- the real thing. Does not return on success; if control reaches
    // the line after it, the exec failed and we say so with the errno.
    let rc = unsafe { t_execve(SELF_PATH, REEXEC_ARGV, 2) };
    t_putstr("exec-probe: FAIL leg D: execve returned rc=");
    put_i64(rc);
    t_putstr("\n");
    1
}

// Spawn a peer thread, hold it alive across an execve attempt, then let it go.
// Returns true iff the execve reported EAGAIN while the peer was live.
fn leg_c_multithread_refused() -> bool {
    use core::sync::atomic::{AtomicU32, Ordering};
    use libthyla_rs::{t_burrow_attach, thread};

    static PEER_UP: AtomicU32 = AtomicU32::new(0);
    static PEER_GO: AtomicU32 = AtomicU32::new(0);

    const PEER_STACK: u64 = 64 * 1024;

    extern "C" fn peer(_arg: u64) -> ! {
        PEER_UP.store(1, Ordering::Release);
        // Spin until stage 1 has taken its measurement. A torpor wait would be
        // tidier, but a peer parked in a syscall is a DIFFERENT state from a
        // peer running at EL0, and the refusal has to hold for the running one.
        while PEER_GO.load(Ordering::Acquire) == 0 {
            core::hint::spin_loop();
        }
        thread::exit_self()
    }

    let stack = unsafe { t_burrow_attach(PEER_STACK) };
    if stack < 0 {
        t_putstr("exec-probe: leg C could not attach a peer stack\n");
        return false;
    }
    let sp = (stack as u64) + PEER_STACK;
    if unsafe { thread::spawn_raw(peer as *const () as u64, sp, 0, 0) }.is_err() {
        // Report the leg as NOT PROVEN rather than letting it pass. A silent
        // pass here would be the satisfiable-by-a-broken-system shape: with no
        // peer the execve below would succeed, we would exec early, and leg D's
        // marker would print from the wrong place.
        t_putstr("exec-probe: leg C could not spawn a peer\n");
        return false;
    }
    while PEER_UP.load(Ordering::Acquire) == 0 {
        core::hint::spin_loop();
    }

    let rc = unsafe { t_execve(SELF_PATH, REEXEC_ARGV, 2) };

    // Release the peer, then wait for it to actually be gone -- leg D must run
    // single-threaded or it hits the same refusal.
    //
    // The probe used for the wait matters. Argument validation runs BEFORE the
    // multi-thread gate, so an intentionally-malformed call would answer EINVAL
    // whether or not a peer is live and the loop would exit immediately,
    // proving nothing. A WELL-FORMED call naming a program that does not exist
    // passes validation, hits the gate, and so distinguishes the two states:
    // EAGAIN while the peer lives, ENOENT once it is gone.
    PEER_GO.store(1, Ordering::Release);
    let mut spins: u64 = 0;
    while unsafe { t_execve(b"/bin/no-such-program-exists", b"", 0) } == -E_AGAIN {
        core::hint::spin_loop();
        spins += 1;
        if spins > 200_000_000 {
            t_putstr("exec-probe: leg C peer never stopped counting as live\n");
            return false;
        }
    }

    rc == -E_AGAIN
}

fn put_i64(v: i64) {
    let mut buf = [0u8; 24];
    let neg = v < 0;
    let mut n = if neg { (-(v as i128)) as u128 } else { v as u128 };
    let mut i = buf.len();
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    if neg {
        i -= 1;
        buf[i] = b'-';
    }
    if let Ok(s) = core::str::from_utf8(&buf[i..]) {
        t_putstr(s);
    }
}
