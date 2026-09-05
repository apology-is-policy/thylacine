// The /srv/nocturne 9P tree at N-1 (docs/NOCTURNE.md section 6.4, the
// heritage floor): / { ctl, info, audio }. A write to `audio` is S16LE stereo
// at the graph rate (48 kHz), queued into a bounded FIFO the device pump
// drains one period at a time; a write that finds the FIFO full PARKS (its
// Rwrite is deferred until the pump frees room -- Plan 9's blocking write) and
// a read of `audio` returns 0 bytes (an output-only device, audio(3)). `info`
// renders the audiostat words (bufsize / buffered) plus the driver counters;
// `ctl` accepts `flush`.
//
// Framing + dispatch mirror usr/ptyfs/src/server.rs (the native /srv server
// idiom); a connection's parked writes die with the connection.

use alloc::collections::VecDeque;
use alloc::vec::Vec;

use libthyla_rs::ninep as p9;
use libthyla_rs::{t_close, t_open, t_walk_create, T_OPATH, T_OREAD, T_WALK_OPEN_FROM_ROOT};

use crate::snd::{Stats, BUFFER_BYTES, PERIODS, PERIOD_BYTES, RATE_HZ};

pub const MAX_CONNS: usize = 8;
const MAX_FIDS: usize = 32;
const MAX_PENDING_WRITES: usize = 8;
const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;
/// ~340 ms of S16LE stereo at 48 kHz; the write-side backlog beyond the four
/// periods the device holds.
const FIFO_CAP: usize = 64 * 1024;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const P9_GETATTR_SIZE: u64 = 0x200;

const P_ROOT: u64 = 0;
const P_CTL: u64 = 1;
const P_INFO: u64 = 2;
const P_AUDIO: u64 = 3;

const CHILDREN: [(&[u8], u64, u32); 3] = [
    (b"ctl", P_CTL, S_IFREG | 0o644),
    (b"info", P_INFO, S_IFREG | 0o444),
    (b"audio", P_AUDIO, S_IFREG | 0o666),
];

fn mode_of(path: u64) -> u32 {
    for (_, p, m) in CHILDREN {
        if p == path {
            return m;
        }
    }
    DIR_MODE
}

/// State shared by every connection and the device pump (single-threaded).
pub struct Shared {
    fifo: VecDeque<u8>,
    pub stats: Stats,
    pub started: bool,
    bytes_in: u64,
    flushes: u64,
}

impl Shared {
    pub fn new() -> Shared {
        Shared {
            fifo: VecDeque::with_capacity(FIFO_CAP),
            stats: Stats::default(),
            started: false,
            bytes_in: 0,
            flushes: 0,
        }
    }

    pub fn fifo_len(&self) -> usize {
        self.fifo.len()
    }

    pub fn drop_fifo(&mut self) {
        self.fifo.clear();
    }

    /// Fill one period from the FIFO. A partial period is padded with silence
    /// (Plan 9 plays what is buffered); an empty FIFO yields silence and false.
    pub fn next_period(&mut self, buf: &mut [u8]) -> bool {
        let want = buf.len();
        let have = self.fifo.len().min(want);
        // Whole frames only: a torn frame would shift the channel phase.
        let have = have - (have % 4);
        for (i, b) in buf.iter_mut().enumerate() {
            *b = if i < have { self.fifo.pop_front().unwrap_or(0) } else { 0 };
        }
        have > 0
    }

    /// Push as many of `data`'s bytes as fit; returns the count accepted.
    fn push(&mut self, data: &[u8]) -> usize {
        let room = FIFO_CAP.saturating_sub(self.fifo.len());
        let n = data.len().min(room);
        self.fifo.extend(data[..n].iter().copied());
        self.bytes_in = self.bytes_in.saturating_add(n as u64);
        n
    }

    fn render_info(&self, out: &mut Vec<u8>) {
        let s = &self.stats;
        let buffered = self.fifo.len() as u64 + u64::from(s.last_latency_bytes);
        let text = alloc::format!(
            "device virtio-snd stream 0 playback\nformat s16c2r{}\nbufsize {}\nbuffered {}\nperiod-bytes {}\nbuffer-bytes {}\nperiods {}\nstarted {}\nperiods-played {}\nsilence-periods {}\ntx-errors {}\nbad-used {}\nlatency-bytes {}\nbytes-in {}\nflushes {}\n",
            RATE_HZ,
            PERIOD_BYTES,
            buffered,
            PERIOD_BYTES,
            BUFFER_BYTES,
            PERIODS,
            u8::from(self.started),
            s.periods_played,
            s.silence_periods,
            s.tx_errors,
            s.bad_used,
            s.last_latency_bytes,
            self.bytes_in,
            self.flushes
        );
        out.extend_from_slice(text.as_bytes());
    }

