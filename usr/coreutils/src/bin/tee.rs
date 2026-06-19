// tee [-a] FILE... -- copy stdin to stdout and to each FILE.
//
// -a: append to the files instead of truncating. Reads stdin in 8 KiB chunks
// and fans each chunk out to stdout + every successfully-opened file. A file
// that fails to open or write is reported and dropped from the sink set;
// stdout and the other files keep flowing. Exit 1 if any file errored.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: tee [-a] FILE...
  Copy stdin to stdout AND to each FILE.
  -a      append to the FILEs instead of truncating
  --help  show this help

Examples:
  ls | tee files.txt        # screen + file
  cmd | tee -a log.txt      # append to a log
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut append = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'a' => append = true,
                    _ => {
                        eprintln!("tee: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut status = 0;
    let mut sinks: Vec<File> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("tee: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        let opened = if append {
            OpenOptions::new().write(true).create(true).append(true).open(path)
        } else {
            File::create(path)
        };
        match opened {
            Ok(f) => sinks.push(f),
            Err(e) => {
                eprintln!("tee: {}: {}", path, e);
                status = 1;
            }
        }
    }

    let mut stdin = io::stdin();
    let mut stdout = io::stdout();
    let mut stdout_failed = false;
    let mut buf = [0u8; 8 * 1024];
    loop {
        let n = match stdin.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                eprintln!("tee: stdin: {}", e);
                status = 1;
                break;
            }
        };
        let chunk = &buf[..n];
        // The stdout leg is payload too: a swallowed write silently truncates
        // tee's primary output. Report once, stop writing the dead sink, but
        // keep the files flowing (the tee contract).
        if !stdout_failed && stdout.write_all(chunk).is_err() {
            eprintln!("tee: stdout: write error");
            status = 1;
            stdout_failed = true;
        }
        // Fan out to the files, dropping any that error mid-stream.
        let mut keep = Vec::with_capacity(sinks.len());
        for mut f in sinks.drain(..) {
            if f.write_all(chunk).is_ok() {
                keep.push(f);
            } else {
                eprintln!("tee: write error");
                status = 1;
            }
        }
        sinks = keep;
        // Every sink is gone -- no reason to drain the rest of stdin.
        if stdout_failed && sinks.is_empty() {
            break;
        }
    }
    status
}
