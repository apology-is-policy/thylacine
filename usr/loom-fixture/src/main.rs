// /bin/loom-fixture -- the strict 9P2000.L server of the private Loom guest
// qualification (docs/ASYNC-SERVICE-ABI.md, NA-4d). The kernel tests
// loom.native_probe_rust and loom.native_probe_c start it beside a probe and pin
// its exit status by image.
//
// fd 0 is the walk-only root of a registry the kernel test made for this run,
// fd 1 the read end of the probe's control pipe, and fd 2 the write end of the
// notify pipe. It posts "fixture" in the registry, writes 'R' to fd 2, and
// serves every connection until fd 1 reaches EOF. Every session must then end
// within SETTLE_NS: the probe has exited, so its rings are closed. A 'Q' read
// from fd 1 asks for one 'Z' on fd 2 once no session is left. Nothing else is
// ever written there: a probe sleeps by polling fd 2 while it stays empty.
//
// Strict: each message below is one the kernel's client never sends, and it
// counts a violation. Most answer Rlerror: anything before Tversion, a second
// Tversion (9P allows one; the kernel never re-versions a session), NOTAG on
// another message, a frame whose size is not its body's, an unknown, reused or
// NOFID fid, a walk from an opened fid or from a file, a walk of "." or "..",
// an open of an opened fid, I/O on a fid not opened for it, a tag still in
// use, an open the node's mode refuses, a Tread count above msize less 11, and
// a WALK with a name or an LOPEN on a fid with no Tgetattr first (the private
// engine checks permission with one before either). The Tgetattr credit is
// per fid: the wire carries no operation identity, so a credit the kernel's
// own permission refusal left unspent can pay for a later LOPEN. A Tversion
// that is not 9P2000.L answers Rversion "unknown", and a frame that cannot be
// parsed, or whose size is out of range, ends its session.
//
// Exit 76: no session was accepted (the default image). 77: at least one
// session, every one ended, no violation. 1: a violation or a setup failure.
// 2: a session outlived the probe.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::format;
use alloc::vec::Vec;
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_poll, t_putstr, t_read, t_srv_accept, t_walk_create, t_write, TPollFd, T_OREAD,
    T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

const REGISTRY: i64 = 0;
const CONTROL: i64 = 1;
const NOTIFY: i64 = 2;
const NAME: &[u8] = b"fixture";
const MSIZE: u32 = 32768;
const SETTLE_NS: u64 = 5_000_000_000;

// Qid paths double as node numbers.
const ROOT: u64 = 1;
const DATA: u64 = 2;
const SINK: u64 = 3;
const STALL: u64 = 4;
const STATS: u64 = 5;
const STREAM: u64 = 6;
const DIR: u64 = 7;
const LEAF: u64 = 8;
const ROOT_NODES: [(&[u8], u64); 6] = [
    (b"data", DATA),
    (b"dir", DIR),
    (b"sink", SINK),
    (b"stall", STALL),
    (b"stats", STATS),
    (b"stream", STREAM),
];
const DIR_NODES: [(&[u8], u64); 1] = [(b"leaf", LEAF)];

const DATA_LEN: u64 = 3 * 4096 + 1000;
// Shorter than a page, so a WALK that reached `data` instead reads longer.
const LEAF_LEN: u64 = 600;
// A multishot read repeats its offset, as a read of a pipe would, so `stream`
// ignores the offset: each read takes the next bytes from the fid's cursor, and
// a read at the end answers 0.
const STREAM_LEN: u64 = 2600;
const SINK_MAX: u64 = 8192;
const P9_GETATTR_SIZE: u64 = 0x200;
const E_ACCES: u32 = 13;

// The stats file: one little-endian u32 per counter, in this order. The probes
// carry the same indices.
const ACCEPTED: usize = 0;
const ENDED: usize = 1;
const VIOLATIONS: usize = 2;
const STALLED: usize = 3;
const FLUSHES: usize = 4;
const CLUNKS: usize = 5;
const GETATTRS: usize = 6;
const WALKS: usize = 7;
const OPENS: usize = 8;
const READS: usize = 9;
const WRITES: usize = 10;
const STAT_COUNT: usize = 11;

