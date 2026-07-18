// /sbin/ptyfs -- the pseudoterminal 9P server (PTY-2, I-20).
//
// A native libthyla-rs, device-less /srv server (the corvus precedent -- NOT the
// warden, which is hardware-device-bind driven; ptyfs owns no hardware). joey
// spawns it directly with T_SPAWN_PERM_MAY_POST_SERVICE and mounts its tree under
// /dev (/dev/ptmx + /dev/pts). It owns the pts pairs + their byte rings + (PTY-2b)
// the per-pts line discipline; the KERNEL owns the session/pgrp/controlling-
// terminal state (the PTY-1 kernel arc). See docs/reference/135-pty-kernel.md +
// the ptyfs reference doc (PTY-2e).
//
// PTY-2a: the server skeleton + the master/slave ring data path + the pts
// registration (SYS_PTY_REGISTER) + an in-server selftest. PTY-2b: the per-pts
// line discipline (the de-globalized LS-8 cooking -- ICRNL/ISIG/ICANON on input,
// ONLCR on output, the echo() no-leak chokepoint; a fresh pts is FULL COOKED).
// Per-pts termios ctl/winsize is PTY-2c; teardown/SIGHUP PTY-2d.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

mod server;

use libthyla_rs::{t_close, t_poll, t_putstr, t_srv_accept, TPollFd, T_POLLHUP, T_POLLIN};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Prove the ring / RecvOutcome / ldisc logic before serving -- deterministic
    // + mount-independent (the netd echo_e2e analog). A failure gates the boot.
    match server::selftest() {
        Ok(()) => {
            t_putstr("ptyfs: 2a+2b selftest PASS\n");
        }
        Err(stage) => {
            t_putstr("ptyfs: 2a+2b selftest FAIL: ");
            t_putstr(stage);
            t_putstr("\n");
            return 1;
        }
    }

    let listener = match server::post_srv_ptyfs() {
        Ok(l) => l,
        Err(()) => {
            t_putstr("ptyfs: post /srv/ptyfs FAILED\n");
            return 1;
        }
    };
    // No "serving" banner here: joey's bounded liveness connect confirms /srv/ptyfs
    // is up (and, since the selftest runs first, that it passed). Keeping ptyfs's
    // startup output to the one selftest line -- printed during joey's silent retry
    // window -- prevents it interleaving with joey's concurrent boot-complete output.

    let mut ptys = server::Ptys::new();
    let mut conns: Vec<server::Conn> = Vec::new();

    loop {
        // 1. Deliver held reads whose ring filled in a prior serviced frame. I-9:
        //    ptyfs is single-threaded and a ring fills ONLY via a client Twrite (a
        //    POLLIN that woke this loop), so a parked read is always re-checked
        //    here before the loop parks again -- no read wake is lost.
        let mut i = conns.len();
        while i > 0 {
            i -= 1;
            if !conns[i].poll_reads(&mut ptys) {
                conns[i].teardown(&mut ptys);
                let _ = unsafe { t_close(conns[i].handle()) };
                conns.remove(i);
            }
        }

        // 2. Poll [listener] + the live connections.
        let nc = conns.len().min(server::MAX_CONNS);
        let mut pollfds: Vec<TPollFd> = Vec::with_capacity(1 + nc);
        pollfds.push(TPollFd {
            fd: listener as i32,
            events: T_POLLIN,
            revents: 0,
        });
        for c in conns.iter().take(nc) {
            pollfds.push(TPollFd {
                fd: c.handle() as i32,
                events: T_POLLIN,
                revents: 0,
            });
        }
        let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), 1000) };
        if rc <= 0 {
            continue;
        }

        // 3. Accept a new connection (joey's /dev mount drives one; headroom for
        //    a future direct open=connect consumer).
        if pollfds[0].revents & T_POLLIN != 0 && conns.len() < server::MAX_CONNS {
            let h = unsafe { t_srv_accept(listener) };
            if h >= 0 {
                conns.push(server::Conn::new(h));
            }
        }

        // 4. Service the readable connections (backward, remove-safe). A conn
        //    accepted in step 3 sits past `nc` and is polled next iteration.
        let mut i = nc;
        while i > 0 {
            i -= 1;
            let pf = pollfds[1 + i];
            if pf.revents & (T_POLLIN | T_POLLHUP) != 0 && !conns[i].service(&mut ptys) {
                conns[i].teardown(&mut ptys);
                let _ = unsafe { t_close(conns[i].handle()) };
                conns.remove(i);
            }
        }
    }
}
