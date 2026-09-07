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

use core::time::Duration;

use alloc::boxed::Box;
use alloc::vec::Vec;

use libdriver::driver::{run, Driver, DriverVa};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::io::Write;
use libthyla_rs::thread;
use libthyla_rs::{
    t_burrow_attach, t_close, t_poll, t_srv_accept, TPollFd, T_POLLHUP, T_POLLIN,
};

use server::{Conn, Shared, MAX_CONNS};
use snd::VirtioSnd;

/// Silence periods (~10.7 ms each) after the last real one before the stream is
/// STOPped: the idle cost of a running stream is one IRQ per period, so an idle
/// box must not pay it forever. ~0.5 s covers a writer's inter-chunk gap.
const IDLE_STOP_PERIODS: u64 = 48;

/// The backstop timeout (ms), used two ways since the N-2c split. In the CYCLE
/// thread it bounds how long a STOPPED stream parks before re-checking for work;
/// a byte write wakes it sooner via the control thread's same-Proc poke, but a
/// ring producer in ANOTHER Proc cannot poke (shared memory, no fd, no
/// cross-Proc torpor -- N-2b-2b), so this is that ring's start latency. In the
/// running cycle it is a device-IRQ backstop. In the CONTROL thread it is the
/// idle poll interval when no write is parked.
const IDLE_POLL_MS: i32 = 100;

/// The CONTROL thread's poll timeout (ms) while any connection has a parked
/// write: the cycle frees FIFO room by draining (~one period, 10.7 ms), and the
/// control thread has no fd event for that (shared memory), so it re-checks at
/// about a period so a parked write completes promptly. A client only parks when
/// it over-produces (its 64 KiB FIFO full, ~340 ms buffered), so this granularity
/// is invisible.
const PARKED_RETRY_MS: i32 = 10;

/// The cycle thread's stack: a fresh page-aligned anon burrow (so its top is
/// 16-aligned for spawn_raw). Generous for the mix path's on-stack period +
/// float32 buffers (~8 KiB) plus frames.
const CYCLE_STACK: u64 = 128 * 1024;

struct Nocturned {
    snd: VirtioSnd,
}

/// The cycle thread's context, leaked to 'static and handed to `cycle_entry`
/// through spawn_raw's one u64 arg. It owns the device; `sh` is the graph both
/// threads share.
struct CycleCtx {
    snd: VirtioSnd,
    sh: &'static Shared,
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

    fn serve(self, _res: &BoundResources) -> Result<(), Error> {
        let listener = match server::post_srv_nocturne() {
            Ok(l) => l,
            Err(()) => {
                say!("nocturned: /srv/nocturne post failed");
                return Err(Error::Hardware);
            }
        };
        // N-3a-3: the sink-authority control post -- NOT mounted, connected
        // per-conn so the volume gate reads the writer as the peer.
        let ctl_listener = match server::post_srv_nocturne_ctl() {
            Ok(l) => l,
            Err(()) => {
                say!("nocturned: /srv/nocturne-ctl post failed");
                return Err(Error::Hardware);
            }
        };
        say!(
            "nocturned: serving /srv/nocturne (playback) + /srv/nocturne-ctl (sink authority); s16c2r{} period {} B x {}",
            snd::RATE_HZ,
            snd::PERIOD_BYTES,
            snd::PERIODS
        );

        // The graph both threads share, leaked to 'static so each thread holds a
        // stable reference for the life of the Proc (D-1c). Seed capture-availability
        // (fixed at driver open) so the control thread's `source` open can answer
        // immediately, no first-cycle race (N-3c-2).
        let sh: &'static Shared = Box::leak(Box::new(Shared::new(self.snd.has_capture())));

        // Spawn the CYCLE thread: it owns the device and runs the audio clock
        // (the IRQ-driven pump + float32 mix), and never touches 9P. This
        // (original) thread becomes the CONTROL thread serving /srv/nocturne.
        // They meet only at `sh`'s try-locked graph.
        let ctx: &'static mut CycleCtx = Box::leak(Box::new(CycleCtx { snd: self.snd, sh }));
        let stack = unsafe { t_burrow_attach(CYCLE_STACK) };
        if stack < 0 {
            say!("nocturned: cycle-thread stack attach failed");
            return Err(Error::Hardware);
        }
        let sp = (stack as u64) + CYCLE_STACK;
        if unsafe {
            thread::spawn_raw(
                cycle_entry as *const () as u64,
                sp,
                ctx as *mut CycleCtx as u64,
                0,
            )
        }
        .is_err()
        {
            say!("nocturned: cycle-thread spawn failed");
            return Err(Error::Hardware);
        }

        // READY last: all bring-up console output + the cycle spawn precede it;
        // the warden's readiness read wakes on this one line.
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        control_run(sh, listener, ctl_listener)
    }
}

