// libtapestry -- the client-side weave for Thylacine's graphics fast-path.
//
// NAMING: a Loom weaves threads into fabric; a Tapestry is the woven picture
// a loom produces. Here the client operates a *Loom ring* (the io_uring-
// inverted 9P transport, docs/LOOM.md) to present a *Tapestry* (a framebuffer
// surface). Loom is the instrument; the Tapestry is what the client weaves
// onto the display.
//
// STATUS: native PROOF-OF-CONCEPT (compile-only; the auxiliary track never
// boots). It is the Phase-B pattern -- an API-shaped skeleton against the
// DOCUMENTED FUTURE Loom surface. The genuine Loom calls sit behind the
// `Loom` seam trait; today only `MockLoom` implements it. When Loom-6 lands a
// native API (`libthyla_rs::loom`), a `ThylaLoom: Loom` impl replaces the mock
// with ZERO change to Display/Tapestry. See usr/apps/TAPESTRY-DESIGN.md.
//
// THE MODEL (what an app -- or the SDL backend -- does):
//   let mut dpy = Display::new(loom)?;          // arms the multishot event stream
//   let mut tap = dpy.new_tapestry(w, h)?;      // server allocs the surface (Burrow + virtio resource)
//   loop {
//       while let Some(ev) = dpy.poll_event(&mut tap) { ... handle input/vsync ... }
//       render(tap.back_buffer_mut());          // draw into the back buffer (zero-copy)
//       dpy.present(&mut tap, rect)?;           // operate the Loom: present + recycle-gate
//   }
//
// THE LOOM MAPPING (how each call becomes a Loom op -- the real backend):
//   * new_tapestry  -> a control RPC to `tapestryd` (allocate a W*H surface =
//                      a shared Burrow + a virtio-gpu 2D resource), then
//                      SYS_LOOM_REGISTER the present-fid + event-fid handles.
//   * present(rect) -> LOOM_OP_WRITE of the rect descriptor to the present-fid
//                      (registered handle); user_data = the buffer id. The
//                      framebuffer pixels are NOT copied over the ring -- the
//                      host DMA-reads the shared Burrow during the server's
//                      TRANSFER_TO_HOST_2D. The CQE = "transfer done, buffer
//                      reusable" (decision D1: present-CQE is the recycle gate;
//                      vsync is a separate event for pacing).
//   * arm_events    -> a MULTISHOT LOOM_OP_READ on the event-fid (Loom-5): one
//                      submission, a CQE per input/vsync event forever.
//   * reap/wait     -> drain the CQ (SQPOLL makes submission syscall-free,
//                      Loom-4; min_complete>=1 waits, death-interruptible #811).

#![no_std]

extern crate alloc;

use alloc::collections::VecDeque;
use alloc::vec::Vec;

use libthyla_rs::err::{Error, Result};

/// A damage rectangle (virtio-gpu rect shape: u32 fields).
#[derive(Clone, Copy, Debug)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

impl Rect {
    /// The whole surface.
    pub fn full(w: u32, h: u32) -> Rect {
        Rect { x: 0, y: 0, w, h }
    }
}

/// An input / display event delivered over the multishot event stream.
#[derive(Clone, Copy, Debug)]
pub enum Event {
    Key { code: u32, pressed: bool },
    Pointer { x: i32, y: i32, buttons: u32 },
    /// The host presented a frame -- the pacing signal (decision D1: distinct
    /// from a present completion, which is the buffer-recycle signal).
    Vsync,
    Resize { width: u32, height: u32 },
    /// The display/server is going away.
    Close,
}

/// What reaping the Loom CQ yields. The real backend decodes a raw `loom_cqe`
/// (user_data tag + the event bytes from the event registered-buffer) into one
/// of these; the mock produces them directly.
pub enum Reaped {
    /// A present op completed: its buffer (correlated by `user_data`) is free.
    PresentDone { user_data: u64 },
    /// An event arrived on the multishot stream.
    Event(Event),
}

/// The seam to the Loom ring. The ONLY Thylacine-specific surface libtapestry
/// needs; everything above it is portable client logic. Implemented today by
/// `MockLoom`; by `libthyla_rs::loom` once Loom-6 lands.
pub trait Loom {
    /// Submit a present: `LOOM_OP_WRITE(present_fid, rect)`. `user_data`
    /// correlates the completion. Non-blocking (SQPOLL drives it).
    fn submit_present(&mut self, surface_id: u32, rect: Rect, user_data: u64) -> Result<()>;
    /// Arm the multishot event read once (`LOOM_OP_READ` on the event-fid).
    fn arm_events(&mut self) -> Result<()>;
    /// Reap one completion if available (non-blocking).
    fn reap(&mut self) -> Option<Reaped>;
    /// Block until at least one completion is available, then reap it
    /// (`SYS_LOOM_ENTER` with `min_complete = 1`; death-interruptible).
    fn wait_reap(&mut self) -> Option<Reaped>;
}

struct Buffer {
    pixels: Vec<u8>,
    /// 0 = free; otherwise the `user_data` of the in-flight present holding it.
    in_flight_ud: u64,
}

/// A framebuffer surface the client weaves. Holds N back buffers (triple-
/// buffered by default) so the CPU can render frame N+1 while frame N is in
/// flight to the host. In the real backend the buffers are sub-regions of one
/// shared Burrow (mapped here, ATTACH_BACKING'd as a virtio-gpu resource in
/// the server); here they are heap allocations.
pub struct Tapestry {
    width: u32,
    height: u32,
    stride: u32,
    surface_id: u32,
    buffers: Vec<Buffer>,
    current: usize,
}

