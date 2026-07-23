// libtapestry -- the native client-side weave for /dev/tapestry (Tapestry
// G-3, stage 0; TAPESTRY.md section 18). The aux-track POC's client model
// (arm events -> drain -> render -> present + recycle-gate) folded onto the
// REAL substrate: the `Loom` seam trait is cashed for `libthyla_rs::loom`
// (the io_uring-inverted 9P ring), the mock surface for the live tapestryd
// protocol.
//
// NAMING: a Loom weaves threads into fabric; a Tapestry is the woven
// picture. The client maps a surface's *weave* (the shared framebuffer the
// V2 grant-is-the-share vote delivers) and operates a *Loom ring* to
// present into it.
//
// THE SESSION MODEL (F2): `Surface::open` connects its OWN 9P session
// (open=connect on /srv/tapestry mints a fresh server conn per opener), so
// this client's surfaces are unresolvable from any other session -- the
// per-session isolation the design binds. Procs that deliberately share the
// session (fd inheritance) share its surfaces; that is the Plan 9
// shared-mount semantic, not a leak.
//
// THE WIRE (stage 0):
//   open /srv/tapestry            -> the private session root
//   open surface/new (mint)      -> the surface ctl fid; read -> "<id>"
//   write ctl "create W H"       -> weave + GPU resource allocated
//   open surface/<id>/weave      -> read geometry; SYS_WEFT_MAP -> the
//                                    zero-copy client mapping (Tweft rides
//                                    the kernel's own session op)
//   open surface/<id>/present    -> LOOM_OP_WRITE of a 32-byte tpresent;
//                                    the CQE is the D1 recycle gate
//   open surface/<id>/event      -> LOOM_OP_READ of 24-byte tevent records
//
// EVENT READS ARE SINGLE-SHOT, deliberately: a multishot READ re-arms into
// the SAME registered slice, so a shot landing before the client drains the
// prior one overwrites it -- droppable for FRAME, a lost KEY for the
// never-drop classes. Until Loom grows a provided-buffer pool (the io_uring
// buf_ring analog; a G-6 seam), the client re-arms after each drain --
// correct by construction, one syscall per delivery batch.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;

use libthyla_rs::loom::{Cqe, Ring, RegisteredBuffer, Sqe, ENTER_GETEVENTS};
use libthyla_rs::{
    t_close, t_open, t_read, t_weft_map, t_write, T_ORDWR, T_OREAD, T_OWRITE,
    T_WALK_OPEN_FROM_ROOT,
};

pub const TPRESENT_LEN: usize = 32;
pub const TPRESENT_V1: u32 = 1;
/// Hold-this-frame (section 18.6; test-mode builds only): the present
/// completes normally but the scanout push defers until `release`.
pub const TPRESENT_HOLD: u32 = 1 << 0;
/// One additional damage rect (multi-rect, G-6c): rect_count k >= 2 rides
/// rects 1..k inline after the 32-byte header (payload 32 + 16*(k-1)).
pub const TRECT_LEN: usize = 16;
pub const TEVENT_LEN: usize = 24;

pub const TEV_KEY: u16 = 1;
pub const TEV_PTR_MOVE: u16 = 2;
pub const TEV_PTR_BTN: u16 = 3;
/// Relative pointer motion: value packs signed display-pixel deltas
/// dx<<16|dy (i16 each); routed to the FOCUSED surface (section 18.4).
pub const TEV_PTR_REL: u16 = 9;
pub const TEV_SCROLL: u16 = 4;
pub const TEV_FRAME: u16 = 5;
pub const TEV_CONFIGURE: u16 = 6;
pub const TEV_FOCUS: u16 = 7;
pub const TEV_CLOSE: u16 = 8;

/// A decoded tevent record (section 18.4; 24 bytes on the wire).
#[derive(Clone, Copy, Debug)]
pub struct Event {
    pub kind: u16,
    pub code: u16,
    pub value: u32,
    pub rune: u32,
    pub mods: u16,
    pub flags: u16,
    pub tick: u64,
}

