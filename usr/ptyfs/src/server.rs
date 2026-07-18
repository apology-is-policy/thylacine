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
// The master writes INPUT toward the slave (m2s); the slave writes OUTPUT toward
// the master (s2m). A read on an empty-but-open ring DEFERS (the netd
// PendingRead / Disp::Deferred / poll_reads mechanism -- a multi-waiter SET, not
// the cons single-reader).
//
// The per-pts line discipline (PTY-2b -- the de-globalized LS-8 cooking, the
// kernel cons.c reference algorithm reimplemented per-pts): a master write runs
// the INPUT cook (ICRNL -> ISIG -> ICANON | raw; echo toward the master through
// the OUTPUT transform, gated at the single echo() chokepoint -- the ECHO-off
// hard no-leak guarantee); a slave write runs the OUTPUT cook (ONLCR). ISIG
// raises via the pts-scoped SYS_TTY_SIGNAL -- the kernel resolves
// pts -> ct_sid -> fg_pgid; ptyfs can never name a process group (I-1/I-22).
// A fresh pts is FULL COOKED (TIO_DEFAULT -- the Linux fresh-pts posture); the
// per-pts ctl surface to change it is PTY-2c.
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
    t_close, t_open, t_pty_register, t_tty_signal, t_walk_create, T_GID_SYSTEM, T_OPATH, T_OREAD,
    T_PRINCIPAL_SYSTEM, T_PTY_REG_FREE, T_PTY_REG_MINT, T_PTY_REG_SLAVE, T_TTY_SIG_HUP,
    T_TTY_SIG_INT, T_TTY_SIG_QUIT, T_TTY_SIG_TSTP, T_TTY_SIG_WINCH, T_WALK_OPEN_FROM_ROOT,
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
/// via a short Rwrite (the writer retries) -- no byte is dropped -- EXCEPT the
/// cooked-input flush + the echo, which drop on full (tty input overrun / the
/// best-effort echo; the kernel cons drop-on-full posture -- see master_write).
const RING_CAP: usize = 4096;

// =============================================================================
// The line discipline (PTY-2b). The five LS-8 flags -- the kernel cons.c bit
// values, so the 2c per-pts ctl grammar speaks the same +name/-name set.
// =============================================================================

const TIO_ICANON: u32 = 0x01;
const TIO_ECHO: u32 = 0x02;
const TIO_ISIG: u32 = 0x04;
const TIO_ICRNL: u32 = 0x08;
const TIO_ONLCR: u32 = 0x10;

/// A fresh pts is FULL COOKED (the Linux fresh-pts default; PTY-DESIGN section 5
/// "CONS_ICANON-default"). The kernel CONSOLE boots ISIG-only, but that is a
/// boot-console posture -- a pts serves a terminal emulator + a shell, where
/// cooked is the POSIX expectation. The 2c ctl flips any flag per-pts.
const TIO_DEFAULT: u32 = TIO_ICANON | TIO_ECHO | TIO_ISIG | TIO_ICRNL | TIO_ONLCR;

/// ICANON line-assembly bound (the kernel CONS_LINE_MAX). Overflow drops the
/// byte un-echoed (the cons contract); NL always flushes what is assembled.
const LINE_MAX: usize = 256;

/// The cooked signal classes collected per master write, as a 3-bit SET
/// (bit `class-1`: INT->b0, QUIT->b1, TSTP->b2). A set, not an array: it
/// dedups repeats on collect (the kernel note-bit delivery dedups anyway)
/// AND can never overflow -- so a distinct class is never lost behind a
/// same-class run (PTY-2e audit F2). Raised in ascending class order.
fn sig_class_bit(class: u64) -> u8 {
    if class >= 1 && class <= 3 {
        1u8 << (class - 1)
    } else {
        0
    }
}

// The cooked control characters (the standard VINTR/VQUIT/VSUSP set -- the
// kernel console cooks only Ctrl-C, a boot-console limit, not the pty contract;
// the kernel signal classes INT/QUIT/TSTP exist for exactly this ldisc).
const CH_INTR: u8 = 0x03; // Ctrl-C  -> TTY_SIG_INT
const CH_QUIT: u8 = 0x1c; // Ctrl-\  -> TTY_SIG_QUIT
const CH_SUSP: u8 = 0x1a; // Ctrl-Z  -> TTY_SIG_TSTP
const CH_BS: u8 = 0x08;
const CH_DEL: u8 = 0x7f;
const CH_NL: u8 = 0x0a;
const CH_CR: u8 = 0x0d;

// The per-pts ctl grammar (PTY-2c; the LS-8b consctl grammar + the winsize op).
const CTL_FLAGS: [(&[u8], u32); 5] = [
    (b"icanon", TIO_ICANON),
    (b"echo", TIO_ECHO),
    (b"isig", TIO_ISIG),
    (b"icrnl", TIO_ICRNL),
    (b"onlcr", TIO_ONLCR),
];
/// Render bound: 5 "+name " tokens (34) + "winsize " + two u16 decimals + NL.
const CTL_RENDER_MAX: usize = 64;
/// Ops per ctl write (bounded parse; more tokens than this rejects).
const CTL_MAX_OPS: usize = 16;
/// Winsize band (the Linux unsigned-short winsize).
const WINSZ_MAX: u32 = 65535;

#[derive(Copy, Clone)]
enum CtlOp {
    Flag(u32, bool), // (bit, set)
    Winsize(u32, u32),
}

