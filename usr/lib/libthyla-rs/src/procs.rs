//! The process set, read the Plan 9 way: /proc's directory names every Proc and
//! each /proc/<pid>/status is one Proc's row.
//!
//! /ctl/procs is the same table in one read, and one read holds only the rows
//! that fit the kernel's buffer (the table ends with a cut line when some did
//! not). A program that reasons over EVERY Proc -- a listing, a monitor, "is
//! there a Proc named X" -- reads the directory instead: its listing is whole at
//! any size (ascending pids, resumed by cookie), and each status is rendered
//! under the process-table lock, so a row is one instant of its Proc.
//!
//! The SET is not one instant. A Proc that ends between the listing and its
//! status read is skipped; one born after the listing passed its pid is seen by
//! the next call. Pids are never reused, so no row is another Proc's.

use crate::err::{Error, Result};
use crate::fs::{self, File};
use crate::io::Read;
use alloc_crate::format;
use alloc_crate::string::String;
use alloc_crate::vec::Vec;

/// The kernel renders a status into a buffer of this size (devproc.c
/// DEVPROC_READ_BUF), so one read this large takes the file whole -- and only a
/// status taken in one read is one instant of its Proc.
pub const STATUS_MAX: usize = 2048;

/// A Proc's state word (kernel/proc.c proc_state_word).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Alive,
    Zombie,
    /// ALIVE under a job stop (a Ctrl-Z, a monitor's suspend).
    Stopped,
    Invalid,
    /// A word this library does not know.
    Other,
}

impl State {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            State::Alive => "ALIVE",
            State::Zombie => "ZOMBIE",
            State::Stopped => "STOPPED",
            State::Invalid => "INVALID",
            State::Other => "?",
        }
    }

    #[must_use]
    pub fn parse(word: &[u8]) -> State {
        match word {
            b"ALIVE" => State::Alive,
            b"ZOMBIE" => State::Zombie,
            b"STOPPED" => State::Stopped,
            b"INVALID" => State::Invalid,
            _ => State::Other,
        }
    }
}

/// One Proc, as /ctl/procs shows it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Row {
    pub pid: u32,
    /// 0 for kproc and for a Proc with no parent left.
    pub ppid: u32,
    pub name: String,
    pub state: State,
    pub threads: u32,
    /// The pages the address space holds, its page tables included.
    pub pages: u32,
    /// The page-table share of `pages`.
    pub tables: u32,
    pub children: u32,
    /// `None` when the kernel withholds it ("-"): a Proc's CPU time is its
    /// owner's and a hostowner's to read.
    pub cpu_ns: Option<u64>,
}

fn trim(mut v: &[u8]) -> &[u8] {
    while let [b' ', rest @ ..] = v {
        v = rest;
    }
    while let [rest @ .., b' '] = v {
        v = rest;
    }
    v
}

/// The value of `key` in a status text: the rest of the line that begins with
/// `key` and a colon, spaces trimmed. A key is matched at the START of a line,
/// never inside one, so "pid" does not find "ppid" and no value can pass for a
/// key: the one free-form value, the name, holds no newline (the kernel stores
/// only printable ASCII there). A last line whose newline did not arrive is
/// no line: a text cut mid-value would otherwise yield the shorter value.
#[must_use]
pub fn field<'a>(status: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let end = status.iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
    for line in status[..end].split(|&b| b == b'\n') {
        if line.len() > key.len() && line.starts_with(key) && line[key.len()] == b':' {
            return Some(trim(&line[key.len() + 1..]));
        }
    }
    None
}

/// A decimal number and nothing else: no sign, no space, no overflow.
#[must_use]
pub fn decimal(v: &[u8]) -> Option<u64> {
    if v.is_empty() {
        return None;
    }
    let mut n: u64 = 0;
    for &b in v {
        if !b.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add(u64::from(b - b'0'))?;
    }
    Some(n)
}

