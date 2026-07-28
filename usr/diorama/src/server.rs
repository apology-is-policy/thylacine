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
// The tree (V-4a Tier 1 + V-4b)
// ---------------------------------------------------------------------------
//
//   /                dir
//   /self            dir      the calling connection's own Proc
//   /self/exe        file     its executable's path   <- the V-4a gate
//   /self/cmdline    file     argv[0], NUL-terminated (Linux shape)
//   /self/status     file     Linux-shaped Name/Pid/Uid/Gid/Threads/VmRSS
//   /self/cwd        file     the working directory (V-4b-1)
//   /self/maps       file     the address space, Linux column layout (V-4b-2)
//   /self/environ    file     the environment, NUL-separated (V-4b-6; /self ONLY)
//   /meminfo         file     MemTotal/MemFree in kB
//   /uptime          file     "<up> <idle>" seconds
//
// Not served, each for its own recorded reason (VIVARIUM section 6.10):
//   /self/fd    BLOCKED on #66c -- a cross-Proc fd-list read of a live peer
//               races the #926 at-exit handle-table free. There is no other
//               native source, and inventing one is the section 6.7 failure.
//   /self/auxv  WEIGHED AND NOT BUILT (section 6.14): zero live readers, and a
//               viv-launched binary receives its auxv on the stack by
//               construction, since ld.so bootstraps out of AT_PHDR/AT_ENTRY.
//   /cpuinfo    Tier 1 by section 6.3, but only PARTLY sourced: ncpus comes from
//               /ctl/cpu, while MIDR (implementer/part/variant/revision) is not
//               EL0-readable at all -- an EL0 `mrs midr_el1` is snare:ill, which
//               is also why AT_HWCAP must never set hwcap_CPUID. V-4c.
//   /stat       Tier 1 by section 6.3, same shape: the cpu/cpuN idle columns come
//               from /ctl/cpu and btime from uptime + REALTIME, but ctxt, intr
//               and processes have no native source at all. V-4c.

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
// V-4b-2 raised this from 1024: /self/maps is the first render whose size scales
// with the Proc rather than being a handful of fixed lines, and the Linux row is
// LONGER than the native row it comes from (a pathname column the native table
// encodes as a devno:qid pair). The kernel's own side caps at DEVPROC_READ_BUF
// (2048) of native text, so 4096 leaves headroom for the expansion. Both layers
// truncate at a whole row, never mid-row.
const RENDER_MAX: usize = 4096;

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_MODE: u32 = S_IFREG | 0o444;
const P9_GETATTR_SIZE: u64 = 0x200;

// ---------------------------------------------------------------------------
// The node table. Static nodes (the fixed tree: /self, /meminfo, /sys/...) use
// the node index AS the qid path, so they can never dangle. V-4b-3 adds one
// dynamic family -- the numeric /proc/<pid> dirs -- which cannot be a static
// index because the pid set is a runtime fact.
// ---------------------------------------------------------------------------

const N_ROOT: u64 = 0;
const N_SELF: u64 = 1;
const N_SELF_EXE: u64 = 2;
const N_SELF_CMDLINE: u64 = 3;
const N_SELF_STATUS: u64 = 4;
const N_SELF_CWD: u64 = 5;
const N_SELF_MAPS: u64 = 6;
const N_SELF_ENVIRON: u64 = 7;
const N_MEMINFO: u64 = 8;
const N_UPTIME: u64 = 9;
const N_SYS: u64 = 10;
const N_SYS_KERNEL: u64 = 11;
const N_OSTYPE: u64 = 12;
const N_OSRELEASE: u64 = 13;
const N_VERSION: u64 = 14;
const N_HOSTNAME: u64 = 15;
const N_COUNT: u64 = 16;

struct Node {
    name: &'static [u8],
    parent: u64,
    is_dir: bool,
}

static NODES: [Node; N_COUNT as usize] = [
    Node { name: b"",          parent: N_ROOT,       is_dir: true  },
    Node { name: b"self",      parent: N_ROOT,       is_dir: true  },
    Node { name: b"exe",       parent: N_SELF,       is_dir: false },
    Node { name: b"cmdline",   parent: N_SELF,       is_dir: false },
    Node { name: b"status",    parent: N_SELF,       is_dir: false },
    Node { name: b"cwd",       parent: N_SELF,       is_dir: false },
    Node { name: b"maps",      parent: N_SELF,       is_dir: false },
    Node { name: b"environ",   parent: N_SELF,       is_dir: false },
    Node { name: b"meminfo",   parent: N_ROOT,       is_dir: false },
    Node { name: b"uptime",    parent: N_ROOT,       is_dir: false },
    Node { name: b"sys",       parent: N_ROOT,       is_dir: true  },
    Node { name: b"kernel",    parent: N_SYS,        is_dir: true  },
    Node { name: b"ostype",    parent: N_SYS_KERNEL, is_dir: false },
    Node { name: b"osrelease", parent: N_SYS_KERNEL, is_dir: false },
    Node { name: b"version",   parent: N_SYS_KERNEL, is_dir: false },
    Node { name: b"hostname",  parent: N_SYS_KERNEL, is_dir: false },
];

