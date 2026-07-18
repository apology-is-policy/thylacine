// The ptyfs 9P2000.L server -- pseudoterminal master/slave (PTY-2, I-20).
//
// A native libthyla-rs 9P server (the netd/corvus device-less precedent) owning
// the pts pairs: the two byte rings + (PTY-2b) the per-pts line discipline. The
// KERNEL owns the session/pgrp/controlling-terminal state (the PTY-1 kernel arc,
// docs/reference/135-pty-kernel.md); ptyfs's ONLY signal authority is the
// pts-scoped SYS_TTY_SIGNAL. It cannot name a process group -- the I-1/I-22
// bound.
//
// The tree -- a Linux-devpts-shaped directory joey mounts at /dev/pts (a devdev
// synthetic mount-point child, the devhw /hw/pci precedent):
//   /              (P_ROOT, dir)   { ptmx, <n> } -- the devpts directory.
//   /ptmx          (P_PTMX, file)  open = the Plan 9 clone: mint pts N, rebind
//                                  the opened fid onto the master endpoint. Reached
//                                  as /dev/pts/ptmx; the POSIX /dev/ptmx master
//                                  path is a PTY-3 concern (a symlink or file-mount,
//                                  deferred until symlinks -- union-mount walking is
//                                  Phase 5+, so /dev/pts is one dir mount).
//   /<n>           (SLAVE n, file) the slave byte channel (/dev/pts/<n>).
//   <master n>     (MASTER n, file) the master byte channel, minted by clone
//                                  (not a walkable/readdir node).
//
// The master writes cooked input toward the slave (m2s); the slave writes output
// toward the master (s2m). A read on an empty-but-open ring DEFERS (the netd
// PendingRead / Disp::Deferred / poll_reads mechanism -- a multi-waiter SET, not
// the cons single-reader). PTY-2a is RAW (no cooking): a master write pushes
// bytes straight to m2s. The line discipline (ICANON/ISIG/ECHO/ICRNL/ONLCR) lands
// at PTY-2b.
//
// Refcounts: a fid bound to a MASTER/SLAVE node holds the pts slot LIVE (`refs`);
// separately, OPENED master/slave fds are counted (`n_master`/`n_slave`) so a
// read sees EOF when the OTHER end has fully closed. The slot frees when no fid
// names it (refs == 0) -- the only free path -- and the kernel registry entry is
// released via SYS_PTY_REGISTER(FREE).
//
// ptyfs is single-threaded (one Proc, one serve loop): every 9P frame across
// every session is processed sequentially, so the pts table needs no lock, and
// I-9 (no lost read wake) holds because poll_reads runs at the loop top before
// any blocking t_poll (a write in a prior frame is always drained before the loop
// parks) -- the netd single-threaded argument.

use alloc::collections::VecDeque;
use alloc::vec::Vec;
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_open, t_pty_register, t_walk_create, T_GID_SYSTEM, T_OPATH, T_OREAD,
    T_PRINCIPAL_SYSTEM, T_PTY_REG_FREE, T_PTY_REG_MINT, T_PTY_REG_SLAVE, T_WALK_OPEN_FROM_ROOT,
};

/// Max concurrent 9P connections the accept loop tracks. In practice joey's one
/// /dev mount drives ONE kernel-client session; the headroom covers a future
/// direct open=connect consumer.
pub const MAX_CONNS: usize = 8;

/// Per-connection fid-table size: one fid per open file/dir the client holds.
const MAX_FIDS: usize = 32;

/// Max live pts pairs. A bound, not headroom: an unbounded pts table is a DoS
/// vector (#65 resource floor), so clone-minting fails (ENFILE) past this.
const PTS_MAX: usize = 16;

/// Server-negotiated msize -- matches the kernel client's SRVCONN_MSIZE proposal
/// (both land at min = 32 KiB), so a full data frame crosses in one op.
const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

/// Per-direction byte-ring capacity (m2s / s2m). A pts is a terminal, not a bulk
/// pipe: 4 KiB each way is the classic tty buffer size. A full ring back-pressures
/// via a short Rwrite (the writer retries) -- no byte is dropped.
const RING_CAP: usize = 4096;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
// The master + slave endpoints are read/write byte channels (0o666, world-rw like
// a Unix pts). The kernel's dev9p per-component X-search + the pts registry gate
// authority; the file mode is the POSIX-shaped surface.
const FILE_RW: u32 = S_IFREG | 0o666;