// A period of 251 does not divide a page, so a shot delivered at the wrong
// offset reads as wrong bytes.
fn data_byte(at: u64) -> u8 {
    (at % 251) as u8
}

fn say(line: &str) {
    t_putstr(&format!("loom-fixture: {}\n", line));
}

fn now() -> u64 {
    libthyla_rs::time::monotonic_ns()
}

fn is_dir(node: u64) -> bool {
    node == ROOT || node == DIR
}

fn children(node: u64) -> &'static [(&'static [u8], u64)] {
    match node {
        ROOT => &ROOT_NODES,
        DIR => &DIR_NODES,
        _ => &[],
    }
}

fn mode_of(node: u64) -> u32 {
    match node {
        ROOT | DIR => 0o040555,
        SINK => 0o100666,
        _ => 0o100444,
    }
}

fn qid_of(node: u64) -> p9::Qid {
    p9::Qid {
        kind: if is_dir(node) { p9::P9_QTDIR } else { p9::P9_QTFILE },
        version: 0,
        path: node,
    }
}

struct Fid {
    node: u64,
    open: Option<u32>,
    // Tgetattrs not yet spent by a WALK with a name or an LOPEN.
    stats: u32,
    cursor: u64,
}

struct Conn {
    fd: i64,
    inb: Vec<u8>,
    versioned: bool,
    msize: u32,
    fids: BTreeMap<u32, Fid>,
    stalled: Vec<u16>,
}

struct Server {
    count: [u32; STAT_COUNT],
    sink: Vec<u8>,
    out: Vec<u8>,
}

enum Reply {
    Send(usize),
    Held,
    Fatal,
}

// The exact frame length of a message this server answers, from its parsed
// fields; None for a type it refuses anyway.
fn body_len(mtype: u8, frame: &[u8]) -> Result<Option<usize>, ()> {
    let h = p9::P9_HDR_LEN;
    Ok(Some(match mtype {
        p9::P9_TVERSION => h + 4 + 2 + p9::parse_tversion(frame)?.version.len(),
        p9::P9_TATTACH => {
            let a = p9::parse_tattach(frame)?;
            h + 4 + 4 + 2 + a.uname.len() + 2 + a.aname.len() + 4
        }
        p9::P9_TGETATTR => h + 4 + 8,
        p9::P9_TWALK => {
            let a = p9::parse_twalk(frame)?;
            h + 4 + 4 + 2 + a.names[..a.nwname as usize].iter().map(|n| 2 + n.len()).sum::<usize>()
        }
        p9::P9_TLOPEN => h + 4 + 4,
        p9::P9_TREAD => h + 4 + 8 + 4,
        p9::P9_TWRITE => h + 4 + 8 + 4 + p9::parse_twrite(frame)?.data.len(),
        p9::P9_TCLUNK => h + 4,
        p9::P9_TFLUSH => h + 2,
        _ => return Ok(None),
    }))
}

impl Server {
    fn violation(&mut self, what: &str) {
        self.count[VIOLATIONS] += 1;
        say(&format!("violation: {}", what));
    }

    fn lerror(&mut self, tag: u16, code: u32) -> Reply {
        match p9::build_rlerror(&mut self.out, tag, code) {
            Ok(n) => Reply::Send(n),
            Err(_) => Reply::Fatal,
        }
    }

    fn refuse(&mut self, tag: u16, code: u32, what: &str) -> Reply {
        self.violation(what);
        self.lerror(tag, code)
    }

    fn built(r: Result<usize, ()>) -> Reply {
        match r {
            Ok(n) => Reply::Send(n),
            Err(_) => Reply::Fatal,
        }
    }

