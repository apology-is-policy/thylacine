// tr SET1 [SET2]  /  tr -d SET1 -- translate or delete bytes of stdin.
//
// SETs expand `a-z` ranges (byte ranges). Translate maps SET1[i]->SET2[i];
// if SET2 is shorter, its last byte repeats (GNU behavior). -d deletes every
// byte in SET1. No -s (squeeze) / -c (complement) at v1. Reads stdin only.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

// Expand `a-z` byte ranges. Returns None if the set opens an unsupported
// POSIX bracket construct (`[:class:]` / `[=equiv=]`): tr v1 has no class
// support, and silently treating `[:space:]` as the literal bytes
// `[ : s p a c e ]` corrupts data (exit 0), so reject it visibly instead.
fn expand(set: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < set.len() {
        if set[i] == b'[' && i + 1 < set.len() && (set[i + 1] == b':' || set[i + 1] == b'=') {
            return None;
        }
        if i + 2 < set.len() && set[i + 1] == b'-' && set[i + 2] >= set[i] {
            for b in set[i]..=set[i + 2] {
                out.push(b);
            }
            i += 3;
        } else {
            out.push(set[i]);
            i += 1;
        }
    }
    Some(out)
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut delete = false;
    if args.get_str(idx) == Some("-d") {
        delete = true;
        idx += 1;
    }

    let set1 = match args.get(idx) {
        Some(s) => match expand(s) {
            Some(v) => v,
            None => {
                eprintln!("tr: POSIX character classes ([:class:], [=equiv=]) are not supported");
                return 1;
            }
        },
        None => {
            eprintln!("tr: missing operand");
            return 1;
        }
    };
    idx += 1;

    let set2 = if delete {
        Vec::new()
    } else {
        match args.get(idx) {
            Some(s) => match expand(s) {
                Some(v) => v,
                None => {
                    eprintln!("tr: POSIX character classes ([:class:], [=equiv=]) are not supported");
                    return 1;
                }
            },
            None => {
                eprintln!("tr: translate mode needs two sets");
                return 1;
            }
        }
    };

    // map[b] = replacement (identity default); del[b] = drop the byte.
    let mut map = [0u8; 256];
    let mut del = [false; 256];
    for (b, m) in map.iter_mut().enumerate() {
        *m = b as u8;
    }
    if delete {
        for &b in &set1 {
            del[b as usize] = true;
        }
    } else if !set2.is_empty() {
        for (i, &b) in set1.iter().enumerate() {
            let r = set2[i.min(set2.len() - 1)];
            map[b as usize] = r;
        }
    }

    let mut inp = io::stdin();
    let mut out = io::stdout();
    let mut buf = [0u8; 4096];
    let mut tmp: Vec<u8> = Vec::with_capacity(4096);
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
        for &b in &buf[..n] {
            if delete {
                if !del[b as usize] {
                    tmp.push(b);
                }
            } else {
                tmp.push(map[b as usize]);
            }
        }
        if out.write_all(&tmp).is_err() {
            break;
        }
    }
    0
}