const P9_GETATTR_SIZE: u64 = 0x200;

// =============================================================================
// qid encoding. Static skeleton nodes occupy the small fixed range [0, 2); a pts
// endpoint encodes PTS_FLAG | (n << 8) | filekind. The ranges never collide
// (PTS_FLAG is bit 40). qid 0 (P_ROOT) is the dev9p attach root -- the kernel
// reserves it, and every pts endpoint qid is != 0 (PTS_FLAG set), so no pts qid
// collides with the reserved attach-root the pts registry rejects.
// =============================================================================

const P_ROOT: u64 = 0; // / -- the devpts directory (joey mounts it at /dev/pts)
const P_PTMX: u64 = 1; // /ptmx  (the clone file; reached as /dev/pts/ptmx)

const PTS_FLAG: u64 = 1 << 40;
const FK_SHIFT: u64 = 8;
const FK_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff; // 24-bit pts number

// Endpoint file kinds (the filekind low byte). 0 is unused so a bare PTS_FLAG|
// (n<<8) is never a valid endpoint (defensive).
const FK_MASTER: u64 = 1;
const FK_SLAVE: u64 = 2;

fn is_pts(path: u64) -> bool {
    path & PTS_FLAG != 0
}

fn make_pts(n: u32, filekind: u64) -> u64 {
    PTS_FLAG | ((n as u64) << FK_SHIFT) | filekind
}

fn pts_n(path: u64) -> u32 {
    ((path >> FK_SHIFT) & N_MASK) as u32
}

fn pts_filekind(path: u64) -> u64 {
    path & FK_MASK
}

fn is_master_path(path: u64) -> bool {
    is_pts(path) && pts_filekind(path) == FK_MASTER
}

/// The pts number a path names (a master or slave endpoint), or None for a static
/// node. The refcount key: every fid bound here holds pts N live.
fn path_pts_n(path: u64) -> Option<u32> {
    if is_pts(path) {
        Some(pts_n(path))
    } else {
        None
    }
}

/// The outcome of draining one ring on a read (the netd RecvOutcome shape). The
/// EOF-vs-no-data-yet distinction is load-bearing: a bare 0 is ambiguous, so a
/// blocking read must tell "the other end closed" (EOF) from "open but empty"
/// (WouldBlock -> park).
enum RecvOutcome {
    Data(usize),
    Eof,
    WouldBlock,
}

// =============================================================================
// The pts table.
// =============================================================================

struct Pts {
    live: bool,
    /// The kernel pts id from SYS_PTY_REGISTER(MINT). 0 = not registered (the
    /// in-server selftest mints a local pts with no kernel entry).
    pts_id: i64,
    m2s: VecDeque<u8>, // master -> slave (the child reads)
    s2m: VecDeque<u8>, // slave -> master (the emulator reads)
    /// Bound fids naming this pts (master or slave). The slot frees when this hits
    /// 0 (the only free path).
    refs: u32,
    /// OPENED master / slave fds. The EOF signal: a slave read sees EOF when
    /// n_master == 0 (the master fully closed); a master read when n_slave == 0.
    n_master: u32,
    n_slave: u32,
}

pub struct Ptys {
    slots: [Option<Pts>; PTS_MAX],
}

impl Ptys {
    pub fn new() -> Ptys {
        Ptys {
            slots: [const { None }; PTS_MAX],
        }
    }

    /// Mint a fresh pts pair; returns its index N (refs 0, no open fds, empty
    /// rings), or None if the table is full (#65 floor -> ENFILE at the caller).
    fn mint(&mut self) -> Option<usize> {
        let n = self.slots.iter().position(|s| s.is_none())?;
        self.slots[n] = Some(Pts {
            live: true,
            pts_id: 0,
            m2s: VecDeque::new(),
            s2m: VecDeque::new(),
            refs: 0,
            n_master: 0,
            n_slave: 0,
        });
        Some(n)
    }

    fn slot(&self, n: u32) -> Option<&Pts> {
        self.slots.get(n as usize).and_then(|s| s.as_ref())
    }

