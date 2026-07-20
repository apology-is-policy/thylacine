// The tapestryd 9P server + compositor core (Tapestry G-3; TAPESTRY.md
// section 18). Serves the /dev/tapestry tree (section 18.5, stage 0: ctl +
// surface/) over /srv + dev9p -- the ptyfs/netd native-server lineage: the
// Conn/fid table, frame extractor, dispatch, deferred replies, and the
// 4-site cancel discipline are the audited ptyfs shapes; the Tweft handler,
// the deferred-read tri-state, and the per-conn qid ownership gate are the
// audited netd shapes.
//
// THE I-40 PRESENT HALF (specs/tapestry_present.tla; the server half of the
// SPEC-TO-CODE map). The surface lifecycle is the spec's state machine:
//
//   WeaveFirst  = `create W H` on the surface ctl: t_dma_create_weave (the
//                 G-2 kernel-minted share-admissible subtype) + map + zero +
//                 RESOURCE_CREATE_2D + whole-weave ATTACH_BACKING
//                 (backed := TRUE, serverRef := TRUE). `armed` becomes real
//                 LAZILY at the first Tweft (weft_ensure below) -- the
//                 netd precedent; the Map guard is indifferent to when the
//                 registration happens, only that retire disarms it.
//   Reweave     = `resize W H <serial>` on the surface ctl (G-6b): the
//                 ack of a size-changing CONFIGURE mints the NEW weave
//                 generation (fresh DMA + fresh resource id) and is THE
//                 GENERATION FENCE -- the Rwrite completes only after the
//                 allocation (reply-after-alloc, R2-F5), and the conn
//                 stream is FIFO, so post-ack presents validate/blit
//                 against the new geometry. The displaced generation
//                 drains passively (never read again; its last content
//                 stays displayed) and retires at the first post-fence
//                 present (RetireDisplaced + ServerRelease) or with the
//                 surface. At most one drains (<=2 gens; busy -> E_AGAIN).
//   Map         = kernel-side (G-2): the client's SYS_WEFT_MAP claims the
//                 registered share consume-once. tapestryd never observes
//                 the claim; its Woven->Live edge rides the first present.
//   Submit/Complete = h_present: every present is handled SYNCHRONOUSLY --
//                 validate, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH, reply
//                 Rwrite (the client's CQE = the D1 recycle gate). The
//                 in-flight window opens and closes INSIDE one dispatch, so
//                 the in-flight present set is EMPTY at every retire
//                 decision point: the tapestry_present.tla quiesce
//                 obligation (ServerRelease's "intransfer = 0") holds BY
//                 CONSTRUCTION at stage 0. A pipelined controlq (G-6+) must
//                 implement the real drain before touching retire.
//   Complete's displayed update = scanout_take(): on a present completion,
//                 a surface with no scanout owner takes scanout (the F16
//                 switch-at-first-present-COMPLETE alignment; never before
//                 its first frame has transferred).
//   Destroy/ServerRelease = retire(): ctl `destroy`, the owning conn's
//                 teardown/Tversion reset, or the R2-F4 WEDGE. Ordering
//                 (the I-40 obligation this server EXISTS to uphold):
//                 (1) quiesce -- empty by construction, asserted above;
//                 (2) SYS_WEFT_UNSHARE (registry-removal-BEFORE-page-free;
//                     discharges the spec Map guard's wstate half -- a Tweft
//                     claim racing the retire finds nothing and fails
//                     closed; on an already-claimed share the unshare is a
//                     harmless miss);
//                 (3) scanout release (SET_SCANOUT 0 if displayed);
//                 (4) DETACH_BACKING + RESOURCE_UNREF (the GPU resource
//                     dies before its backing);
//                 (5) unmap + close the weave DMA (serverRef := FALSE; the
//                     pages free when the client's mapping ref also drops,
//                     #847 -- or when the R2-F3 kernel reaper force-reclaims
//                     an orphaned client mapping after tapestryd itself
//                     dies, the ServerDeath leg).
//
// F2 (per-session isolation): a surface is resolvable ONLY by the conn that
// minted it. Every client attaches its OWN session (open=connect on
// /srv/tapestry mints a fresh SrvConn + dev9p session per opener), so
// conn == client session; walk/readdir/open/ops all gate on owner_conn +
// the per-slot generation (the netd net-3d slot-reuse discipline). Procs
// that deliberately SHARE one session (fd inheritance, or ops through the
// shared /dev/tapestry boot mount) share its surfaces -- the Plan 9
// shared-mount semantic, capability-coherent (the session IS the
// capability).
//
// F9 (caps): MAX_SURFACES_PER_CONN + the dimension bound (<= display) at
// create. R2-F4 (never-drop set): FRAME coalesces/drops; a non-droppable
// event overflowing the bounded queue WEDGES the surface (force-retire +
// CLOSE), never blocks and never drops a control event for a live client.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_burrow_detach, t_close, t_dma_create_weave, t_dma_map, t_weft_share, t_weft_unshare,
    T_GID_SYSTEM, T_PRINCIPAL_SYSTEM, T_PROT_READ, T_PROT_WRITE, T_RIGHT_MAP, T_RIGHT_READ,
    T_RIGHT_WRITE,
};

use crate::gpu::Gpu;
use crate::pane::{self, Layout, Mode, Rect, Role};

pub const MAX_CONNS: usize = 8;
const MAX_FIDS: usize = 32;
pub const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

/// F9: the per-client surface-count cap + the global slot pool.
const MAX_SURFACES: usize = 8;
const MAX_SURFACES_PER_CONN: usize = 4;

/// Triple buffering (D1): one weave carries three page-aligned slots.
const WEAVE_SLOTS: u32 = 3;

/// R2-F4: the bounded per-surface event queue. FRAME coalesces; a
/// non-droppable overflow wedges the surface.
const EVENT_QUEUE_CAP: usize = 128;

const PAGE: u64 = 0x1000;

/// The weave-mapping VA window in tapestryd's own AS (bump-allocated;
/// freed VAs are not reused at stage 0 -- bounded by the surface caps per
/// generation and the 47-bit user VA space; a free-list is a v1.x seam).
const WEAVE_VA_BASE: u64 = 0x0200_0000;

// =============================================================================
// The qid scheme (the ptyfs/netd bit-40 template).
// =============================================================================

const P_ROOT: u64 = 0; // the attach root (qid 0 reserved for it)
const P_CTL: u64 = 1; // global ctl
const P_SURF_DIR: u64 = 2; // surface/
const P_SURF_NEW: u64 = 3; // surface/new
const P_LAYOUT: u64 = 4; // the container tree (G-6)
const P_PANE_DIR: u64 = 5; // pane/

const SURF_FLAG: u64 = 1 << 40;
const PANE_FLAG: u64 = 1 << 41; // pane qids (G-6): PANE_FLAG | id<<8 | fk
const FK_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff;

const FK_DIR: u64 = 0;
const FK_CTL: u64 = 1;
const FK_WEAVE: u64 = 2;
const FK_PRESENT: u64 = 3;
const FK_EVENT: u64 = 4;
const FK_GEOMETRY: u64 = 5;

// Pane-file kinds (pane/<id>/*).
const PFK_DIR: u64 = 0;
const PFK_CTL: u64 = 1;
const PFK_MODE: u64 = 2;
const PFK_ROLE: u64 = 3;
const PFK_TAG: u64 = 4;
const PFK_SURFACE: u64 = 5;
const PFK_GEOMETRY: u64 = 6;

