// /tapestryd -- the compositor: the GPU/input owner + the /dev/tapestry 9P
// server (Tapestry G-3, stage 0; TAPESTRY.md section 18, the V1 vote:
// tapestryd-minimal owns scanout from day 0, one fullscreen surface class,
// Aurora arrives later as an ordinary client of the SAME protocol).
//
// The warden binds this binary to BOTH graphics-path PCI functions --
// `virtio-pci:16` (GPU) + `virtio-pci:18` (keyboard) -- via the manifest
// `gather` mode (section 18.7: "its allowance carries all its devices"; the
// warden's per-node loop gathers the matches into ONE grant/Proc, so the
// I-34 allowance narrows to exactly these functions). `lifecycle =
// persistent`: probe brings both devices up, serve posts /srv/tapestry +
// signals READY + runs the compositor loop forever. The RW-7 proc-death
// quiesce is the teardown story: a tapestryd crash resets its virtio
// devices (scanout blanks until a restart re-inits -- the crash contract's
// visible half) and the R2-F3 kernel reaper force-reclaims any orphaned
// client weave mappings after a bounded grace.
//
// WHY PCI for BOTH devices (the G-1 measured rule): all six populated
// QEMU-virt virtio-mmio slots share ONE page-exclusive 4-KiB page whose
// lifetime belongs to stratumd; a second persistent claimant is
// structurally impossible (boot-fatal starvation, measured). Resident
// drivers ride per-function PCI BARs; the MMIO GPU + keyboard stay wired
// for the one-shot kernel-test probes (P4-L / P4-K).
//
// The absorbed G-1 gpud scaffold retires in the same chunk (one exclusive
// claimant per function).
//
// Diagnostics go to the console (t_putstr); stdout is the warden's
// readiness pipe and carries EXACTLY the one READY line.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd
/// wiring; stdout is reserved for the readiness line.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

mod gpu;
mod input;
mod keymap;
mod server;

use alloc::vec::Vec;

use libdriver::driver::{run, Driver};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::hardware::PCI_BAR_VA_STRIDE;
use libthyla_rs::io::Write;
use libthyla_rs::time::Instant;
use libthyla_rs::{
    t_close, t_open, t_poll, t_srv_accept, t_walk_create, TPollFd, T_OPATH, T_OREAD, T_POLLHUP,
    T_POLLIN, T_WALK_OPEN_FROM_ROOT,
};

use crate::input::{Keyboard, RawInputEvent, EV_KEY};
use crate::keymap::Mods;
use crate::server::{Comp, Conn, MAX_CONNS};

// =============================================================================
// User-VA layout (driver-private): two BAR windows + the device rings; the
// weave mappings bump-allocate from 0x0200_0000 (server.rs).
// =============================================================================

const GPU_BAR_WINDOW_VA: u64 = 0x0080_0000;
const KBD_BAR_WINDOW_VA: u64 = 0x00E0_0000;
const GPU_RING_VA: u64 = 0x0150_0000;
const KBD_DMA_VA: u64 = 0x0152_0000;

const _: () = {
    assert!(GPU_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= KBD_BAR_WINDOW_VA);
    assert!(KBD_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= GPU_RING_VA);
    assert!(GPU_RING_VA + (gpu::RING_DMA_SIZE as u64) <= KBD_DMA_VA);
    assert!(KBD_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= 0x0200_0000);
};

/// Post /srv/tapestry (9P-mode; the ptyfs/netd post idiom). Requires the
/// MAY_POST_SERVICE bit the warden confers on persistent drivers.
fn post_srv_tapestry() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"tapestry".as_ptr(), 8, T_OREAD, 0) };
    unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

struct Tapestryd {
    comp: Comp,
    kbd: Option<Keyboard>,
    mods: Mods,
}

impl Driver for Tapestryd {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        say!(
            "tapestryd: grant compat={} pci={:?} irq={} dma={:#x}",
            res.compatible,
            res.pci,
            res.irq.len(),
            res.dma_max
        );

        // The GPU function is mandatory.
        let g = gpu::Gpu::probe(GPU_BAR_WINDOW_VA, GPU_RING_VA)?;

        // The keyboard function is best-effort: an environment without the
        // virtio-keyboard-pci device (or without its function in the
        // gathered allowance) yields an input-less compositor -- the
        // scanout/present half is unaffected.
        let kbd = match Keyboard::probe(KBD_BAR_WINDOW_VA, KBD_DMA_VA) {
            Ok(k) => Some(k),
            Err(_) => {
                say!("tapestryd: no keyboard function (input-less)");
                None
            }
        };

