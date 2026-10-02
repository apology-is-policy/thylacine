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
use halcyond::servicewire::{Handler, Interest, Stream};
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;

use halcyond::inlineaccum::{AccumStep, PlaceAccum};
use halcyond::paneroute::{self, Node, Quiet};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_getuid, t_open, t_srv_accept, t_srv_peer, t_walk_create, TPollFd, TSrvPeerInfo,
    T_OPATH, T_OREAD, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL, T_POLLOUT, T_WALK_OPEN_FROM_ROOT,
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
    token: u128,
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
    file: File,
    ready: i16,
    stream: Stream,
    protocol: Protocol,
}
impl Conn {
    fn new(file: File) -> Self {
        Self {
            file,
            ready: 0,
            stream: Stream::new(),
            protocol: Protocol::new(),
        }
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
    ) -> bool {
        let mut reply = Reply {
            protocol: &mut self.protocol,
            out,
            shared,
            control,
            other_accums,
            diag,
        };
        self.stream.service(
            &mut NativeEndpoint(self.file.as_raw_fd() as i64),
            &mut reply,
            deadline,
        )
    }
}
struct Reply<'a> {
    protocol: &'a mut Protocol,
    out: &'a mut Vec<PaneCompletedImage>,
    shared: &'a Shared,
    control: &'a libthyla_rs::service_worker::Control,
    other_accums: usize,
    diag: &'a mut Diag,
}
impl Handler for Reply<'_> {
    fn dispatch(&mut self, frame: &[u8]) -> Result<(), ()> {
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
        match self
            .protocol
            .dispatch(frame, hdr, self.out, &routes, budget, self.diag)
        {
            Disp::Fatal => Err(()),
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
                Ok(())
            }
        }
    }
    fn reply(&self) -> &[u8] {
        &self.protocol.out_buf
    }
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
        routes: &Routes,
        budget: Budget,
        diag: &mut Diag,
    ) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        if self.out_buf.try_reserve_exact(SRV_MSIZE_USIZE).is_err() {
            return Disp::Fatal;
        }
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
                    let _ = n; // diagnostics must be drained by the UI, never written here
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
                if let Some(&leaf) = routes.get(&token) {
                    out.push(PaneCompletedImage {
                        token,
                        id,
                        leaf,
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

/// Once setup may have published a listener or enabled a participant, failure
/// ends the session. It must not leave a live poster with a dead executor.
#[derive(Debug)]
pub enum PostError {
    Unavailable,
    Published(Error),
}
const ROUTES: usize = 32;
const _: () = assert!(core::mem::size_of::<PaneCompletedImage>() <= 128);
#[derive(Clone, Copy)]
struct Routes {
    slots: [Option<(u128, u32)>; ROUTES],
}
const _: () = assert!(core::mem::size_of::<Routes>() <= 32 * 128);
impl Routes {
    fn empty() -> Self {
        Self {
            slots: [None; ROUTES],
        }
    }
    fn get(&self, token: &u128) -> Option<&u32> {
        self.slots
            .iter()
            .flatten()
            .find(|(t, _)| t == token)
            .map(|(_, l)| l)
    }
    fn contains_key(&self, token: &u128) -> bool {
        self.get(token).is_some()
    }
    fn insert(&mut self, token: u128, leaf: u32) -> bool {
        if let Some(slot) = self
            .slots
            .iter_mut()
            .find(|s| s.is_some_and(|(t, _)| t == token))
        {
            *slot = Some((token, leaf));
            return true;
        }
        if let Some(slot) = self.slots.iter_mut().find(|s| s.is_none()) {
            *slot = Some((token, leaf));
            return true;
        }
        false
    }
    fn remove_leaf(&mut self, leaf: u32) {
        for slot in &mut self.slots {
            if slot.is_some_and(|(_, l)| l == leaf) {
                *slot = None;
            }
        }
    }
}
fn completion_route_current(routes: &Routes, image: &PaneCompletedImage) -> bool {
    routes.get(&image.token) == Some(&image.leaf)
}
// The desired route table is a bounded, coalesced metadata mailbox: 32 records.
// Removing a route updates this same durable state; revocations cannot fall out
// of a full queue. The executor copies metadata, never holds this lock for I/O.
struct Mailbox {
    routes: Routes,
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
pub struct PanePlaceServer {
    owner: libthyla_rs::service_worker::ServiceWorker<Shared>,
}
impl PanePlaceServer {
    /// Media-only native fixtures do not register clipboard authority.
    pub fn post(user: &str) -> Result<Self, PostError> {
        Self::start(user, None)
    }
    pub fn post_on(user: &str, ring: &tapestry::EventRing) -> Result<Self, PostError> {
        let admission =
            tapestry::admission::Channel::preopen(ring).map_err(|_| PostError::Unavailable)?;
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
                revision: 0,
                max_pixels: PLACE_MAX_PIXELS_HARD,
                residual: PLACE_MAX_PIXELS_HARD * 8 * MAX_CONNS as u64,
                completed: core::array::from_fn(|_| None),
                failed: false,
            }),
        };
        libthyla_rs::service_worker::ServiceWorker::new(shared, run_owner)
            .map(|owner| Self { owner })
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
    pub fn unregister_leaf(&mut self, leaf: u32) {
        if let Ok(shared) = self.owner.state() {
            let mut m = shared.mail.lock();
            m.routes.remove_leaf(leaf);
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
        for slot in &mut m.completed {
            if let Some(img) = slot.take() {
                out.push(img);
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
            let mut p = TPollFd {
                fd: seat.fd(),
                events: T_POLLIN,
                revents: 0,
            };
            unsafe {
                libthyla_rs::t_poll(&mut p, 1, 10);
            }
        }
    }
    let mut conns = Vec::new();
    conns
        .try_reserve_exact(MAX_CONNS)
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
            s.pump()?;
            if stopping && s.retired() {
                return Ok(());
            }
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
                if c.ready == 0 && !c.stream.runnable() {
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
                    );
                if close {
                    conns.remove(i);
                } else {
                    conns[i].ready = 0;
                }
            }
        }
        let mut poll = [TPollFd::default(); MAX_CONNS + 4];
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
        let listener_index = if !stopping && conns.len() < MAX_CONNS {
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
        let timeout = if !stopping && conns.iter().any(|c| c.stream.runnable()) {
            0
        } else {
            -1
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
                if unsafe { libthyla_rs::t_set_nonblock(fd, true) } == 0
                    && unsafe { t_srv_peer(fd, &mut peer) } == 0
                    && peer.alive == 1
                    && peer.principal_id == shared.principal
                {
                    conns.push(Conn::new(file));
                }
            } else if fd != -11 {
                return Err(Error::Io);
            }
        }
    }
}