/// The CYCLE thread entry (spawn_raw ABI: one u64 arg = the leaked CycleCtx).
extern "C" fn cycle_entry(arg: u64) -> ! {
    // SAFETY: `arg` is the address of the CycleCtx leaked in serve(); it lives
    // for the Proc's life and only this thread touches it.
    let ctx: &mut CycleCtx = unsafe { &mut *(arg as *mut CycleCtx) };
    cycle_run(&mut ctx.snd, ctx.sh)
}

/// The audio clock (D-1c). Runs one iteration per device period: try_lock the
/// graph, pump the device (reap completions + refill each freed slot with a
/// freshly-mixed period), decide start/stop, publish stats, then wait for the
/// next period IRQ. A try_lock miss (the control thread is mid-edit) replays the
/// last mixed period so the device never underruns -- "run last cycle's plan".
fn cycle_run(snd: &mut VirtioSnd, sh: &'static Shared) -> ! {
    let irq_fd = snd.irq_fd();
    let mut idle_periods: u64 = 0;
    // The last period we mixed; replayed on a try_lock miss so a brief control
    // edit never starves the device. Voices do not advance on a replay -- they
    // advance next period -- so no data is lost; at most one period repeats.
    let mut last_period = [0u8; snd::PERIOD_BYTES];
    loop {
        match sh.graph.try_lock() {
            Some(mut g) => {
                if snd.started() {
                    let before = snd.stats;
                    let reaped = snd.pump(|buf| {
                        let any = g.next_period(buf);
                        let n = buf.len().min(snd::PERIOD_BYTES);
                        last_period[..n].copy_from_slice(&buf[..n]);
                        any
                    });
                    let real = (snd.stats.periods_played - before.periods_played)
                        - (snd.stats.silence_periods - before.silence_periods);
                    if reaped > 0 {
                        if real > 0 {
                            idle_periods = 0;
                        } else {
                            idle_periods += reaped as u64;
                        }
                    }
                    if idle_periods >= IDLE_STOP_PERIODS && !g.has_playable() {
                        snd.stop();
                        idle_periods = 0;
                    }
                } else if g.has_playable() {
                    if let Err(e) = snd.start(|buf| {
                        let any = g.next_period(buf);
                        let n = buf.len().min(snd::PERIOD_BYTES);
                        last_period[..n].copy_from_slice(&buf[..n]);
                        any
                    }) {
                        say!("nocturned: stream start failed: {:?}", e);
                        g.drop_fifo();
                    }
                    idle_periods = 0;
                }
                // Capture (N-3c-2): the RX twin of playback, ON-DEMAND and
                // independent of the TX stream. `source_open` (set when an
                // authorized reader opens `source`) drives START/STOP; while
                // capturing, pump the rxq into the source mirror the reader drains.
                if snd.has_capture() {
                    if g.source_open() && !snd.capturing() {
                        snd.start_capture();
                    } else if !g.source_open() && snd.capturing() {
                        snd.stop_capture();
                    }
                    if snd.capturing() {
                        snd.pump_rx(|cap| g.source_push(cap));
                    }
                }
                // Publish device state for the control thread's `info` reader.
                g.stats = snd.stats;
                g.started = snd.started();
                // Track a mid-run capture wedge so a post-wedge `source` open gets
                // ENODEV instead of accepting-then-never-delivering (N-3c-2).
                g.capture_available = snd.has_capture();
            }
            None => {
                // Graph edit in progress. Keep the device fed by REPLAYING the
                // last mixed period; skip start/stop (they need the graph). A
                // stopped stream just parks below until the edit clears.
                if snd.started() {
                    let _ = snd.pump(|buf| {
                        let n = buf.len().min(snd::PERIOD_BYTES);
                        buf[..n].copy_from_slice(&last_period[..n]);
                        false
                    });
                }
            }
        }

        // Wait for the next wake.
        if snd.started() || snd.capturing() {
            // Running (playback OR capture): the device period IRQ, with a bounded
            // backstop against a device that stops interrupting (the pumps reap
            // opportunistically). One INTx line serves both the txq and rxq.
            let mut pfd = [TPollFd {
                fd: irq_fd,
                events: T_POLLIN,
                revents: 0,
            }];
            let _ = unsafe { t_poll(pfd.as_mut_ptr(), 1, IDLE_POLL_MS) };
            if pfd[0].revents & T_POLLIN != 0 {
                let _ = snd.irq_wait();
            }
        } else {
            // Stopped: no IRQ. Park on the control thread's poke (a byte write
            // made a voice playable) with a bounded backstop -- a cross-Proc ring
            // producer cannot poke (N-2b-2b), so the backstop starts it.
            sh.cycle_park(Duration::from_millis(IDLE_POLL_MS as u64));
        }
    }
}

