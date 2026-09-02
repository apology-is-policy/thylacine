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
// THE SESSION MODEL (F2; the H-3c-2 EVENT SET): an `EventRing` is ONE 9P
// session to /srv/tapestry (open=connect mints a fresh server conn per
// opener) + ONE Loom ring, and every Surface a client opens on it shares
// both. The compositor keys authority on the peer PROCESS, so a client's
// surfaces stay unresolvable from any other process's session -- the
// per-session isolation the design binds -- while one thread waits for ANY
// of its surfaces' events with one blocking enter (io_uring's one ring per
// thread). One session per ring is load-bearing, not convenience: a Loom
// wait pumps the session of its FIRST in-flight op only
// (loom_wait_for_completions), so a ring over two sessions would starve the
// second -- the H-3c lever measured that starvation across two rings on two
// sessions (the session-reader model), which is what this replaces.
// `Surface::fullscreen` / `open` keep a private ring + session each (the
// one-surface clients: aurora, the battery, warp-prove).
//
// THE WIRE (stage 0):
//   open /srv/tapestry            -> the session root (the ring's)
//   open surface/new (mint)      -> the surface ctl fid; read -> "<id>"
//   write ctl "create W H ..."   -> weave + GPU resource allocated
//   open surface/<id>/weave      -> read geometry; SYS_WEFT_MAP -> the
//                                    zero-copy client mapping (Tweft rides
//                                    the kernel's own session op)
//   open surface/<id>/present    -> a synchronous write of a tpresent; the
//                                    Rwrite is the D1 recycle gate
//   open surface/<id>/event      -> LOOM_OP_READ of 24-byte tevent records
//                                    on the ring's registered handle
//
// EVENT READS ARE SINGLE-SHOT, deliberately: a multishot READ re-arms into
// the SAME registered slice, so a shot landing before the client drains the
// prior one overwrites it -- droppable for FRAME, a lost KEY for the
// never-drop classes. Until Loom grows a provided-buffer pool (the io_uring
// buf_ring analog; a G-6 seam), the client re-arms after each drain --
// correct by construction, one syscall per delivery batch.

#![no_std]
// The `guest` feature is the syscall-bound half (every Surface, the ring's
// Loom + session); off, the crate is the wire types + `ring` (the slot
// bookkeeping), which the host tests drive.
#![cfg_attr(not(feature = "guest"), allow(dead_code))]

extern crate alloc;

#[cfg(feature = "guest")]
use alloc::rc::Rc;
use alloc::vec::Vec;
#[cfg(feature = "guest")]
use core::cell::RefCell;

#[cfg(feature = "guest")]
use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe, ENTER_GETEVENTS};
#[cfg(feature = "guest")]
use libthyla_rs::{
    t_close, t_open, t_read, t_weft_map, t_write, T_ORDWR, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};
#[cfg(feature = "guest")]
use core::sync::atomic::{AtomicBool, Ordering};

mod ring;

/// H-4b-3: the env var the layout-restore tool seeds into a spawned child's
/// `/env`, carrying the one-shot placement claim for the leaf the tool placed
/// it in (13.7's opaque cookie -- the child never learns its placement).
#[cfg(feature = "guest")]
const CLAIM_ENV: &str = "TAPESTRY_CLAIM";

/// Set once the process has consumed its inherited placement claim, so only
/// the FIRST content surface claims. Later surfaces of the same process (and
/// any descendant that inherited the value) open normally. The server-side
/// consume is already one-shot -- a spent token falls back to focus placement
/// -- so this latch is a correctness-neutral optimization that also silences
/// the "claim unmatched" log line a second read would provoke.
#[cfg(feature = "guest")]
static CLAIM_TAKEN: AtomicBool = AtomicBool::new(false);

