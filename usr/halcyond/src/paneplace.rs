// paneplace -- the per-pane SESSION inline-media place server (I-47, HALCYON.md
// 14.7.2). The per-user session compositor (`session.rs`) posts ONE service,
// `/srv/halcyon-<user>`, and routes each place-request to the tile named by a
// per-pane SECRET TOKEN carried as a path component: a pane's programs reach
// their tile by walking `<hex(token)>/place`, the address the compositor put in
// that pane's `/env/HALCYON_PLACE` (per-Proc, deep-copied at spawn, so isolated
// from every other pane). This is the session generalization of the console
// spike's `placesrv` (single global `/srv/halcyon`, MAX_CONNS=1): here MANY
// tiles share ONE service and completions are TAGGED with the target leaf.
//
// FORMAT-FUZZ SURFACE (audit:hard, I-47). The thin syscall shell only: the 9P
// codec is `libthyla_rs::ninep`; the untrusted-NAME decisions (the 32-hex token
// codec + the namespace walk, fail-closed on an unknown/dead pane) live in the
// PURE, host-tested `paneroute`; the untrusted-PAYLOAD decisions
// (validate-before-allocate, the heap-safe per-image cap) live in the PURE,
// host-tested `inlineaccum::PlaceAccum`. What this file adds over `placesrv` is
// the routing (token -> live leaf) and TWO authority axes:
//   1. the SECRET token (a path component; unguessable u128, per-pane, only in
//      the pane's own /env -- another pane cannot name it), and
//   2. the PEER PRINCIPAL check at accept: a connection is refused unless its
//      peer is the SESSION'S OWN USER (t_srv_peer). So even a leaked token
//      cannot let a DIFFERENT user place into this session's panes.
// The DoS floor: MAX_CONNS bounds concurrent transfers; the per-image cap
// (`max_pixels`, the heap residual DIVIDED by MAX_CONNS -- set each loop by the
// compositor) bounds a single transfer AND, times MAX_CONNS, the aggregate
// in-flight; the per-pane stored quota (live placements + total raster bytes)
// is the tile transcript's own content budget (`inject_image` -> enforce_budget
// evicts frozen blocks: max_cost + max_blocks, failing clean, HALCYON.md 14.7.7).

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

use halcyond::inlineaccum::{AccumStep, PlaceAccum};
use halcyond::paneroute::{self, Node};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_getuid, t_open, t_read, t_srv_accept, t_srv_peer, t_walk_create, t_write, TPollFd,
    TSrvPeerInfo, T_OPATH, T_OREAD, T_POLLHUP, T_POLLIN, T_WALK_OPEN_FROM_ROOT,
};

const SRV_MSIZE: u32 = 32768;
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
        // passes; `place` is world-writable so the pane's `view` may open it
        // O_WRONLY. The real gate is the secret token + the peer principal.
        Node::Root | Node::Dir(_) => S_IFDIR | 0o555,
        Node::Place(_) => S_IFREG | 0o666,
    }
}

fn is_dir(node: Node) -> bool {
    matches!(node, Node::Root | Node::Dir(_))
}

/// A raster fully received on a pane's place channel, TAGGED with the tile leaf
/// it must inject into. The compositor drains these and calls the tile's
/// `inject_image` (whose content budget is the per-pane stored quota).
pub struct PaneCompletedImage {
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

struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    /// The in-flight place transfer and the fid it belongs to. One at a time
    /// per connection (a second concurrent place-open is refused E_BUSY), so a
    /// hostile conn cannot hold many partial rasters.
    accum: Option<(u32, PlaceAccum)>,
}