    fn render_ctl(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(b"nocturne n-1: one playback stream; write s16le stereo 48000 Hz to audio; ctl accepts: flush\n");
    }
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
}

enum Disp {
    Reply(usize),
    Deferred,
    Fatal,
}

/// A Twrite to `audio` that found the FIFO full: the bytes not yet accepted
/// and the tag whose Rwrite is owed once they are.
struct PendingWrite {
    tag: u16,
    fid: u32,
    data: Vec<u8>,
    done: usize,
}

pub struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    defer: bool,
    pending: Vec<PendingWrite>,
}

pub fn post_srv_nocturne() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"nocturne".as_ptr(), 8, T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

impl Conn {
    pub fn new(handle: i64) -> Conn {
        Conn {
            handle,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [None; MAX_FIDS],
            in_buf: Vec::new(),
            out_buf: Vec::new(),
            defer: false,
            pending: Vec::new(),
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    pub fn teardown(&mut self, _sh: &mut Shared) {
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.pending.clear();
    }

    /// Retry the parked writes in order; a fully-accepted one gets its Rwrite.
    /// False if the connection's reply write failed (close it).
    pub fn poll_writes(&mut self, sh: &mut Shared) -> bool {
        while !self.pending.is_empty() {
            let (tag, total, finished) = {
                let pw = &mut self.pending[0];
                let n = sh.push(&pw.data[pw.done..]);
                pw.done += n;
                (pw.tag, pw.data.len(), pw.done >= pw.data.len())
            };
            if !finished {
                return true; // still parked; keep order
            }
            self.pending.remove(0);
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            match p9::build_rwrite(&mut self.out_buf, tag, total as u32) {
                Ok(len) => {
                    if !self.send_all(len) {
                        return false;
                    }
                }
                Err(()) => return false,
            }
        }
        true
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids.iter().position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    fn fid_set(&mut self, fid: u32, path: u64) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid { fid, path, opened: false });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid { fid, path, opened: false });
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every complete frame (the ptyfs shape).
    pub fn service(&mut self, sh: &mut Shared) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n = unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
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
            match self.dispatch(sh, &frame, hdr) {
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

    fn dispatch(&mut self, sh: &mut Shared, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag),
            p9::P9_TREAD => self.h_read(sh, tmsg, tag),
            p9::P9_TWRITE => self.h_write(sh, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
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
            let w = unsafe { libthyla_rs::t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent) };
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

    fn qid_of(path: u64) -> p9::Qid {
        p9::Qid {
            kind: if path == P_ROOT { p9::P9_QTDIR } else { p9::P9_QTFILE },
            version: 0,
            path,
        }
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
        self.pending.clear();
        self.msize = negotiated;
        let ver: &[u8] = if a.version == P9_VERSION_9P2000_L {
            self.version_done = true;
            P9_VERSION_9P2000_L
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
        p9::build_rattach(&mut self.out_buf, tag, &Conn::qid_of(P_ROOT))
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
            let name = a.names[k];
            let next = if name == b".." || name == b"." {
                Some(P_ROOT)
            } else if cur == P_ROOT {
                CHILDREN.iter().find(|(nm, _, _)| *nm == name).map(|(_, p, _)| *p)
            } else {
                None
            };
            match next {
                Some(p) => {
                    cur = p;
                    qids[n] = Conn::qid_of(p);
                    n += 1;
                }
                None => break,
            }
        }
        if a.nwname > 0 && n == 0 {
            return self.err(tag, p9::E_NOENT);
        }
        if n == a.nwname as usize {
            if !self.fid_set(a.newfid, cur) {
                return self.err(tag, p9::E_NOMEM);
            }
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
        let mut nf = f;
        nf.opened = true;
        self.fids[i] = Some(nf);
        p9::build_rlopen(&mut self.out_buf, tag, &Conn::qid_of(f.path), 0)
    }

    fn h_read(&mut self, sh: &mut Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        match f.path {
            P_ROOT => self.err(tag, p9::E_ISDIR),
            // audio(3): an output-only device returns a zero length when read.
            P_AUDIO => p9::build_rread(&mut self.out_buf, tag, &[]),
            _ => {
                let mut text: Vec<u8> = Vec::new();
                if f.path == P_INFO {
                    sh.render_info(&mut text);
                } else {
                    sh.render_ctl(&mut text);
                }
                let off = a.offset as usize;
                if off >= text.len() {
                    return p9::build_rread(&mut self.out_buf, tag, &[]);
                }
                let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
                let k = (text.len() - off).min(a.count as usize).min(cap);
                p9::build_rread(&mut self.out_buf, tag, &text[off..off + k])
            }
        }
    }

    fn h_write(&mut self, sh: &mut Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
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
        match f.path {
            P_CTL => {
                let verb = a.data.iter().take_while(|&&b| b != b'\n' && b != b' ').count();
                if &a.data[..verb] == b"flush" {
                    sh.drop_fifo();
                    sh.flushes = sh.flushes.saturating_add(1);
                    p9::build_rwrite(&mut self.out_buf, tag, a.data.len() as u32)
                } else {
                    self.err(tag, p9::E_INVAL)
                }
            }
            P_AUDIO => {
                if a.data.is_empty() {
                    return p9::build_rwrite(&mut self.out_buf, tag, 0);
                }
                // Order matters: a write behind a parked one must queue behind it.
                if self.pending.is_empty() {
                    let n = sh.push(a.data);
                    if n == a.data.len() {
                        return p9::build_rwrite(&mut self.out_buf, tag, n as u32);
                    }
                    if self.pending.len() >= MAX_PENDING_WRITES {
                        return self.err(tag, p9::E_NOMEM);
                    }
                    self.pending.push(PendingWrite {
                        tag,
                        fid: a.fid,
                        data: a.data.to_vec(),
                        done: n,
                    });
                } else {
                    if self.pending.len() >= MAX_PENDING_WRITES {
                        return self.err(tag, p9::E_NOMEM);
                    }
                    self.pending.push(PendingWrite {
                        tag,
                        fid: a.fid,
                        data: a.data.to_vec(),
                        done: 0,
                    });
                }
                self.defer = true;
                Ok(0) // ignored: dispatch returns Disp::Deferred
            }
            _ => self.err(tag, p9::E_PERM),
        }
    }

    fn h_readdir(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_treaddir(tmsg) {
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
        if f.path != P_ROOT {
            return self.err(tag, p9::E_NOTDIR);
        }
        let budget = (a.count as usize).min((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4));
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;
        for (name, path, _) in CHILDREN {
            ord += 1;
            if ord <= a.offset {
                continue;
            }
            if data.len() + p9::dirent_len(name.len()) > budget {
                break;
            }
            let mut scratch = [0u8; 64 + p9::P9_QID_LEN + 8 + 1 + 2];
            match p9::pack_dirent(&mut scratch, 0, &Conn::qid_of(path), ord, p9::DT_REG, name) {
                Ok(used) => data.extend_from_slice(&scratch[..used]),
                Err(()) => break,
            }
        }
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
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
        let (mode, nlink) = if f.path == P_ROOT { (DIR_MODE, 2u64) } else { (mode_of(f.path), 1u64) };
        // The security trio must be filled: dev9p's per-component X-search reads
        // it, and an unfilled trio fails closed (the /dev/pts lesson).
        let valid = p9::P9_GETATTR_MODE | p9::P9_GETATTR_NLINK | p9::P9_GETATTR_UID | p9::P9_GETATTR_GID | P9_GETATTR_SIZE;
        p9::build_rgetattr(&mut self.out_buf, tag, valid, &Conn::qid_of(f.path), mode, 0, 0, nlink, 0)
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        match self.fid_find(a.fid) {
            Some(i) => {
                self.fids[i] = None;
                self.pending.retain(|pw| pw.fid != a.fid);
                p9::build_rclunk(&mut self.out_buf, tag)
            }
            None => self.err(tag, p9::E_BADF),
        }
    }

    /// Tflush(oldtag): cancel a parked write with that tag (its bytes already
    /// accepted stay queued -- Plan 9 semantics: what was buffered plays).
    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        self.pending.retain(|pw| pw.tag != a.oldtag);
        p9::build_rflush(&mut self.out_buf, tag)
    }
}
