// paneplace -- the per-pane SESSION inline-media place server (I-47, HALCYON.md
// 14.7.2). The per-user session compositor (`session.rs`) posts ONE service,
// `/srv/halcyon-<user>`, and routes each place-request to the tile named by a
// per-pane ROUTING TOKEN carried as a path component: a pane's programs reach
// their tile by walking `<hex(token)>/place`, the address the compositor put in
// that pane's `/env/HALCYON_PLACE` (per-Proc, deep-copied at spawn, so every
// program in a pane inherits that pane's). This is the session generalization
// of the console spike's `placesrv` (single global `/srv/halcyon`,
// MAX_CONNS=1): here MANY tiles share ONE service and completions are TAGGED
// with the target leaf.
//
// FORMAT-FUZZ SURFACE (audit:hard, I-47). The thin syscall shell only: the 9P
// codec is `libthyla_rs::ninep`; the untrusted-NAME decisions (the 32-hex token
// codec + the namespace walk, fail-closed on an unknown/dead pane) live in the
// PURE, host-tested `paneroute`; the untrusted-PAYLOAD decisions
// (validate-before-allocate, the heap-safe per-image cap) live in the PURE,
// host-tested `inlineaccum::PlaceAccum`. What this file adds over `placesrv` is
// the authority and the routing:
//   1. the PEER PRINCIPAL check at accept, the authority: a connection is
//      refused unless its peer is the SESSION'S OWN USER and alive
//      (t_srv_peer), fail-closed, so no other principal places into this
//      session's panes;
//   2. the token, the routing (a path component; an unguessable u128 per pane):
//      a request lands only in the live pane it names, never in a gone one. It
//      is not a secret among one principal's panes -- any Proc of the principal
//      reads a pane's /env through /proc/<pid>/environ, as it can write that
//      pane's pts -- and those panes are one authority domain
//      (dec-2026-09-29-inline-media-one-principal).
// The independently scheduled service owns protocol, peer checks, buffers and
// the clipboard broker. The UI exchanges bounded route/budget metadata and
// moved image results; it never lends a Surface or protocol state to the owner.
// The DoS floor: MAX_CONNS bounds concurrent transfers; the per-image cap
// (`max_pixels`, the heap residual DIVIDED by MAX_CONNS -- set each loop by the
// compositor) bounds a single transfer AND, times MAX_CONNS, the aggregate
// in-flight; the per-pane stored quota (live placements + total raster bytes)
// is the tile's bounded ID-keyed raster cache. Text and rasters each get half
// its content share; the cache evicts oldest rasters, leaving readable captions.
// Admission and completion also honor the smallest live cache's pixel budget.

use crate::serviceio::NativeEndpoint;
use alloc::string::String;
use alloc::vec::Vec;
use halcyond::servicewire::{Dispatch, Handler, Interest, Stream};
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;

use halcyond::application::{Application, Context as AppContext, WriteResult};
use halcyond::controllers::Peer;
use halcyond::servicepool::{Pool, Connection, CONNECTION_SLOTS};
use libhalcyon::interaction_wire::Failure;
use halcyond::inlineaccum::{AccumStep, PlaceAccum};
use halcyond::paneroute::{self, Node, Quiet, Route, Routes};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_getuid, t_open, t_srv_accept, t_srv_peer, t_walk_create, TPollFd, TSrvPeerInfo,
    T_OPATH, T_OREAD, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL, T_POLLOUT, T_WALK_OPEN_FROM_ROOT,
};

const SRV_MSIZE: u32 = 8192;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;
const MAX_FIDS: usize = 8;
const ACTIVE_CONNECTIONS: usize = if cfg!(feature="interaction-qualification") { CONNECTION_SLOTS } else { 2 };
/// Media raster slots, independent of the32 control and4 handshake reserves.
/// The compositor divides its image residual by this count. Promotion must
/// succeed before a media request can allocate its first accumulator.
pub const MAX_CONNS: usize = 2;
const P9_VERSION: &[u8] = b"9P2000.L";
/// STATX_SIZE -- ninep exports MODE/NLINK/UID/GID but not SIZE.
const P9_GETATTR_SIZE: u64 = 0x200;

const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;

/// The per-image pixel cap CEILING -- deliberately BELOW `inlinewire::MAX_PIXELS`
/// (16 Mpx). The effective cap is DYNAMIC (`max_pixels`, set each loop by the
/// compositor from the heap residual divided by MAX_CONNS): it never exceeds
/// this ceiling and never drops below `PLACE_MIN_PIXELS`. Mirrors the console
/// spike's ceiling; a source raster over the effective cap is refused (view
/// falls back to a report), and a bigger image is `gallery`'s (uncapped) job.
pub const PLACE_MAX_PIXELS_HARD: u64 = 1024 * 1024;
/// The per-image FLOOR -- the effective cap never drops below this even under
/// heap pressure, so a small image (a 256x256 icon) always displays.
pub const PLACE_MIN_PIXELS: u64 = 64 * 1024;

fn qid_of(node: Node) -> p9::Qid {
    // Per-attach uniqueness is all a client needs (one token per attach); the
    // qid path is NOT identity-bearing -- routing is by the full u128 token
    // (`routes`), never the qid. Both 64-bit halves of the token are folded in
    // (F5) so the qid does not silently discard half the identity; the low 2
    // bits tag the class.
    let fold = |t: u128| ((t as u64) ^ ((t >> 64) as u64)) << 2;
    let (kind, path) = match node {
        Node::Root => (p9::P9_QTDIR, 0u64),
        Node::Dir(t) => (p9::P9_QTDIR, fold(t) | 1),
        Node::Place(t) => (p9::P9_QTFILE, fold(t) | 2),
        Node::Interaction(t) => (p9::P9_QTFILE, fold(t) | 3),
    };
    p9::Qid {
        kind,
        version: 0,
        path,
    }
}

fn mode_of(node: Node) -> u32 {
    match node {
        // The dirs are r-x for all so the kernel dev9p per-component X-search
        // passes; `place` is world-readable and -writable so the pane's `view`
        // may read its limit and write a raster. The gate is the peer principal
        // at accept; the token routes.
        Node::Root | Node::Dir(_) => S_IFDIR | 0o555,
        Node::Place(_) | Node::Interaction(_) => S_IFREG | 0o666,
    }
}

fn is_dir(node: Node) -> bool {
    matches!(node, Node::Root | Node::Dir(_))
}

