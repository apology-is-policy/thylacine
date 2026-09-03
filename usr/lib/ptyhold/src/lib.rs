//! ptyhold -- the shared PTY master-hold core.
//!
//! The mechanism every pts host performs: mint a pts (the fd IS the master),
//! optionally seed the slave's winsize, and spawn a program on the slave as
//! its fd 0/1/2. Extracted verbatim from `/bin/ptyhost` (PTY-4) so the
//! kaua-term (the Halcyon per-tile terminal, KT-1) reuses the identical
//! master-hold rather than duplicating it. What differs between hosts is the
//! RELAY policy over the master (ptyhost pumps raw bytes to an outer terminal;
//! the kaua-term parses master bytes into a cell stream) -- that stays in each
//! host. This crate holds only the shared mint/seed/spawn.
//!
//! Thematic name `den` is a HELD proposal (PTY-DESIGN section 10); `ptyhold`
//! is the working name until it is surfaced for signoff.

#![no_std]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use libthyla_rs::fs::OpenOptions;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::{t_close, t_fstat, t_open, t_write, T_ORDWR, T_WALK_OPEN_FROM_ROOT};

/// The ptyfs endpoint-qid contract (PTY-DESIGN section 5): PTS_FLAG | N<<8 |
/// filekind; filekind 1 = master.
pub const PTS_FLAG: u64 = 1 << 40;
pub const PTS_FK_MASTER: u64 = 1;

/// A master-hold operation that failed. The caller maps each to its own
/// diagnostic string (ptyhost keeps its `ptyhost:` prefixes); this crate
/// prints nothing so it stays host-agnostic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HoldError {
    /// open(/dev/pts/ptmx) failed -- no master fd was opened.
    Mint,
    /// fstat(master) failed -- the master fd was opened then closed.
    Fstat,
    /// the ptmx qid is not a pts master -- the master fd was opened then closed.
    NotMaster,
    /// open(slave) failed -- the master fd is still open (caller owns it).
    OpenSlave,
    /// Command::spawn failed, or argv was empty -- the master fd is still
    /// open (caller owns it).
    Spawn,
}

/// A minted pty master: the owning master fd plus the pts index N. After
/// [`Master::mint`] succeeds the caller owns `mfd` and must `t_close` it when
/// the session ends (or on any later error) -- neither [`seed_winsize`] nor
/// [`spawn_on_slave`] closes it. `mint`'s OWN failure paths close the fd
/// before returning, so a caller closes `mfd` only after a successful mint.
///
/// [`seed_winsize`]: Master::seed_winsize
/// [`spawn_on_slave`]: Master::spawn_on_slave
pub struct Master {
    pub mfd: i64,
    pub n: u64,
}

fn open_rdwr(path: &str) -> i64 {
    // SAFETY: t_open is the SYS_OPEN SVC wrapper; path is a valid byte slice.
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_ORDWR) }
}

/// Set the winsize of pts `n` via /dev/pts/<n>ctl (best-effort). Standalone so a
/// party that holds only the pts index -- the kaua-term's input thread reacting
/// to a Resize record, not the Master owner -- can set it without a Master.
/// A winsize change raises the kernel's TTY_SIG_WINCH -> SIGWINCH to the fg pgrp.
pub fn set_winsize(n: u64, cols: u16, rows: u16) {
    let ctl_path = format!("/dev/pts/{}ctl", n);
    let ctl = open_rdwr(&ctl_path);
    if ctl >= 0 {
        let ws = format!("winsize {} {}", cols, rows);
        // SAFETY: SVC wrapper; ws is a valid byte buffer for its len.
        let _ = unsafe { t_write(ctl, ws.as_ptr(), ws.len()) };
        let _ = unsafe { t_close(ctl) };
    }
}

impl Master {
    /// Mint a new pts. Opens the clone file (the returned fd IS the master),
    /// then validates the fstat qid against the PTS_FLAG|master contract. On
    /// any internal failure the just-opened fd is closed before Err returns.
    pub fn mint() -> Result<Master, HoldError> {
        let mfd = open_rdwr("/dev/pts/ptmx");
        if mfd < 0 {
            return Err(HoldError::Mint);
        }
        // #100: t_stat ABI is 88 bytes; qid_path is at byte offset 8.
        let mut st = [0u8; 88];
        // SAFETY: SVC wrapper; st is a valid 88-byte t_stat buffer.
        if unsafe { t_fstat(mfd, st.as_mut_ptr()) } != 0 {
            let _ = unsafe { t_close(mfd) };
            return Err(HoldError::Fstat);
        }
        let mut q = [0u8; 8];
        q.copy_from_slice(&st[8..16]);
        let qid = u64::from_le_bytes(q);
        if qid & PTS_FLAG == 0 || (qid & 0xff) != PTS_FK_MASTER {
            let _ = unsafe { t_close(mfd) };
            return Err(HoldError::NotMaster);
        }
        let n = (qid >> 8) & 0xff_ffff;
        Ok(Master { mfd, n })
    }

    /// Seed the slave winsize via /dev/pts/<n>ctl. Best-effort: a ctl-open or
    /// write failure is swallowed (the ptyfs default size is the fallback).
    pub fn seed_winsize(&self, cols: u16, rows: u16) {
        set_winsize(self.n, cols, rows);
    }

    /// Open the slave three times over (one File per stdio slot) and spawn
    /// `argv` on it as fd 0/1/2. Each spawn slot consumes its File; the
    /// parent's copies close inside spawn, so the child's exit is what drops
    /// the slave-open count to zero and arms drain-then-EOF on the master.
    /// On Err the master fd is left open (the caller owns it); any slaves
    /// opened before the failure are closed by File's Drop.
    pub fn spawn_on_slave(&self, argv: &[String]) -> Result<Child, HoldError> {
        // A shared entry point: an empty argv would panic on argv[0] (an abort
        // under panic=abort). ptyhost never reaches here empty, but guard it.
        if argv.is_empty() {
            return Err(HoldError::Spawn);
        }
        let slave_path = format!("/dev/pts/{}", self.n);
        let mut slaves = Vec::new();
        for _ in 0..3 {
            match OpenOptions::new().read(true).write(true).open(&slave_path) {
                Ok(f) => slaves.push(f),
                Err(_) => return Err(HoldError::OpenSlave),
            }
        }
        let s2 = slaves.pop().unwrap();
        let s1 = slaves.pop().unwrap();
        let s0 = slaves.pop().unwrap();

        let mut cmd = Command::new(argv[0].clone());
        for a in &argv[1..] {
            cmd.arg(a.clone());
        }
        cmd.stdin(Stdio::File(s0));
        cmd.stdout(Stdio::File(s1));
        cmd.stderr(Stdio::File(s2));
        cmd.spawn().map_err(|_| HoldError::Spawn)
    }
}