/// Take this process's inherited placement claim, exactly once. Returns the
/// token iff `TAPESTRY_CLAIM` is present and a well-formed 32-hex `u128` and
/// no earlier call has taken it. Absent/malformed -> None (the common case:
/// a normally-launched program has no such var and opens un-placed).
#[cfg(feature = "guest")]
fn take_env_claim() -> Option<u128> {
    if CLAIM_TAKEN.load(Ordering::Relaxed) {
        return None;
    }
    let v = libthyla_rs::env::var(CLAIM_ENV)?;
    let s = v.trim();
    if s.len() != 32 || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let tok = u128::from_str_radix(s, 16).ok()?;
    // Only NOW consume the latch: an absent/malformed var leaves it unset, so
    // a later well-formed value (a child that writes its own /env) is still
    // honored, while a race between two first opens lets exactly one win.
    if CLAIM_TAKEN.swap(true, Ordering::Relaxed) {
        return None;
    }
    // Spent: drop the var from THIS process's /env (a deep copy -- the
    // spawner's is untouched) so a grandchild spawned from here does not
    // inherit a token that names a leaf already taken. Best-effort: a
    // surviving stale token only ever falls back to focus placement.
    let _ = libthyla_rs::fs::remove_file("/env/TAPESTRY_CLAIM");
    Some(tok)
}

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
    /// The ring carries MAX_RING_SURFACES surfaces already.
    Full,
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

// The ring's ONE registered buffer: an EV_REGION-byte event landing zone per
// slot (a delivery is up to 4 tevent records).
const EV_REGION: u64 = 128;
const EV_CAP: usize = 4 * TEVENT_LEN;
/// Surfaces one EventRing carries. The bound is the compositor SESSION's,
/// not the ring's: the kernel's 9P client holds one tag per in-flight RPC
/// out of a 64-wide table (`P9_SESSION_MAX_OUTSTANDING`), and a parked event
/// read holds its tag until an event arrives -- so N surfaces pin N tags
/// nearly always, and the synchronous RPCs the same thread makes on the
/// session (presents, ctl verbs, the pane-tree reads, `destroy`, the
/// fire-and-forget clunk of every closed fd) need tags of their own: at 64
/// the kernel refuses every send. 48 leaves 16; halcyond's worst case is
/// 36. (The Loom registers 64 handles; the SQ holds 128.)
pub const MAX_RING_SURFACES: usize = 48;
const RING_ENTRIES: u32 = 128;
/// Events a slot holds unread before the ring stops arming its read: the
/// back-pressure that hands a consumer which never polls a surface to the
/// compositor's own per-surface cap (which retires the surface) -- as it
/// was before the event set pulled every surface's events client-side.
const SLOT_QUEUE_CAP: usize = 256;
/// The client-side rect bound: a tpresent descriptor is 32 + 16*(k-1) bytes
/// (the server caps at 64 independently).
pub const MAX_RECTS: usize = 63;
const PRESENT_MAX: usize = TPRESENT_LEN + (MAX_RECTS - 1) * TRECT_LEN;

/// Slot count this library can track ages for (GPU-DESIGN 4.5.8b). The server
/// ships 3; the bound exists so the array below is fixed-size in no_std, and
/// `attach` refuses a larger advertisement rather than truncating.
pub const MAX_SLOTS: usize = 8;

/// `slot_seen` sentinel: this slot's content is UNDEFINED -- never presented on
/// this generation, or invalidated since. `age` reports 0 for it.
const SLOT_UNSEEN: u64 = u64::MAX;

const UD_EVENT: u64 = 2;

/// One mapped surface on an `EventRing` (its session + its Loom ring).
#[cfg(feature = "guest")]
pub struct Surface {
    ring: EventRing,
    /// This surface's slot on the ring (its event queue + registered handle).
    slot: u16,
    /// The ring's session root (the ring closes it; a surface never does).
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
    cur_slot: u32,
    /// Presents completed on this generation; the clock `slot_seen` reads.
    presents: u64,
    /// Per slot, the `presents` value at which it was last presented, or
    /// `SLOT_UNSEEN` when its content is undefined. Drives `age`.
    slot_seen: [u64; MAX_SLOTS],
}

#[cfg(feature = "guest")]
fn read_all(fd: i64, buf: &mut [u8]) -> usize {
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    if n <= 0 {
        0
    } else {
        n as usize
    }
}

#[cfg(feature = "guest")]
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

/// What `create` mints: a hosted content surface, a Role::Chrome surface
/// bound to a pane's tag bar (H-3b), a Role::Menu surface (H-3c), the
/// Role::Status bar (H-3d), or a content surface steered into a claimed
/// empty leaf (H-4b).
#[cfg(feature = "guest")]
#[derive(Clone, Copy)]
enum Mint {
    Content,
    Chrome(u32),
    Menu,
    Status,
    Claim(u128),
}

#[cfg(feature = "guest")]
impl Surface {
    /// A private ring + session, and a fullscreen surface on it (the
    /// one-surface clients; the display geometry off the session's ctl).
    pub fn fullscreen() -> Result<Surface, TapError> {
        let ring = EventRing::connect()?;
        Self::fullscreen_on(&ring)
    }

