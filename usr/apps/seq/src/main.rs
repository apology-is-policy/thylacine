// seq [FIRST [INCR]] LAST -- print an integer sequence, one per line.
//
// Integer-only at v1 (no floating sequences). FIRST defaults to 1, INCR to
// 1. A zero increment is rejected. Counts up (INCR > 0) or down (INCR < 0).
// Iteration is overflow-safe (stops at the i64 boundary instead of panicking).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut nops = 0;
    while args.get(1 + nops).is_some() {
        nops += 1;
    }
    let p = |i: usize| args.get_str(i).and_then(|s| s.parse::<i64>().ok());

    let triple = match nops {
        1 => p(1).map(|l| (1i64, 1i64, l)),
        2 => match (p(1), p(2)) {
            (Some(f), Some(l)) => Some((f, 1, l)),
            _ => None,
        },
        3 => match (p(1), p(2), p(3)) {
            (Some(f), Some(i), Some(l)) => Some((f, i, l)),
            _ => None,
        },
        _ => {
            aux_rt::eprintln!("seq: usage: seq [FIRST [INCR]] LAST");
            return 1;
        }
    };
    let (first, incr, last) = match triple {
        Some(t) => t,
        None => {
            aux_rt::eprintln!("seq: invalid number");
            return 1;
        }
    };
    if incr == 0 {
        aux_rt::eprintln!("seq: increment must not be zero");
        return 1;
    }

    let mut i = first;
    loop {
        if (incr > 0 && i > last) || (incr < 0 && i < last) {
            break;
        }
        aux_rt::println!("{}", i);
        match i.checked_add(incr) {
            Some(n) => i = n,
            None => break, // would overflow past the i64 boundary
        }
    }
    0
}