fn decimal32(v: &[u8]) -> Option<u32> {
    u32::try_from(decimal(v)?).ok()
}

impl Row {
    /// A status text as a row. `None` when a field the row needs is missing or
    /// is not what it should be: a row never carries a made-up number.
    #[must_use]
    pub fn parse(status: &[u8]) -> Option<Row> {
        let cpu = field(status, b"cpu_ns")?;
        Some(Row {
            pid: decimal32(field(status, b"pid")?)?,
            ppid: decimal32(field(status, b"ppid")?)?,
            name: String::from(core::str::from_utf8(field(status, b"name")?).ok()?),
            state: State::parse(field(status, b"state")?),
            threads: decimal32(field(status, b"threads")?)?,
            pages: decimal32(field(status, b"pages")?)?,
            tables: decimal32(field(status, b"tables")?)?,
            children: decimal32(field(status, b"children")?)?,
            cpu_ns: if cpu == b"-" {
                None
            } else {
                Some(decimal(cpu)?)
            },
        })
    }
}

/// Every pid /proc lists, ascending.
pub fn pids() -> Result<Vec<u32>> {
    let mut out = Vec::new();
    for entry in fs::read_dir("/proc")? {
        if let Some(pid) = decimal32(entry?.file_name().as_bytes()) {
            out.push(pid);
        }
    }
    Ok(out)
}

/// The status text of `pid`, whole, into `buf`; its length, or `None` when no
/// Proc has that pid any more.
pub fn read_status(pid: u32, buf: &mut [u8; STATUS_MAX]) -> Result<Option<usize>> {
    let path = format!("/proc/{}/status", pid);
    let mut f = match File::open(path.as_str()) {
        Ok(f) => f,
        Err(Error::NotFound) => return Ok(None),
        Err(e) => return Err(e),
    };
    match f.read(&mut buf[..]) {
        Ok(0) => Err(Error::Io),
        Ok(n) => Ok(Some(n)),
        // The kernel's unnamed failure: what the read of a Proc reaped after
        // the open returns, and anything else it does not name. Only a pid
        // that no longer opens is gone (pids are never reused); an authority
        // refusal is PermissionDenied and stays an error.
        Err(Error::Io) => match File::open(path.as_str()) {
            Err(Error::NotFound) => Ok(None),
            _ => Err(Error::Io),
        },
        Err(e) => Err(e),
    }
}

/// The row of `pid`, or `None` when no Proc has that pid any more. A status
/// that does not parse is an error, not an absence.
pub fn row(pid: u32) -> Result<Option<Row>> {
    let mut buf = [0u8; STATUS_MAX];
    match read_status(pid, &mut buf)? {
        None => Ok(None),
        Some(n) => Row::parse(&buf[..n]).map(Some).ok_or(Error::Io),
    }
}