    /// A fullscreen surface on `ring`.
    pub fn fullscreen_on(ring: &EventRing) -> Result<Surface, TapError> {
        let (w, h) = ring.display_dims().ok_or(TapError::Protocol)?;
        Self::open_on_bound(ring, w, h, Mint::Content)
    }

    /// A private ring + session, and a W x H surface on it.
    pub fn open(w: u32, h: u32) -> Result<Surface, TapError> {
        let ring = EventRing::connect()?;
        Self::open_on(&ring, w, h)
    }

    /// A W x H content surface on `ring`.
    pub fn open_on(ring: &EventRing, w: u32, h: u32) -> Result<Surface, TapError> {
        Self::open_on_bound(ring, w, h, Mint::Content)
    }

    /// H-3b-2: a Role::Chrome surface bound to pane `pane_id`, on `ring`. The
    /// compositor places it at that pane's Daylight tag-bar strip (read
    /// `pane/<id>/tagbar` for the size), never hosts it in a leaf, and fans
    /// it a CONFIGURE carrying the strip size on every relayout.
    /// Renderer-gated server-side: E_PERM for a peer spawned without
    /// T_SPAWN_PERM_CONSOLE_RENDERER. A renderer owns one tag bar per
    /// visible leaf (the H-3b round R2-F2 put them on one session; the
    /// H-3c-2 event set puts them on one ring too).
    pub fn chrome_on(ring: &EventRing, pane_id: u32, w: u32, h: u32) -> Result<Surface, TapError> {
        Self::open_on_bound(ring, w, h, Mint::Chrome(pane_id))
    }

    /// H-3c: a Role::Menu surface on `ring` -- the one ephemeral menu the
    /// compositor places (`menu place <id> <x> <y>`), grabs input for, and
    /// tears down itself (Esc / click-away / a chord / the owner's death;
    /// HALCYON.md 13.6). Invisible until placed; renderer-gated server-side.
    /// Never hosted, never focusable.
    pub fn menu_on(ring: &EventRing, w: u32, h: u32) -> Result<Surface, TapError> {
        Self::open_on_bound(ring, w, h, Mint::Menu)
    }

    /// H-3d: the Role::Status surface on `ring` -- the screen-bottom status
    /// bar the compositor carves the display for and places at the bottom
    /// strip (HALCYON.md 13.6). `w` must be the display width and `h` the
    /// one vertical unit (`statusbar` / the theme's `status_h`), else the
    /// compositor refuses (E_INVAL); one per display; renderer-gated
    /// server-side. Never hosted, never focusable; a CONFIGURE offers the
    /// new width on a display resize.
    pub fn status_on(ring: &EventRing, w: u32, h: u32) -> Result<Surface, TapError> {
        Self::open_on_bound(ring, w, h, Mint::Status)
    }

    /// H-4b: a W x H content surface hosted into the SPECIFIC empty leaf
    /// whose one-shot placement claim `token` was minted by reading that
    /// leaf's `pane/<id>/claim` -- the layout-restore placement path
    /// (HALCYON.md 13.7): the session tool arranges the empty skeleton,
    /// mints a claim per leaf, and hands each token to the child it spawns
    /// there. The token is an opaque cookie: a claim naming no live empty
    /// leaf -- stale (the leaf was re-claimed, hosted or closed since the
    /// mint), already spent, or never minted -- FALLS BACK to the ordinary
    /// focus placement, so a child never fails to create because its
    /// placement hint went stale under it (the client cannot observe
    /// placement either way). Only a MALFORMED token (not 32 hex digits) is
    /// E_INVAL. Placement only: the token confers no authority over any
    /// other tile.
    pub fn open_claim_on(
        ring: &EventRing,
        w: u32,
        h: u32,
        token: u128,
    ) -> Result<Surface, TapError> {
        Self::open_on_bound(ring, w, h, Mint::Claim(token))
    }

    /// `open_claim_on` on a private ring + session.
    pub fn open_claim(w: u32, h: u32, token: u128) -> Result<Surface, TapError> {
        let ring = EventRing::connect()?;
        Self::open_claim_on(&ring, w, h, token)
    }