/// A raster fully received on a pane's place channel, TAGGED with the tile leaf
/// it must inject into. The compositor drains these and calls the tile's
/// `Tile::place_image` (whose raster cache enforces the per-pane stored quota).
pub struct PaneCompletedImage {
    route: Route,
    pub id: u128,
    pub leaf: u32,
    pub w: u32,
    pub h: u32,
    pub argb: Vec<u32>,
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    node: Node,
    route: Option<Route>,
    opened: bool,
    app_id: u64,
}

enum Disp {
    Reply(usize),
    Fatal,
    Park(u64),
    Cancel(u64, usize),
}

/// The live place-path budget for one service pass (F2/F5): the per-image pixel
/// cap, the bytes the OTHER connections have already reserved, and the total
/// residual ceiling. Bundled so it threads as one argument.
#[derive(Copy, Clone)]
struct Budget {
    max_pixels: u64,
    others_reserved: usize,
    residual_bytes: u64,
    completion_slots: usize,
}

#[derive(Default)]
struct Diag {
    walk_noent: Quiet,
    unrouted: Quiet,
}

// The accepted endpoint is explicitly nonblocking before Conn exists. The
// common pump owns input/offsets; Protocol owns fids, accumulator and ONE reply.
struct Conn {
    id: Connection,
    peer: Peer,
    charged: bool,
    file: File,
    ready: i16,
    stream: Stream,
    protocol: Protocol,
}
const _: () = assert!(core::mem::size_of::<Conn>() <= 8192);
impl Conn {
    fn new(file: File, id: Connection, peer: Peer, session: Option<u64>) -> Result<Self, Error> {
        let mut protocol = Protocol::new();
        protocol.application = session.filter(|_| cfg!(feature="interaction-qualification")).map(|s| Application::new(s, peer)).transpose().map_err(|_| Error::Io)?;
        Ok(Self {
            id, peer, charged: false, file,
            ready: 0,
            stream: Stream::new(),
            protocol,
        })
    }
    fn runnable(&self) -> bool {
        self.stream.runnable() || self.protocol.pending.is_some_and(|p|
            self.stream.resume_available(p.ticket) && self.protocol.application.as_ref()
                .is_some_and(|a| a.answer_status(p.fid) != Err(Failure::Busy)))
    }
    fn events(&self) -> i16 {
        match self.stream.interest() {
            Interest::Read => T_POLLIN,
            Interest::Write => T_POLLOUT,
        }
    }
    fn service(
        &mut self,
        out: &mut Vec<PaneCompletedImage>,
        shared: &Shared,
        control: &libthyla_rs::service_worker::Control,
        other_accums: usize,
        diag: &mut Diag,
        deadline: u64,
        seat: Option<&mut crate::session_seat::Link>,
        pool: &mut Pool,
    ) -> bool {
        let fd = self.file.as_raw_fd() as i64;
        let mut reply = Reply {
            id: self.id, charged: &mut self.charged, pool,
            fd, accepted: self.peer,
            app_context: seat.map(|s| s.context(self.peer)),
            protocol: &mut self.protocol,
            out,
            shared,
            control,
            other_accums,
            diag,
        };
        if let Some(p) = reply.protocol.pending {
            if self.stream.resume_available(p.ticket) &&
                reply.protocol.application.as_ref().is_some_and(|a| a.answer_status(p.fid) != Err(Failure::Busy)) {
                if self.stream.resume_reply(p.ticket, &mut reply, |r| r.protocol.finish_pending()).is_err() { return false; }
            }
        }
        self.stream.service(
            &mut NativeEndpoint(fd),
            &mut reply,
            deadline,
        )
    }
}
struct Reply<'a> {
    id: Connection,
    charged: &'a mut bool,
    pool: &'a mut Pool,
    fd: i64,
    accepted: Peer,
    app_context: Option<AppContext<'a>>,
    protocol: &'a mut Protocol,
    out: &'a mut Vec<PaneCompletedImage>,
    shared: &'a Shared,
    control: &'a libthyla_rs::service_worker::Control,
    other_accums: usize,
    diag: &'a mut Diag,
}
impl Handler for Reply<'_> {
    fn dispatch(&mut self, frame: &[u8]) -> Result<Dispatch, ()> {
        self.dispatch_buffered(frame, 0)
    }
    fn input_allowance(&self) -> usize {
        SRV_MSIZE_USIZE.min(halcyond::servicewire::MAX_FRAME.saturating_sub(
            self.protocol.application.as_ref().map_or(0, |a| a.input_reserved())))
    }
    fn output_reserved(&self) -> usize { self.protocol.out_buf.capacity() }
    fn dispatch_buffered(&mut self, frame: &[u8], input_capacity: usize) -> Result<Dispatch, ()> {
        let fresh = sample_peer(self.fd, self.accepted.connection).ok_or(())?;
        if fresh != self.accepted { return Err(()); }
        if let Some(ctx) = self.app_context.as_mut() {
            ctx.peer = fresh; ctx.now = libthyla_rs::time::monotonic_ns() / 1_000_000;
        }
        let hdr = p9::peek_header(frame)?;
        let (routes, budget) = {
            let m = self.shared.mail.lock();
            if m.failed {
                return Err(());
            }
            (
                m.routes,
                Budget {
                    max_pixels: m.max_pixels,
                    others_reserved: self.other_accums
                        + m.completed
                            .iter()
                            .flatten()
                            .map(|i| i.argb.len() * 4)
                            .sum::<usize>(),
                    residual_bytes: m.residual,
                    completion_slots: m.completed.iter().filter(|s| s.is_none()).count(),
                },
            )
        };
        let result = self.protocol.dispatch_with(frame, hdr, self.out, &routes, budget,
            self.diag, self.app_context.as_mut(), input_capacity);
        if !*self.charged && self.protocol.class == Class::Media {
            self.pool.promote_media(self.id, libthyla_rs::time::monotonic_ns()).map_err(|_| ())?;
            *self.charged = true;
        }
        match result {
            Disp::Fatal => Err(()),
            Disp::Park(ticket) => Ok(Dispatch::Park(ticket)),
            Disp::Cancel(ticket, n) => { self.protocol.out_buf.truncate(n); Ok(Dispatch::Cancel(ticket)) }
            Disp::Reply(n) => {
                self.protocol.out_buf.truncate(n);
                // Publication precedes Rwrite: a successful client may exit or
                // publish its transcript object immediately after that reply.
                // Move payloads; no queue ever duplicates an image allocation.
                if !self.out.is_empty() {
                    let mut m = self.shared.mail.lock();
                    for img in self.out.drain(..) {
                        if !completion_route_current(&m.routes, &img) {
                            continue;
                        }
                        let slot = m.completed.iter_mut().find(|s| s.is_none()).ok_or(())?;
                        *slot = Some(img);
                    }
                    drop(m);
                    self.control.notify().map_err(|_| ())?;
                }
                Ok(Dispatch::Reply)
            }
        }
    }
    fn reply(&self) -> &[u8] {
        &self.protocol.out_buf
    }
}