/// A damage rectangle.
#[derive(Clone, Copy, Debug)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

#[derive(Debug)]
pub enum TapError {
    Connect,
    Protocol,
    Create,
    Map,
    Loom,
    Present,
    Closed,
    /// A resize ack answered E_AGAIN: the serial went stale (a newer
    /// CONFIGURE superseded it -- drain events and ack that one) or a
    /// prior reweave is still draining (present a frame, then re-ack).
    Busy,
}

// The staging RegisteredBuffer layout: the tpresent descriptor (header +
// inline rect array) at 0, the event landing zone at EV_OFF.
const EV_OFF: u64 = 1024;
const EV_CAP: usize = 4 * TEVENT_LEN; // up to 4 records per delivery
const STAGING_LEN: usize = 4096;
/// The client-side rect bound: the descriptor region below EV_OFF holds
/// 32 + 16*(k-1) bytes -> k <= 63 (the server caps at 64 independently).
pub const MAX_RECTS: usize = (EV_OFF as usize - TPRESENT_LEN) / TRECT_LEN + 1;

const UD_PRESENT: u64 = 1;
const UD_EVENT: u64 = 2;

/// One mapped surface + its private tapestryd session + the Loom ring that
/// presents into it.
pub struct Surface {
    root: i64,
    ctl: i64,
    weave_fd: i64,
    present_fd: i64,
    event_fd: i64,
    pub id: u32,
    pub w: u32,
    pub h: u32,
    /// Row stride in BYTES (w * 4).
    pub stride: u32,
    slot_stride: u64,
    nslots: u32,
    map_va: u64,
    ring: Ring,
    staging: RegisteredBuffer,
    cur_slot: u32,
    event_armed: bool,
    /// Latched when the event stream EOFs (the surface retired server-side):
    /// no further reads are armed -- without the latch, a non-blocking
    /// poll_event caller would re-arm through the EOF forever.
    closed: bool,
    pending: Vec<Event>,
    seq: u64,
}

fn read_all(fd: i64, buf: &mut [u8]) -> usize {
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    if n <= 0 {
        0
    } else {
        n as usize
    }
}

fn parse_two(text: &str, key: &str) -> Option<(u32, u32)> {
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix(key) {
            let mut it = rest.split_ascii_whitespace();
            let a = it.next()?.parse().ok()?;
            let b = it.next()?.parse().ok()?;
            return Some((a, b));
        }
    }
    None
}

