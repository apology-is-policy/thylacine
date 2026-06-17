// The /net 9P2000.L server (NET-DESIGN.md section 3).
//
// net-2b-2 serves the STATIC directory skeleton -- the `tcp/`, `udp/`, `icmp/`
// protocol directories, each with a read-only `stats` file -- reachable by walk
// over the dev9p-mounted /net. The `clone`->socket fid state machine (section
// 3.4) and the live per-protocol counters land at net-2c; serving a half-built
// `clone` now would be a fid machine with no socket behind it, so the skeleton
// deliberately stops at walkable dirs + a readable file (the proof of the mount
// path), and net-2c grows it.
//
// netd posts /srv/net 9P-mode (perm=0; requires PROC_FLAG_MAY_POST_SERVICE,
// conferred warden->netd because netd's manifest is `lifecycle = persistent`).
// joey's open=connect mount (t_open(/srv/net, OREAD) -> dev9p root -> t_mount)
// drives ONE kernel dev9p-client 9P session over which every /net access from
// every Proc in the namespace is multiplexed -- so the connection table is
// effectively single-session at v1.0 (net-2c/2d revisit when sockets are
// per-fid). The codec is libthyla_rs::ninep (the corvus-lifted server-side
// 9P2000.L parse/build).

use alloc::vec::Vec;
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_open, t_walk_create, T_GID_SYSTEM, T_OPATH, T_OREAD, T_PRINCIPAL_SYSTEM,
    T_WALK_OPEN_FROM_ROOT,
};

/// Max concurrent 9P connections the accept loop tracks. In practice the dev9p
/// mount drives ONE kernel-client session; the headroom covers a future direct
/// open=connect consumer (a native /net client that does not cross the mount).
pub const MAX_CONNS: usize = 8;

/// Per-connection fid-table size. The static /net tree is shallow; net-2c sizes
/// this to the live-connection fan-out (one fid per open connection directory).
const MAX_FIDS: usize = 32;

/// Server-negotiated msize. Bounds every frame; the per-conn read/out buffers
/// are sized to it. 8 KiB comfortably holds the skeleton's largest reply (a
/// stats read or an Rgetattr).
const SRV_MSIZE: u32 = 8192;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

// Linux mode bits. The /net nodes are SYSTEM-owned but WORLD r-x (dirs) / r
// (files): the kernel's A-3 dev9p enforcement runs a per-component X-search
// against the accessing principal, so a non-`other`-searchable /net would deny
// every logged-in user. World r-x keeps /net usable by any session.
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_MODE: u32 = S_IFREG | 0o444;

/// Tgetattr request-mask: size. ninep exports MODE/NLINK/UID/GID; STATX_SIZE is
/// 0x200 (Linux v9fs), filled so a stat of a stats file reports its length.
const P9_GETATTR_SIZE: u64 = 0x200;

// The static /net node table. The array index IS the qid.path. A node's
// `parent` is its containing directory's path (the root is its own parent, used
// as the `..`-from-root fixpoint). `content` is the file body (empty for dirs).
struct Node {
    name: &'static [u8],
    dir: bool,
    parent: u64,
    content: &'static [u8],
}

// net-2b-2 static counters -- honestly zero (no live connections until net-2c
// stands up the clone->socket machinery, which fills these live).
const TCP_STATS: &[u8] = b"tcp\n  active 0\n  passive 0\n  established 0\n";
const UDP_STATS: &[u8] = b"udp\n  ports 0\n";
const ICMP_STATS: &[u8] = b"icmp\n  echo 0\n";

const NODES: &[Node] = &[
    Node {
        name: b"",
        dir: true,
        parent: 0,
        content: b"",
    }, //          0  /net
    Node {
        name: b"tcp",
        dir: true,
        parent: 0,
        content: b"",
    }, //       1  /net/tcp
    Node {
        name: b"udp",
        dir: true,
        parent: 0,
        content: b"",
    }, //       2  /net/udp
    Node {
        name: b"icmp",
        dir: true,
        parent: 0,
        content: b"",
    }, //      3  /net/icmp
    Node {
        name: b"stats",
        dir: false,
        parent: 1,
        content: TCP_STATS,
    }, //  4 /net/tcp/stats
    Node {
        name: b"stats",
        dir: false,
        parent: 2,
        content: UDP_STATS,
    }, //  5 /net/udp/stats
    Node {
        name: b"stats",
        dir: false,
        parent: 3,
        content: ICMP_STATS,
    }, // 6 /net/icmp/stats
];

const ROOT_PATH: u64 = 0;

fn node(path: u64) -> Option<&'static Node> {
    NODES.get(path as usize)
}

fn qid_of(path: u64) -> p9::Qid {
    let n = &NODES[path as usize];
    p9::Qid {
        kind: if n.dir { p9::P9_QTDIR } else { p9::P9_QTFILE },
        version: 0,
        path,
    }
}

// Resolve one walk component from `dir` (which must be a directory path) to a
// child path. "." stays; ".." goes to the parent; otherwise the named child.
// None == ENOENT.
fn walk_one(dir: u64, name: &[u8]) -> Option<u64> {
    if name == b"." {
        return Some(dir);
    }
    if name == b".." {
        return node(dir).map(|n| n.parent);
    }
    for (i, n) in NODES.iter().enumerate() {
        let p = i as u64;
        if p != ROOT_PATH && n.parent == dir && n.name == name {
            return Some(p);
        }
    }
    None
}

