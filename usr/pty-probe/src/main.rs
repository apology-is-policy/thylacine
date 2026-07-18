// /bin/pty-probe -- the PTY-2e openpty round-trip E2E (boot-fatal via joey).
//
// Two roles in one binary (argv-dispatched):
//   (no args)    the EMULATOR: open /dev/pts/ptmx (the clone mint), decode N
//                via the fstat qid (the documented ptsname contract), spawn
//                itself as the session role, then drive the signal seam end to
//                end: Ctrl-C -> interrupt, winsize -> tty:winch, master close
//                -> tty:hup.
//   session <n>  the SESSION: t_setsid -> open the slave -> acquire the
//                controlling terminal -> open the notes fd (self-managing, so
//                the incoming interrupt queues to the fd instead of
//                default-terminating) -> observe interrupt / tty:winch /
//                tty:hup, acking each over the slave.
//
// The FIRST live exercise of: the DEFERRED master read (the readiness read
// parks server-side until the child's slave opens and writes -- the
// slave_opened_once latch keeps it a park, never a spurious EOF), the
// pts -> ct_sid -> fg_pgid signal routing against a REAL controlling session
// (ptyfs names only (pts_id, class); the kernel resolves the group -- the
// I-1/I-22 bound observed from the receiving end), and the HUP delivery.
// The data-path legs ride joey's 2a-2 probe.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;
use libthyla_rs::notes::Notes;
use libthyla_rs::{
    env, t_close, t_fstat, t_open, t_putstr, t_read, t_setsid, t_spawn_full_argv, t_tty_acquire,
    t_wait_pid_for, t_write, TSpawnArgs, T_ORDWR, T_WALK_OPEN_FROM_ROOT,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// The ptyfs endpoint-qid contract (PTY-DESIGN section 5): PTS_FLAG | N<<8 | kind.
const PTS_FLAG: u64 = 1 << 40;

fn open_rdwr(path: &str) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_ORDWR) }
}

fn wr(fd: i64, b: &[u8]) -> bool {
    unsafe { t_write(fd, b.as_ptr(), b.len()) == b.len() as i64 }
}

/// Read exactly one ack byte (blocks; the deferred-read path when empty).
fn rd1(fd: i64) -> Option<u8> {
    let mut b = [0u8; 8];
    let n = unsafe { t_read(fd, b.as_mut_ptr(), b.len()) };
    if n == 1 {
        Some(b[0])
    } else {
        None
    }
}