        Ok(Tapestryd {
            comp: Comp::new(g),
            kbd,
            mods: Mods::default(),
        })
    }

    fn serve(mut self, _res: &BoundResources) -> Result<(), Error> {
        let listener = match post_srv_tapestry() {
            Ok(l) => l,
            Err(()) => {
                say!("tapestryd: /srv/tapestry post failed");
                return Err(Error::Hardware);
            }
        };

        // READY last: all bring-up console output precedes it; the warden's
        // readiness pipe waits on exactly this line.
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");
        say!("tapestryd: serving /srv/tapestry ({}x{})", self.comp.gpu.width, self.comp.gpu.height);

        let mut conns: Vec<Conn> = Vec::new();
        let mut raw_events: Vec<RawInputEvent> = Vec::new();

        // The FRAME clock (section 18.4): a synthesized fixed-rate tick
        // riding the poll timeout (base virtio-gpu 2D has no guest vblank).
        // Re-anchored when the ctl changes clock-rate.
        let mut anchor = Instant::now();
        let mut ticks_done: u64 = 0;
        let mut cur_hz = self.comp.clock_hz;

        loop {
            // (1) Input drain -> keymap -> the focused surface's events.
            if let Some(kbd) = self.kbd.as_mut() {
                raw_events.clear();
                kbd.drain(|ev| raw_events.push(ev));
                for ev in &raw_events {
                    if ev.etype != EV_KEY {
                        continue; // EV_SYN separators etc.
                    }
                    let pressed = ev.value != 0;
                    self.mods.update(ev.code, pressed);
                    let mask = self.mods.mask();
                    let rune = if pressed {
                        keymap::resolve(ev.code, mask)
                    } else {
                        0
                    };
                    self.comp.key_event(ev.code, ev.value, rune, mask);
                }
            }

            // (2) The FRAME tick (catch-up bounded to one tick per pass --
            // a stalled loop coalesces missed frames, honest for a
            // synthesized clock).
            if self.comp.clock_hz != cur_hz {
                cur_hz = self.comp.clock_hz;
                anchor = Instant::now();
                ticks_done = 0;
            }
            let period_ms = (1000 / cur_hz.max(1)) as u64;
            let elapsed_ms = anchor.elapsed().as_millis() as u64;
            let due = elapsed_ms / period_ms.max(1);
            if due > ticks_done {
                ticks_done = due;
                self.comp.frame_tick();
            }

            // (3) Deferred event-read deliveries (backward, remove-safe).
            let mut i = conns.len();
            while i > 0 {
                i -= 1;
                if !conns[i].poll_events(&mut self.comp) {
                    let mut c = conns.remove(i);
                    c.teardown(&mut self.comp);
                    unsafe { t_close(c.raw_fd()) };
                }
            }

            // (4) Poll: the listener (while room) + every conn, bounded by
            // the time to the next FRAME tick.
            let has_room = conns.len() < MAX_CONNS;
            let mut pollfds: Vec<TPollFd> = Vec::new();
            if has_room {
                pollfds.push(TPollFd {
                    fd: listener as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            for c in &conns {
                pollfds.push(TPollFd {
                    fd: c.raw_fd() as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            let remain = period_ms.saturating_sub(elapsed_ms % period_ms.max(1));
            let timeout = remain.clamp(1, period_ms.max(1)) as i32;
            let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), timeout) };
            if rc < 0 {
                continue;
            }

            // (5) Accept (one per pass).
            let conn_base = if has_room { 1 } else { 0 };
            if has_room && pollfds[0].revents & T_POLLIN != 0 {
                let h = unsafe { t_srv_accept(listener) };
                if h >= 0 {
                    let id = self.comp.next_conn_id();
                    conns.push(Conn::new(h, id));
                }
            }

            // (6) Service ready conns (backward, remove-safe).
            let nc = conns.len().min(pollfds.len().saturating_sub(conn_base));
            let mut i = nc;
            while i > 0 {
                i -= 1;
                let re = pollfds[conn_base + i].revents;
                if re & (T_POLLIN | T_POLLHUP) != 0 && !conns[i].service(&mut self.comp) {
                    let mut c = conns.remove(i);
                    c.teardown(&mut self.comp);
                    unsafe { t_close(c.raw_fd()) };
                }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<Tapestryd>()
}