// --- the per-pid family (V-4b-3) -------------------------------------------
//
// A per-pid qid is (pid << 32) | kind -- the SAME SHAPE devproc uses for its own
// qids (kernel/devproc.c::proc_qid_make), so the two read alike side by side.
// It cannot collide with a static node index: pid 0 is never a live Proc
// (g_next_pid starts at 1), so every per-pid path is >= 1<<32 while every static
// index is < N_COUNT.
//
// The files are exactly /self's, because /self ALWAYS WAS a per-pid render with
// the pid supplied by the connection's peer rather than by the path. That is why
// this sub-chunk needed no kernel work: the pid was a parameter from the start.
//
// VISIBILITY -- and the V-7 obligation this creates. Serving OTHER Procs is the
// first time the diorama answers about anything but its caller, so state the
// boundary rather than leave it implied:
//
//   * The five files are 0444 with devproc.perm_enforced == false -- Plan 9's
//     all-pids-visible posture. Any Proc can read any Proc's status/exe/cwd/maps
//     natively. So the diorama serves EXACTLY what native /proc serves, to
//     exactly the same set of readers: no new authority, section 6.2 intact.
//   * What it does NOT do is scope the pid set to a container, because THERE IS
//     NO SUCH SCOPING NATIVELY YET -- /ctl/procs lists every Proc on the box.
//
// So when V-7 gives a contained Proc its own territory, "which pids can I see"
// becomes a real containment question, and it is a question about /proc and
// /ctl/procs FIRST -- the diorama merely inherits whatever they decide. Scoping
// it here alone would be theatre: a contained Proc that can reach native /proc
// would just read around us. The obligation is therefore recorded against V-7 in
// VIVARIUM.md section 7.1, not worked around here.
const PID_SHIFT: u32 = 32;
const PK_DIR: u32 = 0;
const PK_EXE: u32 = 1;
const PK_CMDLINE: u32 = 2;
const PK_STATUS: u32 = 3;
const PK_CWD: u32 = 4;
const PK_MAPS: u32 = 5;

struct PidFile {
    name: &'static [u8],
    kind: u32,
}

static PID_FILES: [PidFile; 5] = [
    PidFile { name: b"exe",     kind: PK_EXE     },
    PidFile { name: b"cmdline", kind: PK_CMDLINE },
    PidFile { name: b"status",  kind: PK_STATUS  },
    PidFile { name: b"cwd",     kind: PK_CWD     },
    PidFile { name: b"maps",    kind: PK_MAPS    },
];

fn pid_qid(pid: u32, kind: u32) -> u64 {
    ((pid as u64) << PID_SHIFT) | kind as u64
}
fn qid_pid(path: u64) -> u32 {
    (path >> PID_SHIFT) as u32
}
fn qid_kind(path: u64) -> u32 {
    path as u32
}
fn is_pid_node(path: u64) -> bool {
    path >= (1u64 << PID_SHIFT)
}

/// Parse a whole component as a decimal pid. Rejects empty, non-digit,
/// leading-zero and overflowing forms, so "01", "1x" and "99999999999" are all
/// misses. Leading zeros are refused because Linux refuses them (/proc/01 is
/// ENOENT there) and a compat layer that accepted them would give one Proc two
/// names in a namespace where consumers treat the name as the identity.
fn parse_pid(name: &[u8]) -> Option<u32> {
    if name.is_empty() || name.len() > 10 || name[0] == b'0' {
        return None; // a leading 0 also covers "0" itself: never a live Proc,
                     // and 0 is the static-node range
    }
    let mut v: u64 = 0;
    for &c in name {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (c - b'0') as u64;
        if v > u32::MAX as u64 {
            return None;
        }
    }
    Some(v as u32)
}

fn node_is_dir(path: u64) -> bool {
    if is_pid_node(path) {
        return qid_kind(path) == PK_DIR;
    }
    (path < N_COUNT) && NODES[path as usize].is_dir
}

