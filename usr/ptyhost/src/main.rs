// /bin/ptyhost -- the PTY-4 native session host (thematic candidate: `den`,
// held per PTY-DESIGN section 10).
//
// The first real consumer of the pty master side: mint a pts, host a program
// (default `/bin/ut`) on the slave as its fd 0/1/2, and pump bytes between
// the OUTER terminal (this process's own fd 0/1 -- the console when run from
// a console `ut`, or an outer pts when nested) and the master. This is the
// mux's core mechanism -- mint / host / pump / relay is exactly what each
// tmux window does -- with one window and no UI (PTY-DESIGN section 14).
//
// The mint/seed/host core now lives in `ptyhold` (shared with the kaua-term,
// KT-1); ptyhost keeps only its RELAY policy -- the two raw byte pumps below.
//
// The composition that keeps this trivial (section 14.2): `ptyhost` is
// registered in ut's `is_raw_command`, so the OUTER shell's LS-7 dance flips
// the outer console RAW (-isig) around it and restores it after. The outer
// terminal collapses to a byte pipe -- `^C`/`^Z` arrive here as raw
// 0x03/0x1a bytes, are pumped inward, and the PTS is the one line
// discipline that cooks them into the INNER session's fg-pgrp signals. The
// host itself never touches consctl (I-27) and installs no discipline of
// its own; the hosted shell's session dance (`ut: init_pts_session`) does
// the setsid/acquire/set_fg on ITS side of the slave.
//
// Topology + teardown: the hosted shell exits -> its slave fds close
// (#68/#926 close-at-exit) -> the master read drains-then-EOFs (PTY-2d) ->
// the main pump ends -> reap + exit; process exit closes the master (the
// last-master edge -> tty:hup to any survivor in the inner session, whom
// the kernel orphan rule also covers). If instead the OUTER stdin EOFs
// (the outer session died), the input pump ends the whole host
// (`t_exit_group`) and the same master-close teardown follows.
//
// Two blocking pump threads, no poll: the pty DATA endpoints (master + slave)
// are non-QTPOLL (reads park server-side), so the natural shape here is one
// thread per direction. The main thread runs master->fd1 (it ends
// deterministically at the child's exit); the spawned thread runs fd0->master
// and is unwound by the process-exit group cascade (#809) when the main thread
// finishes. (item 10 added a SEPARATE per-pts /dev/pts/<n>ready QTPOLL file for
// a native SLAVE poller (ut) to block on; the master + the data files stay
// non-QTPOLL, so ptyhost's blocking-pump model is unchanged.)

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use libthyla_rs::{
    env, t_burrow_attach, t_close, t_exit_group, t_putstr, t_read, t_thread_exit, t_wait_pid_for,
    t_write, thread,
};
use ptyhold::{HoldError, Master};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// The input-pump thread's stack (a fixed read/write loop; generous).
const PUMP_STACK: u64 = 64 * 1024;

/// The input pump: OUTER stdin -> the master, byte-for-byte. Runs on its own
/// thread (`arg` = the master fd). Ends the WHOLE host on outer-stdin EOF
/// (the outer session died -- nothing left to host for); exits just itself
/// if the master write fails (the main thread is already tearing down).
extern "C" fn pump_in(arg: u64) {
    let mfd = arg as i64;
    let mut buf = [0u8; 512];
    loop {
        // SAFETY: SVC wrappers over this thread's own stack buffer.
        let n = unsafe { t_read(0, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            break;
        }
        let mut off = 0usize;
        let mut zeros = 0u32;
        while off < n as usize {
            let w = unsafe { t_write(mfd, buf.as_ptr().add(off), n as usize - off) };
            if w < 0 {
                // Master unusable: the main thread owns the teardown.
                // SAFETY: `!`-returning SVC.
                unsafe { t_thread_exit() }
            }
            if w == 0 {
                // A raw-mode master accepts what fits and answers 0 when its
                // ring is full: back-pressure, not an error. Wait it out
                // briefly; a ring full past the bound drops the rest.
                zeros += 1;
                if zeros > 200 {
                    break;
                }
                let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(1));
                continue;
            }
            zeros = 0;
            off += w as usize;
        }
    }
    // Outer stdin ended: end the host (the group cascade unwinds the main
    // thread's parked master read; process exit closes the master).
    // SAFETY: `!`-returning SVC.
    unsafe { t_exit_group(0) }
}

