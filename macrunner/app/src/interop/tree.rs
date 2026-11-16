// One channel's 9P2000.L session over the tree version 1 (docs/MAC-RUNNER.md 4.4):
//
//   /           snarf, folders
//   snarf       the Mac pasteboard's text
//   folders/    one directory per shared folder, each that host folder's tree
//
// Every file is reported as owned by the attaching user (the attach's n_uname): the owner bits
// are the host file's, without write in a read-only folder, and group and other have none.
use std::collections::HashMap;
use std::os::fd::{AsRawFd, OwnedFd};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use ninep::*;

use super::folder::{self, name_ok, Rel, Root, Target};
use super::locks::{FileId, Locks, Owner};
use super::log::{latin1, repr, Log};
use super::snarf::Pasteboard;
use super::Share;

/// Past this a write to `snarf` answers EFBIG, and so does a read of a larger pasteboard text.
pub const SNARF_MAX: usize = 16 << 20;
/// The largest msize this server agrees to.
const MSIZE_MAX: u32 = 1 << 20;
/// The smallest: a walk's reply of sixteen qids must fit.
const MSIZE_MIN: u32 = 4096;
/// Requests before a Tversion are held to this.
const MSIZE_FIRST: u32 = 8192;
/// Rread's header and count.
const IOHDR: usize = P9_HDR_LEN + 4;
const V9FS_MAGIC: u32 = 0x0102_1997;

const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;

// The synthetic nodes' qid paths have the top bit set; a host file's never does (qpath).
const SYNTH: u64 = 1 << 63;
const Q_ROOT: u64 = SYNTH | 1;
const Q_SNARF: u64 = SYNTH | 2;
const Q_FOLDERS: u64 = SYNTH | 3;
// A share whose folder cannot be opened is still listed, under this qid path plus its index.
const Q_ABSENT_SHARE: u64 = SYNTH | 0x100;
// A host file's qid path: its device's index + 1 among the devices seen, in bits 56..62, over the
// inode, when the inode fits and a slot is free; otherwise a number from the remap table, whose top
// byte is REMAP. Never the share: two shares can reach one file (one inside the other), and the
// guest knows a file by its qid.
const REMAP: u64 = 0x7F << 56;
const DEV_SLOTS: usize = 126;
pub const MAX_SHARES: usize = 126;

/// What every session of the line shares.
pub struct Shared {
    shares: Vec<ShareRoot>,
    pasteboard: Arc<dyn Pasteboard>,
    pub log: Arc<Log>,
    locks: Locks,
    sessions: AtomicU64,
    remap: Mutex<HashMap<(u64, u64), u64>>,
    devs: Mutex<Vec<u64>>,
    started: (u64, u64),
}

/// A share's folder, opened, and the device it is on.
type OpenRoot = Arc<(Root, u64)>;

struct ShareRoot {
    share: Share,
    // Opened at the first walk into the share that finds the folder, then kept.
    root: Mutex<Option<OpenRoot>>,
}

