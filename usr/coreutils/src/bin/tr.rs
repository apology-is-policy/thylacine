// tr [-cds] SET1 [SET2] -- translate, delete, or squeeze bytes of stdin.
//
// Modes (GNU): translate `tr S1 S2` (S1[i]->S2[i], S2's last byte repeats);
// delete `tr -d S1`; squeeze-only `tr -s S1` (collapse runs of S1 bytes);
// delete+squeeze `tr -ds S1 S2`; translate+squeeze `tr -s S1 S2` (squeeze S2).
// -c complements SET1 (operate on every byte NOT in S1). SETs expand `a-z`
// ranges and the C escapes (\n \t \r \a \b \f \v \\ and \NNN octal). The POSIX
// `[:class:]` / `[=equiv=]` forms are rejected (no class support at v1). Reads
// stdin only.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io::{self, Read};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// Resolve C-style backslash escapes (GNU tr's set syntax). An unknown `\X`
/// keeps `X` literally (GNU's behavior).
fn resolve_escapes(set: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < set.len() {
        if set[i] == b'\\' && i + 1 < set.len() {
            let c = set[i + 1];
            let simple = match c {
                b'n' => Some(b'\n'),
                b't' => Some(b'\t'),
                b'r' => Some(b'\r'),
                b'a' => Some(0x07),
                b'b' => Some(0x08),
                b'f' => Some(0x0c),
                b'v' => Some(0x0b),
                b'\\' => Some(b'\\'),
                _ => None,
            };
            if let Some(byte) = simple {
                out.push(byte);
                i += 2;
            } else if (b'0'..=b'7').contains(&c) {
                // up to 3 octal digits
                let mut val: u32 = 0;
                let mut k = 0;
                while k < 3 && i + 1 + k < set.len() && (b'0'..=b'7').contains(&set[i + 1 + k]) {
                    val = val * 8 + (set[i + 1 + k] - b'0') as u32;
                    k += 1;
                }
                out.push((val & 0xff) as u8);
                i += 1 + k;
            } else {
                // unknown \X -> X literal (GNU keeps X)
                out.push(c);
                i += 2;
            }
        } else {
            out.push(set[i]);
            i += 1;
        }
    }
    out
}

/// Expand a SET: resolve escapes, then expand `a-z` byte ranges. Returns None
/// for an unsupported `[:class:]` / `[=equiv=]` construct (rejecting it visibly
/// beats silently treating it as the literal bytes -- that corrupts data).
fn expand(set: &[u8]) -> Option<Vec<u8>> {
    let r = resolve_escapes(set);
    let mut out = Vec::new();
    let mut i = 0;
    while i < r.len() {
        if r[i] == b'[' && i + 1 < r.len() && (r[i + 1] == b':' || r[i + 1] == b'=') {
            return None;
        }
        if i + 2 < r.len() && r[i + 1] == b'-' && r[i + 2] >= r[i] {
            for b in r[i]..=r[i + 2] {
                out.push(b);
            }
            i += 3;
        } else {
            out.push(r[i]);
            i += 1;
        }
    }
    Some(out)
}

const USAGE: &str = "\
usage: tr [-cds] SET1 [SET2]
  Translate, delete, or squeeze bytes of stdin to stdout.
  (no flags)  translate SET1 to SET2 (SET2's last byte repeats)
  -d          delete bytes in SET1
  -s          squeeze repeated bytes (of SET2, or SET1 when squeeze-only)
  -c          complement SET1 (operate on bytes NOT in SET1)
  SETs expand a-z ranges and \\n \\t \\r \\a \\b \\f \\v \\\\ \\NNN escapes.
  --help      show this help

Examples:
  echo HELLO | tr A-Z a-z   # lowercase
  tr -s ' ' < file          # squeeze runs of spaces
  tr -cd 'a-zA-Z' < file    # keep only letters
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut delete = false;
    let mut squeeze = false;
    let mut complement = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'd' => delete = true,
                    's' => squeeze = true,
                    'c' | 'C' => complement = true,
                    _ => {
                        eprintln!("tr: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    // SET1 (always required).
    let set1 = match args.get(idx) {
        Some(s) => match expand(s) {
            Some(v) => v,
            None => return reject_class(),
        },
        None => {
            eprintln!("tr: missing operand");
            return 1;
        }
    };
    idx += 1;

    // SET2 is needed for translate (no -d, SET2 present) and for delete+squeeze.
    let translate = !delete && args.get(idx).is_some();
    let need_set2 = translate || (delete && squeeze);
    let set2 = if need_set2 {
        match args.get(idx) {
            Some(s) => match expand(s) {
                Some(v) => v,
                None => return reject_class(),
            },
            None => {
                eprintln!("tr: this combination of options requires two sets");
                return 1;
            }
        }
    } else {
        Vec::new()
    };
    if !delete && !squeeze && !translate {
        eprintln!("tr: translate mode needs two sets");
        return 1;
    }

    // in1[b]: b is in (the possibly-complemented) SET1.
    let mut in1 = [false; 256];
    for &b in &set1 {
        in1[b as usize] = true;
    }
    if complement {
        for m in in1.iter_mut() {
            *m = !*m;
        }
    }

    // Translate map (identity default).
    let mut map = [0u8; 256];
    for (b, m) in map.iter_mut().enumerate() {
        *m = b as u8;
    }
    if translate {
        if complement {
            // Every member of the complemented SET1 maps to SET2's last byte.
            let last = set2.last().copied().unwrap_or(0);
            for (b, m) in map.iter_mut().enumerate() {
                if in1[b] {
                    *m = last;
                }
            }
        } else {
            for (i, &b) in set1.iter().enumerate() {
                map[b as usize] = set2[i.min(set2.len() - 1)];
            }
        }
    }

    // Squeeze membership: SET2 when translating/deleting, else SET1.
    let mut sq = [false; 256];
    if squeeze {
        if translate || delete {
            for &b in &set2 {
                sq[b as usize] = true;
            }
        } else {
            sq.copy_from_slice(&in1);
        }
    }

    let mut inp = io::stdin();
    // A filter (like cat/cut/wc): payload through OutSink so a mid-stream
    // stdout error reports + exits nonzero, not silently truncates at exit 0.
    let mut out = io::OutSink::new();
    let mut buf = [0u8; 4096];
    let mut tmp: Vec<u8> = Vec::with_capacity(4096);
    let mut last: i32 = -1; // last emitted byte, for squeeze across read boundaries
    loop {
        let n = match inp.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                eprintln!("tr: stdin: {}", e);
                return 1;
            }
        };
        tmp.clear();
        for &b0 in &buf[..n] {
            if delete && in1[b0 as usize] {
                continue;
            }
            let b = if translate { map[b0 as usize] } else { b0 };
            if squeeze && sq[b as usize] && last == b as i32 {
                continue; // collapse a run of the same squeezed byte
            }
            last = b as i32;
            tmp.push(b);
        }
        out.put(&tmp);
        if out.failed() {
            break;
        }
    }
    if out.failed() {
        eprintln!("tr: write error");
        return 1;
    }
    0
}

fn reject_class() -> i64 {
    eprintln!("tr: POSIX character classes ([:class:], [=equiv=]) are not supported");
    1
}