    fn slot_mut(&mut self, n: u32) -> Option<&mut Pts> {
        self.slots.get_mut(n as usize).and_then(|s| s.as_mut())
    }

    fn live(&self, n: u32) -> bool {
        self.slot(n).map(|p| p.live).unwrap_or(false)
    }

    fn set_pts_id(&mut self, n: u32, id: i64) {
        if let Some(p) = self.slot_mut(n) {
            p.pts_id = id;
        }
    }

    fn pts_id(&self, n: u32) -> i64 {
        self.slot(n).map(|p| p.pts_id).unwrap_or(0)
    }

    /// Ref a fid binding onto `path`. A static node has no pts ref.
    fn ref_path(&mut self, path: u64) {
        if let Some(n) = path_pts_n(path) {
            if let Some(p) = self.slot_mut(n) {
                p.refs += 1;
            }
        }
    }

    /// Unref a fid binding off `path`. If the pts's last binding drops, FREE it
    /// (drop the rings, mark dead) and return its kernel pts_id so the caller can
    /// SYS_PTY_REGISTER(FREE). A static node -> None.
    fn unref_path(&mut self, path: u64) -> Option<i64> {
        let n = path_pts_n(path)?;
        let p = self.slot_mut(n)?;
        if p.refs > 0 {
            p.refs -= 1;
        }
        if p.refs == 0 {
            let id = p.pts_id;
            self.slots[n as usize] = None;
            Some(id)
        } else {
            None
        }
    }

    /// A master/slave fd was OPENED (the EOF-tracking count).
    fn open_inc(&mut self, n: u32, master: bool) {
        if let Some(p) = self.slot_mut(n) {
            if master {
                p.n_master += 1;
            } else {
                p.n_slave += 1;
            }
        }
    }

    /// An opened master/slave fd was clunked.
    fn open_dec(&mut self, n: u32, master: bool) {
        if let Some(p) = self.slot_mut(n) {
            if master {
                p.n_master = p.n_master.saturating_sub(1);
            } else {
                p.n_slave = p.n_slave.saturating_sub(1);
            }
        }
    }

    /// Push bytes onto a ring up to RING_CAP; returns the count accepted (a short
    /// push is back-pressure -- the writer retries, no byte dropped). PTY-2a raw:
    /// the master write goes straight to m2s (cooking is PTY-2b).
    fn ring_push(ring: &mut VecDeque<u8>, data: &[u8]) -> usize {
        let room = RING_CAP.saturating_sub(ring.len());
        let k = room.min(data.len());
        ring.extend(&data[..k]);
        k
    }

    fn master_write(&mut self, n: u32, data: &[u8]) -> usize {
        match self.slot_mut(n) {
            Some(p) => Ptys::ring_push(&mut p.m2s, data),
            None => 0,
        }
    }

    fn slave_write(&mut self, n: u32, data: &[u8]) -> usize {
        match self.slot_mut(n) {
            Some(p) => Ptys::ring_push(&mut p.s2m, data),
            None => 0,
        }
    }

    /// Drain up to buf.len() bytes toward the slave (from m2s). Empty + master
    /// closed -> Eof; empty + master open -> WouldBlock.
    fn slave_read(&mut self, n: u32, buf: &mut [u8]) -> RecvOutcome {
        match self.slot_mut(n) {
            Some(p) => Ptys::ring_drain(&mut p.m2s, buf, p.n_master),
            None => RecvOutcome::Eof,
        }
    }

    /// Drain up to buf.len() bytes toward the master (from s2m). Empty + slave
    /// closed -> Eof; empty + slave open -> WouldBlock.
    fn master_read(&mut self, n: u32, buf: &mut [u8]) -> RecvOutcome {
        match self.slot_mut(n) {
            Some(p) => Ptys::ring_drain(&mut p.s2m, buf, p.n_slave),
            None => RecvOutcome::Eof,
        }
    }

    fn ring_drain(ring: &mut VecDeque<u8>, buf: &mut [u8], other_open: u32) -> RecvOutcome {
        if !ring.is_empty() {
            let k = buf.len().min(ring.len());
            for b in buf.iter_mut().take(k) {
                *b = ring.pop_front().unwrap();
            }
            RecvOutcome::Data(k)
        } else if other_open == 0 {
            RecvOutcome::Eof
        } else {
            RecvOutcome::WouldBlock
        }
    }