impl Shared {
    pub fn new(
        shares: Vec<Share>,
        pasteboard: Arc<dyn Pasteboard>,
        log: Arc<Log>,
    ) -> Result<Shared, String> {
        if shares.len() > MAX_SHARES {
            return Err(format!("at most {MAX_SHARES} folders can be shared"));
        }
        for (i, s) in shares.iter().enumerate() {
            if shares[..i].iter().any(|t| t.name == s.name) {
                return Err(format!("two shares are named '{}'", s.name));
            }
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        let mut shares: Vec<ShareRoot> = shares
            .into_iter()
            .map(|share| ShareRoot {
                share,
                root: Mutex::new(None),
            })
            .collect();
        shares.sort_by(|a, b| a.share.name.cmp(&b.share.name));
        Ok(Shared {
            shares,
            pasteboard,
            log,
            locks: Locks::default(),
            sessions: AtomicU64::new(0),
            remap: Mutex::new(HashMap::new()),
            devs: Mutex::new(Vec::new()),
            started: (now.as_secs(), now.subsec_nanos() as u64),
        })
    }

    /// The share's root, opened now if it was not; the folder's errno if it cannot be.
    fn root(&self, share: usize) -> Result<OpenRoot, u32> {
        let s = &self.shares[share];
        let mut held = s.root.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(r) = held.as_ref() {
            return Ok(r.clone());
        }
        let root = Root::open(&s.share.path, s.share.read_only)?;
        let dev = root.lstat(&[])?.st_dev as u32 as u64;
        let r = Arc::new((root, dev));
        *held = Some(r.clone());
        Ok(r)
    }

    fn qpath(&self, dev: u64, ino: u64) -> u64 {
        if ino < 1 << 56 {
            let mut d = self.devs.lock().unwrap_or_else(|e| e.into_inner());
            let slot = match d.iter().position(|&x| x == dev) {
                Some(i) => Some(i),
                None if d.len() < DEV_SLOTS => {
                    d.push(dev);
                    Some(d.len() - 1)
                }
                None => None,
            };
            if let Some(i) = slot {
                return ((i as u64 + 1) << 56) | ino;
            }
        }
        let mut t = self.remap.lock().unwrap_or_else(|e| e.into_inner());
        let next = REMAP | t.len() as u64;
        *t.entry((dev, ino)).or_insert(next)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Node {
    Root,
    Snarf,
    Folders,
    Host { share: usize, rel: Rel },
}

struct Dirent {
    name: Vec<u8>,
    dtype: u8,
    qid: Qid,
}

enum Open {
    No,
    /// A directory: the host one's descriptor (none for a synthetic one), and the listing taken
    /// at the read from offset 0.
    Dir {
        fd: Option<OwnedFd>,
        list: Option<Vec<Dirent>>,
    },
    /// `snarf` for reading: the text taken at the read from offset 0.
    SnarfRead {
        snap: Option<Vec<u8>>,
    },
    /// `snarf` for writing: what is written, set on the pasteboard at the clunk.
    SnarfWrite {
        buf: Vec<u8>,
    },
    File {
        fd: OwnedFd,
        read: bool,
        write: bool,
        append: bool,
    },
}

struct Fid {
    node: Node,
    /// The attach's n_uname: the owner every file reports.
    uid: u32,
    open: Open,
}

pub struct Session {
    shared: Arc<Shared>,
    chan: u32,
    id: u64,
    msize: u32,
    fids: HashMap<u32, Fid>,
    buf: Vec<u8>,
    reply: Vec<u8>,
}

impl Drop for Session {
    fn drop(&mut self) {
        self.shared.locks.release_session(self.id);
    }
}

fn host_mode(st: &libc::stat, read_only: bool) -> u32 {
    let m = st.st_mode as u32;
    let mut owner = m & 0o700;
    if read_only {
        owner &= !0o200;
    }
    (m & S_IFMT) | owner
}

impl Session {
    pub fn new(chan: u32, shared: Arc<Shared>) -> Session {
        let id = shared.sessions.fetch_add(1, Ordering::Relaxed) + 1;
        Session {
            shared,
            chan,
            id,
            msize: MSIZE_FIRST,
            fids: HashMap::new(),
            buf: Vec::new(),
            reply: vec![0; MSIZE_FIRST as usize],
        }
    }

    fn log(&self, what: &str) {
        self.shared.log.line(&format!("chan {} {what}", self.chan));
    }

    /// Bytes of the channel's stream in; every whole request answered, in order, into `out`. An
    /// error is a stream that is not 9P, and the caller closes the channel.
    pub fn feed(&mut self, data: &[u8], out: &mut Vec<u8>) -> Result<(), String> {
        self.buf.extend_from_slice(data);
        let buf = std::mem::take(&mut self.buf);
        let mut at = 0;
        let mut result = Ok(());
        while buf.len() - at >= 4 {
            let size = u32::from_le_bytes([buf[at], buf[at + 1], buf[at + 2], buf[at + 3]]);
            if (size as usize) < P9_HDR_LEN || size > self.msize.max(MSIZE_FIRST) {
                result = Err(format!("9P size {size}"));
                break;
            }
            if buf.len() - at < size as usize {
                break;
            }
            self.answer(&buf[at..at + size as usize], out);
            at += size as usize;
        }
        self.buf = buf;
        self.buf.drain(..at);
        result
    }

    fn answer(&mut self, msg: &[u8], out: &mut Vec<u8>) {
        let (t, tag) = (msg[4], u16::from_le_bytes([msg[5], msg[6]]));
        let n = match self.handle(t, tag, msg) {
            Ok(n) => n,
            Err(e) => {
                build_rlerror(&mut self.reply, tag, e).expect("an Rlerror fits any reply buffer")
            }
        };
        out.extend_from_slice(&self.reply[..n]);
    }

    fn fid(&self, fid: u32) -> Result<&Fid, u32> {
        self.fids.get(&fid).ok_or(E_BADF)
    }

    fn fid_mut(&mut self, fid: u32) -> Result<&mut Fid, u32> {
        self.fids.get_mut(&fid).ok_or(E_BADF)
    }

    fn handle(&mut self, t: u8, tag: u16, msg: &[u8]) -> Result<usize, u32> {
        let proto = |_| E_PROTO;
        match t {
            P9_TVERSION => self.version(tag, parse_tversion(msg).map_err(proto)?),
            P9_TATTACH => self.attach(tag, parse_tattach(msg).map_err(proto)?),
            P9_TWALK => self.walk(tag, parse_twalk(msg).map_err(proto)?),
            P9_TLOPEN => self.lopen(tag, parse_tlopen(msg).map_err(proto)?),
            P9_TREAD => self.read(tag, parse_tread(msg).map_err(proto)?),
            P9_TWRITE => self.write(tag, parse_twrite(msg).map_err(proto)?),
            P9_TCLUNK => self.clunk(tag, parse_tclunk(msg).map_err(proto)?.fid),
            P9_TGETATTR => self.getattr(tag, parse_tgetattr(msg).map_err(proto)?),
            P9_TSETATTR => self.setattr(tag, parse_tsetattr(msg).map_err(proto)?),
            P9_TREADDIR => self.readdir(tag, parse_treaddir(msg).map_err(proto)?),
            P9_TSTATFS => self.statfs(tag, parse_tfid_only(msg).map_err(proto)?),
            P9_TFSYNC => self.fsync(tag, parse_tfsync(msg).map_err(proto)?.fid),
            P9_TREADLINK => self.readlink(tag, parse_tfid_only(msg).map_err(proto)?),
            P9_TLCREATE => self.lcreate(tag, parse_tlcreate(msg).map_err(proto)?),
            P9_TMKDIR => self.mkdir(tag, parse_tmkdir(msg).map_err(proto)?),
            P9_TSYMLINK => self.symlink(tag, parse_tsymlink(msg).map_err(proto)?),
            P9_TMKNOD => self.no_new_kind(parse_tmknod(msg).map_err(proto)?.dfid),
            P9_TLINK => self.no_new_kind(parse_tlink(msg).map_err(proto)?.dfid),
            P9_TRENAME => {
                let a = parse_trename(msg).map_err(proto)?;
                self.rename(tag, P9_RRENAME, Moved::Fid(a.fid), a.dfid, a.name)
            }
            P9_TRENAMEAT => {
                let a = parse_trenameat(msg).map_err(proto)?;
                self.rename(
                    tag,
                    P9_RRENAMEAT,
                    Moved::Name(a.olddirfid, a.oldname),
                    a.newdirfid,
                    a.newname,
                )
            }
            P9_TUNLINKAT => self.unlinkat(tag, parse_tunlinkat(msg).map_err(proto)?),
            P9_TLOCK => self.lock(tag, parse_tlock(msg).map_err(proto)?),
            P9_TGETLOCK => self.getlock(tag, parse_tgetlock(msg).map_err(proto)?),
            // Requests are answered in order, so the one flushed was answered before this.
            P9_TFLUSH => build_rflush(&mut self.reply, tag).map_err(|_| E_IO),
            _ => {
                self.log(&format!("unanswered T{t}"));
                Err(E_OPNOTSUPP)
            }
        }
    }

    fn version(&mut self, tag: u16, a: TversionArgs<'_>) -> Result<usize, u32> {
        // A Tversion begins the session again: every fid goes, and a write to snarf that was not
        // clunked sets nothing.
        self.fids.clear();
        self.shared.locks.release_session(self.id);
        self.log(&format!(
            "version msize={} {}",
            a.msize,
            repr(&latin1(a.version))
        ));
        let ok = a.version == b"9P2000.L" && a.msize >= MSIZE_MIN;
        self.msize = a.msize.clamp(MSIZE_MIN, MSIZE_MAX);
        self.reply.resize(self.msize.max(MSIZE_FIRST) as usize, 0);
        let version: &[u8] = if ok { b"9P2000.L" } else { b"unknown" };
        build_rversion(&mut self.reply, tag, self.msize, version).map_err(|_| E_IO)
    }

    fn attach(&mut self, tag: u16, a: TattachArgs<'_>) -> Result<usize, u32> {
        self.log(&format!(
            "attach n_uname={} uname={} aname={}",
            a.n_uname,
            repr(&latin1(a.uname)),
            repr(&latin1(a.aname))
        ));
        // Other anames are reserved for later trees.
        if !a.aname.is_empty() {
            return Err(E_NOENT);
        }
        // Every file is the attacher's: an attach that names no one has no one to report.
        if a.n_uname == P9_NOFID {
            return Err(E_INVAL);
        }
        if self.fids.contains_key(&a.fid) {
            return Err(E_EXIST);
        }
        self.fids.insert(
            a.fid,
            Fid {
                node: Node::Root,
                uid: a.n_uname,
                open: Open::No,
            },
        );
        build_rattach(&mut self.reply, tag, &synth_qid(Q_ROOT, true)).map_err(|_| E_IO)
    }

    // ------------------------------------------------------------------------------------------
    // Nodes: a qid, a step of a walk, an attribute set.

    fn host_qid(&self, st: &libc::stat) -> Qid {
        let kind = if folder::is_dir(st) {
            P9_QTDIR
        } else if folder::is_link(st) {
            P9_QTSYMLINK
        } else {
            P9_QTFILE
        };
        Qid {
            kind,
            version: qid_version(st),
            path: self.shared.qpath(st.st_dev as u32 as u64, st.st_ino),
        }
    }

    fn share_qid(&self, share: usize) -> Qid {
        match self.shared.root(share).and_then(|r| r.0.lstat(&[])) {
            Ok(st) => self.host_qid(&st),
            Err(_) => synth_qid(Q_ABSENT_SHARE + share as u64, true),
        }
    }

    fn step(&self, node: &Node, name: &[u8]) -> Result<(Node, Qid), u32> {
        if name == b".." {
            return Ok(match node {
                Node::Root | Node::Folders => (Node::Root, synth_qid(Q_ROOT, true)),
                Node::Snarf => return Err(E_NOTDIR),
                Node::Host { rel, .. } if rel.is_empty() => {
                    (Node::Folders, synth_qid(Q_FOLDERS, true))
                }
                Node::Host { share, rel } => {
                    let up = rel[..rel.len() - 1].to_vec();
                    let r = self.shared.root(*share)?;
                    let q = self.host_qid(&r.0.lstat(&up)?);
                    (
                        Node::Host {
                            share: *share,
                            rel: up,
                        },
                        q,
                    )
                }
            });
        }
        if !name_ok(name) {
            return Err(E_NOENT);
        }
        match node {
            Node::Root => match name {
                b"snarf" => Ok((Node::Snarf, synth_qid(Q_SNARF, false))),
                b"folders" => Ok((Node::Folders, synth_qid(Q_FOLDERS, true))),
                _ => Err(E_NOENT),
            },
            Node::Snarf => Err(E_NOTDIR),
            Node::Folders => {
                let i = self
                    .shared
                    .shares
                    .iter()
                    .position(|s| s.share.name.as_bytes() == name)
                    .ok_or(E_NOENT)?;
                let r = self.shared.root(i)?;
                let q = self.host_qid(&r.0.lstat(&[])?);
                Ok((
                    Node::Host {
                        share: i,
                        rel: Vec::new(),
                    },
                    q,
                ))
            }
            Node::Host { share, rel } => {
                let r = self.shared.root(*share)?;
                // A link is never walked through, and the guest reads it and resolves it itself:
                // the lstat opens `rel` with O_DIRECTORY | O_NOFOLLOW_ANY, so a step past a link
                // is ELOOP and one past a file ENOTDIR.
                let mut next = rel.clone();
                next.push(name.to_vec());
                let q = self.host_qid(&r.0.lstat(&next)?);
                Ok((
                    Node::Host {
                        share: *share,
                        rel: next,
                    },
                    q,
                ))
            }
        }
    }

    fn synth_attr(&self, node: &Node, uid: u32) -> Attr {
        let (qid, mode) = match node {
            // The tree's shape is fixed: nothing is created in / or folders/.
            Node::Root => (synth_qid(Q_ROOT, true), S_IFDIR | 0o500),
            Node::Folders => (synth_qid(Q_FOLDERS, true), S_IFDIR | 0o500),
            _ => (synth_qid(Q_SNARF, false), S_IFREG | 0o600),
        };
        let t = self.shared.started;
        Attr {
            valid: P9_GETATTR_BASIC | P9_GETATTR_BTIME,
            qid,
            mode,
            uid,
            gid: uid,
            nlink: if mode & S_IFDIR != 0 { 2 } else { 1 },
            blksize: 4096,
            atime: t,
            mtime: t,
            ctime: t,
            btime: t,
            ..Attr::default()
        }
    }

    fn host_attr(&self, st: &libc::stat, uid: u32, read_only: bool) -> Attr {
        Attr {
            valid: P9_GETATTR_BASIC | P9_GETATTR_BTIME,
            qid: self.host_qid(st),
            mode: host_mode(st, read_only),
            uid,
            gid: uid,
            nlink: st.st_nlink as u64,
            rdev: 0,
            size: st.st_size.max(0) as u64,
            blksize: st.st_blksize.max(0) as u64,
            blocks: st.st_blocks.max(0) as u64,
            atime: folder::time(st.st_atime, st.st_atime_nsec),
            mtime: folder::time(st.st_mtime, st.st_mtime_nsec),
            ctime: folder::time(st.st_ctime, st.st_ctime_nsec),
            btime: folder::time(st.st_birthtime, st.st_birthtime_nsec),
            gen: 0,
            data_version: 0,
        }
    }

    // ------------------------------------------------------------------------------------------
    // Walk, open, read, write, clunk.

    fn walk(&mut self, tag: u16, a: TwalkArgs<'_>) -> Result<usize, u32> {
        let names = &a.names[..a.nwname as usize];
        let from = self.fids.get(&a.fid).map(|f| self.node_name(&f.node));
        let walked = self.walk_from(a.fid, a.newfid, names);
        // Where each walk began: one from a fid held below a directory the Mac replaced reads
        // apart from one that came down from the root again.
        self.log(&format!(
            "walk {} [{}] -> {}",
            from.as_deref().map_or_else(|| String::from("no fid"), repr),
            names
                .iter()
                .map(|n| repr(&latin1(n)))
                .collect::<Vec<_>>()
                .join(", "),
            match &walked {
                Ok(qids) => format!("{} of {}", qids.len(), names.len()),
                Err(e) => format!("errno {e}"),
            }
        ));
        build_rwalk(&mut self.reply, tag, &walked?).map_err(|_| E_IO)
    }

    fn walk_from(&mut self, fid: u32, newfid: u32, names: &[&[u8]]) -> Result<Vec<Qid>, u32> {
        let f = self.fid(fid)?;
        if !matches!(f.open, Open::No) {
            return Err(E_BADF);
        }
        if newfid != fid && self.fids.contains_key(&newfid) {
            return Err(E_EXIST);
        }
        let (mut node, uid) = (f.node.clone(), f.uid);
        let mut qids = Vec::with_capacity(names.len());
        for name in names {
            match self.step(&node, name) {
                Ok((n, q)) => {
                    node = n;
                    qids.push(q);
                }
                // The first step's failure is the walk's; a later one ends it with the prefix.
                Err(e) if qids.is_empty() => return Err(e),
                Err(_) => break,
            }
        }
        if qids.len() == names.len() {
            self.fids.insert(
                newfid,
                Fid {
                    node,
                    uid,
                    open: Open::No,
                },
            );
        }
        Ok(qids)
    }

    /// The node's path in the tree, as the record names it.
    fn node_name(&self, node: &Node) -> String {
        match node {
            Node::Root => String::from("/"),
            Node::Snarf => String::from("snarf"),
            Node::Folders => String::from("folders"),
            Node::Host { share, rel } => {
                let mut s = format!("folders/{}", self.shared.shares[*share].share.name);
                for c in rel {
                    s.push('/');
                    s.push_str(&latin1(c));
                }
                s
            }
        }
    }

    fn lopen(&mut self, tag: u16, a: TlopenArgs) -> Result<usize, u32> {
        let f = self.fid(a.fid)?;
        if !matches!(f.open, Open::No) {
            return Err(E_BADF);
        }
        let acc = a.flags & 3;
        let node = f.node.clone();
        let (open, qid) = match &node {
            Node::Root | Node::Folders => {
                if acc != 0 {
                    return Err(E_ISDIR);
                }
                let q = synth_qid(
                    if node == Node::Root {
                        Q_ROOT
                    } else {
                        Q_FOLDERS
                    },
                    true,
                );
                (
                    Open::Dir {
                        fd: None,
                        list: None,
                    },
                    q,
                )
            }
            Node::Snarf => {
                let q = synth_qid(Q_SNARF, false);
                match acc {
                    0 => (Open::SnarfRead { snap: None }, q),
                    1 => (Open::SnarfWrite { buf: Vec::new() }, q),
                    // A file that is an offer in each direction is never both at once.
                    2 => return Err(E_ACCES),
                    _ => return Err(E_INVAL),
                }
            }
            Node::Host { share, rel } => {
                let r = self.shared.root(*share)?;
                let st = r.0.lstat(rel)?;
                if folder::is_link(&st) {
                    return Err(E_LOOP);
                }
                if folder::is_dir(&st) {
                    if acc != 0 {
                        return Err(E_ISDIR);
                    }
                    let fd = r.0.dir(rel)?;
                    let q = self.host_qid(&folder::fstat(fd.as_raw_fd())?);
                    (
                        Open::Dir {
                            fd: Some(fd),
                            list: None,
                        },
                        q,
                    )
                } else if folder::is_reg(&st) {
                    let flags = folder::mac_open_flags(a.flags)?;
                    if r.0.read_only && (acc != 0 || flags & libc::O_TRUNC != 0) {
                        return Err(E_ROFS);
                    }
                    let fd = r.0.open_at(rel, flags)?;
                    let st = folder::fstat(fd.as_raw_fd())?;
                    // The name may have changed under the walk: only a regular file is opened.
                    if !folder::is_reg(&st) {
                        return Err(E_PERM);
                    }
                    let q = self.host_qid(&st);
                    let open = Open::File {
                        fd,
                        read: acc != 1,
                        write: acc != 0,
                        append: flags & libc::O_APPEND != 0,
                    };
                    (open, q)
                } else {
                    // A fifo, socket or device: opening it would hand the guest the host's.
                    return Err(E_PERM);
                }
            }
        };
        self.fid_mut(a.fid)?.open = open;
        build_rlopen(&mut self.reply, tag, &qid, 0).map_err(|_| E_IO)
    }

    fn read(&mut self, tag: u16, a: TreadArgs) -> Result<usize, u32> {
        let room = (a.count as usize).min(self.msize as usize - IOHDR);
        let chan = self.chan;
        let pasteboard = self.shared.pasteboard.clone();
        let mut logged = None;
        let f = self.fids.get_mut(&a.fid).ok_or(E_BADF)?;
        let data: Vec<u8> = match &mut f.open {
            Open::No | Open::SnarfWrite { .. } => return Err(E_BADF),
            Open::Dir { .. } => return Err(E_ISDIR),
            Open::SnarfRead { snap } => {
                if a.offset == 0 || snap.is_none() {
                    let text = pasteboard.text();
                    logged = Some(text.len());
                    *snap = Some(text);
                }
                let s = snap.as_ref().map(Vec::as_slice).unwrap_or_default();
                if s.len() > SNARF_MAX {
                    return Err(E_FBIG);
                }
                let off = (a.offset.min(s.len() as u64)) as usize;
                s[off..(off + room).min(s.len())].to_vec()
            }
            Open::File { fd, read, .. } => {
                if !*read {
                    return Err(E_BADF);
                }
                let mut b = vec![0u8; room];
                let n = folder::pread(fd.as_raw_fd(), &mut b, a.offset)?;
                b.truncate(n);
                b
            }
        };
        if let Some(n) = logged {
            self.shared
                .log
                .line(&format!("chan {chan} snarf read {n} bytes"));
        }
        build_rread(&mut self.reply, tag, &data).map_err(|_| E_IO)
    }

    fn write(&mut self, tag: u16, a: TwriteArgs<'_>) -> Result<usize, u32> {
        let f = self.fids.get_mut(&a.fid).ok_or(E_BADF)?;
        let n = match &mut f.open {
            Open::SnarfWrite { buf } => {
                if buf.len() + a.data.len() > SNARF_MAX {
                    return Err(E_FBIG);
                }
                buf.extend_from_slice(a.data);
                a.data.len()
            }
            Open::File {
                fd,
                write: true,
                append,
                ..
            } => folder::write_at(fd.as_raw_fd(), a.data, a.offset, *append)?,
            _ => return Err(E_BADF),
        };
        build_rwrite(&mut self.reply, tag, n as u32).map_err(|_| E_IO)
    }

    fn clunk(&mut self, tag: u16, fid: u32) -> Result<usize, u32> {
        let f = self.fids.remove(&fid).ok_or(E_BADF)?;
        self.shared.locks.release_fid(self.id, fid);
        if let Open::SnarfWrite { buf } = f.open {
            let text = String::from_utf8_lossy(&buf);
            let set = self.shared.pasteboard.set_text(&text);
            self.log(&format!("snarf set {} bytes {}", text.len(), repr(&text)));
            if !set {
                self.log("the pasteboard refused the text");
            }
        }
        build_rclunk(&mut self.reply, tag).map_err(|_| E_IO)
    }

    // ------------------------------------------------------------------------------------------
    // Attributes, directories, the file system.

    fn getattr(&mut self, tag: u16, fid: u32) -> Result<usize, u32> {
        let f = self.fid(fid)?;
        let attr = match &f.node {
            Node::Host { share, rel } => {
                let r = self.shared.root(*share)?;
                // An open file is asked through its descriptor: a rename since does not lose it.
                let st = match &f.open {
                    Open::File { fd, .. } | Open::Dir { fd: Some(fd), .. } => {
                        folder::fstat(fd.as_raw_fd())?
                    }
                    _ => r.0.lstat(rel)?,
                };
                self.host_attr(&st, f.uid, r.0.read_only)
            }
            n => self.synth_attr(n, f.uid),
        };
        build_rgetattr_attr(&mut self.reply, tag, &attr).map_err(|_| E_IO)
    }

    fn setattr(&mut self, tag: u16, a: TsetattrArgs) -> Result<usize, u32> {
        let f = self.fid(a.fid)?;
        // Ownership is the attacher's, always: a change of it is refused, a no-op allowed.
        if (a.valid & P9_SETATTR_UID != 0 && a.uid != f.uid)
            || (a.valid & P9_SETATTR_GID != 0 && a.gid != f.uid)
        {
            return Err(E_PERM);
        }
        let times = P9_SETATTR_ATIME | P9_SETATTR_MTIME | P9_SETATTR_CTIME;
        let (share, rel) = match &f.node {
            Node::Host { share, rel } => (*share, rel.clone()),
            Node::Snarf => {
                // `> snarf` may truncate it, and a touch may set its times: nothing to keep.
                if a.valid & P9_SETATTR_MODE != 0 || (a.valid & P9_SETATTR_SIZE != 0 && a.size != 0)
                {
                    return Err(E_PERM);
                }
                return build_rsetattr(&mut self.reply, tag).map_err(|_| E_IO);
            }
            _ => {
                if a.valid & (P9_SETATTR_MODE | P9_SETATTR_SIZE | times) != 0 {
                    return Err(E_PERM);
                }
                return build_rsetattr(&mut self.reply, tag).map_err(|_| E_IO);
            }
        };
        let changes =
            a.valid & (P9_SETATTR_MODE | P9_SETATTR_SIZE | P9_SETATTR_ATIME | P9_SETATTR_MTIME);
        let r = self.shared.root(share)?;
        if changes != 0 && r.0.read_only {
            return Err(E_ROFS);
        }
        let open_fd = match &f.open {
            Open::File { fd, .. } | Open::Dir { fd: Some(fd), .. } => Some(fd.as_raw_fd()),
            _ => None,
        };
        let parent = if open_fd.is_none() {
            r.0.parent(&rel)?
        } else {
            None
        };
        let root_fd = if open_fd.is_none() && parent.is_none() {
            Some(r.0.dir(&[])?)
        } else {
            None
        };
        let target = match (&open_fd, &parent, &root_fd) {
            (Some(fd), _, _) => Target::Fd(*fd),
            (None, Some((dir, name)), _) => Target::Name(dir.as_raw_fd(), name),
            (None, None, Some(fd)) => Target::Fd(fd.as_raw_fd()),
            (None, None, None) => unreachable!("the root is opened when there is no parent"),
        };
        if a.valid & P9_SETATTR_MODE != 0 {
            folder::chmod(target, a.mode)?;
        }
        if a.valid & P9_SETATTR_SIZE != 0 {
            match &f.open {
                Open::File {
                    fd, write: true, ..
                } => folder::ftruncate(fd.as_raw_fd(), a.size)?,
                _ => {
                    let st = r.0.lstat(&rel)?;
                    if folder::is_dir(&st) {
                        return Err(E_ISDIR);
                    }
                    if !folder::is_reg(&st) {
                        return Err(E_INVAL);
                    }
                    let fd = r.0.open_at(&rel, libc::O_WRONLY)?;
                    folder::ftruncate(fd.as_raw_fd(), a.size)?;
                }
            }
        }
        if a.valid & (P9_SETATTR_ATIME | P9_SETATTR_MTIME) != 0 {
            let at = (a.valid & P9_SETATTR_ATIME != 0).then(|| {
                (a.valid & P9_SETATTR_ATIME_SET != 0).then_some((a.atime_sec, a.atime_nsec))
            });
            let mt = (a.valid & P9_SETATTR_MTIME != 0).then(|| {
                (a.valid & P9_SETATTR_MTIME_SET != 0).then_some((a.mtime_sec, a.mtime_nsec))
            });
            folder::utimes(target, at, mt)?;
        }
        build_rsetattr(&mut self.reply, tag).map_err(|_| E_IO)
    }

    fn listing(&self, node: &Node, fd: Option<&OwnedFd>) -> Result<Vec<Dirent>, u32> {
        Ok(match node {
            Node::Root => vec![
                Dirent {
                    name: b"folders".to_vec(),
                    dtype: DT_DIR,
                    qid: synth_qid(Q_FOLDERS, true),
                },
                Dirent {
                    name: b"snarf".to_vec(),
                    dtype: DT_REG,
                    qid: synth_qid(Q_SNARF, false),
                },
            ],
            Node::Folders => (0..self.shared.shares.len())
                .map(|i| Dirent {
                    name: self.shared.shares[i].share.name.as_bytes().to_vec(),
                    dtype: DT_DIR,
                    qid: self.share_qid(i),
                })
                .collect(),
            Node::Host { .. } => {
                let fd = fd.ok_or(E_BADF)?.as_raw_fd();
                let dev = folder::fstat(fd)?.st_dev as u32 as u64;
                folder::entries(fd)?
                    .into_iter()
                    .map(|e| {
                        let kind = match e.dtype {
                            libc::DT_DIR => P9_QTDIR,
                            libc::DT_LNK => P9_QTSYMLINK,
                            _ => P9_QTFILE,
                        };
                        let path = self.shared.qpath(dev, e.ino);
                        Dirent {
                            name: e.name,
                            dtype: e.dtype,
                            qid: Qid {
                                kind,
                                version: 0,
                                path,
                            },
                        }
                    })
                    .collect()
            }
            Node::Snarf => return Err(E_NOTDIR),
        })
    }

    fn readdir(&mut self, tag: u16, a: TreaddirArgs) -> Result<usize, u32> {
        let f = self.fid(a.fid)?;
        let (fd, have) = match &f.open {
            Open::Dir { fd, list } => (fd.as_ref(), list.is_some()),
            Open::No => return Err(E_BADF),
            _ => return Err(E_NOTDIR),
        };
        // The listing is taken at offset 0, and later offsets continue it: the cookies are its
        // indices, so they stay true however the directory changes meanwhile.
        let fresh = if a.offset == 0 || !have {
            Some(self.listing(&f.node.clone(), fd)?)
        } else {
            None
        };
        let room = (a.count as usize).min(self.msize as usize - IOHDR);
        let f = self.fid_mut(a.fid)?;
        let Open::Dir { list, .. } = &mut f.open else {
            unreachable!("checked above")
        };
        if let Some(l) = fresh {
            *list = Some(l);
        }
        let list = list.as_ref().expect("a listing was just taken");
        let mut data = vec![0u8; room];
        let mut n = 0;
        for (i, e) in list
            .iter()
            .enumerate()
            .skip(a.offset.min(list.len() as u64) as usize)
        {
            if n + dirent_len(e.name.len()) > room {
                break;
            }
            n = pack_dirent(&mut data, n, &e.qid, i as u64 + 1, e.dtype, &e.name)
                .map_err(|_| E_IO)?;
        }
        data.truncate(n);
        build_rreaddir(&mut self.reply, tag, &data).map_err(|_| E_IO)
    }

    fn statfs(&mut self, tag: u16, fid: u32) -> Result<usize, u32> {
        let f = self.fid(fid)?;
        let st = match &f.node {
            Node::Host { share, .. } => {
                let s = self.shared.root(*share)?.0.statfs()?;
                // SAFETY: fsid_t is a repr(C) struct of one [i32; 2], its field private in libc.
                let fsid: [i32; 2] = unsafe { std::mem::transmute(s.f_fsid) };
                Statfs {
                    kind: V9FS_MAGIC,
                    bsize: s.f_bsize,
                    blocks: s.f_blocks,
                    bfree: s.f_bfree,
                    bavail: s.f_bavail,
                    files: s.f_files,
                    ffree: s.f_ffree,
                    fsid: (fsid[0] as u32 as u64) | ((fsid[1] as u32 as u64) << 32),
                    namelen: 255,
                }
            }
            _ => Statfs {
                kind: V9FS_MAGIC,
                bsize: 4096,
                namelen: 255,
                ..Statfs::default()
            },
        };
        build_rstatfs(&mut self.reply, tag, &st).map_err(|_| E_IO)
    }

    fn fsync(&mut self, tag: u16, fid: u32) -> Result<usize, u32> {
        let synced = self.fsync_fid(fid);
        // What was made durable, in order with the walks: a move syncs its copy and the
        // destination's directory before its source goes.
        let name = self.fids.get(&fid).map(|f| self.node_name(&f.node));
        self.log(&format!(
            "fsync {} -> {}",
            name.as_deref().map_or_else(|| String::from("no fid"), repr),
            match &synced {
                Ok(()) => String::from("ok"),
                Err(e) => format!("errno {e}"),
            }
        ));
        synced?;
        build_rfsync(&mut self.reply, tag).map_err(|_| E_IO)
    }

    fn fsync_fid(&self, fid: u32) -> Result<(), u32> {
        match &self.fid(fid)?.open {
            Open::No => return Err(E_BADF),
            Open::File { fd, .. } | Open::Dir { fd: Some(fd), .. } => {
                folder::fsync(fd.as_raw_fd())?
            }
            _ => {}
        }
        Ok(())
    }

    fn readlink(&mut self, tag: u16, fid: u32) -> Result<usize, u32> {
        let Node::Host { share, rel } = &self.fid(fid)?.node else {
            return Err(E_INVAL);
        };
        let r = self.shared.root(*share)?;
        let Some((dir, name)) = r.0.parent(rel)? else {
            return Err(E_INVAL);
        };
        if !folder::is_link(&folder::fstatat(dir.as_raw_fd(), name)?) {
            return Err(E_INVAL);
        }
        let target = folder::readlinkat(dir.as_raw_fd(), name)?;
        build_rreadlink(&mut self.reply, tag, &target).map_err(|_| E_IO)
    }

    // ------------------------------------------------------------------------------------------
    // Creating, renaming, removing.

    /// The directory a create or a name operation acts in: its share, root and path, writable.
    fn writable_dir(&self, fid: u32, name: &[u8]) -> Result<(usize, Rel, OwnedFd), u32> {
        let (share, rel) = match &self.fid(fid)?.node {
            Node::Host { share, rel } => (*share, rel.clone()),
            Node::Snarf => return Err(E_NOTDIR),
            // The tree's shape is fixed.
            Node::Root | Node::Folders => return Err(E_PERM),
        };
        let r = self.shared.root(share)?;
        if r.0.read_only {
            return Err(E_ROFS);
        }
        if !name_ok(name) {
            return Err(E_INVAL);
        }
        let dir = r.0.dir(&rel)?;
        Ok((share, rel, dir))
    }

    fn lcreate(&mut self, tag: u16, a: TlcreateArgs<'_>) -> Result<usize, u32> {
        if !matches!(self.fid(a.fid)?.open, Open::No) {
            return Err(E_BADF);
        }
        let (share, mut rel, dir) = self.writable_dir(a.fid, a.name)?;
        let flags = folder::mac_open_flags(a.flags)?;
        let fd = folder::create(dir.as_raw_fd(), a.name, flags, a.mode)?;
        let qid = self.host_qid(&folder::fstat(fd.as_raw_fd())?);
        rel.push(a.name.to_vec());
        let acc = a.flags & 3;
        let f = self.fid_mut(a.fid)?;
        f.node = Node::Host { share, rel };
        f.open = Open::File {
            fd,
            read: acc != 1,
            write: acc != 0,
            append: flags & libc::O_APPEND != 0,
        };
        build_rlcreate(&mut self.reply, tag, &qid, 0).map_err(|_| E_IO)
    }

    fn mkdir(&mut self, tag: u16, a: TmkdirArgs<'_>) -> Result<usize, u32> {
        let (_, _, dir) = self.writable_dir(a.dfid, a.name)?;
        folder::mkdirat(dir.as_raw_fd(), a.name, a.mode)?;
        let qid = self.host_qid(&folder::fstatat(dir.as_raw_fd(), a.name)?);
        build_rmkdir(&mut self.reply, tag, &qid).map_err(|_| E_IO)
    }

    fn symlink(&mut self, tag: u16, a: TsymlinkArgs<'_>) -> Result<usize, u32> {
        let (_, _, dir) = self.writable_dir(a.fid, a.name)?;
        folder::symlinkat(a.target, dir.as_raw_fd(), a.name)?;
        let qid = self.host_qid(&folder::fstatat(dir.as_raw_fd(), a.name)?);
        build_rsymlink(&mut self.reply, tag, &qid).map_err(|_| E_IO)
    }

    /// Tmknod and Tlink: a device, a fifo or a second name for a file is never made on the Mac.
    fn no_new_kind(&mut self, dfid: u32) -> Result<usize, u32> {
        match &self.fid(dfid)?.node {
            Node::Host { share, .. } if self.shared.root(*share)?.0.read_only => Err(E_ROFS),
            _ => Err(E_PERM),
        }
    }

    fn rename(
        &mut self,
        tag: u16,
        rtype: u8,
        from: Moved<'_>,
        newdir: u32,
        newname: &[u8],
    ) -> Result<usize, u32> {
        // In order with the walks and syncs: a move renames its synced copy over the destination
        // before it syncs the destination's directory.
        let old = match &from {
            Moved::Fid(fid) => self.named(*fid, None),
            Moved::Name(dfid, name) => self.named(*dfid, Some(*name)),
        };
        let new = self.named(newdir, Some(newname));
        let moved = self.rename_node(from, newdir, newname);
        self.log(&format!(
            "rename {old} -> {new} -> {}",
            match &moved {
                Ok(()) => String::from("ok"),
                Err(e) => format!("errno {e}"),
            }
        ));
        moved?;
        let n = match rtype {
            P9_RRENAME => build_rrename(&mut self.reply, tag),
            _ => build_rrenameat(&mut self.reply, tag),
        };
        n.map_err(|_| E_IO)
    }

    fn rename_node(&mut self, from: Moved<'_>, newdir: u32, newname: &[u8]) -> Result<(), u32> {
        // The moved file: its share, and its path as the directory and name it is in.
        let (share, from_rel) = match from {
            Moved::Fid(fid) => match &self.fid(fid)?.node {
                Node::Host { rel, .. } if rel.is_empty() => return Err(E_BUSY),
                Node::Host { share, rel } => (*share, rel.clone()),
                _ => return Err(E_PERM),
            },
            Moved::Name(dfid, name) => match &self.fid(dfid)?.node {
                Node::Host { share, rel } => {
                    if !name_ok(name) {
                        return Err(E_INVAL);
                    }
                    let mut r = rel.clone();
                    r.push(name.to_vec());
                    (*share, r)
                }
                Node::Snarf => return Err(E_NOTDIR),
                _ => return Err(E_PERM),
            },
        };
        let (to_share, to_dir_rel, to_dir) = self.writable_dir(newdir, newname)?;
        if to_share != share {
            return Err(E_XDEV);
        }
        let r = self.shared.root(share)?;
        let (from_dir, from_name) =
            r.0.parent(&from_rel)?
                .expect("a moved path is never the root");
        folder::renameat(from_dir.as_raw_fd(), from_name, to_dir.as_raw_fd(), newname)?;
        let mut to_rel = to_dir_rel;
        to_rel.push(newname.to_vec());
        self.moved(share, &from_rel, &to_rel);
        Ok(())
    }

    /// How the record names what a request names: the fid's file, and with `name`, that entry
    /// in it.
    fn named(&self, fid: u32, name: Option<&[u8]>) -> String {
        match self.fids.get(&fid) {
            None => String::from("no fid"),
            Some(f) => {
                let mut s = self.node_name(&f.node);
                if let Some(n) = name {
                    s.push('/');
                    s.push_str(&latin1(n));
                }
                repr(&s)
            }
        }
    }

    // A fid names a path: after a rename in this session, the fids below the old path name the
    // new one, as the guest's open names follow the file.
    fn moved(&mut self, share: usize, from: &[Vec<u8>], to: &[Vec<u8>]) {
        for f in self.fids.values_mut() {
            if let Node::Host { share: s, rel } = &mut f.node {
                if *s == share && rel.len() >= from.len() && rel[..from.len()] == *from {
                    let mut n = to.to_vec();
                    n.extend_from_slice(&rel[from.len()..]);
                    *rel = n;
                }
            }
        }
    }

    fn unlinkat(&mut self, tag: u16, a: TunlinkatArgs<'_>) -> Result<usize, u32> {
        // In order with the walks, renames and syncs: a move removes its source only once its
        // copy is synced, renamed into place and the destination's directory synced.
        let name = self.named(a.dfid, Some(a.name));
        let removed = self.unlink_entry(&a);
        self.log(&format!(
            "unlink {name} -> {}",
            match &removed {
                Ok(()) => String::from("ok"),
                Err(e) => format!("errno {e}"),
            }
        ));
        removed?;
        build_runlinkat(&mut self.reply, tag).map_err(|_| E_IO)
    }

    fn unlink_entry(&self, a: &TunlinkatArgs<'_>) -> Result<(), u32> {
        if a.flags & !P9_UNLINK_AT_REMOVEDIR != 0 {
            return Err(E_INVAL);
        }
        let (_, _, dir) = self.writable_dir(a.dfid, a.name)?;
        let rmdir = a.flags & P9_UNLINK_AT_REMOVEDIR != 0;
        // macOS answers EPERM for an unlink of a directory; Linux, whose errno the guest reads,
        // answers EISDIR.
        if !rmdir && folder::is_dir(&folder::fstatat(dir.as_raw_fd(), a.name)?) {
            return Err(E_ISDIR);
        }
        folder::unlinkat(dir.as_raw_fd(), a.name, rmdir)
    }

    // ------------------------------------------------------------------------------------------
    // Locks.

    fn lock_file(&self, fid: u32) -> Result<FileId, u32> {
        let f = self.fid(fid)?;
        match &f.node {
            Node::Host { share, rel } => {
                let st = match &f.open {
                    Open::File { fd, .. } | Open::Dir { fd: Some(fd), .. } => {
                        folder::fstat(fd.as_raw_fd())?
                    }
                    _ => self.shared.root(*share)?.0.lstat(rel)?,
                };
                Ok((st.st_dev as u32 as u64, st.st_ino))
            }
            Node::Root => Ok((u64::MAX, Q_ROOT)),
            Node::Snarf => Ok((u64::MAX, Q_SNARF)),
            Node::Folders => Ok((u64::MAX, Q_FOLDERS)),
        }
    }

    fn lock(&mut self, tag: u16, a: TlockArgs<'_>) -> Result<usize, u32> {
        if a.kind > P9_LOCK_TYPE_UNLCK {
            return Err(E_INVAL);
        }
        let file = self.lock_file(a.fid)?;
        let owner = Owner {
            session: self.id,
            client: a.client_id.to_vec(),
            proc_id: a.proc_id,
        };
        let status = self
            .shared
            .locks
            .lock(file, &owner, a.kind, a.start, a.length, a.fid);
        build_rlock(&mut self.reply, tag, status).map_err(|_| E_IO)
    }

    fn getlock(&mut self, tag: u16, a: TgetlockArgs<'_>) -> Result<usize, u32> {
        if a.kind > P9_LOCK_TYPE_UNLCK {
            return Err(E_INVAL);
        }
        let file = self.lock_file(a.fid)?;
        let owner = Owner {
            session: self.id,
            client: a.client_id.to_vec(),
            proc_id: a.proc_id,
        };
        let n = match self
            .shared
            .locks
            .test(file, &owner, a.kind, a.start, a.length)
        {
            Some(h) => {
                let kind = if h.write {
                    P9_LOCK_TYPE_WRLCK
                } else {
                    P9_LOCK_TYPE_RDLCK
                };
                build_rgetlock(
                    &mut self.reply,
                    tag,
                    kind,
                    h.start,
                    h.length,
                    h.proc_id,
                    &h.client,
                )
            }
            None => build_rgetlock(
                &mut self.reply,
                tag,
                P9_LOCK_TYPE_UNLCK,
                a.start,
                a.length,
                a.proc_id,
                a.client_id,
            ),
        };
        n.map_err(|_| E_IO)
    }
}

enum Moved<'a> {
    /// Trename: the fid names the file.
    Fid(u32),
    /// Trenameat: a directory's fid and a name in it.
    Name(u32, &'a [u8]),
}

/// A file's version. Any change to its content, size, mode or owner moves its change time, which
/// no call sets back, so the change and modification times to the nanosecond, with the size, are
/// folded into the 32 bits a qid carries: a rewrite that keeps the size within one second is a new
/// version, and so is a new file given a removed one's inode.
fn qid_version(st: &libc::stat) -> u32 {
    let mut h: u64 = 0;
    for v in [
        st.st_ctime as u64,
        st.st_ctime_nsec as u64,
        st.st_mtime as u64,
        st.st_mtime_nsec as u64,
        st.st_size as u64,
    ] {
        // splitmix64's finalizer over each word in turn: every bit of every word reaches the
        // low 32.
        h = (h ^ v).wrapping_add(0x9e37_79b9_7f4a_7c15);
        h = (h ^ (h >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        h = (h ^ (h >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        h ^= h >> 31;
    }
    h as u32
}

fn synth_qid(path: u64, dir: bool) -> Qid {
    Qid {
        kind: if dir { P9_QTDIR } else { P9_QTFILE },
        version: 0,
        path,
    }
}

#[cfg(test)]
#[path = "tree_tests.rs"]
mod tests;
