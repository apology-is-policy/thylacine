// The diorama 9P2000.L server -- the synthetic Linux world (VIVARIUM V-4).
//
// A native libthyla-rs 9P server (the ptyfs / netd device-less precedent) that
// presents Thylacine's native introspection surfaces in the shapes an unmodified
// Linux binary expects. It is a REFORMATTER, never an authority: every byte it
// serves is derived from a source the CALLING Proc could already have read
// itself (docs/VIVARIUM.md section 6.2), which is what makes I-43 -- "a phenotype
// confers ABI shape, never authority" -- structural rather than review-dependent.
//
// THE RULE, restated because it is the whole design: do NOT "improve" a diorama
// file by reading state through a path a native Proc could not use, and NEVER
// accept an answer supplied by the client. A compatibility shim that starts
// sourcing answers outside the native surface has stopped reformatting and become
// an authority. When a file has no native source, the fix belongs in the KERNEL
// (that is what V-4a-0's Proc.exe_path and V-4a-0b's srv_peer_info.pid are).
//
// READ-ONLY. There is no write path at v1.0: h_write returns E_PERM for every
// file. That single decision removes most of the surface a /proc would carry.
//
// ---------------------------------------------------------------------------
// WHO IS `self` -- read this before mounting the diorama anywhere
// ---------------------------------------------------------------------------
//
// /proc/self/... is not a file, it is a question about the CALLER. This server
// answers it with SYS_SRV_PEER, which reports the peer of the 9P CONNECTION --
// that is, the Proc that opened it, which for a mounted tree is the MOUNTER.
//
// So `self` means "the Proc that owns this connection", and that is correct ONLY
// when the mount is per-Proc or per-container. A SHARED mount (the way joey
// mounts /net and /dev/pts once for every session) would silently report the
// MOUNTER's identity to every reader -- exactly the shape cfg-3 hit, where the
// shared /dev/tapestry mount's peer is joey rather than the session.
//
// This is why VIVARIUM section 6 says the diorama is "mounted into the
// container's territory only", and it is not a limitation to be engineered
// around: a per-container mount is what a vivarium sets up anyway (V-7), and a
// Proc's territory is private, so a Proc that mounts the diorama itself gets
// itself as `self` by construction. /bin/diorama-probe does exactly that, which
// is what makes the V-4a gate meaningful.
//
// The alternative -- letting the client name a pid -- is the section 6.2 failure
// mode and is deliberately not offered.
//
// ---------------------------------------------------------------------------
// The tree (V-4a, Tier 1)
// ---------------------------------------------------------------------------
//
//   /                dir
//   /self            dir      the calling connection's own Proc
//   /self/exe        file     its executable's path   <- the V-4a gate
//   /self/cmdline    file     argv[0], NUL-terminated (Linux shape)
//   /self/status     file     Linux-shaped Name/Pid/Uid/Gid/Threads/VmRSS
//   /meminfo         file     MemTotal/MemFree in kB
//   /uptime          file     "<up> <idle>" seconds
//
// Deferred to V-4b with their kernel prerequisites (VIVARIUM section 6.7):
// /self/maps (the VMA list is unexposed), /self/cwd (Territory has dot_path but
// devproc does not surface it), /proc/<pid>/... , /cpuinfo, /stat.

use alloc::vec::Vec;
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_open, t_srv_peer, t_walk_create, TSrvPeerInfo, T_OPATH, T_OREAD,
    T_WALK_OPEN_FROM_ROOT,
};

pub const MAX_CONNS: usize = 8;
const MAX_FIDS: usize = 32;
const SRV_MSIZE: u32 = 8192;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

/// Largest rendered file. Every renderer is bounded by this; a renderer that
/// would exceed it truncates rather than overflowing (best-effort introspection,
/// the devproc DEVPROC_READ_BUF discipline).
const RENDER_MAX: usize = 1024;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_MODE: u32 = S_IFREG | 0o444;
const P9_GETATTR_SIZE: u64 = 0x200;