fn sample_peer(fd: i64, connection: u64) -> Option<Peer> {
    let mut p = TSrvPeerInfo::default();
    if unsafe { t_srv_peer(fd, &mut p) } != 0 || p.alive != 1 { return None; }
    Some(Peer { connection, stripes: p.stripes, principal: p.principal_id, alive: true })
}
struct Applications<'a> { conns: &'a mut Vec<Conn>, pool: &'a mut Pool, routes: Routes }
impl Applications<'_> {
    fn remove(&mut self, index: usize, owner: &mut halcyond::interaction::Interaction) -> Result<(), Error> {
        let mut conn = self.conns.remove(index);
        self.pool.retire(conn.id).map_err(|_| Error::Io)?;
        if let Some(app) = conn.protocol.application.as_mut() { app.retire(owner); }
        owner.disconnect(conn.id.id());
        // Close before releasing the quota or returning to HSC acknowledgement.
        let id = conn.id; drop(conn);
        self.pool.reclaimed(id).map_err(|_| Error::Io)
    }
}
impl crate::session_seat::Applications for Applications<'_> {
    fn retire(&mut self, owner: &mut halcyond::interaction::Interaction) -> Result<(), Error> {
        for i in (0..self.conns.len()).rev() {
            if self.conns[i].protocol.class != Class::Media { self.remove(i, owner)?; }
        }
        Ok(())
    }
    fn prune(&mut self, owner: &mut halcyond::interaction::Interaction) -> Result<(), Error> {
        for i in (0..self.conns.len()).rev() {
            let c = &self.conns[i];
            let invalid = c.protocol.application.as_ref().is_some_and(|a|
                !a.valid(owner) || a.route().is_some_and(|r| !self.routes.current(r)));
            if invalid { self.remove(i, owner)?; }
        }
        Ok(())
    }
    fn complete(&mut self, done: halcyond::interaction::Completion) {
        for c in self.conns.iter_mut() {
            if let Some(a) = c.protocol.application.as_mut() { if a.complete(done) { break; } }
        }
    }
    fn decision(&mut self, owner: &mut halcyond::interaction::Interaction,
        q: libhalcyon::interaction_control::Request,
        result: Result<libhalcyon::interaction_control::Reply, Failure>, now: u64) -> Result<bool, Error> {
        let Some(i) = self.conns.iter().position(|c| c.protocol.application.as_ref()
            .is_some_and(|a| a.pending_request() == Some(q))) else { return Ok(false); };
        let c = &mut self.conns[i];
        let fresh = sample_peer(c.file.as_raw_fd() as i64, c.id.id()).unwrap_or(Peer { alive: false, ..c.peer });
        let app = c.protocol.application.as_mut().ok_or(Error::Io)?;
        if app.decision(owner, q, fresh, result, now).is_err() { self.remove(i, owner)?; return Ok(true); }
        if !c.charged && app.scope().is_some() {
            let leaf = app.route().ok_or(Error::Io)?.incarnation;
            if self.pool.promote_control(c.id, leaf, libthyla_rs::time::monotonic_ns()).is_err() {
                self.remove(i, owner)?;
            } else { c.charged = true; }
        }
        Ok(true)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Class { Handshake, Media, Control }
#[derive(Clone, Copy)]
struct PendingWrite { tag: u16, fid: u64, count: u32, ticket: u64 }
struct Protocol {
    application: Option<Application>,
    class: Class,
    next_fid: u64,
    next_park: u64,
    pending: Option<PendingWrite>,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    out_buf: Vec<u8>,
    /// The in-flight place transfer and the fid it belongs to. One at a time
    /// per connection (a second concurrent place-open is refused E_BUSY), so a
    /// hostile conn cannot hold many partial rasters.
    accum: Option<(u32, PlaceAccum)>,
}

impl Protocol {
    fn new() -> Self {
        Self {
            application: None,
            class: Class::Handshake,
            next_fid: 1,
            next_park: 1,
            pending: None,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [None; MAX_FIDS],
            out_buf: Vec::new(),
            accum: None,
        }
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    /// Bytes this connection's in-flight accumulator has reserved (0 if none).
    /// The aggregate ceiling (F2/F5) sums this over the OTHER connections.
    fn reserved(&self) -> usize {
        self.accum.as_ref().map_or(0, |(_, a)| a.reserved_bytes())
    }

    fn fid_set(&mut self, fid: u32, node: Node, route: Option<Route>) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid {
                fid,
                node,
                route,
                opened: false,
                app_id: 0,
            });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid {
                fid,
                node,
                route,
                opened: false,
                app_id: 0,
            });
            return true;
        }
        false
    }

    fn dispatch(
        &mut self,
        tmsg: &[u8],
        hdr: p9::Header,
        out: &mut Vec<PaneCompletedImage>,
        routes: &Routes,
        budget: Budget,
        diag: &mut Diag,
    ) -> Disp {
        self.dispatch_with(tmsg, hdr, out, routes, budget, diag, None, 0)
    }
    fn dispatch_with(&mut self, tmsg: &[u8], hdr: p9::Header,
        out: &mut Vec<PaneCompletedImage>, routes: &Routes, budget: Budget,
        diag: &mut Diag, mut context: Option<&mut AppContext<'_>>, input_capacity: usize) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        let capacity = if hdr.mtype == p9::P9_TREAD {
            p9::parse_tread(tmsg).map(|a| 11 + (a.count as usize).min(self.msize.saturating_sub(11) as usize)).unwrap_or(512)
        } else { 512 };
        if self.out_buf.try_reserve_exact(capacity).is_err() {
            return Disp::Fatal;
        }
        self.out_buf.resize(capacity, 0);
        if self.pending.is_some_and(|p| p.tag == tag) { return Disp::Fatal; }
        if tmsg.len() > self.msize as usize { return Disp::Fatal; }
        if let Some(answer) = self.application_dispatch(tmsg, hdr, routes, context.as_deref_mut(), input_capacity) {
            return answer;
        }
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag, routes, diag),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag, routes),
            p9::P9_TREAD => self.h_read(tmsg, tag, budget.max_pixels, routes),
            p9::P9_TWRITE => self.h_write(tmsg, tag, out, routes, budget, diag),
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag, routes),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            _ => self.err(tag, p9::E_NOSYS),
        };
        let len = r.unwrap_or_else(|_| {
            self.out_buf.clear();
            self.out_buf.resize(11, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        });
        if len == 0 {
            Disp::Fatal
        } else {
            Disp::Reply(len)
        }
    }

    fn application_dispatch(&mut self, frame: &[u8], hdr: p9::Header,
        routes: &Routes, context: Option<&mut AppContext<'_>>, input_capacity: usize) -> Option<Disp> {
        let tag = hdr.tag;
        if hdr.mtype == p9::P9_TVERSION && self.application.is_some() && self.version_done {
            // Reconnect for a new protocol session. Renegotiation must not
            // silently orphan a controller or reset incarnation watermarks.
            return Some(Disp::Fatal);
        }
        // Decide ownership before dispatch. Once an interaction fid is chosen,
        // an internal failure closes the connection; it must never fall back
        // to the legacy media parser (or acknowledge an incomplete clunk).
        if hdr.mtype != p9::P9_TFLUSH {
            let numeric = match hdr.mtype {
                p9::P9_TLOPEN => p9::parse_tlopen(frame).map(|a| a.fid),
                p9::P9_TREAD => p9::parse_tread(frame).map(|a| a.fid),
                p9::P9_TWRITE => p9::parse_twrite(frame).map(|a| a.fid),
                p9::P9_TCLUNK => p9::parse_tclunk(frame).map(|a| a.fid),
                _ => return None,
            }.ok()?;
            let f = self.fids[self.fid_find(numeric)?]?;
            if !matches!(f.node, Node::Interaction(_)) { return None; }
        } else if self.application.is_none() { return None; }
        let Some(ctx) = context else { return Some(Disp::Fatal); };
        let mut cancel = None;
        let result: Result<usize, ()> = (|| {
            if hdr.mtype == p9::P9_TFLUSH {
                let oldtag = match p9::parse_tflush(frame) { Ok(v) => v.oldtag, Err(_) => return self.err(tag, p9::E_PROTO) };
                if let Some(p) = self.pending.filter(|p| p.tag == oldtag) {
                    self.application.as_mut().ok_or(())?.cancel(p.fid, ctx.owner).map_err(|_| ())?;
                    self.pending = None; cancel = Some(p.ticket);
                }
                return p9::build_rflush(&mut self.out_buf, tag);
            }
            let numeric = match hdr.mtype {
                p9::P9_TLOPEN => p9::parse_tlopen(frame).map(|a| a.fid),
                p9::P9_TREAD => p9::parse_tread(frame).map(|a| a.fid),
                p9::P9_TWRITE => p9::parse_twrite(frame).map(|a| a.fid),
                p9::P9_TCLUNK => p9::parse_tclunk(frame).map(|a| a.fid),
                _ => return Err(()),
            }?;
            let i = self.fid_find(numeric).ok_or(())?;
            let f = self.fids[i].unwrap();
            if !matches!(f.node, Node::Interaction(_)) { return Err(()); }
            if hdr.mtype == p9::P9_TCLUNK {
                if f.app_id != 0 { self.application.as_mut().ok_or(())?.clunk(f.app_id, ctx.owner).map_err(|_| ())?; }
                if let Some(p) = self.pending.filter(|p| p.fid == f.app_id) {
                    cancel = Some(p.ticket); self.pending = None;
                }
                self.fids[i] = None;
                return p9::build_rclunk(&mut self.out_buf, tag);
            }
            if !routes.fid_current(f.node, f.route) { return self.err(tag, p9::E_NOENT); }
            if hdr.mtype == p9::P9_TLOPEN {
                if f.opened || self.class == Class::Media { return self.err(tag, p9::E_PERM); }
                let a = p9::parse_tlopen(frame)?;
                if a.flags != 2 { return self.err(tag, p9::E_INVAL); }
                let id = self.next_fid; self.next_fid = id.checked_add(1).ok_or(())?;
                if let Err(e) = self.application.as_mut().ok_or(())?.open(id, f.route.ok_or(())?) { return self.err(tag, e as u32); }
                self.class = Class::Control;
                self.fids[i] = Some(Fid { opened: true, app_id: id, ..f });
                return p9::build_rlopen(&mut self.out_buf, tag, &qid_of(f.node), 0);
            }
            if !f.opened { return self.err(tag, p9::E_BADF); }
            if hdr.mtype == p9::P9_TREAD {
                let a = p9::parse_tread(frame)?;
                let view = match self.application.as_ref().ok_or(())?.response(f.app_id, ctx.peer, ctx.owner, ctx.now) {
                    Ok(v) => v, Err(e) => return self.err(tag, e as u32),
                };
                let count = (a.count as usize).min(self.msize.saturating_sub(11) as usize);
                // Build the fixed 9P header without another payload buffer.
                let n = view.copy_range(usize::try_from(a.offset).unwrap_or(usize::MAX), &mut self.out_buf[11..11 + count]);
                self.out_buf[..4].copy_from_slice(&((11 + n) as u32).to_le_bytes());
                self.out_buf[4] = p9::P9_RREAD;
                self.out_buf[5..7].copy_from_slice(&tag.to_le_bytes());
                self.out_buf[7..11].copy_from_slice(&(n as u32).to_le_bytes());
                return Ok(11 + n);
            }
            let a = p9::parse_twrite(frame)?;
            let allowance = halcyond::servicewire::MAX_FRAME.saturating_sub(input_capacity);
            let result = self.application.as_mut().ok_or(())?.write(f.app_id, ctx.peer, a.offset, a.data,
                allowance, ctx.owner, ctx.bindings, ctx.desired, ctx.now);
            match result {
                Err(e) => self.err(tag, e as u32),
                Ok(WriteResult::Partial) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Ok(WriteResult::Answered) => match self.application.as_ref().unwrap().answer_status(f.app_id) {
                    Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count), Err(e) => {
                        self.err(tag, e as u32)
                    },
                },
                Ok(WriteResult::Pending(q)) => {
                    if self.pending.is_some() || ctx.queued.is_some() { return Err(()); }
                    *ctx.queued = Some(q);
                    let ticket = self.next_park; self.next_park = ticket.checked_add(1).ok_or(())?;
                    self.pending = Some(PendingWrite { tag, fid: f.app_id, count: a.count, ticket });
                    Ok(0)
                }
            }
        })();
        match result {
            Ok(0) => { self.out_buf.clear(); Some(Disp::Park(self.pending?.ticket)) }
            Ok(n) => Some(if let Some(ticket) = cancel { Disp::Cancel(ticket, n) } else { Disp::Reply(n) }),
            Err(()) => Some(Disp::Fatal),
        }
    }
    fn finish_pending(&mut self) -> Result<(), ()> {
        let p = self.pending.ok_or(())?;
        self.out_buf.resize(11, 0);
        let n = match self.application.as_ref().ok_or(())?.answer_status(p.fid) {
            Ok(()) => p9::build_rwrite(&mut self.out_buf, p.tag, p.count)?,
            Err(e) => p9::build_rlerror(&mut self.out_buf, p.tag, e as u32)?,
        };
        self.out_buf.truncate(n); self.pending = None; Ok(())
    }

    fn err(&mut self, tag: u16, code: u32) -> Result<usize, ()> {
        p9::build_rlerror(&mut self.out_buf, tag, code)
    }

    fn h_version(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let negotiated = a.msize.min(SRV_MSIZE);
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.accum = None;
        self.msize = negotiated;
        let ver: &[u8] = if a.version == P9_VERSION {
            self.version_done = true;
            P9_VERSION
        } else {
            self.version_done = false;
            b"unknown"
        };
        p9::build_rversion(&mut self.out_buf, tag, negotiated, ver)
    }

    fn h_attach(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        if !self.version_done {
            return self.err(tag, p9::E_PROTO);
        }
        let a = match p9::parse_tattach(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if a.afid != p9::P9_NOFID {
            return self.err(tag, p9::E_OPNOTSUPP);
        }
        if a.fid == p9::P9_NOFID || self.fid_find(a.fid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        if !self.fid_set(a.fid, Node::Root, None) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rattach(&mut self.out_buf, tag, &qid_of(Node::Root))
    }

    fn h_walk(
        &mut self,
        tmsg: &[u8],
        tag: u16,
        routes: &Routes,
        diag: &mut Diag,
    ) -> Result<usize, ()> {
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
        if a.newfid != a.fid && self.fid_find(a.newfid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        if !routes.fid_current(f.node, f.route) {
            return self.err(tag, p9::E_NOENT);
        }
        let mut cur = f.node;
        let mut pin = f.route;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut n = 0usize;
        for k in 0..(a.nwname as usize).min(p9::P9_MAX_WALK) {
            match routes.walk(cur, pin, a.names[k]) {
                Some((p, route)) => {
                    if matches!(p, Node::Interaction(_)) && self.application.is_none() { break; }
                    cur = p;
                    pin = route;
                    qids[n] = qid_of(p);
                    n += 1;
                }
                None => break,
            }
        }
        if a.nwname > 0 && n == 0 {
            if matches!(f.node, Node::Root) {
                // A root walk that resolved nothing: the first name is a token
                // not (yet) routed, or malformed (a 32-byte name is a token
                // attempt; anything else is not a place path).
                if let Some(n) = diag.walk_noent.next() {
                    let _ = n; // diagnostics must be drained by the UI, never written here
                }
            }
            return self.err(tag, p9::E_NOENT);
        }
        if n == a.nwname as usize && !self.fid_set(a.newfid, cur, pin) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..n])
    }

    fn h_lopen(&mut self, tmsg: &[u8], tag: u16, routes: &Routes) -> Result<usize, ()> {
        let a = match p9::parse_tlopen(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !routes.fid_current(f.node, f.route) {
            if self.accum.as_ref().is_some_and(|(fid, _)| *fid == f.fid) {
                self.accum = None;
            }
            return self.err(tag, p9::E_NOENT);
        }
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if matches!(f.node, Node::Place(_)) {
            if self.class == Class::Control { return self.err(tag, p9::E_PERM); }
            self.class = Class::Media;
        }
        self.fids[i] = Some(Fid {
            fid: f.fid,
            node: f.node,
            route: f.route,
            opened: true,
            app_id: f.app_id,
        });
        p9::build_rlopen(&mut self.out_buf, tag, &qid_of(f.node), 0)
    }

    fn h_read(&mut self, tmsg: &[u8], tag: u16, max_pixels: u64, routes: &Routes) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !routes.fid_current(f.node, f.route) {
            if self.accum.as_ref().is_some_and(|(fid, _)| *fid == f.fid) {
                self.accum = None;
            }
            return self.err(tag, p9::E_NOENT);
        }
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if is_dir(f.node) {
            return self.err(tag, p9::E_ISDIR);
        }
        // A read of `place` answers the per-image limit a new transfer is held
        // to now, so `view` fits its raster before the header rather than
        // learning the cap from a refusal.
        let mut text = [0u8; inlinewire::LIMIT_TEXT_MAX];
        let data = inlinewire::limit_read(max_pixels, a.offset, a.count, &mut text);
        p9::build_rread(&mut self.out_buf, tag, data)
    }

    fn h_write(
        &mut self,
        tmsg: &[u8],
        tag: u16,
        out: &mut Vec<PaneCompletedImage>,
        routes: &Routes,
        budget: Budget,
        diag: &mut Diag,
    ) -> Result<usize, ()> {
        if out.len() >= budget.completion_slots {
            return self.err(tag, p9::E_BUSY);
        }
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !routes.fid_current(f.node, f.route) {
            if self.accum.as_ref().is_some_and(|(fid, _)| *fid == f.fid) {
                self.accum = None;
            }
            return self.err(tag, p9::E_NOENT);
        }
        // Only an opened `place` file accepts writes.
        let token = match f.node {
            Node::Place(t) if f.opened => t,
            _ => return self.err(tag, p9::E_INVAL),
        };
        // Charge queued completions and the current conversion peak even on
        // a reused fid. Keeping an accumulator object cannot bypass admission
        // for the next image after an earlier completion moved into the queue.
        let current_peak = (self.reserved() as u64).saturating_mul(2);
        let next_peak = budget.max_pixels.saturating_mul(8).max(current_peak);
        if (budget.others_reserved as u64).saturating_add(next_peak) > budget.residual_bytes {
            self.accum = None;
            return self.err(tag, p9::E_NOMEM);
        }
        // One place transfer per connection at a time: a second place-fid write
        // while one is in flight is refused (bounds the held partials).
        match &mut self.accum {
            Some((afid, _)) if *afid != a.fid => return self.err(tag, p9::E_BUSY),
            Some((_, acc)) => {
                // A continuation, or a next image on a reused fid: refresh the
                // cap so the NEXT header parse uses the current display-scaled
                // cap, not the one captured when this accum was created (F2).
                acc.set_max_pixels(budget.max_pixels);
            }
            None => {
                // A new transfer: admit it only if the buffers OTHER connections
                // already reserved, plus this transfer's worst-case peak, fit
                // the current residual (F2/F5). Using the OTHER conns' ACTUAL
                // reserved bytes catches a sibling accum sized at a pre-resize
                // (larger) cap, so two transfers cannot combine to over-commit
                // the compositor heap. Fail clean (the client falls back to a
                // report), never OOM.
                let new_peak = budget.max_pixels.saturating_mul(8);
                if (budget.others_reserved as u64).saturating_add(new_peak) > budget.residual_bytes
                {
                    return self.err(tag, p9::E_NOMEM);
                }
                self.accum = Some((a.fid, PlaceAccum::new(budget.max_pixels)));
            }
        }
        let acc = &mut self.accum.as_mut().unwrap().1;
        match acc.write(a.offset, a.data) {
            AccumStep::More => p9::build_rwrite(&mut self.out_buf, tag, a.count),
            AccumStep::Done { id, w, h, argb } => {
                // Route to the live leaf the token names. A token whose tile
                // closed BETWEEN the walk and now (routes dropped it) resolves
                // to nothing: the raster is DISCARDED (the pane is gone), not
                // misrouted. The accumulator stays bound to the fid for a
                // subsequent image on the same connection (inlineaccum's
                // multi-image path), freed on clunk/teardown.
                // A split can reduce the live quota after the first header.
                // Reject before the success reply if this completed raster no
                // longer fits; an id-less legacy upload cannot be ordered in a
                // session transcript at all (the console path still accepts it).
                if id == 0 || u64::from(w) * u64::from(h) > budget.max_pixels {
                    return self.err(tag, p9::E_INVAL);
                }
                if let Some(route) = f.route.filter(|r| r.token == token && routes.current(*r)) {
                    out.push(PaneCompletedImage {
                        route,
                        id,
                        leaf: route.leaf,
                        w,
                        h,
                        argb,
                    });
                } else {
                    if let Some(n) = diag.unrouted.next() {
                        let _ = n; // diagnostics must be drained by the UI, never written here
                    }
                    return self.err(tag, p9::E_NOENT);
                }
                p9::build_rwrite(&mut self.out_buf, tag, a.count)
            }
            AccumStep::Reject => {
                // A malformed / over-cap / non-sequential write: drop the
                // partial and refuse. The client's transfer is spent.
                self.accum = None;
                self.err(tag, p9::E_INVAL)
            }
        }
    }

    fn h_getattr(&mut self, tmsg: &[u8], tag: u16, routes: &Routes) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !routes.fid_current(f.node, f.route) {
            if self.accum.as_ref().is_some_and(|(fid, _)| *fid == f.fid) {
                self.accum = None;
            }
            return self.err(tag, p9::E_NOENT);
        }
        let mode = mode_of(f.node);
        let nlink = if is_dir(f.node) { 2u64 } else { 1u64 };
        // Fill the security trio -- dev9p's per-component X-search reads it, and
        // an unfilled trio fails closed. uid/gid 0 (the session renderer serves
        // AS the user; the peer-principal gate at accept is the real check).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &qid_of(f.node),
            mode,
            0,
            0,
            nlink,
            0,
        )
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        match self.fid_find(a.fid) {
            Some(i) => {
                self.fids[i] = None;
                // Clunking the fid mid-transfer discards its partial raster.
                if matches!(&self.accum, Some((afid, _)) if *afid == a.fid) {
                    self.accum = None;
                }
                p9::build_rclunk(&mut self.out_buf, tag)
            }
            None => self.err(tag, p9::E_BADF),
        }
    }

    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        // Dispatch is ordered: a previous reply is fully written before a
        // later Tflush is dispatched. No asynchronous request remains to cancel.
        let _ = p9::parse_tflush(tmsg);
        p9::build_rflush(&mut self.out_buf, tag)
    }
}

