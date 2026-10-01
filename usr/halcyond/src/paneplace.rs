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
// The DoS floor: MAX_CONNS bounds concurrent transfers; the per-image cap
// (`max_pixels`, the heap residual DIVIDED by MAX_CONNS -- set each loop by the
// compositor) bounds a single transfer AND, times MAX_CONNS, the aggregate
// in-flight; the per-pane stored quota (live placements + total raster bytes)
// is the tile's bounded ID-keyed raster cache. Text and rasters each get half
// its content share; the cache evicts oldest rasters, leaving readable captions.
// Admission and completion also honor the smallest live cache's pixel budget.

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use halcyond::servicewire::{Handler, Interest, Stream};
use crate::serviceio::NativeEndpoint;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;
use libthyla_rs::poll::AsFd;
use libthyla_rs::poll_worker::{PollWorker, WatchId};

use halcyond::inlineaccum::{AccumStep, PlaceAccum};
use halcyond::paneroute::{self, Node, Quiet};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_getuid, t_open, t_srv_accept, t_srv_peer, t_walk_create, TPollFd,
    TSrvPeerInfo, T_OPATH, T_OREAD, T_POLLHUP, T_POLLIN, T_POLLOUT, T_POLLERR, T_POLLNVAL, T_WALK_OPEN_FROM_ROOT,
};

const SRV_MSIZE: u32 = halcyond::servicewire::MAX_FRAME as u32;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;
const MAX_FIDS: usize = 8;
/// Concurrent connections the session service accepts. Unlike the console spike
/// (MAX_CONNS=1, one `view` at a time), a session has many tiles that may each
/// run `view`, so more than one transfer can be in flight. It stays SMALL
/// because the aggregate in-flight memory is bounded STATICALLY as
/// `MAX_CONNS * per-image-peak`: the compositor sets the per-image cap to the
/// heap residual DIVIDED by MAX_CONNS (see `session.rs place_cap`), so the sum
/// of all in-flight transfers never exceeds the residual regardless of the
/// display scale -- no per-write byte accounting, and `inlineaccum` is untouched.
/// A connection beyond this WAITS (the listener drops from the poll set while
/// full), bounded acceptance, never a spin -- the console spike's model.
/// Public so the compositor sizes the per-image cap as residual / MAX_CONNS.
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
        Node::Place(_) => S_IFREG | 0o666,
    }
}

fn is_dir(node: Node) -> bool {
    matches!(node, Node::Root | Node::Dir(_))
}

/// A raster fully received on a pane's place channel, TAGGED with the tile leaf
/// it must inject into. The compositor drains these and calls the tile's
/// `Tile::place_image` (whose raster cache enforces the per-pane stored quota).
pub struct PaneCompletedImage {
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
    opened: bool,
}

enum Disp {
    Reply(usize),
    Fatal,
}

/// The live place-path budget for one service pass (F2/F5): the per-image pixel
/// cap, the bytes the OTHER connections have already reserved, and the total
/// residual ceiling. Bundled so it threads as one argument.
#[derive(Copy, Clone)]
struct Budget {
    max_pixels: u64,
    others_reserved: usize,
    residual_bytes: u64,
}

#[derive(Default)]
struct Diag {
    accepted: Quiet,
    refused: Quiet,
    walk_noent: Quiet,
    unrouted: Quiet,
}

// The accepted endpoint is explicitly nonblocking before Conn exists. The
// common pump owns input/offsets; Protocol owns fids, accumulator and ONE reply.
struct Conn {
    watch: WatchId,
    ready: i16,
    stream: Stream,
    protocol: Protocol,
}
impl Conn {
    fn new(watch: WatchId) -> Self {
        Self { watch, ready: 0, stream: Stream::new(), protocol: Protocol::new() }
    }
    fn events(&self) -> i16 {
        match self.stream.interest() { Interest::Read => T_POLLIN, Interest::Write => T_POLLOUT }
    }
    fn service(&mut self, worker: &mut PollWorker, out: &mut Vec<PaneCompletedImage>, routes: &BTreeMap<u128, u32>, budget: Budget, diag: &mut Diag, deadline: u64) -> Result<bool, Error> {
        let mut reply = Reply { protocol: &mut self.protocol, out, routes, budget, diag };
        worker.with_fd(self.watch, |fd| self.stream.service(&mut NativeEndpoint(fd as i64), &mut reply, deadline))
    }
}
struct Reply<'a> {
    protocol: &'a mut Protocol,
    out: &'a mut Vec<PaneCompletedImage>,
    routes: &'a BTreeMap<u128, u32>,
    budget: Budget,
    diag: &'a mut Diag,
}
impl Handler for Reply<'_> {
    fn dispatch(&mut self, frame: &[u8]) -> Result<(), ()> {
        let hdr = p9::peek_header(frame)?;
        match self.protocol.dispatch(frame, hdr, self.out, self.routes, self.budget, self.diag) {
            Disp::Fatal => Err(()),
            Disp::Reply(n) => { self.protocol.out_buf.truncate(n); Ok(()) }
        }
    }
    fn reply(&self) -> &[u8] { &self.protocol.out_buf }
}

