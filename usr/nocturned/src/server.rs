// The /srv/nocturne 9P tree (docs/NOCTURNE.md section 6.4). N-2a-1 grows the
// N-1 heritage floor into the graph core's first half: multiple VOICES mixed to
// the one sink.
//
//   / { ctl, info, audio, nodes/ }
//   nodes/new                 open -> mints a voice owned by this connection;
//                             read the same fid -> the new voice's id (decimal)
//   nodes/<id>/ { audio, ctl, info }
//
// `audio` at the root is voice 0 -- a persistent default voice, so `bind
// /dev/nocturne/audio /dev/audio` and the Plan 9 audio(3) shape still work.
// Every voice carries a bounded S16LE-stereo FIFO; a write that fills it PARKS
// (its Rwrite deferred until the mixer drains room -- Plan 9's blocking write).
// The device pump pulls one period at a time via `next_period`, which MIXES all
// voices in float32 with per-voice gain, clamps to S16, and hands the sink one
// period. A voice created through nodes/new dies with the connection that made
// it (the tapestry surface-lifetime idiom); voice 0 is never removed.
//
// The internal graph is byte-copy at N-2a-1 -- the designed fallback below the
// Weft hybrid threshold (section 6.5). The per-node Weft ring (SYS_WEFT_SHARE ->
// Tweft on nodes/<id>/data), ports, links and descants are N-2b / N-4.
//
// Framing + dispatch + the parked-write / Tflush machinery mirror
// usr/ptyfs/src/server.rs and are preserved verbatim from the N-1 audit.

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
/// ~340 ms of S16LE stereo at 48 kHz, per voice; the write-side backlog beyond
/// the four periods the device holds.
const FIFO_CAP: usize = 64 * 1024;
/// The mixer bound: voice 0 (persistent) + up to 15 client voices. Each voice's
/// FIFO_CAP is charged only as it fills, so the ceiling is a DoS bound, not a
/// reservation.
const MAX_VOICES: usize = 16;
/// Bytes per stereo S16 frame.
const FRAME: usize = 4;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const P9_GETATTR_SIZE: u64 = 0x200;

// Root paths.
const P_ROOT: u64 = 0;
const P_CTL: u64 = 1;
const P_INFO: u64 = 2;
const P_AUDIO: u64 = 3;
const P_NODES: u64 = 4;
const P_NODES_NEW: u64 = 5;

// Voice paths: VBIT | (id << 4) | leaf. Leaf 0 = the voice dir; 1/2/3 = the
// audio/ctl/info files. VBIT is above the 6 fixed root paths and clear of any
// realistic voice id (id < 2^28).
const VBIT: u64 = 1 << 32;
const VLEAF_DIR: u64 = 0;
const VLEAF_AUDIO: u64 = 1;
const VLEAF_CTL: u64 = 2;
const VLEAF_INFO: u64 = 3;

fn vpath(id: u32, leaf: u64) -> u64 {
    VBIT | ((id as u64) << 4) | leaf
}
fn is_voice(path: u64) -> bool {
    path & VBIT != 0
}
fn vid(path: u64) -> u32 {
    ((path & 0xFFFF_FFFF) >> 4) as u32
}
fn vleaf(path: u64) -> u64 {
    path & 0xF
}

// Root directory children (name, path, mode).
const ROOT_CHILDREN: [(&[u8], u64, u32); 4] = [
    (b"ctl", P_CTL, S_IFREG | 0o644),
    (b"info", P_INFO, S_IFREG | 0o444),
    (b"audio", P_AUDIO, S_IFREG | 0o666),
    (b"nodes", P_NODES, S_IFDIR | 0o555),
];
// nodes/ directory children (only the static `new`; voices are listed dynamically).
const NODES_STATIC: [(&[u8], u64, u32); 1] = [(b"new", P_NODES_NEW, S_IFREG | 0o666)];
// A voice directory's children (name, leaf, mode).
const VOICE_CHILDREN: [(&[u8], u64, u32); 3] = [
    (b"audio", VLEAF_AUDIO, S_IFREG | 0o666),
    (b"ctl", VLEAF_CTL, S_IFREG | 0o644),
    (b"info", VLEAF_INFO, S_IFREG | 0o444),
];