/// Once setup may have published a listener or enabled a participant, failure
/// ends the session. It must not leave a live poster with a dead executor.
#[derive(Debug)]
pub enum PostError {
    Unavailable,
    Published(Error),
}
const _: () = assert!(core::mem::size_of::<PaneCompletedImage>() <= 128);
fn completion_route_current(routes: &Routes, image: &PaneCompletedImage) -> bool {
    image.leaf == image.route.leaf && routes.current(image.route)
}
// The desired route table is a bounded, coalesced metadata mailbox: 32 records.
// Removing a route updates this same durable state; revocations cannot fall out
// of a full queue. The executor copies metadata, never holds this lock for I/O.
struct Mailbox {
    routes: Routes,
    hosts: halcyond::hostbindings::Desired,
    #[cfg(feature="test-mode")]
    bound_count: u64,
    revision: u64,
    max_pixels: u64,
    residual: u64,
    completed: [Option<PaneCompletedImage>; MAX_CONNS],
    failed: bool,
}
struct Shared {
    user: String,
    principal: u32,
    published: alloc::sync::Arc<core::sync::atomic::AtomicBool>,
    mail: libthyla_rs::sync::Mutex<Mailbox>,
    seat: libthyla_rs::sync::Mutex<Option<crate::session_seat::Setup>>,
}
const _: () = assert!(core::mem::size_of::<Shared>() <= 16 * 1024);
pub struct PanePlaceServer {
    owner: libthyla_rs::service_worker::ServiceWorker<Shared>,
    #[cfg(feature="test-mode")]
    seen_bindings: u64,
}
impl PanePlaceServer {
    /// Media-only native fixtures do not register clipboard authority.
    pub fn post(user: &str) -> Result<Self, PostError> {
        Self::start(user, None)
    }
    pub fn post_on(user: &str, ring: &tapestry::EventRing) -> Result<Self, PostError> {
        let admission =
            tapestry::ordered::Channel::preopen(ring).map_err(|_| PostError::Unavailable)?;
        let (reservation, snapshot) =
            tapestry::seat::reserve(ring).map_err(|_| PostError::Unavailable)?;
        Self::start(
            user,
            Some(crate::session_seat::Setup {
                reservation,
                admission,
                snapshot,
            }),
        )
    }
    fn start(user: &str, seat: Option<crate::session_seat::Setup>) -> Result<Self, PostError> {
        let uid = unsafe { t_getuid() };
        if uid < 0 || user.is_empty() || user.len() > 24 {
            return Err(PostError::Unavailable);
        }
        let published = alloc::sync::Arc::new(core::sync::atomic::AtomicBool::new(false));
        let shared = Shared {
            user: String::from(user),
            principal: uid as u32,
            published: published.clone(),
            seat: libthyla_rs::sync::Mutex::new(seat),
            mail: libthyla_rs::sync::Mutex::new(Mailbox {
                routes: Routes::empty(),
                hosts: halcyond::hostbindings::Desired::empty(),
                #[cfg(feature="test-mode")]
                bound_count: 0,
                revision: 0,
                max_pixels: PLACE_MAX_PIXELS_HARD,
                residual: PLACE_MAX_PIXELS_HARD * 8 * MAX_CONNS as u64,
                completed: core::array::from_fn(|_| None),
                failed: false,
            }),
        };
        libthyla_rs::service_worker::ServiceWorker::new(shared, run_owner)
            .map(|owner| Self { owner, #[cfg(feature="test-mode")] seen_bindings: 0 })
            .map_err(|e| {
                if published.load(core::sync::atomic::Ordering::Acquire) {
                    PostError::Published(e)
                } else {
                    PostError::Unavailable
                }
            })
    }
    pub fn place_address(&self, token: u128) -> String {
        let user = &self.owner.state().unwrap().user;
        let hex = paneroute::hex32(token);
        let mut s = String::with_capacity(13 + user.len() + 32 + 6);
        s.push_str("/srv/halcyon-");
        s.push_str(user);
        s.push('/');
        for b in hex {
            s.push(b as char);
        }
        s.push_str("/place");
        s
    }
    pub fn register(&mut self, token: u128, leaf: u32) -> bool {
        let Ok(shared) = self.owner.state() else {
            return false;
        };
        {
            let mut m = shared.mail.lock();
            if !m.routes.insert(token, leaf) {
                return false;
            }
            let Some(n) = m.revision.checked_add(1) else {
                m.failed = true;
                return false;
            };
            m.revision = n;
        }
        self.owner.wake().is_ok()
    }
    /// Only the UI calls this, with metadata from its sealed child's pipe.
    pub fn bind_host(&mut self, leaf: u32, pid: u32, binding: u64) -> bool {
        let Ok(shared) = self.owner.state() else { return false; };
        {
            let mut m = shared.mail.lock();
            let routes = m.routes;
            let Some(route) = routes.leaf(leaf) else { return false; };
            if !m.hosts.announce(&routes, halcyond::hostbindings::Host { route, pid, binding }) {
                return false;
            }
        }
        self.owner.wake().is_ok()
    }
    pub fn unregister_leaf(&mut self, leaf: u32) {
        if let Ok(shared) = self.owner.state() {
            let mut m = shared.mail.lock();
            m.routes.remove_leaf(leaf);
            let routes = m.routes;
            m.hosts.retain(&routes);
            for slot in &mut m.completed {
                if slot.as_ref().is_some_and(|i| i.leaf == leaf) {
                    *slot = None;
                }
            }
            if let Some(n) = m.revision.checked_add(1) {
                m.revision = n;
            } else {
                m.failed = true;
            }
        }
        let _ = self.owner.wake();
    }
    pub fn set_budget(&mut self, per_image: u64, residual: u64) {
        let Ok(shared) = self.owner.state() else {
            return;
        };
        let mut m = shared.mail.lock();
        let max = per_image.clamp(PLACE_MIN_PIXELS, PLACE_MAX_PIXELS_HARD);
        let residual = residual.max(max * 8);
        let changed = m.max_pixels != max || m.residual != residual;
        m.max_pixels = max;
        m.residual = residual;
        drop(m);
        if changed {
            let _ = self.owner.wake();
        }
    }
    pub fn runnable(&self) -> bool {
        self.owner
            .state()
            .is_ok_and(|s| s.mail.lock().completed.iter().any(Option::is_some))
    }
    pub fn push_fds(&self, fds: &mut Vec<TPollFd>) {
        fds.push(TPollFd {
            fd: self.owner.notice_fd(),
            events: T_POLLIN,
            revents: 0,
        });
    }
    pub fn service(&mut self) -> Result<(), Error> {
        self.owner.check()?;
        #[cfg(feature="test-mode")]
        {
            let count = self.owner.state()?.mail.lock().bound_count;
            if count != self.seen_bindings {
                self.seen_bindings = count;
                // Console output can park during SAK: diagnostics belong to
                // the UI, never the independent cancellation executor.
                say!("halcyond: service-owner terminal bindings={}", count);
            }
        }
        if self.owner.state()?.mail.lock().failed {
            Err(Error::Io)
        } else {
            Ok(())
        }
    }
    pub fn take_completed(&mut self) -> Vec<PaneCompletedImage> {
        let mut out = Vec::new();
        let Ok(shared) = self.owner.state() else {
            return out;
        };
        let mut m = shared.mail.lock();
        if out.try_reserve_exact(MAX_CONNS).is_err() {
            m.failed = true;
            return out;
        }
        let routes = m.routes;
        for slot in &mut m.completed {
            if let Some(img) = slot.take() {
                if completion_route_current(&routes, &img) { out.push(img); }
            }
        }
        drop(m);
        let _ = self.owner.wake();
        out
    }
}
fn run_owner(shared: &Shared, control: &libthyla_rs::service_worker::Control) -> Result<(), Error> {
    let setup = shared.seat.lock().take();
    let participated = setup.is_some();
    let result = serve_owner(shared, control, setup);
    // A stopped or failed posted service cannot leave a dead resident name.
    // Shutdown occurs at process/session exit; no console write can precede it
    // on failure because ordinary console output parks during trusted input.
    if result.is_err()
        && (participated || shared.published.load(core::sync::atomic::Ordering::Acquire))
    {
        unsafe {
            libthyla_rs::t_exit_group(1);
        }
    }
    result
}
fn serve_owner(
    shared: &Shared,
    control: &libthyla_rs::service_worker::Control,
    setup: Option<crate::session_seat::Setup>,
) -> Result<(), Error> {
    let mut seat = match setup {
        Some(s) => Some(crate::session_seat::Link::new(s, shared.principal)?),
        None => None,
    };
    let deadline = libthyla_rs::time::monotonic_ns() + 4_000_000_000;
    if let Some(seat) = seat.as_mut() {
        while !seat.ready() {
            if control.stopping() || libthyla_rs::time::monotonic_ns() >= deadline {
                return Err(Error::TimedOut);
            }
            seat.pump()?;
            let mut p = [
                TPollFd { fd: seat.fd(), events: T_POLLIN, revents: 0 },
                TPollFd { fd: seat.admission_fd(), events: T_POLLIN, revents: 0 },
            ];
            unsafe { libthyla_rs::t_poll(p.as_mut_ptr(), p.len(), 10); }
        }
    }
    let mut conns = Vec::new();
    let mut pool = Pool::new();
    conns
        .try_reserve_exact(ACTIVE_CONNECTIONS)
        .map_err(|_| Error::NoMemory)?;
    let mut completed = Vec::new();
    completed
        .try_reserve_exact(MAX_CONNS)
        .map_err(|_| Error::NoMemory)?;
    let name = alloc::format!("halcyon-{}", shared.user);
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if root < 0 {
        return Err(Error::Io);
    }
    let fd = unsafe { t_walk_create(root, name.as_ptr(), name.len(), T_OREAD, 0) };
    unsafe {
        t_close(root);
    }
    if fd < 0 {
        return Err(Error::Io);
    }
    let listener = unsafe { File::from_raw_fd(fd as i32, Rights::READ) };
    // Publication has happened; caller error paths now end the posting Proc.
    // This flag is independent of mailbox revision and cannot be coalesced.
    shared
        .published
        .store(true, core::sync::atomic::Ordering::Release);
    libthyla_rs::service_worker::qualify_published()?;
    control.ready()?;
    let mut diag = Diag::default();
    let mut stopping = false;
    loop {
        control.drain_wake()?;
        if control.stopping() {
            stopping = true;
            if let Some(s) = seat.as_mut() {
                s.stop();
            } else {
                return Ok(());
            }
        }
        if let Some(s) = seat.as_mut() {
            // Copy under the mailbox lock; release before any control I/O.
            let (routes, hosts) = { let m = shared.mail.lock(); (m.routes, m.hosts) };
            s.routes(routes, hosts);
            s.pump_with(&mut Applications { conns: &mut conns, pool: &mut pool, routes })?;
            #[cfg(feature="test-mode")]
            {
                let changed = {
                    let mut m = shared.mail.lock();
                    let changed = m.bound_count != s.bound_count();
                    m.bound_count = s.bound_count();
                    changed
                };
                if changed { control.notify()?; }
            }
            if stopping && s.retired() {
                return Ok(());
            }
        }
        while let Some(id) = pool.expire_one(libthyla_rs::time::monotonic_ns()) {
            if let Some(i) = conns.iter().position(|c| c.id == id) {
                if let Some(s) = seat.as_mut() { s.disconnect(id.id()); }
                drop(conns.remove(i));
            }
            pool.reclaimed(id).map_err(|_| Error::Io)?;
        }
        if shared.mail.lock().failed {
            return Err(Error::Io);
        }
        if !stopping {
            if !conns.is_empty() {
                conns.rotate_left(1);
            }
            let deadline = libthyla_rs::time::monotonic_ns() + 2_000_000;
            let mut i = conns.len();
            while i > 0 {
                i -= 1;
                let c: &Conn = &conns[i];
                if c.ready == 0 && !c.runnable() {
                    continue;
                }
                let others = conns
                    .iter()
                    .enumerate()
                    .filter(|(j, _)| *j != i)
                    .map(|(_, c)| c.protocol.reserved())
                    .sum::<usize>();
                let close = conns[i].ready & (T_POLLHUP | T_POLLERR | T_POLLNVAL) != 0
                    || !conns[i].service(
                        &mut completed,
                        shared,
                        control,
                        others,
                        &mut diag,
                        deadline,
                        seat.as_mut(),
                        &mut pool,
                    );
                if close {
                    let id = conns[i].id;
                    pool.retire(id).map_err(|_| Error::Io)?;
                    if let Some(s) = seat.as_mut() { s.disconnect(id.id()); }
                    drop(conns.remove(i));
                    pool.reclaimed(id).map_err(|_| Error::Io)?;
                } else {
                    conns[i].ready = 0;
                }
            }
        }
        let mut poll = [TPollFd::default(); CONNECTION_SLOTS + 4];
        poll[0] = TPollFd {
            fd: control.stop_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        let mut n = 1;
        let seat_index = seat.as_ref().map(|s| {
            let i = n;
            poll[i] = TPollFd {
                fd: s.fd(),
                events: T_POLLIN,
                revents: 0,
            };
            n += 1;
            i
        });
        let admission_index = seat.as_ref().map(|s| {
            let i = n;
            poll[i] = TPollFd {
                fd: s.admission_fd(),
                events: T_POLLIN,
                revents: 0,
            };
            n += 1;
            i
        });
        // A full service must still accept-and-close excess endpoints. Omitting
        // the listener would strand their kernel 9P attach until a live peer
        // disconnects. One accept per pass keeps rejection work bounded.
        let listener_index = if !stopping {
            let i = n;
            poll[i] = TPollFd {
                fd: listener.as_raw_fd(),
                events: T_POLLIN,
                revents: 0,
            };
            n += 1;
            Some(i)
        } else {
            None
        };
        let base = n;
        if !stopping {
            for c in &conns {
                poll[n] = TPollFd {
                    fd: c.file.as_raw_fd(),
                    events: c.events(),
                    revents: 0,
                };
                n += 1;
            }
        }
        let timeout = if seat.as_ref().is_some_and(|s| s.runnable())
            || (!stopping && conns.iter().any(|c| c.runnable()))
        {
            0
        } else {
            let deadline = [pool.deadline(), seat.as_ref().and_then(|s| s.deadline())]
                .into_iter().flatten().min();
            halcyond::servicepool::poll_timeout(libthyla_rs::time::monotonic_ns(), deadline)
        };
        Error::from_syscall_return(unsafe { libthyla_rs::t_poll(poll.as_mut_ptr(), n, timeout) })?;
        for i in [seat_index, admission_index, listener_index]
            .into_iter()
            .flatten()
        {
            if poll[i].revents & (T_POLLHUP | T_POLLERR | T_POLLNVAL) != 0 {
                return Err(Error::Io);
            }
        }
        if !stopping {
            for (i, c) in conns.iter_mut().enumerate() {
                c.ready |= poll[base + i].revents;
            }
        }
        if listener_index.is_some_and(|i| poll[i].revents & T_POLLIN != 0) {
            let fd = unsafe { t_srv_accept(listener.as_raw_fd() as i64) };
            if fd >= 0 {
                let file = unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) };
                let mut peer = TSrvPeerInfo::default();
                // The File drops here on saturation, before Conn or protocol buffers
                // are allocated. Existing peers retain their slots unchanged.
                if conns.len() < ACTIVE_CONNECTIONS
                    && unsafe { libthyla_rs::t_set_nonblock(fd, true) } == 0
                    && unsafe { t_srv_peer(fd, &mut peer) } == 0
                    && peer.alive == 1
                    && peer.principal_id == shared.principal
                {
                    if let Ok(id) = pool.accept(peer.stripes, libthyla_rs::time::monotonic_ns()) {
                        let observed = Peer { connection: id.id(), stripes: peer.stripes,
                            principal: peer.principal_id, alive: true };
                        match Conn::new(file, id, observed, seat.as_ref().map(|s| s.session())) {
                            Ok(c) => conns.push(c),
                            Err(_) => { pool.retire(id).map_err(|_| Error::Io)?; pool.reclaimed(id).map_err(|_| Error::Io)?; }
                        }
                    }
                }
            } else if fd != -11 {
                return Err(Error::Io);
            }
        }
    }
}