    fn open_on_bound(ring: &EventRing, w: u32, h: u32, mint: Mint) -> Result<Surface, TapError> {
        // H-4b-3: a restored child's FIRST content surface consumes the
        // placement claim the restore tool seeded in its /env (13.7's opaque
        // cookie -- plain `open` lands it in the tool's target leaf without
        // the child knowing). One-shot; an explicit open_claim or a
        // chrome/menu/status mint is untouched. A normally-launched program
        // has no such var and this is a no-op.
        let mint = match mint {
            Mint::Content => match take_env_claim() {
                Some(tok) => Mint::Claim(tok),
                None => Mint::Content,
            },
            other => other,
        };
        let root = ring.root();
        let fail = |fds: &[i64], e: TapError| {
            for &fd in fds {
                if fd >= 0 {
                    unsafe { t_close(fd) };
                }
            }
            Err(e)
        };
        // After `create` succeeded the surface exists server-side, and only
        // `destroy` (or the session's death) retires it: a failure past that
        // point says so before closing, or the slot leaks until the session
        // ends.
        let fail_created = |ctl: i64, fds: &[i64], e: TapError| {
            unsafe { t_write(ctl, b"destroy".as_ptr(), 7) };
            fail(fds, e)
        };

        // Mint: opening surface/new rebinds the fid onto the new surface's
        // ctl (the netd clone idiom); its read yields the id.
        let ctl = unsafe { t_open(root, b"surface/new".as_ptr(), 11, T_ORDWR) };
        if ctl < 0 {
            return Err(TapError::Create);
        }
        let mut idbuf = [0u8; 16];
        let n = read_all(ctl, &mut idbuf);
        let id: u32 = match core::str::from_utf8(&idbuf[..n])
            .ok()
            .and_then(|s| s.trim().parse().ok())
        {
            Some(id) => id,
            None => return fail(&[ctl], TapError::Protocol),
        };

        // create W H [role=chrome bind=<pane-id> | role=menu | role=status | claim=<tok>]
        let mut cmd = alloc::string::String::new();
        let _ = core::fmt::write(&mut cmd, format_args!("create {} {}", w, h));
        match mint {
            Mint::Content => {}
            Mint::Chrome(pid) => {
                let _ = core::fmt::write(&mut cmd, format_args!(" role=chrome bind={}", pid));
            }
            Mint::Menu => cmd.push_str(" role=menu"),
            Mint::Status => cmd.push_str(" role=status"),
            // The 32-hex form `pane/<id>/claim` minted (the server refuses
            // any other width).
            Mint::Claim(tok) => {
                let _ = core::fmt::write(&mut cmd, format_args!(" claim={:032x}", tok));
            }
        }
        let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
        if rc < 0 {
            // The mint already took a server-side slot (the per-conn cap
            // counts a Minted surface): say `destroy`, or a refused create
            // pins the slot for the session's life (the H-3c-2 round F2).
            return fail_created(ctl, &[ctl], TapError::Create);
        }

        // The weave: geometry read + the zero-copy map (Tweft under the
        // kernel's SYS_WEFT_MAP).
        let mut path = alloc::string::String::new();
        let _ = core::fmt::write(&mut path, format_args!("surface/{}/weave", id));
        let weave_fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
        if weave_fd < 0 {
            return fail_created(ctl, &[ctl], TapError::Map);
        }
        let mut gbuf = [0u8; 128];
        let n = read_all(weave_fd, &mut gbuf);
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
            Some(v) => v,
            None => return fail_created(ctl, &[ctl, weave_fd], TapError::Protocol),
        };
        // `nslots` bounds the age bookkeeping, so it is validated against the
        // array that holds it -- a server advertising more slots than we can
        // track would silently lose invalidations, which is the one failure
        // mode `age` exists to prevent.
        if gw != w || gh != h || stride != w * 4 || nslots == 0 || nslots as usize > MAX_SLOTS {
            return fail_created(ctl, &[ctl, weave_fd], TapError::Protocol);
        }
        let map_va = unsafe { t_weft_map(weave_fd as u64, 0) };
        if map_va <= 0 {
            return fail_created(ctl, &[ctl, weave_fd], TapError::Map);
        }

        let mut ppath = alloc::string::String::new();
        let _ = core::fmt::write(&mut ppath, format_args!("surface/{}/present", id));
        let present_fd = unsafe { t_open(root, ppath.as_ptr(), ppath.len(), T_OWRITE) };
        let mut epath = alloc::string::String::new();
        let _ = core::fmt::write(&mut epath, format_args!("surface/{}/event", id));
        let event_fd = unsafe { t_open(root, epath.as_ptr(), epath.len(), T_OREAD) };
        if present_fd < 0 || event_fd < 0 {
            return fail_created(
                ctl,
                &[ctl, weave_fd, present_fd, event_fd],
                TapError::Protocol,
            );
        }