/// The CONTROL thread (D-1c): the original thread after it spawns the cycle. It
/// serves /srv/nocturne -- accept, framing, dispatch, parked-write retry -- and
/// touches the graph only under `sh`'s blocking lock, never across a 9P reply.
fn control_run(sh: &'static Shared, listener: i64, ctl_listener: i64) -> ! {
    let mut conns: Vec<Conn> = Vec::new();
    loop {
        // 1. Retry parked writes: the cycle thread frees FIFO room by draining.
        let mut i = conns.len();
        while i > 0 {
            i -= 1;
            if !conns[i].poll_writes(sh) {
                conns[i].teardown(sh);
                let _ = unsafe { t_close(conns[i].handle()) };
                conns.remove(i);
            }
        }

        // 2. Poll the listener + connections (no IRQ -- that is the cycle's). A
        //    short timeout while any write is parked so it completes within a
        //    period of the cycle draining room; otherwise the idle interval.
        let any_pending = conns.iter().any(|c| c.has_pending());
        let timeout = if any_pending { PARKED_RETRY_MS } else { IDLE_POLL_MS };
        let nc = conns.len().min(MAX_CONNS);
        let mut pollfds: Vec<TPollFd> = Vec::with_capacity(2 + nc);
        // Both listeners are ALWAYS polled; when full we accept-and-close (below)
        // so a connector fails fast instead of stalling on the handshake. [0] is
        // the playback post, [1] the sink-authority control post (N-3a-3).
        pollfds.push(TPollFd {
            fd: listener as i32,
            events: T_POLLIN,
            revents: 0,
        });
        pollfds.push(TPollFd {
            fd: ctl_listener as i32,
            events: T_POLLIN,
            revents: 0,
        });
        let conn_base = pollfds.len();
        for c in conns.iter().take(nc) {
            pollfds.push(TPollFd {
                fd: c.handle() as i32,
                events: T_POLLIN,
                revents: 0,
            });
        }
        let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), timeout) };
        if rc < 0 {
            continue;
        }

        // 3. Accept on either post -- push if there is room, else close so the
        //    connector fails fast and falls to its own fallback. Each conn is
        //    tagged by the post it arrived on: playback (pollfds[0]) vs the
        //    sink-authority control post (pollfds[1], the volume-write gate).
        for (idx, (lfd, control)) in [(listener, false), (ctl_listener, true)].iter().enumerate() {
            if pollfds[idx].revents & T_POLLIN != 0 {
                let h = unsafe { t_srv_accept(*lfd) };
                if h >= 0 {
                    if conns.len() < MAX_CONNS {
                        conns.push(Conn::new(h, *control));
                    } else {
                        let _ = unsafe { t_close(h) };
                    }
                }
            }
        }

        // 4. Service the readable connections (backward, remove-safe).
        let mut i = nc;
        while i > 0 {
            i -= 1;
            let pf = pollfds[conn_base + i];
            if pf.revents & (T_POLLIN | T_POLLHUP) != 0 && !conns[i].service(sh) {
                conns[i].teardown(sh);
                let _ = unsafe { t_close(conns[i].handle()) };
                conns.remove(i);
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<Nocturned>()
}