fn make_surf(n: usize, fk: u64) -> u64 {
    SURF_FLAG | ((n as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn surf_n(path: u64) -> usize {
    ((path >> 8) & N_MASK) as usize
}
fn surf_fk(path: u64) -> u64 {
    path & FK_MASK
}
fn is_surf(path: u64) -> bool {
    path & SURF_FLAG != 0
}

/// Pane qids name the pane's PUBLIC id (monotonic, never reused -- the
/// net-3d discipline structurally: a stale pane fid resolves to nothing).
fn make_pane(id: u32, fk: u64) -> u64 {
    PANE_FLAG | ((id as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn pane_id(path: u64) -> u32 {
    ((path >> 8) & N_MASK) as u32
}
fn pane_fk(path: u64) -> u64 {
    path & FK_MASK
}
fn is_pane(path: u64) -> bool {
    path & PANE_FLAG != 0
}

fn is_dir(path: u64) -> bool {
    path == P_ROOT
        || path == P_SURF_DIR
        || path == P_PANE_DIR
        || (is_surf(path) && surf_fk(path) == FK_DIR)
        || (is_pane(path) && pane_fk(path) == PFK_DIR)
}

// Mode constants (the ptyfs set).
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_RW: u32 = S_IFREG | 0o666;
const P9_GETATTR_SIZE: u64 = 0x200;

// =============================================================================
// The tpresent descriptor (section 18.2; 32 bytes, version-pinned).
// =============================================================================

pub const TPRESENT_LEN: usize = 32;
pub const TPRESENT_V1: u32 = 1;
pub const TPRESENT_HOLD: u32 = 1 << 0; // section 18.6 determinism -- G-6

// =============================================================================
// The tevent record (section 18.4; 24 bytes, version-pinned wire).
// =============================================================================

pub const TEVENT_LEN: usize = 24;

pub const TEV_KEY: u16 = 1;
#[allow(dead_code)] // wire vocabulary (section 18.4); the pointer/scroll
pub const TEV_PTR_MOVE: u16 = 2; // kinds arrive with the tablet device and
#[allow(dead_code)] // the CONFIGURE/FOCUS kinds with the pane layer (G-6).
pub const TEV_PTR_BTN: u16 = 3;
#[allow(dead_code)]
pub const TEV_SCROLL: u16 = 4;
pub const TEV_FRAME: u16 = 5;
#[allow(dead_code)]
pub const TEV_CONFIGURE: u16 = 6;
#[allow(dead_code)]
pub const TEV_FOCUS: u16 = 7;
// CLOSE is the queued exit REQUEST (a compositor-initiated pane close
// strands the surface + asks the client to leave); a retired surface's
// stream-END is still the event-fid EOF (poll_events' dead-surface arm +
// h_read's gone-surface arm). Request and end are distinct on purpose.
pub const TEV_CLOSE: u16 = 8;

#[derive(Clone, Copy)]
pub struct Tevent {
    pub kind: u16,
    pub code: u16,
    pub value: u32,
    pub rune: u32,
    pub mods: u16,
    pub flags: u16,
    pub tick: u64,
}

impl Tevent {
    fn encode(&self, out: &mut [u8]) {
        out[0..2].copy_from_slice(&self.kind.to_le_bytes());
        out[2..4].copy_from_slice(&self.code.to_le_bytes());
        out[4..8].copy_from_slice(&self.value.to_le_bytes());
        out[8..12].copy_from_slice(&self.rune.to_le_bytes());
        out[12..14].copy_from_slice(&self.mods.to_le_bytes());
        out[14..16].copy_from_slice(&self.flags.to_le_bytes());
        out[16..24].copy_from_slice(&self.tick.to_le_bytes());
    }
    fn coalescible(&self) -> bool {
        // R2-F4: the droppable class is exactly {FRAME, PTR_MOVE}.
        self.kind == TEV_FRAME || self.kind == TEV_PTR_MOVE
    }
}

// =============================================================================
// The surface table (the compositor's domain state).
// =============================================================================

/// The weave backing: one G-2 share-admissible DMA chunk (server-side
/// handle + mapping + the lazily-minted share registration).
struct Weave {
    handle: i64,
    va: u64,
    size: u64,
    share_id: Option<u64>, // minted at the first Tweft (armed); idempotent
}

#[derive(PartialEq, Clone, Copy)]
enum SurfState {
    Minted, // surface id allocated; no weave yet
    Woven,  // weave + resource up; no present yet
    Live,   // presents flowing
}

struct Surface {
    gen: u32,        // the slot-reuse guard (net-3d); fids capture it at bind
    owner_conn: u64, // F2: the minting conn's id
    state: SurfState,
    w: u32,
    h: u32,
    slot_stride: u64,
    /// The CURRENT weave generation (the spec's g-highest). weft_ensure,
    /// the geometry reads, and every post-fence present serve/validate
    /// against THIS one.
    weave: Option<Weave>,
    /// The CURRENT generation's GPU resource (per-generation ids minted
    /// from Comp.res_seq -- a reweave's fresh resource never aliases the
    /// old one or SCREEN_RES).
    resource_id: u32,
    /// The DISPLACED generation draining after a resize ack (weave + its
    /// resource id). At most one -- the spec's <=2-gens bound: a second
    /// ack while this drains is E_AGAIN (busy). Retired by the first
    /// post-fence present (RetireDisplaced + ServerRelease) or the
    /// surface retire.
    old_weave: Option<(Weave, u32)>,
    /// The CONFIGURE serial counter (section 18.3; low 16 bits ride the
    /// tevent `code`).
    cfg_serial: u16,
    /// The last CONFIGURE issued: (serial, w, h). The resize ack must
    /// echo exactly this; a newer emission overwrites it (only the
    /// latest offer is ackable -- the wayland serial dance).
    offered: Option<(u16, u32, u32)>,
    /// The client 2D resource's host-side content is stale (presents were
    /// composed, not transferred to it -- or the resource is a reweave's
    /// fresh mint). A deferred direct-scanout switch must expand its
    /// first transfer to the full surface (G-6).
    res_stale: bool,
    title: String,
    events: VecDeque<Tevent>,
    presents: u64, // diagnostic counter
}

/// The compositor's own screen buffer (Composed mode), attached to
/// SCREEN_RES. A WEAVE-subtype DMA chunk -- the G-2 type discipline puts
/// every RESOURCE_ATTACH_BACKING scanout backing in that class (plain
/// SYS_DMA_CREATE is the virtqueue/command class, capped at
/// KOBJ_DMA_MAX_SIZE = 1 MiB -- a display buffer does not fit and does
/// not belong). Share-admissible by TYPE but never REGISTERED
/// (t_weft_share is never called on it), so no share_id exists for a
/// client to claim -- unshared in practice. Held for the process
/// lifetime (no screen teardown path; a tapestryd death reclaims it via
/// the RW-7 crash contract).
struct Screen {
    _handle: i64,
    va: u64,
}

/// The screen resource id. Per-generation surface resources mint from
/// Comp.res_seq, which starts ABOVE this -- no id ever aliases it.
const SCREEN_RES: u32 = 0x40;

/// What scanout 0 references (G-6). `Boot` = untouched since startup (the
/// kernel test pattern stays until a first present -- the stage-0 look);
/// `Off` = explicitly disabled after content went away.
#[derive(Clone, Copy, PartialEq)]
enum Scanout {
    Boot,
    Off,
    Direct(usize),
    Composed,
}

pub struct Comp {
    pub gpu: Gpu,
    surfaces: [Option<Surface>; MAX_SURFACES],
    gen_seq: u32,
    conn_seq: u64,
    /// GPU resource ids are PER-GENERATION (a reweave mints a fresh one);
    /// pre-incremented, so the first id is SCREEN_RES + 1.
    res_seq: u32,
    /// The container tree (G-6): hosting, geometry, focus.
    layout: Layout,
    screen: Option<Screen>,
    scanout: Scanout,
    /// The F16 deferred direct switch: SET_SCANOUT to this surface's
    /// resource rides its next present-COMPLETE, never earlier.
    pending_direct: Option<usize>,
    /// The layout epoch the chrome (bg + borders) was last painted at.
    chrome_epoch: u64,
    /// The visible-geometry signature at the last STRUCTURAL repaint: a
    /// focus-only epoch bump redraws borders without blanking content
    /// (idle clients must not lose their pixels to a focus ring move).
    geom_sig: u64,
    /// The FRAME clock (section 18.4): a synthesized fixed-rate tick.
    pub tick: u64,
    pub clock_hz: u32,
    weave_va_next: u64,
}

const NO_SURFACE: Option<Surface> = None;

impl Comp {
    pub fn new(gpu: Gpu) -> Comp {
        Comp {
            gpu,
            surfaces: [NO_SURFACE; MAX_SURFACES],
            gen_seq: 0,
            conn_seq: 0,
            res_seq: SCREEN_RES,
            layout: Layout::new(),
            screen: None,
            scanout: Scanout::Boot,
            pending_direct: None,
            chrome_epoch: 0,
            geom_sig: 0,
            tick: 0,
            clock_hz: 60,
            weave_va_next: WEAVE_VA_BASE,
        }
    }

    pub fn next_conn_id(&mut self) -> u64 {
        self.conn_seq += 1;
        self.conn_seq
    }

    fn surf(&self, n: usize) -> Option<&Surface> {
        self.surfaces.get(n).and_then(|s| s.as_ref())
    }
    fn surf_mut(&mut self, n: usize) -> Option<&mut Surface> {
        self.surfaces.get_mut(n).and_then(|s| s.as_mut())
    }

    /// The F2 ownership + generation gate every surface-qid consumer runs.
    fn surf_owned(&self, n: usize, conn_id: u64, gen: u32) -> bool {
        match self.surf(n) {
            Some(s) => s.owner_conn == conn_id && s.gen == gen,
            None => false,
        }
    }

    fn owned_count(&self, conn_id: u64) -> usize {
        self.surfaces
            .iter()
            .filter(|s| s.as_ref().map_or(false, |s| s.owner_conn == conn_id))
            .count()
    }

    /// Mint a surface slot for `conn_id` (F9 caps enforced by the caller).
    fn mint(&mut self, conn_id: u64) -> Option<usize> {
        let n = self.surfaces.iter().position(|s| s.is_none())?;
        self.gen_seq = self.gen_seq.wrapping_add(1);
        self.surfaces[n] = Some(Surface {
            gen: self.gen_seq,
            owner_conn: conn_id,
            state: SurfState::Minted,
            w: 0,
            h: 0,
            slot_stride: 0,
            weave: None,
            resource_id: 0, // minted with the first generation (create)
            old_weave: None,
            res_stale: false,
            cfg_serial: 0,
            offered: None,
            title: String::new(),
            events: VecDeque::new(),
            presents: 0,
        });
        Some(n)
    }

    fn next_res_id(&mut self) -> u32 {
        self.res_seq += 1;
        self.res_seq
    }

    /// Allocate one weave GENERATION: DMA chunk + map + zero + a fresh 2D
    /// resource with the whole weave attached as backing. The shared body
    /// of the spec's WeaveFirst (create) and Reweave (resize ack).
    /// Returns (weave, slot_stride, resource_id); every failure path
    /// rolls back fully.
    fn alloc_weave(&mut self, w: u32, h: u32) -> Result<(Weave, u64, u32), u32> {
        let stride = (w as u64) * 4;
        let slot_bytes = stride * (h as u64);
        let slot_stride = (slot_bytes + PAGE - 1) & !(PAGE - 1);
        let size = slot_stride * (WEAVE_SLOTS as u64);

        // The G-2 mint: the kernel-tracked share-admissible weave subtype
        // (device-passive pixels; a plain SYS_DMA_CREATE region would be
        // structurally unshareable, R2-F1).
        let handle =
            unsafe { t_dma_create_weave(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if handle < 0 {
            say!("tapestryd: t_dma_create_weave({}) failed {}", size, handle);
            return Err(p9::E_NOMEM);
        }
        let va = self.weave_va_next;
        self.weave_va_next += (size + PAGE - 1) & !(PAGE - 1);
        let pa = unsafe { t_dma_map(handle, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        // Zero the weave: DMA chunk content must never leak a prior
        // occupant's bytes into a client mapping.
        unsafe { core::ptr::write_bytes(va as *mut u8, 0, size as usize) };

        let res = self.next_res_id();
        if self.gpu.resource_create_2d(res, w, h).is_err() {
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        if self.gpu.attach_backing(res, pa as u64, size as u32).is_err() {
            let _ = self.gpu.resource_unref(res);
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        Ok((
            Weave {
                handle,
                va,
                size,
                share_id: None,
            },
            slot_stride,
            res,
        ))
    }

    /// Tear down one weave generation's server side, in the R2-F5 order:
    /// unshare (registry-removal-before-page-free) -> the GPU resource
    /// dies before its backing -> unmap + close (serverRef -> FALSE; #847
    /// keeps the pages until the client's mapping ref drops too). The
    /// caller has already ensured no scanout references `res` (the mode
    /// machine + force-away in retire; the present-tail old drop runs
    /// after the current generation's content took the display).
    fn release_gen(&mut self, w: &Weave, res: u32) {
        if let Some(id) = w.share_id {
            let rc = unsafe { t_weft_unshare(id) };
            if rc < 0 {
                // Already claimed (consumed at Map) -- expected.
            }
        }
        let _ = self.gpu.detach_backing(res);
        let _ = self.gpu.resource_unref(res);
        unsafe { t_burrow_detach(w.va, w.size) };
        unsafe { t_close(w.handle) };
    }

    /// `create W H`: the spec's WeaveFirst -- allocate + zero the weave,
    /// create the 2D resource, attach the whole weave as its backing.
    fn create(&mut self, n: usize, w: u32, h: u32) -> Result<(), u32> {
        let (disp_w, disp_h) = (self.gpu.width, self.gpu.height);
        let s = self.surf(n).ok_or(p9::E_BADF)?;
        if s.state != SurfState::Minted {
            return Err(p9::E_EXIST); // create is once per surface
        }
        // F9: the dimension bound (a weave is tapestryd's DMA allocation).
        if w == 0 || h == 0 || w > disp_w || h > disp_h {
            return Err(p9::E_INVAL);
        }
        let (weave, slot_stride, res) = self.alloc_weave(w, h)?;

        let s = self.surf_mut(n).unwrap();
        s.w = w;
        s.h = h;
        s.slot_stride = slot_stride;
        s.weave = Some(weave);
        s.resource_id = res;
        s.state = SurfState::Woven;
        // G-6: host at create -- the focused empty leaf takes it, else the
        // focused leaf splits. A pane-table-exhausted surface stays
        // unhosted (invisible; presents complete without pixels).
        if self.layout.host(n).is_none() {
            say!("tapestryd: surface {} unhosted (pane table full)", n);
        }
        self.reconcile();
        Ok(())
    }

    /// The Tweft mint (the netd weft_ensure precedent): register the weave
    /// once, echo the stored id thereafter. `armed` becomes TRUE here; the
    /// kernel's claim consumes it (Map); retire disarms it (unshare).
    fn weft_ensure(&mut self, n: usize) -> Option<(u64, u32)> {
        let s = self.surf_mut(n)?;
        let w = s.weave.as_mut()?;
        if let Some(id) = w.share_id {
            return Some((id, w.size as u32));
        }
        let id = unsafe { t_weft_share(w.va, w.size) };
        if id <= 0 {
            say!("tapestryd: t_weft_share failed {}", id);
            return None;
        }
        w.share_id = Some(id as u64);
        Some((id as u64, w.size as u32))
    }

    /// The resize ack `resize W H <serial>` (section 18.3; the spec's
    /// Reweave). The ack is THE GENERATION FENCE: its Rwrite completes
    /// only after the new generation is fully allocated (the R2-F5
    /// reply-after-alloc precedent), and the conn stream is FIFO, so
    /// every present the client sends after reading that Rwrite
    /// validates + blits against the NEW geometry. The displaced
    /// generation drains passively (its last content stays displayed;
    /// never read again -- tearing-freedom holds) until the first
    /// post-fence present retires it.
    ///
    /// Errors: stale serial (a newer CONFIGURE superseded) -> E_AGAIN,
    /// re-ack after draining events; unknown serial / echo mismatch ->
    /// E_INVAL; prior reweave still draining -> E_AGAIN (the <=2-gens
    /// bound: present a frame, then re-ack).
    fn resize_ack(&mut self, n: usize, w: u32, h: u32, serial: u16) -> Result<(), u32> {
        let s = self.surf(n).ok_or(p9::E_BADF)?;
        if s.weave.is_none() {
            return Err(p9::E_INVAL); // no generation to reweave
        }
        let (os, ow, oh) = s.offered.ok_or(p9::E_INVAL)?;
        if serial != os {
            // u16 compare; serial spaces are tiny per surface lifetime.
            // A wrap-straddling stale reads as "unknown" -- fail-closed
            // either way (both are rejections).
            return Err(if serial < s.cfg_serial { E_AGAIN } else { p9::E_INVAL });
        }
        if w != ow || h != oh {
            return Err(p9::E_INVAL); // the ack must echo the offer
        }
        if w == s.w && h == s.h {
            // A same-size offer (the redraw request) acked: legal no-op.
            self.surf_mut(n).unwrap().offered = None;
            return Ok(());
        }
        if s.old_weave.is_some() {
            return Err(E_AGAIN); // one reweave in flight (<=2 gens)
        }

        // Reweave: mint the new generation FIRST (a failure leaves the
        // current one untouched and the offer standing for a retry).
        let (weave, slot_stride, res) = self.alloc_weave(w, h)?;
        let s = self.surf_mut(n).unwrap();
        let old = s.weave.take().unwrap();
        let old_res = s.resource_id;
        s.old_weave = Some((old, old_res));
        s.weave = Some(weave);
        s.resource_id = res;
        s.w = w;
        s.h = h;
        s.slot_stride = slot_stride;
        s.res_stale = true; // the fresh resource has no content yet
        s.offered = None;
        // Defensive (mode-machine-unreachable: Direct(n) implies the
        // surface was display-sized, which implies any outstanding offer
        // was same-size -- handled above): if scanout still names n via
        // the OLD resource, defer to the new generation's first present.
        // No set_scanout(0) -- that would blank the user's screen
        // mid-resize; the old pixels persist until the F16 flip.
        if self.scanout == Scanout::Direct(n) {
            self.scanout = Scanout::Off;
            self.pending_direct = Some(n);
        }
        // The new size feeds the scanout-mode predicate (a letterboxed
        // single leaf acking up to display size becomes Direct-eligible).
        self.reconcile();
        Ok(())
    }

    /// Allocate the compositor's screen buffer + resource (lazy; kept for
    /// the process lifetime once made).
    fn ensure_screen(&mut self) -> bool {
        if self.screen.is_some() {
            return true;
        }
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        let size = ((dw as u64) * (dh as u64) * 4 + PAGE - 1) & !(PAGE - 1);
        let handle =
            unsafe { t_dma_create_weave(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if handle < 0 {
            say!("tapestryd: screen t_dma_create_weave({}) failed {}", size, handle);
            return false;
        }
        let va = self.weave_va_next;
        self.weave_va_next += size;
        let pa = unsafe { t_dma_map(handle, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(handle) };
            return false;
        }
        if self.gpu.resource_create_2d(SCREEN_RES, dw, dh).is_err() {
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return false;
        }
        if self.gpu.attach_backing(SCREEN_RES, pa as u64, size as u32).is_err() {
            let _ = self.gpu.resource_unref(SCREEN_RES);
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return false;
        }
        self.screen = Some(Screen { _handle: handle, va });
        true
    }

    /// Paint the full chrome into the screen buffer: background everywhere
    /// (blanking pane content -- panes heal on their next present) + the
    /// border frames. Client pixels enter the screen buffer ONLY inside a
    /// present dispatch (the G-6 tearing-freedom invariant), never here.
    fn paint_chrome(&mut self) {
        let (dw, dh) = (self.gpu.width as u64, self.gpu.height as u64);
        let va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let px = va as *mut u32;
        // SAFETY: the screen buffer is dw*dh*4 bytes, mapped RW for the
        // process lifetime.
        unsafe {
            for i in 0..(dw * dh) as usize {
                *px.add(i) = pane::BG_COLOR;
            }
        }
        self.paint_borders();
        self.chrome_epoch = self.layout.epoch;
    }

    /// Redraw ONLY the 1px leaf frames (focus ring moves must not blank
    /// idle clients' content).
    fn paint_borders(&mut self) {
        let dw = self.gpu.width as u64;
        let va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let px = va as *mut u32;
        let focused = self.layout.focused;
        for (slot, _id) in self.layout.live_ids() {
            let p = match self.layout.get(slot) {
                Some(p) => p,
                None => continue,
            };
            if !p.visible || !self.layout.is_leaf(slot) || p.rect == p.content {
                continue;
            }
            let color = if slot == focused {
                pane::FOCUS_COLOR
            } else {
                pane::BORDER_COLOR
            };
            let r = p.rect;
            // SAFETY: the geometry pass bounds every visible rect inside
            // the display; the buffer covers the display.
            unsafe {
                for x in r.x..r.x + r.w {
                    *px.add((r.y as u64 * dw + x as u64) as usize) = color;
                    *px.add(((r.y + r.h - 1) as u64 * dw + x as u64) as usize) = color;
                }
                for y in r.y..r.y + r.h {
                    *px.add((y as u64 * dw + r.x as u64) as usize) = color;
                    *px.add((y as u64 * dw + (r.x + r.w - 1) as u64) as usize) = color;
                }
            }
        }
    }

    /// A signature of the visible leaf geometry (FNV-1a over id + content
    /// rects): structural relayouts change it, focus moves do not.
    fn calc_geom_sig(&self) -> u64 {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        let mut fold = |v: u64| {
            h ^= v;
            h = h.wrapping_mul(0x1_0000_01b3);
        };
        for (slot, id) in self.layout.live_ids() {
            let p = match self.layout.get(slot) {
                Some(p) => p,
                None => continue,
            };
            if !p.visible || !self.layout.is_leaf(slot) {
                continue;
            }
            fold(id as u64);
            let c = p.content;
            fold((c.x as u64) << 32 | c.y as u64);
            fold((c.w as u64) << 32 | c.h as u64);
        }
        h
    }

    /// Push the whole screen buffer to the host resource + display.
    fn screen_flush_full(&mut self) {
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        let _ = self.gpu.transfer(SCREEN_RES, 0, 0, 0, dw, dh);
        let _ = self.gpu.flush(SCREEN_RES, 0, 0, dw, dh);
    }

    /// Reconcile scanout + chrome with the layout (run after every layout
    /// or hosting mutation). The scanout MODE machine:
    ///   - exactly one visible leaf hosting a display-sized surface ->
    ///     Direct(n), switched at that surface's next present-COMPLETE
    ///     (pending_direct -- the F16 rule, uniformly);
    ///   - anything else visible (splits, letterbox, empty panes) ->
    ///     Composed (the screen resource scans out; presents blit);
    ///   - nothing at all -> Off (Boot stays untouched pre-first-content).
    fn reconcile(&mut self) {
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        self.layout.recompute(dw, dh);
        let vis = self.layout.visible_hosted();
        let nleaves = self.layout.visible_leaf_count();

        let want = if vis.is_empty() && nleaves <= 1 {
            match self.scanout {
                Scanout::Boot => Scanout::Boot,
                _ => Scanout::Off,
            }
        } else if vis.len() == 1 && nleaves == 1 {
            let n = vis[0].1;
            let full = self
                .surf(n)
                .map_or(false, |s| s.w == dw && s.h == dh);
            if full {
                Scanout::Direct(n)
            } else {
                Scanout::Composed
            }
        } else {
            Scanout::Composed
        };

        match want {
            Scanout::Boot => {}
            Scanout::Off => {
                self.pending_direct = None;
                if self.scanout != Scanout::Off && self.scanout != Scanout::Boot {
                    let _ = self.gpu.set_scanout(0, dw, dh);
                    self.scanout = Scanout::Off;
                }
            }
            Scanout::Direct(n) => {
                if self.scanout == Scanout::Direct(n) {
                    self.pending_direct = None;
                } else if self.pending_direct != Some(n) {
                    // Defer to n's next present-COMPLETE (F16). Until then
                    // the current scanout (composed frame / boot pattern)
                    // stays -- transitional content, compositor policy.
                    // The edge also emits the redraw CONFIGURE: an
                    // accumulator client's individual slots are patchwork
                    // (only the resource/screen accumulates), so the
                    // switch's full-slot transfer needs a full repaint to
                    // land next. Same-size by construction: Direct(n)
                    // requires the surface display-sized.
                    self.pending_direct = Some(n);
                    if !self.emit_configure_to(n, dw, dh) {
                        self.retire(n); // wedged; retire clears pending
                    }
                }
            }
            Scanout::Composed => {
                self.pending_direct = None;
                if !self.ensure_screen() {
                    return; // degraded: keep the current scanout; retried
                }
                let entering = self.scanout != Scanout::Composed;
                let sig = self.calc_geom_sig();
                let structural = entering || sig != self.geom_sig;
                if structural {
                    // Structural: full repaint (content blanks; panes heal
                    // by the redraw CONFIGUREs below).
                    self.paint_chrome();
                    self.geom_sig = sig;
                    self.screen_flush_full();
                } else if self.chrome_epoch != self.layout.epoch {
                    // Focus-only: redraw the frames, keep the content.
                    self.paint_borders();
                    self.chrome_epoch = self.layout.epoch;
                    self.screen_flush_full();
                }
                if entering {
                    let _ = self.gpu.set_scanout(SCREEN_RES, dw, dh);
                    self.scanout = Scanout::Composed;
                }
                if structural {
                    // The CONFIGURE fan, last (retire recursion-safe at
                    // the tail): every visible hosted surface gets its
                    // pane's CONTENT size -- same-size = the redraw
                    // request, different = the resize offer (G-6b). A
                    // client that ignores an offer keeps its size and is
                    // cropped/letterboxed by the blit clip.
                    let mut wedged: Vec<usize> = Vec::new();
                    for (_, n, c) in self.layout.visible_hosted() {
                        if !self.emit_configure_to(n, c.w, c.h) {
                            wedged.push(n);
                        }
                    }
                    for n in wedged {
                        self.retire(n);
                    }
                }
            }
        }
    }

    /// Blit a presented damage rect from the client's weave slot into the
    /// screen buffer at its pane's content rect (clipped both ways), then
    /// transfer + flush that screen region. Client weave bytes are read
    /// ONLY here, inside the present dispatch, for the slot the client
    /// just presented -- the G-6 tearing-freedom invariant.
    fn blit_composed(&mut self, n: usize, slot: u32, x: u32, y: u32, pw: u32, ph: u32) {
        let (sw, slot_stride, weave_va) = match self.surf(n) {
            Some(s) => match &s.weave {
                Some(w) => (s.w, s.slot_stride, w.va),
                None => return,
            },
            None => return,
        };
        let content = match self.layout.find_hosting(n) {
            Some(leaf) => match self.layout.get(leaf) {
                Some(p) if p.visible => p.content,
                _ => return, // hidden: no compose target
            },
            None => return, // unhosted
        };
        // Clip the damage to the pane's content size (an oversized or
        // CONFIGURE-deaf client shows its top-left crop).
        let inter = Rect { x, y, w: pw, h: ph }
            .intersect(Rect { x: 0, y: 0, w: content.w, h: content.h });
        if inter.is_empty() {
            return;
        }
        let screen_va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let dw = self.gpu.width as u64;
        let src_base = weave_va + (slot as u64) * slot_stride;
        // SAFETY: src rows lie within the weave slot (damage was validated
        // against the surface geometry; inter only shrinks it); dst rows
        // lie within the screen buffer (content is inside the display by
        // the geometry pass; inter is inside content).
        unsafe {
            for row in 0..inter.h as u64 {
                let sy = inter.y as u64 + row;
                let dy = content.y as u64 + inter.y as u64 + row;
                let src = (src_base + (sy * sw as u64 + inter.x as u64) * 4) as *const u8;
                let dst = (screen_va + (dy * dw + (content.x + inter.x) as u64) * 4) as *mut u8;
                core::ptr::copy_nonoverlapping(src, dst, inter.w as usize * 4);
            }
        }
        let gx = content.x + inter.x;
        let gy = content.y + inter.y;
        let off = ((gy as u64) * dw + gx as u64) * 4;
        let _ = self.gpu.transfer(SCREEN_RES, off, gx, gy, inter.w, inter.h);
        let _ = self.gpu.flush(SCREEN_RES, gx, gy, inter.w, inter.h);
    }

    /// The `layout` file grammar (G-6): `<verb> <pane-id> [args]` --
    /// `split <id> h|v`, `close <id>`, `focus <id>`, `mode <id> <mode>`.
    pub fn layout_cmd(&mut self, s: &str) -> Result<(), u32> {
        let s = s.trim();
        let mut it = s.splitn(3, ' ');
        let verb = it.next().ok_or(p9::E_INVAL)?;
        let id: u32 = it
            .next()
            .and_then(|t| t.trim().parse().ok())
            .ok_or(p9::E_INVAL)?;
        let rest = it.next().unwrap_or("").trim();
        let cmd = match verb {
            "split" | "mode" => {
                if rest.is_empty() {
                    return Err(p9::E_INVAL);
                }
                alloc::format!("{} {}", verb, rest)
            }
            "close" | "focus" => {
                if !rest.is_empty() {
                    return Err(p9::E_INVAL);
                }
                String::from(verb)
            }
            _ => return Err(p9::E_INVAL),
        };
        self.pane_cmd(id, &cmd)
    }

    /// One layout mutation targeting pane `id` (shared by the layout file
    /// and each pane's ctl). Every successful mutation reconciles.
    pub fn pane_cmd(&mut self, id: u32, cmd: &str) -> Result<(), u32> {
        let slot = self.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
        let cmd = cmd.trim();
        if let Some(rest) = cmd.strip_prefix("split ") {
            let mode = match rest.trim() {
                "h" => Mode::SplitH,
                "v" => Mode::SplitV,
                _ => return Err(p9::E_INVAL),
            };
            if !self.layout.is_leaf(slot) {
                return Err(p9::E_INVAL);
            }
            self.layout.split(slot, mode).ok_or(p9::E_NOMEM)?;
        } else if cmd == "close" {
            // Closing a pane strands its surfaces invisible BY DESIGN
            // (hosting is once-per-life, at create) and asks each
            // stranded client to exit via the queued TEV_CLOSE (G-6b).
            // The surface stays live until the client destroys it or its
            // conn tears down -- a compositor-initiated pane close is a
            // request, never a forced retire (the client may need to
            // save). The event is non-droppable; a wedge force-retires.
            let unhosted = self.layout.close(slot);
            for n in unhosted {
                self.send_close(n);
            }
        } else if cmd == "focus" {
            if !self.layout.focus(slot) {
                return Err(p9::E_INVAL);
            }
        } else if let Some(m) = cmd.strip_prefix("mode ") {
            let mode = Mode::parse(m.trim()).ok_or(p9::E_INVAL)?;
            if !self.layout.set_mode(slot, mode) {
                return Err(p9::E_INVAL);
            }
        } else {
            return Err(p9::E_INVAL);
        }
        self.reconcile();
        Ok(())
    }

    /// The retire (spec Destroy -> ServerRelease -> Free, server side).
    /// See the file header for the I-40 ordering this realizes.
    fn retire(&mut self, n: usize) {
        let s = match self.surfaces.get_mut(n).and_then(|s| s.take()) {
            Some(s) => s,
            None => return,
        };
        // (0) The pane side (G-6): the hosting leaf closes (single-child
        // containers collapse; the root collapses to an empty leaf). Done
        // BEFORE reconcile so the layout no longer names n.
        if let Some(leaf) = self.layout.find_hosting(n) {
            let _ = self.layout.close(leaf);
        }
        // (1) Quiesce: presents are handled synchronously (see header) --
        // the in-flight set is empty here by construction.
        if let Some(w) = &s.weave {
            // (2) Disarm BEFORE any backing free: registry-removal-before-
            // page-free (R2-F5). A consumed (claimed) share is a harmless
            // miss; an unclaimed one is removed so no Tweft claim can race
            // the retire onto a dying weave (the spec's NoStaleMap).
            if let Some(id) = w.share_id {
                let rc = unsafe { t_weft_unshare(id) };
                if rc < 0 {
                    // Already claimed (consumed at Map) -- expected.
                }
            }
            // (3) Scanout release BEFORE the resource dies: reconcile moves
            // scanout off n (the layout no longer names it). Two arms can
            // leave scanout still referencing n's resource -- a want of
            // Direct(survivor) (deferred to the survivor's present, F16)
            // and a degraded Composed entry (screen alloc failed) -- so
            // force it away explicitly in both.
            if self.pending_direct == Some(n) {
                self.pending_direct = None;
            }
            self.reconcile();
            if self.scanout == Scanout::Direct(n) {
                let (dw, dh) = (self.gpu.width, self.gpu.height);
                let _ = self.gpu.set_scanout(0, dw, dh);
                self.scanout = Scanout::Off;
            }
            // (4) The GPU resource dies before its backing.
            let _ = self.gpu.detach_backing(s.resource_id);
            let _ = self.gpu.resource_unref(s.resource_id);
            // (5) Drop the server refs: unmap our own mapping, close the
            // weave handle (serverRef -> FALSE; #847 keeps the pages until
            // the client's mapping ref drops too).
            unsafe { t_burrow_detach(w.va, w.size) };
            unsafe { t_close(w.handle) };
        }
        // A displaced generation still draining (resize acked, no present
        // yet) dies with the surface -- same per-generation order; its
        // resource was never scanned out (only a post-fence present could
        // have made it visible, and that present would have retired it).
        if let Some((oldw, old_res)) = s.old_weave {
            self.release_gen(&oldw, old_res);
        }
        say!("tapestryd: surface {} retired (presents={})", n, s.presents);
    }

    /// Retire every surface owned by a dying conn (teardown / Tversion).
    fn retire_conn(&mut self, conn_id: u64) {
        for n in 0..MAX_SURFACES {
            if self.surf(n).map_or(false, |s| s.owner_conn == conn_id) {
                self.retire(n);
            }
        }
    }

    /// Queue an event on surface `n` under the R2-F4 policy. Returns false
    /// if the push WEDGED the surface (caller must retire it).
    fn push_event(&mut self, n: usize, ev: Tevent) -> bool {
        let s = match self.surf_mut(n) {
            Some(s) => s,
            None => return true,
        };
        if ev.kind == TEV_FRAME {
            // Coalesce GLOBALLY: at most one FRAME queued per surface (the
            // G-3-audit F3 fix -- a back-of-queue-only check let interleaved
            // KEY/FRAME streams accumulate FRAMEs). Refresh the queued one's
            // tick in place; the scan is bounded by EVENT_QUEUE_CAP.
            if let Some(f) = s.events.iter_mut().find(|e| e.kind == TEV_FRAME) {
                f.tick = ev.tick;
                return true;
            }
            if s.events.len() >= EVENT_QUEUE_CAP {
                return true; // droppable class: drop the new FRAME
            }
            s.events.push_back(ev);
            return true;
        }
        if ev.kind == TEV_CONFIGURE {
            // Unacked CONFIGUREs coalesce -- only the latest serial matters
            // (section 18.3): replace a queued unread one WHOLESALE.
            if let Some(c) = s.events.iter_mut().find(|e| e.kind == TEV_CONFIGURE) {
                *c = ev;
                return true;
            }
            // Falls through to the non-droppable push below.
        }
        if s.events.len() >= EVENT_QUEUE_CAP {
            // Evict one coalescible to make room for the non-droppable.
            if let Some(i) = s.events.iter().position(|e| e.coalescible()) {
                s.events.remove(i);
            } else {
                // R2-F4: non-droppables alone fill the bounded buffer --
                // the client is dead/stalled; WEDGE (force-retire).
                say!("tapestryd: surface {} WEDGED (event overflow)", n);
                return false;
            }
        }
        s.events.push_back(ev);
        true
    }

    /// Emit the FRAME tick to every VISIBLE hosted surface (G-6: hidden
    /// surfaces -- tab-background, unhosted -- get no pacing signal; a
    /// paced client naturally suspends while hidden). Wedged surfaces
    /// retire inline.
    pub fn frame_tick(&mut self) {
        self.tick += 1;
        let t = self.tick;
        let vis: Vec<usize> = self.layout.visible_hosted().iter().map(|v| v.1).collect();
        for n in vis {
            let ev = Tevent {
                kind: TEV_FRAME,
                code: 0,
                value: 0,
                rune: 0,
                mods: 0,
                flags: 0,
                tick: t,
            };
            if !self.push_event(n, ev) {
                self.retire(n);
            }
        }
    }

    /// Emit CONFIGURE {serial, W<<16|H} to surface `n` (sections 18.3 +
    /// 18.4), recording it as the surface's ackable offer. A SAME-size
    /// CONFIGURE is the REDRAW request (a structural repaint blanks pane
    /// content; an accumulator client heals only by a full repaint); a
    /// DIFFERENT-size one is the resize offer the `resize W H <serial>`
    /// ack answers (G-6b). Coalesce-by-replacement in the queue + the
    /// single `offered` slot both encode "only the latest matters".
    /// Returns push_event's wedge verdict (false = caller must retire).
    fn emit_configure_to(&mut self, n: usize, w: u32, h: u32) -> bool {
        if w == 0 || h == 0 || w > 0xffff || h > 0xffff {
            return true; // degenerate pane: nothing showable to offer
        }
        let t = self.tick;
        let serial = match self.surf_mut(n) {
            Some(s) => {
                s.cfg_serial = s.cfg_serial.wrapping_add(1);
                s.offered = Some((s.cfg_serial, w, h));
                s.cfg_serial
            }
            None => return true,
        };
        let ev = Tevent {
            kind: TEV_CONFIGURE,
            code: serial,
            value: (w << 16) | h,
            rune: 0,
            mods: 0,
            flags: 0,
            tick: t,
        };
        self.push_event(n, ev)
    }

    /// Queue TEV_CLOSE on surface `n` (its pane closed under it -- the
    /// exit request). Wedged surfaces retire inline (R2-F4).
    fn send_close(&mut self, n: usize) {
        let ev = Tevent {
            kind: TEV_CLOSE,
            code: 0,
            value: 0,
            rune: 0,
            mods: 0,
            flags: 0,
            tick: self.tick,
        };
        if !self.push_event(n, ev) {
            self.retire(n);
        }
    }

    /// Deliver a key to the FOCUSED leaf's surface (G-6 routing).
    pub fn key_event(&mut self, code: u16, value: u32, rune: u32, mods: u16) {
        let n = match self.layout.focused_surface() {
            Some(n) => n,
            None => return, // no focused surface; input drops
        };
        let ev = Tevent {
            kind: TEV_KEY,
            code,
            value,
            rune,
            mods,
            flags: 0,
            tick: self.tick,
        };
        if !self.push_event(n, ev) {
            self.retire(n);
        }
    }

    fn live_count(&self) -> usize {
        self.surfaces.iter().filter(|s| s.is_some()).count()
    }
}

// =============================================================================
// The connection (the ptyfs Conn shape + the netd deferral).
// =============================================================================

#[derive(Clone, Copy)]
struct Fid {
    fid: u32,
    path: u64,
    gen: u32, // the surface generation captured at bind (0 for static qids)
    opened: bool,
}

enum Disp {
    Reply(usize),
    Deferred,
    Fatal,
}

#[derive(Clone, Copy)]
struct PendingRead {
    fid: u32,
    surf: usize,
    gen: u32,
    tag: u16,
    cap: usize,
}

pub struct Conn {
    handle: i64,
    pub conn_id: u64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    defer: bool,
    pending_reads: Vec<PendingRead>,
}

const NO_FID: Option<Fid> = None;

impl Conn {
    pub fn new(handle: i64, conn_id: u64) -> Conn {
        Conn {
            handle,
            conn_id,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [NO_FID; MAX_FIDS],
            in_buf: Vec::new(),
            out_buf: Vec::new(),
            defer: false,
            pending_reads: Vec::new(),
        }
    }

    // --- fid table -----------------------------------------------------------

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| f.map_or(false, |f| f.fid == fid))
    }

    fn fid_set(&mut self, fid: u32, path: u64, gen: u32) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid {
                fid,
                path,
                gen,
                opened: false,
            });
            return true;
        }
        match self.fids.iter().position(|f| f.is_none()) {
            Some(i) => {
                self.fids[i] = Some(Fid {
                    fid,
                    path,
                    gen,
                    opened: false,
                });
                true
            }
            None => false,
        }
    }

    fn fid_clunk(&mut self, fid: u32) {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = None;
        }
        // Cancel site 2 (clunk): this fid's held replies die with it.
        self.pending_reads.retain(|pr| pr.fid != fid);
    }

    fn drop_all_fids(&mut self, comp: &mut Comp) {
        // Cancel site 3 (Tversion, session reset): surfaces this conn owns
        // retire; every fid + held reply drops.
        comp.retire_conn(self.conn_id);
        self.fids = [NO_FID; MAX_FIDS];
        self.pending_reads.clear();
    }

    pub fn teardown(&mut self, comp: &mut Comp) {
        // Cancel site 1 (conn death): the owning conn's surfaces retire
        // (spec Destroy via client death); held replies die.
        comp.retire_conn(self.conn_id);
        self.pending_reads.clear();
    }

    pub fn raw_fd(&self) -> i64 {
        self.handle
    }

    // --- frame pump (the ptyfs bodies, verbatim shape) -----------------------

    pub fn service(&mut self, comp: &mut Comp) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n =
            unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            self.in_buf.truncate(cur);
            return false;
        }
        self.in_buf.truncate(cur + n as usize);

        loop {
            if self.in_buf.len() < p9::P9_HDR_LEN {
                return true;
            }
            let hdr = match p9::peek_header(&self.in_buf) {
                Ok(h) => h,
                Err(_) => return false,
            };
            let size = hdr.size as usize;
            if !(p9::P9_HDR_LEN..=SRV_MSIZE_USIZE).contains(&size) {
                return false;
            }
            if self.in_buf.len() < size {
                return true;
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            match self.dispatch(comp, &frame, hdr) {
                Disp::Fatal => return false,
                Disp::Deferred => {}
                Disp::Reply(rlen) => {
                    if !self.send_all(rlen) {
                        return false;
                    }
                }
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, comp: &mut Comp, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(comp, tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(comp, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(comp, tmsg, tag),
            p9::P9_TREAD => self.h_read(comp, tmsg, tag),
            p9::P9_TWRITE => self.h_write(comp, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(comp, tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(comp, tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            p9::P9_TWEFT => self.h_weft(comp, tmsg, tag),
            _ => self.err(tag, p9::E_NOSYS),
        };
        if self.defer {
            self.defer = false;
            return Disp::Deferred;
        }
        let len = r.unwrap_or_else(|_| {
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        });
        if len == 0 {
            Disp::Fatal
        } else {
            Disp::Reply(len)
        }
    }

    fn send_all(&mut self, rlen: usize) -> bool {
        let mut sent = 0usize;
        while sent < rlen {
            let w = unsafe {
                libthyla_rs::t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent)
            };
            if w <= 0 {
                return false;
            }
            sent += w as usize;
        }
        true
    }

    fn err(&mut self, tag: u16, code: u32) -> Result<usize, ()> {
        p9::build_rlerror(&mut self.out_buf, tag, code)
    }

    fn qid_of(&self, path: u64) -> p9::Qid {
        let kind = if is_dir(path) {
            p9::P9_QTDIR
        } else {
            p9::P9_QTFILE
        };
        p9::Qid {
            kind,
            version: 0,
            path,
        }
    }

    // --- handlers ------------------------------------------------------------

    fn h_version(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        self.drop_all_fids(comp);
        self.msize = a.msize.min(SRV_MSIZE);
        self.version_done = true;
        p9::build_rversion(&mut self.out_buf, tag, self.msize, b"9P2000.L")
    }

    fn h_attach(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        if !self.version_done {
            return self.err(tag, p9::E_PROTO);
        }
        let a = match p9::parse_tattach(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if a.afid != p9::P9_NOFID || a.fid == p9::P9_NOFID {
            return self.err(tag, p9::E_OPNOTSUPP);
        }
        if !self.fid_set(a.fid, P_ROOT, 0) {
            return self.err(tag, p9::E_NOMEM);
        }
        let q = self.qid_of(P_ROOT);
        p9::build_rattach(&mut self.out_buf, tag, &q)
    }

    /// Resolve one path component (the F2 ownership gate lives here).
    fn walk_child(&self, comp: &Comp, dir: u64, name: &[u8]) -> Option<(u64, u32)> {
        if name == b"." {
            return Some((dir, self.gen_for(comp, dir)));
        }
        match dir {
            P_ROOT => {
                if name == b".." {
                    Some((P_ROOT, 0))
                } else if name == b"ctl" {
                    Some((P_CTL, 0))
                } else if name == b"surface" {
                    Some((P_SURF_DIR, 0))
                } else if name == b"layout" {
                    Some((P_LAYOUT, 0))
                } else if name == b"pane" {
                    Some((P_PANE_DIR, 0))
                } else {
                    None
                }
            }
            P_PANE_DIR => {
                if name == b".." {
                    return Some((P_ROOT, 0));
                }
                let id = parse_u32(name)?;
                comp.layout.slot_of_id(id)?;
                Some((make_pane(id, PFK_DIR), 0))
            }
            d if is_pane(d) && pane_fk(d) == PFK_DIR => {
                let id = pane_id(d);
                comp.layout.slot_of_id(id)?;
                let fk = match name {
                    b".." => return Some((P_PANE_DIR, 0)),
                    b"ctl" => PFK_CTL,
                    b"mode" => PFK_MODE,
                    b"role" => PFK_ROLE,
                    b"tag" => PFK_TAG,
                    b"surface" => PFK_SURFACE,
                    b"geometry" => PFK_GEOMETRY,
                    _ => return None,
                };
                Some((make_pane(id, fk), 0))
            }
            P_SURF_DIR => {
                if name == b".." {
                    return Some((P_ROOT, 0));
                }
                if name == b"new" {
                    return Some((P_SURF_NEW, 0));
                }
                let n = parse_dec(name)?;
                let s = comp.surf(n)?;
                // F2: only the owning conn resolves a surface.
                if s.owner_conn != self.conn_id {
                    return None;
                }
                Some((make_surf(n, FK_DIR), s.gen))
            }
            d if is_surf(d) && surf_fk(d) == FK_DIR => {
                let n = surf_n(d);
                let s = comp.surf(n)?;
                if s.owner_conn != self.conn_id {
                    return None;
                }
                let fk = match name {
                    b".." => return Some((P_SURF_DIR, 0)),
                    b"ctl" => FK_CTL,
                    b"weave" => FK_WEAVE,
                    b"present" => FK_PRESENT,
                    b"event" => FK_EVENT,
                    b"geometry" => FK_GEOMETRY,
                    _ => return None,
                };
                Some((make_surf(n, fk), s.gen))
            }
            _ => None,
        }
    }

    fn gen_for(&self, comp: &Comp, path: u64) -> u32 {
        if is_surf(path) {
            comp.surf(surf_n(path)).map_or(0, |s| s.gen)
        } else {
            0
        }
    }

    fn h_walk(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twalk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        let mut cur = f.path;
        let mut cur_gen = f.gen;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut nwalked = 0usize;
        for k in 0..(a.nwname as usize) {
            match self.walk_child(comp, cur, a.names[k]) {
                Some((next, gen)) => {
                    cur = next;
                    cur_gen = gen;
                    qids[nwalked] = self.qid_of(next);
                    nwalked += 1;
                }
                None => break,
            }
        }
        if nwalked == a.nwname as usize {
            if !self.fid_set(a.newfid, cur, cur_gen) {
                return self.err(tag, p9::E_NOMEM);
            }
        } else if nwalked == 0 && a.nwname > 0 {
            return self.err(tag, p9::E_NOENT);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..nwalked])
    }

    fn h_lopen(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tlopen(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }

        // The mint idiom (netd clone / ptyfs ptmx): opening surface/new
        // allocates a surface in THIS conn and rebinds the fid onto its ctl.
        if f.path == P_SURF_NEW {
            if comp.owned_count(self.conn_id) >= MAX_SURFACES_PER_CONN {
                return self.err(tag, p9::E_NOMEM); // F9 per-conn cap
            }
            let conn_id = self.conn_id;
            let n = match comp.mint(conn_id) {
                Some(n) => n,
                None => return self.err(tag, p9::E_NOMEM),
            };
            let gen = comp.surf(n).unwrap().gen;
            let path = make_surf(n, FK_CTL);
            self.fids[i] = Some(Fid {
                fid: a.fid,
                path,
                gen,
                opened: true,
            });
            let q = self.qid_of(path);
            return p9::build_rlopen(&mut self.out_buf, tag, &q, 0);
        }

        // Surface files: re-validate liveness + ownership + generation (a
        // walk could have raced a retire).
        if is_surf(f.path) && !comp.surf_owned(surf_n(f.path), self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        // Pane files: re-validate liveness (ids are never reused, so a
        // stale fid can only resolve to nothing).
        if is_pane(f.path) && comp.layout.slot_of_id(pane_id(f.path)).is_none() {
            return self.err(tag, p9::E_NOENT);
        }
        self.fids[i].as_mut().unwrap().opened = true;
        let q = self.qid_of(f.path);
        p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
    }

    fn h_read(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || is_dir(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let cap = ((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4)).min(a.count as usize);

        if f.path == P_CTL {
            // `display W H` first: the line a client parses to size a
            // fullscreen create (placement stays hidden -- D5 -- but the
            // DISPLAY geometry is global, not placement).
            let mut s = String::new();
            let _ = core::fmt::write(
                &mut s,
                format_args!(
                    "display {} {}\nsurfaces {}\nclock-rate {}\ntick {}\npanes {}\nfocused {}\n",
                    comp.gpu.width,
                    comp.gpu.height,
                    comp.live_count(),
                    comp.clock_hz,
                    comp.tick,
                    comp.layout.live_ids().len(),
                    comp.layout.id_of(comp.layout.focused).unwrap_or(0)
                ),
            );
            return self.read_str(tag, &s, a.offset, cap);
        }
        if f.path == P_LAYOUT {
            // The container tree as text (G-6). Reads regenerate the
            // string; a multi-read straddling a mutation can tear -- the
            // text fits one frame at every realistic size (the stage-0
            // ctl posture).
            let s = comp.layout.render_text();
            return self.read_str(tag, &s, a.offset, cap);
        }
        if is_pane(f.path) {
            return self.pane_read(comp, f.path, tag, a.offset, cap);
        }

        if !is_surf(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        let fk = surf_fk(f.path);

        // The event stream outlives its surface by exactly one EOF: a
        // retired surface's event fid reads empty (stream end).
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            if fk == FK_EVENT {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            return self.err(tag, p9::E_NOENT);
        }

        match fk {
            FK_CTL => {
                // The netd clone idiom: the ctl read returns the surface id.
                let mut s = String::new();
                let _ = core::fmt::write(&mut s, format_args!("{}\n", n));
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_WEAVE => {
                let surf = comp.surf(n).unwrap();
                if surf.state == SurfState::Minted {
                    return self.err(tag, p9::E_INVAL);
                }
                let mut s = String::new();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!(
                        "{} {} {} {} {} b8g8r8a8\n",
                        surf.w,
                        surf.h,
                        surf.w * 4,
                        surf.slot_stride,
                        WEAVE_SLOTS
                    ),
                );
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_GEOMETRY => {
                let surf = comp.surf(n).unwrap();
                let mut s = String::new();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("0 0 {} {} 0 0\n", surf.w, surf.h),
                );
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_EVENT => {
                if cap < TEVENT_LEN {
                    // Too small for even one record: answer empty rather
                    // than park a read that could never complete.
                    return p9::build_rread(&mut self.out_buf, tag, &[]);
                }
                if let Some(len) = self.drain_events(comp, n, cap) {
                    let data: Vec<u8> = self.scratch_events(comp, n, cap, len);
                    return p9::build_rread(&mut self.out_buf, tag, &data);
                }
                // Empty: park (the netd WouldBlock leg).
                if self.pending_reads.len() >= MAX_FIDS {
                    return self.err(tag, p9::E_PROTO);
                }
                self.pending_reads.push(PendingRead {
                    fid: a.fid,
                    surf: n,
                    gen: f.gen,
                    tag,
                    cap,
                });
                self.defer = true;
                Ok(0)
            }
            _ => self.err(tag, p9::E_INVAL),
        }
    }

    /// How many whole event records are deliverable now (None = zero).
    fn drain_events(&self, comp: &Comp, n: usize, cap: usize) -> Option<usize> {
        let s = comp.surf(n)?;
        if s.events.is_empty() {
            return None;
        }
        Some((cap / TEVENT_LEN).min(s.events.len()))
    }

    fn scratch_events(&self, comp: &mut Comp, n: usize, _cap: usize, count: usize) -> Vec<u8> {
        let mut data = alloc::vec![0u8; count * TEVENT_LEN];
        if let Some(s) = comp.surf_mut(n) {
            for k in 0..count {
                let ev = s.events.pop_front().unwrap();
                ev.encode(&mut data[k * TEVENT_LEN..(k + 1) * TEVENT_LEN]);
            }
        }
        data
    }

    fn read_str(&mut self, tag: u16, s: &str, offset: u64, cap: usize) -> Result<usize, ()> {
        let b = s.as_bytes();
        let off = (offset as usize).min(b.len());
        let take = (b.len() - off).min(cap);
        let slice = b[off..off + take].to_vec();
        p9::build_rread(&mut self.out_buf, tag, &slice)
    }

    /// Pane file reads (G-6). A stale id (the pane closed) is E_NOENT --
    /// ids are never reused, so there is nothing it could alias.
    fn pane_read(
        &mut self,
        comp: &Comp,
        path: u64,
        tag: u16,
        offset: u64,
        cap: usize,
    ) -> Result<usize, ()> {
        let id = pane_id(path);
        let slot = match comp.layout.slot_of_id(id) {
            Some(s) => s,
            None => return self.err(tag, p9::E_NOENT),
        };
        let mut s = String::new();
        match pane_fk(path) {
            PFK_CTL => {
                let _ = core::fmt::write(&mut s, format_args!("{}\n", id));
            }
            PFK_MODE => {
                let name = match comp.layout.get(slot).map(|p| &p.kind) {
                    Some(pane::Kind::Container { mode, .. }) => mode.name(),
                    _ => "leaf",
                };
                let _ = core::fmt::write(&mut s, format_args!("{}\n", name));
            }
            PFK_ROLE => {
                let p = comp.layout.get(slot).unwrap();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!(
                        "{} {}\n",
                        p.role.name(),
                        if p.focusable { "focusable" } else { "nofocus" }
                    ),
                );
            }
            PFK_TAG => {
                let p = comp.layout.get(slot).unwrap();
                let _ = core::fmt::write(&mut s, format_args!("{}\n", p.tag));
            }
            PFK_SURFACE => match comp.layout.leaf_surface(slot) {
                Some(n) => {
                    let _ = core::fmt::write(&mut s, format_args!("{}\n", n));
                }
                None => s.push_str("none\n"),
            },
            PFK_GEOMETRY => {
                let c = comp.layout.get(slot).unwrap().content;
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("{} {} {} {}\n", c.x, c.y, c.w, c.h),
                );
            }
            _ => return self.err(tag, p9::E_INVAL),
        }
        self.read_str(tag, &s, offset, cap)
    }

    /// Pane file writes (G-6): the per-pane ctl carries the layout verbs
    /// with the fid's pane as the implicit target; mode/role/tag are
    /// direct field writes.
    fn pane_write(&mut self, comp: &mut Comp, path: u64, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        let id = pane_id(path);
        match pane_fk(path) {
            PFK_CTL => comp.pane_cmd(id, s),
            PFK_MODE => {
                let mode = Mode::parse(s).ok_or(p9::E_INVAL)?;
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                if !comp.layout.set_mode(slot, mode) {
                    return Err(p9::E_INVAL);
                }
                comp.reconcile();
                Ok(())
            }
            PFK_TAG => {
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                comp.layout.get_mut(slot).unwrap().tag = String::from(s);
                Ok(())
            }
            PFK_ROLE => {
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                let mut it = s.split_ascii_whitespace();
                let role = Role::parse(it.next().ok_or(p9::E_INVAL)?).ok_or(p9::E_INVAL)?;
                let focusable = match it.next() {
                    None => true,
                    Some("focusable") => true,
                    Some("nofocus") => false,
                    Some(_) => return Err(p9::E_INVAL),
                };
                if it.next().is_some() {
                    return Err(p9::E_INVAL);
                }
                let p = comp.layout.get_mut(slot).unwrap();
                p.role = role;
                p.focusable = focusable;
                Ok(())
            }
            _ => Err(p9::E_PERM),
        }
    }

    fn h_write(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || is_dir(f.path) {
            return self.err(tag, p9::E_INVAL);
        }

        if f.path == P_CTL {
            return match self.global_ctl(comp, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        if f.path == P_LAYOUT {
            return match core::str::from_utf8(a.data)
                .map_err(|_| p9::E_INVAL)
                .and_then(|s| comp.layout_cmd(s))
            {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        if is_pane(f.path) {
            return match self.pane_write(comp, f.path, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        if !is_surf(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        match surf_fk(f.path) {
            FK_CTL => match self.surface_ctl(comp, n, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            },
            FK_PRESENT => match self.present(comp, n, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            },
            _ => self.err(tag, p9::E_PERM),
        }
    }

    fn global_ctl(&mut self, comp: &mut Comp, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        if let Some(rate) = s.strip_prefix("clock-rate ") {
            let hz: u32 = rate.trim().parse().map_err(|_| p9::E_INVAL)?;
            if !(1..=240).contains(&hz) {
                return Err(p9::E_INVAL);
            }
            comp.clock_hz = hz;
            return Ok(());
        }
        if s.starts_with("test-mode") {
            // Section 18.6 determinism mode: dev/test-only, lands with the
            // golden-image capture pass (G-6). Honest reject until then.
            return Err(p9::E_OPNOTSUPP);
        }
        Err(p9::E_INVAL)
    }

    fn surface_ctl(&mut self, comp: &mut Comp, n: usize, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        if let Some(rest) = s.strip_prefix("create ") {
            let mut it = rest.split_ascii_whitespace();
            let w: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let h: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.create(n, w, h);
        }
        if s == "destroy" {
            comp.retire(n);
            return Ok(());
        }
        if let Some(t) = s.strip_prefix("title ") {
            if let Some(surf) = comp.surf_mut(n) {
                surf.title = String::from(t.trim());
            }
            return Ok(());
        }
        if let Some(rest) = s.strip_prefix("resize ") {
            // The section-18.3 resize ack: `resize W H <serial>` echoes a
            // CONFIGURE offer; a successful Rwrite IS the generation
            // fence (see resize_ack).
            let mut it = rest.split_ascii_whitespace();
            let w: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let h: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let serial: u16 =
                it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.resize_ack(n, w, h, serial);
        }
        Err(p9::E_INVAL)
    }

    /// The present engine (section 18.2): parse + validate the tpresent
    /// descriptor against the surface geometry (the untrusted-client
    /// boundary), then TRANSFER + FLUSH synchronously. The Rwrite this
    /// returns becomes the client's CQE -- the D1 recycle gate.
    fn present(&mut self, comp: &mut Comp, n: usize, data: &[u8]) -> Result<(), u32> {
        if data.len() != TPRESENT_LEN {
            return Err(p9::E_INVAL);
        }
        let word = |o: usize| u32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
        let version = word(0);
        let slot = word(4);
        let flags = word(8);
        let rect_count = word(12);
        let (rx, ry, rw, rh) = (word(16), word(20), word(24), word(28));

        if version != TPRESENT_V1 {
            return Err(p9::E_INVAL);
        }
        if flags & TPRESENT_HOLD != 0 {
            return Err(p9::E_OPNOTSUPP); // section 18.6, G-6
        }
        if rect_count > 1 {
            // Multi-rect damage rides a registered-buffer slice (D4, the
            // compositor case) -- G-6.
            return Err(p9::E_OPNOTSUPP);
        }

        let (w, h, slot_stride, res, state) = {
            let s = comp.surf(n).ok_or(p9::E_BADF)?;
            (s.w, s.h, s.slot_stride, s.resource_id, s.state)
        };
        if state == SurfState::Minted {
            return Err(p9::E_INVAL); // no weave yet
        }
        if slot >= WEAVE_SLOTS {
            return Err(p9::E_INVAL);
        }
        let (x, y, pw, ph) = if rect_count == 0 {
            (0, 0, w, h)
        } else {
            (rx, ry, rw, rh)
        };
        // Rect bounds: overflow-safe (u32 + u32 in u64).
        if pw == 0
            || ph == 0
            || (x as u64) + (pw as u64) > w as u64
            || (y as u64) + (ph as u64) > h as u64
        {
            return Err(p9::E_INVAL);
        }

        // Route by scanout mode (G-6). The slot base + rect origin ride
        // the TRANSFER offset; rows advance by the resource stride (w*4).
        if comp.pending_direct == Some(n) {
            // The deferred direct switch (F16: SET_SCANOUT only at a
            // present-COMPLETE). A stale client resource (composed-era
            // presents never transferred to it) expands this transfer to
            // the full surface first.
            let stale = comp.surf(n).map_or(false, |s| s.res_stale);
            let (tx, ty, tw, th) = if stale { (0, 0, w, h) } else { (x, y, pw, ph) };
            let offset =
                (slot as u64) * slot_stride + ((ty as u64) * (w as u64) + tx as u64) * 4;
            if comp.gpu.transfer(res, offset, tx, ty, tw, th).is_err() {
                return Err(E_IO);
            }
            if comp.gpu.flush(res, tx, ty, tw, th).is_err() {
                return Err(E_IO);
            }
            if comp.gpu.set_scanout(res, w, h).is_ok() {
                comp.scanout = Scanout::Direct(n);
                comp.pending_direct = None;
                if let Some(s) = comp.surf_mut(n) {
                    s.res_stale = false;
                }
            }
        } else if comp.scanout == Scanout::Direct(n) {
            // The stage-0 direct path, byte-identical: damage transfer +
            // flush on the client's own scanned-out resource (the
            // zero-copy fullscreen case).
            let offset =
                (slot as u64) * slot_stride + ((y as u64) * (w as u64) + x as u64) * 4;
            if comp.gpu.transfer(res, offset, x, y, pw, ph).is_err() {
                return Err(E_IO);
            }
            if comp.gpu.flush(res, x, y, pw, ph).is_err() {
                return Err(E_IO);
            }
        } else if comp.scanout == Scanout::Composed {
            // Composed: blit the damage into the screen buffer at the
            // pane's content rect (a hidden/unhosted surface skips); the
            // client resource is now stale for a future direct switch.
            comp.blit_composed(n, slot, x, y, pw, ph);
            if let Some(s) = comp.surf_mut(n) {
                s.res_stale = true;
            }
        } else {
            // Boot / Off / another surface's Direct: the present completes
            // without pixels (D1 contract kept; content heals on later
            // presents once visible).
            if let Some(s) = comp.surf_mut(n) {
                s.res_stale = true;
            }
        }

        {
            let s = comp.surf_mut(n).unwrap();
            s.presents += 1;
            if s.state == SurfState::Woven {
                s.state = SurfState::Live;
            }
        }
        // The first post-fence present retires the displaced generation
        // (the spec's RetireDisplaced + ServerRelease): the display now
        // shows current-generation content -- composed blits COPY (the
        // screen resource references no client weave) and the direct arms
        // target the current resource -- and quiesce holds by construction
        // (presents complete inside one dispatch), so the old weave's
        // server refs drop here. The client's own old mapping drains via
        // its weave-fid clunk (ClunkMap; #847 keeps the pages until then).
        if let Some((oldw, old_res)) = comp.surf_mut(n).and_then(|s| s.old_weave.take()) {
            comp.release_gen(&oldw, old_res);
            say!("tapestryd: surface {} old generation retired (res {})", n, old_res);
        }
        Ok(())
    }

    fn h_readdir(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_treaddir(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || !is_dir(f.path) {
            return self.err(tag, p9::E_NOTDIR);
        }

        // Collect the child list (name, path) for the fid's directory,
        // ownership-filtered (F2: readdir shows only the caller's surfaces).
        let mut names: Vec<(Vec<u8>, u64)> = Vec::new();
        match f.path {
            P_ROOT => {
                names.push((b"ctl".to_vec(), P_CTL));
                names.push((b"surface".to_vec(), P_SURF_DIR));
                names.push((b"layout".to_vec(), P_LAYOUT));
                names.push((b"pane".to_vec(), P_PANE_DIR));
            }
            P_PANE_DIR => {
                for (_slot, id) in comp.layout.live_ids() {
                    let mut nm = String::new();
                    let _ = core::fmt::write(&mut nm, format_args!("{}", id));
                    names.push((nm.into_bytes(), make_pane(id, PFK_DIR)));
                }
            }
            d if is_pane(d) && pane_fk(d) == PFK_DIR => {
                let id = pane_id(d);
                if comp.layout.slot_of_id(id).is_some() {
                    for (nm, fk) in [
                        (&b"ctl"[..], PFK_CTL),
                        (&b"mode"[..], PFK_MODE),
                        (&b"role"[..], PFK_ROLE),
                        (&b"tag"[..], PFK_TAG),
                        (&b"surface"[..], PFK_SURFACE),
                        (&b"geometry"[..], PFK_GEOMETRY),
                    ] {
                        names.push((nm.to_vec(), make_pane(id, fk)));
                    }
                }
            }
            P_SURF_DIR => {
                names.push((b"new".to_vec(), P_SURF_NEW));
                for n in 0..MAX_SURFACES {
                    if comp.surf(n).map_or(false, |s| s.owner_conn == self.conn_id) {
                        let mut nm = String::new();
                        let _ = core::fmt::write(&mut nm, format_args!("{}", n));
                        names.push((nm.into_bytes(), make_surf(n, FK_DIR)));
                    }
                }
            }
            d if is_surf(d) && surf_fk(d) == FK_DIR => {
                let n = surf_n(d);
                if comp.surf_owned(n, self.conn_id, f.gen) {
                    for (nm, fk) in [
                        (&b"ctl"[..], FK_CTL),
                        (&b"weave"[..], FK_WEAVE),
                        (&b"present"[..], FK_PRESENT),
                        (&b"event"[..], FK_EVENT),
                        (&b"geometry"[..], FK_GEOMETRY),
                    ] {
                        names.push((nm.to_vec(), make_surf(n, fk)));
                    }
                }
            }
            _ => {}
        }

        // The ordinal-cookie pack (the ptyfs shape): entries [offset..)
        // fit within both the request count and the reply frame budget.
        let budget = (self.msize as usize)
            .saturating_sub(p9::P9_HDR_LEN + 4)
            .min(a.count as usize);
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;
        for (nm, path) in &names {
            ord += 1;
            if ord <= a.offset {
                continue;
            }
            let need = p9::dirent_len(nm.len());
            if data.len() + need > budget {
                break;
            }
            let q = self.qid_of(*path);
            let dtype = if is_dir(*path) { p9::DT_DIR } else { p9::DT_REG };
            let mut tmp = alloc::vec![0u8; need];
            let n = p9::pack_dirent(&mut tmp, 0, &q, ord, dtype, nm)?;
            data.extend_from_slice(&tmp[..n]);
        }
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    fn h_getattr(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        let _ = comp;
        let (mode, nlink) = if is_dir(f.path) {
            (DIR_MODE, 2u64)
        } else {
            (FILE_RW, 1u64)
        };
        // The security trio (mode/uid/gid) MUST be filled: the kernel's
        // dev9p per-component X-search reads them; unfilled fails closed.
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        let q = self.qid_of(f.path);
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &q,
            mode,
            T_PRINCIPAL_SYSTEM,
            T_GID_SYSTEM,
            nlink,
            0,
        )
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if self.fid_find(a.fid).is_none() {
            return self.err(tag, p9::E_BADF);
        }
        self.fid_clunk(a.fid);
        p9::build_rclunk(&mut self.out_buf, tag)
    }

    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        // Cancel site 4 (Tflush): the held reply under oldtag dies; per 9P
        // the client reuses oldtag only after this Rflush.
        self.pending_reads.retain(|pr| pr.tag != a.oldtag);
        p9::build_rflush(&mut self.out_buf, tag)
    }

    /// The Tweft handler (the netd h_weft shape): mint-or-echo the weave's
    /// share registration. ring_entries = 0 is the WEAVE kind contract the
    /// kernel's weft_claimed_kind cross-checks (G-2).
    fn h_weft(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tweft(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || !is_surf(f.path) || surf_fk(f.path) != FK_WEAVE {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        match comp.weft_ensure(n) {
            Some((share_id, size)) => {
                p9::build_rweft(&mut self.out_buf, tag, share_id, size, 0)
            }
            None => self.err(tag, p9::E_NOMEM),
        }
    }

    // --- deferred delivery (the loop-top pass) -------------------------------

    /// Deliver held event reads whose surfaces have events (or died: EOF).
    /// False = the conn's transport failed (caller closes it).
    pub fn poll_events(&mut self, comp: &mut Comp) -> bool {
        let mut i = 0;
        while i < self.pending_reads.len() {
            let pr = self.pending_reads[i];
            let alive = comp.surf_owned(pr.surf, self.conn_id, pr.gen);
            if !alive {
                // The surface died with this read parked: EOF the stream.
                if !self.deliver_read(pr.tag, &[]) {
                    return false;
                }
                self.pending_reads.remove(i);
                continue;
            }
            match self.drain_events(comp, pr.surf, pr.cap) {
                Some(count) => {
                    let data = self.scratch_events(comp, pr.surf, pr.cap, count);
                    if !self.deliver_read(pr.tag, &data) {
                        return false;
                    }
                    self.pending_reads.remove(i);
                }
                None => i += 1,
            }
        }
        true
    }

    fn deliver_read(&mut self, tag: u16, data: &[u8]) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rread(&mut self.out_buf, tag, data) {
            Ok(len) => self.send_all(len),
            Err(_) => false,
        }
    }
}

/// POSIX EIO (the ninep constant set has no E_IO name): a GPU command
/// failure surfaces to the client as a remote I/O error.
const E_IO: u32 = 5;

/// POSIX EAGAIN: the resize-ack "not now" verdicts (stale serial; a
/// prior reweave still draining) -- the client drains events / presents
/// a frame and re-acks.
const E_AGAIN: u32 = 11;

fn parse_dec(name: &[u8]) -> Option<usize> {
    if name.is_empty() || name.len() > 3 {
        return None;
    }
    let mut v = 0usize;
    for &b in name {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as usize;
    }
    Some(v)
}

/// Pane ids are monotonic u32s (never reused) -- wider than the surface
/// index parser above.
fn parse_u32(name: &[u8]) -> Option<u32> {
    if name.is_empty() || name.len() > 8 {
        return None;
    }
    let mut v = 0u32;
    for &b in name {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u32)?;
    }
    Some(v)
}