fn put_bytes(out: &mut [u8], w: &mut usize, s: &[u8]) {
    if *w + s.len() <= out.len() {
        out[*w..*w + s.len()].copy_from_slice(s);
        *w += s.len();
    }
}

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
// The master + slave + ctl endpoints are read/write byte channels. HONEST
// v1.0 POSTURE (PTY-2e audit F1/F5): the mode is 0666 SYSTEM-owned, so slave
// and ctl I/O is gated ONLY by this world-rw mode -- the kernel dev9p
// perm_check passes any principal as "other" with rw. The pts registry gates
// ONLY the controlling-terminal syscalls (SYS_TTY_ACQUIRE/SET_FG/CONT), NOT
// slave/ctl open/read/write. So on the shared /dev/pts mount, any Proc that
// can name a live pts can read/inject/re-termios it -- an I-1 gap that is
// inert at single-session v1.0 but goes live under A-5b concurrent multi-user
// (task #13: per-pts owner + 0600, the Unix pts model, needs per-session
// submounts or per-op principal forwarding since the shared kernel mount
// arrives as SYSTEM).
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
// (n<<8) is never a valid endpoint (defensive). FK_CTL is the per-pts ctl file
// /dev/pts/<n>ctl (PTY-2c; the Plan 9 suffix-ctl idiom -- eia0/eia0ctl -- so
// the flat Linux-devpts slave names stay POSIX-intact): the termios grammar +
// winsize, server-served and kernel-opaque (they carry no security routing --
// PTY-DESIGN section 4). A ctl fid holds the slot LIVE (refs) but is NOT an
// EOF-counted endpoint (n_master/n_slave count master/slave DATA fds only).
const FK_MASTER: u64 = 1;
const FK_SLAVE: u64 = 2;
const FK_CTL: u64 = 3;

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

fn is_ctl_path(path: u64) -> bool {
    is_pts(path) && pts_filekind(path) == FK_CTL
}