fn emulator() -> i64 {
    let mfd = open_rdwr("/dev/pts/ptmx");
    if mfd < 0 {
        t_putstr("pty-probe: open(/dev/pts/ptmx) FAILED\n");
        return 2;
    }
    // ptsname: t_stat.qid_path (byte offset 8) carries the 9P qid verbatim.
    let mut st = [0u8; 80];
    if unsafe { t_fstat(mfd, st.as_mut_ptr()) } != 0 {
        t_putstr("pty-probe: fstat(master) FAILED\n");
        return 3;
    }
    let mut qb = [0u8; 8];
    qb.copy_from_slice(&st[8..16]);
    let qp = u64::from_le_bytes(qb);
    if qp & PTS_FLAG == 0 {
        t_putstr("pty-probe: master qid is not a pts endpoint\n");
        return 4;
    }
    let n = (qp >> 8) & 0xff_ffff;

    // The raw fd-less spawn: the prover itself was spawned fd-less (joey's
    // plain t_spawn), so Command's Stdio::Inherit (pass the parent's fd 0/1/2)
    // has nothing to pass -- and the session role needs no fds anyway
    // (t_putstr is console-syscall-backed; it opens its own pts fds).
    let name = b"/bin/pty-probe";
    let n_s = format!("{}", n);
    let mut argv: Vec<u8> = Vec::new();
    argv.extend_from_slice(name);
    argv.push(0);
    argv.extend_from_slice(b"session");
    argv.push(0);
    argv.extend_from_slice(n_s.as_bytes());
    argv.push(0);
    let req = TSpawnArgs {
        name_va: name.as_ptr() as u64,
        argv_data_va: argv.as_ptr() as u64,
        fd_list_va: 0,
        name_len: name.len() as u32,
        argv_data_len: argv.len() as u32,
        argc: 3,
        fd_count: 0,
        perm_flags: 0,
        _pad_envp: 0,
        cap_mask: 0,
        principal_id: 0,
        primary_gid: 0,
        supp_gids_va: 0,
        supp_gid_count: 0,
        identity_flags: 0,
        allowance_va: 0,
        allowance_flags: 0,
        _pad_allow: 0,
    };
    let child_pid = unsafe { t_spawn_full_argv(&req) };
    if child_pid <= 0 {
        t_putstr("pty-probe: spawn(session role) FAILED\n");
        return 5;
    }

    // (1) The readiness read PARKS -- the child's slave is not open yet (the
    //     slave_opened_once latch keeps this WouldBlock server-side, never a
    //     spurious EOF) -- and completes when the child writes "R".
    if rd1(mfd) != Some(b'R') {
        t_putstr("pty-probe: readiness (parked master read) FAILED\n");
        return 6;
    }
    // (2) The keystroke: Ctrl-C through the cooked master. ISIG consumes it
    //     (never a byte, never an echo) and ptyfs raises INT; the kernel
    //     routes "interrupt" to the controlling session's fg pgrp = the child.
    if !wr(mfd, &[0x03]) || rd1(mfd) != Some(b'I') {
        t_putstr("pty-probe: Ctrl-C -> interrupt FAILED\n");
        return 7;
    }
    // (3) The resize: a winsize ctl write -> tty:winch to the fg pgrp.
    let cfd = open_rdwr(&format!("/dev/pts/{}ctl", n));
    if cfd < 0 || !wr(cfd, b"winsize 100 40") || rd1(mfd) != Some(b'W') {
        t_putstr("pty-probe: winsize -> tty:winch FAILED\n");
        return 8;
    }
    let _ = unsafe { t_close(cfd) };
    // (4) Carrier loss: the master close is the last-master edge -> tty:hup.
    let _ = unsafe { t_close(mfd) };
    // (5) The child's verdict carries the session-side observations (its own
    //     failure prints reach the console via t_putstr).
    let mut status: i32 = -1;
    let reaped = unsafe { t_wait_pid_for(child_pid as i32, 0, &mut status) };
    if reaped == child_pid && status == 0 {
        t_putstr("pty-probe: openpty E2E PASS (park-read + INT + WINCH + HUP over a live controlling session)\n");
        0
    } else {
        t_putstr("pty-probe: session role FAILED (see its stage line above)\n");
        9
    }
}

fn session(n_str: &str) -> i64 {
    if unsafe { t_setsid() } < 0 {
        t_putstr("pty-probe(session): setsid FAILED\n");
        return 20;
    }
    let sfd = open_rdwr(&format!("/dev/pts/{}", n_str));
    if sfd < 0 {
        t_putstr("pty-probe(session): open(slave) FAILED\n");
        return 21;
    }
    if unsafe { t_tty_acquire(sfd) } < 0 {
        t_putstr("pty-probe(session): tty_acquire FAILED\n");
        return 22;
    }
    // The notes fd FIRST (self-managing: the incoming interrupt must queue to
    // the fd, not default-terminate) -- only then signal readiness.
    let notes = match Notes::open_self() {
        Ok(nfd) => nfd,
        Err(_) => {
            t_putstr("pty-probe(session): notes open FAILED\n");
            return 23;
        }
    };
    if !wr(sfd, b"R") {
        t_putstr("pty-probe(session): readiness write FAILED\n");
        return 24;
    }
    match notes.read() {
        Ok(nt) if nt.name == "interrupt" => {}
        _ => {
            t_putstr("pty-probe(session): expected interrupt FAILED\n");
            return 25;
        }
    }
    if !wr(sfd, b"I") {
        return 26;
    }
    match notes.read() {
        Ok(nt) if nt.name == "tty:winch" => {}
        _ => {
            t_putstr("pty-probe(session): expected tty:winch FAILED\n");
            return 27;
        }
    }
    if !wr(sfd, b"W") {
        return 28;
    }
    match notes.read() {
        Ok(nt) if nt.name == "tty:hup" => {}
        _ => {
            t_putstr("pty-probe(session): expected tty:hup FAILED\n");
            return 29;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut args = env::args();
    let _argv0 = args.next();
    match args.next() {
        Some(role) if role == b"session" => match args.next() {
            Some(nb) => match core::str::from_utf8(nb) {
                Ok(n) => session(n),
                Err(_) => 31,
            },
            None => 30,
        },
        _ => emulator(),
    }
}
