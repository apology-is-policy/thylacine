// /bin/curl -- a native libthyla_rs HTTP + HTTPS GET client (net-utils).
//
// A thin frontend over the shared `curl` engine lib (URL parsing + HTTP exchange
// + the /net / TLS transport): parse flags, fetch, emit the body to stdout (or a
// file with -o; -I sends HEAD + prints the headers). See `lib.rs` for the engine
// and the trust/capability model.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use curl::{fetch, selftest, split_response, status_line};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{eprintln, println};

const USAGE: &str = "\
usage: curl [-I] [-s] [-o FILE] URL
  Fetch URL (http:// or https://) with an HTTP GET and print the body.
  -I, --head      send a HEAD request; print the response headers
  -o, --output F  write the body to file F instead of stdout
  -s, --silent    suppress error messages
  --selftest      run the deterministic URL/HTTP parser self-test (no network)
  -h, --help      show this help

Examples:
  curl http://10.0.2.2:8000/        # plain HTTP
  curl https://example.com/         # TLS (validates against the baked CA bundle)
  curl -o page.html https://host/   # save the body to a file
  curl -I http://host/              # headers only
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn die(msg: &str) -> i64 {
    eprintln!("curl: {}", msg);
    1
}

fn run(args: Args) -> i64 {
    let mut head_only = false;
    let mut silent = false;
    let mut out_file: Option<&str> = None;
    let mut url: Option<&str> = None;
    let mut i = 1;
    while i < args.len() {
        let a = match args.get_str(i) {
            Some(a) => a,
            None => return die("invalid argument"),
        };
        match a {
            "--selftest" => return run_selftest(),
            "-h" | "--help" => {
                println!("{}", USAGE);
                return 0;
            }
            "-I" | "--head" => head_only = true,
            "-s" | "--silent" => silent = true,
            "-o" | "--output" => {
                i += 1;
                out_file = match args.get_str(i) {
                    Some(f) => Some(f),
                    None => return die("-o needs a file argument"),
                };
            }
            _ if a.starts_with('-') => return die(&format!("unknown option '{}'", a)),
            _ => {
                if url.is_some() {
                    return die("only one URL is supported");
                }
                url = Some(a);
            }
        }
        i += 1;
    }
    let url = match url {
        Some(u) => u,
        None => return die("no URL specified (try --help)"),
    };
    do_fetch(url, head_only, out_file, silent)
}

fn do_fetch(raw: &str, head_only: bool, out_file: Option<&str>, silent: bool) -> i64 {
    let method = if head_only { "HEAD" } else { "GET" };
    let resp = match fetch(raw, method) {
        Ok(v) => v,
        Err(m) => {
            if !silent {
                eprintln!("curl: {}", m);
            }
            return 1;
        }
    };
    let (head, body) = split_response(&resp);
    // -I prints the head (status + headers); a GET prints the body.
    let payload: &[u8] = if head_only { head } else { body };

    if let Some(path) = out_file {
        match File::create(path) {
            Ok(mut f) => {
                if f.write_all(payload).is_err() {
                    return die(&format!("write to {} failed", path));
                }
            }
            Err(_) => return die(&format!("cannot create {}", path)),
        }
        if !silent {
            eprintln!(
                "curl: {} -> {} ({} bytes, {})",
                raw,
                path,
                payload.len(),
                status_line(head)
            );
        }
    } else {
        let mut out = io::stdout();
        let _ = out.write_all(payload);
        if head_only {
            let _ = out.write_all(b"\r\n");
        }
    }
    0
}

fn run_selftest() -> i64 {
    match selftest() {
        Ok(n) => {
            println!(
                "curl: SELFTEST OK ({} URL cases, request build, response split [CRLF + LF])",
                n
            );
            0
        }
        Err(e) => {
            eprintln!("curl: SELFTEST FAILED: {}", e);
            1
        }
    }
}