// ---------------------------------------------------------------------------
// The static node table. Unlike ptyfs there are no dynamic slots -- every node
// is known at compile time, so a qid path IS the node index and resolution can
// never dangle.
// ---------------------------------------------------------------------------

const N_ROOT: u64 = 0;
const N_SELF: u64 = 1;
const N_SELF_EXE: u64 = 2;
const N_SELF_CMDLINE: u64 = 3;
const N_SELF_STATUS: u64 = 4;
const N_MEMINFO: u64 = 5;
const N_UPTIME: u64 = 6;
const N_COUNT: u64 = 7;

struct Node {
    name: &'static [u8],
    parent: u64,
    is_dir: bool,
}

static NODES: [Node; N_COUNT as usize] = [
    Node { name: b"",        parent: N_ROOT, is_dir: true  },
    Node { name: b"self",    parent: N_ROOT, is_dir: true  },
    Node { name: b"exe",     parent: N_SELF, is_dir: false },
    Node { name: b"cmdline", parent: N_SELF, is_dir: false },
    Node { name: b"status",  parent: N_SELF, is_dir: false },
    Node { name: b"meminfo", parent: N_ROOT, is_dir: false },
    Node { name: b"uptime",  parent: N_ROOT, is_dir: false },
];

fn node_is_dir(path: u64) -> bool {
    (path < N_COUNT) && NODES[path as usize].is_dir
}

/// Resolve one component under `dir`. `..` walks to the parent (the root's
/// parent is itself, per 9P). Returns None on a miss -- there is no dynamic
/// namespace here, so a miss is always a genuine ENOENT.
fn walk_child(dir: u64, name: &[u8]) -> Option<u64> {
    if dir >= N_COUNT || !NODES[dir as usize].is_dir {
        return None;
    }
    if name == b"." {
        return Some(dir);
    }
    if name == b".." {
        return Some(NODES[dir as usize].parent);
    }
    for i in 0..N_COUNT {
        let n = &NODES[i as usize];
        if n.parent == dir && i != N_ROOT && n.name == name {
            return Some(i);
        }
    }
    None
}

// ---------------------------------------------------------------------------
// A bounded render buffer. Every push is cap-checked, so a renderer can never
// overrun and a too-long render truncates cleanly.
// ---------------------------------------------------------------------------

pub struct Render {
    buf: [u8; RENDER_MAX],
    len: usize,
}

