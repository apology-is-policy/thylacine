// mac-line-probe -- a guest witness of the line to the Mac (docs/MAC-RUNNER.md 4.4). It opens
// /srv/mac as the calling user, so the kernel attaches with that user's principal, and reads the
// tree version 1 through the kernel's 9P client:
//
//   mac-line-probe once <text>   read snarf, check its stat and folders/, see O_RDWR refused,
//                                then set snarf to <text>
//   mac-line-probe hold <n>      connect, then read snarf once a second for n seconds; a read
//                                after the Mac app went away is an error, and is said
//
// Every result is one `mac-line-probe:` line on stdout; the status is 0 only when each step went
// as the tree says.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::time::Duration;

use libthyla_rs::io::Write;
use libthyla_rs::{
    t_close, t_fstat, t_open, t_read, t_readdir, t_write, T_OREAD, T_ORDWR, T_OTRUNC, T_OWRITE,
    T_WALK_OPEN_FROM_ROOT,
};

const S_IFMT: u32 = 0o170000;
const S_IFREG: u32 = 0o100000;
const QTDIR: u8 = 0x80;

fn say(line: &str) {
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(b"mac-line-probe: ");
    let _ = out.write_all(line.as_bytes());
    let _ = out.write_all(b"\n");
}

fn why(rc: i64) -> String {
    match libthyla_rs::err::Error::from_syscall_return(rc) {
        Err(e) => alloc::format!("{}", e),
        Ok(_) => String::from("no error"),
    }
}

/// The tree's root over a fresh connection to /srv/mac.
fn connect() -> Result<i64, String> {
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/mac".as_ptr(), 8, T_OREAD) };
    if root < 0 {
        return Err(why(root));
    }
    Ok(root)
}

fn open_at(root: i64, name: &[u8], mode: u32) -> i64 {
    unsafe { t_open(root, name.as_ptr(), name.len(), mode) }
}

/// (uid, gid, mode, qid type) of an open file.
fn stat(fd: i64) -> Result<(u32, u32, u32, u8), String> {
    let mut st = [0u8; 88];
    let rc = unsafe { t_fstat(fd, st.as_mut_ptr()) };
    if rc < 0 {
        return Err(why(rc));
    }
    let u32_at = |o: usize| u32::from_le_bytes([st[o], st[o + 1], st[o + 2], st[o + 3]]);
    Ok((u32_at(72), u32_at(76), u32_at(40), st[52]))
}

fn read_all(fd: i64) -> Result<Vec<u8>, String> {
    let mut got = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
        if n < 0 {
            return Err(why(n));
        }
        if n == 0 {
            return Ok(got);
        }
        got.extend_from_slice(&buf[..n as usize]);
    }
}

fn read_snarf(root: i64) -> Result<Vec<u8>, String> {
    let fd = open_at(root, b"snarf", T_OREAD);
    if fd < 0 {
        return Err(why(fd));
    }
    let r = read_all(fd);
    unsafe { t_close(fd) };
    r
}

fn once(text: &[u8]) -> bool {
    let root = match connect() {
        Ok(r) => r,
        Err(e) => {
            say(&alloc::format!("connect /srv/mac failed: {}", e));
            return false;
        }
    };
    let mut good = true;
    match read_snarf(root) {
        Ok(b) => say(&alloc::format!("snarf read {} bytes: {}", b.len(), String::from_utf8_lossy(&b))),
        Err(e) => {
            say(&alloc::format!("snarf read failed: {}", e));
            good = false;
        }
    }
    let fd = open_at(root, b"snarf", T_OREAD);
    match if fd < 0 { Err(why(fd)) } else { stat(fd) } {
        Ok((uid, gid, mode, _)) => {
            say(&alloc::format!("snarf owner uid={} gid={} mode={:o}", uid, gid, mode));
            if mode & S_IFMT != S_IFREG || mode & 0o777 != 0o600 {
                say("snarf's mode is not a regular file 0600");
                good = false;
            }
        }
        Err(e) => {
            say(&alloc::format!("snarf stat failed: {}", e));
            good = false;
        }
    }
    if fd >= 0 {
        unsafe { t_close(fd) };
    }
    let fd = open_at(root, b"snarf", T_ORDWR);
    if fd >= 0 {
        say("snarf opened O_RDWR: the tree says EACCES");
        unsafe { t_close(fd) };
        good = false;
    } else {
        say(&alloc::format!("snarf O_RDWR refused: {}", why(fd)));
    }
    let fd = open_at(root, b"folders", T_OREAD);
    if fd < 0 {
        say(&alloc::format!("folders open failed: {}", why(fd)));
        good = false;
    } else {
        let mut buf = [0u8; 1024];
        let n = unsafe { t_readdir(fd, buf.as_mut_ptr(), buf.len()) };
        let dir = matches!(stat(fd), Ok((_, _, _, q)) if q & QTDIR != 0);
        say(&alloc::format!("folders is {} and lists {} bytes", if dir { "a directory" } else { "NOT a directory" }, n));
        good &= dir && n == 0;
        unsafe { t_close(fd) };
    }
    let fd = open_at(root, b"snarf", T_OWRITE | T_OTRUNC);
    if fd < 0 {
        say(&alloc::format!("snarf open for writing failed: {}", why(fd)));
        good = false;
    } else {
        let n = unsafe { t_write(fd, text.as_ptr(), text.len()) };
        let c = unsafe { t_close(fd) };
        if n != text.len() as i64 || c < 0 {
            say(&alloc::format!("snarf write gave {} and its close {}", n, c));
            good = false;
        } else {
            say(&alloc::format!("snarf set {} bytes", n));
        }
    }
    unsafe { t_close(root) };
    say(if good { "once: every step as the tree says" } else { "once: NOT as the tree says" });
    good
}

fn hold(secs: u64) -> bool {
    let root = match connect() {
        Ok(r) => r,
        Err(e) => {
            say(&alloc::format!("connect /srv/mac failed: {}", e));
            return false;
        }
    };
    say("hold: connected");
    let mut failed = 0u64;
    for i in 0..secs {
        match read_snarf(root) {
            Ok(b) => say(&alloc::format!("hold {}: snarf read {} bytes", i, b.len())),
            Err(e) => {
                say(&alloc::format!("hold {}: snarf read failed: {}", i, e));
                failed += 1;
            }
        }
        let _ = libthyla_rs::time::sleep(Duration::from_secs(1));
    }
    unsafe { t_close(root) };
    say(&alloc::format!("hold: done, {} read(s) failed", failed));
    true
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args: Vec<&[u8]> = libthyla_rs::env::args().collect();
    let ok = match args.get(1).copied() {
        Some(b"once") => once(args.get(2).copied().unwrap_or(b"")),
        Some(b"hold") => {
            let n = args.get(2).and_then(|a| core::str::from_utf8(a).ok()).and_then(|s| s.parse().ok()).unwrap_or(5);
            hold(n)
        }
        _ => {
            say("usage: mac-line-probe once <text> | hold <seconds>");
            false
        }
    };
    if ok {
        0
    } else {
        1
    }
}