fn run() -> i64 {
    // argv: `ptyhost [prog [args...]]`; default the session shell.
    let mut args = env::args();
    let _argv0 = args.next();
    let mut argv: Vec<String> = Vec::new();
    for a in args {
        match core::str::from_utf8(a) {
            Ok(s) => argv.push(String::from(s)),
            Err(_) => {
                t_putstr("ptyhost: non-utf8 argument\n");
                return 2;
            }
        }
    }
    if argv.is_empty() {
        argv.push(String::from("/bin/ut"));
    }

    // 1. Mint: the returned master fd IS this host's endpoint (ptyhold owns
    //    the clone-open + qid validation; its failures close the fd for us).
    let master = match Master::mint() {
        Ok(m) => m,
        Err(HoldError::Mint) => {
            t_putstr("ptyhost: open(/dev/pts/ptmx) failed\n");
            return 2;
        }
        Err(HoldError::Fstat) => {
            t_putstr("ptyhost: fstat(master) failed\n");
            return 2;
        }
        Err(HoldError::NotMaster) => {
            t_putstr("ptyhost: ptmx qid is not a pts master\n");
            return 2;
        }
        Err(_) => return 2,
    };
    let mfd = master.mfd;

    // 2. Seed the winsize (the host has no queryable outer size at v1.0 --
    //    the kernel console reports none -- so the classic 80x24; a resize
    //    surface is the graphical-terminal consumer's, Halcyon).
    master.seed_winsize(80, 24);

    // 3+4. Host the program on the slave. The child runs its own session dance
    //      (ut detects fd-0-is-a-pts and does setsid/acquire/set_fg). On any
    //      failure past mint we own the master fd, so we close it.
    let child = match master.spawn_on_slave(&argv) {
        Ok(c) => c,
        Err(HoldError::OpenSlave) => {
            t_putstr("ptyhost: open(slave) failed\n");
            let _ = unsafe { t_close(mfd) };
            return 2;
        }
        Err(HoldError::Spawn) => {
            t_putstr("ptyhost: spawn failed\n");
            let _ = unsafe { t_close(mfd) };
            return 2;
        }
        Err(_) => {
            let _ = unsafe { t_close(mfd) };
            return 2;
        }
    };
    let pid = child.pid();

    // 5. The input pump (outer fd 0 -> master) on its own thread. A spawn
    //    failure degrades to output-only (the hosted program still runs and
    //    renders; it just cannot be typed at) -- reported, not fatal.
    let stack = unsafe { t_burrow_attach(PUMP_STACK) };
    if stack < 0
        || unsafe {
            thread::spawn_raw(
                pump_in as *const () as u64,
                stack as u64 + PUMP_STACK,
                mfd as u64,
                0,
            )
        }
        .is_err()
    {
        t_putstr("ptyhost: input pump spawn failed (output-only)\n");
    }

    // 6. The output pump (master -> outer fd 1) on THIS thread: it ends
    //    deterministically when the hosted program exits (drain-then-EOF).
    let mut buf = [0u8; 512];
    loop {
        // SAFETY: SVC wrappers over this thread's own stack buffer.
        let rn = unsafe { t_read(mfd, buf.as_mut_ptr(), buf.len()) };
        if rn <= 0 {
            break;
        }
        let mut off = 0usize;
        while off < rn as usize {
            let w = unsafe { t_write(1, buf.as_ptr().add(off), rn as usize - off) };
            if w <= 0 {
                break;
            }
            off += w as usize;
        }
    }

    // 7. Reap + propagate; process exit closes the master (tty:hup inward)
    //    and unwinds the input-pump thread (the #809 group cascade).
    let mut status: i32 = 0;
    // SAFETY: SVC wrapper; &mut status is a valid writable i32.
    let reaped = unsafe { t_wait_pid_for(pid, 0, &mut status as *mut i32) };
    let _ = unsafe { t_close(mfd) };
    if reaped == pid as i64 {
        status as i64
    } else {
        2
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