impl Render {
    fn new() -> Render {
        Render { buf: [0u8; RENDER_MAX], len: 0 }
    }
    fn push(&mut self, s: &[u8]) {
        let room = RENDER_MAX - self.len;
        let n = if s.len() > room { room } else { s.len() };
        self.buf[self.len..self.len + n].copy_from_slice(&s[..n]);
        self.len += n;
    }
    fn push_dec(&mut self, mut v: u64) {
        let mut tmp = [0u8; 20];
        let mut i = 0;
        if v == 0 {
            tmp[i] = b'0';
            i += 1;
        }
        while v > 0 {
            tmp[i] = b'0' + (v % 10) as u8;
            v /= 10;
            i += 1;
        }
        while i > 0 {
            i -= 1;
            self.push(&[tmp[i]]);
        }
    }
    fn bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

// ---------------------------------------------------------------------------
// Native sources. Each is a plain read of a file the CALLING Proc could open
// itself -- that is the section 6.2 property, and it is why the diorama needs no
// privilege of its own.
// ---------------------------------------------------------------------------

/// Read a whole native file into `out`. Returns the byte count, or None if the
/// file is unreachable -- which is a legitimate outcome (a nameless Proc's
/// /proc/<pid>/exe is EMPTY, not an error), so callers degrade rather than fail.
fn read_native(path: &[u8], out: &mut [u8]) -> Option<usize> {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut total = 0usize;
    loop {
        if total >= out.len() {
            break;
        }
        let n = unsafe { libthyla_rs::t_read(fd, out.as_mut_ptr().add(total), out.len() - total) };
        if n <= 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    Some(total)
}

/// Build "/proc/<pid>/<leaf>" into `out`, returning the used length.
fn native_proc_path(pid: u32, leaf: &[u8], out: &mut [u8; 64]) -> usize {
    let mut r = Render::new();
    r.push(b"/proc/");
    r.push_dec(pid as u64);
    r.push(b"/");
    r.push(leaf);
    let b = r.bytes();
    let n = if b.len() > out.len() { out.len() } else { b.len() };
    out[..n].copy_from_slice(&b[..n]);
    n
}

/// Parse "<key>:<spaces><decimal>" out of a native key/value render (the shape
/// /proc/<pid>/status and /ctl/memory both use). Returns None if absent, so a
/// consumer can omit a field rather than invent one.
pub fn parse_kv_dec(text: &[u8], key: &[u8]) -> Option<u64> {
    let mut i = 0usize;
    while i < text.len() {
        // Is `key` at the start of this line, followed by ':'?
        if text.len() - i > key.len()
            && &text[i..i + key.len()] == key
            && text[i + key.len()] == b':'
        {
            let mut j = i + key.len() + 1;
            while j < text.len() && (text[j] == b' ' || text[j] == b'\t') {
                j += 1;
            }
            let mut v: u64 = 0;
            let mut any = false;
            while j < text.len() && text[j].is_ascii_digit() {
                v = v.wrapping_mul(10).wrapping_add((text[j] - b'0') as u64);
                any = true;
                j += 1;
            }
            return if any { Some(v) } else { None };
        }
        // Advance to the next line.
        while i < text.len() && text[i] != b'\n' {
            i += 1;
        }
        i += 1;
    }
    None
}

// ---------------------------------------------------------------------------
// The renderers. Each takes the connection's peer (never a client-supplied id).
// ---------------------------------------------------------------------------

/// /self/exe -- the executable's path, bare (no NUL, no newline: Linux's
/// readlink("/proc/self/exe") yields a bare path). Empty when the kernel has no
/// recorded name, which is a real state (kproc, the blob-loaded init).
fn render_self_exe(peer: &TSrvPeerInfo, r: &mut Render) {
    if peer.alive == 0 {
        return; // dead peer -> empty, never a stale answer
    }
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(peer.pid, b"exe", &mut pbuf);
    let mut ebuf = [0u8; RENDER_MAX];
    if let Some(got) = read_native(&pbuf[..n], &mut ebuf) {
        r.push(&ebuf[..got]);
    }
}

/// /self/cmdline -- Linux serves NUL-separated argv. Thylacine has no argv on a
/// running Proc (SYS_SPAWN's argv is consumed at exec and not retained), so this
/// serves argv[0] == the executable path, NUL-terminated, which is the universal
/// convention and is DERIVED from a native source rather than invented. A Proc
/// with no recorded exe renders empty, exactly as Linux does for a kernel thread.
fn render_self_cmdline(peer: &TSrvPeerInfo, r: &mut Render) {
    render_self_exe(peer, r);
    if r.len > 0 {
        r.push(&[0u8]);
    }
}

/// /self/status -- the Linux key/value shape, filled only from fields we really
/// have. Name comes from the exe's basename; Pid/Uid/Gid from the kernel-stamped
/// peer; Threads/VmRSS from the native /proc/<pid>/status render.
fn render_self_status(peer: &TSrvPeerInfo, r: &mut Render) {
    if peer.alive == 0 {
        return;
    }
    // Name: basename of the exe.
    let mut ebuf = [0u8; RENDER_MAX];
    let mut elen = 0usize;
    {
        let mut pbuf = [0u8; 64];
        let n = native_proc_path(peer.pid, b"exe", &mut pbuf);
        if let Some(got) = read_native(&pbuf[..n], &mut ebuf) {
            elen = got;
        }
    }
    let mut base = 0usize;
    for i in 0..elen {
        if ebuf[i] == b'/' {
            base = i + 1;
        }
    }
    r.push(b"Name:\t");
    r.push(&ebuf[base..elen]);
    r.push(b"\n");

    r.push(b"Pid:\t");
    r.push_dec(peer.pid as u64);
    r.push(b"\n");

    // Linux prints four ids (real/effective/saved/fs); Thylacine has one
    // principal, so all four columns are the same value -- honest, and it keeps
    // a Linux parser that splits on whitespace working.
    r.push(b"Uid:\t");
    for k in 0..4 {
        if k > 0 {
            r.push(b"\t");
        }
        r.push_dec(peer.principal_id as u64);
    }
    r.push(b"\n");
    r.push(b"Gid:\t");
    for k in 0..4 {
        if k > 0 {
            r.push(b"\t");
        }
        r.push_dec(peer.primary_gid as u64);
    }
    r.push(b"\n");

    // Threads + VmRSS from the native per-Proc render.
    let mut sbuf = [0u8; RENDER_MAX];
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(peer.pid, b"status", &mut pbuf);
    if let Some(got) = read_native(&pbuf[..n], &mut sbuf) {
        let text = &sbuf[..got];
        if let Some(t) = parse_kv_dec(text, b"threads") {
            r.push(b"Threads:\t");
            r.push_dec(t);
            r.push(b"\n");
        }
        if let Some(pages) = parse_kv_dec(text, b"pages") {
            r.push(b"VmRSS:\t");
            r.push_dec(pages * 4); // 4 KiB pages -> kB, the Linux unit
            r.push(b" kB\n");
        }
    }
}

/// /meminfo -- MemTotal/MemFree in kB, from /ctl/memory's page counts.
fn render_meminfo(r: &mut Render) {
    let mut buf = [0u8; RENDER_MAX];
    let got = match read_native(b"/ctl/memory", &mut buf) {
        Some(n) => n,
        None => return,
    };
    let text = &buf[..got];
    if let Some(total) = parse_kv_dec(text, b"total") {
        r.push(b"MemTotal:       ");
        r.push_dec(total * 4);
        r.push(b" kB\n");
    }
    if let Some(free) = parse_kv_dec(text, b"free") {
        r.push(b"MemFree:        ");
        r.push_dec(free * 4);
        r.push(b" kB\n");
        // Linux consumers overwhelmingly read MemAvailable; without a reclaim
        // model the honest value is MemFree, not a fabricated estimate.
        r.push(b"MemAvailable:   ");
        r.push_dec(free * 4);
        r.push(b" kB\n");
    }
}

/// /uptime -- "<seconds-up> <seconds-idle>", from CLOCK_MONOTONIC (ns since
/// boot). Linux's second field is aggregate idle time, which Thylacine does not
/// track per-CPU here; 0 is the honest placeholder (Linux itself reports 0 on
/// some virtualized configurations, and no consumer treats it as an error).
fn render_uptime(r: &mut Render) {
    // libthyla_rs::time keeps its TimeSpec private and Instant exposes no
    // "since boot" accessor, so read the clock directly. The layout is the
    // kernel-pinned struct t_timespec (two i64, 16 bytes).
    #[repr(C)]
    struct Ts {
        tv_sec: i64,
        tv_nsec: i64,
    }
    let mut ts = Ts { tv_sec: 0, tv_nsec: 0 };
    let rc = unsafe {
        libthyla_rs::t_clock_gettime(
            libthyla_rs::T_CLOCK_MONOTONIC,
            &mut ts as *mut Ts as u64,
        )
    };
    if rc != 0 {
        return;
    }
    let secs = ts.tv_sec.max(0) as u64;
    let hund = (ts.tv_nsec.clamp(0, 999_999_999) as u64) / 10_000_000;
    r.push_dec(secs);
    r.push(b".");
    if hund < 10 {
        r.push(b"0");
    }
    r.push_dec(hund);
    r.push(b" 0.00\n");
}

/// Render `node` for `peer`. Directories render empty (they are read via
/// Treaddir, not Tread).
pub fn render(node: u64, peer: &TSrvPeerInfo) -> Render {
    let mut r = Render::new();
    match node {
        N_SELF_EXE => render_self_exe(peer, &mut r),
        N_SELF_CMDLINE => render_self_cmdline(peer, &mut r),
        N_SELF_STATUS => render_self_status(peer, &mut r),
        N_MEMINFO => render_meminfo(&mut r),
        N_UPTIME => render_uptime(&mut r),
        _ => {}
    }
    r
}

// ---------------------------------------------------------------------------
// The connection + its fid table.
// ---------------------------------------------------------------------------

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    node: u64,
    opened: bool,
}

pub struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
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
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    /// The kernel-stamped identity of whoever opened this connection. Queried
    /// LIVE per use rather than cached at accept: `alive`/`caps`/`pid` are
    /// alive-gated, so a peer that exited reports alive == 0 and the renderers
    /// serve empty instead of a stale answer.
    fn peer(&self) -> TSrvPeerInfo {
        let mut info = TSrvPeerInfo::default();
        let rc = unsafe { t_srv_peer(self.handle, &mut info as *mut TSrvPeerInfo) };
        if rc != 0 {
            // Fail closed: an unreadable peer is an unknown peer.
            return TSrvPeerInfo::default();
        }
        info
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids.iter().position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    fn fid_set(&mut self, fid: u32, node: u64) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid { fid, node, opened: false });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid { fid, node, opened: false });
            return true;
        }
        false
    }

    /// Read + dispatch every complete frame currently buffered. Returns false to
    /// drop the connection.
    pub fn service(&mut self) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false; // a full msize buffered with no complete frame
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
            let rlen = self.dispatch(&frame, hdr);
            if rlen == 0 || !self.send_all(rlen) {
                return false;
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, tmsg: &[u8], hdr: p9::Header) -> usize {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(tmsg, tag),
            p9::P9_TREAD => self.h_read(tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            // READ-ONLY: every mutation is refused at the protocol edge, so no
            // renderer ever has to consider a write.
            p9::P9_TWRITE => self.err(tag, p9::E_PERM),
            _ => self.err(tag, p9::E_NOSYS),
        };
        r.unwrap_or_else(|_| {
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        })
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

    fn qid_of(&self, node: u64) -> p9::Qid {
        p9::Qid {
            kind: if node_is_dir(node) { p9::P9_QTDIR } else { p9::P9_QTFILE },
            version: 0,
            path: node,
        }
    }

    fn h_version(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let negotiated = a.msize.min(SRV_MSIZE);
        self.fids = [None; MAX_FIDS];
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
            return self.err(tag, p9::E_OPNOTSUPP); // no auth fid (trusted local transport)
        }
        if a.fid == p9::P9_NOFID || self.fid_find(a.fid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        if !self.fid_set(a.fid, N_ROOT) {
            return self.err(tag, p9::E_NOMEM);
        }
        let q = self.qid_of(N_ROOT);
        p9::build_rattach(&mut self.out_buf, tag, &q)
    }

    fn h_walk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twalk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let src = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let src_fid = self.fids[src].unwrap();
        if src_fid.opened {
            return self.err(tag, p9::E_PROTO); // 9P forbids walking from an opened fid
        }
        if a.newfid == p9::P9_NOFID {
            return self.err(tag, p9::E_INVAL);
        }
        if a.newfid != a.fid && self.fid_find(a.newfid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }

        let mut cur = src_fid.node;
        let mut qids: Vec<p9::Qid> = Vec::new();
        for i in 0..a.nwname as usize {
            let name = a.names[i];
            match walk_child(cur, name) {
                Some(next) => {
                    cur = next;
                    qids.push(self.qid_of(cur));
                }
                None => break, // partial walk: 9P reports the qids we managed
            }
        }
        // A zero-length walk clones the fid; a full walk binds newfid to the
        // target. A PARTIAL walk binds nothing (9P2000.L), so the client sees
        // fewer qids than names and knows the tail did not resolve.
        if qids.len() == a.nwname as usize {
            if !self.fid_set(a.newfid, cur) {
                return self.err(tag, p9::E_NOMEM);
            }
        } else if qids.is_empty() {
            return self.err(tag, p9::E_NOENT);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids)
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
        // READ-ONLY: refuse any open that asks for write access, so the refusal
        // lands at open() where a caller can act on it rather than at write().
        // O_WRONLY = 1, O_RDWR = 2 in the low two bits.
        if (a.flags & 0x3) != 0 {
            return self.err(tag, p9::E_PERM);
        }
        let mut f = self.fids[i].unwrap();
        f.opened = true;
        self.fids[i] = Some(f);
        let q = self.qid_of(f.node);
        p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
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
        if node_is_dir(f.node) {
            return self.err(tag, p9::E_ISDIR);
        }
        let peer = self.peer();
        let r = render(f.node, &peer);
        let body = r.bytes();
        let off = a.offset as usize;
        if off >= body.len() {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        let avail = body.len() - off;
        let cap = (a.count as usize).min(self.msize as usize - p9::P9_HDR_LEN - 4);
        let n = avail.min(cap);
        p9::build_rread(&mut self.out_buf, tag, &body[off..off + n])
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
        if !node_is_dir(f.node) {
            return self.err(tag, p9::E_NOTDIR);
        }
        let budget = (a.count as usize).min(self.msize as usize - p9::P9_HDR_LEN - 4);
        let mut data: Vec<u8> = Vec::new();
        // The cookie is the NEXT child index to emit, so it is strictly
        // increasing and never 0 for a non-first entry -- the devproc/netd
        // readdir discipline.
        let mut child = a.offset;
        while child < N_COUNT {
            let next = child + 1; // the cookie to report for the FOLLOWING call
            if child == N_ROOT || NODES[child as usize].parent != f.node {
                child = next;
                continue;
            }
            let dt = if NODES[child as usize].is_dir { p9::DT_DIR } else { p9::DT_REG };
            let mut scratch = [0u8; 64];
            let used = match p9::pack_dirent(
                &mut scratch,
                0,
                &self.qid_of(child),
                next,
                dt,
                NODES[child as usize].name,
            ) {
                Ok(u) => u,
                Err(_) => break,
            };
            if data.len() + used > budget {
                break; // did not fit; `child` stays here so the client re-asks for it
            }
            data.extend_from_slice(&scratch[..used]);
            child = next;
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
        let is_dir = node_is_dir(f.node);
        let size = if is_dir {
            0u64
        } else {
            let peer = self.peer();
            render(f.node, &peer).bytes().len() as u64
        };
        let q = self.qid_of(f.node);
        let mode = if is_dir { DIR_MODE } else { FILE_MODE };
        // The security trio (mode/uid/gid) MUST be marked valid: the kernel's
        // dev9p per-component X-search reads them, and an UNFILLED trio fails
        // CLOSED -- the whole mounted tree becomes untraversable while the mount
        // itself still reports success (which is exactly how this presented:
        // t_mount rc=0, then every open under it denied). ptyfs carries the same
        // warning for the same reason.
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &q,
            mode,
            libthyla_rs::T_PRINCIPAL_SYSTEM,
            libthyla_rs::T_GID_SYSTEM,
            1, // nlink: every node here is a single link
            size,
        )
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if let Some(i) = self.fid_find(a.fid) {
            self.fids[i] = None;
        }
        p9::build_rclunk(&mut self.out_buf, tag)
    }

    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        // Nothing here ever defers -- every render completes inside its own
        // dispatch -- so a flush has nothing to cancel and is a bare ack.
        let _ = p9::parse_tflush(tmsg);
        p9::build_rflush(&mut self.out_buf, tag)
    }
}

