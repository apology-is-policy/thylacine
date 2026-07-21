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
mod pane;
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

use crate::input::{
    InputDev, RawInputEvent, ABS_X, ABS_Y, BTN_LEFT, EV_ABS, EV_KEY, EV_REL, EV_SYN, REL_WHEEL,
};
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
// The tablet's windows (G-7c): its eventq page above the keyboard's, its
// 6-BAR window above the whole DMA region (the KBD-window..GPU-ring gap
// is only 1 MiB -- too small for a 6 MiB BAR window).
const TAB_DMA_VA: u64 = 0x0153_0000;
const TAB_BAR_WINDOW_VA: u64 = 0x0160_0000;

const _: () = {
    assert!(GPU_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= KBD_BAR_WINDOW_VA);
    assert!(KBD_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= GPU_RING_VA);
    assert!(GPU_RING_VA + (gpu::RING_DMA_SIZE as u64) <= KBD_DMA_VA);
    assert!(KBD_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= TAB_DMA_VA);
    assert!(TAB_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= TAB_BAR_WINDOW_VA);
    assert!(TAB_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= 0x0200_0000);
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
    kbd: Option<InputDev>,
    /// The tablet function (G-7c; best-effort like the keyboard).
    tablet: Option<InputDev>,
    /// The tablet's raw absolute position + per-axis inclusive max
    /// (ABS_INFO); scaled to display px at the SYN commit.
    tab_ax: u32,
    tab_ay: u32,
    tab_max: (u32, u32),
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

        // The input functions are best-effort: an environment without them
        // (or without their functions in the gathered allowance) yields an
        // input-less compositor -- the scanout/present half is unaffected.
        // G-7c: BOTH virtio-input instances are probed identically (the
        // keyboard and the tablet share device id 18, reached as nth 0/1),
        // then classified by the EV_BITS config probe -- supports_abs() is
        // the tablet -- so QEMU device ordering never matters.
        let mut kbd: Option<InputDev> = None;
        let mut tablet: Option<InputDev> = None;
        let windows = [(0u32, KBD_BAR_WINDOW_VA, KBD_DMA_VA), (1u32, TAB_BAR_WINDOW_VA, TAB_DMA_VA)];
        for (nth, bar_va, dma_va) in windows {
            // Probe BOTH instances unconditionally (G-7c audit F5): a
            // bring-up fault on nth 0 (FEATURES_OK, DMA, eventq size)
            // must not cost the other function too -- a claim on an
            // ABSENT nth fails in microseconds, so there is nothing to
            // save by breaking early. probe() logs its own failure arm.
            match InputDev::probe(nth, bar_va, dma_va) {
                Ok(dev) => {
                    if dev.supports_abs() {
                        if tablet.is_none() {
                            say!("tapestryd: input[{}] is the tablet", nth);
                            tablet = Some(dev);
                        }
                    } else if kbd.is_none() {
                        say!("tapestryd: input[{}] is the keyboard", nth);
                        kbd = Some(dev);
                    }
                }
                Err(_) => {}
            }
        }
        if kbd.is_none() && tablet.is_none() {
            say!("tapestryd: no input functions (input-less)");
        }
        let tab_max = tablet
            .as_ref()
            .map(|t| (t.abs_max(ABS_X as u8), t.abs_max(ABS_Y as u8)))
            .unwrap_or((0x7FFF, 0x7FFF));

        Ok(Tapestryd {
            comp: Comp::new(g),
            kbd,
            tablet,
            tab_ax: 0,
            tab_ay: 0,
            tab_max,
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
        let mut was_frozen = false;

        loop {
            // (1) Input drain -> keymap -> the Super chord layer (G-6c:
            // the compositor's reserved plane, intercepted ABOVE the
            // event stream -- a consumed key never reaches a surface) ->
            // the focused surface's events. Modifier keys always flow
            // (clients see mods; Mods::update identifies them).
            if let Some(kbd) = self.kbd.as_mut() {
                raw_events.clear();
                kbd.drain(|ev| raw_events.push(ev));
                for ev in &raw_events {
                    if ev.etype != EV_KEY {
                        continue; // EV_SYN separators etc.
                    }
                    let pressed = ev.value != 0;
                    let was_mod = self.mods.update(ev.code, pressed);
                    let mask = self.mods.mask();
                    if !was_mod && self.comp.chord_key(ev.code, ev.value, mask) {
                        continue;
                    }
                    let rune = if pressed {
                        keymap::resolve(ev.code, mask)
                    } else {
                        0
                    };
                    self.comp.key_event(ev.code, ev.value, rune, mask);
                }
            }

            // (1b) Tablet drain (G-7c) -> the under-pointer surface. ABS
            // records update the raw position; the EV_SYN frame boundary
            // commits ONE coalesced MOVE (scaled to display px); BTN_*
            // and the wheel dispatch immediately (non-droppable). The
            // Super chord plane is a KEYBOARD reservation -- pointer
            // events flow regardless of held modifiers (clients see mods).
            if let Some(tab) = self.tablet.as_mut() {
                raw_events.clear();
                tab.drain(|ev| raw_events.push(ev));
                let mask = self.mods.mask();
                let (dw, dh) = (self.comp.gpu.width, self.comp.gpu.height);
                let mut moved = false;
                let commit =
                    |c: &mut Comp, ax: u32, ay: u32, moved: &mut bool| {
                        if !*moved {
                            return;
                        }
                        *moved = false;
                        let (mx, my) = self.tab_max;
                        let px = (ax.min(mx) as u64 * dw.saturating_sub(1) as u64
                            / mx.max(1) as u64) as u32;
                        let py = (ay.min(my) as u64 * dh.saturating_sub(1) as u64
                            / my.max(1) as u64) as u32;
                        c.ptr_move(px, py, mask);
                    };
                for ev in &raw_events {
                    match ev.etype {
                        EV_ABS if ev.code == ABS_X => {
                            self.tab_ax = ev.value;
                            moved = true;
                        }
                        EV_ABS if ev.code == ABS_Y => {
                            self.tab_ay = ev.value;
                            moved = true;
                        }
                        EV_SYN => {
                            commit(&mut self.comp, self.tab_ax, self.tab_ay, &mut moved);
                        }
                        EV_KEY if ev.code >= BTN_LEFT => {
                            // A button click must land AT its position even
                            // if the device batched MOVE+BTN in one frame.
                            commit(&mut self.comp, self.tab_ax, self.tab_ay, &mut moved);
                            self.comp.ptr_btn(ev.code, ev.value != 0, mask);
                        }
                        EV_REL if ev.code == REL_WHEEL => {
                            commit(&mut self.comp, self.tab_ax, self.tab_ay, &mut moved);
                            self.comp.ptr_scroll(ev.value as i32, mask);
                        }
                        _ => {}
                    }
                }
                // A trailing ABS run without its SYN (ring-boundary split):
                // commit anyway -- the next drain's SYN is a no-op then.
                commit(&mut self.comp, self.tab_ax, self.tab_ay, &mut moved);
            }

            // (2) The FRAME tick (catch-up bounded to one tick per pass --
            // a stalled loop coalesces missed frames, honest for a
            // synthesized clock). Frozen under test-mode (section 18.6):
            // `tick` ctl writes drive time; the anchor re-seats on
            // unfreeze so no wall-clock backlog fires.
            let frozen = self.comp.test_frozen();
            if self.comp.clock_hz != cur_hz || (was_frozen && !frozen) {
                cur_hz = self.comp.clock_hz;
                anchor = Instant::now();
                ticks_done = 0;
            }
            was_frozen = frozen;
            let period_ms = (1000 / cur_hz.max(1)) as u64;
            let elapsed_ms = anchor.elapsed().as_millis() as u64;
            if !frozen {
                let due = elapsed_ms / period_ms.max(1);
                if due > ticks_done {
                    ticks_done = due;
                    self.comp.frame_tick();
                }
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
            let timeout = if frozen {
                30 // no tick scheduled; a short pace keeps input drained
            } else {
                remain.clamp(1, period_ms.max(1)) as i32
            };
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