/// Resolve one component under `dir`. `..` walks to the parent (the root's
/// parent is itself, per 9P).
///
/// A numeric component under the root names a LIVE Proc, and liveness is decided
/// by a native open of `/proc/<pid>` -- not by a table this server keeps. That
/// matters twice: it is the section 6.2 property (the answer comes from the
/// surface the caller could have walked itself), and it is what makes a dead or
/// never-existent pid an honest ENOENT rather than a directory of empty files,
/// which is how every Linux consumer detects that a process is gone.
fn walk_child(dir: u64, name: &[u8]) -> Option<u64> {
    if is_pid_node(dir) {
        if qid_kind(dir) != PK_DIR {
            return None; // walking from a file has no meaning
        }
        if name == b"." {
            return Some(dir);
        }
        if name == b".." {
            return Some(N_ROOT);
        }
        for f in PID_FILES.iter() {
            if f.name == name {
                return Some(pid_qid(qid_pid(dir), f.kind));
            }
        }
        return None;
    }
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
    if dir == N_ROOT {
        if let Some(pid) = parse_pid(name) {
            if native_pid_exists(pid) {
                return Some(pid_qid(pid, PK_DIR));
            }
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
    /// Lowercase hex, zero-padded to at least `min` digits and with no `0x`
    /// prefix -- Linux's /proc/*/maps column format (the kernel's own
    /// seq_put_hex_ll pads addresses to 8).
    fn push_hex(&mut self, v: u64, min: usize) {
        let mut tmp = [0u8; 16];
        let mut i = 0;
        let mut x = v;
        if x == 0 {
            tmp[i] = b'0';
            i += 1;
        }
        while x > 0 {
            let d = (x & 0xf) as u8;
            tmp[i] = if d < 10 { b'0' + d } else { b'a' + (d - 10) };
            x >>= 4;
            i += 1;
        }
        while i < min {
            tmp[i] = b'0';
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
    fn len(&self) -> usize {
        self.len
    }
    /// Discard everything after `mark`. Used to abandon a partially-built row
    /// once it turns out not to fit, so the render always ends at a row
    /// boundary (the kernel-side format_maps row-commit idiom, mirrored).
    fn truncate_to(&mut self, mark: usize) {
        if mark <= self.len {
            self.len = mark;
        }
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

/// Does `pid` name a live Proc? Decided by a native `O_PATH` open of
/// `/proc/<pid>`, which is devproc's own existence test (its walk_one calls
/// proc_find_by_pid and misses on a dead pid). O_PATH because this asks only
/// "does the name resolve" -- it opens nothing for reading.
fn native_pid_exists(pid: u32) -> bool {
    let mut r = Render::new();
    r.push(b"/proc/");
    r.push_dec(pid as u64);
    let p = r.bytes();
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, p.as_ptr(), p.len(), T_OPATH) };
    if fd < 0 {
        return false;
    }
    let _ = unsafe { t_close(fd) };
    true
}

/// The live pid list, from `/ctl/procs`' first column. Used only to ENUMERATE
/// the root (Linux's /proc lists its processes); resolution never consults it,
/// so a stale entry costs a readdir row, never a wrong answer.
///
/// COHERENCY: the list is re-read per Treaddir call, so a Proc that exits or
/// spawns mid-enumeration shifts the indices the cookie counts against and can
/// make a pid appear twice or not at all. Linux's own /proc readdir has exactly
/// this property (its cookie is a position in the tgid list), and no consumer
/// treats a single enumeration as a consistent snapshot.
fn native_pid_list(out: &mut [u32]) -> usize {
    let mut buf = [0u8; 2048]; // matches the kernel's DEVCTL_READ_BUF
    let got = match read_native(b"/ctl/procs", &mut buf) {
        Some(n) => n,
        None => return 0,
    };
    parse_pid_list(&buf[..got], out)
}

/// The pure half of native_pid_list: first column of every line that parses as
/// a pid. The header ("PID    PPID    ...") fails parse_pid and is skipped by
/// the same rule as any junk, so no separate header-stripping step can drift
/// out of sync with the kernel's format.
pub fn parse_pid_list(text: &[u8], out: &mut [u32]) -> usize {
    let mut n = 0usize;
    for line in text.split(|&c| c == b'\n') {
        if n >= out.len() {
            break;
        }
        let end = line.iter().position(|&c| c == b' ').unwrap_or(line.len());
        if let Some(pid) = parse_pid(&line[..end]) {
            out[n] = pid;
            n += 1;
        }
    }
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

/// Parse the decimal that follows `marker` ANYWHERE in `text`. The line-start
/// discipline of parse_kv_dec cannot reach a mid-line field, and the native
/// status packs two on one line ("principal:<N> gid:<M>"). Callers must pass a
/// marker specific enough not to match a prefix of another key -- " gid:" leads
/// with the separating space for exactly that reason.
pub fn parse_dec_after(text: &[u8], marker: &[u8]) -> Option<u64> {
    if marker.is_empty() || text.len() < marker.len() {
        return None;
    }
    for i in 0..=text.len() - marker.len() {
        if &text[i..i + marker.len()] != marker {
            continue;
        }
        let mut j = i + marker.len();
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
    None
}

// ---------------------------------------------------------------------------
// The renderers.
//
// Each takes a PID. For /self that pid comes from the connection's peer (the
// kernel-stamped srv_peer_info, never a client-supplied id); for /<pid> it comes
// from a path component that walk_child already proved names a live Proc. The
// renderers themselves cannot tell the two apart, and must not need to: their
// only source is /proc/<pid>/*, whose own gates decide what a caller may see.
// ---------------------------------------------------------------------------

/// exe -- the executable's path, bare (no NUL, no newline: Linux's
/// readlink("/proc/self/exe") yields a bare path). Empty when the kernel has no
/// recorded name, which is a real state (kproc, the blob-loaded init).
fn render_exe(pid: u32, r: &mut Render) {
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(pid, b"exe", &mut pbuf);
    let mut ebuf = [0u8; RENDER_MAX];
    if let Some(got) = read_native(&pbuf[..n], &mut ebuf) {
        r.push(&ebuf[..got]);
    }
}

/// cmdline -- Linux serves NUL-separated argv. Thylacine has no argv on a
/// running Proc (SYS_SPAWN's argv is consumed at exec and not retained), so this
/// serves argv[0] == the executable path, NUL-terminated, which is the universal
/// convention and is DERIVED from a native source rather than invented. A Proc
/// with no recorded exe renders empty, exactly as Linux does for a kernel thread.
fn render_cmdline(pid: u32, r: &mut Render) {
    render_exe(pid, r);
    if r.len > 0 {
        r.push(&[0u8]);
    }
}

/// status -- the Linux key/value shape, filled only from fields we really have.
/// Name comes from the exe's basename; Threads/VmRSS from the native
/// /proc/<pid>/status render.
///
/// `ids` is the kernel-stamped (principal, gid) when the caller has one -- which
/// only /self does, from the connection's srv_peer_info. It is passed rather
/// than always parsed because that channel is UNFORGEABLE and needs no parse,
/// and it is the V-4a-0b mechanism this server's identity story rests on;
/// dropping it for code tidiness would trade provenance for symmetry. A per-pid
/// read has no such channel and parses the same two values out of the native
/// render, which is the same kernel state (devproc's format_status prints
/// p->principal_id / p->primary_gid, the identical fields srv_peer_info stamps).
fn render_status(pid: u32, ids: Option<(u32, u32)>, r: &mut Render) {
    // Name: basename of the exe.
    let mut ebuf = [0u8; RENDER_MAX];
    let mut elen = 0usize;
    {
        let mut pbuf = [0u8; 64];
        let n = native_proc_path(pid, b"exe", &mut pbuf);
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
    r.push_dec(pid as u64);
    r.push(b"\n");

    // Threads + VmRSS + (for a per-pid read) the ids, from the native render.
    let mut sbuf = [0u8; RENDER_MAX];
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(pid, b"status", &mut pbuf);
    let got = read_native(&pbuf[..n], &mut sbuf).unwrap_or(0);
    let text = &sbuf[..got];

    // Linux prints four ids (real/effective/saved/fs); Thylacine has one
    // principal, so all four columns are the same value -- honest, and it keeps
    // a Linux parser that splits on whitespace working. A pid whose ids cannot
    // be read renders no Uid/Gid line at all, rather than a fabricated 0 (which
    // would read as root to every Linux consumer).
    let uid_gid = match ids {
        Some(v) => Some(v),
        None => match (
            parse_kv_dec(text, b"principal"),
            parse_dec_after(text, b" gid:"),
        ) {
            (Some(u), Some(g)) => Some((u as u32, g as u32)),
            _ => None,
        },
    };
    if let Some((uid, gid)) = uid_gid {
        r.push(b"Uid:\t");
        for k in 0..4 {
            if k > 0 {
                r.push(b"\t");
            }
            r.push_dec(uid as u64);
        }
        r.push(b"\n");
        r.push(b"Gid:\t");
        for k in 0..4 {
            if k > 0 {
                r.push(b"\t");
            }
            r.push_dec(gid as u64);
        }
        r.push(b"\n");
    }

    {
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

/// cwd -- the Proc's current working directory, bare (no NUL, no newline), from
/// the kernel's /proc/<pid>/cwd (V-4b-1). Never empty for a live Proc: an
/// un-chdir'd one reads "/".
fn render_cwd(pid: u32, r: &mut Render) {
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(pid, b"cwd", &mut pbuf);
    let mut cbuf = [0u8; RENDER_MAX];
    if let Some(got) = read_native(&pbuf[..n], &mut cbuf) {
        r.push(&cbuf[..got]);
    }
}

// ---------------------------------------------------------------------------
// /self/maps (V-4b-2). The kernel's /proc/<pid>/maps is a Thylacine-native
// table; Linux's shape is this server's job, which is the whole VIVARIUM split
// -- the kernel stays Thylacine, the phenotype lives out here.
//
// native:  0x10000000-0x10001000 rw-p 0x0 anon - -
// Linux:   10000000-10001000 rw-p 00000000 00:00 0
//
// Six fixed columns in, six out. The interesting translations:
//
//   dev    Thylacine's devno is a FLAT namespace with no major/minor split, so
//          it renders as minor under major 00. That is not a fabrication: Linux
//          itself uses 00:xx for every filesystem with no backing block device
//          (tmpfs, and 9P mounts specifically), which is exactly what a Stratum
//          mount is. An anonymous mapping is 00:00 with inode 0, as on Linux.
//   path   a FILE-backed mapping renders the executable's path. PREMISE: at
//          v1.0 the only FILE Burrows in an address space are the exec'd
//          binary's segments -- burrow_create_file has exactly one caller
//          (image_lookup_or_create, from exec), and there is no file-mmap
//          syscall. The premise is stated rather than assumed away: when a
//          file-mmap surface lands, the KERNEL line must start carrying a path
//          and this branch must read it instead of substituting exe.
//   [stack]/[vdso]  from the native role column, so the layout constants stay
//          in the kernel and no consumer hardcodes them.
//   guard  a prot-0 VMA renders ---p with no pathname -- byte-for-byte how
//          Linux shows a PROT_NONE guard page. Emitted, never hidden: it is
//          real reserved address space, and dropping it would make the map
//          claim the range is free.
// ---------------------------------------------------------------------------

/// Substring search over bytes -- selftest-only, so a render can be checked for
/// a fragment without pinning the whole (padded) row.
fn contains_bytes(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || needle.len() > hay.len() {
        return needle.is_empty();
    }
    (0..=hay.len() - needle.len()).any(|i| &hay[i..i + needle.len()] == needle)
}

/// Parse a `0x`-prefixed (or bare) lowercase hex integer. None on any stray
/// byte -- a malformed native line is skipped, never half-rendered.
fn parse_hex(s: &[u8]) -> Option<u64> {
    let body = if s.len() > 2 && s[0] == b'0' && s[1] == b'x' { &s[2..] } else { s };
    if body.is_empty() || body.len() > 16 {
        return None;
    }
    let mut v: u64 = 0;
    for &c in body {
        let d = match c {
            b'0'..=b'9' => c - b'0',
            b'a'..=b'f' => c - b'a' + 10,
            _ => return None,
        };
        v = (v << 4) | d as u64;
    }
    Some(v)
}

/// Split `line` on single spaces into at most `out.len()` fields. Returns the
/// field count. Empty fields are kept, so a shape mismatch shows up as a count
/// mismatch rather than a silent shift.
fn split_fields<'a>(line: &'a [u8], out: &mut [&'a [u8]]) -> usize {
    let mut n = 0;
    let mut start = 0usize;
    let mut i = 0usize;
    while i <= line.len() && n < out.len() {
        if i == line.len() || line[i] == b' ' {
            out[n] = &line[start..i];
            n += 1;
            start = i + 1;
        }
        i += 1;
    }
    n
}

/// Render one native maps row into Linux's shape. Returns false if the line is
/// malformed (wrong field count, unparsable number) -- the caller skips it.
fn maps_row(fields: &[&[u8]], exe: &[u8], r: &mut Render) -> bool {
    // fields: start-end perms off type file role
    let range = fields[0];
    let dash = match range.iter().position(|&c| c == b'-') {
        Some(i) => i,
        None => return false,
    };
    let start = match parse_hex(&range[..dash]) { Some(v) => v, None => return false };
    let end = match parse_hex(&range[dash + 1..]) { Some(v) => v, None => return false };
    let perms = fields[1];
    if perms.len() != 4 {
        return false;
    }
    let off = match parse_hex(fields[2]) { Some(v) => v, None => return false };
    let typ = fields[3];
    let file = fields[4];
    let role = fields[5];

    // The file column is "-" or "<devno>:<qid>", both 0x-prefixed hex.
    let (devno, inode) = if file == b"-" {
        (0u64, 0u64)
    } else {
        match file.iter().position(|&c| c == b':') {
            Some(i) => match (parse_hex(&file[..i]), parse_hex(&file[i + 1..])) {
                (Some(d), Some(q)) => (d, q),
                _ => return false,
            },
            None => return false,
        }
    };

    r.push_hex(start, 8);
    r.push(b"-");
    r.push_hex(end, 8);
    r.push(b" ");
    r.push(perms);
    r.push(b" ");
    r.push_hex(off, 8);
    r.push(b" ");
    // <major>:<minor> identifies the DEVICE and is independent of the inode.
    // Thylacine's devno is flat, so it is the minor under a 00 major -- the way
    // Linux renders every filesystem with no backing block device. Folding any
    // part of the inode in here would make two files on the SAME filesystem
    // report different devices, breaking the st_dev comparison this column
    // exists for.
    r.push(b"00:");
    r.push_hex(devno, 2);
    r.push(b" ");
    r.push_dec(inode);

    // The pathname column is optional on Linux and gets padded when present.
    if role == b"stack" {
        r.push(b"                    [stack]");
    } else if role == b"vdso" {
        r.push(b"                    [vdso]");
    } else if typ == b"file" && !exe.is_empty() {
        r.push(b"                    ");
        r.push(exe);
    }
    r.push(b"\n");
    true
}

/// environ -- the environment as Linux serves it: NUL-terminated "NAME=VALUE"
/// records, back to back, from the kernel's /proc/<pid>/environ (V-4b-6).
///
/// A PASSTHROUGH, uniquely among these renderers: the kernel source is already
/// in Linux's exact shape, because Thylacine's own /env has no flat form to be
/// in a different shape FROM -- the block is synthesized for this purpose. So
/// there is no translation to get wrong, and adding one would only add a place
/// to lose bytes.
///
/// GATED at the source, and that is why this file exists under `/self` ONLY --
/// the one asymmetry in this tree, and the reason for it is the whole of
/// section 6.2.
///
/// /proc/<pid>/environ is owner-or-CAP_HOSTOWNER (unlike the 0444 siblings)
/// because nothing else discloses another Proc's environment and environment
/// variables carry secrets by convention. That gate keys on the READER -- which
/// is THIS SERVER, not its client. So:
///
///   * `/self/environ` is sound. The target is the peer's own pid, so a read
///     that the kernel allows is a read of the CLIENT's own environment, which
///     the client could have done itself. A read the kernel denies (a user-
///     principal peer, since the shared boot diorama runs as SYSTEM) renders
///     empty. Either way the client gains nothing it did not have.
///   * `/<pid>/environ` would NOT be sound, and is therefore absent. This server
///     is SYSTEM, so the kernel would ALLOW it to read any SYSTEM Proc's
///     environment -- and it would then hand those bytes to a client of any
///     principal, who natively would have been denied. That is precisely the
///     deputy-as-authority failure section 6.2 forbids, and unlike its siblings
///     (all 0444, all readable by anyone natively) environ is the first proxied
///     file where the client's authority and this server's differ.
///
/// A walk to `/<pid>/environ` is therefore an honest ENOENT. Two things would
/// make it servable, and neither is a change here: a per-container diorama
/// running as ITS container's principal (V-7), where server and client authority
/// coincide by construction; or MANDATE (I-35), which would let a deputy act
/// with its client's authority rather than its own. Replicating the kernel's
/// owner check against `peer.principal_id` was considered and rejected -- it
/// would work, but it makes a component whose entire design property is having
/// no policy into a policy point, to serve a file no v1.0 consumer reads.
///
/// Truncation is to a WHOLE RECORD. RENDER_MAX bounds what this server serves,
/// and a block cut mid-record would hand the consumer a truncated value that
/// parses as a complete one -- so trim back to the last terminator. Unconditional
/// because it is a no-op on an untruncated block (which already ends in a NUL).
fn render_environ(pid: u32, r: &mut Render) {
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(pid, b"environ", &mut pbuf);
    let mut ebuf = [0u8; RENDER_MAX];
    // A DENIAL arrives as Some(0), not None: the open succeeds (devproc gates at
    // the read, not the open) and the read returns -1, which read_native reports
    // as zero bytes. None means the open itself failed -- a gone pid. Both render
    // empty, which is what makes the deny path indistinguishable from an empty
    // environment, exactly as it should be.
    let got = match read_native(&pbuf[..n], &mut ebuf) {
        Some(g) => g,
        None => return,
    };
    r.push(&ebuf[..trim_to_last_record(&ebuf[..got])]);
}

/// Length of the longest prefix of `block` that ends on a record boundary -- i.e.
/// everything up to and including the last NUL. 0 when there is none, which means
/// a single record longer than the whole buffer: serve nothing rather than a
/// headless fragment that would parse as a complete NAME=VALUE.
///
/// Its own function so the selftest can drive it with a synthetic block; the live
/// path cannot produce a truncated one on demand (it would need a >4 KiB
/// environment on the reading Proc). The maps_row precedent.
pub fn trim_to_last_record(block: &[u8]) -> usize {
    for i in (0..block.len()).rev() {
        if block[i] == 0 {
            return i + 1;
        }
    }
    0
}

/// maps -- the Proc's address space in Linux's /proc/*/maps shape.
fn render_maps(pid: u32, r: &mut Render) {
    let mut pbuf = [0u8; 64];

    // The executable path, for FILE-backed rows. Absent is fine (a blob-loaded
    // Proc has no recorded exe) -- those rows then carry no pathname, which is
    // exactly how Linux shows a mapping it cannot name.
    let mut ebuf = [0u8; 256];
    let n = native_proc_path(pid, b"exe", &mut pbuf);
    let elen = read_native(&pbuf[..n], &mut ebuf).unwrap_or(0);
    let exe = &ebuf[..elen];

    let mut mbuf = [0u8; 2048]; // matches the kernel's DEVPROC_READ_BUF
    let n = native_proc_path(pid, b"maps", &mut pbuf);
    let mlen = match read_native(&pbuf[..n], &mut mbuf) {
        Some(v) => v,
        None => return,
    };

    let mut first = true;
    for line in mbuf[..mlen].split(|&c| c == b'\n') {
        if line.is_empty() {
            continue;
        }
        if first {
            first = false; // the native header names the columns; Linux has none
            continue;
        }
        let mut fields: [&[u8]; 6] = [b""; 6];
        if split_fields(line, &mut fields) != 6 {
            continue;
        }
        // Commit whole rows only: if this one does not fit, drop it and stop,
        // so the render always ends at a row boundary.
        let mark = r.len();
        if !maps_row(&fields, exe, r) {
            r.truncate_to(mark);
            continue;
        }
        if r.len() == RENDER_MAX {
            r.truncate_to(mark);
            break;
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

// ---------------------------------------------------------------------------
// /sys/kernel -- the phenotype's self-description (V-4b-3).
//
// These are the FIRST diorama files that do not reformat a native source, and
// the distinction is worth stating precisely rather than treating as an
// exception. Section 6.2's rule exists to stop the diorama becoming an
// AUTHORITY -- serving something the native surface would have refused. A
// constant carries no information about the system at all, so it cannot leak
// anything; what it describes is the phenotype itself, which is this server's
// own property and nobody else's state.
//
// The discriminator to hold to, for every file added after this one: a value
// DERIVED FROM KERNEL STATE needs a native source, no exceptions. A constant
// declaring which ABI the caller is looking at is the phenotype speaking about
// itself. If a file cannot be argued into the second category in one sentence,
// it belongs in the first and needs a native source.
const OSTYPE: &[u8] = b"Linux\n";

// osrelease is the one constant with teeth: glibc-linked programs parse it and
// some refuse to start on a kernel below their minimum (3.2 for modern glibc).
// Declaring 6.1 clears every such check. The "-thylacine" suffix is the honesty
// -- Linux's own convention carries local suffixes (-generic, -arch1-1), so a
// parser that copes with real distro kernels copes with this, while anything
// that prints the string tells the truth about what it is running on.
//
// STATED TRADEOFF: a program COULD version-gate a feature on this number and
// take a path we do not implement. The alternative -- declaring a low version --
// makes those same programs refuse to run at all, which is strictly worse, and
// runtime feature probing (the overwhelmingly common pattern, and the one Linux
// itself pushes people to) degrades gracefully where version-gating does not.
const OSRELEASE: &[u8] = b"6.1.0-thylacine\n";
const KVERSION: &[u8] = b"#1 SMP Thylacine\n";

// hostname is NOT a constant of the same kind -- it would be system state if
// Thylacine had any. It does not (there is no hostname surface; see
// usr/coreutils/src/bin/uname.rs, which hardcodes the same answer for the same
// reason), so the honest render is the one the native tool already gives. That
// it is ALSO byte-identical to real Linux with no hostname set -- the kernel's
// init_uts_ns.name.nodename is literally "(none)" -- is a happy accident, not
// the justification. If a hostname surface ever lands, this reads from it.
const HOSTNAME: &[u8] = b"(none)\n";

/// Render `node` for `peer`. Directories render empty (they are read via
/// Treaddir, not Tread).
pub fn render(node: u64, peer: &TSrvPeerInfo) -> Render {
    let mut r = Render::new();
    if is_pid_node(node) {
        // A per-pid read. walk_child proved the Proc live when the path was
        // resolved; it can have exited since, in which case the native reads
        // fail and every file renders empty -- the same outcome Linux gives for
        // a pid that dies with a fid open, and never a stale answer.
        let pid = qid_pid(node);
        match qid_kind(node) {
            PK_EXE => render_exe(pid, &mut r),
            PK_CMDLINE => render_cmdline(pid, &mut r),
            PK_STATUS => render_status(pid, None, &mut r),
            PK_CWD => render_cwd(pid, &mut r),
            PK_MAPS => render_maps(pid, &mut r),
            _ => {}
        }
        return r;
    }
    // /self/*. The peer's liveness is checked HERE rather than inside each
    // renderer: a dead peer must render empty rather than read /proc/<pid>/*
    // for a pid that may since have been reused.
    let alive = peer.alive != 0;
    match node {
        N_SELF_EXE if alive => render_exe(peer.pid, &mut r),
        N_SELF_CMDLINE if alive => render_cmdline(peer.pid, &mut r),
        N_SELF_STATUS if alive => render_status(
            peer.pid,
            Some((peer.principal_id, peer.primary_gid)),
            &mut r,
        ),
        N_SELF_CWD if alive => render_cwd(peer.pid, &mut r),
        N_SELF_MAPS if alive => render_maps(peer.pid, &mut r),
        N_SELF_ENVIRON if alive => render_environ(peer.pid, &mut r),
        N_MEMINFO => render_meminfo(&mut r),
        N_UPTIME => render_uptime(&mut r),
        N_OSTYPE => r.push(OSTYPE),
        N_OSRELEASE => r.push(OSRELEASE),
        N_VERSION => r.push(KVERSION),
        N_HOSTNAME => r.push(HOSTNAME),
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

        // A /<pid>/ directory: a fixed five-entry list, cookie = index + 1.
        if is_pid_node(f.node) {
            let pid = qid_pid(f.node);
            let mut i = a.offset as usize;
            while i < PID_FILES.len() {
                let node = pid_qid(pid, PID_FILES[i].kind);
                if !self.emit_dirent(&mut data, budget, node, (i + 1) as u64, PID_FILES[i].name) {
                    break;
                }
                i += 1;
            }
            return p9::build_rreaddir(&mut self.out_buf, tag, &data);
        }

        // A static directory. The cookie is the NEXT child index to emit, so it
        // is strictly increasing and never 0 for a non-first entry -- the
        // devproc/netd readdir discipline.
        let mut child = a.offset;
        while child < N_COUNT {
            let next = child + 1; // the cookie to report for the FOLLOWING call
            if child == N_ROOT || NODES[child as usize].parent != f.node {
                child = next;
                continue;
            }
            if !self.emit_dirent(
                &mut data,
                budget,
                child,
                next,
                NODES[child as usize].name,
            ) {
                break; // did not fit; `child` stays here so the client re-asks
            }
            child = next;
        }
        // The root continues into the live pids once its static children are
        // done, at cookies >= N_COUNT -- so a `ps` that readdirs /proc sees the
        // numeric dirs Linux puts there. Reached only when the static phase ran
        // to completion, so a budget-truncated call resumes in the right phase.
        if f.node == N_ROOT && child >= N_COUNT {
            let mut pids = [0u32; 64];
            let n = native_pid_list(&mut pids);
            let mut i = (child - N_COUNT) as usize;
            while i < n {
                let mut nm = Render::new();
                nm.push_dec(pids[i] as u64);
                let node = pid_qid(pids[i], PK_DIR);
                if !self.emit_dirent(
                    &mut data,
                    budget,
                    node,
                    N_COUNT + i as u64 + 1,
                    nm.bytes(),
                ) {
                    break;
                }
                i += 1;
            }
        }
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    /// Pack one dirent if it fits the budget. Returns false when it did not fit
    /// (or would not pack), leaving `data` untouched so the caller can stop with
    /// its cursor still pointing at the unemitted entry.
    ///
    /// The scratch must be able to hold ANY name this server emits, because a
    /// pack failure returns false WITHOUT advancing the cursor -- so a name that
    /// can never fit would make a client re-ask for the same entry forever. A
    /// dirent is 24 bytes of header plus the name; the longest names here are a
    /// 10-digit pid and "osrelease" (9), so 64 is roughly double what is
    /// reachable. KEEP THAT TRUE when adding a node: a long name belongs with a
    /// bigger scratch, not with a silent truncation.
    fn emit_dirent(
        &self,
        data: &mut Vec<u8>,
        budget: usize,
        node: u64,
        cookie: u64,
        name: &[u8],
    ) -> bool {
        let dt = if node_is_dir(node) { p9::DT_DIR } else { p9::DT_REG };
        let mut scratch = [0u8; 64];
        let used = match p9::pack_dirent(&mut scratch, 0, &self.qid_of(node), cookie, dt, name) {
            Ok(u) => u,
            Err(_) => return false,
        };
        if data.len() + used > budget {
            return false;
        }
        data.extend_from_slice(&scratch[..used]);
        true
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
    if walk_child(N_SELF, b"cwd") != Some(N_SELF_CWD) {
        return Err("walk /self/cwd");
    }
    if walk_child(N_SELF, b"maps") != Some(N_SELF_MAPS) {
        return Err("walk /self/maps");
    }
    if walk_child(N_SELF, b"environ") != Some(N_SELF_ENVIRON) {
        return Err("walk /self/environ");
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

    // --- V-4b-3: /sys/kernel, the phenotype's self-description
    if walk_child(N_ROOT, b"sys") != Some(N_SYS) {
        return Err("walk /sys");
    }
    if walk_child(N_SYS, b"kernel") != Some(N_SYS_KERNEL) {
        return Err("walk /sys/kernel");
    }
    if walk_child(N_SYS_KERNEL, b"ostype") != Some(N_OSTYPE) {
        return Err("walk /sys/kernel/ostype");
    }
    if walk_child(N_SYS_KERNEL, b"..") != Some(N_SYS) {
        return Err("walk /sys/kernel/..");
    }
    // These render the SAME for any peer, dead or alive -- they describe the
    // phenotype, not the Proc, so a peer-dependent answer would be a bug.
    let dead = TSrvPeerInfo::default(); // alive == 0
    if render(N_OSTYPE, &dead).bytes() != b"Linux\n" {
        return Err("ostype");
    }
    if render(N_HOSTNAME, &dead).bytes() != b"(none)\n" {
        return Err("hostname");
    }
    // osrelease must parse as Linux <major>.<minor>.<patch> with a major high
    // enough to clear glibc's minimum-kernel refusal.
    {
        let rel = render(N_OSRELEASE, &dead);
        let b = rel.bytes();
        if b.len() < 6 || !b[0].is_ascii_digit() || b[1] != b'.' {
            return Err("osrelease shape");
        }
        if b[0] - b'0' < 4 {
            return Err("osrelease major too low for glibc");
        }
    }

    // --- V-4b-3: per-pid qids. The encoding must round-trip and must never
    //     collide with a static node index, which is what keeps a numeric path
    //     from resolving onto /self or /meminfo.
    if qid_pid(pid_qid(1234, PK_MAPS)) != 1234 || qid_kind(pid_qid(1234, PK_MAPS)) != PK_MAPS {
        return Err("pid qid round-trip");
    }
    if !is_pid_node(pid_qid(1, PK_DIR)) {
        return Err("pid qid not recognized");
    }
    for i in 0..N_COUNT {
        if is_pid_node(i) {
            return Err("a static node looked like a pid node");
        }
    }
    if !node_is_dir(pid_qid(7, PK_DIR)) || node_is_dir(pid_qid(7, PK_EXE)) {
        return Err("pid node dir-ness");
    }
    // A pid dir carries exactly /self's file set, and climbs to the root.
    if walk_child(pid_qid(7, PK_DIR), b"maps") != Some(pid_qid(7, PK_MAPS)) {
        return Err("walk /<pid>/maps");
    }
    // environ is deliberately absent under /<pid> -- see render_environ. A miss
    // here is the property; resolving it would mean this server had started
    // laundering its own read authority to clients of another principal.
    if walk_child(pid_qid(7, PK_DIR), b"environ").is_some() {
        return Err("/<pid>/environ resolved -- the cross-principal leak is back");
    }
    if walk_child(pid_qid(7, PK_DIR), b"..") != Some(N_ROOT) {
        return Err("walk /<pid>/..");
    }
    if walk_child(pid_qid(7, PK_DIR), b"nope").is_some() {
        return Err("resolved a nonexistent per-pid file");
    }
    if walk_child(pid_qid(7, PK_MAPS), b"anything").is_some() {
        return Err("walked into a per-pid file");
    }

    // --- V-4b-3: parse_pid is the resolution gate. Everything it rejects is a
    //     name that never reaches the native existence check, so its rejections
    //     are load-bearing, not cosmetic.
    if parse_pid(b"1") != Some(1) || parse_pid(b"4294967295") != Some(4294967295) {
        return Err("parse_pid decimal");
    }
    if parse_pid(b"0").is_some() {
        return Err("parse_pid accepted 0 (the static range)");
    }
    if parse_pid(b"").is_some()
        || parse_pid(b"1x").is_some()
        || parse_pid(b"x1").is_some()
        || parse_pid(b" 1").is_some()
        || parse_pid(b"4294967296").is_some()
        || parse_pid(b"99999999999").is_some()
    {
        return Err("parse_pid accepted garbage");
    }
    // Leading zeros are Linux-ENOENT, and accepting them would give one Proc
    // two names in a namespace whose consumers treat the name as the identity.
    if parse_pid(b"01").is_some() || parse_pid(b"007").is_some() {
        return Err("parse_pid accepted a leading zero");
    }
    // A pid that cannot exist must MISS, so /proc/<gone> is ENOENT rather than a
    // directory of empty files -- which is how a Linux consumer detects death.
    if walk_child(N_ROOT, b"4294967295").is_some() {
        return Err("resolved a nonexistent pid");
    }

    // --- V-4b-3: the /ctl/procs pid-list parse, incl. the header skip.
    {
        let procs = b"PID    PPID    NAME    STATE    THREADS\n\
                      1    0    joey    ALIVE    1\n\
                      42    1    ptyfs    ALIVE    2\n";
        let mut pids = [0u32; 8];
        let n = parse_pid_list(procs, &mut pids);
        if n != 2 || pids[0] != 1 || pids[1] != 42 {
            return Err("pid list parse");
        }
        // The bound is honored rather than overrunning the caller's array.
        let mut one = [0u32; 1];
        if parse_pid_list(procs, &mut one) != 1 {
            return Err("pid list bound");
        }
    }

    // --- V-4b-3: the mid-line decimal parse (native status packs
    //     "principal:<N> gid:<M>" on one line, which parse_kv_dec cannot reach).
    {
        let st = b"pid:     42\nprincipal:1000 gid:100\nthreads: 2\n";
        if parse_kv_dec(st, b"principal") != Some(1000) {
            return Err("principal parse");
        }
        if parse_dec_after(st, b" gid:") != Some(100) {
            return Err("gid parse");
        }
        if parse_dec_after(st, b" absent:").is_some() {
            return Err("parse_dec_after invented a value");
        }
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
    if !render(N_SELF_EXE, &dead).bytes().is_empty() {
        return Err("dead peer served an exe");
    }
    if !render(N_SELF_STATUS, &dead).bytes().is_empty() {
        return Err("dead peer served a status");
    }
    if !render(N_SELF_CMDLINE, &dead).bytes().is_empty() {
        return Err("dead peer served a cmdline");
    }
    if !render(N_SELF_CWD, &dead).bytes().is_empty() {
        return Err("dead peer served a cwd");
    }
    if !render(N_SELF_MAPS, &dead).bytes().is_empty() {
        return Err("dead peer served a maps");
    }
    if !render(N_SELF_ENVIRON, &dead).bytes().is_empty() {
        return Err("dead peer served an environ");
    }

    // --- V-4b-6: the environ whole-record trim, driven directly. The live path
    // cannot produce a truncated block on demand (it would want a >4 KiB
    // environment on this very Proc), and the property is the one that keeps a
    // truncated read from parsing as a complete variable.
    if trim_to_last_record(b"A=1\0BB=22\0") != 10 {
        return Err("trim shortened a whole block");
    }
    if trim_to_last_record(b"A=1\0BB=2") != 4 {
        return Err("trim kept a partial trailing record");
    }
    if trim_to_last_record(b"LONGVAR=abc") != 0 {
        return Err("trim served a headless fragment");
    }
    if trim_to_last_record(b"") != 0 {
        return Err("trim of an empty block");
    }

    // --- V-4b-2: the native-maps -> Linux-maps translation, driven directly so
    //     the column math is pinned without needing a live address space.
    if parse_hex(b"0x10000000") != Some(0x10000000) {
        return Err("parse_hex 0x form");
    }
    if parse_hex(b"7ff00000") != Some(0x7ff00000) {
        return Err("parse_hex bare form");
    }
    // A malformed number must be REJECTED, not silently coerced -- a coerced 0
    // would render a plausible-looking row for a line we did not understand.
    if parse_hex(b"0xzz").is_some() || parse_hex(b"").is_some() {
        return Err("parse_hex accepted garbage");
    }
    let mut f: [&[u8]; 6] = [b""; 6];
    if split_fields(b"a b c d e f", &mut f) != 6 || f[0] != b"a" || f[5] != b"f" {
        return Err("split_fields");
    }
    if split_fields(b"a b c", &mut f) != 3 {
        return Err("split_fields short line");
    }

    // An anon mapping: no file identity, no pathname (Linux's 00:00 0).
    let mut m = Render::new();
    let mut anon: [&[u8]; 6] = [b""; 6];
    split_fields(b"0x10000000-0x10001000 rw-p 0x0 anon - -", &mut anon);
    if !maps_row(&anon, b"/bin/x", &mut m) {
        return Err("maps_row rejected an anon row");
    }
    if m.bytes() != b"10000000-10001000 rw-p 00000000 00:00 0\n" {
        return Err("maps_row anon shape");
    }

    // A file-backed mapping takes the exe path, and the devno lands in the
    // major-0 column the way Linux renders a device-less filesystem.
    let mut mf = Render::new();
    let mut fr: [&[u8]; 6] = [b""; 6];
    split_fields(b"0x400000-0x452000 r-xp 0x0 file 0x3:0x12 -", &mut fr);
    if !maps_row(&fr, b"/bin/diorama", &mut mf) {
        return Err("maps_row rejected a file row");
    }
    if !contains_bytes(mf.bytes(), b"00400000-00452000 r-xp 00000000 00:03 18")
        || !contains_bytes(mf.bytes(), b"/bin/diorama")
    {
        return Err("maps_row file shape");
    }

    // The role column becomes Linux's bracket tag.
    let mut ms = Render::new();
    let mut sr: [&[u8]; 6] = [b""; 6];
    split_fields(b"0x7ff00000-0x80000000 rw-p 0x0 anon - stack", &mut sr);
    maps_row(&sr, b"", &mut ms);
    if !contains_bytes(ms.bytes(), b"[stack]") {
        return Err("maps_row stack tag");
    }

    // A malformed row is skipped, never half-rendered.
    let mut mb = Render::new();
    let mut br: [&[u8]; 6] = [b""; 6];
    split_fields(b"notarange rw-p 0x0 anon - -", &mut br);
    if maps_row(&br, b"", &mut mb) {
        return Err("maps_row accepted a malformed range");
    }

    // --- uptime is always renderable and monotonic-shaped
    let up = render(N_UPTIME, &dead);
    if up.bytes().is_empty() {
        return Err("uptime empty");
    }

    Ok(())
}
