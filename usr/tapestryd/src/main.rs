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

mod chords;
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
    REL_X, REL_Y,
};
use crate::keymap::Mods;
use crate::server::{Comp, Conn, MAX_CONNS, MAX_WARP_CONNS, ROOT_TAPESTRY, ROOT_WARP};

// =============================================================================
// User-VA layout (driver-private): the BAR windows + the device rings; the
// weave mappings bump-allocate from 0x0240_0000 (server.rs).
// =============================================================================

const GPU_BAR_WINDOW_VA: u64 = 0x0080_0000;
const KBD_BAR_WINDOW_VA: u64 = 0x00E0_0000;
const GPU_RING_VA: u64 = 0x0150_0000;
// Warp-6 V-1: the one-page backing for the guest-blob probe, in the gap
// between the ring's end and the keyboard DMA. Mapped only transiently inside
// Gpu::probe (created, the blob unref'd, the page unmapped, all before probe
// returns), so it never coexists with a steady-state mapping -- but it still
// gets a non-overlapping fixed VA, asserted below.
const GPU_BLOB_PROBE_VA: u64 = 0x0151_0000;
const KBD_DMA_VA: u64 = 0x0152_0000;
// The tablet's windows (G-7c): its eventq page above the keyboard's, its
// 6-BAR window above the whole DMA region (the KBD-window..GPU-ring gap
// is only 1 MiB -- too small for a 6 MiB BAR window).
const TAB_DMA_VA: u64 = 0x0153_0000;
const TAB_BAR_WINDOW_VA: u64 = 0x0160_0000;
// The mouse function (relative pointer): its eventq page above the
// tablet's, its 6-BAR window above the tablet's window -- which pushed
// the weave bump-allocator base to 0x0240_0000 (server.rs).
const MOUSE_DMA_VA: u64 = 0x0154_0000;
const MOUSE_BAR_WINDOW_VA: u64 = 0x01C0_0000;
// The GPU fenced lane (Warp-2d): 4x64 KiB submit slots + a response page,
// in the gap between the mouse BAR window's end and the weave base. Mapped
// only on virgl devices (Gpu::probe).
const GPU_FLANE_VA: u64 = 0x0220_0000;

// Idle throttle (residual-2: the ~16% compositor idle cost). The FRAME clock
// is a fixed-rate tick that wakes every visible surface -- at 60 Hz a wholly
// idle console still wakes tapestryd AND aurora 60x/sec (each wake pays an HVF
// deep-park vCPU resume). When neither INPUT nor sustained PRESENT pressure has
// been seen for IDLE_AFTER_MS, drop the effective tick to IDLE_HZ; the next
// keyboard/tablet/mouse event snaps it back to the ctl rate. Activity is input
// OR Comp::animating() -- >= ~8 Hz of well-formed presents over a two-bucket
// sliding window (#164): a game holding a key emits no further input events,
// so input-only throttling flapped a paced GLQuake between 60 and the 15 Hz
// tick x the SDL pacer's 50 ms bound (~30 fps, A/B-measured 28.7 quiet vs 61.3
// with one injected input per 100 ms). Bare presents still don't count -- only
// SUSTAINED, SCREEN-CHANGING pressure (hidden-surface presents are filtered in
// note_present -- audit F1): aurora's ~2 Hz cursor blink sums to <= 2 per
// window pair, margin 2 under the threshold, so a quiet console throttles
// exactly as before (re-measured on this build: idle mean 7.2%; the
// DISCRIMINATING witness is `THYLACINE_IDLE_STRICT=20 tools/ci-idle-gate.sh` --
// the gate's default 80% threshold catches spins, not throttle regressions,
// audit F4). The floor
// is bounded by two poll-mode costs -- the keyboard is drained once per pass
// (so first-key-after-idle latency is one idle period) and aurora polls
// /dev/consdrain once per FRAME (so console-output latency during idle is one
// idle period); IDLE_HZ = 15 keeps both <= ~67 ms (imperceptible-to-mild for
// the FIRST event after true idle, then 60 Hz) while cutting the steady idle
// wakes 4x. Disabled under test-mode (the frozen clock is ctl-driven). See
// docs/reference/139-tapestryd.md "Idle throttle".
const IDLE_HZ: u32 = 15;
const IDLE_AFTER_MS: u64 = 250;