        // Join the ring: a slot (the event queue) + the event fid on its
        // registered-handle table.
        let slot = match ring.core.borrow_mut().join(event_fd as i32) {
            Ok(s) => s,
            Err(e) => return fail_created(ctl, &[ctl, weave_fd, present_fd, event_fd], e),
        };

        Ok(Surface {
            ring: ring.clone(),
            slot,
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
            cur_slot: 0,
            presents: 0,
            slot_seen: [SLOT_UNSEEN; MAX_SLOTS],
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
            // The redraw request. Geometry is unchanged, but the compositor
            // may have skipped transfers while we were hidden, so EVERY slot's
            // host-side content is now suspect (GPU-DESIGN 4.5.8b).
            self.invalidate_slots();
            return Ok(false);
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
            return Err(if rc == -11 {
                TapError::Busy
            } else {
                TapError::Protocol
            });
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
        // A fresh generation: every slot is zeroed server-side, so no slot
        // carries usable content.
        self.invalidate_slots();
        Ok(())
    }

    /// BUFFER AGE of the slot `pixels` is about to hand out (GPU-DESIGN
    /// 4.5.8b; the `EGL_EXT_buffer_age` contract). **0 means the slot's
    /// content is UNDEFINED -- repaint the whole surface.** `n >= 1` means it
    /// holds the frame presented `n` presents ago, so a client that repaints
    /// only damage must repaint the UNION of every damage rect since then.
    ///
    /// Why a client must consult this at all: slots rotate and nothing copies
    /// content between them, so a slot's non-repainted pixels are whatever
    /// this client last wrote there -- `nslots` presents back. That was
    /// invisible while one host resource per generation quietly accumulated
    /// the frames; per-slot resources (C-2d) remove that accumulator.
    ///
    /// Derived here rather than reported by the compositor because this
    /// library owns the rotation. That is sound only while every server-side
    /// invalidation reaches us as a redraw or a reweave -- the C-2d invariant
    /// in GPU-DESIGN 4.5.8b, which is the compositor's side of this contract.
    pub fn age(&self) -> u32 {
        let seen = self.slot_seen[self.cur_slot as usize];
        if seen == SLOT_UNSEEN {
            return 0;
        }
        // Saturates rather than wrapping: `presents` only ever advances past
        // a recorded `seen`, so this is a total function on real state, and a
        // corrupted one degrades to "repaint more", never to "repaint less".
        (self.presents - seen).min(u32::MAX as u64) as u32
    }

    /// Mark every slot's content undefined, so the next `nslots` presents each
    /// see `age` 0 and repaint fully (GPU-DESIGN 4.5.8b).
    ///
    /// `handle_configure` and `reweave` call this for you. It is PUBLIC for the
    /// case they cannot cover: a client that handles a `CONFIGURE` itself
    /// without routing it through `handle_configure` -- declining a degenerate
    /// resize offer, say -- takes an invalidation that no library call sees.
    /// Leaving that to be remembered is what put the invalidation out of reach
    /// in aurora's sub-floor arm.
    ///
    /// One full repaint is NOT a substitute: it fixes the slot it lands in and
    /// leaves the other `nslots - 1` stale.
    pub fn invalidate(&mut self) {
        self.invalidate_slots();
    }

    /// Mark every slot's content undefined: the next `nslots` presents each
    /// see `age` 0 and repaint fully. The compositor's redraw request and a
    /// reweave both land here -- a redraw invalidates EVERY slot, not just
    /// the one about to be drawn, so one full repaint is NOT enough.
    fn invalidate_slots(&mut self) {
        self.slot_seen = [SLOT_UNSEEN; MAX_SLOTS];
        self.presents = 0;
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

    /// Warp-4: write one verb on THIS surface's own ctl -- the fid the
    /// surface/new mint rebound, so it rides the owning conn by
    /// construction (F2: no other conn can even resolve this surface).
    /// The glsrc adoption half is the first consumer; present/release
    /// keep their dedicated paths.
    pub fn surface_ctl(&self, cmd: &str) -> Result<(), TapError> {
        let rc = unsafe { t_write(self.ctl, cmd.as_ptr(), cmd.len()) };
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
        let len = if rects.len() <= 1 {
            TPRESENT_LEN
        } else {
            TPRESENT_LEN + (rects.len() - 1) * TRECT_LEN
        };
        let mut d = [0u8; PRESENT_MAX];
        d[0..4].copy_from_slice(&TPRESENT_V1.to_le_bytes());
        d[4..8].copy_from_slice(&self.cur_slot.to_le_bytes());
        d[8..12].copy_from_slice(&flags.to_le_bytes());
        d[12..16].copy_from_slice(&(rects.len() as u32).to_le_bytes());
        let r0 = rects.first().copied().unwrap_or(Rect {
            x: 0,
            y: 0,
            w: 0,
            h: 0,
        });
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
        // A synchronous write: the compositor composes inside the write's
        // dispatch, so the Rwrite IS the recycle gate. It rides the ring's
        // session, and this thread is that session's reader for the call --
        // any event reply in flight is demuxed to its CQE meanwhile, for the
        // next poll to reap. (The Loom WRITE it replaces bought nothing here:
        // a present waits for its own completion either way, and a
        // registered handle per present fid halved the ring's surface count.)
        let rc = unsafe { t_write(self.present_fd, d.as_ptr(), len) };
        if rc < 0 {
            // No rotation on failure, so no slot changed hands and the age
            // bookkeeping must not advance either.
            return Err(TapError::Present);
        }
        self.slot_seen[self.cur_slot as usize] = self.presents;
        self.presents += 1;
        self.cur_slot = (self.cur_slot + 1) % self.nslots;
        Ok(())
    }

    /// Non-blocking event poll: reap what the ring has completed, re-arm.
    /// `Err(Closed)` once the stream has EOF'd and the backlog is drained.
    pub fn poll_event(&mut self) -> Result<Option<Event>, TapError> {
        self.ring.poll()?;
        self.ring.core.borrow_mut().take_event(self.slot)
    }

    /// Block until an event for THIS surface is available, then return it
    /// (other surfaces' events reaped meanwhile wait in their own queues;
    /// a multi-surface client waits on the ring instead -- `EventRing::wait`
    /// -- and polls each surface). An empty completion (a retired surface's
    /// EOF) yields `Err(Closed)`.
    pub fn wait_event(&mut self) -> Result<Event, TapError> {
        loop {
            if let Some(ev) = self.ring.core.borrow_mut().take_event(self.slot)? {
                return Ok(ev);
            }
            self.ring.wait()?;
        }
    }
}

#[cfg(feature = "guest")]
impl Drop for Surface {
    fn drop(&mut self) {
        // The weave fid's clunk drops the client mapping (the kernel
        // ClunkMap). The SURFACE retires server-side only on ctl `destroy`,
        // its conn's teardown, or a wedge -- a clunk is bookkeeping -- and
        // the ring's session outlives any one surface on it, so every
        // surface says `destroy` itself (the H-3b close's shared chrome
        // leaked one server-side surface per dropped tag bar without it).
        // Harmless on a surface the compositor already retired (a menu it
        // dismissed): the stale fid answers an error nobody reads. FIRST,
        // so the event read still in flight EOFs promptly and frees the
        // ring slot; then leave the ring (the registered table drops this
        // fid; the in-flight read keeps its own pin); then the fds.
        if self.ctl >= 0 {
            unsafe { t_write(self.ctl, b"destroy".as_ptr(), 7) };
        }
        self.ring.core.borrow_mut().leave(self.slot);
        for fd in [self.event_fd, self.present_fd, self.weave_fd, self.ctl] {
            if fd >= 0 {
                unsafe { t_close(fd) };
            }
        }
    }
}

/// cfg-3: read the display geometry off a throwaway connection (the
/// startup push's verify-readback; the same "display W H" line
/// Surface::fullscreen parses).
#[cfg(feature = "guest")]
pub fn display_dims() -> Option<(u32, u32)> {
    let root = unsafe {
        t_open(
            T_WALK_OPEN_FROM_ROOT,
            b"/srv/tapestry".as_ptr(),
            13,
            T_OREAD,
        )
    };
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
#[cfg(feature = "guest")]
pub fn global_ctl_once(cmd: &str) -> Result<(), TapError> {
    let root = unsafe {
        t_open(
            T_WALK_OPEN_FROM_ROOT,
            b"/srv/tapestry".as_ptr(),
            13,
            T_OREAD,
        )
    };
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

// =============================================================================
// The event set (H-3c-2): ONE session + ONE Loom ring per client, shared by
// every Surface opened on it -- io_uring's one ring per thread. A slot per
// surface holds its event queue + its place on the registered-handle table
// (index == slot index, always: the table is replaced whole at every join
// and leave, a placeholder fid standing in every slot without a live event
// fid, so an SQE the kernel has queued but not yet consumed can never be
// re-bound to another surface's fid by a rebuild in between); a slot is
// reused only after the read in flight on it has completed (the retiring
// state), so a stale completion can never land in a re-minted surface's
// region, and its generation tag makes the check belt and braces.
// =============================================================================

/// An fd closed with its owner; declared last in `RingCore` so the session
/// root outlives the Loom (whose registered table + in-flight reads hold
/// their own Spoor refs) by construction, not by a comment.
#[cfg(feature = "guest")]
struct OwnedFd(i64);

#[cfg(feature = "guest")]
impl Drop for OwnedFd {
    fn drop(&mut self) {
        if self.0 >= 0 {
            unsafe { t_close(self.0) };
        }
    }
}

#[cfg(feature = "guest")]
struct RingCore {
    ring: Ring,
    staging: RegisteredBuffer,
    slots: Vec<ring::Slot>,
    /// A read-only `ctl` fid: the registered table's entry for every slot
    /// without a live event fid (free, retiring, or left). A stale read on
    /// it returns text nobody reads -- never another surface's event.
    placeholder: OwnedFd,
    root: OwnedFd,
}

/// ONE 9P session to the compositor + ONE Loom ring, shared by every
/// Surface opened on it: `wait` blocks until ANY of them has an event
/// (then poll each); `poll` reaps without blocking. Clone = another handle
/// to the same ring; the session closes with the last one.
#[cfg(feature = "guest")]
pub struct EventRing {
    core: Rc<RefCell<RingCore>>,
}

#[cfg(feature = "guest")]
impl Clone for EventRing {
    fn clone(&self) -> EventRing {
        EventRing {
            core: self.core.clone(),
        }
    }
}

#[cfg(feature = "guest")]
impl EventRing {
    /// Connect a fresh session to /srv/tapestry and set up its ring.
    pub fn connect() -> Result<EventRing, TapError> {
        let root = unsafe {
            t_open(
                T_WALK_OPEN_FROM_ROOT,
                b"/srv/tapestry".as_ptr(),
                13,
                T_OREAD,
            )
        };
        if root < 0 {
            return Err(TapError::Connect);
        }
        Self::adopt(root)
    }

    /// A ring over a session root fd the caller opened; the ring owns it
    /// from here (closed with the ring).
    pub fn adopt(root: i64) -> Result<EventRing, TapError> {
        if root < 0 {
            return Err(TapError::Connect);
        }
        let ring = match Ring::setup(RING_ENTRIES, 0) {
            Ok(r) => r,
            Err(_) => {
                unsafe { t_close(root) };
                return Err(TapError::Loom);
            }
        };
        let mut staging = match RegisteredBuffer::new(MAX_RING_SURFACES * EV_REGION as usize) {
            Ok(b) => b,
            Err(_) => {
                unsafe { t_close(root) };
                return Err(TapError::Loom);
            }
        };
        staging.as_mut_slice().fill(0);
        if ring.register_buffers(&[staging.buf_reg()]).is_err() {
            unsafe { t_close(root) };
            return Err(TapError::Loom);
        }
        let placeholder = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OREAD) };
        if placeholder < 0 {
            unsafe { t_close(root) };
            return Err(TapError::Protocol);
        }
        let slots = ring::new_slots();
        Ok(EventRing {
            core: Rc::new(RefCell::new(RingCore {
                ring,
                staging,
                slots,
                placeholder: OwnedFd(placeholder),
                root: OwnedFd(root),
            })),
        })
    }

    /// The session root fd (for the pane-tree files, `ctl`, ...). Owned by
    /// the ring: never close it.
    pub fn root(&self) -> i64 {
        self.core.borrow().root.0
    }

    /// The display geometry off this session's global ctl.
    pub fn display_dims(&self) -> Option<(u32, u32)> {
        let root = self.root();
        let gctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OREAD) };
        if gctl < 0 {
            return None;
        }
        let mut buf = [0u8; 256];
        let n = read_all(gctl, &mut buf);
        unsafe { t_close(gctl) };
        let text = core::str::from_utf8(&buf[..n]).ok()?;
        parse_two(text, "display ")
    }