    fn dispatch(&mut self, c: &mut Conn, frame: &[u8], h: p9::Header) -> Reply {
        self.out.clear();
        self.out.resize(MSIZE as usize, 0);
        if c.stalled.contains(&h.tag) {
            return self.refuse(h.tag, p9::E_PROTO, "a tag still in use");
        }
        if !c.versioned && h.mtype != p9::P9_TVERSION {
            return self.refuse(h.tag, p9::E_PROTO, "a message before Tversion");
        }
        if h.tag == p9::P9_NOTAG && h.mtype != p9::P9_TVERSION {
            return self.refuse(h.tag, p9::E_PROTO, "NOTAG on a message other than Tversion");
        }
        match body_len(h.mtype, frame) {
            Ok(Some(want)) if want != frame.len() => {
                return self.refuse(h.tag, p9::E_PROTO, "a frame whose size is not its body's");
            }
            Err(()) => return Reply::Fatal,
            _ => {}
        }
        match h.mtype {
            p9::P9_TVERSION => self.version(c, frame, h.tag),
            p9::P9_TATTACH => self.attach(c, frame, h.tag),
            p9::P9_TGETATTR => self.getattr(c, frame, h.tag),
            p9::P9_TWALK => self.walk(c, frame, h.tag),
            p9::P9_TLOPEN => self.lopen(c, frame, h.tag),
            p9::P9_TREAD => self.read(c, frame, h.tag),
            p9::P9_TWRITE => self.write(c, frame, h.tag),
            p9::P9_TCLUNK => self.clunk(c, frame, h.tag),
            p9::P9_TFLUSH => self.flush(c, frame, h.tag),
            _ => self.refuse(h.tag, p9::E_OPNOTSUPP, "an unexpected message type"),
        }
    }

    fn version(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tversion(frame) else { return Reply::Fatal };
        if c.versioned {
            return self.refuse(tag, p9::E_PROTO, "a second Tversion");
        }
        if a.version != b"9P2000.L" || a.msize < 4096 {
            self.violation("a Tversion that is not 9P2000.L with msize >= 4096");
            return Self::built(p9::build_rversion(&mut self.out, tag, MSIZE, b"unknown"));
        }
        c.versioned = true;
        c.msize = a.msize.min(MSIZE);
        Self::built(p9::build_rversion(&mut self.out, tag, c.msize, b"9P2000.L"))
    }

    fn attach(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tattach(frame) else { return Reply::Fatal };
        if a.afid != p9::P9_NOFID {
            return self.refuse(tag, p9::E_PROTO, "a Tattach with an auth fid");
        }
        if a.fid == p9::P9_NOFID {
            return self.refuse(tag, p9::E_BADF, "Tattach to NOFID");
        }
        if c.fids.contains_key(&a.fid) {
            return self.refuse(tag, p9::E_BADF, "Tattach to a fid in use");
        }
        c.fids.insert(a.fid, Fid { node: ROOT, open: None, stats: 0, cursor: 0 });
        Self::built(p9::build_rattach(&mut self.out, tag, &qid_of(ROOT)))
    }

    fn getattr(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(fid) = p9::parse_tgetattr(frame) else { return Reply::Fatal };
        self.count[GETATTRS] += 1;
        let Some(f) = c.fids.get_mut(&fid) else {
            return self.refuse(tag, p9::E_BADF, "Tgetattr of an unknown fid");
        };
        f.stats += 1;
        let node = f.node;
        let size = match node {
            DATA => DATA_LEN,
            LEAF => LEAF_LEN,
            STREAM => STREAM_LEN,
            SINK => self.sink.len() as u64,
            _ => 0,
        };
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        let nlink = if is_dir(node) { 2 } else { 1 };
        Self::built(p9::build_rgetattr(
            &mut self.out, tag, valid, &qid_of(node), mode_of(node), 0, 0, nlink, size,
        ))
    }

