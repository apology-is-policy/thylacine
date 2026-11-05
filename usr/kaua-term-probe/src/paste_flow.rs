//! The real session's half of the paste (ASYNC-SERVICE-LIFECYCLE, AS-4f), driven by
//! tools/interactive/ls-halcyon-paste-flow.exp. Each verb is one foreground process in a
//! tile, so what it may read is what that tile's focus and session admit. Its line names
//! its leg, so a red says which.
//!
//! This is the other path: a blocking native /srv connection, reading what the editor and
//! the shell did over their private rings, and reading their files.
use crate::clipboard_client::{self, Result};
use ::alloc::{format, string::String, vec::Vec};
use interaction_native::{parse_locator, Host};
use libthyla_rs::{fs::File, io::Read, loom::ThylaKernel, *};

const EOPNOTSUPP: i32 = -95;
const EPERM: i64 = -1;

/// The scenario's keyboard has no tab and reads `\\n` as Enter, so a line break and a tab
/// travel as `%n` and `%t`.
fn unescape(text: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len());
    let mut bytes = text.iter().copied();
    while let Some(b) = bytes.next() {
        if b != b'%' {
            out.push(b);
            continue;
        }
        match bytes.next() {
            Some(b'n') => out.push(b'\n'),
            Some(b't') => out.push(b'\t'),
            Some(other) => out.extend_from_slice(&[b'%', other]),
            None => out.push(b'%'),
        }
    }
    out
}

fn count(text: &[u8], word: &[u8]) -> usize {
    text.windows(word.len()).filter(|w| *w == word).count()
}

fn arg(n: usize, what: &'static str) -> Result<&'static [u8]> {
    env::args().nth(n).ok_or(what)
}

fn home_file(name: &[u8]) -> Result<Vec<u8>> {
    let home = env::var("HOME").ok_or("no home")?;
    let name = core::str::from_utf8(name).map_err(|_| "file name")?;
    let mut file = File::open(format!("{}/{}", home, name)).map_err(|_| "open the file")?;
    let mut text = Vec::new();
    file.read_to_end(&mut text).map_err(|_| "read the file")?;
    Ok(text)
}

/// `WORD:N` pairs, each word held exactly N times. Zero is only a witness beside a word
/// that must be there, since an empty file holds no word at all.
fn has(text: &[u8], pairs: &[u8]) -> Result<()> {
    let (word, n) = pairs
        .iter()
        .rposition(|b| *b == b':')
        .map(|at| (&pairs[..at], &pairs[at + 1..]))
        .ok_or("WORD:N")?;
    let n = core::str::from_utf8(n)
        .ok()
        .and_then(|n| n.parse::<usize>().ok())
        .ok_or("WORD:N")?;
    if word.is_empty() || count(text, word) != n {
        return Err("a word is not there the number of times it should be");
    }
    Ok(())
}

fn ring() -> core::result::Result<(), i32> {
    let value = env::var("HALCYON_INTERACTION").ok_or(-2)?;
    let locator = parse_locator(value.as_bytes()).ok_or(-22)?;
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(srv as i32);
    }
    let host = Host::new(ThylaKernel, srv as i32, locator);
    let _ = unsafe { t_close(srv) };
    host.map(drop)
}

fn verb(verb: &[u8]) -> Result<()> {
    match verb {
        b"mark" => Ok(()),
        b"put" => clipboard_client::publish(&unescape(arg(4, "no text")?)),
        b"same" => {
            if clipboard_client::current()? == unescape(arg(4, "no text")?) {
                Ok(())
            } else {
                Err("the clipboard holds something else")
            }
        }
        b"empty" => {
            if clipboard_client::current()?.is_empty() {
                Ok(())
            } else {
                Err("the clipboard is not empty")
            }
        }
        b"refused" => match clipboard_client::read_errno()? {
            Ok(_) => Err("the read was admitted"),
            Err(EPERM) => Ok(()),
            Err(_) => Err("the read failed, but not with a refusal"),
        },
        b"has" => {
            let text = home_file(arg(4, "no file")?)?;
            let mut any = false;
            for at in 5.. {
                let Some(pair) = env::args().nth(at) else {
                    break;
                };
                has(&text, pair)?;
                any = true;
            }
            if any {
                Ok(())
            } else {
                Err("no word to look for")
            }
        }
        _ => Err("no such verb"),
    }
}

pub fn run() -> i64 {
    let which = env::args().nth(2).unwrap_or(b"");
    let leg = env::args()
        .nth(3)
        .map(String::from_utf8_lossy)
        .unwrap_or_default();
    if which == b"ring" {
        return match ring() {
            Ok(()) => {
                t_putstr(&format!("paste-flow: {} PASS\n", leg));
                0
            }
            Err(EOPNOTSUPP) => {
                t_putstr("paste-flow: UNAVAILABLE\n");
                0
            }
            Err(e) => {
                t_putstr(&format!("paste-flow: FAIL {} -- ring errno {}\n", leg, e));
                1
            }
        };
    }
    match verb(which) {
        Ok(()) => {
            t_putstr(&format!("paste-flow: {} PASS\n", leg));
            0
        }
        Err(e) => {
            t_putstr(&format!("paste-flow: FAIL {} -- {}\n", leg, e));
            1
        }
    }
}