    /// How many surfaces are on the ring (a retiring slot counts until its
    /// last read completes).
    pub fn surfaces(&self) -> usize {
        self.core.borrow().slots.iter().filter(|s| s.used).count()
    }

    /// Block until at least one event has completed for SOME surface on the
    /// ring, then reap everything posted (into the surfaces' queues). The
    /// wait is bounded by the compositor's FRAME ticks to any visible surface.
    pub fn wait(&self) -> Result<(), TapError> {
        self.core.borrow_mut().pump(true)
    }

    /// Reap what has completed without blocking (re-arming idle reads).
    pub fn poll(&self) -> Result<(), TapError> {
        self.core.borrow_mut().pump(false)
    }
}

#[cfg(feature = "guest")]
impl RingCore {
    /// Take a slot for `event_fd` and put the fid on the table.
    fn join(&mut self, event_fd: i32) -> Result<u16, TapError> {
        let i = ring::join(&mut self.slots, event_fd)?;
        if let Err(e) = self.reregister() {
            self.slots[i as usize].used = false;
            return Err(e);
        }
        Ok(i)
    }

    /// A surface is going: off the table now; the slot frees now, or when
    /// the read in flight on it completes.
    fn leave(&mut self, slot: u16) {
        if ring::leave(&mut self.slots, slot) {
            let _ = self.reregister();
        }
    }

