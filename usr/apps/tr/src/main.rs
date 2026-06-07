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

use aux_rt::{Args, Read, Write};

aux_rt::main!(run);

fn expand(set: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < set.len() {
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
    out
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut delete = false;
    if args.get_str(idx) == Some("-d") {
        delete = true;
        idx += 1;
    }

    let set1 = match args.get(idx) {
        Some(s) => expand(s),
        None => {
            aux_rt::eprintln!("tr: missing operand");
            return 1;
        }
    };
    idx += 1;

    // Build a 256-entry mapping or deletion table.
    let set2 = if delete {
        Vec::new()
    } else {
        match args.get(idx) {
            Some(s) => expand(s),
            None => {
                aux_rt::eprintln!("tr: translate mode needs two sets");
                return 1;
            }
        }
    };

    // table[b] = Some(replacement) | None(delete) ; default identity.
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

    let mut inp = aux_rt::stdin();
    let mut out = aux_rt::stdout();
    let mut buf = [0u8; 4096];
    let mut tmp: Vec<u8> = Vec::with_capacity(4096);
    loop {
        let n = match inp.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                aux_rt::eprintln!("tr: stdin: {}", e);
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