struct Protocol {
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

    fn fid_set(&mut self, fid: u32, node: Node) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid {
                fid,
                node,
                opened: false,
            });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid {
                fid,
                node,
                opened: false,
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
        routes: &BTreeMap<u128, u32>,
        budget: Budget,
        diag: &mut Diag,
    ) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        if self.out_buf.try_reserve_exact(SRV_MSIZE_USIZE).is_err() { return Disp::Fatal; }
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag, routes, diag),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag),
            p9::P9_TREAD => self.h_read(tmsg, tag, budget.max_pixels),
            p9::P9_TWRITE => self.h_write(tmsg, tag, out, routes, budget, diag),
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            _ => self.err(tag, p9::E_NOSYS),
        };
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
        if !self.fid_set(a.fid, Node::Root) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rattach(&mut self.out_buf, tag, &qid_of(Node::Root))
    }

    fn h_walk(&mut self, tmsg: &[u8], tag: u16, routes: &BTreeMap<u128, u32>,
        diag: &mut Diag) -> Result<usize, ()> {
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
        let mut cur = f.node;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut n = 0usize;
        for k in 0..(a.nwname as usize).min(p9::P9_MAX_WALK) {
            match paneroute::walk_child(cur, a.names[k], |t| routes.contains_key(&t)) {
                Some(p) => {
                    cur = p;
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
                    say!(
                        "halcyond: place walk NOENT at root (nwname={} first={} bytes; {} so far)",
                        a.nwname,
                        a.names[0].len(),
                        n
                    );
                }
            }
            return self.err(tag, p9::E_NOENT);
        }
        if n == a.nwname as usize && !self.fid_set(a.newfid, cur) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..n])
    }

    fn h_lopen(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        self.fids[i] = Some(Fid {
            fid: f.fid,
            node: f.node,
            opened: true,
        });
        p9::build_rlopen(&mut self.out_buf, tag, &qid_of(f.node), 0)
    }

    fn h_read(&mut self, tmsg: &[u8], tag: u16, max_pixels: u64) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
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
        routes: &BTreeMap<u128, u32>,
        budget: Budget,
        diag: &mut Diag,
    ) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        // Only an opened `place` file accepts writes.
        let token = match f.node {
            Node::Place(t) if f.opened => t,
            _ => return self.err(tag, p9::E_INVAL),
        };
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
                if (budget.others_reserved as u64).saturating_add(new_peak) > budget.residual_bytes {
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
                if let Some(&leaf) = routes.get(&token) {
                    out.push(PaneCompletedImage { id, leaf, w, h, argb });
                } else {
                    if let Some(n) = diag.unrouted.next() {
                        say!(
                            "halcyond: place completed {}x{} but its token is not routed (tile gone; {} so far)",
                            w,
                            h,
                            n
                        );
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

    fn h_getattr(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
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

/// Once POST succeeds, failure must end the posting process: closing the
/// listener does not unpost its name. The session caller handles this explicitly.
#[derive(Debug)]
pub enum PostError {
    Unavailable,
    Published(Error),
}

/// The per-user session place server. PollWorker owns all watched handles;
/// token routes, peer checks and completed rasters stay on the UI thread.
pub struct PanePlaceServer {
    worker: PollWorker,
    listener: WatchId,
    listener_armed: bool,
    listener_ready: bool,
    conns: Vec<Conn>,
    completed: Vec<PaneCompletedImage>,
    /// token -> live tile leaf. The compositor keeps this current
    /// (`register`/`unregister_leaf`); a walk resolves a token ONLY while it is
    /// present, so a closed tile's token fails closed (E_NOENT).
    routes: BTreeMap<u128, u32>,
    /// The session's own principal (from t_getuid at post). A connection whose
    /// peer principal differs is refused at accept -- the authority gate; tokens route.
    principal: u32,
    /// The session user, for the `/srv/halcyon-<user>/...` addresses this
    /// server hands panes (`place_address`). Owned so the compositor need not
    /// thread it alongside the server.
    user: String,
    /// The current per-image pixel cap (heap residual / (8*MAX_CONNS)), set each
    /// loop by the compositor.
    max_pixels: u64,
    /// The current total place-path residual in BYTES (the heap left after the
    /// scrollback budget + baseline + the display-scaled atlas). The live
    /// AGGREGATE bound (F2/F5): a new transfer is admitted only if the buffers
    /// already reserved by OTHER connections plus this transfer's worst-case
    /// peak fit here -- so two accums, one sized at a pre-resize (larger) cap,
    /// cannot combine to over-commit the heap.
    residual_bytes: u64,
    diag: Diag,
}

impl PanePlaceServer {
    /// Post `/srv/halcyon-<user>` (9P-mode; perm 0). Requires
    /// MAY_POST_SERVICE (login grants the session compositor the bit, one hop).
    /// Unavailable means no name was published; media may stay unavailable.
    /// Published means the caller must exit so the registry cannot retain a
    /// dead endpoint. Start the worker and preallocate connection metadata
    /// before POST so ordinary allocation/startup failures publish nothing.
    pub fn post(user: &str) -> Result<PanePlaceServer, PostError> {
        let mut worker = PollWorker::new(MAX_CONNS + 1).map_err(|_| PostError::Unavailable)?;
        let mut conns = Vec::new();
        conns.try_reserve_exact(MAX_CONNS).map_err(|_| PostError::Unavailable)?;
        let uid = unsafe { t_getuid() };
        if uid < 0 {
            return Err(PostError::Unavailable); // fail-closed: without a principal the accept gate cannot hold.
        }
        let mut name = String::with_capacity(8 + user.len());
        name.push_str("halcyon-");
        name.push_str(user);
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
        if srv < 0 {
            return Err(PostError::Unavailable);
        }
        let listener =
            unsafe { t_walk_create(srv, name.as_ptr(), name.len(), T_OREAD, 0) };
        let _ = unsafe { t_close(srv) };
        if listener < 0 {
            return Err(PostError::Unavailable);
        }
        let listener = unsafe { File::from_raw_fd(listener as i32, Rights::READ) };
        let listener = worker.register_owned(listener, T_POLLIN).map_err(PostError::Published)?;
        Ok(PanePlaceServer {
            worker,
            listener,
            listener_armed: true,
            listener_ready: false,
            conns,
            completed: Vec::new(),
            routes: BTreeMap::new(),
            principal: uid as u32,
            user: String::from(user),
            max_pixels: PLACE_MAX_PIXELS_HARD,
            residual_bytes: PLACE_MAX_PIXELS_HARD * 8 * MAX_CONNS as u64,
            diag: Diag::default(),
        })
    }

    /// The `/env/HALCYON_PLACE` address a pane's programs open to place into a
    /// tile: `/srv/halcyon-<user>/<hex(token)>/place`. The compositor writes
    /// this into the tile's environment before spawning it.
    pub fn place_address(&self, token: u128) -> String {
        let hex = paneroute::hex32(token);
        let mut s = String::with_capacity(13 + self.user.len() + 32 + 6);
        s.push_str("/srv/halcyon-");
        s.push_str(&self.user);
        s.push('/');
        for &b in hex.iter() {
            s.push(b as char);
        }
        s.push_str("/place");
        s
    }

    /// Bind a token to a live tile leaf (called when the compositor spawns the
    /// tile, alongside writing the tile's `/env/HALCYON_PLACE`).
    pub fn register(&mut self, token: u128, leaf: u32) {
        self.routes.insert(token, leaf);
    }

    /// Drop every token routing to `leaf` (called when the tile closes/crashes),
    /// so a subsequent walk to it fails closed.
    pub fn unregister_leaf(&mut self, leaf: u32) {
        self.routes.retain(|_, &mut l| l != leaf);
    }

    /// Set the effective per-image pixel cap AND the total residual budget. The
    /// compositor derives both from the live display-scaled atlas residual: the
    /// per-image cap is `residual / (8*MAX_CONNS)` (clamped to
    /// `[PLACE_MIN_PIXELS, PLACE_MAX_PIXELS_HARD]`), and `residual_bytes` is the
    /// full residual -- the live aggregate ceiling checked at new-accum
    /// admission (F2/F5), which catches a stale accum sized at a pre-resize cap.
    pub fn set_budget(&mut self, per_image_px: u64, residual_bytes: u64) {
        self.max_pixels = per_image_px.clamp(PLACE_MIN_PIXELS, PLACE_MAX_PIXELS_HARD);
        // Never below the space one clamped image needs, so a small display can
        // still admit one transfer.
        self.residual_bytes = residual_bytes.max(self.max_pixels * 8);
    }

    /// Complete buffered requests need another turn even without a read edge.
    pub fn runnable(&self) -> bool {
        self.conns.iter().any(|c| c.stream.runnable())
    }

    /// One descriptor represents every service source in the UI poll set.
    pub fn push_fds(&self, fds: &mut Vec<TPollFd>) {
        fds.push(TPollFd { fd: self.worker.as_raw_fd(), events: T_POLLIN, revents: 0 });
    }

    /// One bounded UI pass; the worker reports readiness only. Protocol state,
    /// peer checks and image delivery never leave this thread. Errors are fatal
    /// to the posting compositor, not permission to leave a dead service name.
    pub fn service(&mut self) -> Result<(), Error> {
        for event in self.worker.take_ready()?.iter() {
            if event.watch == self.listener {
                if event.events & (T_POLLHUP | T_POLLERR | T_POLLNVAL) != 0 { return Err(Error::Io); }
                self.listener_ready = true;
                self.listener_armed = false;
            } else if let Some(c) = self.conns.iter_mut().find(|c| c.watch == event.watch) {
                c.ready |= event.events;
            }
        }
        if self.listener_ready && self.conns.len() < MAX_CONNS && self.worker.free_slots()? > 0 {
            self.listener_ready = false;
            let h = self.worker.with_fd(self.listener, |fd| unsafe { t_srv_accept(fd as i64) })?;
            if h >= 0 {
                let file = unsafe { File::from_raw_fd(h as i32, Rights::READ | Rights::WRITE) };
                let mut info = TSrvPeerInfo::default();
                if unsafe { libthyla_rs::t_set_nonblock(h, true) } == 0
                    && unsafe { t_srv_peer(h, &mut info) } == 0
                    && info.alive == 1 && info.principal_id == self.principal {
                    let watch = self.worker.register_owned(file, T_POLLIN)?;
                    self.conns.push(Conn::new(watch));
                    if let Some(n) = self.diag.accepted.next() {
                        say!("halcyond: place conn from principal {} ({} so far)", info.principal_id, n);
                    }
                } else {
                    if let Some(n) = self.diag.refused.next() {
                        say!("halcyond: place conn REFUSED (peer {} alive {} != self {}; {} so far)",
                            info.principal_id, info.alive, self.principal, n);
                    }
                    // Refused peers close via File before any publication.
                }
            } else if h != -11 { return Err(Error::Io); }
        }
        if !self.conns.is_empty() { self.conns.rotate_left(1); }
        let deadline = libthyla_rs::time::monotonic_ns().saturating_add(2_000_000);
        let mut i = self.conns.len();
        while i > 0 {
            i -= 1;
            if self.conns[i].ready != 0 || self.conns[i].stream.runnable() {
                let others = self.conns.iter().enumerate().filter(|(j, _)| *j != i)
                    .map(|(_, c)| c.protocol.reserved()).sum();
                let budget = Budget { max_pixels: self.max_pixels, others_reserved: others,
                    residual_bytes: self.residual_bytes };
                let close = self.conns[i].ready & (T_POLLHUP | T_POLLERR | T_POLLNVAL) != 0
                    || !self.conns[i].service(&mut self.worker, &mut self.completed, &self.routes, budget, &mut self.diag, deadline)?;
                self.conns[i].ready = 0;
                if close {
                    let c = self.conns.remove(i);
                    self.worker.remove(c.watch)?;
                } else if !self.conns[i].stream.runnable() {
                    // Buffered work remains disarmed and keeps the UI runnable.
                    // Partial input or blocked output needs real readiness.
                    self.worker.rearm(self.conns[i].watch, self.conns[i].events())?;
                }
            }
        }
        // A retired connection still consumes a worker slot. Reclamation emits
        // a notice; only then rearm the listener. A full service cannot spin.
        if !self.listener_armed && !self.listener_ready && self.conns.len() < MAX_CONNS
            && self.worker.free_slots()? > 0 {
            self.worker.rearm(self.listener, T_POLLIN)?;
            self.listener_armed = true;
        }
        Ok(())
    }

    /// Take the rasters completed since the last call (each tagged with its
    /// target tile leaf).
    pub fn take_completed(&mut self) -> Vec<PaneCompletedImage> {
        core::mem::take(&mut self.completed)
    }
}