    /// The registered-handle table (`ring::table`): index == slot index,
    /// replaced whole (the kernel has no per-entry update;
    /// IORING_REGISTER_FILES_UPDATE's index stability, emulated). A read in
    /// flight keeps its own pin; an SQE still queued keeps its index --
    /// which names the placeholder once its slot left. A failed replace
    /// leaves the kernel's table as it was, which every index still matches.
    fn reregister(&mut self) -> Result<(), TapError> {
        let fds = ring::table(&self.slots, self.placeholder.0 as i32);
        self.ring.register_handles(&fds).map_err(|_| TapError::Loom)
    }

    /// Arm a read on every slot that wants one (`ring::arm_wanted`; single-
    /// shot, see the header note). Returns how many were queued.
    fn arm_all(&mut self) -> Result<u32, TapError> {
        let mut n = 0u32;
        for i in 0..self.slots.len() {
            if !ring::arm_wanted(&self.slots[i]) {
                continue;
            }
            let gen = self.slots[i].gen;
            let sqe = Sqe::read(
                i as u32,
                0,
                EV_CAP as u32,
                0,
                (i as u64) * EV_REGION,
                ring::ud(i, gen),
            );
            self.ring.try_submit(&sqe).map_err(|_| TapError::Loom)?;
            self.slots[i].armed = true;
            n += 1;
        }
        Ok(n)
    }