/// Post the /net 9P service (9P-mode) into the boot namespace's /srv. Returns
/// the listener handle. The boot /srv is the immortal registry joey re-grafts
/// across the pivot, so a service posted here is reachable by joey for the
/// mount. perm=0 = 9P-mode (vs DMSRVBYTE byte-mode); netd serves /net as a 9P
/// tree. Err(()) on a post failure (most likely a missing MAY_POST_SERVICE).
pub fn post_srv_net() -> Result<i64, ()> {
    // O_PATH = a navigation base (9P forbids create from an opened fid).
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"net".as_ptr(), 3, T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
}

/// One accepted 9P connection. Owns its fid table + framing buffers.
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

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    fn fid_bind(&mut self, fid: u32, path: u64, opened: bool) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid { fid, path, opened });
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid { fid, path, opened });
            return true;
        }
        false
    }

    fn fid_clunk(&mut self, fid: u32) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = None;
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every COMPLETE 9P frame. Returns false
    /// to close the connection (EOF, framing violation, or write failure). One
    /// `t_read` per call (the caller re-enters via the poll loop), so a partial
    /// frame waits for the next readable event rather than blocking mid-frame.
    pub fn service(&mut self) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            // A full msize buffered with no complete frame -> oversized/malformed.
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n =
            unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            self.in_buf.truncate(cur);
            return false; // EOF (0) or error (<0): tear the connection down.
        }
        self.in_buf.truncate(cur + n as usize);

        loop {
            if self.in_buf.len() < p9::P9_HDR_LEN {
                return true; // need more header bytes
            }
            let hdr = match p9::peek_header(&self.in_buf) {
                Ok(h) => h,
                Err(_) => return false,
            };
            let size = hdr.size as usize;
            if !(p9::P9_HDR_LEN..=SRV_MSIZE_USIZE).contains(&size) {
                return false; // framing violation
            }
            if self.in_buf.len() < size {
                return true; // incomplete frame; wait for more
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            let rlen = self.dispatch(&frame, hdr);
            if rlen == 0 {
                return false; // unrecoverable build failure
            }
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
            p9::P9_TGETATTR => self.h_getattr(tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            // Tauth/Twrite/Treaddir/... are not served by the read-only skeleton.
            _ => self.err(tag, p9::E_NOSYS),
        };
        r.unwrap_or_else(|_| {
            // A build/parse error mid-reply: re-clear (a partial build may have
            // written into out_buf) and emit Rlerror(EPROTO).
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        })
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
        // Tversion resets all session state (the 9P "clunk every fid" semantics).
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
        if !self.fid_bind(a.fid, ROOT_PATH, false) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rattach(&mut self.out_buf, tag, &qid_of(ROOT_PATH))
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
        if a.newfid != a.fid && self.fid_find(a.newfid).is_some() {
            return self.err(tag, p9::E_INVAL); // newfid already in use
        }

        let mut cur = src_fid.path;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut nwalked = 0usize;
        for i in 0..(a.nwname as usize) {
            // Each component requires `cur` to be a directory.
            match node(cur) {
                Some(n) if n.dir => {}
                _ => break,
            }
            match walk_one(cur, a.names[i]) {
                Some(next) => {
                    cur = next;
                    qids[nwalked] = qid_of(next);
                    nwalked += 1;
                }
                None => break,
            }
        }
        // First component failed -> ENOENT (no partial fid established).
        if a.nwname > 0 && nwalked == 0 {
            return self.err(tag, p9::E_NOENT);
        }
        // Per 9P: newfid is set to the last walked element ONLY on a full walk
        // (nwqid == nwname). A partial walk leaves newfid untouched; the client
        // sees nwqid < nwname and reissues. nwname==0 is a clone (newfid -> fid).
        if nwalked == a.nwname as usize && !self.fid_bind(a.newfid, cur, false) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..nwalked])
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
        let mut f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        let _ = a.flags; // read-only tree: write modes are ignored (reads only)
        f.opened = true;
        self.fids[i] = Some(f);
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
        let n = &NODES[f.path as usize];
        if n.dir {
            // Directory listing is Treaddir (net-2c). A Tread on a dir is EISDIR.
            return self.err(tag, p9::E_ISDIR);
        }
        let content = n.content;
        let off = a.offset as usize;
        let avail: &[u8] = if off >= content.len() {
            &[]
        } else {
            &content[off..]
        };
        let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
        let want = (a.count as usize).min(cap).min(avail.len());
        p9::build_rread(&mut self.out_buf, tag, &avail[..want])
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
        let n = &NODES[f.path as usize];
        let (mode, nlink, size) = if n.dir {
            (DIR_MODE, 2u64, 0u64)
        } else {
            (FILE_MODE, 1u64, n.content.len() as u64)
        };
        // The security trio (mode/uid/gid) MUST be filled: the kernel's A-3
        // dev9p per-component X-search reads them, and an unfilled trio
        // fails closed -> the /net walk is DENIED (ninep build_rgetattr doc).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &qid_of(f.path),
            mode,
            T_PRINCIPAL_SYSTEM,
            T_GID_SYSTEM,
            nlink,
            size,
        )
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if !self.fid_clunk(a.fid) {
            return self.err(tag, p9::E_BADF);
        }
        p9::build_rclunk(&mut self.out_buf, tag)
    }
}