const _: () = {
    assert!(GPU_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= KBD_BAR_WINDOW_VA);
    assert!(KBD_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= GPU_RING_VA);
    assert!(GPU_RING_VA + (gpu::RING_DMA_SIZE as u64) <= GPU_BLOB_PROBE_VA);
    assert!(GPU_BLOB_PROBE_VA + 0x1000 <= KBD_DMA_VA); // one page
    assert!(KBD_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= TAB_DMA_VA);
    assert!(TAB_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= MOUSE_DMA_VA);
    assert!(MOUSE_DMA_VA + (input::INPUT_DMA_SIZE as u64) <= TAB_BAR_WINDOW_VA);
    assert!(TAB_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= MOUSE_BAR_WINDOW_VA);
    assert!(MOUSE_BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= GPU_FLANE_VA);
    assert!(GPU_FLANE_VA + (gpu::FLANE_DMA_SIZE as u64) <= 0x0240_0000);
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

/// Post /srv/warp -- the GPU seam's own tree (Warp-2c; GPU-DESIGN section
/// 5: tapestryd hosts the GPU service because warden binding is one
/// exclusive claimant per virtio function). Two posts from one Proc are
/// kernel-supported (no per-Proc service cap; both tombstone together at
/// this Proc's death).
fn post_srv_warp() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"warp".as_ptr(), 4, T_OREAD, 0) };
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
    /// The relative-mouse function (best-effort like both): EV_REL
    /// deltas accumulate into the pointer position AND reach the focused
    /// surface as exact TEV_PTR_REL (mouse-look).
    mouse: Option<InputDev>,
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
        let g = gpu::Gpu::probe(GPU_BAR_WINDOW_VA, GPU_RING_VA, GPU_FLANE_VA, GPU_BLOB_PROBE_VA)?;

        // The input functions are best-effort: an environment without them
        // (or without their functions in the gathered allowance) yields an
        // input-less compositor -- the scanout/present half is unaffected.
        // G-7c: BOTH virtio-input instances are probed identically (the
        // keyboard and the tablet share device id 18, reached as nth 0/1),
        // then classified by the EV_BITS config probe -- supports_abs() is
        // the tablet -- so QEMU device ordering never matters.
        let mut kbd: Option<InputDev> = None;
        let mut tablet: Option<InputDev> = None;
        let mut mouse: Option<InputDev> = None;
        let windows = [
            (0u32, KBD_BAR_WINDOW_VA, KBD_DMA_VA),
            (1u32, TAB_BAR_WINDOW_VA, TAB_DMA_VA),
            (2u32, MOUSE_BAR_WINDOW_VA, MOUSE_DMA_VA),
        ];
        for (nth, bar_va, dma_va) in windows {
            // Probe ALL instances unconditionally (G-7c audit F5): a
            // bring-up fault on one nth (FEATURES_OK, DMA, eventq size)
            // must not cost the other functions too -- a claim on an
            // ABSENT nth fails in microseconds, so there is nothing to
            // save by breaking early. probe() logs its own failure arm.
            // Classification is capability-driven (device ordering never
            // matters): EV_ABS = the tablet; EV_REL without ABS = the
            // mouse (the tablet also reports REL -- its wheel -- so the
            // ABS check runs first); neither = the keyboard.
            match InputDev::probe(nth, bar_va, dma_va) {
                Ok(dev) => {
                    if dev.supports_abs() {
                        if tablet.is_none() {
                            say!("tapestryd: input[{}] is the tablet", nth);
                            tablet = Some(dev);
                        }
                    } else if dev.supports_rel() {
                        if mouse.is_none() {
                            say!("tapestryd: input[{}] is the mouse", nth);
                            mouse = Some(dev);
                        }
                    } else if kbd.is_none() {
                        say!("tapestryd: input[{}] is the keyboard", nth);
                        kbd = Some(dev);
                    }
                }
                Err(_) => {}
            }
        }
        if kbd.is_none() && tablet.is_none() && mouse.is_none() {
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
            mouse,
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
        let warp_listener = match post_srv_warp() {
            Ok(l) => l,
            Err(()) => {
                say!("tapestryd: /srv/warp post failed");
                return Err(Error::Hardware);
            }
        };

        // V-3b-1c-2a: prove the server host3d-ring path (venus-gated,
        // self-skipping) as bring-up evidence, before READY like the gpu probes.
        self.comp.warp_host3d_selftest();

        // V-3b-3c F1: prove a destroyed ring's ridx is re-mintable (the interim
        // monotonic-ridx could not reuse). Venus-gated + self-skipping like above.
        self.comp.warp_ring_recreate_selftest();

        // V-3b-3c-2: prove the device-memory (mem/<handle>) lifecycle -- alloc,
        // sentinel round-trip, destroy, handle-reuse. Venus-gated + self-skipping.
        self.comp.warp_mem_selftest();

        // vkQuake-arc W-3a (WARP-WSI-DESIGN section 7): the WSI host-capability
        // probe -- SET_SCANOUT_BLOB vocabulary + cross-ctx attach, shmem-class,
        // paired negatives. Venus-gated + self-skipping like the siblings; a
        // measurement, not a witness (either verdict is a valid boot).
        self.comp.warp_scanout_blob_probe();

        // vkQuake-arc W-3c-1 (WARP-WSI-DESIGN sections 4-6): the PRESENTABLE
        // lifecycle -- registration accept-set discrimination, the
        // `USE_MAPPABLE`-and-never-mapped HOST3D mint (4.1 as AMENDED: the
        // host refuses USE_SHAREABLE, and guest-invisibility is the absence
        // of any map, not the flag), the Direct bind, and the display-safe
        // teardown's ordering witness (destroy WHILE BOUND). Unlike the W-3a
        // probe above this is a WITNESS, not a measurement: its arms assert.
        self.comp.warp_img_selftest();

        // READY last: all bring-up console output precedes it; the warden's
        // readiness pipe waits on exactly this line.
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");
        self.comp.report_composed_posture();
        say!(
            "tapestryd: serving /srv/tapestry + /srv/warp ({}x{})",
            self.comp.gpu.width,
            self.comp.gpu.height
        );

        let mut conns: Vec<Conn> = Vec::new();
        let mut raw_events: Vec<RawInputEvent> = Vec::new();

        // The FRAME clock (section 18.4): a synthesized fixed-rate tick
        // riding the poll timeout (base virtio-gpu 2D has no guest vblank).
        // Re-anchored when the ctl changes clock-rate.
        let mut anchor = Instant::now();
        let mut ticks_done: u64 = 0;
        let mut cur_hz = self.comp.clock_hz;
        let mut was_frozen = false;
        // Residual-2 idle throttle: the last time real INPUT arrived. Init to
        // now so bring-up runs at the ctl rate until the console settles.
        let mut last_input = Instant::now();

        loop {
            // Residual-2: did any input device drain a raw event this pass?
            // Set after each of the three drains below; bumps last_input.
            let mut input_seen = false;
            // (1) Input drain -> keymap -> the Super chord layer (G-6c:
            // the compositor's reserved plane, intercepted ABOVE the
            // event stream -- a consumed key never reaches a surface) ->
            // the focused surface's events. Modifier keys always flow
            // (clients see mods; Mods::update identifies them).
            if let Some(kbd) = self.kbd.as_mut() {
                raw_events.clear();
                kbd.drain(|ev| raw_events.push(ev));
                input_seen |= !raw_events.is_empty();
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
                input_seen |= !raw_events.is_empty();
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

            // (1c) The mouse drain: REL_X/REL_Y deltas accumulate per SYN
            // frame then commit as ONE ptr_move_rel (exact deltas to the
            // focused surface + pointer accumulation); BTN_* and the
            // wheel dispatch immediately at the accumulated position
            // (same discipline as the tablet arm). REL values are signed
            // on the wire (i32 as u32).
            if let Some(m) = self.mouse.as_mut() {
                raw_events.clear();
                m.drain(|ev| raw_events.push(ev));
                input_seen |= !raw_events.is_empty();
                let mask = self.mods.mask();
                let (mut dx, mut dy) = (0i32, 0i32);
                let commit = |c: &mut Comp, dx: &mut i32, dy: &mut i32| {
                    if *dx != 0 || *dy != 0 {
                        c.ptr_move_rel(*dx, *dy, mask);
                        *dx = 0;
                        *dy = 0;
                    }
                };
                for ev in &raw_events {
                    match ev.etype {
                        EV_REL if ev.code == REL_X => dx = dx.saturating_add(ev.value as i32),
                        EV_REL if ev.code == REL_Y => dy = dy.saturating_add(ev.value as i32),
                        EV_SYN => commit(&mut self.comp, &mut dx, &mut dy),
                        EV_KEY if ev.code >= BTN_LEFT => {
                            commit(&mut self.comp, &mut dx, &mut dy);
                            self.comp.ptr_btn(ev.code, ev.value != 0, mask);
                        }
                        EV_REL if ev.code == REL_WHEEL => {
                            commit(&mut self.comp, &mut dx, &mut dy);
                            self.comp.ptr_scroll(ev.value as i32, mask);
                        }
                        _ => {}
                    }
                }
                commit(&mut self.comp, &mut dx, &mut dy);
            }

            // (2) The FRAME tick (catch-up bounded to one tick per pass --
            // a stalled loop coalesces missed frames, honest for a
            // synthesized clock). Frozen under test-mode (section 18.6):
            // `tick` ctl writes drive time; the anchor re-seats on
            // unfreeze so no wall-clock backlog fires.
            let frozen = self.comp.test_frozen();
            // Residual-2 idle throttle: fold the input activity into last_input,
            // then pick the effective tick rate. Never throttle under test-mode
            // (the frozen clock is ctl-driven). `base_hz` is the ctl rate (60,
            // or whatever `tick`/clock ctl set); `eff_hz` drops to IDLE_HZ once
            // the console has been input-quiet for IDLE_AFTER_MS.
            if input_seen {
                last_input = Instant::now();
            }
            let base_hz = self.comp.clock_hz;
            let eff_hz = if frozen
                || (last_input.elapsed().as_millis() as u64) < IDLE_AFTER_MS
                || self.comp.animating()
            {
                base_hz
            } else {
                IDLE_HZ.min(base_hz)
            };
            if eff_hz != cur_hz || (was_frozen && !frozen) {
                cur_hz = eff_hz;
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

            // (3) The fence pump (W2d): drain retired fenced chains off the
            // device and post each on its owning ctx -- then the deferred
            // event-read + fence-read deliveries (backward, remove-safe).
            self.comp.warp_service_fences();
            let mut i = conns.len();
            while i > 0 {
                i -= 1;
                let ok = conns[i].poll_events(&mut self.comp)
                    && conns[i].poll_fences(&mut self.comp)
                    && conns[i].poll_ring_fences(&mut self.comp);
                if !ok {
                    let mut c = conns.remove(i);
                    c.teardown(&mut self.comp);
                    unsafe { t_close(c.raw_fd()) };
                }
            }
            // #210 custody mirror: fold every conn's park/parse posture
            // into Comp so the W_CTL reader (one conn) sees its siblings.
            #[cfg(feature = "test-mode")]
            {
                let (mut fp, mut rp, mut ib) = (0u32, 0u32, 0u32);
                let (mut fc, mut ff) = (0u32, 0u32);
                for c in &conns {
                    let (f, r, b, cx, fd) = c.w210_summary();
                    if f > 0 && fp == 0 {
                        fc = cx;
                        ff = fd;
                    }
                    fp += f as u32;
                    rp += r as u32;
                    if (b as u32) > ib {
                        ib = b as u32;
                    }
                }
                self.comp.w210_fparked = fp;
                self.comp.w210_rparked = rp;
                self.comp.w210_inbuf_max = ib;
                self.comp.w210_f_ctx = fc;
                self.comp.w210_f_fid = ff;
            }

            // (4) Poll: both listeners (while room; tapestry then warp,
            // Warp-2c) + every conn, bounded by the time to the next FRAME
            // tick. The conns Vec mixes both trees -- Conn.root disjoins
            // their qid spaces, everything else is shared machinery.
            // Per-root budgets (audit F7): a warp client can never close
            // the compositor's own door. Both listeners are still polled
            // together so `conn_base` stays 2 whenever either is armed;
            // each accept re-tests its own budget below.
            // ARM ONLY WHAT WE WILL ACCEPT FROM (round-2 F2 [P1]): a
            // pending connection keeps its listener permanently readable
            // (devsrv's backlog -> POLLIN), and t_poll returns instantly
            // when any fd is ready. Polling a listener whose accept the
            // budget will decline therefore spins the serve loop at 100%
            // -- in the console renderer, bypassing the whole idle
            // throttle. Each listener is armed iff its own accept can run.
            let warp_conns = conns.iter().filter(|c| c.root() == ROOT_WARP).count();
            // Both listeners stay armed when the pool is FULL: the accept
            // below then refuses the connect at once (accept + close), so
            // the backlog entry is consumed (no spin) and the caller sees
            // EOF immediately instead of blocking on the kernel handshake
            // deadline (5 s) inside its own loop -- the H-3b round R2-F2's
            // renderer stall. The warp arm still gates on ITS budget
            // (audit F7: it must never starve the tapestry listener).
            let arm_tapestry = true;
            let arm_warp = warp_conns < MAX_WARP_CONNS;
            let mut pollfds: Vec<TPollFd> = Vec::new();
            if arm_tapestry {
                pollfds.push(TPollFd {
                    fd: listener as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            if arm_warp {
                pollfds.push(TPollFd {
                    fd: warp_listener as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
            }
            let conn_base = pollfds.len();
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
            // W2d: the GPU IRQ is not pollable (the G-5 engine owns it;
            // irq.wait only inside a sync submit), so while fenced chains
            // are in flight the pass pace IS the fence-completion latency
            // -- clamp to 1 ms (the input-drain precedent).
            let timeout = if self.comp.warp_fences_pending() {
                timeout.min(1)
            } else {
                timeout
            };
            let rc = unsafe { t_poll(pollfds.as_mut_ptr(), pollfds.len(), timeout) };
            if rc < 0 {
                continue;
            }

            // (5) Accept (one per listener per pass).
            // Accept from each ARMED listener. The arming above already
            // encodes both budgets, and `conn_base` is derived from how
            // many were pushed -- never a hardcoded 2 (round-2 F2). The
            // tapestry accept runs first and may fill the last slot, so
            // the warp arm re-tests (round-1 F9's overshoot guard, kept).
            let mut idx = 0usize;
            if arm_tapestry {
                if pollfds[idx].revents & T_POLLIN != 0 {
                    let h = unsafe { t_srv_accept(listener) };
                    if h >= 0 {
                        if conns.len() < MAX_CONNS {
                            let id = self.comp.next_conn_id();
                            conns.push(Conn::new(h, id, ROOT_TAPESTRY));
                        } else {
                            // Refuse fast: the pool is full.
                            unsafe { t_close(h) };
                        }
                    }
                }
                idx += 1;
            }
            if arm_warp && pollfds[idx].revents & T_POLLIN != 0 {
                let h = unsafe { t_srv_accept(warp_listener) };
                if h >= 0 {
                    if conns.len() < MAX_CONNS {
                        let id = self.comp.next_conn_id();
                        conns.push(Conn::new(h, id, ROOT_WARP));
                    } else {
                        unsafe { t_close(h) };
                    }
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
