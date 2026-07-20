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

const SURF_FLAG: u64 = 1 << 40;
const FK_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff;

const FK_DIR: u64 = 0;
const FK_CTL: u64 = 1;
const FK_WEAVE: u64 = 2;
const FK_PRESENT: u64 = 3;
const FK_EVENT: u64 = 4;
const FK_GEOMETRY: u64 = 5;

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

fn is_dir(path: u64) -> bool {
    path == P_ROOT || path == P_SURF_DIR || (is_surf(path) && surf_fk(path) == FK_DIR)
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
#[allow(dead_code)] // Stage 0 signals a retired surface's stream-end via the
pub const TEV_CLOSE: u16 = 8; // event-fid EOF (poll_events' dead-surface arm
// + h_read's gone-surface arm); the queued-CLOSE record proper rides the
// pane layer, where compositor-initiated closes need distinguishing (G-6).

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
    weave: Option<Weave>,
    resource_id: u32,
    title: String,
    events: VecDeque<Tevent>,
    presents: u64, // diagnostic counter
}

pub struct Comp {
    pub gpu: Gpu,
    surfaces: [Option<Surface>; MAX_SURFACES],
    gen_seq: u32,
    conn_seq: u64,
    /// The surface currently bound to scanout 0 (spec: `displayed`).
    scanout_owner: Option<usize>,
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
            scanout_owner: None,
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
            resource_id: n as u32 + 1,
            title: String::new(),
            events: VecDeque::new(),
            presents: 0,
        });
        Some(n)
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

        if self.gpu.resource_create_2d(n as u32 + 1, w, h).is_err() {
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        if self.gpu.attach_backing(n as u32 + 1, pa as u64, size as u32).is_err() {
            let _ = self.gpu.resource_unref(n as u32 + 1);
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }

        let s = self.surf_mut(n).unwrap();
        s.w = w;
        s.h = h;
        s.slot_stride = slot_stride;
        s.weave = Some(Weave {
            handle,
            va,
            size,
            share_id: None,
        });
        s.state = SurfState::Woven;
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

    /// A present completed on surface `n`: the spec Complete's displayed
    /// update -- an ownerless scanout is taken by the completing surface
    /// (F16: the switch happens only after a first frame has transferred).
    fn scanout_take(&mut self, n: usize) {
        if self.scanout_owner.is_some() {
            return;
        }
        let (w, h, res) = match self.surf(n) {
            Some(s) => (s.w, s.h, s.resource_id),
            None => return,
        };
        if self.gpu.set_scanout(res, w, h).is_ok() {
            self.scanout_owner = Some(n);
        }
    }

    /// The retire (spec Destroy -> ServerRelease -> Free, server side).
    /// See the file header for the I-40 ordering this realizes.
    fn retire(&mut self, n: usize) {
        let s = match self.surfaces.get_mut(n).and_then(|s| s.take()) {
            Some(s) => s,
            None => return,
        };
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
            // (3) Scanout release before the resource dies.
            if self.scanout_owner == Some(n) {
                let (dw, dh) = (self.gpu.width, self.gpu.height);
                let _ = self.gpu.set_scanout(0, dw, dh);
                self.scanout_owner = None;
            }
            // (4) The GPU resource dies before its backing.
            let _ = self.gpu.detach_backing(s.resource_id);
            let _ = self.gpu.resource_unref(s.resource_id);
            // (5) Drop the server refs: unmap our own mapping, close the
            // weave handle (serverRef -> FALSE; #847 keeps the pages until
            // the client's mapping ref drops too).
            unsafe { t_burrow_detach(w.va, w.size) };
            unsafe { t_close(w.handle) };
        } else if self.scanout_owner == Some(n) {
            self.scanout_owner = None;
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

    /// Emit the FRAME tick to every created surface; deliver a key to the
    /// focused (scanout-owning) surface. Wedged surfaces retire inline.
    pub fn frame_tick(&mut self) {
        self.tick += 1;
        let t = self.tick;
        for n in 0..MAX_SURFACES {
            if self.surf(n).map_or(false, |s| s.state != SurfState::Minted) {
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
    }

    pub fn key_event(&mut self, code: u16, value: u32, rune: u32, mods: u16) {
        let n = match self.scanout_owner {
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
                } else {
                    None
                }
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
                    "display {} {}\nsurfaces {}\nclock-rate {}\ntick {}\n",
                    comp.gpu.width,
                    comp.gpu.height,
                    comp.live_count(),
                    comp.clock_hz,
                    comp.tick
                ),
            );
            return self.read_str(tag, &s, a.offset, cap);
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
        if s.starts_with("resize ") {
            // The reweave protocol (section 18.3) is compositor-initiated
            // and lands with the pane layer (G-6).
            return Err(p9::E_OPNOTSUPP);
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

        // The slot base + rect origin ride the TRANSFER offset; rows
        // advance by the resource stride (w*4).
        let offset = (slot as u64) * slot_stride + ((y as u64) * (w as u64) + x as u64) * 4;
        if comp.gpu.transfer(res, offset, x, y, pw, ph).is_err() {
            return Err(E_IO);
        }
        if comp.gpu.flush(res, x, y, pw, ph).is_err() {
            return Err(E_IO);
        }

        {
            let s = comp.surf_mut(n).unwrap();
            s.presents += 1;
            if s.state == SurfState::Woven {
                s.state = SurfState::Live;
            }
        }
        // The spec Complete's displayed update: an ownerless scanout is
        // taken at present-COMPLETE (F16), never before the first frame.
        comp.scanout_take(n);
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