    fn is_dir(&self, path: u64) -> bool {
        path == P_ROOT
    }

    /// Resolve one walk step. A pts slave resolves only while its slot is LIVE, so
    /// a stale/forged slave qid is unreachable (the netd live-slot property).
    fn walk_child(&self, cur: u64, name: &[u8]) -> Option<u64> {
        if cur != P_ROOT {
            return None; // only the devpts root has children
        }
        if name == b"ptmx" {
            return Some(P_PTMX);
        }
        // A decimal name resolves to a live slave (a stale/forged N is unreachable,
        // the netd live-slot property).
        let n = parse_dec(name)?;
        if self.live(n) {
            Some(make_pts(n, FK_SLAVE))
        } else {
            None
        }
    }

    /// Enumerate a directory's children (name, child qid.path, is_dir). Used by
    /// Treaddir. The child order is stable across a paginated read (the ordinal
    /// resume cookie relies on it).
    fn for_each_child<F: FnMut(&[u8], u64, bool)>(&self, path: u64, mut f: F) {
        if path != P_ROOT {
            return;
        }
        f(b"ptmx", P_PTMX, false);
        for n in 0..PTS_MAX as u32 {
            if self.live(n) {
                let mut buf = [0u8; 12];
                let name = fmt_dec(n, &mut buf);
                f(name, make_pts(n, FK_SLAVE), false);
            }
        }
    }
}

/// Parse a decimal ASCII slave name (no leading zeros beyond "0"; bounded to the
/// pts number range). Returns None on any non-digit / overflow / empty.
fn parse_dec(name: &[u8]) -> Option<u32> {
    if name.is_empty() || name.len() > 10 {
        return None;
    }
    if name.len() > 1 && name[0] == b'0' {
        return None; // no leading zeros -> one canonical name per pts
    }
    let mut v: u32 = 0;
    for &c in name {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u32)?;
    }
    Some(v)
}