fn mode_of(path: u64) -> u32 {
    if is_voice(path) {
        match vleaf(path) {
            VLEAF_DIR => DIR_MODE,
            leaf => VOICE_CHILDREN
                .iter()
                .find(|(_, l, _)| *l == leaf)
                .map(|(_, _, m)| *m)
                .unwrap_or(S_IFREG | 0o444),
        }
    } else {
        for (_, p, m) in ROOT_CHILDREN {
            if p == path {
                return m;
            }
        }
        for (_, p, m) in NODES_STATIC {
            if p == path {
                return m;
            }
        }
        DIR_MODE
    }
}

fn is_dir(path: u64) -> bool {
    path == P_ROOT || path == P_NODES || (is_voice(path) && vleaf(path) == VLEAF_DIR)
}

/// One mixer input: an independent S16LE-stereo stream with its own bounded
/// FIFO and gain. Voice 0 is the persistent default (the root `audio` file);
/// every other voice is owned by the connection that minted it.
struct Voice {
    id: u32,
    fifo: VecDeque<u8>,
    /// Linear gain (1.0 = unity); set via `ctl gain <percent>`.
    gain: f32,
    /// The connection handle that minted this voice, or -1 for the persistent
    /// voice 0. Every voice a connection owns is dropped when it closes.
    owner: i64,
    bytes_in: u64,
    flushes: u64,
}

impl Voice {
    fn new(id: u32, owner: i64) -> Voice {
        Voice {
            id,
            fifo: VecDeque::new(),
            gain: 1.0,
            owner,
            bytes_in: 0,
            flushes: 0,
        }
    }
}

/// State shared by every connection and the device pump (single-threaded at
/// N-2a-1: the serve loop is one thread; the cycle/control split is N-2c).
pub struct Shared {
    voices: Vec<Voice>,
    next_id: u32,
    pub stats: Stats,
    pub started: bool,
}

impl Shared {
    pub fn new() -> Shared {
        let mut voices = Vec::with_capacity(MAX_VOICES);
        voices.push(Voice::new(0, -1)); // the persistent default voice
        Shared {
            voices,
            next_id: 1,
            stats: Stats::default(),
            started: false,
        }
    }

    fn voice_pos(&self, id: u32) -> Option<usize> {
        self.voices.iter().position(|v| v.id == id)
    }

    /// Total buffered bytes across every voice (the device idle-stop reads this).
    pub fn fifo_len(&self) -> usize {
        self.voices.iter().map(|v| v.fifo.len()).sum()
    }

    /// Clear every voice's FIFO. Used when the device stream fails to start:
    /// the backlog cannot play, so drop it rather than wedge the idle-stop.
    pub fn drop_fifo(&mut self) {
        for v in self.voices.iter_mut() {
            v.fifo.clear();
        }
    }