impl Surface {
    /// Connect a fresh session and create a fullscreen surface (the display
    /// geometry is read from the global ctl).
    pub fn fullscreen() -> Result<Surface, TapError> {
        let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
        if root < 0 {
            return Err(TapError::Connect);
        }
        let gctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OREAD) };
        if gctl < 0 {
            unsafe { t_close(root) };
            return Err(TapError::Protocol);
        }
        let mut buf = [0u8; 256];
        let n = read_all(gctl, &mut buf);
        unsafe { t_close(gctl) };
        let text = core::str::from_utf8(&buf[..n]).map_err(|_| TapError::Protocol)?;
        let (w, h) = parse_two(text, "display ").ok_or(TapError::Protocol)?;
        Self::open_on(root, w, h)
    }

    /// Connect a fresh session and create a W x H surface.
    pub fn open(w: u32, h: u32) -> Result<Surface, TapError> {
        let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
        if root < 0 {
            return Err(TapError::Connect);
        }
        Self::open_on(root, w, h)
    }

    fn open_on(root: i64, w: u32, h: u32) -> Result<Surface, TapError> {
        let fail = |fds: &[i64], e: TapError| {
            for &fd in fds {
                if fd >= 0 {
                    unsafe { t_close(fd) };
                }
            }
            Err(e)
        };

        // Mint: opening surface/new rebinds the fid onto the new surface's
        // ctl (the netd clone idiom); its read yields the id.
        let ctl = unsafe { t_open(root, b"surface/new".as_ptr(), 11, T_ORDWR) };
        if ctl < 0 {
            return fail(&[root], TapError::Create);
        }
        let mut idbuf = [0u8; 16];
        let n = read_all(ctl, &mut idbuf);
        let id: u32 = match core::str::from_utf8(&idbuf[..n])
            .ok()
            .and_then(|s| s.trim().parse().ok())
        {
            Some(id) => id,
            None => return fail(&[root, ctl], TapError::Protocol),
        };

        // create W H
        let mut cmd = alloc::string::String::new();
        let _ = core::fmt::write(&mut cmd, format_args!("create {} {}", w, h));
        let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
        if rc < 0 {
            return fail(&[root, ctl], TapError::Create);
        }

        // The weave: geometry read + the zero-copy map (Tweft under the
        // kernel's SYS_WEFT_MAP).
        let mut path = alloc::string::String::new();
        let _ = core::fmt::write(&mut path, format_args!("surface/{}/weave", id));
        let weave_fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
        if weave_fd < 0 {
            return fail(&[root, ctl], TapError::Map);
        }
        let mut gbuf = [0u8; 128];
        let n = read_all(weave_fd, &mut gbuf);
        let gtext = core::str::from_utf8(&gbuf[..n]).map_err(|_| TapError::Protocol)?;
        let mut it = gtext.split_ascii_whitespace();
        let gw: u32 = it.next().and_then(|s| s.parse().ok()).ok_or(TapError::Protocol)?;
        let gh: u32 = it.next().and_then(|s| s.parse().ok()).ok_or(TapError::Protocol)?;
        let stride: u32 = it.next().and_then(|s| s.parse().ok()).ok_or(TapError::Protocol)?;
        let slot_stride: u64 = it.next().and_then(|s| s.parse().ok()).ok_or(TapError::Protocol)?;
        let nslots: u32 = it.next().and_then(|s| s.parse().ok()).ok_or(TapError::Protocol)?;
        if gw != w || gh != h || stride != w * 4 || nslots == 0 {
            return fail(&[root, ctl, weave_fd], TapError::Protocol);
        }
        let map_va = unsafe { t_weft_map(weave_fd as u64, 0) };
        if map_va <= 0 {
            return fail(&[root, ctl, weave_fd], TapError::Map);
        }

        let mut ppath = alloc::string::String::new();
        let _ = core::fmt::write(&mut ppath, format_args!("surface/{}/present", id));
        let present_fd = unsafe { t_open(root, ppath.as_ptr(), ppath.len(), T_OWRITE) };
        let mut epath = alloc::string::String::new();
        let _ = core::fmt::write(&mut epath, format_args!("surface/{}/event", id));
        let event_fd = unsafe { t_open(root, epath.as_ptr(), epath.len(), T_OREAD) };
        if present_fd < 0 || event_fd < 0 {
            return fail(&[root, ctl, weave_fd, present_fd, event_fd], TapError::Protocol);
        }

        // The Loom ring: register the two op fids + the staging buffer.
        let ring = match Ring::setup(8, 0) {
            Ok(r) => r,
            Err(_) => return fail(&[root, ctl, weave_fd, present_fd, event_fd], TapError::Loom),
        };
        let mut staging = RegisteredBuffer::new(STAGING_LEN).map_err(|_| TapError::Loom)?;
        staging.as_mut_slice().fill(0);
        if ring
            .register_handles(&[present_fd as i32, event_fd as i32])
            .is_err()
            || ring.register_buffers(&[staging.buf_reg()]).is_err()
        {
            return fail(&[root, ctl, weave_fd, present_fd, event_fd], TapError::Loom);
        }

        Ok(Surface {
            root,
            ctl,
            weave_fd,
            present_fd,
            event_fd,
            id,
            w,
            h,
            stride,
            slot_stride,
            nslots,
            map_va: map_va as u64,
            ring,
            staging,
            cur_slot: 0,
            event_armed: false,
            closed: false,
            pending: Vec::new(),
            seq: 2,
        })
    }

    /// Handle a CONFIGURE event (section 18.3). A same-size CONFIGURE is
    /// the compositor's full-REDRAW request; a size-changing one is the
    /// resize offer, which this acks and reweaves onto. Returns whether
    /// the surface geometry CHANGED (true: the old pixel view is gone --
    /// re-derive layout, then repaint). On ANY Ok return from a CONFIGURE
    /// the caller must fully repaint + present. `Err(Busy)` means the
    /// offer went stale mid-ack -- keep draining events; a newer
    /// CONFIGURE carries the current offer.
    pub fn handle_configure(&mut self, ev: &Event) -> Result<bool, TapError> {
        if ev.kind != TEV_CONFIGURE {
            return Ok(false);
        }
        let cw = ev.value >> 16;
        let ch = ev.value & 0xffff;
        if cw == self.w && ch == self.h {
            return Ok(false); // the redraw request; geometry unchanged
        }
        self.reweave(cw, ch, ev.code)?;
        Ok(true)
    }

    /// Ack a resize offer and swap onto the new weave generation:
    /// write `resize W H <serial>` (the Rwrite is the server's generation
    /// fence), open a FRESH weave fid (the old fid's kernel-side map
    /// binding is pinned to the old generation -- fresh state needs a
    /// fresh fid), re-read the geometry, map the new weave, then clunk
    /// the old fid (the kernel unmaps the old client mapping -- the
    /// spec's ClunkMap; map-new-before-clunk-old keeps the client mapped
    /// throughout). `cur_slot` restarts at 0 -- every slot of the new
    /// generation is untouched.
    ///
    /// A failure AFTER a successful ack (map/open trouble) leaves the
    /// server on the new generation with this client still holding the
    /// old mapping: presents would show the new generation's zeroed
    /// slots. Callers treat any non-Busy error as fatal for the surface.
    pub fn reweave(&mut self, w: u32, h: u32, serial: u16) -> Result<(), TapError> {
        let mut cmd = alloc::string::String::new();
        let _ = core::fmt::write(&mut cmd, format_args!("resize {} {} {}", w, h, serial));
        let rc = unsafe { t_write(self.ctl, cmd.as_ptr(), cmd.len()) };
        if rc < 0 {
            return Err(if rc == -11 { TapError::Busy } else { TapError::Protocol });
        }

        let mut path = alloc::string::String::new();
        let _ = core::fmt::write(&mut path, format_args!("surface/{}/weave", self.id));
        let new_fd = unsafe { t_open(self.root, path.as_ptr(), path.len(), T_OREAD) };
        if new_fd < 0 {
            return Err(TapError::Map);
        }
        let mut gbuf = [0u8; 128];
        let n = read_all(new_fd, &mut gbuf);
        let parsed = core::str::from_utf8(&gbuf[..n]).ok().and_then(|t| {
            let mut it = t.split_ascii_whitespace();
            let gw: u32 = it.next()?.parse().ok()?;
            let gh: u32 = it.next()?.parse().ok()?;
            let stride: u32 = it.next()?.parse().ok()?;
            let slot_stride: u64 = it.next()?.parse().ok()?;
            let nslots: u32 = it.next()?.parse().ok()?;
            Some((gw, gh, stride, slot_stride, nslots))
        });
        let (gw, gh, stride, slot_stride, nslots) = match parsed {
            Some(p) => p,
            None => {
                unsafe { t_close(new_fd) };
                return Err(TapError::Protocol);
            }
        };
        if gw != w || gh != h || stride != w * 4 || nslots != self.nslots {
            unsafe { t_close(new_fd) };
            return Err(TapError::Protocol);
        }
        let map_va = unsafe { t_weft_map(new_fd as u64, 0) };
        if map_va <= 0 {
            unsafe { t_close(new_fd) };
            return Err(TapError::Map);
        }

        // The swap: the old fid's clunk drops the old generation's client
        // mapping (its VA is dead after this line).
        unsafe { t_close(self.weave_fd) };
        self.weave_fd = new_fd;
        self.map_va = map_va as u64;
        self.w = w;
        self.h = h;
        self.stride = stride;
        self.slot_stride = slot_stride;
        self.cur_slot = 0;
        Ok(())
    }

    /// The CURRENT draw slot's pixels (u32 BGRA little-endian: 0xAARRGGBB).
    pub fn pixels(&mut self) -> &mut [u32] {
        let base = self.map_va + (self.cur_slot as u64) * self.slot_stride;
        let count = (self.w as usize) * (self.h as usize);
        // SAFETY: the mapped weave covers nslots * slot_stride bytes and
        // slot_stride >= w*h*4; the mapping is RW for the Proc's lifetime
        // (held by the weave fid + the kernel share machinery).
        unsafe { core::slice::from_raw_parts_mut(base as *mut u32, count) }
    }

    /// Present the current slot (None = full surface), wait for its CQE (the
    /// recycle gate), rotate to the next slot. Event CQEs reaped while
    /// waiting are queued for `poll_event`.
    pub fn present(&mut self, rect: Option<Rect>) -> Result<(), TapError> {
        match rect {
            None => self.submit_present(0, &[]),
            Some(r) => self.submit_present(0, &[r]),
        }
    }

    /// Present a MULTI-rect damage list (G-6c): rect0 rides the header,
    /// the rest inline after it. An empty list = full-surface damage.
    pub fn present_rects(&mut self, rects: &[Rect]) -> Result<(), TapError> {
        self.submit_present(0, rects)
    }

    /// Present with TPRESENT_HOLD (section 18.6; test-mode builds only):
    /// the pixel work lands but the scanout push waits for `release` --
    /// the golden-image capture primitive.
    pub fn present_hold(&mut self, rect: Option<Rect>) -> Result<(), TapError> {
        match rect {
            None => self.submit_present(TPRESENT_HOLD, &[]),
            Some(r) => self.submit_present(TPRESENT_HOLD, &[r]),
        }
    }

    /// Flush this surface's held presents (F13): `release <id>` on the
    /// session's global ctl (ownership-gated server-side).
    pub fn release(&mut self) -> Result<(), TapError> {
        let ctl = unsafe { t_open(self.root, b"ctl".as_ptr(), 3, T_OWRITE) };
        if ctl < 0 {
            return Err(TapError::Protocol);
        }
        let mut cmd = alloc::string::String::new();
        let _ = core::fmt::write(&mut cmd, format_args!("release {}", self.id));
        let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
        unsafe { t_close(ctl) };
        if rc < 0 {
            return Err(TapError::Protocol);
        }
        Ok(())
    }

    /// cfg-3: write one global-ctl command on THIS connection. The
    /// apply-authority gate checks the CONN's kernel-stamped peer, so an
    /// authority verb (`mode ...`) must ride the caller's own conn --
    /// never the shared /dev/tapestry mount, whose peer is the mounter.
    /// Opens ctl per write (authority writes are rare).
    pub fn global_ctl(&self, cmd: &str) -> Result<(), TapError> {
        let ctl = unsafe { t_open(self.root, b"ctl".as_ptr(), 3, T_OWRITE) };
        if ctl < 0 {
            return Err(TapError::Protocol);
        }
        let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
        unsafe { t_close(ctl) };
        if rc < 0 {
            return Err(TapError::Protocol);
        }
        Ok(())
    }

    fn submit_present(&mut self, flags: u32, rects: &[Rect]) -> Result<(), TapError> {
        if rects.len() > MAX_RECTS {
            return Err(TapError::Present);
        }
        self.seq += 1;
        let ud = (self.seq << 8) | UD_PRESENT;
        let len = if rects.len() <= 1 {
            TPRESENT_LEN
        } else {
            TPRESENT_LEN + (rects.len() - 1) * TRECT_LEN
        };
        {
            let d = self.staging.as_mut_slice();
            d[0..4].copy_from_slice(&TPRESENT_V1.to_le_bytes());
            d[4..8].copy_from_slice(&self.cur_slot.to_le_bytes());
            d[8..12].copy_from_slice(&flags.to_le_bytes());
            d[12..16].copy_from_slice(&(rects.len() as u32).to_le_bytes());
            let r0 = rects.first().copied().unwrap_or(Rect { x: 0, y: 0, w: 0, h: 0 });
            d[16..20].copy_from_slice(&r0.x.to_le_bytes());
            d[20..24].copy_from_slice(&r0.y.to_le_bytes());
            d[24..28].copy_from_slice(&r0.w.to_le_bytes());
            d[28..32].copy_from_slice(&r0.h.to_le_bytes());
            for (i, r) in rects.iter().enumerate().skip(1) {
                let o = TPRESENT_LEN + (i - 1) * TRECT_LEN;
                d[o..o + 4].copy_from_slice(&r.x.to_le_bytes());
                d[o + 4..o + 8].copy_from_slice(&r.y.to_le_bytes());
                d[o + 8..o + 12].copy_from_slice(&r.w.to_le_bytes());
                d[o + 12..o + 16].copy_from_slice(&r.h.to_le_bytes());
            }
        }
        let sqe = Sqe::write(0, 0, len as u32, 0, 0, ud);
        self.ring.try_submit(&sqe).map_err(|_| TapError::Loom)?;
        // Wait for THIS present's CQE; route any event CQE that arrives
        // first (one ring, mixed ops -- correlate by user_data).
        loop {
            self.ring
                .enter(1, 1, ENTER_GETEVENTS)
                .map_err(|_| TapError::Loom)?;
            let mut saw = false;
            while let Some(cqe) = self.ring.reap() {
                saw = true;
                if cqe.user_data == ud {
                    if cqe.result < 0 {
                        return Err(TapError::Present);
                    }
                    self.cur_slot = (self.cur_slot + 1) % self.nslots;
                    return Ok(());
                }
                self.route(cqe);
            }
            if !saw {
                // enter returned without a reapable CQE; go around.
                continue;
            }
        }
    }

    fn route(&mut self, cqe: Cqe) {
        if cqe.user_data == UD_EVENT {
            self.event_armed = false;
            if cqe.result == 0 {
                // EOF: the surface retired server-side; latch closed.
                self.closed = true;
            }
            if cqe.result > 0 {
                let n = (cqe.result as usize).min(EV_CAP);
                let d = self.staging.as_mut_slice();
                let mut off = EV_OFF as usize;
                let end = EV_OFF as usize + n;
                while off + TEVENT_LEN <= end {
                    let g16 = |o: usize| u16::from_le_bytes([d[o], d[o + 1]]);
                    let g32 =
                        |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]);
                    let g64 = |o: usize| {
                        let mut b = [0u8; 8];
                        b.copy_from_slice(&d[o..o + 8]);
                        u64::from_le_bytes(b)
                    };
                    self.pending.push(Event {
                        kind: g16(off),
                        code: g16(off + 2),
                        value: g32(off + 4),
                        rune: g32(off + 8),
                        mods: g16(off + 12),
                        flags: g16(off + 14),
                        tick: g64(off + 16),
                    });
                    off += TEVENT_LEN;
                }
            }
        }
    }

    /// Arm the event read if idle (single-shot; see the header note).
    fn arm_events(&mut self) -> Result<(), TapError> {
        if self.event_armed || self.closed {
            return Ok(());
        }
        let sqe = Sqe::read(1, 0, EV_CAP as u32, 0, EV_OFF, UD_EVENT);
        self.ring.try_submit(&sqe).map_err(|_| TapError::Loom)?;
        self.ring.enter(1, 0, 0).map_err(|_| TapError::Loom)?;
        self.event_armed = true;
        Ok(())
    }

    /// Non-blocking event poll: drain any completed reads, re-arm.
    /// `Err(Closed)` once the stream has EOF'd and the backlog is drained.
    pub fn poll_event(&mut self) -> Result<Option<Event>, TapError> {
        self.arm_events()?;
        while let Some(cqe) = self.ring.reap() {
            self.route(cqe);
        }
        self.arm_events()?;
        if self.closed && self.pending.is_empty() {
            return Err(TapError::Closed);
        }
        Ok(self.pending.pop_first())
    }

    /// Block until at least one event is available, then return it. An empty
    /// completion (a retired surface's EOF) yields `Err(Closed)`.
    pub fn wait_event(&mut self) -> Result<Event, TapError> {
        loop {
            if let Some(ev) = self.pending.pop_first() {
                return Ok(ev);
            }
            if self.closed {
                return Err(TapError::Closed); // EOF observed; backlog drained
            }
            self.arm_events()?;
            self.ring
                .enter(0, 1, ENTER_GETEVENTS)
                .map_err(|_| TapError::Loom)?;
            while let Some(cqe) = self.ring.reap() {
                self.route(cqe); // an EOF latches `closed` inside route()
            }
        }
    }
}