impl Conn {
    fn new(handle: i64) -> Conn {
        Conn {
            handle,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [None; MAX_FIDS],
            in_buf: Vec::new(),
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

    /// Read available bytes and dispatch every COMPLETE 9P frame (the placesrv
    /// shape). Completed rasters are pushed to `out`, each tagged via `routes`
    /// with the live leaf its token names. Returns false to close the
    /// connection (EOF, a wire violation, or a reply write failure).
    fn service(
        &mut self,
        out: &mut Vec<PaneCompletedImage>,
        routes: &BTreeMap<u128, u32>,
        budget: Budget,
    ) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false; // a full msize buffered with no complete frame
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n = unsafe { t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
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
                return true; // a partial frame waits for the next read
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            match self.dispatch(&frame, hdr, out, routes, budget) {
                Disp::Fatal => return false,
                Disp::Reply(rlen) => {
                    if !self.send_all(rlen) {
                        return false;
                    }
                }
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(
        &mut self,
        tmsg: &[u8],
        hdr: p9::Header,
        out: &mut Vec<PaneCompletedImage>,
        routes: &BTreeMap<u128, u32>,
        budget: Budget,
    ) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag, routes),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag),
            p9::P9_TREAD => self.h_read(tmsg, tag),
            p9::P9_TWRITE => self.h_write(tmsg, tag, out, routes, budget),
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

    fn send_all(&mut self, rlen: usize) -> bool {
        let mut sent = 0usize;
        while sent < rlen {
            let w = unsafe { t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent) };
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

    fn h_walk(&mut self, tmsg: &[u8], tag: u16, routes: &BTreeMap<u128, u32>) -> Result<usize, ()> {
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
                say!(
                    "halcyond: place walk NOENT at root (nwname={} first={} bytes)",
                    a.nwname,
                    a.names[0].len()
                );
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

    fn h_read(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        // `place` is write-only: a read returns EOF (an empty Rread), never data.
        p9::build_rread(&mut self.out_buf, tag, &[])
    }

    fn h_write(
        &mut self,
        tmsg: &[u8],
        tag: u16,
        out: &mut Vec<PaneCompletedImage>,
        routes: &BTreeMap<u128, u32>,
        budget: Budget,
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
            AccumStep::Done { w, h, argb } => {
                // Route to the live leaf the token names. A token whose tile
                // closed BETWEEN the walk and now (routes dropped it) resolves
                // to nothing: the raster is DISCARDED (the pane is gone), not
                // misrouted. The accumulator stays bound to the fid for a
                // subsequent image on the same connection (inlineaccum's
                // multi-image path), freed on clunk/teardown.
                if let Some(&leaf) = routes.get(&token) {
                    out.push(PaneCompletedImage { leaf, w, h, argb });
                } else {
                    say!(
                        "halcyond: place completed {}x{} but its token is not routed (tile gone)",
                        w,
                        h
                    );
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
        // Every op replies synchronously, so there is never an in-flight
        // request to abandon; acknowledge the flush.
        let _ = p9::parse_tflush(tmsg);
        p9::build_rflush(&mut self.out_buf, tag)
    }
}

/// The per-user session place server: the listener, its live connections, the
/// token->leaf routing table, and the queue of rasters completed since the last
/// drain (each tagged with its target leaf).
pub struct PanePlaceServer {
    listener: i64,
    conns: Vec<Conn>,
    completed: Vec<PaneCompletedImage>,
    /// token -> live tile leaf. The compositor keeps this current
    /// (`register`/`unregister_leaf`); a walk resolves a token ONLY while it is
    /// present, so a closed tile's token fails closed (E_NOENT).
    routes: BTreeMap<u128, u32>,
    /// The session's own principal (from t_getuid at post). A connection whose
    /// peer principal differs is refused at accept -- the second authority axis.
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
}

impl PanePlaceServer {
    /// Post `/srv/halcyon-<user>` (9P-mode; perm 0). Requires
    /// MAY_POST_SERVICE (login grants the session compositor the bit, one hop).
    /// None on failure -- the caller keeps running as a plain compositor
    /// (inline `view` is simply unavailable in its tiles).
    pub fn post(user: &str) -> Option<PanePlaceServer> {
        let uid = unsafe { t_getuid() };
        if uid < 0 {
            return None; // fail-closed: without a principal the accept gate cannot hold.
        }
        let mut name = String::with_capacity(8 + user.len());
        name.push_str("halcyon-");
        name.push_str(user);
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
        if srv < 0 {
            return None;
        }
        let listener =
            unsafe { t_walk_create(srv, name.as_ptr(), name.len(), T_OREAD, 0) };
        let _ = unsafe { t_close(srv) };
        if listener < 0 {
            return None;
        }
        Some(PanePlaceServer {
            listener,
            conns: Vec::new(),
            completed: Vec::new(),
            routes: BTreeMap::new(),
            principal: uid as u32,
            user: String::from(user),
            max_pixels: PLACE_MAX_PIXELS_HARD,
            residual_bytes: PLACE_MAX_PIXELS_HARD * 8 * MAX_CONNS as u64,
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

    /// Append this server's fds (the listener while there is room, plus every
    /// live connection) to a caller's poll set, so a place write wakes the
    /// caller's blocking wait alongside its own events.
    pub fn push_fds(&self, fds: &mut Vec<TPollFd>) {
        if self.conns.len() < MAX_CONNS {
            fds.push(TPollFd {
                fd: self.listener as i32,
                events: T_POLLIN,
                revents: 0,
            });
        }
        for c in &self.conns {
            fds.push(TPollFd {
                fd: c.handle as i32,
                events: T_POLLIN | T_POLLHUP,
                revents: 0,
            });
        }
    }

    /// One non-blocking pass: accept a pending connection (while there is room,
    /// gated on the peer being the session's own user) and service every
    /// readable one. Completed rasters accumulate tagged with their leaf; the
    /// caller drains them with `take_completed`.
    pub fn service(&mut self) {
        let has_room = self.conns.len() < MAX_CONNS;
        let mut pfds: Vec<TPollFd> = Vec::with_capacity(1 + self.conns.len());
        if has_room {
            pfds.push(TPollFd {
                fd: self.listener as i32,
                events: T_POLLIN,
                revents: 0,
            });
        }
        let listener_slot = has_room as usize;
        for c in &self.conns {
            pfds.push(TPollFd {
                fd: c.handle as i32,
                events: T_POLLIN | T_POLLHUP,
                revents: 0,
            });
        }
        let rc = unsafe { libthyla_rs::t_poll(pfds.as_mut_ptr(), pfds.len(), 0) };
        if rc <= 0 {
            return;
        }
        if has_room && pfds[0].revents & T_POLLIN != 0 {
            let h = unsafe { t_srv_accept(self.listener) };
            if h >= 0 {
                // The peer-principal gate: the connection's peer must be the
                // session's own user, and alive. Fail-closed on a dead/unknown
                // peer. A DIFFERENT principal is refused -- the secret token is
                // the primary gate; this closes the leaked-token-across-users
                // vector entirely.
                let mut info = TSrvPeerInfo::default();
                let ok = unsafe { t_srv_peer(h, &mut info) } == 0
                    && info.alive == 1
                    && info.principal_id == self.principal;
                if ok {
                    say!("halcyond: place conn from principal {}", info.principal_id);
                    self.conns.push(Conn::new(h));
                } else {
                    say!(
                        "halcyond: place conn REFUSED (peer {} alive {} != self {})",
                        info.principal_id,
                        info.alive,
                        self.principal
                    );
                    let _ = unsafe { t_close(h) };
                }
            }
        }
        let nc = pfds.len() - listener_slot;
        let mut i = nc;
        while i > 0 {
            i -= 1;
            let pf = pfds[listener_slot + i];
            if pf.revents & (T_POLLIN | T_POLLHUP) != 0 {
                // F2/F5: the buffers the OTHER live connections have reserved
                // right now -- the live aggregate ceiling for conn[i]'s next new
                // accum. Computed fresh per conn (the immutable sum completes
                // before the mutable service borrow), so it reflects earlier
                // iterations' completions.
                let others: usize = self
                    .conns
                    .iter()
                    .enumerate()
                    .filter(|(j, _)| *j != i)
                    .map(|(_, c)| c.reserved())
                    .sum();
                let budget = Budget {
                    max_pixels: self.max_pixels,
                    others_reserved: others,
                    residual_bytes: self.residual_bytes,
                };
                if !self.conns[i].service(&mut self.completed, &self.routes, budget) {
                    let _ = unsafe { t_close(self.conns[i].handle) };
                    self.conns.remove(i);
                }
            }
        }
    }

    /// Take the rasters completed since the last call (each tagged with its
    /// target tile leaf).
    pub fn take_completed(&mut self) -> Vec<PaneCompletedImage> {
        core::mem::take(&mut self.completed)
    }
}