    /// Mint a voice owned by `conn`; returns its id, or None at the cap.
    fn mint_voice(&mut self, conn: i64) -> Option<u32> {
        if self.voices.len() >= MAX_VOICES {
            return None;
        }
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1);
        // wrapping_add is defensive only: MAX_VOICES caps live voices at 16, so
        // 2^32 mints (id reuse) is unreachable; guard against a live collision
        // regardless, so a wrapped id can never alias a living voice.
        if id == 0 || self.voice_pos(id).is_some() {
            return None;
        }
        self.voices.push(Voice::new(id, conn));
        Some(id)
    }

    /// Drop every voice a closing connection owned (never voice 0).
    pub fn drop_conn_voices(&mut self, conn: i64) {
        self.voices.retain(|v| v.id == 0 || v.owner != conn);
    }

    fn drop_fifo_voice(&mut self, id: u32) {
        if let Some(i) = self.voice_pos(id) {
            self.voices[i].fifo.clear();
            self.voices[i].flushes = self.voices[i].flushes.saturating_add(1);
        }
    }

    /// Mix one period from every voice into `buf` (S16LE stereo). Each voice
    /// contributes whole frames in float32 scaled by its gain; the sum is
    /// clamped to the S16 range. An empty voice contributes silence. Returns
    /// true if ANY voice supplied real data (false = pure silence, which the
    /// idle-stop counts).
    ///
    /// I-14 posture at the graph layer: the accumulator is f32 so N unity
    /// voices cannot integer-overflow the sink sample; the clamp is the only
    /// place a hot mix is bounded, exactly once.
    pub fn next_period(&mut self, buf: &mut [u8]) -> bool {
        let nsamp = buf.len() / 2; // i16 samples (2 per frame)
        // A fixed scratch sized to the period; buf is always PERIOD_BYTES.
        let mut mix = [0f32; PERIOD_BYTES / 2];
        let mix = &mut mix[..nsamp];
        let mut any = false;
        for v in self.voices.iter_mut() {
            // Whole frames only: a torn frame would shift the channel phase.
            let have = v.fifo.len().min(buf.len());
            let have = have - (have % FRAME);
            if have == 0 {
                continue;
            }
            any = true;
            let g = v.gain;
            let samples = have / 2;
            for m in mix.iter_mut().take(samples) {
                let lo = v.fifo.pop_front().unwrap_or(0) as u16;
                let hi = v.fifo.pop_front().unwrap_or(0) as u16;
                let s = (lo | (hi << 8)) as i16;
                *m += s as f32 * g;
            }
        }
        for (i, m) in mix.iter().enumerate() {
            let clamped = if *m > 32767.0 {
                32767i16
            } else if *m < -32768.0 {
                -32768i16
            } else {
                *m as i16
            };
            let b = (clamped as u16).to_le_bytes();
            buf[2 * i] = b[0];
            buf[2 * i + 1] = b[1];
        }
        // If buf held an odd trailing byte (never, PERIOD_BYTES is even), leave
        // it zeroed by the caller's cleared buffer.
        let _ = nsamp;
        any
    }

    /// Append `data` to voice `id`; returns the count accepted (0 if the voice
    /// is gone or its FIFO is full).
    fn push(&mut self, id: u32, data: &[u8]) -> usize {
        let i = match self.voice_pos(id) {
            Some(i) => i,
            None => return 0,
        };
        let v = &mut self.voices[i];
        let room = FIFO_CAP.saturating_sub(v.fifo.len());
        let n = data.len().min(room);
        v.fifo.extend(data[..n].iter().copied());
        v.bytes_in = v.bytes_in.saturating_add(n as u64);
        n
    }

    fn set_gain(&mut self, id: u32, percent: u32) -> bool {
        if let Some(i) = self.voice_pos(id) {
            // Plan 9 volume(3) grammar: 0..100 is the ordinary range; allow up
            // to 1000% for headroom, clamped so a hostile value cannot blow the
            // mix past the f32 clamp's usefulness.
            let p = percent.min(1000);
            self.voices[i].gain = p as f32 / 100.0;
            return true;
        }
        false
    }

    fn render_info(&self, out: &mut Vec<u8>) {
        let s = &self.stats;
        let buffered = self.fifo_len() as u64 + u64::from(s.last_latency_bytes);
        let text = alloc::format!(
            "device virtio-snd stream 0 playback\nformat s16c2r{}\nvoices {}\nbufsize {}\nbuffered {}\nperiod-bytes {}\nbuffer-bytes {}\nperiods {}\nstarted {}\nperiods-played {}\nsilence-periods {}\ntx-errors {}\nbad-used {}\nlatency-bytes {}\n",
            RATE_HZ,
            self.voices.len(),
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
        );
        out.extend_from_slice(text.as_bytes());
    }

    fn render_voice_info(&self, id: u32, out: &mut Vec<u8>) {
        if let Some(i) = self.voice_pos(id) {
            let v = &self.voices[i];
            let text = alloc::format!(
                "voice {}\ngain {}\nbuffered {}\nbytes-in {}\nflushes {}\nowner {}\n",
                v.id,
                (v.gain * 100.0) as u32,
                v.fifo.len(),
                v.bytes_in,
                v.flushes,
                v.owner,
            );
            out.extend_from_slice(text.as_bytes());
        }
    }

    fn render_ctl(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(b"nocturne n-2a: mixed voices; write s16le stereo 48000 Hz to a voice's audio; ctl: flush; per-voice ctl: gain <percent>, flush, remove\n");
    }
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
    /// The voice minted when this fid opened nodes/new; -1 if this is not a
    /// freshly-minted new fid. A read of such a fid returns the id decimal.
    minted: i64,
}