/// Every Proc as a row, ascending by pid.
pub fn list() -> Result<Vec<Row>> {
    let mut rows = Vec::new();
    for pid in pids()? {
        if let Some(r) = row(pid)? {
            rows.push(r);
        }
    }
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;

    const JOEY: &[u8] = b"name:    joey\npid:     1\nstate:   ALIVE\nthreads: 3\nthreadcap:64\n\
cpu_ns:  123456789\nppid:    0\nprincipal:0 gid:0\npages:   412\ntables:  9\nfile:    300\n\
children:7\npeak:    500\nbudget:  65536\n";

    #[test]
    fn a_status_parses_into_its_row() {
        let r = Row::parse(JOEY).expect("joey's status parses");
        assert_eq!(
            r,
            Row {
                pid: 1,
                ppid: 0,
                name: String::from("joey"),
                state: State::Alive,
                threads: 3,
                pages: 412,
                tables: 9,
                children: 7,
                cpu_ns: Some(123_456_789),
            }
        );
    }

    #[test]
    fn a_key_matches_at_a_line_start_only() {
        // "pid" must not find "ppid", whichever comes first.
        let ppid_first = b"ppid:    42\npid:     7\n";
        assert_eq!(field(ppid_first, b"pid"), Some(&b"7"[..]));
        assert_eq!(field(ppid_first, b"ppid"), Some(&b"42"[..]));
        // "gid" sits inside the principal line and is not a key of its own.
        assert_eq!(field(JOEY, b"gid"), None);
        // "threads" and "threadcap" are told apart by the colon.
        assert_eq!(field(JOEY, b"threads"), Some(&b"3"[..]));
        assert_eq!(field(JOEY, b"thread"), None);
    }

    #[test]
    fn a_cut_last_line_is_no_field() {
        // "ppid:    41" cut to "ppid:    4" must not read as parent 4.
        let cut = b"pid:     57\nppid:    4";
        assert_eq!(field(cut, b"pid"), Some(&b"57"[..]));
        assert_eq!(field(cut, b"ppid"), None);
        assert_eq!(field(b"ppid:    4", b"ppid"), None);
        // A status cut inside the last field a row needs is no row; the same
        // text through that line's newline is one.
        let at = JOEY.windows(10).position(|w| w == b"children:7").expect("fixture") + 10;
        assert_eq!(Row::parse(&JOEY[..at]), None);
        assert_eq!(Row::parse(&JOEY[..=at]).expect("whole through children").children, 7);
    }

    #[test]
    fn withheld_cpu_time_is_none_and_garbage_is_no_row() {
        let withheld = JOEY
            .split(|&b| b == b'\n')
            .map(|l| {
                if l.starts_with(b"cpu_ns:") {
                    &b"cpu_ns:  -"[..]
                } else {
                    l
                }
            })
            .collect::<Vec<_>>()
            .join(&b'\n');
        assert_eq!(Row::parse(&withheld).expect("parses").cpu_ns, None);

        let garbage = JOEY
            .split(|&b| b == b'\n')
            .map(|l| {
                if l.starts_with(b"cpu_ns:") {
                    &b"cpu_ns:  12x"[..]
                } else {
                    l
                }
            })
            .collect::<Vec<_>>()
            .join(&b'\n');
        assert_eq!(Row::parse(&garbage), None);
    }

    #[test]
    fn a_missing_field_is_no_row() {
        for key in [
            "name", "pid", "state", "threads", "cpu_ns", "ppid", "pages", "tables", "children",
        ] {
            let cut = JOEY
                .split(|&b| b == b'\n')
                .filter(|l| !(l.starts_with(key.as_bytes()) && l.get(key.len()) == Some(&b':')))
                .collect::<Vec<_>>()
                .join(&b'\n');
            assert_eq!(Row::parse(&cut), None, "a status without {key} is no row");
        }
    }

    #[test]
    fn decimal_takes_digits_only() {
        assert_eq!(decimal(b"0"), Some(0));
        assert_eq!(decimal(b"18446744073709551615"), Some(u64::MAX));
        assert_eq!(decimal(b"18446744073709551616"), None);
        assert_eq!(decimal(b""), None);
        assert_eq!(decimal(b"-1"), None);
        assert_eq!(decimal(b"1 "), None);
        assert_eq!(decimal32(b"4294967296"), None);
    }

    #[test]
    fn the_state_words_round_trip() {
        for s in [State::Alive, State::Zombie, State::Stopped, State::Invalid] {
            assert_eq!(State::parse(s.as_str().as_bytes()), s);
        }
        assert_eq!(State::parse(b"Running"), State::Other);
    }

    #[test]
    fn a_name_with_a_space_is_one_name() {
        let st = b"name:    a?b c\npid:     9\nstate:   STOPPED\nthreads: 1\ncpu_ns:  -\n\
ppid:    1\npages:   1\ntables:  1\nchildren:0\n";
        let r = Row::parse(st).expect("parses");
        assert_eq!(r.name, "a?b c");
        assert_eq!(r.state, State::Stopped);
    }
}