impl Drop for Surface {
    fn drop(&mut self) {
        // The weave fid's clunk drops the client mapping (the kernel
        // ClunkMap); the ctl clunk + conn close retire the surface
        // server-side.
        for fd in [self.event_fd, self.present_fd, self.weave_fd, self.ctl, self.root] {
            if fd >= 0 {
                unsafe { t_close(fd) };
            }
        }
    }
}

/// cfg-3: read the display geometry off a throwaway connection (the
/// startup push's verify-readback; the same "display W H" line
/// Surface::fullscreen parses).
pub fn display_dims() -> Option<(u32, u32)> {
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
    if root < 0 {
        return None;
    }
    let gctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OREAD) };
    if gctl < 0 {
        unsafe { t_close(root) };
        return None;
    }
    let mut buf = [0u8; 256];
    let n = read_all(gctl, &mut buf);
    unsafe { t_close(gctl) };
    unsafe { t_close(root) };
    let text = core::str::from_utf8(&buf[..n]).ok()?;
    parse_two(text, "display ")
}

/// cfg-3: one-shot global-ctl write on a THROWAWAY connection -- the
/// aurora startup mode push runs BEFORE any Surface exists (so the
/// console surface is born at the pushed geometry), and the gate's
/// peer identity is per-conn, so the throwaway conn still carries the
/// CALLER's identity. Connect + write + close; fail-soft to the caller.
pub fn global_ctl_once(cmd: &str) -> Result<(), TapError> {
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
    if root < 0 {
        return Err(TapError::Connect);
    }
    let ctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OWRITE) };
    if ctl < 0 {
        unsafe { t_close(root) };
        return Err(TapError::Protocol);
    }
    let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
    unsafe { t_close(ctl) };
    unsafe { t_close(root) };
    if rc < 0 {
        return Err(TapError::Protocol);
    }
    Ok(())
}

/// A tiny front-pop helper (Vec as a FIFO; event volumes are small).
trait PopFirst<T> {
    fn pop_first(&mut self) -> Option<T>;
}
impl<T> PopFirst<T> for Vec<T> {
    fn pop_first(&mut self) -> Option<T> {
        if self.is_empty() {
            None
        } else {
            Some(self.remove(0))
        }
    }
}