enum Disp {
    Reply(usize),
    Deferred,
    Fatal,
}

/// A Twrite to a voice's audio that found the FIFO full: the bytes not yet
/// accepted, the target voice, and the tag whose Rwrite is owed once they are.
struct PendingWrite {
    tag: u16,
    fid: u32,
    voice: u32,
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

    pub fn teardown(&mut self, sh: &mut Shared) {
        for slot in self.fids.iter_mut() {
            *slot = None;
        }
        self.pending.clear();
        // Every voice this connection minted dies with it.
        sh.drop_conn_voices(self.handle);
    }

    /// Retry the parked writes in order; a fully-accepted one gets its Rwrite.
    /// False if the connection's reply write failed (close it).
    pub fn poll_writes(&mut self, sh: &mut Shared) -> bool {
        while !self.pending.is_empty() {
            let (tag, total, finished) = {
                let pw = &mut self.pending[0];
                let n = sh.push(pw.voice, &pw.data[pw.done..]);
                pw.done += n;
                // A voice that vanished under a parked write (its conn is us,
                // so this cannot happen for our own voice; defensive) drains as
                // accepted so the reply is not stuck forever.
                let gone = sh.voice_pos(pw.voice).is_none();
                (pw.tag, pw.data.len(), gone || pw.done >= pw.data.len())
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
            self.fids[i] = Some(Fid { fid, path, opened: false, minted: -1 });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid { fid, path, opened: false, minted: -1 });
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
            p9::P9_TWALK => self.h_walk(sh, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(sh, tmsg, tag),
            p9::P9_TREAD => self.h_read(sh, tmsg, tag),
            p9::P9_TWRITE => self.h_write(sh, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(sh, tmsg, tag),
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
            kind: if is_dir(path) { p9::P9_QTDIR } else { p9::P9_QTFILE },
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

    /// Resolve one path component from `cur`. Returns the child path, or None.
    fn walk_child(sh: &Shared, cur: u64, name: &[u8]) -> Option<u64> {
        if name == b".." || name == b"." {
            // ".." off a voice leaf/dir climbs to nodes/, off nodes/ to root.
            return Some(match cur {
                P_ROOT => P_ROOT,
                P_NODES => P_ROOT,
                _ if is_voice(cur) && vleaf(cur) == VLEAF_DIR => P_NODES,
                _ if is_voice(cur) => vpath(vid(cur), VLEAF_DIR),
                _ => P_ROOT,
            });
        }
        match cur {
            P_ROOT => ROOT_CHILDREN
                .iter()
                .find(|(nm, _, _)| *nm == name)
                .map(|(_, p, _)| *p),
            P_NODES => {
                if let Some((_, p, _)) = NODES_STATIC.iter().find(|(nm, _, _)| *nm == name) {
                    return Some(*p);
                }
                // A decimal voice id that names a live voice.
                let id = parse_u32(name)?;
                if sh.voice_pos(id).is_some() {
                    Some(vpath(id, VLEAF_DIR))
                } else {
                    None
                }
            }
            _ if is_voice(cur) && vleaf(cur) == VLEAF_DIR => {
                let id = vid(cur);
                VOICE_CHILDREN
                    .iter()
                    .find(|(nm, _, _)| *nm == name)
                    .map(|(_, leaf, _)| vpath(id, *leaf))
            }
            _ => None,
        }
    }

    fn h_walk(&mut self, sh: &mut Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
            match Conn::walk_child(sh, cur, a.names[k]) {
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
        if n == a.nwname as usize && !self.fid_set(a.newfid, cur) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..n])
    }

    fn h_lopen(&mut self, sh: &mut Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        // Opening nodes/new MINTS a voice owned by this connection; the fid
        // remembers the id so a read returns it (the tapestry surface/new idiom).
        let minted = if f.path == P_NODES_NEW {
            match sh.mint_voice(self.handle) {
                Some(id) => id as i64,
                None => return self.err(tag, p9::E_NOMEM),
            }
        } else {
            -1
        };
        self.fids[i] = Some(Fid {
            fid: f.fid,
            path: f.path,
            opened: true,
            minted,
        });
        p9::build_rlopen(&mut self.out_buf, tag, &Conn::qid_of(f.path), 0)
    }

    fn read_text(&mut self, tag: u16, off: u64, count: u32, text: &[u8]) -> Result<usize, ()> {
        let off = off as usize;
        if off >= text.len() {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
        let k = (text.len() - off).min(count as usize).min(cap);
        p9::build_rread(&mut self.out_buf, tag, &text[off..off + k])
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
        // A freshly-minted nodes/new fid: its read is the new voice's id.
        if f.path == P_NODES_NEW && f.minted >= 0 {
            let text = alloc::format!("{}\n", f.minted);
            return self.read_text(tag, a.offset, a.count, text.as_bytes());
        }
        if is_dir(f.path) {
            return self.err(tag, p9::E_ISDIR);
        }
        // audio(3): an output-only device returns zero when read (root + voice).
        if f.path == P_AUDIO || (is_voice(f.path) && vleaf(f.path) == VLEAF_AUDIO) {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        let mut text: Vec<u8> = Vec::new();
        if is_voice(f.path) {
            match vleaf(f.path) {
                VLEAF_INFO => sh.render_voice_info(vid(f.path), &mut text),
                VLEAF_CTL => sh.render_ctl(&mut text),
                _ => {}
            }
        } else if f.path == P_INFO {
            sh.render_info(&mut text);
        } else {
            sh.render_ctl(&mut text);
        }
        self.read_text(tag, a.offset, a.count, &text)
    }

    /// A ctl verb line (`flush`, `gain <n>`, `remove`) applied to `voice`.
    /// Returns Ok(true) if the verb is known + accepted, Ok(false) if unknown.
    fn apply_ctl(sh: &mut Shared, voice: u32, data: &[u8]) -> bool {
        // First token.
        let vend = data
            .iter()
            .position(|&b| b == b' ' || b == b'\n')
            .unwrap_or(data.len());
        let verb = &data[..vend];
        if verb == b"flush" {
            sh.drop_fifo_voice(voice);
            true
        } else if verb == b"remove" {
            // Never remove voice 0; the connection's teardown reaps the rest,
            // but an explicit remove is allowed for a client that is done.
            if voice != 0 {
                sh.voices.retain(|v| v.id != voice);
            }
            true
        } else if verb == b"gain" {
            // gain <percent>
            let rest = &data[vend..];
            let start = rest.iter().position(|&b| b != b' ').unwrap_or(rest.len());
            match parse_u32(trim_line(&rest[start..])) {
                Some(p) => sh.set_gain(voice, p),
                None => false,
            }
        } else {
            false
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

        // Which voice, if any, does this fid write audio into?
        let audio_voice: Option<u32> = if f.path == P_AUDIO {
            Some(0)
        } else if is_voice(f.path) && vleaf(f.path) == VLEAF_AUDIO {
            Some(vid(f.path))
        } else {
            None
        };

        // ctl (root or per-voice).
        let ctl_voice: Option<u32> = if f.path == P_CTL {
            Some(0) // root ctl acts on voice 0 (the default)
        } else if is_voice(f.path) && vleaf(f.path) == VLEAF_CTL {
            Some(vid(f.path))
        } else {
            None
        };

        if let Some(voice) = ctl_voice {
            // Root ctl historically accepts only `flush`; a per-voice ctl adds
            // gain/remove. Route both through apply_ctl (root ctl's `remove`
            // no-ops on voice 0 by the guard above).
            return if Conn::apply_ctl(sh, voice, a.data) {
                p9::build_rwrite(&mut self.out_buf, tag, a.data.len() as u32)
            } else {
                self.err(tag, p9::E_INVAL)
            };
        }

        if let Some(voice) = audio_voice {
            if a.data.is_empty() {
                return p9::build_rwrite(&mut self.out_buf, tag, 0);
            }
            if sh.voice_pos(voice).is_none() {
                return self.err(tag, p9::E_BADF);
            }
            // Order matters: a write behind a parked one must queue behind it.
            if self.pending.is_empty() {
                let n = sh.push(voice, a.data);
                if n == a.data.len() {
                    return p9::build_rwrite(&mut self.out_buf, tag, n as u32);
                }
                if self.pending.len() >= MAX_PENDING_WRITES {
                    return self.err(tag, p9::E_NOMEM);
                }
                self.pending.push(PendingWrite {
                    tag,
                    fid: a.fid,
                    voice,
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
                    voice,
                    data: a.data.to_vec(),
                    done: 0,
                });
            }
            self.defer = true;
            return Ok(0); // ignored: dispatch returns Disp::Deferred
        }

        self.err(tag, p9::E_PERM)
    }

    fn h_readdir(&mut self, sh: &mut Shared, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        let budget = (a.count as usize).min((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4));
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;

        // Assemble the entry list for this directory: (name-bytes, path, dt).
        let mut push_entry = |name: &[u8], path: u64, ord: &mut u64| -> bool {
            *ord += 1;
            if *ord <= a.offset {
                return true;
            }
            if data.len() + p9::dirent_len(name.len()) > budget {
                return false;
            }
            let dt = if is_dir(path) { p9::DT_DIR } else { p9::DT_REG };
            let mut scratch = [0u8; 64 + p9::P9_QID_LEN + 8 + 1 + 2];
            match p9::pack_dirent(&mut scratch, 0, &Conn::qid_of(path), *ord, dt, name) {
                Ok(used) => {
                    data.extend_from_slice(&scratch[..used]);
                    true
                }
                Err(()) => false,
            }
        };

        match f.path {
            P_ROOT => {
                for (name, path, _) in ROOT_CHILDREN {
                    if !push_entry(name, path, &mut ord) {
                        break;
                    }
                }
            }
            P_NODES => {
                for (name, path, _) in NODES_STATIC {
                    if !push_entry(name, path, &mut ord) {
                        break;
                    }
                }
                // The live voices, by decimal id (voice 0 included -- it is
                // reachable as both /audio and /nodes/0).
                let mut buf16 = [0u8; 16];
                for v in sh.voices.iter() {
                    let name = fmt_u32(v.id, &mut buf16);
                    if !push_entry(name, vpath(v.id, VLEAF_DIR), &mut ord) {
                        break;
                    }
                }
            }
            _ if is_voice(f.path) && vleaf(f.path) == VLEAF_DIR => {
                for (name, leaf, _) in VOICE_CHILDREN {
                    if !push_entry(name, vpath(vid(f.path), leaf), &mut ord) {
                        break;
                    }
                }
            }
            _ => return self.err(tag, p9::E_NOTDIR),
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
        let (mode, nlink) = if is_dir(f.path) {
            (mode_of(f.path), 2u64)
        } else {
            (mode_of(f.path), 1u64)
        };
        // The security trio must be filled: dev9p's per-component X-search reads
        // it, and an unfilled trio fails closed (the /dev/pts lesson).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
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

/// Parse an unsigned decimal from a byte slice (no leading/trailing space).
/// Returns None on empty or a non-digit.
fn parse_u32(b: &[u8]) -> Option<u32> {
    if b.is_empty() {
        return None;
    }
    let mut n: u32 = 0;
    for &c in b {
        if !c.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((c - b'0') as u32)?;
    }
    Some(n)
}

/// Trim a trailing newline / spaces from a line for token parsing.
fn trim_line(b: &[u8]) -> &[u8] {
    let mut end = b.len();
    while end > 0 && (b[end - 1] == b'\n' || b[end - 1] == b' ' || b[end - 1] == b'\r') {
        end -= 1;
    }
    &b[..end]
}

/// Format a u32 into `buf`, returning the used slice (decimal, no NUL).
fn fmt_u32(mut v: u32, buf: &mut [u8; 16]) -> &[u8] {
    if v == 0 {
        buf[0] = b'0';
        return &buf[..1];
    }
    let mut tmp = [0u8; 10];
    let mut n = 0;
    while v > 0 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    for i in 0..n {
        buf[i] = tmp[n - 1 - i];
    }
    &buf[..n]
}
