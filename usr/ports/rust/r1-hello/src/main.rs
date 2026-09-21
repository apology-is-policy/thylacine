//! R-1 witness. Exercises Rust `std` on Thylacine and prints one token per
//! leg. Core legs (stdout / HashMap / threads / panic-unwind) gate the final
//! `R1-HELLO: PASS`; file read and TCP connect are reported but do NOT gate
//! PASS (the boot-test env has no guaranteed file and no netd listener).
use std::collections::HashMap;

fn main() {
    println!("R1-HELLO: std alive on thylacine");
    let mut ok = true;

    // HashMap (alloc + hashing).
    let mut m: HashMap<String, u32> = HashMap::new();
    m.insert("thylacine".into(), 1936);
    m.insert("joey".into(), 7);
    if m.get("thylacine") == Some(&1936) && m.len() == 2 {
        println!("R1-HASHMAP: ok len={}", m.len());
    } else {
        println!("R1-HASHMAP: FAIL");
        ok = false;
    }

    // Threads (spawn + join over the pthread/torpor path).
    let handles: Vec<_> = (0..4u32).map(|i| std::thread::spawn(move || i * i)).collect();
    let sum: u32 = handles.into_iter().map(|h| h.join().unwrap()).sum();
    if sum == 0 + 1 + 4 + 9 {
        println!("R1-THREADS: ok sum={}", sum);
    } else {
        println!("R1-THREADS: FAIL sum={}", sum);
        ok = false;
    }

    // A panic that unwinds, caught (proves the unwinder + libunwind link).
    let caught = std::panic::catch_unwind(|| panic!("intentional R-1 unwind probe"));
    if caught.is_err() {
        println!("R1-PANIC-UNWIND: ok (caught)");
    } else {
        println!("R1-PANIC-UNWIND: FAIL (not caught)");
        ok = false;
    }

    // File read -- informational (needs a readable file in the image).
    match std::fs::read_to_string("/manual/index.md") {
        Ok(s) => println!("R1-FILE: ok {} bytes", s.len()),
        Err(e) => println!("R1-FILE: skip ({})", e.kind() as u8),
    }

    // TCP connect -- informational (needs netd + a listener).
    match std::net::TcpStream::connect("127.0.0.1:9") {
        Ok(_) => println!("R1-TCP: connected"),
        Err(e) => println!("R1-TCP: skip ({})", e.kind() as u8),
    }

    if ok {
        println!("R1-HELLO: PASS");
        std::process::exit(0);
    } else {
        println!("R1-HELLO: FAIL");
        std::process::exit(1);
    }
}