/// A master/slave DATA endpoint -- the opened-fd EOF counts (n_master/n_slave)
/// track exactly these. A ctl fid is NOT an endpoint: gating every open_dec on
/// this keeps an opened ctl from corrupting the peer-closed EOF signal.
fn is_endpoint_path(path: u64) -> bool {
    is_pts(path) && (pts_filekind(path) == FK_MASTER || pts_filekind(path) == FK_SLAVE)
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
    /// n_master == 0 (the master fully closed); a master read when n_slave == 0
    /// -- but only once a slave has EVER opened (`slave_opened_once`): a master
    /// read before the slave side comes up PARKS, not EOF (the Linux
    /// master-blocks-until-the-slave-opens semantic -- EOF means the slave side
    /// is GONE, which requires it to have existed; without the latch, an
    /// emulator that reads for the child's first output races the child's
    /// slave open and gets a spurious 0). The master needs no latch: it is
    /// born open (the mint IS its open), so n_master == 0 implies it existed.
    n_master: u32,
    n_slave: u32,
    slave_opened_once: bool,
    /// The per-pts line discipline (PTY-2b): the five-flag termios word + the
    /// ICANON assembly line + the signal classes the input cook collected
    /// (drained by h_write, which raises them via SYS_TTY_SIGNAL -- keeping the
    /// syscall OUT of the pure cook, so the selftest asserts classes directly).
    tio: u32,
    line: [u8; LINE_MAX],
    line_len: usize,
    /// The cooked signal classes the input cook collected, as a 3-bit set
    /// (see `sig_class_bit`). Drained + raised by h_write's master arm.
    sig_set: u8,
    /// Per-pts winsize (PTY-2c). 0x0 until the emulator sets it (the Linux
    /// fresh-pts posture; openpty sets it at once in practice).
    winsz_cols: u32,
    winsz_rows: u32,
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
            slave_opened_once: false,
            tio: TIO_DEFAULT,
            line: [0; LINE_MAX],
            line_len: 0,
            sig_set: 0,
            winsz_cols: 0,
            winsz_rows: 0,
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

    /// A master/slave fd was OPENED (the EOF-tracking count). A slave open
    /// also latches `slave_opened_once` (the master-read EOF gate).
    fn open_inc(&mut self, n: u32, master: bool) {
        if let Some(p) = self.slot_mut(n) {
            if master {
                p.n_master += 1;
            } else {
                p.n_slave += 1;
                p.slave_opened_once = true;
            }
        }
    }

    /// An opened master/slave fd was clunked. Returns true on the LAST-master
    /// edge (n_master 1 -> 0) -- the carrier-loss event the caller answers
    /// with TTY_SIG_HUP (PTY-2d). A slave close is never a hup edge (POSIX:
    /// no SIGHUP on slave close -- the master just reads EOF).
    fn open_dec(&mut self, n: u32, master: bool) -> bool {
        if let Some(p) = self.slot_mut(n) {
            if master {
                let was = p.n_master;
                p.n_master = was.saturating_sub(1);
                return was == 1;
            }
            p.n_slave = p.n_slave.saturating_sub(1);
        }
        false
    }

    /// Push bytes onto a ring up to RING_CAP; returns the count accepted (a short
    /// push is back-pressure at the raw/output writers; the cooked-input flush +
    /// the echo treat a short push as a DROP -- see master_write / echo).
    fn ring_push(ring: &mut VecDeque<u8>, data: &[u8]) -> usize {
        let room = RING_CAP.saturating_sub(ring.len());
        let k = room.min(data.len());
        ring.extend(&data[..k]);
        k
    }

    /// The single echo chokepoint (PTY-2b): EVERY echo staging passes here,
    /// gated on ECHO -- the LS-8b ECHO-off hard no-leak guarantee, per-pts:
    /// clear means NOTHING reaches the master, cooked erase/redraw included
    /// (the password mask). An echoed byte rides the OUTPUT transform (ONLCR;
    /// echo IS output, the Linux model) into s2m, drop-on-full (echo is
    /// best-effort -- a keystroke echo cannot back-pressure the writer).
    fn echo(p: &mut Pts, b: u8) {
        if p.tio & TIO_ECHO == 0 {
            return;
        }
        if b == CH_NL && p.tio & TIO_ONLCR != 0 {
            let _ = Ptys::ring_push(&mut p.s2m, b"\r\n");
        } else {
            let _ = Ptys::ring_push(&mut p.s2m, &[b]);
        }
    }

    /// The INPUT line discipline (a master write = the terminal emulator's
    /// keystrokes) -- the kernel cons_rx_input algorithm per-pts, in the LS-8
    /// order: ICRNL first -> ISIG (collect the class + CONSUME the byte: never
    /// enqueued, never echoed -- pty.tla CookSignal / SignalXorByte) -> ICANON
    /// (erase / line assembly / NL flush, the line INCLUDING its newline) ->
    /// raw (pty.tla CookData). Echo goes toward the master via echo().
    ///
    /// Returns the count of input bytes CONSUMED (the Rwrite count). Cooked
    /// (ICANON) consumes everything offered: an assembled byte past LINE_MAX
    /// drops un-echoed (tty input overrun, the cons contract), and a line
    /// flush into a full m2s drops the tail (the slave has RING_CAP unread --
    /// a wedged reader; the console reference drops likewise). Raw
    /// back-pressures instead: a full m2s stops consumption and returns the
    /// short count (the 2a contract, unchanged).
    fn master_write(&mut self, n: u32, data: &[u8]) -> usize {
        let p = match self.slot_mut(n) {
            Some(p) => p,
            None => return 0,
        };
        let mut consumed = 0usize;
        for &raw in data {
            let mut b = raw;
            // 1. ICRNL: CR -> NL on input.
            if b == CH_CR && p.tio & TIO_ICRNL != 0 {
                b = CH_NL;
            }
            // 2. ISIG: a signal-class control char raises + CONSUMES (no byte
            //    reaches the slave, no echo -- SignalXorByte).
            if p.tio & TIO_ISIG != 0 {
                let class: u64 = match b {
                    CH_INTR => T_TTY_SIG_INT,
                    CH_QUIT => T_TTY_SIG_QUIT,
                    CH_SUSP => T_TTY_SIG_TSTP,
                    _ => 0,
                };
                if class != 0 {
                    p.sig_set |= sig_class_bit(class); // dedup-on-collect; never overflows
                    consumed += 1;
                    continue;
                }
            }
            if p.tio & TIO_ICANON != 0 {
                // 3. ICANON line assembly.
                if b == CH_DEL || b == CH_BS {
                    // Erase: drop the last unflushed byte; echo "\b \b".
                    if p.line_len > 0 {
                        p.line_len -= 1;
                        Ptys::echo(p, CH_BS);
                        Ptys::echo(p, b' ');
                        Ptys::echo(p, CH_BS);
                    }
                } else if b == CH_NL {
                    // NL: the line INCLUDING its newline flushes to the slave
                    // (drop-on-full -- see above); echo the NL (ONLCR-aware).
                    let len = p.line_len;
                    let _ = Ptys::ring_push(&mut p.m2s, &p.line[..len]);
                    let _ = Ptys::ring_push(&mut p.m2s, &[CH_NL]);
                    p.line_len = 0;
                    Ptys::echo(p, CH_NL);
                } else if p.line_len < LINE_MAX {
                    p.line[p.line_len] = b;
                    p.line_len += 1;
                    Ptys::echo(p, b);
                }
                // else: overflow -- dropped, NOT echoed (the cons contract).
                consumed += 1;
            } else {
                // 4. Raw: straight to the slave; back-pressure on full (the
                //    byte is NOT consumed -- the writer retries).
                if Ptys::ring_push(&mut p.m2s, &[b]) == 0 {
                    break;
                }
                Ptys::echo(p, b);
                consumed += 1;
            }
        }
        consumed
    }

    /// The OUTPUT line discipline (a slave write = the application's output):
    /// ONLCR expands NL -> CR NL toward the master (pty.tla SlaveWrite).
    /// Back-pressure: an expansion that does not fully fit stops BEFORE its
    /// input byte and returns the short count of INPUT bytes consumed (never a
    /// torn CR-NL pair -- a retry would double the CR).
    fn slave_write(&mut self, n: u32, data: &[u8]) -> usize {
        let p = match self.slot_mut(n) {
            Some(p) => p,
            None => return 0,
        };
        let mut consumed = 0usize;
        for &b in data {
            if b == CH_NL && p.tio & TIO_ONLCR != 0 {
                if RING_CAP.saturating_sub(p.s2m.len()) < 2 {
                    break;
                }
                let _ = Ptys::ring_push(&mut p.s2m, b"\r\n");
            } else if Ptys::ring_push(&mut p.s2m, &[b]) == 0 {
                break;
            }
            consumed += 1;
        }
        consumed
    }

    /// Drain the collected signal-class SET (bits per `sig_class_bit`). h_write
    /// raises the set members via the pts-scoped SYS_TTY_SIGNAL AFTER the ring
    /// work (the syscall stays out of the pure cook, so the selftest asserts
    /// the set directly -- its local pts has no kernel entry to signal).
    fn take_sigs(&mut self, n: u32) -> u8 {
        match self.slot_mut(n) {
            Some(p) => {
                let out = p.sig_set;
                p.sig_set = 0;
                out
            }
            None => 0,
        }
    }

    /// Set the per-pts termios word (the 2c ctl surface + the selftest). A mode
    /// change resets the ICANON assembly line (the TCSAFLUSH posture the kernel
    /// consctl apply carries).
    fn set_tio(&mut self, n: u32, tio: u32) {
        if let Some(p) = self.slot_mut(n) {
            p.tio = tio;
            p.line_len = 0;
        }
    }

    /// Render the per-pts ctl read-back: the five +/-name tokens (the kernel
    /// consctl order) + the winsize, one line -- e.g.
    /// "+icanon +echo +isig +icrnl +onlcr winsize 80 24\n". Returns the length
    /// written (out should hold CTL_RENDER_MAX).
    fn ctl_render(&self, n: u32, out: &mut [u8]) -> usize {
        let p = match self.slot(n) {
            Some(p) => p,
            None => return 0,
        };
        let mut w = 0usize;
        for (name, bit) in CTL_FLAGS {
            put_bytes(out, &mut w, if p.tio & bit != 0 { b"+" } else { b"-" });
            put_bytes(out, &mut w, name);
            put_bytes(out, &mut w, b" ");
        }
        put_bytes(out, &mut w, b"winsize ");
        let mut d = [0u8; 12];
        let l = fmt_dec(p.winsz_cols, &mut d).len();
        put_bytes(out, &mut w, &d[..l]);
        put_bytes(out, &mut w, b" ");
        let l = fmt_dec(p.winsz_rows, &mut d).len();
        put_bytes(out, &mut w, &d[..l]);
        put_bytes(out, &mut w, b"\n");
        w
    }

    /// Parse + apply a ctl write (the tcsetattr-atomic posture: ALL tokens are
    /// validated BEFORE ANY is applied -- one malformed token rejects the whole
    /// write with the mode unchanged, the kernel consctl contract). Grammar:
    /// whitespace-separated tokens; "+name"/"-name" over the five LS-8 flags;
    /// "winsize <cols> <rows>" (decimal, <= 65535; canonical -- no leading
    /// zeros). Ops apply in order; a flag change resets the ICANON assembly
    /// (TCSAFLUSH). Returns Ok(winsize_changed) -- the caller raises
    /// TTY_SIG_WINCH iff the size actually changed (the Linux TIOCSWINSZ
    /// behavior); Err(()) on any malformed token.
    fn ctl_apply(&mut self, n: u32, data: &[u8]) -> Result<bool, ()> {
        // Tokenize (bounded).
        let mut toks: [&[u8]; CTL_MAX_OPS + 8] = [b""; CTL_MAX_OPS + 8];
        let mut ntok = 0usize;
        for t in data.split(|&b| b == b' ' || b == b'\t' || b == b'\r' || b == b'\n') {
            if t.is_empty() {
                continue;
            }
            if ntok >= toks.len() {
                return Err(());
            }
            toks[ntok] = t;
            ntok += 1;
        }
        // Pass 1: validate ALL into ops.
        let mut ops: [CtlOp; CTL_MAX_OPS] = [CtlOp::Flag(0, false); CTL_MAX_OPS];
        let mut nops = 0usize;
        let mut i = 0usize;
        while i < ntok {
            let t = toks[i];
            let op = if t == b"winsize" {
                if i + 3 > ntok {
                    return Err(()); // arity: needs two following tokens
                }
                let c = parse_dec(toks[i + 1]).ok_or(())?;
                let r = parse_dec(toks[i + 2]).ok_or(())?;
                if c > WINSZ_MAX || r > WINSZ_MAX {
                    return Err(());
                }
                i += 2;
                CtlOp::Winsize(c, r)
            } else if t.len() > 1 && (t[0] == b'+' || t[0] == b'-') {
                let set = t[0] == b'+';
                let name = &t[1..];
                let mut bit = 0u32;
                for (fname, fbit) in CTL_FLAGS {
                    if name == fname {
                        bit = fbit;
                        break;
                    }
                }
                if bit == 0 {
                    return Err(()); // unknown flag name
                }
                CtlOp::Flag(bit, set)
            } else {
                return Err(()); // unknown token shape
            };
            if nops >= CTL_MAX_OPS {
                return Err(());
            }
            ops[nops] = op;
            nops += 1;
            i += 1;
        }
        // Pass 2: apply in order.
        let p = self.slot_mut(n).ok_or(())?;
        let mut flags_touched = false;
        let mut winch = false;
        for op in ops.iter().take(nops) {
            match *op {
                CtlOp::Flag(bit, set) => {
                    if set {
                        p.tio |= bit;
                    } else {
                        p.tio &= !bit;
                    }
                    flags_touched = true;
                }
                CtlOp::Winsize(c, r) => {
                    if (c, r) != (p.winsz_cols, p.winsz_rows) {
                        p.winsz_cols = c;
                        p.winsz_rows = r;
                        winch = true;
                    }
                }
            }
        }
        if flags_touched {
            p.line_len = 0; // TCSAFLUSH: a mode change resets the assembly
        }
        Ok(winch)
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
    /// GONE (opened once, now fully closed) -> Eof; empty + slave open OR not
    /// yet arrived -> WouldBlock (the master blocks until the slave side comes
    /// up -- see the slave_opened_once field note).
    fn master_read(&mut self, n: u32, buf: &mut [u8]) -> RecvOutcome {
        match self.slot_mut(n) {
            Some(p) => {
                let other = if p.slave_opened_once { p.n_slave } else { 1 };
                Ptys::ring_drain(&mut p.s2m, buf, other)
            }
            None => RecvOutcome::Eof,
        }
    }

    /// Peek whether a parked read on this endpoint would complete NOW (Data or
    /// Eof) vs WouldBlock -- so poll_reads only allocates a drain buffer when
    /// there is something to deliver (a long-parked read otherwise churns the
    /// allocator every 1 s poll; PTY-2e audit note). A pure read mirror of the
    /// master_read/slave_read ring + EOF logic (which stay authoritative for
    /// the actual drain).
    fn read_ready(&self, n: u32, master: bool) -> bool {
        match self.slot(n) {
            Some(p) => {
                let (ring, other_open) = if master {
                    (&p.s2m, if p.slave_opened_once { p.n_slave } else { 1 })
                } else {
                    (&p.m2s, p.n_master)
                };
                !ring.is_empty() || other_open == 0
            }
            None => true, // slot gone -> the drain returns Eof; deliver it
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

    /// Resolve one walk step. A pts slave/ctl resolves only while its slot is
    /// LIVE, so a stale/forged qid is unreachable (the netd live-slot property).
    fn walk_child(&self, cur: u64, name: &[u8]) -> Option<u64> {
        if cur != P_ROOT {
            return None; // only the devpts root has children
        }
        if name == b"ptmx" {
            return Some(P_PTMX);
        }
        // "<n>ctl" resolves to the live per-pts ctl (the suffix-ctl idiom).
        if name.len() > 3 && name.ends_with(b"ctl") {
            let n = parse_dec(&name[..name.len() - 3])?;
            return if self.live(n) {
                Some(make_pts(n, FK_CTL))
            } else {
                None
            };
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
                let len = fmt_dec(n, &mut buf).len();
                f(&buf[..len], make_pts(n, FK_SLAVE), false);
                // The suffix-ctl sibling: "<n>ctl".
                let mut cbuf = [0u8; 15];
                cbuf[..len].copy_from_slice(&buf[..len]);
                cbuf[len..len + 3].copy_from_slice(b"ctl");
                f(&cbuf[..len + 3], make_pts(n, FK_CTL), false);
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

    /// Close an OPENED endpoint fid's count -- and on the LAST-master edge
    /// raise TTY_SIG_HUP (PTY-2d: carrier loss; the kernel routes tty:hup
    /// dual-target to the fg pgrp + the session leader -- PTY-1d). Reached
    /// from every opened-endpoint drop: the normal clunk, a connection
    /// teardown, and a Tversion reset (a dying emulator connection IS carrier
    /// loss). HupAtMostOnce is BY CONSTRUCTION: exactly one master fd per pts
    /// can ever exist (masters are mint-only -- no walk resolves a master
    /// path, 9P forbids walking FROM an opened fid so the master fid cannot
    /// be cloned, and a walk to an existing newfid is rejected) -- so the
    /// n_master 1 -> 0 edge fires at most once per pts lifetime. The raise
    /// PRECEDES the caller's unref (the slot + pts_id must still be live).
    /// A ctl fid is not an endpoint: no count, no edge.
    fn close_endpoint(ptys: &mut Ptys, path: u64) {
        if !is_endpoint_path(path) {
            return;
        }
        let n = pts_n(path);
        if ptys.open_dec(n, is_master_path(path)) {
            let pid = ptys.pts_id(n);
            if pid > 0 {
                unsafe {
                    let _ = t_tty_signal(pid as u64, T_TTY_SIG_HUP);
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
                if f.opened {
                    Conn::close_endpoint(ptys, f.path);
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
            if old.opened {
                Conn::close_endpoint(ptys, old.path);
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
            if f.opened {
                Conn::close_endpoint(ptys, f.path);
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
                if f.opened {
                    Conn::close_endpoint(ptys, f.path);
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
        } else if is_ctl_path(f.path) {
            // The per-pts ctl (/pts/<n>ctl): opens plainly -- no registration,
            // no EOF count (a ctl fid is not an endpoint); the bound fid
            // already holds the slot ref.
            let q = self.qid_of(ptys, f.path);
            let mut nf = f;
            nf.opened = true;
            self.fids[i] = Some(nf);
            p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
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
        if is_ctl_path(f.path) {
            // The ctl read-back: a small offset-served file (the kernel consctl
            // read shape) -- NOT a stream, never defers.
            let mut lb = [0u8; CTL_RENDER_MAX];
            let len = ptys.ctl_render(pts_n(f.path), &mut lb);
            let off = a.offset as usize;
            if off >= len {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            let k = (len - off).min(a.count as usize);
            return p9::build_rread(&mut self.out_buf, tag, &lb[off..off + k]);
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
        if is_ctl_path(f.path) {
            // The ctl write (PTY-2c): the atomic grammar apply. A winsize
            // CHANGE raises TTY_SIG_WINCH via the pts-scoped seam -- the
            // kernel routes tty:winch to the fg pgrp (ptyfs never names one).
            let n = pts_n(f.path);
            return match ptys.ctl_apply(n, a.data) {
                Ok(winch) => {
                    if winch {
                        let pid = ptys.pts_id(n);
                        if pid > 0 {
                            unsafe {
                                let _ = t_tty_signal(pid as u64, T_TTY_SIG_WINCH);
                            }
                        }
                    }
                    p9::build_rwrite(&mut self.out_buf, tag, a.data.len() as u32)
                }
                Err(()) => self.err(tag, p9::E_INVAL),
            };
        }
        let n = pts_n(f.path);
        // PTY-2b: a master write runs the INPUT cook (toward m2s + the echo);
        // a slave write runs the OUTPUT cook (ONLCR toward s2m).
        let pushed = if is_master_path(f.path) {
            let consumed = ptys.master_write(n, a.data);
            // Raise the collected signal-class SET via the pts-scoped seam: the
            // KERNEL resolves pts -> ct_sid -> fg_pgid and routes -- ptyfs can
            // never name a process group (the I-1/I-22 bound). pts_id 0 = a
            // local/unregistered pts (the selftest) -- nothing to signal.
            // Ascending class order (INT, QUIT, TSTP).
            let sig_set = ptys.take_sigs(n);
            let pts_id = ptys.pts_id(n);
            if pts_id > 0 && sig_set != 0 {
                for class in [T_TTY_SIG_INT, T_TTY_SIG_QUIT, T_TTY_SIG_TSTP] {
                    if sig_set & sig_class_bit(class) != 0 {
                        unsafe {
                            let _ = t_tty_signal(pts_id as u64, class);
                        }
                    }
                }
            }
            consumed
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
            if !ptys.read_ready(pr.slot_n, pr.master) {
                i += 1;
                continue; // still WouldBlock -- do not allocate a drain buffer
            }
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
// In-server selftest (PTY-2a rings + PTY-2b cooking): deterministic and
// mount-independent (the netd echo_e2e analog). Proves the master/slave byte
// round-trip, the EOF-vs-WouldBlock discipline, and the full ldisc truth table
// without a real client. The 9P server path + the kernel registration are
// exercised by the in-guest /dev/pts boot probe.
// =============================================================================

/// Returns Ok(()) or a stage name on failure.
pub fn selftest() -> Result<(), &'static str> {
    let mut ptys = Ptys::new();
    let n = ptys.mint().ok_or("mint")? as u32;
    // Simulate both ends open (a real pts opens the master via clone + the slave
    // via /pts/<n>).
    ptys.open_inc(n, true); // master open
    ptys.open_inc(n, false); // slave open

    // ---- The RAW battery (tio = 0: the 2a ring semantics, byte-transparent).
    ptys.set_tio(n, 0);

    // Empty + both open -> WouldBlock, not EOF.
    let mut buf = [0u8; 16];
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

    // Raw + no-echo transparency: signal chars + CR are DATA with tio = 0.
    if ptys.master_write(n, &[CH_INTR, CH_CR]) != 2 {
        return Err("raw-transparent-write");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(2) if buf[0] == CH_INTR && buf[1] == CH_CR => {}
        _ => return Err("raw-transparent-read"),
    }
    if ptys.take_sigs(n) != 0 {
        return Err("raw-no-sigs");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {} // no echo staged with ECHO clear
        _ => return Err("raw-no-echo"),
    }

    // ---- The COOKED battery (PTY-2b: the ldisc truth table vs the cons.c
    // reference). set_tio resets the assembly line (the TCSAFLUSH posture).
    ptys.set_tio(n, TIO_DEFAULT);

    // (1) ICRNL + ICANON flush + ECHO + ONLCR in one stroke: "hi\r" -> the CR
    // folds to NL -> the line flushes WITH its newline -> slave "hi\n"; the
    // echo is "hi" + NL-> "\r\n" (echo rides the output transform).
    if ptys.master_write(n, b"hi\r") != 3 {
        return Err("cooked-write");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(3) if &buf[..3] == b"hi\n" => {}
        _ => return Err("cooked-icrnl-flush"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(4) if &buf[..4] == b"hi\r\n" => {}
        _ => return Err("cooked-echo-onlcr"),
    }

    // (2) Assembly holds until NL: no flush -> the slave sees nothing yet.
    if ptys.master_write(n, b"ab") != 2 {
        return Err("assembly-write");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("assembly-no-flush"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"ab" => {} // the echo IS immediate
        _ => return Err("assembly-echo"),
    }

    // (3) Erase: DEL pops the 'b'; the echo is "\b \b"; the flushed line is "ac\n".
    if ptys.master_write(n, &[CH_DEL]) != 1 {
        return Err("erase-write");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(3) if buf[0] == CH_BS && buf[1] == b' ' && buf[2] == CH_BS => {}
        _ => return Err("erase-echo"),
    }
    if ptys.master_write(n, b"c\n") != 2 {
        return Err("erase-refill");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(3) if &buf[..3] == b"ac\n" => {}
        _ => return Err("erase-flush"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(3) if &buf[..3] == b"c\r\n" => {}
        _ => return Err("erase-refill-echo"),
    }

    // (4) Erase on an empty line: a no-op, NOT echoed.
    if ptys.master_write(n, &[CH_DEL]) != 1 {
        return Err("erase-empty-write");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("erase-empty-no-echo"),
    }

    // (5) ISIG: the VINTR/VQUIT/VSUSP trio raises + CONSUMES -- never a byte
    // toward the slave, never an echo (SignalXorByte). The set holds all three.
    if ptys.master_write(n, &[CH_INTR, CH_QUIT, CH_SUSP]) != 3 {
        return Err("isig-write");
    }
    let want = sig_class_bit(T_TTY_SIG_INT)
        | sig_class_bit(T_TTY_SIG_QUIT)
        | sig_class_bit(T_TTY_SIG_TSTP);
    if ptys.take_sigs(n) != want {
        return Err("isig-classes");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("isig-not-a-byte"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("isig-not-echoed"),
    }

    // (5b) F2: a DISTINCT class overflowing behind a same-class run is NOT
    // lost (the set dedups repeats + never overflows). 8x INT then 1x QUIT:
    // the set carries BOTH.
    let mut burst = alloc::vec![CH_INTR; 8];
    burst.push(CH_QUIT);
    if ptys.master_write(n, &burst) != 9 {
        return Err("isig-overflow-write");
    }
    if ptys.take_sigs(n) != (sig_class_bit(T_TTY_SIG_INT) | sig_class_bit(T_TTY_SIG_QUIT)) {
        return Err("isig-overflow-distinct-class-lost");
    }

    // (6) ECHO off: the typed line still cooks toward the slave; NOTHING --
    // data echo or erase echo -- reaches the master (the hard no-leak
    // guarantee, the password mask).
    ptys.set_tio(n, TIO_DEFAULT & !TIO_ECHO);
    if ptys.master_write(n, b"pw") != 2 || ptys.master_write(n, &[CH_DEL]) != 1 {
        return Err("echo-off-write");
    }
    if ptys.master_write(n, b"x\n") != 2 {
        return Err("echo-off-nl");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(3) if &buf[..3] == b"px\n" => {}
        _ => return Err("echo-off-cooks"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::WouldBlock => {}
        _ => return Err("echo-off-no-leak"),
    }

    // (7) Raw + ISIG (the boot-console-style mode): bytes flow transparently,
    // the signal chars still cook.
    ptys.set_tio(n, TIO_ISIG);
    if ptys.master_write(n, &[b'k', CH_INTR]) != 2 {
        return Err("raw-isig-write");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(1) if buf[0] == b'k' => {}
        _ => return Err("raw-isig-data"),
    }
    if ptys.take_sigs(n) != sig_class_bit(T_TTY_SIG_INT) {
        return Err("raw-isig-class");
    }

    // (8) Output ONLCR: the slave's "a\nb" reaches the master as "a\r\nb".
    ptys.set_tio(n, TIO_DEFAULT);
    if ptys.slave_write(n, b"a\nb") != 3 {
        return Err("onlcr-write");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(4) if &buf[..4] == b"a\r\nb" => {}
        _ => return Err("onlcr-read"),
    }

    // (9) Line-overflow: the byte past LINE_MAX drops un-echoed; NL still
    // flushes the assembled LINE_MAX + the newline.
    {
        let fill = alloc::vec![b'A'; LINE_MAX];
        if ptys.master_write(n, &fill) != LINE_MAX {
            return Err("overflow-fill");
        }
        let mut sink = alloc::vec![0u8; LINE_MAX + 8];
        match ptys.master_read(n, &mut sink) {
            RecvOutcome::Data(k2) if k2 == LINE_MAX => {} // the fill echoed
            _ => return Err("overflow-fill-echo"),
        }
        if ptys.master_write(n, b"B") != 1 {
            return Err("overflow-byte"); // consumed...
        }
        match ptys.master_read(n, &mut sink) {
            RecvOutcome::WouldBlock => {} // ...but dropped: NOT echoed
            _ => return Err("overflow-not-echoed"),
        }
        if ptys.master_write(n, b"\n") != 1 {
            return Err("overflow-nl");
        }
        match ptys.slave_read(n, &mut sink) {
            RecvOutcome::Data(k2)
                if k2 == LINE_MAX + 1
                    && sink[LINE_MAX - 1] == b'A'
                    && sink[LINE_MAX] == b'\n' => {}
            _ => return Err("overflow-flush"),
        }
        match ptys.master_read(n, &mut sink) {
            RecvOutcome::Data(2) if &sink[..2] == b"\r\n" => {} // the NL echo
            _ => return Err("overflow-nl-echo"),
        }
    }

    // ---- The ctl battery (PTY-2c: the per-pts termios grammar + winsize).
    ptys.set_tio(n, TIO_DEFAULT);

    // (a) The render format (fresh winsize is 0x0 until set).
    let mut cbuf = [0u8; CTL_RENDER_MAX];
    let l = ptys.ctl_render(n, &mut cbuf);
    if &cbuf[..l] != b"+icanon +echo +isig +icrnl +onlcr winsize 0 0\n" {
        return Err("ctl-render");
    }

    // (b) Atomic reject: one malformed token rejects the WHOLE write (mode
    // unchanged -- the tcsetattr-atomic posture).
    if ptys.ctl_apply(n, b"-echo").is_err() {
        return Err("ctl-apply-simple");
    }
    if ptys.ctl_apply(n, b"+echo +bogus").is_ok() {
        return Err("ctl-atomic-accepted");
    }
    let l = ptys.ctl_render(n, &mut cbuf);
    if !cbuf[..l].starts_with(b"+icanon -echo") {
        return Err("ctl-atomic-mutated"); // the valid +echo must NOT have applied
    }
    if ptys.ctl_apply(n, b"+echo").is_err() {
        return Err("ctl-reapply");
    }

    // (c) winsize: raise iff CHANGED (the Linux TIOCSWINSZ behavior); band +
    // arity + shape rejects.
    match ptys.ctl_apply(n, b"winsize 132 43") {
        Ok(true) => {}
        _ => return Err("winsize-set"),
    }
    match ptys.ctl_apply(n, b"winsize 132 43") {
        Ok(false) => {}
        _ => return Err("winsize-same-no-winch"),
    }
    let l = ptys.ctl_render(n, &mut cbuf);
    if !cbuf[..l].ends_with(b"winsize 132 43\n") {
        return Err("winsize-render");
    }
    if ptys.ctl_apply(n, b"winsize 1").is_ok()
        || ptys.ctl_apply(n, b"winsize 70000 1").is_ok()
        || ptys.ctl_apply(n, b"winsize x 1").is_ok()
    {
        return Err("winsize-reject");
    }

    // (d) Mixed flag + winsize in one atomic write.
    match ptys.ctl_apply(n, b"-onlcr winsize 80 24") {
        Ok(true) => {}
        _ => return Err("ctl-mixed"),
    }
    let l = ptys.ctl_render(n, &mut cbuf);
    if &cbuf[..l] != b"+icanon +echo +isig +icrnl -onlcr winsize 80 24\n" {
        return Err("ctl-mixed-render");
    }
    if ptys.ctl_apply(n, b"+onlcr").is_err() {
        return Err("ctl-restore");
    }

    // (e) A flag apply resets the assembly (TCSAFLUSH): a half-typed line is
    // discarded by the mode change; the following NL flushes an EMPTY line.
    if ptys.master_write(n, b"zz") != 2 {
        return Err("tcsaflush-type");
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(2) => {} // drain the "zz" echo
        _ => return Err("tcsaflush-echo"),
    }
    if ptys.ctl_apply(n, b"+icanon").is_err() {
        return Err("tcsaflush-apply");
    }
    if ptys.master_write(n, b"\n") != 1 {
        return Err("tcsaflush-nl");
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(1) if buf[0] == b'\n' => {} // just the NL: "zz" gone
        _ => return Err("tcsaflush-reset"),
    }
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"\r\n" => {}
        _ => return Err("tcsaflush-nl-echo"),
    }

    // (f) The walk grammar: "<n>ctl" resolves while live; degenerate shapes do
    // not ("ctl" bare, a leading-zero prefix, a suffix mismatch).
    if ptys.walk_child(P_ROOT, b"0ctl") != Some(make_pts(n, FK_CTL)) {
        return Err("walk-ctl"); // n == 0: the fresh-table first mint
    }
    if ptys.walk_child(P_ROOT, b"ctl").is_some()
        || ptys.walk_child(P_ROOT, b"00ctl").is_some()
        || ptys.walk_child(P_ROOT, b"0ctlx").is_some()
    {
        return Err("walk-ctl-degenerate");
    }

    // ---- Back-pressure on a SECOND fresh pts (its own rings; freed after):
    // a write past RING_CAP is a SHORT push, never a drop.
    {
        let b = ptys.mint().ok_or("mint2")? as u32;
        // The slave_opened_once latch (PTY-2e): a master read BEFORE any slave
        // open PARKS (WouldBlock), never EOF -- the emulator's read for the
        // child's first output must not race the child's slave open.
        match ptys.master_read(b, &mut buf) {
            RecvOutcome::WouldBlock => {}
            _ => return Err("master-read-before-slave-not-park"),
        }
        let big = alloc::vec![0x5au8; RING_CAP + 100];
        if ptys.slave_write(b, &big) != RING_CAP {
            return Err("backpressure-short-push");
        }
        ptys.ref_path(make_pts(b, FK_SLAVE));
        if ptys.unref_path(make_pts(b, FK_SLAVE)) != Some(0) {
            return Err("backpressure-free");
        }
    }

    // ---- The teardown tail (PTY-2d; raw so writes are transparent).
    ptys.set_tio(n, 0);

    // Queued bytes SURVIVE the master close: drain-then-EOF (pty.tla
    // TeardownDrainsThenEof) -- the slave reads the queued bytes, THEN Eof.
    // The close is the LAST-master edge (the HUP raise point).
    if ptys.master_write(n, b"XY") != 2 {
        return Err("teardown-queue");
    }
    if !ptys.open_dec(n, true) {
        return Err("hup-edge"); // n_master 1 -> 0 MUST report the edge
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"XY" => {}
        _ => return Err("teardown-drain-first"),
    }
    match ptys.slave_read(n, &mut buf) {
        RecvOutcome::Eof => {}
        _ => return Err("master-close-not-eof"),
    }

    // The edge fires at most once (a saturated re-dec is not an edge), and a
    // slave close is NEVER a hup edge.
    if ptys.open_dec(n, true) {
        return Err("hup-double-edge");
    }
    if ptys.open_dec(n, false) {
        return Err("slave-close-not-hup");
    }
    // Symmetry: with the slave closed, the master's (empty) drain is EOF too.
    match ptys.master_read(n, &mut buf) {
        RecvOutcome::Eof => {}
        _ => return Err("slave-close-not-eof"),
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
