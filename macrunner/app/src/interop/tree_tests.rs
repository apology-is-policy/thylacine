// The session against a real host folder, request by request, as the kernel's client sends them
// (kernel/9p_wire.c's layouts).
use super::*;
use crate::interop::snarf::MemoryPasteboard;
use std::fs;
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::PathBuf;

const ME: u32 = 1000;

struct Dir(PathBuf);

impl Dir {
    fn new(tag: &str) -> Dir {
        let d = std::env::temp_dir().join(format!("thylacine-tree-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        Dir(d)
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// A T-message body, field by field.
#[derive(Default)]
struct B(Vec<u8>);

impl B {
    fn u8(mut self, v: u8) -> B {
        self.0.push(v);
        self
    }
    fn u16(mut self, v: u16) -> B {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u32(mut self, v: u32) -> B {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u64(mut self, v: u64) -> B {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn s(self, v: &[u8]) -> B {
        let mut b = self.u16(v.len() as u16);
        b.0.extend_from_slice(v);
        b
    }
}

/// An R-message: its type and body.
struct R {
    t: u8,
    body: Vec<u8>,
}

impl R {
    fn ok(&self, want: u8) -> &[u8] {
        assert_eq!(
            self.t,
            want,
            "wanted R{want}, got R{} (errno {:?})",
            self.t,
            self.errno()
        );
        &self.body
    }
    fn errno(&self) -> Option<u32> {
        (self.t == P9_RLERROR).then(|| u32::from_le_bytes(self.body[..4].try_into().unwrap()))
    }
    fn qid(&self, at: usize) -> Qid {
        let b = &self.body[at..];
        Qid {
            kind: b[0],
            version: u32::from_le_bytes(b[1..5].try_into().unwrap()),
            path: u64::from_le_bytes(b[5..13].try_into().unwrap()),
        }
    }
}

struct Client {
    s: Session,
    tag: u16,
    pb: Arc<MemoryPasteboard>,
    log: PathBuf,
}

impl Client {
    fn new(shares: Vec<Share>, snarf: &str, log_dir: &Dir) -> Client {
        let pb = Arc::new(MemoryPasteboard::new(snarf));
        let log_path = log_dir.0.join("line.log");
        let log = Arc::new(Log::open(Some(&log_path)).unwrap());
        let shared = Arc::new(Shared::new(shares, pb.clone(), log).unwrap());
        let mut c = Client {
            s: Session::new(7, shared),
            tag: 0,
            pb,
            log: log_path,
        };
        c.call(P9_TVERSION, B::default().u32(65536).s(b"9P2000.L"))
            .ok(P9_RVERSION);
        c.call(
            P9_TATTACH,
            B::default().u32(1).u32(P9_NOFID).s(b"").s(b"").u32(ME),
        )
        .ok(P9_RATTACH);
        c
    }

    fn call(&mut self, t: u8, body: B) -> R {
        self.tag += 1;
        let mut msg = Vec::new();
        msg.extend_from_slice(&((P9_HDR_LEN + body.0.len()) as u32).to_le_bytes());
        msg.push(t);
        msg.extend_from_slice(&self.tag.to_le_bytes());
        msg.extend_from_slice(&body.0);
        let mut out = Vec::new();
        self.s.feed(&msg, &mut out).unwrap();
        let h = peek_header(&out).unwrap();
        assert_eq!(
            (h.size as usize, h.tag),
            (out.len(), self.tag),
            "one whole reply, to this tag"
        );
        R {
            t: h.mtype,
            body: out[P9_HDR_LEN..].to_vec(),
        }
    }

    /// Walk from the attach root to `path` as `newfid`.
    fn walk(&mut self, newfid: u32, path: &str) -> R {
        let names: Vec<&str> = path.split('/').filter(|s| !s.is_empty()).collect();
        let mut b = B::default().u32(1).u32(newfid).u16(names.len() as u16);
        for n in &names {
            b = b.s(n.as_bytes());
        }
        self.call(P9_TWALK, b)
    }

    fn open(&mut self, fid: u32, flags: u32) -> R {
        self.call(P9_TLOPEN, B::default().u32(fid).u32(flags))
    }

    fn read(&mut self, fid: u32, off: u64, count: u32) -> Vec<u8> {
        let r = self.call(P9_TREAD, B::default().u32(fid).u64(off).u32(count));
        r.ok(P9_RREAD)[4..].to_vec()
    }

    fn clunk(&mut self, fid: u32) {
        self.call(P9_TCLUNK, B::default().u32(fid)).ok(P9_RCLUNK);
    }

    /// mode, uid, gid, size of a getattr.
    fn attr(&mut self, fid: u32) -> (u32, u32, u32, u64) {
        let r = self.call(P9_TGETATTR, B::default().u32(fid).u64(P9_GETATTR_BASIC));
        let b = r.ok(P9_RGETATTR);
        assert_eq!(b.len(), P9_RGETATTR_BODY_LEN);
        let u = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
        let size = u64::from_le_bytes(b[8 + 13 + 12 + 16..8 + 13 + 12 + 24].try_into().unwrap());
        (u(21), u(25), u(29), size)
    }

    /// Every name a readdir of the open directory `fid` lists, `count` bytes at a time.
    fn names(&mut self, fid: u32, count: u32) -> Vec<String> {
        let mut out = Vec::new();
        let mut off = 0u64;
        loop {
            let r = self.call(P9_TREADDIR, B::default().u32(fid).u64(off).u32(count));
            let b = r.ok(P9_RREADDIR);
            let n = u32::from_le_bytes(b[..4].try_into().unwrap()) as usize;
            if n == 0 {
                return out;
            }
            let mut p = 4;
            while p < 4 + n {
                let next = u64::from_le_bytes(b[p + 13..p + 21].try_into().unwrap());
                let len = u16::from_le_bytes(b[p + 22..p + 24].try_into().unwrap()) as usize;
                out.push(String::from_utf8(b[p + 24..p + 24 + len].to_vec()).unwrap());
                assert!(next > off, "cookies only grow");
                off = next;
                p += 24 + len;
            }
        }
    }

    fn logged(&self) -> String {
        fs::read_to_string(&self.log).unwrap_or_default()
    }
}

fn share(name: &str, d: &Dir, read_only: bool) -> Share {
    Share {
        name: name.to_string(),
        path: d.0.clone(),
        read_only,
    }
}

#[test]
fn the_root_lists_folders_and_snarf_and_everything_is_the_attachers() {
    let logs = Dir::new("root-log");
    let mut c = Client::new(vec![], "", &logs);
    assert_eq!(c.attr(1), (S_IFDIR | 0o500, ME, ME, 0));
    c.walk(2, "").ok(P9_RWALK);
    c.open(2, 0).ok(P9_RLOPEN);
    assert_eq!(c.names(2, 8192), ["folders", "snarf"]);
    c.walk(3, "folders").ok(P9_RWALK);
    c.open(3, 0).ok(P9_RLOPEN);
    assert!(c.names(3, 8192).is_empty(), "nothing shared: an empty list");
    assert_eq!(c.walk(4, "nothing").errno(), Some(E_NOENT));
    assert!(
        c.logged()
            .contains("chan 7 attach n_uname=1000 uname='' aname=''"),
        "{}",
        c.logged()
    );
}

#[test]
fn one_host_file_has_one_qid_whichever_share_reaches_it() {
    let d = Dir::new("qid-outer");
    fs::create_dir(d.0.join("sub")).unwrap();
    fs::write(d.0.join("sub/f.txt"), b"one file\n").unwrap();
    fs::write(d.0.join("g.txt"), b"another\n").unwrap();
    let logs = Dir::new("qid-log");
    let inner = Share {
        name: "inner".to_string(),
        path: d.0.join("sub"),
        read_only: false,
    };
    let mut c = Client::new(vec![share("outer", &d, false), inner], "", &logs);
    let via_outer = c.walk(2, "folders/outer/sub/f.txt");
    via_outer.ok(P9_RWALK);
    let via_inner = c.walk(3, "folders/inner/f.txt");
    via_inner.ok(P9_RWALK);
    let other = c.walk(4, "folders/outer/g.txt");
    other.ok(P9_RWALK);
    let (a, b, g) = (
        via_outer.qid(2 + 3 * P9_QID_LEN).path,
        via_inner.qid(2 + 2 * P9_QID_LEN).path,
        other.qid(2 + 2 * P9_QID_LEN).path,
    );
    assert_eq!(a, b, "one host file, one qid path, through either share");
    assert_ne!(a, g, "two host files, two qid paths");
    let dir_outer = c.walk(5, "folders/outer/sub");
    dir_outer.ok(P9_RWALK);
    let dir_inner = c.walk(6, "folders/inner");
    dir_inner.ok(P9_RWALK);
    assert_eq!(
        dir_outer.qid(2 + 2 * P9_QID_LEN).path,
        dir_inner.qid(2 + P9_QID_LEN).path,
        "the inner share's root is the outer share's sub directory"
    );
}

#[test]
fn snarf_reads_the_pasteboard_and_a_clunked_write_sets_it() {
    let logs = Dir::new("snarf-log");
    let mut c = Client::new(vec![], "hello from the mac", &logs);
    c.walk(2, "snarf").ok(P9_RWALK);
    assert_eq!(c.attr(2), (S_IFREG | 0o600, ME, ME, 0), "size 0, mode 0600");
    assert_eq!(c.open(2, 2).errno(), Some(E_ACCES), "O_RDWR");
    c.open(2, 0).ok(P9_RLOPEN);
    assert_eq!(c.read(2, 0, 5), b"hello");
    // Later offsets continue the snapshot, whatever the pasteboard does meanwhile.
    c.pb.set_text("changed");
    assert_eq!(c.read(2, 5, 100), b" from the mac");
    c.clunk(2);

    c.walk(3, "snarf").ok(P9_RWALK);
    c.open(3, 1 | 0o1000).ok(P9_RLOPEN);
    c.call(
        P9_TWRITE,
        B::default()
            .u32(3)
            .u64(0)
            .u32(4)
            .bytes(&[b'f', 0xff, b'o', b'o']),
    )
    .ok(P9_RWRITE);
    assert_eq!(
        String::from_utf8(c.pb.text()).unwrap(),
        "changed",
        "set only at the clunk"
    );
    c.clunk(3);
    assert_eq!(
        String::from_utf8(c.pb.text()).unwrap(),
        "f\u{fffd}oo",
        "invalid UTF-8 becomes U+FFFD"
    );
    assert!(
        c.logged()
            .contains("chan 7 snarf set 6 bytes 'f\u{fffd}oo'"),
        "{}",
        c.logged()
    );

    // A write past 16 MiB answers EFBIG.
    c.walk(4, "snarf").ok(P9_RWALK);
    c.open(4, 1).ok(P9_RLOPEN);
    let chunk = vec![b'x'; 60000];
    let mut off = 0u64;
    let errno = loop {
        let r = c.call(
            P9_TWRITE,
            B::default()
                .u32(4)
                .u64(off)
                .u32(chunk.len() as u32)
                .bytes(&chunk),
        );
        if r.t != P9_RWRITE {
            break r.errno();
        }
        off += chunk.len() as u64;
    };
    assert_eq!(errno, Some(E_FBIG));
    assert!(off as usize <= SNARF_MAX && off as usize + chunk.len() > SNARF_MAX);
}

impl B {
    fn bytes(mut self, v: &[u8]) -> B {
        self.0.extend_from_slice(v);
        self
    }
}

#[test]
fn a_folder_is_read_and_written_as_the_mac_user_and_reports_the_attacher() {
    let logs = Dir::new("rw-log");
    let d = Dir::new("rw");
    fs::write(d.0.join("a.txt"), b"alpha").unwrap();
    fs::set_permissions(d.0.join("a.txt"), fs::Permissions::from_mode(0o654)).unwrap();
    let mut c = Client::new(vec![share("work", &d, false)], "", &logs);
    c.walk(2, "folders").ok(P9_RWALK);
    c.open(2, 0).ok(P9_RLOPEN);
    assert_eq!(c.names(2, 8192), ["work"]);

    c.walk(3, "folders/work/a.txt").ok(P9_RWALK);
    let (mode, uid, gid, size) = c.attr(3);
    assert_eq!(
        (mode, uid, gid, size),
        (S_IFREG | 0o600, ME, ME, 5),
        "owner bits rw-, group and other none"
    );
    c.open(3, 0).ok(P9_RLOPEN);
    assert_eq!(c.read(3, 1, 100), b"lpha");

    // Create, write, read back on the Mac.
    c.walk(4, "folders/work").ok(P9_RWALK);
    let r = c.call(
        P9_TLCREATE,
        B::default().u32(4).s(b"new").u32(1).u32(0o644).u32(ME),
    );
    assert_eq!(r.ok(P9_RLCREATE).len(), 17);
    c.call(P9_TWRITE, B::default().u32(4).u64(0).u32(3).bytes(b"new"))
        .ok(P9_RWRITE);
    c.clunk(4);
    assert_eq!(fs::read(d.0.join("new")).unwrap(), b"new");
    c.walk(5, "folders/work").ok(P9_RWALK);
    assert_eq!(
        c.call(
            P9_TLCREATE,
            B::default().u32(5).s(b"new").u32(1).u32(0o644).u32(ME)
        )
        .errno(),
        Some(E_EXIST)
    );

    // mkdir, and an rmdir of a directory that is not empty: ENOTEMPTY in Linux's numbering.
    c.call(P9_TMKDIR, B::default().u32(5).s(b"sub").u32(0o755).u32(ME))
        .ok(P9_RMKDIR);
    fs::write(d.0.join("sub/inner"), b"").unwrap();
    assert_eq!(
        c.call(
            P9_TUNLINKAT,
            B::default().u32(5).s(b"sub").u32(P9_UNLINK_AT_REMOVEDIR)
        )
        .errno(),
        Some(E_NOTEMPTY)
    );
    assert_eq!(
        c.call(P9_TUNLINKAT, B::default().u32(5).s(b"sub").u32(0))
            .errno(),
        Some(E_ISDIR)
    );
    fs::remove_file(d.0.join("sub/inner")).unwrap();
    c.call(
        P9_TUNLINKAT,
        B::default().u32(5).s(b"sub").u32(P9_UNLINK_AT_REMOVEDIR),
    )
    .ok(P9_RUNLINKAT);
    assert!(!d.0.join("sub").exists());
}

#[test]
fn a_rename_moves_the_file_and_the_fids_below_it_follow() {
    let logs = Dir::new("mv-log");
    let d = Dir::new("mv");
    fs::create_dir_all(d.0.join("old/deep")).unwrap();
    fs::write(d.0.join("old/deep/f"), b"F").unwrap();
    let mut c = Client::new(vec![share("w", &d, false)], "", &logs);
    c.walk(2, "folders/w").ok(P9_RWALK);
    c.walk(3, "folders/w/old/deep/f").ok(P9_RWALK);
    c.call(P9_TRENAMEAT, B::default().u32(2).s(b"old").u32(2).s(b"new"))
        .ok(P9_RRENAMEAT);
    assert!(d.0.join("new/deep/f").exists());
    c.open(3, 0).ok(P9_RLOPEN);
    assert_eq!(
        c.read(3, 0, 10),
        b"F",
        "the fid named old/deep/f and now names new/deep/f"
    );
    // Trename: the fid's own file moves into another directory.
    c.walk(4, "folders/w/new/deep/f").ok(P9_RWALK);
    c.call(P9_TRENAME, B::default().u32(4).u32(2).s(b"top"))
        .ok(P9_RRENAME);
    assert_eq!(fs::read(d.0.join("top")).unwrap(), b"F");
    // A share's root is no file to move.
    assert_eq!(
        c.call(P9_TRENAME, B::default().u32(2).u32(2).s(b"x"))
            .errno(),
        Some(E_BUSY)
    );
}

#[test]
fn a_read_only_folder_answers_erofs_to_every_change() {
    let logs = Dir::new("ro-log");
    let d = Dir::new("ro");
    fs::write(d.0.join("f"), b"x").unwrap();
    fs::set_permissions(d.0.join("f"), fs::Permissions::from_mode(0o644)).unwrap();
    let mut c = Client::new(vec![share("r", &d, true)], "", &logs);
    c.walk(2, "folders/r/f").ok(P9_RWALK);
    assert_eq!(
        c.attr(2).0,
        S_IFREG | 0o400,
        "no write bit in a read-only folder"
    );
    assert_eq!(c.open(2, 1).errno(), Some(E_ROFS));
    assert_eq!(
        c.open(2, 0o1000).errno(),
        Some(E_ROFS),
        "O_TRUNC changes the file too"
    );
    let chmod = B::default()
        .u32(2)
        .u32(P9_SETATTR_MODE)
        .u32(0o600)
        .u32(0)
        .u32(0)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0);
    assert_eq!(c.call(P9_TSETATTR, chmod).errno(), Some(E_ROFS));
    c.walk(3, "folders/r").ok(P9_RWALK);
    assert_eq!(
        c.call(
            P9_TLCREATE,
            B::default().u32(3).s(b"n").u32(1).u32(0o644).u32(ME)
        )
        .errno(),
        Some(E_ROFS)
    );
    assert_eq!(
        c.call(P9_TMKDIR, B::default().u32(3).s(b"d").u32(0o755).u32(ME))
            .errno(),
        Some(E_ROFS)
    );
    assert_eq!(
        c.call(P9_TUNLINKAT, B::default().u32(3).s(b"f").u32(0))
            .errno(),
        Some(E_ROFS)
    );
    c.open(2, 0).ok(P9_RLOPEN);
    assert_eq!(
        c.read(2, 0, 10),
        b"x",
        "reading is what a read-only folder is for"
    );
}

#[test]
fn a_link_is_served_as_a_link_and_never_walked_through() {
    let logs = Dir::new("ln-log");
    let d = Dir::new("ln");
    let outside = Dir::new("ln-outside");
    fs::write(outside.0.join("secret"), b"s").unwrap();
    symlink(&outside.0, d.0.join("out")).unwrap();
    let mut c = Client::new(vec![share("l", &d, false)], "", &logs);
    let r = c.walk(2, "folders/l/out");
    assert_eq!(r.qid(2 + 2 * P9_QID_LEN).kind, P9_QTSYMLINK);
    let r = c.walk(3, "folders/l/out/secret");
    assert_eq!(
        u16::from_le_bytes(r.ok(P9_RWALK)[..2].try_into().unwrap()),
        3,
        "the walk stops at the link"
    );
    assert_eq!(
        c.open(3, 0).errno(),
        Some(E_BADF),
        "a partial walk makes no fid"
    );
    assert_eq!(c.open(2, 0).errno(), Some(E_LOOP));
    let r = c.call(P9_TREADLINK, B::default().u32(2));
    assert_eq!(
        unpack_str(r.ok(P9_RREADLINK), 0).unwrap().0,
        outside.0.as_os_str().as_encoded_bytes()
    );
    c.walk(4, "folders/l").ok(P9_RWALK);
    c.call(
        P9_TSYMLINK,
        B::default().u32(4).s(b"made").s(b"../anywhere").u32(ME),
    )
    .ok(P9_RSYMLINK);
    assert_eq!(
        fs::read_link(d.0.join("made")).unwrap().to_str(),
        Some("../anywhere")
    );
}

#[test]
fn a_walk_from_a_directory_the_mac_replaced_with_a_link_is_refused_and_recorded() {
    let logs = Dir::new("swap-log");
    let d = Dir::new("swap");
    let outside = Dir::new("swap-outside");
    fs::create_dir(d.0.join("d")).unwrap();
    fs::write(outside.0.join("secret"), b"outside").unwrap();
    let mut c = Client::new(vec![share("l", &d, false)], "", &logs);
    c.walk(2, "folders/l/d").ok(P9_RWALK);
    fs::rename(d.0.join("d"), d.0.join("d-before")).unwrap();
    symlink(&outside.0, d.0.join("d")).unwrap();
    let r = c.call(P9_TWALK, B::default().u32(2).u32(3).u16(1).s(b"secret"));
    assert_eq!(
        r.errno(),
        Some(E_LOOP),
        "the held fid's walk meets the link"
    );
    let log = c.logged();
    assert!(
        log.contains("chan 7 walk '/' ['folders', 'l', 'd'] -> 3 of 3"),
        "{log}"
    );
    assert!(
        log.contains("chan 7 walk 'folders/l/d' ['secret'] -> errno 40"),
        "{log}"
    );
    c.walk(4, "folders/l/d-before").ok(P9_RWALK);
    c.call(P9_TWALK, B::default().u32(4).u32(5).u16(1).s(b"none"));
    c.call(P9_TWALK, B::default().u32(99).u32(6).u16(0));
    let log = c.logged();
    assert!(
        log.contains("chan 7 walk 'folders/l/d-before' ['none'] -> errno 2"),
        "{log}"
    );
    assert!(log.contains("chan 7 walk no fid [] -> errno 9"), "{log}");
}

#[test]
fn ownership_never_changes_and_a_device_is_never_made() {
    let logs = Dir::new("own-log");
    let d = Dir::new("own");
    fs::write(d.0.join("f"), b"").unwrap();
    let mut c = Client::new(vec![share("o", &d, false)], "", &logs);
    c.walk(2, "folders/o/f").ok(P9_RWALK);
    let chown = |uid: u32| {
        B::default()
            .u32(2)
            .u32(P9_SETATTR_UID)
            .u32(0)
            .u32(uid)
            .u32(0)
            .u64(0)
            .u64(0)
            .u64(0)
            .u64(0)
            .u64(0)
    };
    assert_eq!(c.call(P9_TSETATTR, chown(0)).errno(), Some(E_PERM));
    c.call(P9_TSETATTR, chown(ME)).ok(P9_RSETATTR);
    let trunc = B::default()
        .u32(2)
        .u32(P9_SETATTR_SIZE)
        .u32(0)
        .u32(0)
        .u32(0)
        .u64(3)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0);
    c.call(P9_TSETATTR, trunc).ok(P9_RSETATTR);
    assert_eq!(fs::metadata(d.0.join("f")).unwrap().len(), 3);
    c.walk(3, "folders/o").ok(P9_RWALK);
    let mknod = B::default()
        .u32(3)
        .s(b"fifo")
        .u32(0o10644)
        .u32(0)
        .u32(0)
        .u32(ME);
    assert_eq!(c.call(P9_TMKNOD, mknod).errno(), Some(E_PERM));
    assert_eq!(
        c.call(P9_TLINK, B::default().u32(3).u32(2).s(b"hard"))
            .errno(),
        Some(E_PERM)
    );
}

#[test]
fn an_operation_this_server_lacks_is_eopnotsupp_and_a_broken_stream_is_an_error() {
    let logs = Dir::new("ops-log");
    let mut c = Client::new(vec![], "", &logs);
    // Txattrwalk, and the kernel's fused walk (POUNCE): the kernel falls back on EOPNOTSUPP.
    assert_eq!(
        c.call(30, B::default().u32(1).u32(9).s(b"user.x")).errno(),
        Some(E_OPNOTSUPP)
    );
    assert_eq!(
        c.call(140, B::default().u32(1).u32(9).u64(0).u16(0))
            .errno(),
        Some(E_OPNOTSUPP)
    );
    assert!(c.logged().contains("chan 7 unanswered T140"));
    let mut out = Vec::new();
    assert!(
        c.s.feed(&[3, 0, 0, 0, 100, 0, 0], &mut out).is_err(),
        "a size below the header"
    );
}

#[test]
fn readdir_continues_from_its_cookie_a_few_entries_at_a_time() {
    let logs = Dir::new("rd-log");
    let d = Dir::new("rd");
    let want: Vec<String> = (0..40).map(|i| format!("entry-{i:02}")).collect();
    for n in &want {
        fs::write(d.0.join(n), b"").unwrap();
    }
    let mut c = Client::new(vec![share("r", &d, false)], "", &logs);
    c.walk(2, "folders/r").ok(P9_RWALK);
    c.open(2, 0).ok(P9_RLOPEN);
    let mut got = c.names(2, 100);
    got.sort();
    assert_eq!(got, want);
}

#[test]
fn a_lock_is_arbitrated_between_owners_and_a_clunk_lets_it_go() {
    let logs = Dir::new("lk-log");
    let d = Dir::new("lk");
    fs::write(d.0.join("db"), b"").unwrap();
    let mut c = Client::new(vec![share("k", &d, false)], "", &logs);
    c.walk(2, "folders/k/db").ok(P9_RWALK);
    c.open(2, 2).ok(P9_RLOPEN);
    c.walk(3, "folders/k/db").ok(P9_RWALK);
    c.open(3, 2).ok(P9_RLOPEN);
    let lock = |fid: u32, kind: u8, pid: u32| {
        B::default()
            .u32(fid)
            .u8(kind)
            .u32(0)
            .u64(0)
            .u64(0)
            .u32(pid)
            .s(b"thylacine")
    };
    assert_eq!(
        c.call(P9_TLOCK, lock(2, P9_LOCK_TYPE_WRLCK, 10))
            .ok(P9_RLOCK),
        [P9_LOCK_SUCCESS]
    );
    assert_eq!(
        c.call(P9_TLOCK, lock(3, P9_LOCK_TYPE_RDLCK, 11))
            .ok(P9_RLOCK),
        [P9_LOCK_BLOCKED]
    );
    let r = c.call(
        P9_TGETLOCK,
        B::default()
            .u32(3)
            .u8(P9_LOCK_TYPE_RDLCK)
            .u64(0)
            .u64(0)
            .u32(11)
            .s(b"thylacine"),
    );
    let b = r.ok(P9_RGETLOCK);
    assert_eq!(
        (b[0], u32::from_le_bytes(b[17..21].try_into().unwrap())),
        (P9_LOCK_TYPE_WRLCK, 10)
    );
    c.clunk(2);
    assert_eq!(
        c.call(P9_TLOCK, lock(3, P9_LOCK_TYPE_RDLCK, 11))
            .ok(P9_RLOCK),
        [P9_LOCK_SUCCESS]
    );
}