    fn walk(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_twalk(frame) else { return Reply::Fatal };
        self.count[WALKS] += 1;
        let Some(src) = c.fids.get_mut(&a.fid) else {
            return self.refuse(tag, p9::E_BADF, "Twalk from an unknown fid");
        };
        if src.open.is_some() {
            return self.refuse(tag, p9::E_BADF, "Twalk from an opened fid");
        }
        let node = src.node;
        if a.nwname > 0 {
            if src.stats == 0 {
                return self.refuse(tag, E_ACCES, "Twalk with a name and no Tgetattr first");
            }
            src.stats -= 1;
        }
        if a.newfid == p9::P9_NOFID {
            return self.refuse(tag, p9::E_BADF, "Twalk to NOFID");
        }
        if a.newfid != a.fid && c.fids.contains_key(&a.newfid) {
            return self.refuse(tag, p9::E_BADF, "Twalk to a newfid in use");
        }
        if a.nwname > 0 && !is_dir(node) {
            return self.refuse(tag, p9::E_NOTDIR, "Twalk with a name from a file");
        }
        // The kernel's builder refuses both names (loom_service_wire.c).
        if a.names[..a.nwname as usize].iter().any(|n| *n == b"." || *n == b"..") {
            return self.refuse(tag, p9::E_INVAL, "a Twalk of . or ..");
        }
        let mut qids = Vec::new();
        let mut at = node;
        for i in 0..a.nwname as usize {
            if !is_dir(at) {
                break;
            }
            match children(at).iter().find(|(n, _)| *n == a.names[i]) {
                Some(&(_, next)) => {
                    at = next;
                    qids.push(qid_of(next));
                }
                None => break,
            }
        }
        if a.nwname > 0 && qids.is_empty() {
            return self.lerror(tag, p9::E_NOENT);
        }
        if qids.len() == a.nwname as usize {
            c.fids.insert(a.newfid, Fid { node: at, open: None, stats: 0, cursor: 0 });
        }
        Self::built(p9::build_rwalk(&mut self.out, tag, &qids))
    }

    fn lopen(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tlopen(frame) else { return Reply::Fatal };
        self.count[OPENS] += 1;
        let Some(f) = c.fids.get_mut(&a.fid) else {
            return self.refuse(tag, p9::E_BADF, "Tlopen of an unknown fid");
        };
        if f.open.is_some() {
            return self.refuse(tag, p9::E_BADF, "Tlopen of an opened fid");
        }
        if f.stats == 0 {
            return self.refuse(tag, E_ACCES, "Tlopen with no Tgetattr first");
        }
        f.stats -= 1;
        let mode = a.flags & 3;
        let node = f.node;
        if mode == 3 || (mode != 0 && mode_of(node) & 0o222 == 0) {
            return self.refuse(tag, E_ACCES, "Tlopen that the node's mode refuses");
        }
        f.open = Some(mode);
        Self::built(p9::build_rlopen(&mut self.out, tag, &qid_of(node), 0))
    }

