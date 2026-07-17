// /stack-probe -- the Stage-8b settled-thread kstack inspect E2E (DEBUG-FS 5b). It
// proves the 8b headline: reading the KERNEL stack of a thread blocked DEEP in a
// syscall, WITHOUT a debug-stop (the Linux /proc/<pid>/stack tier). A kernel unit
// test cannot produce this -- a kthread never EL0-returns, and the synthetic test
// hand-builds a settled thread; only a real EL0 Proc blocked in torpor_wait does.
//
// Flow:
//   1. spawn /stack-child (it blocks forever in torpor_wait -> sleep() on the
//      torpor rendez: settled, on_cpu==false, NOT debug-stopped),
//   2. poll /proc/<pid>/kstack (OWNER-authorized -- same principal; NO attach, NO
//      stop) until the child is settled with a STABLE symbolized kernel backtrace
//      (3 identical settled reads -> durably blocked, not a transient preempt),
//   3. assert a non-empty walk + a SYMBOLIZED frame ("name+0x...") -- a real
//      kernel backtrace read off a blocked-in-syscall thread, which the 8a
//      debug-stop (parking only at the EL0-return tail) could never give,
//   4. killgrp + reap the child (the kill wakes the torpor sleeper via #811),
//      exit 0.
//
// joey spawns + reaps + asserts exit 0 + the "stack-probe: PASS" marker, so any
// failure gates the boot. On any failure path the child is killed + reaped (an
// unreaped killed child orphans to joey -> a wrong-pid kproc extinction).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use core::sync::atomic::AtomicU32;
use core::time::Duration;
use libthyla_rs::fs::File;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::{t_exits, t_pread, t_putstr, t_wait_pid_for};

// Kill + reap the child, print the failure reason, exit 1 (gates the boot).
fn die(child: &Child, msg: &str) -> ! {
    t_putstr(msg);
    let _ = child.kill();
    let mut st: i32 = 0;
    unsafe {
        t_wait_pid_for(child.pid(), 0, &mut st as *mut i32);
    }
    unsafe { t_exits(1) }
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("stack-probe: starting (8b settled-thread kstack inspect)\n");

    // Root-anchored + Piped stdio (stack-probe is spawned fd-less; stack-child
    // prints nothing + never touches fd 0/1/2, so the pipes stay empty -> no
    // drain deadlock).
    let child = match Command::new("/stack-child")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            t_putstr(&format!(
                "stack-probe: FAIL -- spawn stack-child (errno {})\n",
                e.as_errno()
            ));
            unsafe { t_exits(1) }
        }
    };
    let pid = child.pid();

    // Open the child's kstack file. OWNER-authorized (same principal_id) -- the 8b
    // inspect needs I-39 authorization ONLY, NO attach/stop.
    let kstack_f = match File::open(&format!("/proc/{}/kstack", pid)) {
        Ok(f) => f,
        Err(_) => die(&child, "stack-probe: FAIL -- open kstack\n"),
    };
    let fd = kstack_f.as_raw_fd() as i64;

    // Poll until the child is blocked DEEP in a syscall: a settled (non-<running>),
    // SYMBOLIZED kernel backtrace that contains the `sleep` primitive -- the
    // airtight proof that the child is parked in sleep() (torpor_wait -> tsleep ->
    // sleep), not merely descheduled. This also excludes the freshly-spawned
    // NEVER-RUN state, which is settled (off-cpu, never dispatched) but shows a
    // SINGLE frame at its entry (ctx.lr == thread_trampoline, the initial value
    // cpu_switch_context has not yet overwritten with a mid-kernel return addr);
    // only after the child runs + enters torpor_wait does its saved ctx.lr point
    // into sched and the fp-chain unwind sched -> sleep -> tsleep -> torpor -> the
    // SVC dispatch -> the exception entry. To hand the child CPU time we BLOCK
    // ourselves on a short never-woken timed wait (a real deschedule -- harder than
    // t_yield), then re-read. Bounded so a broken child fails the boot (never hangs).
    let park = AtomicU32::new(0);
    let mut buf = [0u8; 2048];
    let mut n: usize = 0;
    let mut settled = false;
    for _ in 0..2000 {
        // Relinquish the CPU for ~2 ms so the child runs + reaches its torpor block.
        let _ = libthyla_rs::torpor::wait(&park, 0, Some(Duration::from_millis(2)));
        let r = unsafe { t_pread(fd, buf.as_mut_ptr(), buf.len(), 0i64) };
        if r > 0 {
            let rn = r as usize;
            let s = &buf[..rn];
            if !contains(s, b"<running>") && contains(s, b"+0x") && contains(s, b"sleep") {
                n = rn;
                settled = true;
                break;
            }
        }
    }
    if !settled {
        die(
            &child,
            "stack-probe: FAIL -- child never blocked in sleep() (no settled symbolized sleep-frame stack)\n",
        );
    }

    // Log the blocked kernel backtrace (ground truth: the sleep -> rendez path).
    // The bytes are kernel-formatted ASCII (safe to view as a str).
    t_putstr("stack-probe: /proc/<pid>/kstack of a torpor-blocked child (NO debug-stop):\n");
    if let Ok(txt) = core::str::from_utf8(&buf[..n]) {
        t_putstr(txt);
    }

    // Kill + reap the child. It is blocked forever in torpor_wait; killgrp wakes
    // the sleeper via the #811 death-interruptible path -> it unwinds + dies.
    let _ = child.kill();
    let mut st: i32 = 0;
    unsafe {
        t_wait_pid_for(child.pid(), 0, &mut st as *mut i32);
    }

    t_putstr(
        "stack-probe: PASS (blocked-in-syscall kernel stack read without a debug-stop)\n",
    );
    unsafe { t_exits(0) }
}