/// Post /srv/diorama (9P-mode). Requires PROC_FLAG_MAY_POST_SERVICE (joey spawns
/// the diorama with T_SPAWN_PERM_MAY_POST_SERVICE, the ptyfs/corvus precedent).
pub fn post_srv_diorama() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"diorama".as_ptr(), 7, T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

// ---------------------------------------------------------------------------
// In-server selftest: the tree walk + the bounded renderer + the key/value
// parser, deterministic and mount-independent (the ptyfs/netd selftest-before-
// serve pattern, so a logic failure gates the boot instead of surfacing later as
// a mystery). The live 9P path + the peer identity are proven in-guest by
// /bin/diorama-probe.
// ---------------------------------------------------------------------------

pub fn selftest() -> Result<(), &'static str> {
    // --- the static tree resolves exactly as declared
    if walk_child(N_ROOT, b"self") != Some(N_SELF) {
        return Err("walk /self");
    }
    if walk_child(N_SELF, b"exe") != Some(N_SELF_EXE) {
        return Err("walk /self/exe");
    }
    if walk_child(N_ROOT, b"meminfo") != Some(N_MEMINFO) {
        return Err("walk /meminfo");
    }
    // A file has no children, and a miss is a miss.
    if walk_child(N_SELF_EXE, b"anything").is_some() {
        return Err("walked into a file");
    }
    if walk_child(N_ROOT, b"nope").is_some() {
        return Err("resolved a nonexistent name");
    }
    // `..` climbs; the root's parent is itself.
    if walk_child(N_SELF, b"..") != Some(N_ROOT) {
        return Err("walk ..");
    }
    if walk_child(N_ROOT, b"..") != Some(N_ROOT) {
        return Err("root .. must be root");
    }
    // /meminfo is NOT reachable under /self (parent-scoped resolution).
    if walk_child(N_SELF, b"meminfo").is_some() {
        return Err("cross-parent name resolved");
    }

    // --- the render buffer is bounded and cannot overrun
    let mut r = Render::new();
    for _ in 0..(RENDER_MAX + 64) {
        r.push(b"x");
    }
    if r.bytes().len() != RENDER_MAX {
        return Err("render overran its cap");
    }

    // --- decimal formatting
    let mut d = Render::new();
    d.push_dec(0);
    d.push(b",");
    d.push_dec(1);
    d.push(b",");
    d.push_dec(4096);
    if d.bytes() != b"0,1,4096" {
        return Err("push_dec");
    }

    // --- the key/value parser used to lift native renders
    let text = b"total:    2048 pages\nfree:     1024 pages\nreserved: 8 pages\n";
    if parse_kv_dec(text, b"total") != Some(2048) {
        return Err("kv total");
    }
    if parse_kv_dec(text, b"free") != Some(1024) {
        return Err("kv free");
    }
    if parse_kv_dec(text, b"absent").is_some() {
        return Err("kv invented a missing key");
    }
    // A key must match at a LINE START, not mid-line -- otherwise "reserved"
    // could be matched by a suffix search and report the wrong number.
    let tricky = b"xfree:  7\nfree:  9\n";
    if parse_kv_dec(tricky, b"free") != Some(9) {
        return Err("kv matched mid-line");
    }

    // --- a dead peer renders EMPTY rather than a stale or fabricated answer
    let dead = TSrvPeerInfo::default(); // alive == 0
    if !render(N_SELF_EXE, &dead).bytes().is_empty() {
        return Err("dead peer served an exe");
    }
    if !render(N_SELF_STATUS, &dead).bytes().is_empty() {
        return Err("dead peer served a status");
    }
    if !render(N_SELF_CMDLINE, &dead).bytes().is_empty() {
        return Err("dead peer served a cmdline");
    }

    // --- uptime is always renderable and monotonic-shaped
    let up = render(N_UPTIME, &dead);
    if up.bytes().is_empty() {
        return Err("uptime empty");
    }

    Ok(())
}
