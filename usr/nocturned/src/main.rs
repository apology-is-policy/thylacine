// /nocturned -- the Nocturne audio daemon, N-1: the virtio-snd driver + the
// Plan 9 audio(3) file (docs/NOCTURNE.md section 8, N-1).
//
// Warden-bound (`virtio-pci:25`, persistent): probe brings the playback stream
// up over the modern-PCI transport (snd.rs); serve posts /srv/nocturne (joey
// mounts it at /dev/nocturne) and runs ONE poll loop over the listener, the 9P
// connections and the device IRQ. The tree at N-1 is the heritage floor --
// `audio` (write S16LE stereo 48 kHz to play; Plan 9's /dev/audio shape),
// `info` (the audiostat words + the driver counters), `ctl` -- so `bind
// /dev/nocturne/audio /dev/audio` gives any namespace a 9front-shaped device.
// The graph, the rings, voices and descants are N-2+.
//
// Diagnostics go to the console (t_putstr); stdout carries exactly the one
// READY line the warden's readiness contract requires.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

mod server;
mod snd;

use alloc::vec::Vec;

use libdriver::driver::{run, Driver, DriverVa};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::io::Write;
use libthyla_rs::{t_close, t_poll, t_srv_accept, TPollFd, T_POLLHUP, T_POLLIN};

use server::{Conn, Shared, MAX_CONNS};
use snd::VirtioSnd;

/// Silence periods (~10.7 ms each) after the last real one before the stream is
/// STOPped: the idle cost of a running stream is one IRQ per period, so an idle
/// box must not pay it forever. ~0.5 s covers a writer's inter-chunk gap.
const IDLE_STOP_PERIODS: u64 = 48;

struct Nocturned {
    snd: VirtioSnd,
}

impl Driver for Nocturned {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        say!(
            "nocturned: grant compat={} pci={:?} irq={} dma={:#x}",
            res.compatible,
            res.pci,
            res.irq.len(),
            res.dma_max
        );
        let mut va = DriverVa::new();
        let snd = VirtioSnd::open(res, &mut va)?;
        Ok(Nocturned { snd })
    }

    fn serve(mut self, _res: &BoundResources) -> Result<(), Error> {
        let listener = match server::post_srv_nocturne() {
            Ok(l) => l,
            Err(()) => {
                say!("nocturned: /srv/nocturne post failed");
                return Err(Error::Hardware);
            }
        };
        say!(
            "nocturned: serving /srv/nocturne (virtio-snd playback; s16c2r{} period {} B x {})",
            snd::RATE_HZ,
            snd::PERIOD_BYTES,
            snd::PERIODS
        );
        // READY last: all bring-up console output precedes it; the warden's
        // readiness read wakes on this one line.
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        let mut shared = Shared::new();
        let mut conns: Vec<Conn> = Vec::new();
        let irq_fd = self.snd.irq_fd();
        let mut idle_periods: u64 = 0;

        loop {
            // 1. The device: reap completions (each one is a period), refilling
            //    every slot from the FIFO (silence when it is empty). A stream
            //    that has played only silence for IDLE_STOP_PERIODS stops.
            if self.snd.started() {
                let before = self.snd.stats;
                let reaped = self.snd.pump(|buf| shared.next_period(buf));
                let real = (self.snd.stats.periods_played - before.periods_played)
                    - (self.snd.stats.silence_periods - before.silence_periods);
                if reaped > 0 {
                    if real > 0 {
                        idle_periods = 0;
                    } else {
                        idle_periods += reaped as u64;
                    }
                }
                if idle_periods >= IDLE_STOP_PERIODS && shared.fifo_len() == 0 {
                    self.snd.stop();
                    idle_periods = 0;
                }
            } else if shared.fifo_len() > 0 {
                if let Err(e) = self.snd.start(|buf| shared.next_period(buf)) {
                    say!("nocturned: stream start failed: {:?}", e);
                    shared.drop_fifo();
                }
                idle_periods = 0;
            }
            shared.stats = self.snd.stats;
            shared.started = self.snd.started();

            // 2. Writers parked on a full FIFO: the reap above freed room.
            let mut i = conns.len();
            while i > 0 {
                i -= 1;
                if !conns[i].poll_writes(&mut shared) {
                    conns[i].teardown(&mut shared);
                    let _ = unsafe { t_close(conns[i].handle()) };
                    conns.remove(i);
                }
            }

            // 3. Poll: the listener (only with room, the ptyfs F4 lesson), the
            //    connections, and the IRQ (readable at pending-count >= 1).
            let nc = conns.len().min(MAX_CONNS);
            let has_room = conns.len() < MAX_CONNS;
            let mut pollfds: Vec<TPollFd> = Vec::with_capacity(2 + nc);
            if has_room {
                pollfds.push(TPollFd {
                    fd: listener as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            let conn_base = pollfds.len();
            for c in conns.iter().take(nc) {
                pollfds.push(TPollFd {
                    fd: c.handle() as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            let irq_slot = pollfds.len();
            pollfds.push(TPollFd {
                fd: irq_fd,
                events: T_POLLIN,
                revents: 0,
            });
            let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), 1000) };
            if rc < 0 {
                continue;
            }

            // 4. The IRQ: consume the pending count (non-blocking now) -- the
            //    reap itself runs at the top of the next iteration.
            if pollfds[irq_slot].revents & T_POLLIN != 0 {
                let _ = self.snd.irq_wait();
            }

            // 5. Accept.
            if has_room && pollfds[0].revents & T_POLLIN != 0 {
                let h = unsafe { t_srv_accept(listener) };
                if h >= 0 {
                    conns.push(Conn::new(h));
                }
            }

            // 6. Service the readable connections (backward, remove-safe).
            let mut i = nc;
            while i > 0 {
                i -= 1;
                let pf = pollfds[conn_base + i];
                if pf.revents & (T_POLLIN | T_POLLHUP) != 0 && !conns[i].service(&mut shared) {
                    conns[i].teardown(&mut shared);
                    let _ = unsafe { t_close(conns[i].handle()) };
                    conns.remove(i);
                }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<Nocturned>()
}
