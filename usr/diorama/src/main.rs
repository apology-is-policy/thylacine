// /sbin/diorama -- the synthetic Linux world (VIVARIUM V-4).
//
// A native libthyla-rs, device-less /srv server (the ptyfs / corvus precedent --
// it owns no hardware, so it is NOT warden-bound). joey spawns it with
// T_SPAWN_PERM_MAY_POST_SERVICE; it posts /srv/diorama and serves a read-only
// Linux-shaped /proc built ENTIRELY from natively-reachable sources.
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
    t_close, t_getpid, t_poll, t_putstr, t_srv_accept, TPollFd, T_POLLHUP, T_POLLIN,
};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // V-7: `--vivarium <runner-pid>` selects the per-container mode -- post
    // /srv/viv-dio and answer pid enumeration/existence only for the
    // container's process tree (docs/VIVARIUM.md section 7.2; the mode
    // rationale is the server.rs header note). A malformed pid is a hard
    // failure, not a silent fall-back to the unfiltered boot mode: falling
    // back would serve the HOST view to a container.
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
            }
        }
    }

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

    let listener = match server::post_srv_diorama() {
        Ok(l) => l,
        Err(()) => {
            t_putstr("diorama: post /srv/diorama FAILED\n");
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