    /// Submit the armed reads and reap: blocking (>= 1 completion; the
    /// kernel drives this session's reader meanwhile) or not (a submit-only
    /// enter -- the kernel demuxes replies only inside a blocking wait or
    /// this thread's own RPCs on the session, so a poll sees what those
    /// posted).
    fn pump(&mut self, block: bool) -> Result<(), TapError> {
        let n = self.arm_all()?;
        if block && !ring::any_armed(&self.slots) {
            // Nothing in flight: the kernel's wait returns at once and a
            // caller looping on `wait` would spin. Every surface here is
            // closed, full, or gone -- nothing on this ring can complete.
            return Err(TapError::Closed);
        }
        let rc = if block {
            self.ring.enter(n, 1, ENTER_GETEVENTS)
        } else if n > 0 {
            self.ring.enter(n, 0, 0)
        } else {
            Ok(0)
        };
        rc.map_err(|_| TapError::Loom)?;
        while let Some(cqe) = self.ring.reap() {
            ring::route(
                &mut self.slots,
                self.staging.as_mut_slice(),
                cqe.user_data,
                cqe.result,
            );
        }
        Ok(())
    }

    fn take_event(&mut self, slot: u16) -> Result<Option<Event>, TapError> {
        ring::take_event(&mut self.slots, slot)
    }
}

// RingCore drops by field order: the Loom (its fd close abandons any read
// still in flight), the staging buffer, the queues, then the two owned fds
// -- the placeholder, and the session root last.

/// A tiny front-pop helper (Vec as a FIFO; event volumes are small).
pub(crate) trait PopFirst<T> {
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
