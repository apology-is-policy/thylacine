// /sbin/diorama -- the synthetic Linux world (VIVARIUM V-4).
//
// A native libthyla-rs, device-less 9P server (the ptyfs / corvus precedent --
// it owns no hardware, so it is NOT warden-bound). Two modes, one server:
//   * boot: joey spawns it with T_SPAWN_PERM_MAY_POST_SERVICE; it posts
//     /srv/diorama and accepts connections;
//   * --vivarium <runner-pid> (V-7): the runner spawned it with the server
//     ends of a private pipe pair as fds 0/1; it serves that ONE connection
//     until EOF and posts nothing (no privilege needed, no name to collide on).
// Either way it serves a read-only Linux-shaped /proc built ENTIRELY from
// natively-reachable sources.
//
// See src/server.rs for the two things that matter most: the section 6.2 rule
// (reformatter, never authority) and what `self` means (the connection's peer,
// i.e. the MOUNTER -- so the diorama belongs in a per-container territory, which
// is exactly what a vivarium sets up).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

mod server;

use libthyla_rs::{
    t_close, t_getpid, t_note_mask, t_poll, t_putstr, t_srv_accept, TPollFd,
    T_NOTE_BIT_INTERRUPT, T_NOTE_BIT_TTY, T_POLLHUP, T_POLLIN,
};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // V-7: `--vivarium <runner-pid>` selects the per-container mode -- serve
    // the runner's pipe pair on fds 0/1 and answer pid enumeration/existence
    // only for the container's process tree (docs/VIVARIUM.md section 7.2; the
    // mode rationale is the server.rs vivarium section). A malformed pid is a
    // hard failure, not a silent fall-back to the unfiltered boot mode: falling
    // back would serve the HOST view to a container.
    let mut vivarium = false;
    {
        let mut it = libthyla_rs::env::args();
        let _ = it.next(); // argv[0]
        while let Some(a) = it.next() {
            if a == b"--vivarium" {
                let runner = it
                    .next()
                    .and_then(|v| core::str::from_utf8(v).ok())
                    .and_then(|v| v.parse::<u32>().ok())
                    .unwrap_or(0);
                if runner == 0 {
                    t_putstr("diorama: bad --vivarium pid\n");
                    return 1;
                }
                server::set_vivarium(runner, unsafe { t_getpid() } as u32);
                vivarium = true;
            }
        }
    }

    // A server does not die of a keystroke. In vivarium mode this Proc sits in
    // the terminal's foreground pgrp with the container it serves, so the pts's
    // `interrupt` (^C) and the tty family (^Z, ^\, hangup) reach it too, and a
    // native Proc with no handler DIES of the terminate-class ones -- taking
    // the container's /proc with it. Its lifetime is its channel's: it exits
    // on EOF when the last client is gone (or when the runner kills it), never
    // on what the user typed at the shell. Mask both families; the mask starts
    // at zero on a spawn, so this is ours alone. (The boot diorama never
    // receives either -- the console's ^C goes to the console owner, LS-5 --
    // and masking is harmless there.)
    let _ = unsafe {
        t_note_mask((1u64 << T_NOTE_BIT_INTERRUPT) | (1u64 << T_NOTE_BIT_TTY),
                    core::ptr::null_mut())
    };

    // Prove the tree walk + the bounded renderer + the parser before serving --
    // deterministic and mount-independent (the ptyfs selftest-before-post
    // pattern). A failure gates the boot rather than surfacing later as a
    // mystery inside a Linux binary.
    match server::selftest() {
        Ok(()) => {
            t_putstr("diorama: selftest PASS\n");
        }
        Err(stage) => {
            t_putstr("diorama: selftest FAIL: ");
            t_putstr(stage);
            t_putstr("\n");
            return 1;
        }
    }

    if vivarium {
        // The runner that handed us fds 0/1 must be our parent -- the one Proc
        // that could have made the pair. A mismatch is a wiring error, and it
        // fails hard for the same reason a bad pid does: never serve a view
        // whose scope premise is false.
        if !server::viv_check_parent() {
            t_putstr("diorama: --vivarium pid is not my parent\n");
            return 1;
        }
        // One connection, pre-established, no listener: requests on fd 0,
        // replies on fd 1. EOF (the runner's attach torn down -- the last
        // holder of the client ends is gone) ends the serve and the process;
        // the runner's ctl-kill at container exit is the belt to this brace.
        let mut conn = server::Conn::over(0, 1);
        loop {
            let mut pf = [TPollFd { fd: 0, events: T_POLLIN, revents: 0 }];
            let rc = unsafe { t_poll(pf.as_mut_ptr(), 1, 1000) };
            if rc < 0 {
                // A poll that cannot watch our only fd cannot be waited on --
                // exit rather than spin.
                return 1;
            }
            if rc == 0 {
                continue;
            }
            if pf[0].revents & (T_POLLIN | T_POLLHUP) != 0 {
                if !conn.service() {
                    return 0;
                }
            } else if pf[0].revents != 0 {
                return 1; // POLLERR/POLLNVAL on the request end
            }
        }
    }

    let listener = match server::post_srv_diorama() {
        Ok(l) => l,
        Err(()) => {
            t_putstr("diorama: post /srv/");
            t_putstr(server::srv_name());
            t_putstr(" FAILED (already posted?)\n");
            return 1;
        }
    };
    // No "serving" banner: joey's bounded liveness connect confirms /srv/diorama
    // is up (and, since the selftest runs first, that it passed). Keeping startup
    // output to the one selftest line stops it interleaving with joey's
    // concurrent boot output -- the ptyfs discipline.

    let mut conns: Vec<server::Conn> = Vec::new();

    loop {
        // Poll the live connections, AND the listener only while there is room
        // to accept. A full table with a pending connection would otherwise keep
        // the listener perpetually readable, so the accept is skipped and the
        // loop busy-spins at full CPU (the PTY-2e audit F4 finding). Dropping the
        // listener from the set while full parks the loop instead; the pending
        // client waits -- bounded acceptance, not a spin.
        let nc = conns.len().min(server::MAX_CONNS);
        let has_room = conns.len() < server::MAX_CONNS;
        let mut pollfds: Vec<TPollFd> = Vec::with_capacity(1 + nc);
        if has_room {
            pollfds.push(TPollFd { fd: listener as i32, events: T_POLLIN, revents: 0 });
        }
        let listener_slot = has_room as usize; // 1 if the listener is at [0], else 0
        for c in conns.iter().take(nc) {
            pollfds.push(TPollFd { fd: c.handle() as i32, events: T_POLLIN, revents: 0 });
        }
        let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), 1000) };
        if rc <= 0 {
            continue;
        }

        // Accept. Only reachable with room.
        if has_room && pollfds[0].revents & T_POLLIN != 0 {
            let h = unsafe { t_srv_accept(listener) };
            if h >= 0 {
                conns.push(server::Conn::new(h));
            }
        }

        // Service the readable connections (backward, remove-safe). A connection
        // accepted just above sits past `nc` and is polled next iteration.
        let mut i = nc;
        while i > 0 {
            i -= 1;
            let pf = pollfds[listener_slot + i];
            if pf.revents & (T_POLLIN | T_POLLHUP) != 0 && !conns[i].service() {
                let _ = unsafe { t_close(conns[i].handle()) };
                conns.remove(i);
            }
        }
    }
}