    fn read(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tread(frame) else { return Reply::Fatal };
        let Some(f) = c.fids.get_mut(&a.fid) else {
            return self.refuse(tag, p9::E_BADF, "Tread of an unknown fid");
        };
        let node = f.node;
        if f.open.is_none() || f.open == Some(1) {
            return self.refuse(tag, p9::E_BADF, "Tread of a fid not opened for reading");
        }
        // The kernel bounds a count by its msize less Rread's 11 bytes
        // (loom_service_wire.c), and the session's msize is this server's.
        let room = (c.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
        if a.count as usize > room {
            return self.refuse(tag, p9::E_INVAL, "a Tread count above msize less 11");
        }
        let count = a.count as usize;
        let mut bytes = Vec::new();
        match node {
            ROOT | DIR => return self.lerror(tag, p9::E_ISDIR),
            STALL => {
                self.count[STALLED] += 1;
                c.stalled.push(tag);
                return Reply::Held;
            }
            STATS => {
                for v in self.count.iter() {
                    bytes.extend_from_slice(&v.to_le_bytes());
                }
            }
            DATA | LEAF => {
                let len = if node == DATA { DATA_LEN } else { LEAF_LEN };
                for at in a.offset..len.min(a.offset.saturating_add(count as u64)) {
                    bytes.push(data_byte(at));
                }
            }
            STREAM => {
                let end = STREAM_LEN.min(f.cursor + count as u64);
                for at in f.cursor..end {
                    bytes.push(data_byte(at));
                }
                f.cursor = end;
            }
            _ => bytes.extend_from_slice(&self.sink),
        }
        if node != STATS {
            self.count[READS] += 1;
        }
        if node != DATA && node != LEAF && node != STREAM {
            let from = (a.offset as usize).min(bytes.len());
            let to = from.saturating_add(count).min(bytes.len());
            bytes = bytes[from..to].to_vec();
        }
        Self::built(p9::build_rread(&mut self.out, tag, &bytes))
    }

    fn write(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_twrite(frame) else { return Reply::Fatal };
        self.count[WRITES] += 1;
        let Some(f) = c.fids.get(&a.fid) else {
            return self.refuse(tag, p9::E_BADF, "Twrite of an unknown fid");
        };
        if f.open.is_none() || f.open == Some(0) {
            return self.refuse(tag, p9::E_BADF, "Twrite of a fid not opened for writing");
        }
        if f.node != SINK {
            return self.refuse(tag, E_ACCES, "Twrite to a read-only node");
        }
        let end = a.offset.saturating_add(a.data.len() as u64);
        if end > SINK_MAX {
            return self.lerror(tag, p9::E_INVAL);
        }
        if (self.sink.len() as u64) < end {
            self.sink.resize(end as usize, 0);
        }
        self.sink[a.offset as usize..end as usize].copy_from_slice(a.data);
        Self::built(p9::build_rwrite(&mut self.out, tag, a.data.len() as u32))
    }

    fn clunk(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tclunk(frame) else { return Reply::Fatal };
        self.count[CLUNKS] += 1;
        if c.fids.remove(&a.fid).is_none() {
            return self.refuse(tag, p9::E_BADF, "Tclunk of an unknown fid");
        }
        Self::built(p9::build_rclunk(&mut self.out, tag))
    }

    fn flush(&mut self, c: &mut Conn, frame: &[u8], tag: u16) -> Reply {
        let Ok(a) = p9::parse_tflush(frame) else { return Reply::Fatal };
        self.count[FLUSHES] += 1;
        c.stalled.retain(|t| *t != a.oldtag);
        Self::built(p9::build_rflush(&mut self.out, tag))
    }

    // Reads what is there and answers every complete frame. False ends the
    // session: EOF, a read error, a framing violation or a failed reply.
    fn service(&mut self, c: &mut Conn) -> bool {
        let cur = c.inb.len();
        let want = MSIZE as usize - cur;
        if want == 0 {
            self.violation("a full msize buffered with no complete frame");
            return false;
        }
        c.inb.resize(cur + want, 0);
        let n = unsafe { t_read(c.fd, c.inb.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            c.inb.truncate(cur);
            return false;
        }
        c.inb.truncate(cur + n as usize);
        loop {
            if c.inb.len() < p9::P9_HDR_LEN {
                return true;
            }
            let Ok(h) = p9::peek_header(&c.inb) else { return false };
            let size = h.size as usize;
            if size < p9::P9_HDR_LEN || size > c.msize.max(4096) as usize {
                self.violation("a frame whose size is out of range");
                return false;
            }
            if c.inb.len() < size {
                return true;
            }
            let frame: Vec<u8> = c.inb[..size].to_vec();
            c.inb.drain(..size);
            match self.dispatch(c, &frame, h) {
                Reply::Held => {}
                Reply::Fatal => {
                    self.violation("a malformed message");
                    return false;
                }
                Reply::Send(len) => {
                    let mut sent = 0;
                    while sent < len {
                        let w = unsafe { t_write(c.fd, self.out.as_ptr().add(sent), len - sent) };
                        if w <= 0 {
                            return false;
                        }
                        sent += w as usize;
                    }
                }
            }
        }
    }
}

fn run() -> i64 {
    let listener = unsafe { t_walk_create(REGISTRY, NAME.as_ptr(), NAME.len(), T_OREAD, 0) };
    if listener < 0 {
        say(&format!("FAIL: post the service ({})", listener));
        return 1;
    }
    if unsafe { t_write(NOTIFY, b"R".as_ptr(), 1) } != 1 {
        say("FAIL: notify the probe");
        return 1;
    }
    let mut s = Server { count: [0; STAT_COUNT], sink: Vec::new(), out: Vec::new() };
    let mut conns: Vec<Conn> = Vec::new();
    let mut settle: Option<u64> = None;
    let (mut asked, mut answered) = (false, false);
    loop {
        if asked && !answered && conns.is_empty() {
            answered = true;
            // A probe that stopped waiting has exited and closed the pipe's
            // read end (the runtime masks the pipe note): it failed already.
            let rc = unsafe { t_write(NOTIFY, b"Z".as_ptr(), 1) };
            if rc != 1 {
                say(&format!("the probe left before its answer ({})", rc));
            }
        }
        if let Some(until) = settle {
            if conns.is_empty() {
                break;
            }
            if now() >= until {
                say(&format!("FAIL: {} session(s) outlived the probe", conns.len()));
                return 2;
            }
        }
        let mut fds = Vec::with_capacity(conns.len() + 2);
        fds.push(TPollFd { fd: listener as i32, events: T_POLLIN, revents: 0 });
        if settle.is_none() {
            fds.push(TPollFd { fd: CONTROL as i32, events: T_POLLIN, revents: 0 });
        }
        let base = fds.len();
        for c in &conns {
            fds.push(TPollFd { fd: c.fd as i32, events: T_POLLIN, revents: 0 });
        }
        let timeout = if settle.is_some() { 100 } else { -1 };
        let rc = unsafe { t_poll(fds.as_mut_ptr(), fds.len(), timeout) };
        if rc < 0 {
            say(&format!("FAIL: poll ({})", rc));
            return 1;
        }
        if settle.is_none() && fds[1].revents != 0 {
            let mut b = [0u8; 16];
            let n = unsafe { t_read(CONTROL, b.as_mut_ptr(), b.len()) };
            if n < 0 {
                say(&format!("FAIL: read the control pipe ({})", n));
                return 1;
            }
            if n == 0 {
                settle = Some(now() + SETTLE_NS);
            }
            asked |= b[..n as usize].contains(&b'Q');
        }
        for i in (0..conns.len()).rev() {
            let ev = fds[base + i].revents;
            if ev == 0 {
                continue;
            }
            let live = if ev & T_POLLIN != 0 {
                s.service(&mut conns[i])
            } else {
                ev & (T_POLLHUP | T_POLLERR | T_POLLNVAL) == 0
            };
            if !live {
                let c = conns.remove(i);
                unsafe { t_close(c.fd) };
                s.count[ENDED] += 1;
            }
        }
        if fds[0].revents & T_POLLIN != 0 {
            let fd = unsafe { t_srv_accept(listener) };
            if fd < 0 {
                say(&format!("FAIL: accept ({})", fd));
                return 1;
            }
            s.count[ACCEPTED] += 1;
            conns.push(Conn {
                fd,
                inb: Vec::new(),
                versioned: false,
                msize: MSIZE,
                fids: BTreeMap::new(),
                stalled: Vec::new(),
            });
        } else if fds[0].revents != 0 {
            say(&format!("FAIL: the listener polled {:#x}", fds[0].revents));
            return 1;
        }
    }
    unsafe { t_close(listener) };
    let k = &s.count;
    say(&format!(
        "sessions {} ended {} violations {} stalled {} flushes {} clunks {} getattrs {} walks {} opens {} reads {} writes {}",
        k[ACCEPTED], k[ENDED], k[VIOLATIONS], k[STALLED], k[FLUSHES], k[CLUNKS], k[GETATTRS],
        k[WALKS], k[OPENS], k[READS], k[WRITES]
    ));
    if k[VIOLATIONS] != 0 {
        1
    } else if k[ACCEPTED] == 0 {
        76
    } else {
        77
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
