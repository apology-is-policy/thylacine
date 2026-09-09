// placesrv -- the /srv/halcyon inline-media place server (I-47, HALCYON.md
// 14.7). The console renderer posts it so a short-lived `view` can hand it a
// decoded raster (the console spike; the per-pane session channel is a later
// slice). A minimal 9P2000.L server: the root dir "/" and one write-only file
// "place". A client walks to `place`, opens it O_WRONLY, and writes a
// place-request -- an `inlinewire` header then the ARGB payload; on completion
// the raster is queued for `Transcript::inject_image`.
//
// FORMAT-FUZZ SURFACE (audit:hard, I-47). This is the thin syscall shell: the
// 9P codec is the shared `libthyla_rs::ninep` server codec, and every untrusted
// place-payload decision lives in the PURE, host-tested `inlineaccum::PlaceAccum`
// (validate-before-allocate, the heap-safe per-image cap, sequential-only,
// no over-accumulation). The renderer runs the whole session on one fixed heap,
// so PLACE_MAX_PIXELS is deliberately BELOW the wire's own ceiling.
//
// The structure (frame read + fid table + dispatch) mirrors nocturned's proven
// /srv server (usr/nocturned/src/server.rs); what differs is the tiny namespace
// and that the one operation of substance -- a write to `place` -- feeds the
// accumulator instead of an audio ring.

use alloc::vec::Vec;

use halcyond::inlineaccum::{AccumStep, PlaceAccum};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_open, t_read, t_srv_accept, t_walk_create, t_write, TPollFd, T_OPATH, T_OREAD,
    T_POLLHUP, T_POLLIN, T_WALK_OPEN_FROM_ROOT,
};

const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;
const MAX_FIDS: usize = 8;
/// The console spike drives exactly ONE `view` at a time (a user runs `view x`
/// in their shell), so ONE connection is the honest bound -- and it is what
/// makes the aggregate place-memory footprint safe (audit F1): with ONE
/// connection there is at most ONE in-flight accumulator, so the aggregate IS
/// the per-image footprint, and the per-image cap is set from the heap residual
/// (`set_max_pixels`, F4). No cross-connection byte sum is wired (F5); it is
/// unnecessary at MAX_CONNS = 1. A second connection WAITS (the listener drops
/// from both poll sets while full -- see `push_fds`/`service`), bounded
/// acceptance, never a spin. KNOWN spike tradeoff (F6): a client that completes
/// an image but never clunks/disconnects holds the sole slot, denying inline
/// media to other shells until it dies -- `view` is short-lived and exits (EOF ->
/// POLLHUP -> reaped), and the held memory is an empty buffer, so this is a
/// deliberate availability tradeoff, not an OOM. Raising MAX_CONNS REQUIRES
/// wiring a real cross-connection byte budget (sum `PlaceAccum::reserved_bytes()`)
/// -- do NOT bump it alone. The session-path channel (a per-pane endpoint) is
/// where concurrency generalizes, with its own per-pane quota.
const MAX_CONNS: usize = 1;
const P9_VERSION: &[u8] = b"9P2000.L";
/// STATX_SIZE -- ninep exports MODE/NLINK/UID/GID but not SIZE.
const P9_GETATTR_SIZE: u64 = 0x200;

// The two-node namespace.
const P_ROOT: u64 = 0;
const P_PLACE: u64 = 1;

// Mode bits (mirror nocturned's 9P-mode service): the dir is r-x for all so the
// kernel dev9p per-component X-search passes; `place` is world-writable so a
// non-root `view` may open it O_WRONLY. The security trio is filled in getattr
// (an unfilled trio fails the X-search closed -- the /dev/pts lesson).
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;

/// The per-image pixel cap CEILING -- deliberately BELOW `inlinewire::MAX_PIXELS`
/// (16 Mpx). The effective cap is DYNAMIC (`PlaceServer.max_pixels`, set each
/// loop by `set_max_pixels` from the renderer's remaining heap): it never exceeds
/// this ceiling and never drops below `PLACE_MIN_PIXELS`.
///
/// THE HEAP BUDGET (audit F1/F2/F4). The renderer runs the whole session on a
/// fixed 64 MiB heap (`main.rs` ThylaAllocN) shared by the transcript's 32 MiB
/// content budget, the atlas (which SCALES WITH THE SCANOUT -- ~6 MiB at
/// 1280x800, ~18 MiB at 4K), the layout cache, and this place path. With
/// `reserve_exact` (F2) one transfer holds EXACTLY total_len (no Vec doubling)
/// and the completion `collect` adds one exact same-size `Vec<u32>` transient, so
/// the place peak is `8 bytes x pixels`; with `MAX_CONNS = 1` (F1) that is the
/// WHOLE place footprint. The F4 defect was budgeting at 1280x800 only, where
/// this 1 Mpx ceiling (an 8 MiB peak) is comfortable but the atlas is smallest;
/// at 4K the atlas alone is ~18 MiB and a fixed 8 MiB place peak leaves a thin,
/// unproven margin. So the cap is now the heap RESIDUAL after the display-scaled
/// atlas (`main.rs place_cap_for` -> `set_max_pixels`): it stays at this 1 Mpx
/// ceiling through 2560x1600 (the operator's HiDPI, native-size images) and
/// shrinks only past ~3K, where the atlas would otherwise crowd it out. A source
/// raster over the effective cap is refused (view falls back to a report); the
/// un-capped path is the gallery/session expand slice.
pub const PLACE_MAX_PIXELS_HARD: u64 = 1024 * 1024;

