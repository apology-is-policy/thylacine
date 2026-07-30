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
// The tree (V-4a Tier 1 + V-4b + V-4c)
// ---------------------------------------------------------------------------
//
// The ROOT IS THE WORLD, not /proc: its children are named for the mount points
// Linux expects, and a container BINDS each where it belongs (V-4c-1; see the
// node table for why binding rather than a second Tattach aname).
//
//   /                     dir
//   /proc                 dir      -> bind at /proc
//   /proc/self            dir      the calling connection's own Proc
//   /proc/self/exe        file     its executable's path   <- the V-4a gate
//   /proc/self/cmdline    file     argv[0], NUL-terminated (Linux shape)
//   /proc/self/status     file     Linux-shaped Name/Pid/Uid/Gid/Threads/VmRSS
//   /proc/self/cwd        file     the working directory (V-4b-1)
//   /proc/self/maps       file     the address space, Linux column layout (V-4b-2)
//   /proc/self/environ    file     the environment, NUL-separated (V-4b-6; self ONLY)
//   /proc/<pid>/...       dir      every live Proc, same file set (V-4b-3)
//   /proc/meminfo         file     MemTotal/MemFree in kB
//   /proc/uptime          file     "<up> <idle>" seconds
//   /proc/sys/kernel/...  files    ostype/osrelease/version/hostname (V-4b-3)
//   /sys                  dir      -> bind at /sys
//   /sys/devices/system/cpu/online    file   the online cpulist  (V-4c-1)
//   /sys/devices/system/cpu/possible  file   the declared cpulist
//   /sys/devices/system/cpu/present   file   the declared cpulist
//   /sys/devices/system/cpu/cpuN      dir    one per CPU (V-4c-1)
//   .../cpuN/cache/index0/coherency_line_size  file  CTR_EL0.DminLine (V-4c-2c)
//   /proc/stat            file     cpu/cpuN + intr/ctxt/btime/processes (V-4c-2c)
//   /proc/cpuinfo         file     one block per online CPU, aarch64 shape
//
// The last three closed section 6.17's per-field question the way it decided:
// GIVE THE KERNEL A SOURCE. MIDR_EL1 and CTR_EL0 are EL0-trapped (SCTLR_EL1.UCT
// is clear in INIT_SCTLR_EL1_MMU_OFF -- an EL0 `mrs midr_el1` is snare:ill,
// which is also why AT_HWCAP must never set hwcap_CPUID), and ctxt/intr had no
// counter at all, so V-4c-2b added per-CPU columns to /ctl/cpu and this file
// reformats them. Two fields were deliberately NOT built: BogoMIPS (no truth to
// tell) and procs_running/procs_blocked (a live census). The one field with no
// source that could not be omitted is the cpu-line user/system split -- see
// push_stat_cpu_line, where the premise is stated at the site.
//
// Not served, each for its own recorded reason (VIVARIUM section 6.10):
//   /self/fd    BLOCKED on #66c -- a cross-Proc fd-list read of a live peer
//               races the #926 at-exit handle-table free. There is no other
//               native source, and inventing one is the section 6.7 failure.
//   /self/auxv  WEIGHED AND NOT BUILT (section 6.14): zero live readers, and a
//               viv-launched binary receives its auxv on the stack by
//               construction, since ld.so bootstraps out of AT_PHDR/AT_ENTRY.
//   .../cpuN/topology  no source: core/cluster identity is not derivable from
//               MPIDR alone on a board whose DTB we do not re-read here.

use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};
use libthyla_rs::ninep as p9;
use libthyla_rs::{
    t_close, t_open, t_srv_peer, t_walk_create, TSrvPeerInfo, T_OPATH, T_OREAD,
    T_WALK_OPEN_FROM_ROOT,
};

// --- V-7: the vivarium (per-container) mode --------------------------------
//
// `--vivarium <runner-pid>` puts the server in per-container mode
// (docs/VIVARIUM.md section 7.2): it posts /srv/viv-dio instead of
// /srv/diorama, and both pid ENUMERATION and per-pid EXISTENCE answer only
// for pids in the container's process tree -- so the diorama cannot be a read
// oracle for the surface the container's territory withheld (the section 7.1
// F6 close). Membership is ppid-descent from the container ENTRYPOINT, and
// the entrypoint is located rather than passed: it is the runner's child that
// is not this server (the runner spawns exactly two children -- this diorama,
// then the entrypoint -- and the entrypoint's pid does not exist yet when the
// diorama must already be up to serve the pre-spawn territory mounts). Before
// the entrypoint exists the set is EMPTY -- fail-closed, never a host view.
//
// Known shape (not a defect): membership is by LIVE ppid chains, so a
// container proc orphaned by its parent's death is reparented to init and
// falls OUT of the view (it disappears from the container's /proc; it gains
// nothing). Linux virtualizes this with a pid namespace; the v1.x pid-1
// virtualization seam owns it.
//
// /proc/self stays PEER-based and unfiltered: `self` answers about the
// CONNECTION'S OWN Proc, so a non-member reader reads only itself -- the
// /self/environ authority-coincidence argument, never a cross-boundary leak.
static VIV_RUNNER: AtomicU32 = AtomicU32::new(0);
static VIV_SELF: AtomicU32 = AtomicU32::new(0);

pub fn set_vivarium(runner_pid: u32, self_pid: u32) {
    VIV_RUNNER.store(runner_pid, Ordering::Relaxed);
    VIV_SELF.store(self_pid, Ordering::Relaxed);
}

fn viv_runner() -> u32 {
    VIV_RUNNER.load(Ordering::Relaxed)
}

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
// The node table. Static nodes use the node index AS the qid path, so they can
// never dangle. Two dynamic families hang off it: the numeric /proc/<pid> dirs
// (V-4b-3) and the /sys .../cpu/cpuN dirs (V-4c-1), neither of which can be a
// static index because both sets are runtime facts.
//
// THE ROOT IS THE SYNTHETIC LINUX WORLD, NOT /proc (restructured at V-4c-1).
// Its children are named for the mount points Linux expects -- `proc` and `sys`
// -- and a container BINDS each where it belongs, which is the mechanism
// section 6.15 already chose for /dev. That correction matters twice:
//
//   * It is what makes ONE server able to serve TWO Linux trees. 9P's other
//     answer -- a second `Tattach` with a different `aname` (Stratum's
//     `ds:<name>` form) -- is UNREACHABLE for this server: a 9P-mode /srv
//     service is reached by open=connect, and the kernel's connect path
//     (devsrv.c::devsrv_open_connect) attaches with a hardcoded EMPTY aname.
//     SYS_ATTACH_9P_SRV does carry one, but is byte-mode-gated and rejects a
//     9P-mode conn for a sound reason (a second p9_client over the same rings
//     would interleave frames). Binding needs no kernel change at all, because
//     SYS_MOUNT takes any readable Spoor -- a subdirectory included.
//   * It keeps the served trees HONEST. Hanging `sys` off a root that IS /proc
//     would put a `/proc/sys`-shaped directory in the namespace that Linux has
//     never had -- section 6.15's "fabrication with a plausible face", one
//     level up from the per-field version.
// ---------------------------------------------------------------------------

const N_ROOT: u64 = 0;
const N_PROC: u64 = 1;
const N_SELF: u64 = 2;
const N_SELF_EXE: u64 = 3;
const N_SELF_CMDLINE: u64 = 4;
const N_SELF_STATUS: u64 = 5;
const N_SELF_CWD: u64 = 6;
const N_SELF_MAPS: u64 = 7;
const N_SELF_ENVIRON: u64 = 8;
const N_MEMINFO: u64 = 9;
const N_UPTIME: u64 = 10;
const N_PROC_SYS: u64 = 11;
const N_PROC_SYS_KERNEL: u64 = 12;
const N_OSTYPE: u64 = 13;
const N_OSRELEASE: u64 = 14;
const N_VERSION: u64 = 15;
const N_HOSTNAME: u64 = 16;
const N_SYSFS: u64 = 17;
const N_SYSFS_DEVICES: u64 = 18;
const N_SYSFS_SYSTEM: u64 = 19;
const N_SYSFS_CPU: u64 = 20;
const N_CPU_ONLINE: u64 = 21;
const N_CPU_POSSIBLE: u64 = 22;
const N_CPU_PRESENT: u64 = 23;
const N_STAT: u64 = 24;      // V-4c-2c
const N_CPUINFO: u64 = 25;   // V-4c-2c
const N_COUNT: u64 = 26;

struct Node {
    name: &'static [u8],
    parent: u64,
    is_dir: bool,
}