/// Format `n` as decimal ASCII into `buf`, returning the used slice.
fn fmt_dec(n: u32, buf: &mut [u8; 12]) -> &[u8] {
    if n == 0 {
        buf[0] = b'0';
        return &buf[..1];
    }
    let mut tmp = [0u8; 12];
    let mut i = 0;
    let mut v = n;
    while v > 0 {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    for j in 0..i {
        buf[j] = tmp[i - 1 - j];
    }
    &buf[..i]
}

// =============================================================================
// The connection + its fid table (the netd Conn shape, stripped to the pts tree).
// =============================================================================

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

/// A blocking master/slave read holding its Rread. Parked on an empty-but-open
/// ring; completed by poll_reads when bytes arrive (or 0 on EOF). A flat Vec (the
/// net-6a-1 multi-waiter discipline), so two peer threads' concurrent reads on one
/// fd drain in order -- NOT the cons single-reader.
#[derive(Copy, Clone)]
struct PendingRead {
    fid: u32,
    slot_n: u32,
    master: bool, // reading the master endpoint (drain s2m) vs the slave (m2s)
    tag: u16,
    cap: usize,
}

pub struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    defer: bool,
    pending_reads: Vec<PendingRead>,
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
            pending_reads: Vec::new(),
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    /// Free a pts kernel-registry entry (SYS_PTY_REGISTER(FREE)) if `freed` names
    /// a registered id. ptyfs is the minting server, so FREE is authorized.
    fn free_pts(freed: Option<i64>) {
        if let Some(id) = freed {
            if id > 0 {
                unsafe {
                    let _ = t_pty_register(T_PTY_REG_FREE, id as u64, 0, 0);
                }
            }
        }
    }

    /// Drop every reference this connection holds before it is closed (the serve
    /// loop calls this before removing a dead Conn), so a session teardown frees
    /// the pts pairs only this session held open and releases their kernel entries.
    pub fn teardown(&mut self, ptys: &mut Ptys) {
        for slot in self.fids.iter_mut() {
            if let Some(f) = slot.take() {
                if f.opened && is_pts(f.path) {
                    ptys.open_dec(pts_n(f.path), is_master_path(f.path));
                }
                Conn::free_pts(ptys.unref_path(f.path));
            }
        }
        self.pending_reads.clear(); // the held Rreads die with the connection
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    /// Bind `fid` -> `path` (a walk result; unopened). Refs the NEW pts first, then
    /// unrefs the OLD (a within-connection rebind never transits refs==0). Returns
    /// false only if the fid table is full (a fresh bind, no free slot).
    fn fid_set(&mut self, ptys: &mut Ptys, fid: u32, path: u64) -> bool {
        if let Some(i) = self.fid_find(fid) {
            let old = self.fids[i].unwrap();
            ptys.ref_path(path);
            self.fids[i] = Some(Fid {
                fid,
                path,
                opened: false,
            });
            if old.opened && is_pts(old.path) {
                ptys.open_dec(pts_n(old.path), is_master_path(old.path));
            }
            Conn::free_pts(ptys.unref_path(old.path));
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            ptys.ref_path(path);
            self.fids[i] = Some(Fid {
                fid,
                path,
                opened: false,
            });
            return true;
        }
        false
    }

    fn fid_clunk(&mut self, ptys: &mut Ptys, fid: u32) -> bool {
        if let Some(i) = self.fid_find(fid) {
            let f = self.fids[i].take().unwrap();
            self.pending_reads.retain(|pr| pr.fid != fid);
            if f.opened && is_pts(f.path) {
                ptys.open_dec(pts_n(f.path), is_master_path(f.path));
            }
            Conn::free_pts(ptys.unref_path(f.path));
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every COMPLETE 9P frame. False to close
    /// the connection (EOF, framing violation, or write failure). One t_read per
    /// call; a partial frame waits for the next readable event.
    pub fn service(&mut self, ptys: &mut Ptys) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false; // a full msize buffered with no complete frame
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
            match self.dispatch(ptys, &frame, hdr) {
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

    fn dispatch(&mut self, ptys: &mut Ptys, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(ptys, tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(ptys, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(ptys, tmsg, tag),
            p9::P9_TREAD => self.h_read(ptys, tmsg, tag),
            p9::P9_TWRITE => self.h_write(ptys, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(ptys, tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(ptys, tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(ptys, tmsg, tag),
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

    fn qid_of(&self, ptys: &Ptys, path: u64) -> p9::Qid {
        let kind = if ptys.is_dir(path) {
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

    fn h_version(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let negotiated = a.msize.min(SRV_MSIZE);
        self.drop_all_fids(ptys);
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

    fn drop_all_fids(&mut self, ptys: &mut Ptys) {
        for slot in self.fids.iter_mut() {
            if let Some(f) = slot.take() {
                if f.opened && is_pts(f.path) {
                    ptys.open_dec(pts_n(f.path), is_master_path(f.path));
                }
                Conn::free_pts(ptys.unref_path(f.path));
            }
        }
        self.pending_reads.clear();
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
        // The root is a static node (no pts ref) -- bind directly.
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid {
                fid: a.fid,
                path: P_ROOT,
                opened: false,
            });
        } else {
            return self.err(tag, p9::E_NOMEM);
        }
        let q = p9::Qid {
            kind: p9::P9_QTDIR,
            version: 0,
            path: P_ROOT,
        };
        p9::build_rattach(&mut self.out_buf, tag, &q)
    }

    fn h_walk(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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

        let mut cur = src_fid.path;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut nwalked = 0usize;
        for i in 0..(a.nwname as usize) {
            match ptys.walk_child(cur, a.names[i]) {
                Some(next) => {
                    cur = next;
                    qids[nwalked] = self.qid_of(ptys, next);
                    nwalked += 1;
                }
                None => break,
            }
        }
        if a.nwname > 0 && nwalked == 0 {
            return self.err(tag, p9::E_NOENT);
        }
        // newfid binds to the last walked element ONLY on a full walk (nwqid ==
        // nwname). A partial walk leaves newfid untouched; nwname==0 is a clone.
        if nwalked == a.nwname as usize && !self.fid_set(ptys, a.newfid, cur) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..nwalked])
    }

    fn h_lopen(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        let _ = a.flags;

        // The Plan 9 clone idiom: opening /ptmx MINTS a pts and rebinds THIS fid
        // onto the master endpoint (the kernel dev9p client accepts the differing
        // Rlopen qid). The master is registered with the kernel pts registry keyed
        // on (this connection, master_qid) -- SYS_TTY_* on the master fd resolves
        // there. Ref/register-before-build so a build failure rolls back cleanly.
        if f.path == P_PTMX {
            let n = match ptys.mint() {
                Some(n) => n as u32,
                None => return self.err(tag, p9::E_NOMEM), // table full (ENFILE-class)
            };
            let master = make_pts(n, FK_MASTER);
            // MINT(conn_fd, master_qid, 0) -> pts_id > 0. Requires MAY_POST_SERVICE
            // + a server-endpoint conn Spoor (both hold: ptyfs is spawned with the
            // service bit, and self.handle is the t_srv_accept product).
            let pid = unsafe { t_pty_register(T_PTY_REG_MINT, self.handle as u64, master, 0) };
            if pid <= 0 {
                ptys.slots[n as usize] = None; // roll the mint back
                // Propagate the kernel errno (T_E_* == POSIX == the 9P ecode):
                // EAGAIN (registry full) / EACCES (gate) etc.
                let code = if pid < 0 { (-pid) as u32 } else { p9::E_NOMEM };
                return self.err(tag, code);
            }
            ptys.set_pts_id(n, pid);
            ptys.ref_path(master); // refs 0 -> 1 (this fid owns the pts)
            ptys.open_inc(n, true); // n_master 0 -> 1
            let q = self.qid_of(ptys, master);
            match p9::build_rlopen(&mut self.out_buf, tag, &q, 0) {
                Ok(len) => {
                    self.fids[i] = Some(Fid {
                        fid: a.fid,
                        path: master,
                        opened: true,
                    });
                    Ok(len)
                }
                Err(()) => {
                    ptys.open_dec(n, true);
                    Conn::free_pts(ptys.unref_path(master)); // refs 1 -> 0 -> freed + FREE
                    Err(())
                }
            }
        } else if is_pts(f.path) && !is_master_path(f.path) {
            // A slave open (/pts/<n>): register the slave binding, mark opened.
            let n = pts_n(f.path);
            let pid = ptys.pts_id(n);
            // SLAVE(conn_fd, slave_qid, pts_id) -> 0. pid is the mint's id; a slot
            // reached via a live walk always carries a registered id in the real
            // path (mint registers before the slave is walkable). A 0 pid (the
            // selftest-local pts, never walked over 9P) cannot reach here.
            let rc =
                unsafe { t_pty_register(T_PTY_REG_SLAVE, self.handle as u64, f.path, pid as u64) };
            if rc < 0 {
                return self.err(tag, (-rc) as u32);
            }
            ptys.open_inc(n, false); // n_slave += 1
            let q = self.qid_of(ptys, f.path);
            let mut nf = f;
            nf.opened = true;
            self.fids[i] = Some(nf);
            p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
        } else {
            // A directory (P_ROOT) open, for Treaddir.
            let q = self.qid_of(ptys, f.path);
            let mut nf = f;
            nf.opened = true;
            self.fids[i] = Some(nf);
            p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
        }
    }

    fn h_read(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        if ptys.is_dir(f.path) {
            return self.err(tag, p9::E_ISDIR); // a dir listing is Treaddir
        }
        if !is_pts(f.path) {
            return self.err(tag, p9::E_INVAL); // no readable static file (ptmx is open-only)
        }
        let n = pts_n(f.path);
        let master = is_master_path(f.path);
        // A pts is a byte STREAM: the Tread offset is meaningless (no seek).
        let cap = (self.msize as usize)
            .saturating_sub(p9::P9_HDR_LEN + 4)
            .min(a.count as usize)
            .min(RING_CAP);
        if cap == 0 {
            // POSIX: a 0-count read returns 0 at once; never park (an empty drain
            // is Data(0), which would otherwise look like WouldBlock forever).
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        let mut scratch = alloc::vec![0u8; cap];
        let outcome = if master {
            ptys.master_read(n, &mut scratch[..cap])
        } else {
            ptys.slave_read(n, &mut scratch[..cap])
        };
        match outcome {
            RecvOutcome::Data(k) => p9::build_rread(&mut self.out_buf, tag, &scratch[..k]),
            RecvOutcome::Eof => p9::build_rread(&mut self.out_buf, tag, &[]),
            RecvOutcome::WouldBlock => {
                if self.pending_reads.len() >= MAX_FIDS {
                    return self.err(tag, p9::E_PROTO);
                }
                self.pending_reads.push(PendingRead {
                    fid: a.fid,
                    slot_n: n,
                    master,
                    tag,
                    cap,
                });
                self.defer = true;
                Ok(0) // ignored: dispatch returns Disp::Deferred
            }
        }
    }

    fn h_write(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || !is_pts(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let n = pts_n(f.path);
        // PTY-2a RAW: a master write goes straight to m2s; a slave write to s2m.
        // The line discipline (cook a master write) lands at PTY-2b.
        let pushed = if is_master_path(f.path) {
            ptys.master_write(n, a.data)
        } else {
            ptys.slave_write(n, a.data)
        };
        p9::build_rwrite(&mut self.out_buf, tag, pushed as u32)
    }

    fn h_readdir(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
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
        if !ptys.is_dir(f.path) {
            return self.err(tag, p9::E_NOTDIR);
        }
        let budget = rreaddir_budget(a.count, self.msize);
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;
        let mut full = false;
        ptys.for_each_child(f.path, |name, child, is_dir| {
            ord += 1;
            if full || ord <= a.offset {
                return;
            }
            let entry_len = p9::dirent_len(name.len());
            if data.len() + entry_len > budget {
                full = true;
                return;
            }
            let mut scratch = [0u8; 64 + p9::P9_QID_LEN + 8 + 1 + 2];
            let q = p9::Qid {
                kind: if is_dir { p9::P9_QTDIR } else { p9::P9_QTFILE },
                version: 0,
                path: child,
            };
            let dtype = if is_dir { p9::DT_DIR } else { p9::DT_REG };
            if let Ok(used) = p9::pack_dirent(&mut scratch, 0, &q, ord, dtype, name) {
                data.extend_from_slice(&scratch[..used]);
            } else {
                full = true;
            }
        });
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    fn h_getattr(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        let (mode, nlink) = if ptys.is_dir(f.path) {
            (DIR_MODE, 2u64)
        } else {
            (FILE_RW, 1u64) // ptmx + master + slave are all rw byte channels
        };
        // The security trio (mode/uid/gid) MUST be filled: the kernel's dev9p
        // per-component X-search reads them; an unfilled trio fails closed and the
        // /dev/pts walk is DENIED.
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        let q = self.qid_of(ptys, f.path);
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &q,
            mode,
            T_PRINCIPAL_SYSTEM,
            T_GID_SYSTEM,
            nlink,
            0, // a stream/terminal has no meaningful size
        )
    }

    fn h_clunk(&mut self, ptys: &mut Ptys, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if !self.fid_clunk(ptys, a.fid) {
            return self.err(tag, p9::E_BADF);
        }
        p9::build_rclunk(&mut self.out_buf, tag)
    }

    /// Tflush: the kernel client abandoned an in-flight request (a death-interrupt
    /// on a thread blocked in a master/slave read). Cancel any held Rread under
    /// oldtag -- no late reply -- then Rflush. Per 9P the client reuses oldtag only
    /// after this Rflush (I-10), so the held tag is reclaimed cleanly.
    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        self.pending_reads.retain(|pr| pr.tag != a.oldtag);
        p9::build_rflush(&mut self.out_buf, tag)
    }

    /// Complete any blocking read whose ring now has bytes (or EOF): the serve-loop
    /// analog of netd's poll_data, called at the loop top before the blocking
    /// t_poll. For each parked read, re-attempt the drain: Data/Eof sends the held
    /// Rread (removing the pending); WouldBlock keeps waiting. FIFO over the slot.
    /// False on a held-Rread write failure (the session is dead -> tear down). I-9:
    /// ptyfs is single-threaded, so a write that filled a ring (a prior serviced
    /// frame) is always drained here before the loop parks -- no wake is lost.
    pub fn poll_reads(&mut self, ptys: &mut Ptys) -> bool {
        let mut i = 0;
        while i < self.pending_reads.len() {
            let pr = self.pending_reads[i];
            let mut scratch = alloc::vec![0u8; pr.cap];
            let outcome = if pr.master {
                ptys.master_read(pr.slot_n, &mut scratch[..pr.cap])
            } else {
                ptys.slave_read(pr.slot_n, &mut scratch[..pr.cap])
            };
            match outcome {
                RecvOutcome::WouldBlock => i += 1,
                RecvOutcome::Data(k) => {
                    self.pending_reads.remove(i);
                    if !self.deliver_read(pr.tag, &scratch[..k]) {
                        return false;
                    }
                }
                RecvOutcome::Eof => {
                    self.pending_reads.remove(i);
                    if !self.deliver_read(pr.tag, &[]) {
                        return false;
                    }
                }
            }
        }
        true
    }

    fn deliver_read(&mut self, tag: u16, data: &[u8]) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rread(&mut self.out_buf, tag, data) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }
}

fn rreaddir_budget(count: u32, msize: u32) -> usize {
    (count as usize).min((msize as usize).saturating_sub(p9::P9_HDR_LEN + 4))
}

/// Post /srv/ptyfs (9P-mode; perm 0). Requires PROC_FLAG_MAY_POST_SERVICE (joey
/// spawns ptyfs with T_SPAWN_PERM_MAY_POST_SERVICE, the corvus precedent). The
/// returned listener fd is accepted in the serve loop.
pub fn post_srv_ptyfs() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"ptyfs".as_ptr(), 5, T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

// =============================================================================
// In-server selftest (PTY-2a): the ring/RecvOutcome logic, deterministic and
// mount-independent (the netd echo_e2e analog). Proves the master/slave byte
// round-trip + the EOF-vs-WouldBlock discipline without a real client. The 9P
// server path + the kernel registration are exercised by the in-guest /dev probe
// (PTY-2a's second half).
// =============================================================================

/// Returns Ok(()) or a stage name on failure.
pub fn selftest() -> Result<(), &'static str> {
    let mut ptys = Ptys::new();
    let n = ptys.mint().ok_or("mint")? as u32;
    // Simulate both ends open (a real pts opens the master via clone + the slave
    // via /pts/<n>).
    ptys.open_inc(n, true); // master open
    ptys.open_inc(n, false); // slave open

    // Empty + both open -> WouldBlock, not EOF.
    let mut buf = [0u8; 8];
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("empty-slave-not-wouldblock"),
    }

    // master -> slave.
    if ptys.master_write(n, b"hi") != 2 {
        return Err("master-write");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"hi" => {}
        _ => return Err("slave-read"),
    }
    // Drained.
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("slave-drained-not-wouldblock"),
    }

    // slave -> master (the other direction).
    if ptys.slave_write(n, b"yo") != 2 {
        return Err("slave-write");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"yo" => {}
        _ => return Err("master-read"),
    }

    // FIFO order across two writes.
    let _ = ptys.master_write(n, b"AB");
    let _ = ptys.master_write(n, b"CD");
    let mut one = [0u8; 1];
    for &expect in b"ABCD" {
        match ptys.slave_read(n, &mut one) {
            RecvOutcome::Data(1) if one[0] == expect => {}
            _ => return Err("fifo-order"),
        }
    }

    // Master close -> the slave read sees EOF (drain-then-EOF: nothing queued).
    ptys.open_dec(n, true); // master closed
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Eof => {}
        _ => return Err("master-close-not-eof"),
    }

    // Back-pressure: a write past RING_CAP is a SHORT push, never a drop.
    let big = alloc::vec![0x5au8; RING_CAP + 100];
    let pushed = ptys.slave_write(n, &big);
    if pushed != RING_CAP {
        return Err("backpressure-short-push");
    }

    // The last binding drop frees the slot (refs discipline).
    ptys.ref_path(make_pts(n, FK_SLAVE)); // simulate one bound fid
    let freed = ptys.unref_path(make_pts(n, FK_SLAVE));
    if freed != Some(0) {
        return Err("free-on-last-unref");
    }
    if ptys.live(n) {
        return Err("slot-not-freed");
    }

    Ok(())
}