/// The per-image FLOOR -- the effective cap never drops below this even under
/// heap pressure, so a small image (a 256x256 = 64 Kpx icon) always displays.
pub const PLACE_MIN_PIXELS: u64 = 64 * 1024;

fn qid_of(path: u64) -> p9::Qid {
    p9::Qid {
        kind: if path == P_ROOT {
            p9::P9_QTDIR
        } else {
            p9::P9_QTFILE
        },
        version: 0,
        path,
    }
}

fn mode_of(path: u64) -> u32 {
    if path == P_ROOT {
        S_IFDIR | 0o555
    } else {
        S_IFREG | 0o666
    }
}

/// A raster fully received on the place channel, awaiting injection into the
/// transcript by the owner of the render loop.
pub struct CompletedImage {
    pub w: u32,
    pub h: u32,
    pub argb: Vec<u32>,
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
}

enum Disp {
    Reply(usize),
    Fatal,
}

struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    /// The in-flight place transfer, and the fid it belongs to. One at a time
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

    fn fid_set(&mut self, fid: u32, path: u64) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid {
                fid,
                path,
                opened: false,
            });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid {
                fid,
                path,
                opened: false,
            });
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every COMPLETE 9P frame (the nocturned
    /// shape). Completed rasters are pushed to `out`. Returns false to close the
    /// connection (EOF, a wire violation, or a reply write failure).
    fn service(&mut self, out: &mut Vec<CompletedImage>, max_pixels: u64) -> bool {
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
            match self.dispatch(&frame, hdr, out, max_pixels) {
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
        out: &mut Vec<CompletedImage>,
        max_pixels: u64,
    ) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag),
            p9::P9_TREAD => self.h_read(tmsg, tag),
            p9::P9_TWRITE => self.h_write(tmsg, tag, out, max_pixels),
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
            let w =
                unsafe { t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent) };
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
        if !self.fid_set(a.fid, P_ROOT) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rattach(&mut self.out_buf, tag, &qid_of(P_ROOT))
    }

    fn walk_child(cur: u64, name: &[u8]) -> Option<u64> {
        if name == b".." || name == b"." {
            return Some(if cur == P_PLACE { P_ROOT } else { cur });
        }
        match cur {
            P_ROOT if name == b"place" => Some(P_PLACE),
            _ => None,
        }
    }

    fn h_walk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        let mut cur = f.path;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut n = 0usize;
        for k in 0..(a.nwname as usize).min(p9::P9_MAX_WALK) {
            match Conn::walk_child(cur, a.names[k]) {
                Some(p) => {
                    cur = p;
                    qids[n] = qid_of(p);
                    n += 1;
                }
                None => break,
            }
        }
        if a.nwname > 0 && n == 0 {
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
            path: f.path,
            opened: true,
        });
        p9::build_rlopen(&mut self.out_buf, tag, &qid_of(f.path), 0)
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
        if f.path == P_ROOT {
            return self.err(tag, p9::E_ISDIR);
        }
        // `place` is write-only: a read returns EOF (an empty Rread), never data.
        p9::build_rread(&mut self.out_buf, tag, &[])
    }

    fn h_write(
        &mut self,
        tmsg: &[u8],
        tag: u16,
        out: &mut Vec<CompletedImage>,
        max_pixels: u64,
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
        if !f.opened || f.path != P_PLACE {
            // Only `place` accepts writes; the dir does not.
            return self.err(tag, p9::E_INVAL);
        }
        // One place transfer per connection at a time: a second place-fid write
        // while one is in flight is refused (bounds the held partials).
        match &self.accum {
            Some((afid, _)) if *afid != a.fid => return self.err(tag, p9::E_BUSY),
            None => self.accum = Some((a.fid, PlaceAccum::new(max_pixels))),
            _ => {}
        }
        let acc = &mut self.accum.as_mut().unwrap().1;
        match acc.write(a.offset, a.data) {
            AccumStep::More => {
                p9::build_rwrite(&mut self.out_buf, tag, a.count)
            }
            AccumStep::Done { w, h, argb } => {
                // Keep the accumulator bound to this fid: it has advanced its own
                // base and reset its buffer, so a subsequent image on the same
                // fid (whose first write arrives at the cumulative offset) is
                // accepted -- the multi-image path `inlineaccum` is built + tested
                // for. It is freed on clunk/teardown, or replaced on a Reject.
                out.push(CompletedImage { w, h, argb });
                p9::build_rwrite(&mut self.out_buf, tag, a.count)
            }
            AccumStep::Reject => {
                // A malformed / over-cap / non-sequential write: drop the partial
                // and refuse. The client's transfer is spent (it should clunk).
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
        let mode = mode_of(f.path);
        let nlink = if f.path == P_ROOT { 2u64 } else { 1u64 };
        // Fill the security trio -- dev9p's per-component X-search reads it, and
        // an unfilled trio fails closed. uid/gid 0 (the system renderer).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        p9::build_rgetattr(&mut self.out_buf, tag, valid, &qid_of(f.path), mode, 0, 0, nlink, 0)
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
        // Every op replies synchronously, so there is never an in-flight request
        // to abandon; acknowledge the flush.
        let _ = p9::parse_tflush(tmsg);
        p9::build_rflush(&mut self.out_buf, tag)
    }
}

/// The /srv/halcyon place server: the listener, its live connections, and the
/// queue of rasters completed since the last drain.
pub struct PlaceServer {
    listener: i64,
    conns: Vec<Conn>,
    completed: Vec<CompletedImage>,
    /// The current per-image pixel cap: the heap RESIDUAL after the display-scaled
    /// atlas (set each loop by `set_max_pixels`, F4). A transfer's accumulator is
    /// created with this value, so the cap tracks the live display without any
    /// per-connection state.
    max_pixels: u64,
}

impl PlaceServer {
    /// Post /srv/halcyon (9P-mode; perm 0). Requires MAY_POST_SERVICE (joey
    /// grants the console renderer the bit). None on failure -- the caller keeps
    /// running as a plain console renderer (inline `view` is simply unavailable).
    pub fn post() -> Option<PlaceServer> {
        let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
        if srv < 0 {
            return None;
        }
        let listener = unsafe { t_walk_create(srv, b"halcyon".as_ptr(), 7, T_OREAD, 0) };
        let _ = unsafe { t_close(srv) };
        if listener < 0 {
            return None;
        }
        Some(PlaceServer {
            listener,
            conns: Vec::new(),
            completed: Vec::new(),
            max_pixels: PLACE_MAX_PIXELS_HARD,
        })
    }

    /// Set the effective per-image pixel cap from the renderer's remaining heap
    /// (F4). The caller derives it from the live display-scaled atlas each loop;
    /// this clamps it to `[PLACE_MIN_PIXELS, PLACE_MAX_PIXELS_HARD]` so a small
    /// image always fits and no image ever exceeds the ceiling. A transfer in
    /// flight keeps the cap it started with (the accumulator already holds it);
    /// the next transfer picks up the new value.
    pub fn set_max_pixels(&mut self, px: u64) {
        self.max_pixels = px.clamp(PLACE_MIN_PIXELS, PLACE_MAX_PIXELS_HARD);
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
                // POLLHUP too (audit F3): a peer that closes with no pending
                // data must wake the caller's blocking poll so the conn is
                // reaped promptly, matching `service`'s own poll -- otherwise a
                // dead conn holds a MAX_CONNS slot until some other event wakes
                // the loop.
                fd: c.handle as i32,
                events: T_POLLIN | T_POLLHUP,
                revents: 0,
            });
        }
    }

    /// One non-blocking pass: accept a pending connection (while there is room)
    /// and service every readable one. Completed rasters accumulate; the caller
    /// drains them with `take_completed`. The caller's outer loop re-polls, so a
    /// multi-frame transfer proceeds one read per pass -- keeping the render loop
    /// responsive between chunks.
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
                self.conns.push(Conn::new(h));
            }
        }
        // Service readable conns backward (remove-safe). A conn accepted just now
        // sits past the polled prefix and is serviced next pass.
        let nc = pfds.len() - listener_slot;
        let mut i = nc;
        while i > 0 {
            i -= 1;
            let pf = pfds[listener_slot + i];
            if pf.revents & (T_POLLIN | T_POLLHUP) != 0
                && !self.conns[i].service(&mut self.completed, self.max_pixels)
            {
                let _ = unsafe { t_close(self.conns[i].handle) };
                self.conns.remove(i);
            }
        }
    }

    /// Take the rasters completed since the last call.
    pub fn take_completed(&mut self) -> Vec<CompletedImage> {
        core::mem::take(&mut self.completed)
    }
}