static NODES: [Node; N_COUNT as usize] = [
    Node { name: b"",          parent: N_ROOT,            is_dir: true  },
    // --- the /proc tree ---
    Node { name: b"proc",      parent: N_ROOT,            is_dir: true  },
    Node { name: b"self",      parent: N_PROC,            is_dir: true  },
    Node { name: b"exe",       parent: N_SELF,            is_dir: false },
    Node { name: b"cmdline",   parent: N_SELF,            is_dir: false },
    Node { name: b"status",    parent: N_SELF,            is_dir: false },
    Node { name: b"cwd",       parent: N_SELF,            is_dir: false },
    Node { name: b"maps",      parent: N_SELF,            is_dir: false },
    Node { name: b"environ",   parent: N_SELF,            is_dir: false },
    Node { name: b"meminfo",   parent: N_PROC,            is_dir: false },
    Node { name: b"uptime",    parent: N_PROC,            is_dir: false },
    Node { name: b"sys",       parent: N_PROC,            is_dir: true  },
    Node { name: b"kernel",    parent: N_PROC_SYS,        is_dir: true  },
    Node { name: b"ostype",    parent: N_PROC_SYS_KERNEL, is_dir: false },
    Node { name: b"osrelease", parent: N_PROC_SYS_KERNEL, is_dir: false },
    Node { name: b"version",   parent: N_PROC_SYS_KERNEL, is_dir: false },
    Node { name: b"hostname",  parent: N_PROC_SYS_KERNEL, is_dir: false },
    // --- the /sys tree (V-4c-1) ---
    Node { name: b"sys",       parent: N_ROOT,            is_dir: true  },
    Node { name: b"devices",   parent: N_SYSFS,           is_dir: true  },
    Node { name: b"system",    parent: N_SYSFS_DEVICES,   is_dir: true  },
    Node { name: b"cpu",       parent: N_SYSFS_SYSTEM,    is_dir: true  },
    Node { name: b"online",    parent: N_SYSFS_CPU,       is_dir: false },
    Node { name: b"possible",  parent: N_SYSFS_CPU,       is_dir: false },
    Node { name: b"present",   parent: N_SYSFS_CPU,       is_dir: false },
    // --- the two Tier-1 stragglers (V-4c-2c, sourced per section 6.17) ---
    Node { name: b"stat",      parent: N_PROC,            is_dir: false },
    Node { name: b"cpuinfo",   parent: N_PROC,            is_dir: false },
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

// --- the per-CPU family (V-4c-1) -------------------------------------------
//
// /sys/devices/system/cpu/cpuN. Like the pid family this is a runtime set, so
// it cannot be a static index; unlike the pid family it needs no kind field yet
// because the dirs are EMPTY. That emptiness is deliberate, not an oversight:
// the contents Linux puts there (`cache/index0/coherency_line_size`,
// `topology/`) are hardware facts with NO EL0 source -- CTR_EL0 is trapped for
// EL0 (SCTLR_EL1.UCT is clear in INIT_SCTLR_EL1_MMU_OFF), exactly as MIDR_EL1
// is -- so serving them means either fabricating a plausible number or giving
// the kernel a source. That is the same per-field decision section 6.15 defers
// for cpuinfo/stat, and it is deliberately made ONCE, for all three, rather
// than piecemeal here.
//
// The dir itself is not a fabrication: it genuinely names a CPU the kernel
// reports, and its existence is what the legacy "count the cpuN entries"
// enumeration path (busybox nproc, older glibc _SC_NPROCESSORS_CONF) reads.
// Modern consumers read the `online`/`present` range files one level up.
const CPU_BASE: u64 = 1 << 24; // above every static index, below the pid range
const CPU_INDEX_MAX: u64 = 255; // bounds the readdir loop against a wild /ctl/cpu

// V-4c-2c: a cpuN qid gains a KIND above the index. Bits 0..7 stay the CPU
// index exactly as before and kind 0 stays the dir itself, so `cpu_qid(n)` is
// bit-identical to the V-4c-1 encoding -- the subtree is an extension, not a
// renumbering. Bits 8..15 hold the kind; CPU_BASE (bit 24) still separates the
// whole range from the pid qids above it.
const CK_DIR: u64 = 0; // cpuN
const CK_CACHE: u64 = 1; // cpuN/cache
const CK_INDEX0: u64 = 2; // cpuN/cache/index0
const CK_LINESZ: u64 = 3; // cpuN/cache/index0/coherency_line_size

fn cpu_qid_kind(n: u64, kind: u64) -> u64 {
    CPU_BASE | (kind << 8) | n
}
fn cpu_qid(n: u64) -> u64 {
    cpu_qid_kind(n, CK_DIR)
}
fn qid_cpu(path: u64) -> u64 {
    path & 0xFF
}
fn qid_cpu_kind(path: u64) -> u64 {
    (path >> 8) & 0xFF
}
fn is_cpu_node(path: u64) -> bool {
    path >= CPU_BASE && !is_pid_node(path)
}

/// Parse a whole component as `cpu<N>`, returning N. Rejects `cpu`, `cpu0x1`,
/// `cpu01` and an out-of-range index by the same rules parse_pid uses -- except
/// that `cpu0` IS valid here, because unlike a pid, 0 is a real CPU index.
fn parse_cpu_name(name: &[u8]) -> Option<u64> {
    if name.len() < 4 || &name[..3] != b"cpu" {
        return None;
    }
    let digits = &name[3..];
    if digits.len() > 1 && digits[0] == b'0' {
        return None; // "cpu01" would give one CPU two names
    }
    let mut v: u64 = 0;
    for &c in digits {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (c - b'0') as u64;
        if v > CPU_INDEX_MAX {
            return None;
        }
    }
    Some(v)
}

/// The parent of a cpu-range node, total over every kind INCLUDING the leaf.
///
/// Its own function rather than a match inside `walk_child` so the chain's parent
/// relation stays right whether or not the is-a-directory gate lets the leaf
/// reach it. V-4c-2c's warning was that a catch-all `..` here would answer
/// .../system/cpu from the leaf and skip TWO levels -- a wrong answer rather than
/// an error. The gate now makes that unreachable, but a gate and a wrong answer
/// behind it is one edit away from being a wrong answer again.
fn cpu_parent(n: u64, kind: u64) -> u64 {
    match kind {
        CK_CACHE => cpu_qid(n),
        CK_INDEX0 => cpu_qid_kind(n, CK_CACHE),
        CK_LINESZ => cpu_qid_kind(n, CK_INDEX0),
        _ => N_SYSFS_CPU, // CK_DIR: cpuN sits directly under .../system/cpu
    }
}

fn node_is_dir(path: u64) -> bool {
    if is_pid_node(path) {
        return qid_kind(path) == PK_DIR;
    }
    if is_cpu_node(path) {
        // V-4c-2c: the cpuN subtree is dirs down to index0, then one leaf.
        return qid_cpu_kind(path) != CK_LINESZ;
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
    // V-4c-3 SA-2 (#71): nothing is a child of a FILE -- `.` and `..` included.
    // Linux answers ENOTDIR for `<regular file>/anything`, and h_walk turns this
    // None into exactly that errno when the source fid is the one at fault.
    //
    // ONE gate here rather than one per arm, because per-arm was not uniform: the
    // pid and static arms each carried their own, the cpu arm did not, and so the
    // cache leaf was the single node in the whole tree from which `..` resolved.
    // It also makes `dir < N_COUNT` a precondition of the static arm below, which
    // is what keeps its NODES index in bounds.
    if !node_is_dir(dir) {
        return None;
    }
    if is_pid_node(dir) {
        if name == b"." {
            return Some(dir);
        }
        if name == b".." {
            return Some(N_PROC);
        }
        for f in PID_FILES.iter() {
            if f.name == name {
                return Some(pid_qid(qid_pid(dir), f.kind));
            }
        }
        return None;
    }
    if is_cpu_node(dir) {
        let n = qid_cpu(dir);
        let kind = qid_cpu_kind(dir);
        if name == b"." {
            return Some(dir);
        }
        // V-4c-2c: `..` climbs the cache chain rather than always landing on
        // .../system/cpu -- the V-4c-1 shortcut was correct only while cpuN was
        // a leaf dir. A wrong parent here would let `cd cache/..` skip a level.
        if name == b".." {
            return Some(cpu_parent(n, kind));
        }
        // The cache subtree: single-child chains, so each level is a literal
        // name match and there is no enumeration to get wrong. Sourced from
        // /ctl/cpu's `cacheline` column (CTR_EL0.DminLine, decoded to bytes
        // kernel-side so the arm64 register encoding never crosses into
        // userspace -- section 6.8). Anything else is an honest ENOENT.
        return match (kind, name) {
            (CK_DIR, b"cache") => Some(cpu_qid_kind(n, CK_CACHE)),
            (CK_CACHE, b"index0") => Some(cpu_qid_kind(n, CK_INDEX0)),
            (CK_INDEX0, b"coherency_line_size") => Some(cpu_qid_kind(n, CK_LINESZ)),
            _ => None,
        };
    }
    // The is-a-dir gate at the top already proved `dir < N_COUNT` for anything
    // reaching here (node_is_dir bounds its own NODES index), so this arm indexes
    // in range by construction.
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
    if dir == N_PROC {
        if let Some(pid) = parse_pid(name) {
            if native_pid_exists(pid) {
                return Some(pid_qid(pid, PK_DIR));
            }
        }
    }
    // cpuN under .../system/cpu, live iff the kernel reports that many CPUs.
    // Same discipline as the pid arm: existence is decided by a NATIVE read
    // (/ctl/cpu), never by a table this server keeps, so an index the kernel
    // does not have is an honest ENOENT.
    if dir == N_SYSFS_CPU {
        if let Some(n) = parse_cpu_name(name) {
            if n < native_cpu_count() {
                return Some(cpu_qid(n));
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
    /// Discard the first `n` bytes, so what remains begins at file offset `n`.
    ///
    /// This is how `render` honors a Tread offset for the files that build
    /// themselves whole (V-4c-3 SA-3, #72). It lives here rather than as a slice
    /// in `h_read` so that `render`'s contract can be the SAME sentence for every
    /// node -- "the bytes at [off, ...)" -- whether the renderer produced the
    /// whole file or, as environ does, only that window. A caller that had to
    /// know which is a caller that can get it wrong.
    fn drop_prefix(&mut self, n: u64) {
        if n == 0 {
            return;
        }
        if n >= self.len as u64 {
            self.len = 0;
            return;
        }
        let n = n as usize;
        self.buf.copy_within(n..self.len, 0);
        self.len -= n;
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

/// Read a native file into `out` starting at byte `off`. Same degrade-to-None
/// contract as `read_native`.
///
/// POSITIONED, so it is NOT a drop-in for `read_native`: `t_pread` fails ESPIPE
/// on a Dev that is not `.seekable` (#37), and devctl is not -- so every `/ctl`
/// source in this file (meminfo, stat, cpuinfo, the cpu lists, the cache line
/// size) must keep using the cursor reader. devproc IS seekable, which is what
/// makes the one caller here, `render_environ`, legal.
fn read_native_at(path: &[u8], off: u64, out: &mut [u8]) -> Option<usize> {
    if off > i64::MAX as u64 {
        return Some(0); // past any possible file: EOF, not an error
    }
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut total = 0usize;
    loop {
        if total >= out.len() {
            break;
        }
        // Saturating and re-bounded per iteration: `off` is a client-supplied
        // Tread offset, so nothing upstream constrains it.
        let at = off.saturating_add(total as u64);
        if at > i64::MAX as u64 {
            break;
        }
        let n = unsafe {
            libthyla_rs::t_pread(fd, out.as_mut_ptr().add(total), out.len() - total, at as i64)
        };
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
    // V-7: in vivarium mode a pid outside the container tree does not RESOLVE
    // (scripture: "a pid outside the tree does not resolve"). Membership first,
    // then the liveness probe -- membership is read from the live /ctl/procs,
    // so a member is live modulo the same re-read window enumeration has.
    if viv_runner() != 0 && !viv_is_member(pid) {
        return false;
    }
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

// V-7 membership. VIV_PROCS_MAX bounds both the (pid, ppid) table and the
// member set; /ctl/procs is read into the same 2048-byte window as
// native_pid_list (the kernel render is itself bounded at DEVCTL_READ_BUF and
// stops at buffer-full, so a truncated tail loses rows fail-closed -- a member
// past the cut disappears from the view, never the reverse).
const VIV_PROCS_MAX: usize = 64;

fn viv_read_pairs(pairs: &mut [(u32, u32)]) -> usize {
    let mut buf = [0u8; 2048];
    let got = match read_native(b"/ctl/procs", &mut buf) {
        Some(n) => n,
        None => return 0, // unreadable source -> empty set, fail-closed
    };
    parse_pid_ppid_list(&buf[..got], pairs)
}

fn viv_members(out: &mut [u32]) -> usize {
    let runner = viv_runner();
    if runner == 0 {
        return 0;
    }
    let mut pairs = [(0u32, 0u32); VIV_PROCS_MAX];
    let np = viv_read_pairs(&mut pairs);
    compute_members(&pairs[..np], runner, VIV_SELF.load(Ordering::Relaxed), out)
}

fn viv_is_member(pid: u32) -> bool {
    let mut members = [0u32; VIV_PROCS_MAX];
    let n = viv_members(&mut members);
    members[..n].contains(&pid)
}

/// The (pid, ppid) pairs from /ctl/procs -- columns 1 and 2 of every row that
/// parses (the header's "PID" fails the decimal parse and is skipped by the
/// same rule as any junk, the parse_pid_list discipline). NOT split_fields:
/// that splits on SINGLE spaces (the /ctl/cpu shape) and would read the
/// 4-space-run /ctl/procs rows as pid + an empty column, skipping every row.
pub fn parse_pid_ppid_list(text: &[u8], out: &mut [(u32, u32)]) -> usize {
    fn token(line: &[u8], from: usize) -> (&[u8], usize) {
        let mut s = from;
        while s < line.len() && line[s] == b' ' {
            s += 1;
        }
        let mut e = s;
        while e < line.len() && line[e] != b' ' {
            e += 1;
        }
        (&line[s..e], e)
    }
    // The PPID column accepts "0" -- a legitimate VALUE there (kproc-parented
    // and reparented-orphan roots render ppid 0, joey's own row included) --
    // where parse_pid rightly rejects it as a /proc NAME. Without this the
    // whole ppid-0 tier of the table silently vanished from the snapshot
    // (caught by the selftest vector at boot).
    fn parse_ppid(tok: &[u8]) -> Option<u32> {
        if tok == b"0" {
            return Some(0);
        }
        parse_pid(tok)
    }
    let mut n = 0usize;
    for line in text.split(|&c| c == b'\n') {
        if n >= out.len() {
            break;
        }
        let (c1, rest) = token(line, 0);
        let (c2, _) = token(line, rest);
        if let (Some(pid), Some(ppid)) = (parse_pid(c1), parse_ppid(c2)) {
            out[n] = (pid, ppid);
            n += 1;
        }
    }
    n
}

/// The pure half of viv_members: the container's process tree from a (pid,
/// ppid) snapshot. Roots = the runner's children minus this server; members =
/// the roots plus their ppid-descendants, to a fixpoint. With the runner
/// spawning exactly {diorama, entrypoint}, this IS ppid-descent from the
/// entrypoint (docs/VIVARIUM.md section 7.2); stated over a root SET so a
/// hypothetical extra runner child widens the view to its own tree only,
/// never to the host's.
pub fn compute_members(pairs: &[(u32, u32)], runner: u32, me: u32, out: &mut [u32]) -> usize {
    let mut n = 0usize;
    for &(pid, ppid) in pairs {
        if ppid == runner && pid != me && pid != runner && n < out.len() {
            out[n] = pid;
            n += 1;
        }
    }
    // Descend to a fixpoint. Each pass adds only pids whose parent is already a
    // member; bounded by the member capacity, so this terminates even on a
    // (corrupt) cyclic snapshot.
    loop {
        let before = n;
        for &(pid, ppid) in pairs {
            if n >= out.len() {
                break;
            }
            if pid != runner && !out[..n].contains(&pid) && out[..n].contains(&ppid) {
                out[n] = pid;
                n += 1;
            }
        }
        if n == before {
            break;
        }
    }
    n
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
    // V-7: in vivarium mode the enumeration IS the member set (same source,
    // same snapshot semantics); pre-entrypoint it is empty, fail-closed.
    if viv_runner() != 0 {
        return viv_members(out);
    }
    let mut buf = [0u8; 2048]; // matches the kernel's DEVCTL_READ_BUF
    let got = match read_native(b"/ctl/procs", &mut buf) {
        Some(n) => n,
        None => return 0,
    };
    parse_pid_list(&buf[..got], out)
}

/// The declared CPU count, live from /ctl/cpu. Used to decide whether a `cpuN`
/// component names a real CPU -- the cpu-family twin of native_pid_exists.
fn native_cpu_count() -> u64 {
    let mut buf = [0u8; 1024];
    let got = read_ctl_cpu(&mut buf);
    parse_cpu_count(&buf[..got])
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

// --- /ctl/cpu, the source for the whole /sys cpu tree (V-4c-1) -------------
//
// The native render is a header plus one row per CPU:
//
//     cpus: 4
//     cpu idle_ns capacity
//     0 123456 1024
//     1 offline
//
// `cpus:` is smp_cpu_count() -- every CPU the DTB DECLARED, including one that
// failed PSCI bring-up, which the kernel keeps counting (prowl-5 F2). That is
// exactly Linux's `present`/`possible` set, and the rows' `offline` marker is
// exactly its `online` subset -- so both files below are SOURCED, not guessed.
// The mapping is a happy one and worth stating plainly: it exists because
// devctl already had to make the same present-vs-online distinction for prowl.

/// Read /ctl/cpu into `buf`, returning the used slice length. Separate from the
/// parsers so the pure halves stay selftest-able with no VM.
fn read_ctl_cpu(buf: &mut [u8]) -> usize {
    read_native(b"/ctl/cpu", buf).unwrap_or(0)
}

/// The declared CPU count from /ctl/cpu's header. 0 when the source is
/// unreadable, which renders every cpu file empty rather than inventing a count.
pub fn parse_cpu_count(text: &[u8]) -> u64 {
    parse_dec_after(text, b"cpus:").unwrap_or(0).min(CPU_INDEX_MAX + 1)
}

/// Fill `out[i]` with CPU i's online state. FAIL-SAFE: `out` is written only
/// where a row parses, so a truncated or malformed render leaves the caller's
/// `false` in place -- this never claims a CPU is online without a row saying so.
/// Returns the number of rows that parsed.
///
/// The "cpu idle_ns capacity" header is skipped by the same rule as any junk
/// (its first field fails the decimal parse), so no separate header-stripping
/// step can drift out of sync with the kernel's format -- the parse_pid_list
/// discipline.
pub fn parse_cpu_online(text: &[u8], out: &mut [bool]) -> usize {
    let mut rows = 0usize;
    for line in text.split(|&c| c == b'\n') {
        let mut fields: [&[u8]; 4] = [b""; 4];
        let nf = split_fields(line, &mut fields);
        if nf < 2 {
            continue;
        }
        let mut idx: u64 = 0;
        let mut any = false;
        for &c in fields[0] {
            if !c.is_ascii_digit() {
                any = false;
                break;
            }
            idx = idx * 10 + (c - b'0') as u64;
            any = true;
            if idx > CPU_INDEX_MAX {
                any = false;
                break;
            }
        }
        if !any || idx as usize >= out.len() {
            continue;
        }
        out[idx as usize] = fields[1] != b"offline";
        rows += 1;
    }
    rows
}

// --- V-4c-2c: the per-CPU columns (VIVARIUM section 6.17) ------------------
//
// Since V-4c-2b a /ctl/cpu data row is:
//
//     cpu idle_ns capacity ctxt intr cacheline midr
//     0 123456 1024 98765 4321 64 0x410fd083
//
// with an offline CPU still rendering the two-token `<i> offline`. Parsed by
// the same rule as parse_cpu_online: a line whose first field is not a decimal
// index is junk (which is how the header skips itself), and a row shorter than
// the column we want yields None rather than a guess.

/// One CPU's V-4c-2b columns. `None` for a field the row did not carry, so a
/// short/offline/truncated row can never be mistaken for a zero measurement.
#[derive(Copy, Clone, Default)]
pub struct CpuCols {
    pub ctxt: Option<u64>,
    pub intr: Option<u64>,
    pub cacheline: Option<u64>,
    pub midr: Option<u64>,
}

// parse_hex is defined once, below -- the kernel's fmt_uhex emits lowercase
// `0x...` so the existing lowercase-only parser is exactly right, and a second
// laxer copy would be a mirror verified only against itself (the #100 trap, and
// the V-4b-4 duplicated-t_stat lesson).

fn parse_u64(f: &[u8]) -> Option<u64> {
    if f.is_empty() || f.len() > 20 {
        return None;
    }
    let mut v: u64 = 0;
    for &c in f {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

/// Parse CPU `want`'s columns out of a /ctl/cpu render. Every field is
/// independently optional, so a kernel that has not grown a column yet (or a
/// truncated read) degrades field-by-field instead of all-or-nothing.
pub fn parse_cpu_cols(text: &[u8], want: u64) -> CpuCols {
    let mut out = CpuCols::default();
    for line in text.split(|&c| c == b'\n') {
        let mut fields: [&[u8]; 8] = [b""; 8];
        let nf = split_fields(line, &mut fields);
        if nf < 2 {
            continue;
        }
        match parse_u64(fields[0]) {
            Some(i) if i == want => {}
            _ => continue,
        }
        if fields[1] == b"offline" {
            return out; // an offline CPU carries no measurements at all
        }
        // idle_ns=1 capacity=2 ctxt=3 intr=4 cacheline=5 midr=6
        if nf > 3 {
            out.ctxt = parse_u64(fields[3]);
        }
        if nf > 4 {
            out.intr = parse_u64(fields[4]);
        }
        if nf > 5 {
            out.cacheline = parse_u64(fields[5]);
        }
        if nf > 6 {
            out.midr = parse_hex(fields[6]);
        }
        return out;
    }
    out
}

/// A /ctl/cpu render is bounded by the kernel's own 2 KiB leaf buffer; the
/// widened V-4c-2b row is ~90 B x <= 8 CPUs plus two header lines.
const CTL_CPU_MAX: usize = 2048;

/// CPU `want`'s cumulative idle-park ns (column 1), or None when the row is
/// absent, offline, or unparseable -- never 0, which would read as a pegged
/// 100%-busy core (the prowl-5 F2 hazard, on the other side of the boundary).
pub fn parse_cpu_idle_ns(text: &[u8], want: u64) -> Option<u64> {
    for line in text.split(|&c| c == b'\n') {
        let mut fields: [&[u8]; 8] = [b""; 8];
        if split_fields(line, &mut fields) < 2 {
            continue;
        }
        match parse_u64(fields[0]) {
            Some(i) if i == want => return parse_u64(fields[1]),
            _ => continue,
        }
    }
    None
}

/// The `0x`-prefixed hex value after a marker (the /ctl/cpu `hwcap:` line).
/// The decimal twin of this is parse_dec_after; kept separate rather than
/// generalized, because a parser that silently accepts both bases would read a
/// bare `10` as sixteen somewhere down the line.
pub fn parse_hex_after(text: &[u8], marker: &[u8]) -> Option<u64> {
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
        let start = j;
        while j < text.len() && text[j] != b'\n' && text[j] != b' ' {
            j += 1;
        }
        return parse_hex(&text[start..j]);
    }
    None
}

/// The arm64 uapi HWCAP bit -> the name Linux prints in /proc/cpuinfo's
/// `Features` line, in bit order. Kept to the bits hw_features_detect actually
/// derives (arch/arm64/hwfeat.c): a name for a bit the kernel never sets could
/// never appear, and a bit with no name here is silently dropped rather than
/// printed as a number -- absent, not invented.
const HWCAP_NAMES: [(u32, &[u8]); 11] = [
    (0, b"fp"),
    (1, b"asimd"),
    (3, b"aes"),
    (4, b"pmull"),
    (5, b"sha1"),
    (6, b"sha2"),
    (7, b"crc32"),
    (8, b"atomics"),
    (17, b"sha3"),
    (20, b"asimddp"),
    (21, b"sha512"),
];

fn push_hwcap_names(r: &mut Render, hwcap: u64) {
    for (bit, name) in HWCAP_NAMES.iter() {
        if hwcap & (1u64 << bit) != 0 {
            r.push(b" ");
            r.push(name);
        }
    }
}

/// (CLOCK_MONOTONIC, CLOCK_REALTIME) in ns. Read as a pair so /proc/stat's
/// btime (realtime - uptime) comes from ONE clock sample rather than two
/// independent reads a scheduling gap apart. (The "two samples taken back to
/// back" this comment used to claim was the best the old raw-syscall form could
/// do; the vDSO path now makes it literally one -- V-4c-3 SA-4.)
fn clock_pair_ns() -> (u64, u64) {
    // V-4c-3 SA-4: was a local t_timespec mirror plus two raw t_clock_gettime
    // calls, which silently opted out of the #343 vDSO on a file a monitoring
    // tool reads in a loop. The shared reader takes the vDSO page (no syscall)
    // AND derives both values from one counter sample, so btime's
    // realtime - monotonic is exactly the wall offset rather than two samples
    // with a schedulable gap between them.
    libthyla_rs::time::monotonic_realtime_ns()
}

/// Procs created since boot, from /ctl/sched's `created:` -- the kernel's
/// proc_total_created(), which is exactly Linux's forks-since-boot (its own
/// kernel threads included, as ours counts kproc and joey).
fn native_procs_created() -> u64 {
    let mut buf = [0u8; 512];
    let got = read_native(b"/ctl/sched", &mut buf).unwrap_or(0);
    parse_dec_after(&buf[..got], b"created:").unwrap_or(0)
}

/// Sum a column across every CPU that reports it. Linux's `ctxt` and `intr` are
/// system-wide totals; the kernel accounts them per-CPU (as Linux does
/// internally), so the summation is the translation.
pub fn sum_cpu_col(text: &[u8], ncpus: u64, pick: fn(&CpuCols) -> Option<u64>) -> u64 {
    let mut total: u64 = 0;
    let mut i = 0u64;
    while i < ncpus && i <= CPU_INDEX_MAX {
        if let Some(v) = pick(&parse_cpu_cols(text, i)) {
            total = total.saturating_add(v);
        }
        i += 1;
    }
    total
}

/// Push a CPU set in Linux's cpulist format: comma-separated runs, each `a` or
/// `a-b` ("0-3", "0,2-3", "0"). Emits nothing at all for an empty set, so an
/// unreadable source renders an empty file rather than a misleading "0".
pub fn push_cpu_list(r: &mut Render, set: &[bool]) {
    let mut i = 0usize;
    let mut first = true;
    while i < set.len() {
        if !set[i] {
            i += 1;
            continue;
        }
        let start = i;
        while i + 1 < set.len() && set[i + 1] {
            i += 1;
        }
        if !first {
            r.push(b",");
        }
        first = false;
        r.push_dec(start as u64);
        if i > start {
            r.push(b"-");
            r.push_dec(i as u64);
        }
        i += 1;
    }
    if !first {
        r.push(b"\n");
    }
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
            // V-4c-3 F7: checked, matching parse_u64. The wrapping form this
            // replaced turned an over-long digit run into an arbitrary u64 with
            // no signal -- and the callers multiply the result by 4 under
            // overflow-checks, so a fabricated value became a panic in a
            // panic=abort server. An unrepresentable number is not a value:
            // None (the same answer as "absent") is the honest reading.
            let mut v: u64 = 0;
            let mut any = false;
            while j < text.len() && text[j].is_ascii_digit() {
                v = v.checked_mul(10)?.checked_add((text[j] - b'0') as u64)?;
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
        // V-4c-3 F7: checked, as in parse_kv_dec above.
        let mut v: u64 = 0;
        let mut any = false;
        while j < text.len() && text[j].is_ascii_digit() {
            v = v.checked_mul(10)?.checked_add((text[j] - b'0') as u64)?;
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
            // V-4c-3 F7: saturating, like every other arithmetic site in this
            // file. Under overflow-checks an unchecked `* 4` panics, and
            // panic = "abort" makes a panic the death of the whole server.
            r.push_dec(pages.saturating_mul(4)); // 4 KiB pages -> kB, the Linux unit
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
/// There is no truncation to describe. Since V-4c-3 SA-3 (#72) this serves the
/// WINDOW at `off`, so what does not fit in one read continues at the next
/// offset. The whole-record trim it used to carry belonged to a cap that dropped
/// the tail outright, and it retired with that cap.
fn render_environ(pid: u32, off: u64, r: &mut Render) {
    let mut pbuf = [0u8; 64];
    let n = native_proc_path(pid, b"environ", &mut pbuf);
    let mut ebuf = [0u8; RENDER_MAX];
    // A DENIAL arrives as Some(0), not None: the open succeeds (devproc gates at
    // the read, not the open) and the read returns -1, which read_native reports
    // as zero bytes. None means the open itself failed -- a gone pid. Both render
    // empty, which is what makes the deny path indistinguishable from an empty
    // environment, exactly as it should be.
    let got = match read_native_at(&pbuf[..n], off, &mut ebuf) {
        Some(g) => g,
        None => return,
    };
    // Pushed VERBATIM -- no record trim. Until V-4c-3 SA-3 (#72) this read the
    // file's first 4 KiB and dropped back to the last NUL, so a record straddling
    // the boundary was discarded and every record after it was LOST: an Env holds
    // up to ENV_MAX_ENTRIES x ENV_VALUE_MAX, and past 4 KiB the tail simply
    // vanished. That is the failure the kernel side built an offset-aware
    // env_render_environ to avoid, re-imposed one layer up, and it does not look
    // like an error to a consumer -- it looks like the variable was never set.
    //
    // A window has no tail to discard: the straddling record continues at the
    // next offset, exactly as it does through the native file. So the trim is not
    // merely unnecessary here, it would be the bug.
    r.push(&ebuf[..got]);
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
        r.push_dec(total.saturating_mul(4));
        r.push(b" kB\n");
    }
    if let Some(free) = parse_kv_dec(text, b"free") {
        r.push(b"MemFree:        ");
        r.push_dec(free.saturating_mul(4));
        r.push(b" kB\n");
        // Linux consumers overwhelmingly read MemAvailable; without a reclaim
        // model the honest value is MemFree, not a fabricated estimate.
        r.push(b"MemAvailable:   ");
        r.push_dec(free.saturating_mul(4));
        r.push(b" kB\n");
    }
}

/// /sys/devices/system/cpu/{online,possible,present}.
///
/// `possible` and `present` are the DECLARED set (every CPU /ctl/cpu's header
/// counts); `online` is the subset whose row is not `offline`. On QEMU-virt the
/// two coincide, because PSCI bring-up never fails there -- the distinction is
/// real on a board where it can, which is precisely why devctl reports it.
///
/// An unreadable source renders EMPTY (push_cpu_list emits nothing for an empty
/// set), never "0" -- a consumer that reads "0" would conclude one CPU exists.
fn render_cpu_list(node: u64, r: &mut Render) {
    let mut buf = [0u8; 1024];
    let got = read_ctl_cpu(&mut buf);
    let count = parse_cpu_count(&buf[..got]) as usize;
    if count == 0 {
        return;
    }
    let mut set = [false; (CPU_INDEX_MAX + 1) as usize];
    if node == N_CPU_ONLINE {
        parse_cpu_online(&buf[..got], &mut set[..count]);
    } else {
        // possible / present: the declared set, all of it.
        for s in set[..count].iter_mut() {
            *s = true;
        }
    }
    push_cpu_list(r, &set[..count]);
}

/// /uptime -- "<seconds-up> <seconds-idle>", from CLOCK_MONOTONIC (ns since
/// boot). Linux's second field is aggregate idle time, which Thylacine does not
/// track per-CPU here; 0 is the honest placeholder (Linux itself reports 0 on
/// some virtualized configurations, and no consumer treats it as an error).
fn render_uptime(r: &mut Render) {
    // V-4c-3 SA-4: the comment that used to sit here explained that
    // libthyla_rs::time kept its TimeSpec private and Instant exposed no
    // "since boot" accessor, so this read the clock directly. That was a real
    // API gap, and going around it silently cost the #343 vDSO fast path.
    // Instant::since_boot closes it.
    let up = libthyla_rs::time::Instant::now().since_boot();
    let secs = up.as_secs();
    let hund = (up.subsec_nanos() as u64) / 10_000_000;
    r.push_dec(secs);
    r.push(b".");
    if hund < 10 {
        r.push(b"0");
    }
    r.push_dec(hund);
    r.push(b" 0.00\n");
}

// --- /proc/stat + /proc/cpuinfo, the two Tier-1 stragglers (V-4c-2c) -------
//
// Every field here is sourced per section 6.17. The one exception is stated
// where it happens, in render_stat's jiffies line, and is stated because a
// positional column cannot be omitted the way a whole line can.

/// Linux reports CPU time in USER_HZ, conventionally 100 Hz, regardless of the
/// kernel's own tick. So a jiffy is 10 ms and the conversion is ns / 10^7.
const NS_PER_JIFFY: u64 = 10_000_000;

/// Push one `cpu`/`cpuN` line. `idle_ns` is measured; `busy_ns` is derived
/// (elapsed minus idle) and therefore also measured -- what is NOT measured is
/// how that busy time SPLITS between user and kernel.
///
/// THE STATED PREMISE (section 6.17): Thylacine has no EL0-vs-EL1 time
/// accounting anywhere, so the split has no source and no material. Unlike
/// every other unsourced field in this arc it cannot be omitted -- the columns
/// are positional, so a missing middle column is a WRONG answer rather than an
/// absent one. All non-idle time is therefore reported as `system`. Utilization
/// (1 - idle/total), which is what essentially every consumer computes, is
/// exactly right either way; a consumer that specifically wants the split gets
/// a degenerate answer rather than a plausible fabricated distribution.
/// Revisit this the day per-mode accounting lands.
fn push_stat_cpu_line(r: &mut Render, label: &[u8], busy_ns: u64, idle_ns: u64) {
    r.push(label);
    //     user nice system idle iowait irq softirq steal guest guest_nice
    // iowait/steal/guest are legitimately zero for us (no block-wait
    // accounting, not a guest, no nested guests) -- those are honest zeros,
    // not the fabricated kind.
    r.push(b" 0 0 ");
    r.push_dec(busy_ns / NS_PER_JIFFY);
    r.push(b" ");
    r.push_dec(idle_ns / NS_PER_JIFFY);
    r.push(b" 0 0 0 0 0 0\n");
}

/// /proc/stat. cpu/cpuN from /ctl/cpu's idle_ns against CLOCK_MONOTONIC;
/// ctxt and intr summed from the per-CPU columns V-4c-2b added; processes from
/// /ctl/sched's `created:`; btime from REALTIME minus MONOTONIC.
fn render_stat(r: &mut Render) {
    let mut buf = [0u8; CTL_CPU_MAX];
    let got = read_ctl_cpu(&mut buf);
    let text = &buf[..got];
    let ncpus = parse_cpu_count(text);

    // Elapsed since boot IS CLOCK_MONOTONIC, and it is the denominator every
    // per-CPU busy figure is derived against.
    let (up_ns, real_ns) = clock_pair_ns();

    // The aggregate `cpu` line is the sum over CPUs, exactly as Linux's is --
    // NOT a single-CPU figure scaled up, which would misreport a partly-idle
    // box. An offline CPU contributes nothing to either column.
    let mut idle_total: u64 = 0;
    let mut busy_total: u64 = 0;
    let mut i = 0u64;
    while i < ncpus && i <= CPU_INDEX_MAX {
        if let Some(idle) = parse_cpu_idle_ns(text, i) {
            idle_total = idle_total.saturating_add(idle);
            busy_total = busy_total.saturating_add(up_ns.saturating_sub(idle));
        }
        i += 1;
    }
    push_stat_cpu_line(r, b"cpu", busy_total, idle_total);
    i = 0;
    while i < ncpus && i <= CPU_INDEX_MAX {
        if let Some(idle) = parse_cpu_idle_ns(text, i) {
            // Whole LINES here (V-4c-3 SA-3, #72) -- the same commit discipline
            // as cpuinfo's blocks, at this file's unit. A truncated `cpuN` row
            // would hand a jiffies parser a short column count, and the lines
            // after it are what carry intr/ctxt/btime.
            let mark = r.len();
            let mut label = Render::new();
            label.push(b"cpu");
            label.push_dec(i);
            push_stat_cpu_line(r, label.bytes(), up_ns.saturating_sub(idle), idle);
            if r.len() == RENDER_MAX {
                r.truncate_to(mark);
                break;
            }
        }
        i += 1;
    }

    r.push(b"intr ");
    r.push_dec(sum_cpu_col(text, ncpus, |c| c.intr));
    r.push(b"\nctxt ");
    r.push_dec(sum_cpu_col(text, ncpus, |c| c.ctxt));

    // btime is the wall-clock second the system booted: REALTIME now minus how
    // long we have been up. Both halves are sourced (LS-K's RTC anchor and the
    // monotonic counter), so this is a derivation, not an invention.
    r.push(b"\nbtime ");
    r.push_dec((real_ns.saturating_sub(up_ns)) / 1_000_000_000);

    r.push(b"\nprocesses ");
    r.push_dec(native_procs_created());
    // procs_running/procs_blocked would each need a live state census; they are
    // omitted rather than zeroed, which is the whole-line freedom the jiffies
    // columns above do not have.
    r.push(b"\n");
}

/// /proc/cpuinfo, one block per online CPU -- the aarch64 shape. `Features` is
/// the AT_HWCAP word (already the arm64 uapi numbering, so the names map
/// one-to-one); the four identity lines are MIDR_EL1's fields, which is what
/// Linux prints there. BogoMIPS is OMITTED: it is a calibration artifact of a
/// loop Thylacine does not run, and meaningless on Linux too.
fn render_cpuinfo(r: &mut Render) {
    let mut buf = [0u8; CTL_CPU_MAX];
    let got = read_ctl_cpu(&mut buf);
    let text = &buf[..got];
    let ncpus = parse_cpu_count(text);
    let hwcap = parse_hex_after(text, b"hwcap:").unwrap_or(0);

    let mut i = 0u64;
    while i < ncpus && i <= CPU_INDEX_MAX {
        let cols = parse_cpu_cols(text, i);
        // An offline CPU carries no MIDR, and Linux lists only online CPUs.
        if let Some(midr) = cols.midr {
            // V-4c-3 SA-3 (#72): commit whole BLOCKS, the render_maps row idiom
            // at the unit a cpuinfo consumer actually parses. A block is ~150 B,
            // so RENDER_MAX holds about 27 -- unreachable at DTB_MAX_CPUS = 8,
            // but the failure if it were reached is a half block with a
            // `processor` line and no `CPU part`, which reads as a malformed
            // entry rather than a short file. Whole lines would not be enough.
            let mark = r.len();
            r.push(b"processor\t: ");
            r.push_dec(i);
            r.push(b"\nFeatures\t:");
            push_hwcap_names(r, hwcap);
            // "CPU architecture: 8" is a CONSTANT, not a measurement -- the
            // section 6.9 category: a declaration about which ABI the caller
            // sees. Every part we can run on is ARMv8.
            r.push(b"\nCPU implementer\t: 0x");
            r.push_hex((midr >> 24) & 0xFF, 2);
            r.push(b"\nCPU architecture: 8\nCPU variant\t: 0x");
            r.push_hex((midr >> 20) & 0xF, 1);
            r.push(b"\nCPU part\t: 0x");
            r.push_hex((midr >> 4) & 0xFFF, 3);
            r.push(b"\nCPU revision\t: ");
            r.push_dec(midr & 0xF);
            r.push(b"\n\n");
            if r.len() == RENDER_MAX {
                r.truncate_to(mark);
                break;
            }
        }
        i += 1;
    }
}

/// cpuN/cache/index0/coherency_line_size -- CTR_EL0.DminLine, decoded to bytes
/// kernel-side. Empty when the CPU reports none, never a plausible default: a
/// consumer sizing an allocation off a guessed line size is exactly the harm.
fn render_cpu_cacheline(cpu: u64, r: &mut Render) {
    let mut buf = [0u8; CTL_CPU_MAX];
    let got = read_ctl_cpu(&mut buf);
    if let Some(v) = parse_cpu_cols(&buf[..got], cpu).cacheline {
        r.push_dec(v);
        r.push(b"\n");
    }
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

/// Render `node` for `peer`: the bytes at `[off, ...)`, bounded by RENDER_MAX.
/// Directories render empty (they are read via Treaddir, not Tread).
///
/// Almost every renderer here builds its WHOLE file and `render` drops the first
/// `off` bytes at the end, because every one of their sources is bounded far
/// under RENDER_MAX -- /ctl/cpu at DTB_MAX_CPUS, a fixed-shape status block, a
/// single path. `environ` is the exception and the reason `off` exists at all:
/// its source is an Env of up to ENV_MAX_ENTRIES x ENV_VALUE_MAX bytes, so it
/// renders the window directly and returns early (V-4c-3 SA-3, #72).
///
/// The contract is the same sentence either way, which is the point -- `h_read`
/// takes what it is given and does no offset arithmetic of its own.
pub fn render(node: u64, peer: &TSrvPeerInfo, off: u64) -> Render {
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
        // No environ in PID_FILES -- the kernel serves a Proc's environment to
        // its owner only, and the diorama answers for its connection's own peer
        // and never for an arbitrary pid. So every per-pid file is whole-render.
        r.drop_prefix(off);
        return r;
    }
    // /self/*. The peer's liveness is checked HERE rather than inside each
    // renderer: a dead peer must render empty rather than read /proc/<pid>/*
    // for a pid that may since have been reused.
    let alive = peer.alive != 0;
    // The one windowed renderer, so it returns before the drop_prefix below --
    // its Render already BEGINS at `off`. A dead peer still renders empty.
    if node == N_SELF_ENVIRON {
        if alive {
            render_environ(peer.pid, off, &mut r);
        }
        return r;
    }
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
        N_MEMINFO => render_meminfo(&mut r),
        N_UPTIME => render_uptime(&mut r),
        N_STAT => render_stat(&mut r),
        N_CPUINFO => render_cpuinfo(&mut r),
        N_CPU_ONLINE | N_CPU_POSSIBLE | N_CPU_PRESENT => render_cpu_list(node, &mut r),
        N_OSTYPE => r.push(OSTYPE),
        N_OSRELEASE => r.push(OSRELEASE),
        N_VERSION => r.push(KVERSION),
        N_HOSTNAME => r.push(HOSTNAME),
        // V-4c-2c: cpuN/cache/index0/coherency_line_size, the only leaf in the
        // cpu subtree. Rendered by CPU index, so a heterogeneous board reports
        // each core's own line size rather than the boot CPU's for all of them.
        _ if is_cpu_node(node) && qid_cpu_kind(node) == CK_LINESZ => {
            render_cpu_cacheline(qid_cpu(node), &mut r)
        }
        _ => {}
    }
    r.drop_prefix(off);
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
        // V-4c-3 SA-2 (#71): walking a NAME from a file is ENOTDIR, Linux's own
        // answer, rather than the ENOENT an empty partial walk would produce.
        // Scoped to nwname > 0 because a ZERO-length walk is how a client clones
        // a fid, and cloning a fid that names a file is legal 9P.
        //
        // Only the SOURCE gets a real errno. A walk that dies at element k > 0
        // returns k qids and no error at all -- 9P2000.L's partial-walk rule
        // leaves no room to say why -- so `dir/file/x` reports "file" resolved
        // and nothing after it. That is what v9fs does on Linux too: the client
        // re-walks from the file it did reach and collects the ENOTDIR then.
        if a.nwname > 0 && !node_is_dir(src_fid.node) {
            return self.err(tag, p9::E_NOTDIR);
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
        // V-4c-3 SA-3 (#72): render is given the offset and hands back the bytes
        // AT it, so there is no slice to get wrong here. An offset past the end
        // yields an empty Render, which is the EOF the old `off >= body.len()`
        // early return produced.
        let r = render(f.node, &peer, a.offset);
        let body = r.bytes();
        // V-4c-3 F2: saturating, because Tversion accepts any u32 msize
        // (including 0) and negotiates min(client, SRV_MSIZE) with no floor, so
        // a raw `msize - 11` UNDERFLOWS for any negotiated msize < 11. This
        // crate builds with overflow-checks = true and panic = "abort", so that
        // is not a wrap -- it terminates the server, and /dio dies for every
        // mount on the box. Three messages from any Proc that can open
        // /srv/diorama reach it. netd, ptyfs and corvus all spell this exact
        // expression saturating; the diorama was the outlier.
        let cap = (a.count as usize).min((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4));
        let n = body.len().min(cap);
        p9::build_rread(&mut self.out_buf, tag, &body[..n])
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
        // V-4c-3 F2: saturating -- see h_read for why a raw subtraction here is
        // an abort rather than a wrap. Treaddir on the root is reachable with no
        // walk at all, which is what made this the shortest path to the crash.
        let budget = (a.count as usize).min((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4));
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
        // /proc continues into the live pids once its static children are done,
        // at cookies >= N_COUNT -- so a `ps` that readdirs /proc sees the
        // numeric dirs Linux puts there. Reached only when the static phase ran
        // to completion, so a budget-truncated call resumes in the right phase.
        if f.node == N_PROC && child >= N_COUNT {
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
        // .../system/cpu continues into the live cpuN dirs, by the same
        // two-phase cookie rule -- the enumeration path older consumers use to
        // COUNT CPUs (busybox nproc greps this listing).
        if f.node == N_SYSFS_CPU && child >= N_COUNT {
            let n = native_cpu_count();
            let mut i = child - N_COUNT;
            while i < n {
                let mut nm = Render::new();
                nm.push(b"cpu");
                nm.push_dec(i);
                if !self.emit_dirent(&mut data, budget, cpu_qid(i), N_COUNT + i + 1, nm.bytes()) {
                    break;
                }
                i += 1;
            }
        }
        // V-4c-2c: the cache chain. Each level has exactly one child, so the
        // cookie is simply "have I emitted it yet" -- 0 means not, and any
        // non-zero cookie means the single entry is already behind us.
        //
        // V-4c-3 F3: gate on `a.offset`, NOT on `child`. `child` is the STATIC
        // loop's cursor, and that loop runs first over every static node looking
        // for one whose parent is f.node. A cpu qid is >= CPU_BASE (1<<24) and
        // every static parent is < N_COUNT (26), so no entry ever matches, every
        // iteration takes the `continue`, and `child` exits the loop at N_COUNT
        // -- never 0. The guard could therefore never fire, and the ENTIRE
        // cpuN/cache/index0 subtree readdir'd as an empty directory.
        //
        // walk still resolved every level by name, which is exactly why nothing
        // caught it: the selftest below drives walk_child, and diorama-probe
        // opens the leaf by literal path. Neither issued a Treaddir on a cpu
        // node. A consumer that ENUMERATES to find index* (the portable way --
        // the index numbering is not fixed across cache levels) saw nothing.
        if is_cpu_node(f.node) && a.offset == 0 {
            let n = qid_cpu(f.node);
            let one = match qid_cpu_kind(f.node) {
                CK_DIR => Some((cpu_qid_kind(n, CK_CACHE), &b"cache"[..])),
                CK_CACHE => Some((cpu_qid_kind(n, CK_INDEX0), &b"index0"[..])),
                CK_INDEX0 => Some((
                    cpu_qid_kind(n, CK_LINESZ),
                    &b"coherency_line_size"[..],
                )),
                _ => None,
            };
            if let Some((q, nm)) = one {
                self.emit_dirent(&mut data, budget, q, 1, nm);
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
        // V-4c-3 SA-3 (#72): environ reports 0, not a rendered length.
        //
        // The self-sufficient reason: its content is a WINDOW now, so there is no
        // total to report without reading the whole file on every stat -- and the
        // pre-#72 answer, the TRUNCATED length, was a lie the moment an
        // environment passed 4 KiB. 0 is the only number here that is not a
        // guess, and it says the true thing: read to EOF.
        //
        // It also matches the source. devproc_stat_native (kernel/devproc.c)
        // zeroes the whole t_stat and never sets size for ANY /proc file --
        // verified, not assumed -- so this reports what the surface it
        // re-presents reports. Linux is understood to do the same for
        // /proc/*/environ, generated rather than stored; that corroborates the
        // choice but is not what it rests on.
        //
        // Deliberately NOT extended to the rest: their sources are bounded, one
        // render measures them exactly, and a stat that agrees with its read is
        // worth more than symmetry with a zero.
        let size = if is_dir || f.node == N_SELF_ENVIRON {
            0u64
        } else {
            let peer = self.peer();
            render(f.node, &peer, 0).bytes().len() as u64
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
///
/// V-7: vivarium mode posts the FIXED name /srv/viv-dio instead. Fixed on
/// purpose: the boot SrvRegistry never frees a dead entry (task #33), so a
/// per-container unique name would burn a registry slot per `viv run` forever;
/// a fixed name rebinds one tombstone across sequential runs. A CONCURRENT
/// second container collides here and the runner fails closed -- concurrent
/// containers are a v1.x seam riding the #33 registry-lifecycle fix.
pub fn post_srv_diorama() -> Result<i64, ()> {
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let name: &[u8] = if viv_runner() != 0 { b"viv-dio" } else { b"diorama" };
    let listener = unsafe { t_walk_create(srv, name.as_ptr(), name.len(), T_OREAD, 0) };
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
    // --- V-4c-1: the root is the WORLD, and its children are the Linux mount
    //     points. A container binds each; nothing here is reachable from the
    //     other, which is what makes one server able to serve two trees.
    if walk_child(N_ROOT, b"proc") != Some(N_PROC) {
        return Err("walk /proc");
    }
    if walk_child(N_ROOT, b"sys") != Some(N_SYSFS) {
        return Err("walk /sys");
    }
    // The two trees are SIBLINGS, not nested: /proc must not contain the sysfs
    // tree and /sys must not contain proc's. A hit either way would put a
    // directory in a container's namespace that Linux has never had.
    if walk_child(N_PROC, b"devices").is_some() {
        return Err("/proc leaked the sysfs tree");
    }
    if walk_child(N_SYSFS, b"self").is_some() || walk_child(N_SYSFS, b"meminfo").is_some() {
        return Err("/sys leaked the proc tree");
    }

    // --- the static tree resolves exactly as declared
    if walk_child(N_PROC, b"self") != Some(N_SELF) {
        return Err("walk /proc/self");
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
    if walk_child(N_PROC, b"meminfo") != Some(N_MEMINFO) {
        return Err("walk /proc/meminfo");
    }
    // A file has no children, and a miss is a miss. V-4c-3 SA-2 (#71) extends
    // that to the two names every path normalizer tries: `<file>/..` and
    // `<file>/.` must MISS, or a walker probing a component to classify it reads
    // a file as a directory.
    if walk_child(N_SELF_EXE, b"anything").is_some()
        || walk_child(N_SELF_EXE, b"..").is_some()
        || walk_child(N_SELF_EXE, b".").is_some()
    {
        return Err("walked into a file");
    }
    if walk_child(N_ROOT, b"nope").is_some() {
        return Err("resolved a nonexistent name");
    }
    // `..` climbs; the root's parent is itself.
    if walk_child(N_SELF, b"..") != Some(N_PROC) {
        return Err("walk ..");
    }
    if walk_child(N_PROC, b"..") != Some(N_ROOT) {
        return Err("walk /proc/..");
    }
    if walk_child(N_ROOT, b"..") != Some(N_ROOT) {
        return Err("root .. must be root");
    }
    // /meminfo is NOT reachable under /self (parent-scoped resolution).
    if walk_child(N_SELF, b"meminfo").is_some() {
        return Err("cross-parent name resolved");
    }

    // --- V-4b-3: /proc/sys/kernel, the phenotype's self-description. NOTE the
    //     path: this is the SYSCTL tree, a different thing from the /sys the
    //     root now carries, which happens to share a name.
    if walk_child(N_PROC, b"sys") != Some(N_PROC_SYS) {
        return Err("walk /proc/sys");
    }
    if walk_child(N_PROC_SYS, b"kernel") != Some(N_PROC_SYS_KERNEL) {
        return Err("walk /proc/sys/kernel");
    }
    if walk_child(N_PROC_SYS_KERNEL, b"ostype") != Some(N_OSTYPE) {
        return Err("walk /proc/sys/kernel/ostype");
    }
    if walk_child(N_PROC_SYS_KERNEL, b"..") != Some(N_PROC_SYS) {
        return Err("walk /proc/sys/kernel/..");
    }
    // These render the SAME for any peer, dead or alive -- they describe the
    // phenotype, not the Proc, so a peer-dependent answer would be a bug.
    let dead = TSrvPeerInfo::default(); // alive == 0
    if render(N_OSTYPE, &dead, 0).bytes() != b"Linux\n" {
        return Err("ostype");
    }
    if render(N_HOSTNAME, &dead, 0).bytes() != b"(none)\n" {
        return Err("hostname");
    }
    // osrelease must parse as Linux <major>.<minor>.<patch> with a major high
    // enough to clear glibc's minimum-kernel refusal.
    {
        let rel = render(N_OSRELEASE, &dead, 0);
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
    if walk_child(pid_qid(7, PK_DIR), b"..") != Some(N_PROC) {
        return Err("walk /<pid>/..");
    }
    if walk_child(pid_qid(7, PK_DIR), b"nope").is_some() {
        return Err("resolved a nonexistent per-pid file");
    }
    if walk_child(pid_qid(7, PK_MAPS), b"anything").is_some()
        || walk_child(pid_qid(7, PK_MAPS), b"..").is_some()
        || walk_child(pid_qid(7, PK_MAPS), b".").is_some()
    {
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
    if walk_child(N_PROC, b"4294967295").is_some() {
        return Err("resolved a nonexistent pid");
    }
    // V-4c-1: pids live under /proc, NOT at the world root. Pre-restructure this
    // name reached the native existence check at the root; now it must not, or a
    // container's `/` would carry Linux's process dirs.
    if walk_child(N_ROOT, b"1").is_some() {
        return Err("a pid resolved at the world root");
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

    // --- V-7: the vivarium membership computation, pure vectors.
    {
        let procs = b"PID    PPID    NAME    STATE    THREADS\n\
                      1    0    joey    ALIVE    1\n\
                      10    1    viv    ALIVE    1\n\
                      11    10    diorama    ALIVE    1\n\
                      20    10    probe    ALIVE    1\n\
                      21    20    child    ALIVE    1\n\
                      22    21    grandchild    ALIVE    1\n\
                      30    1    login    ALIVE    1\n";
        let mut pairs = [(0u32, 0u32); 8];
        let np = parse_pid_ppid_list(procs, &mut pairs);
        if np != 7 || pairs[1] != (10, 1) || pairs[3] != (20, 10) {
            return Err("pid/ppid list parse");
        }
        // runner=10, me=11: the container tree is exactly {20, 21, 22} --
        // the diorama self-excludes, the runner and the host procs are out.
        let mut m = [0u32; 8];
        let n = compute_members(&pairs[..np], 10, 11, &mut m);
        if n != 3 || !m[..n].contains(&20) || !m[..n].contains(&21) || !m[..n].contains(&22) {
            return Err("vivarium members");
        }
        if m[..n].contains(&10) || m[..n].contains(&11) || m[..n].contains(&1) || m[..n].contains(&30)
        {
            return Err("vivarium member leak");
        }
        // Pre-entrypoint (only the diorama exists yet): EMPTY, fail-closed.
        let early = [(1u32, 0u32), (10u32, 1u32), (11u32, 10u32)];
        if compute_members(&early, 10, 11, &mut m) != 0 {
            return Err("vivarium pre-entrypoint not empty");
        }
        // A reparented orphan (ppid fell back to init) leaves the view --
        // the documented fail-closed shape, never a host leak.
        let orphaned = [(1u32, 0u32), (10u32, 1u32), (11u32, 10u32), (21u32, 1u32)];
        if compute_members(&orphaned, 10, 11, &mut m) != 0 {
            return Err("vivarium orphan leaked in");
        }
        // A corrupt CYCLIC snapshot terminates and admits nothing rooted
        // outside the runner.
        let cyclic = [(40u32, 41u32), (41u32, 40u32), (10u32, 1u32), (11u32, 10u32)];
        if compute_members(&cyclic, 10, 11, &mut m) != 0 {
            return Err("vivarium cycle handled wrong");
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

    // --- V-4c-3 F7: an over-long digit run must not become an arbitrary value.
    //     Both parsers used to accumulate with wrapping_mul/wrapping_add and no
    //     length bound, so 30 digits silently produced some u64 -- which the
    //     meminfo and status renderers then multiply by 4. Under
    //     overflow-checks that multiply panics, and with panic = "abort" the
    //     panic is the whole server dying. The sources are kernel-generated and
    //     bounded, so this was never reachable; it is the inconsistency with the
    //     checked/saturating discipline used everywhere else in the file that
    //     makes it worth closing. None is the honest answer for a number that
    //     does not fit.
    {
        let huge = b"total: 99999999999999999999999999 pages\nfree: 12 pages\n";
        if parse_kv_dec(huge, b"total").is_some() {
            return Err("parse_kv_dec wrapped an over-long digit run into a value");
        }
        // The overflowing key must not poison a later well-formed one.
        if parse_kv_dec(huge, b"free") != Some(12) {
            return Err("parse_kv_dec lost a good key after an over-long one");
        }
        let huge2 = b"x pid: 99999999999999999999999999\n";
        if parse_dec_after(huge2, b"pid:").is_some() {
            return Err("parse_dec_after wrapped an over-long digit run into a value");
        }
        // u64::MAX itself is representable and must still parse.
        let max = b"total: 18446744073709551615 pages\n";
        if parse_kv_dec(max, b"total") != Some(u64::MAX) {
            return Err("parse_kv_dec rejected u64::MAX");
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
    if !render(N_SELF_EXE, &dead, 0).bytes().is_empty() {
        return Err("dead peer served an exe");
    }
    if !render(N_SELF_STATUS, &dead, 0).bytes().is_empty() {
        return Err("dead peer served a status");
    }
    if !render(N_SELF_CMDLINE, &dead, 0).bytes().is_empty() {
        return Err("dead peer served a cmdline");
    }
    if !render(N_SELF_CWD, &dead, 0).bytes().is_empty() {
        return Err("dead peer served a cwd");
    }
    if !render(N_SELF_MAPS, &dead, 0).bytes().is_empty() {
        return Err("dead peer served a maps");
    }
    if !render(N_SELF_ENVIRON, &dead, 0).bytes().is_empty() {
        return Err("dead peer served an environ");
    }

    // --- V-4c-3 SA-3 (#72): the offset contract. This REPLACES the environ
    // whole-record trim legs, because the trim is gone: environ is a real window
    // now, so a record straddling the boundary continues at the next offset
    // instead of being dropped along with everything after it.
    //
    // drop_prefix is what carries the offset for every whole-render file, so its
    // three boundaries are pinned directly -- shortening by too much or too
    // little would serve the wrong bytes silently, at an offset the client never
    // sees questioned.
    {
        let mut r = Render::new();
        r.push(b"0123456789");
        r.drop_prefix(0);
        if r.bytes() != b"0123456789" {
            return Err("drop_prefix(0) moved bytes");
        }
        r.drop_prefix(4);
        if r.bytes() != b"456789" {
            return Err("drop_prefix mid-buffer");
        }
        r.drop_prefix(6);
        if !r.bytes().is_empty() {
            return Err("drop_prefix of the whole buffer");
        }
        let mut r2 = Render::new();
        r2.push(b"abc");
        r2.drop_prefix(9999); // past the end: EOF, never an underflow
        if !r2.bytes().is_empty() {
            return Err("drop_prefix past the end");
        }
    }
    // End to end through render, on a node whose content is a constant.
    if render(N_OSTYPE, &dead, 2).bytes() != b"nux\n" {
        return Err("render ignored the offset");
    }
    if !render(N_OSTYPE, &dead, 6).bytes().is_empty() {
        return Err("render past the end was not EOF");
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
    let up = render(N_UPTIME, &dead, 0);
    if up.bytes().is_empty() {
        return Err("uptime empty");
    }

    // --- V-4c-1: the /sys tree.
    if walk_child(N_SYSFS, b"devices") != Some(N_SYSFS_DEVICES)
        || walk_child(N_SYSFS_DEVICES, b"system") != Some(N_SYSFS_SYSTEM)
        || walk_child(N_SYSFS_SYSTEM, b"cpu") != Some(N_SYSFS_CPU)
    {
        return Err("walk /sys/devices/system/cpu");
    }
    if walk_child(N_SYSFS_CPU, b"online") != Some(N_CPU_ONLINE)
        || walk_child(N_SYSFS_CPU, b"present") != Some(N_CPU_PRESENT)
        || walk_child(N_SYSFS_CPU, b"possible") != Some(N_CPU_POSSIBLE)
    {
        return Err("walk /sys/.../cpu/{online,present,possible}");
    }
    if walk_child(N_SYSFS_CPU, b"..") != Some(N_SYSFS_SYSTEM) {
        return Err("walk /sys/.../cpu/..");
    }
    // kernel_max is DELIBERATELY absent: Linux sources it from a compile-time
    // NR_CPUS, and Thylacine's equivalent (DTB_MAX_CPUS) is not on any surface
    // EL0 can read. Omitting beats reporting the present count under a name that
    // means something else -- section 6.15's rule, applied to the one file here
    // that has no source.
    if walk_child(N_SYSFS_CPU, b"kernel_max").is_some() {
        return Err("kernel_max resolved -- it has no native source");
    }

    // --- V-4c-1: the cpu qid family. Same two properties the pid family needs:
    //     the encoding round-trips, and it can never alias a static index or a
    //     pid (which is what stops `cpu3` resolving onto /proc/self or a Proc).
    if qid_cpu(cpu_qid(3)) != 3 || !is_cpu_node(cpu_qid(0)) {
        return Err("cpu qid round-trip");
    }
    for i in 0..N_COUNT {
        if is_cpu_node(i) {
            return Err("a static node looked like a cpu node");
        }
    }
    if is_cpu_node(pid_qid(1, PK_DIR)) || is_pid_node(cpu_qid(7)) {
        return Err("the cpu and pid ranges overlap");
    }
    if !node_is_dir(cpu_qid(1)) {
        return Err("a cpuN node must be a dir");
    }
    // V-4c-2c: the cache chain now resolves -- V-4c-1 left the dir empty
    // BECAUSE the values had no EL0 source, and section 6.17 gave the kernel
    // one. The unsourced-child property still holds for everything else.
    let cache = walk_child(cpu_qid(0), b"cache").ok_or("walk cpuN/cache")?;
    let index0 = walk_child(cache, b"index0").ok_or("walk cpuN/cache/index0")?;
    let linesz =
        walk_child(index0, b"coherency_line_size").ok_or("walk coherency_line_size")?;
    if !node_is_dir(cache) || !node_is_dir(index0) || node_is_dir(linesz) {
        return Err("cache chain: dirs down to index0, then one leaf");
    }
    // Distinct qids: a collision would alias two files onto one identity.
    if cache == index0 || index0 == linesz || cache == linesz || cache == cpu_qid(0) {
        return Err("cache chain qids collide");
    }
    // Every level still belongs to CPU 0 and stays inside the cpu range.
    for q in [cache, index0, linesz] {
        if !is_cpu_node(q) || qid_cpu(q) != 0 || is_pid_node(q) {
            return Err("cache chain escaped the cpu qid range");
        }
    }
    // CPU 1's chain is a DIFFERENT chain -- the per-CPU property the whole
    // subtree exists for (a heterogeneous board has per-core line sizes).
    if walk_child(cpu_qid(1), b"cache") == Some(cache) {
        return Err("cpu1 cache aliased cpu0's");
    }
    if walk_child(cpu_qid(0), b"online").is_some() || walk_child(cache, b"cache").is_some() {
        return Err("a cpuN dir served an unsourced child");
    }
    // `..` climbs the chain rather than short-cutting to .../system/cpu, which
    // was correct only while cpuN was a leaf dir.
    if walk_child(cpu_qid(0), b"..") != Some(N_SYSFS_CPU) {
        return Err("walk cpuN/..");
    }
    if walk_child(cache, b"..") != Some(cpu_qid(0)) || walk_child(index0, b"..") != Some(cache) {
        return Err("cache chain .. skipped a level");
    }
    // V-4c-3 SA-2 (#71): the LEAF is a file, so NOTHING resolves from it -- `..`
    // and `.` included. This is the leg that changed: the cache leaf used to be
    // the one node in the tree from which `..` walked, because the cpu arm had no
    // is-a-dir gate while the pid and static arms did.
    if walk_child(linesz, b"..").is_some() || walk_child(linesz, b".").is_some() {
        return Err("a file served .. or .");
    }
    // The parent relation itself still holds for every kind including the leaf.
    // It is checked HERE rather than through walk_child precisely because the
    // gate now hides it: a `_ =>` catch-all would answer .../system/cpu from the
    // leaf and skip two levels, and nothing else would notice.
    if cpu_parent(0, CK_CACHE) != cpu_qid(0)
        || cpu_parent(0, CK_INDEX0) != cache
        || cpu_parent(0, CK_LINESZ) != index0
        || cpu_parent(0, CK_DIR) != N_SYSFS_CPU
    {
        return Err("cpu_parent skipped a level");
    }

    // --- V-4c-1: parse_cpu_name is the resolution gate, like parse_pid. Unlike
    //     parse_pid, 0 is VALID here -- cpu0 is a real CPU.
    if parse_cpu_name(b"cpu0") != Some(0) || parse_cpu_name(b"cpu12") != Some(12) {
        return Err("parse_cpu_name decimal");
    }
    if parse_cpu_name(b"cpu").is_some()
        || parse_cpu_name(b"cpux").is_some()
        || parse_cpu_name(b"cpu1x").is_some()
        || parse_cpu_name(b"CPU0").is_some()
        || parse_cpu_name(b"cpu01").is_some()
        || parse_cpu_name(b"cpu256").is_some()
        || parse_cpu_name(b"").is_some()
    {
        return Err("parse_cpu_name accepted garbage");
    }

    // --- V-4c-1: the /ctl/cpu parse. The kernel's `offline` marker (prowl-5 F2)
    //     is what separates Linux's `online` set from its `present` set, so the
    //     two files below disagree for a REASON that is sourced, not invented.
    {
        let ctl = b"cpus: 4\ncpu idle_ns capacity\n0 123 1024\n1 offline\n2 456 1024\n3 789 1024\n";
        if parse_cpu_count(ctl) != 4 {
            return Err("cpu count parse");
        }
        let mut set = [false; 4];
        if parse_cpu_online(ctl, &mut set) != 4 {
            return Err("cpu online row count");
        }
        if set != [true, false, true, true] {
            return Err("cpu online mask");
        }
        // The list renderer emits Linux's cpulist runs.
        let mut r = Render::new();
        push_cpu_list(&mut r, &set);
        if r.bytes() != b"0,2-3\n" {
            return Err("cpulist runs");
        }
        let mut r2 = Render::new();
        push_cpu_list(&mut r2, &[true, true, true, true]);
        if r2.bytes() != b"0-3\n" {
            return Err("cpulist single run");
        }
        let mut r3 = Render::new();
        push_cpu_list(&mut r3, &[true]);
        if r3.bytes() != b"0\n" {
            return Err("cpulist single cpu");
        }
        // An empty set renders NOTHING -- not "0", which a consumer would read
        // as one CPU. This is the unreadable-source path.
        let mut r4 = Render::new();
        push_cpu_list(&mut r4, &[false, false]);
        if !r4.bytes().is_empty() {
            return Err("cpulist empty set must render empty");
        }
    }
    // FAIL-SAFE: a truncated render leaves un-rowed CPUs alone, so this can
    // never promote a CPU to online without a row saying so.
    {
        let truncated = b"cpus: 4\ncpu idle_ns capacity\n0 123 1024\n";
        let mut set = [false; 4];
        parse_cpu_online(truncated, &mut set);
        if set != [true, false, false, false] {
            return Err("truncated /ctl/cpu invented an online CPU");
        }
        // A header-only render yields nothing at all.
        let mut none = [false; 4];
        if parse_cpu_online(b"cpus: 4\ncpu idle_ns capacity\n", &mut none) != 0 {
            return Err("header row parsed as a CPU");
        }
    }
    // An unreadable source renders an EMPTY file rather than a misleading count.
    if parse_cpu_count(b"") != 0 || parse_cpu_count(b"garbage\n") != 0 {
        return Err("cpu count from a bad source");
    }

    Ok(())
}