impl Tapestry {
    pub fn width(&self) -> u32 {
        self.width
    }
    pub fn height(&self) -> u32 {
        self.height
    }
    /// Bytes per row (the framebuffer is 32bpp RGBA).
    pub fn stride(&self) -> u32 {
        self.stride
    }
    /// The buffer the client should draw into this frame.
    pub fn back_buffer_mut(&mut self) -> &mut [u8] {
        &mut self.buffers[self.current].pixels
    }
}

/// A connection to the display (the Loom ring + the `tapestryd` session).
pub struct Display<L: Loom> {
    loom: L,
    events: VecDeque<Event>,
    next_surface_id: u32,
    next_user_data: u64,
}

impl<L: Loom> Display<L> {
    /// Open the display over a Loom ring. Arms the multishot event stream.
    pub fn new(loom: L) -> Result<Display<L>> {
        let mut d = Display {
            loom,
            events: VecDeque::new(),
            next_surface_id: 1,
            next_user_data: 1,
        };
        d.loom.arm_events()?;
        Ok(d)
    }

    /// Allocate a W*H surface. The real backend asks `tapestryd` to create the
    /// shared Burrow + a virtio-gpu 2D resource and maps it here; this POC
    /// allocates heap buffers of the same geometry.
    pub fn new_tapestry(&mut self, width: u32, height: u32) -> Result<Tapestry> {
        if width == 0 || height == 0 {
            return Err(Error::InvalidArgument);
        }
        let stride = width.checked_mul(4).ok_or(Error::InvalidArgument)?;
        let size = stride.checked_mul(height).ok_or(Error::InvalidArgument)? as usize;
        let mut buffers = Vec::with_capacity(3);
        for _ in 0..3 {
            buffers.push(Buffer {
                pixels: alloc::vec![0u8; size],
                in_flight_ud: 0,
            });
        }
        let surface_id = self.next_surface_id;
        self.next_surface_id += 1;
        Ok(Tapestry {
            width,
            height,
            stride,
            surface_id,
            buffers,
            current: 0,
        })
    }

    /// Drain one pending event (non-blocking). Pumps the CQ first so present
    /// completions free buffers and events queue.
    pub fn poll_event(&mut self, tap: &mut Tapestry) -> Option<Event> {
        while let Some(r) = self.loom.reap() {
            self.route(tap, r);
        }
        self.events.pop_front()
    }

    /// Present the current back buffer, then rotate to the next free buffer.
    /// Blocks (recycle gate, D1) only if every other buffer is still in flight.
    pub fn present(&mut self, tap: &mut Tapestry, rect: Rect) -> Result<()> {
        let ud = self.next_ud();
        tap.buffers[tap.current].in_flight_ud = ud;
        self.loom.submit_present(tap.surface_id, rect, ud)?;

        let next = (tap.current + 1) % tap.buffers.len();
        // Reap until the buffer we are about to draw into is free. With triple
        // buffering this almost never blocks; it bounds in-flight presents.
        while tap.buffers[next].in_flight_ud != 0 {
            match self.loom.wait_reap() {
                Some(r) => self.route(tap, r),
                None => return Err(Error::BrokenPipe), // ring closed
            }
        }
        tap.current = next;
        Ok(())
    }

    fn route(&mut self, tap: &mut Tapestry, r: Reaped) {
        match r {
            Reaped::PresentDone { user_data } => {
                for b in tap.buffers.iter_mut() {
                    if b.in_flight_ud == user_data {
                        b.in_flight_ud = 0;
                    }
                }
            }
            Reaped::Event(ev) => self.events.push_back(ev),
        }
    }

    fn next_ud(&mut self) -> u64 {
        let ud = self.next_user_data;
        // Never hand out 0 (the free sentinel); wrap past it.
        self.next_user_data = self.next_user_data.wrapping_add(1);
        if self.next_user_data == 0 {
            self.next_user_data = 1;
        }
        ud
    }
}

// =============================================================================
// MockLoom -- the temporary backend so the POC compiles + the model runs.
// =============================================================================

/// A stand-in `Loom` that completes presents immediately, emits a Vsync per
/// present, and schedules a Close after `frames_to_close` presents so an
/// offline demo terminates. REPLACE with `libthyla_rs::loom` (Loom-6).
pub struct MockLoom {
    pending: VecDeque<Reaped>,
    presents: u32,
    frames_to_close: u32,
    events_armed: bool,
}

impl MockLoom {
    pub fn new(frames_to_close: u32) -> MockLoom {
        MockLoom {
            pending: VecDeque::new(),
            presents: 0,
            frames_to_close,
            events_armed: false,
        }
    }
}

impl Loom for MockLoom {
    fn submit_present(&mut self, _surface_id: u32, _rect: Rect, user_data: u64) -> Result<()> {
        // Model: TRANSFER_TO_HOST_2D + FLUSH complete; the buffer is reusable,
        // and the host presents a frame (Vsync).
        self.pending.push_back(Reaped::PresentDone { user_data });
        self.pending.push_back(Reaped::Event(Event::Vsync));
        self.presents += 1;
        if self.presents == self.frames_to_close {
            self.pending.push_back(Reaped::Event(Event::Close));
        }
        Ok(())
    }

    fn arm_events(&mut self) -> Result<()> {
        self.events_armed = true;
        Ok(())
    }

    fn reap(&mut self) -> Option<Reaped> {
        self.pending.pop_front()
    }

    fn wait_reap(&mut self) -> Option<Reaped> {
        // The mock never truly blocks -- completions are always already queued.
        self.pending.pop_front()
    }
}
