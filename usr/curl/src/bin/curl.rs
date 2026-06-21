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

use curl::{fetch, fetch_stream, selftest, split_response, status_line, CountSink};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{eprintln, println};

const USAGE: &str = "\
usage: curl [-I] [-s] [-o FILE] [--bench] URL
  Fetch URL (http:// or https://) with an HTTP GET and print the body.
  -I, --head      send a HEAD request; print the response headers
  -o, --output F  stream the body to file F instead of stdout (no size cap)
  -s, --silent    suppress error messages
  --bench         stream the body to /dev/null + report timing (connect / ttfb /
                  total) + throughput (KiB/s) -- a real-network micro-benchmark
  --selftest      run the deterministic URL/HTTP parser self-test (no network)
  -h, --help      show this help

Examples:
  curl http://10.0.2.2:8000/        # plain HTTP
  curl https://example.com/         # TLS (validates against the baked CA bundle)
  curl -o page.html https://host/   # save the body to a file
  curl --bench http://host/big.bin  # time a download to /dev/null
  curl -I http://host/              # headers only
";

/// Split nanoseconds into (whole ms, fractional us) for "N.NNN ms" formatting
/// (no float in no_std): ms = ns/1e6, frac = (ns%1e6)/1000.
fn ms3(ns: u64) -> (u64, u64) {
    (ns / 1_000_000, (ns % 1_000_000) / 1000)
}

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
    let mut bench = false;
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
            "--bench" => bench = true,
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
    if bench {
        return do_bench(url, if head_only { "HEAD" } else { "GET" }, silent);
    }
    // A GET with -o streams the body to the file (no size cap); -I or stdout keep
    // the small-buffer path (it needs the head/body split).
    if let (Some(path), false) = (out_file, head_only) {
        return do_download(url, path, silent);
    }
    do_fetch(url, head_only, out_file, silent)
}

/// `--bench`: stream the body to a counting /dev/null sink + report the timing
/// breakdown (connect+handshake / time-to-first-byte / total) and throughput.
fn do_bench(raw: &str, method: &str, silent: bool) -> i64 {
    let mut sink = CountSink::default();
    let st = match fetch_stream(raw, method, &mut sink) {
        Ok(s) => s,
        Err(m) => {
            if !silent {
                eprintln!("curl: {}", m);
            }
            return 1;
        }
    };
    let (cm, cf) = ms3(st.connect_ns);
    let (tm, tf) = ms3(st.ttfb_ns);
    let (om, of) = ms3(st.total_ns);
    // KiB/s over the whole transfer (body_bytes * 1000 fits u64 well past 1 GiB).
    let total_ms = st.total_ns / 1_000_000;
    let kibps = if total_ms > 0 {
        st.body_bytes * 1000 / 1024 / total_ms
    } else {
        0
    };
    println!(
        "curl bench: {} | {} bytes | connect {}.{:03} ms ttfb {}.{:03} ms total {}.{:03} ms | {} KiB/s ({} MiB/s)",
        st.status, st.body_bytes, cm, cf, tm, tf, om, of, kibps, kibps / 1024
    );
    0
}

/// A GET with `-o FILE`: stream the body straight to the file (no whole-body
/// buffer, so an arbitrarily large download fits in bounded memory).
fn do_download(raw: &str, path: &str, silent: bool) -> i64 {
    let mut f = match File::create(path) {
        Ok(f) => f,
        Err(_) => return die(&format!("cannot create {}", path)),
    };
    let st = match fetch_stream(raw, "GET", &mut f) {
        Ok(s) => s,
        Err(m) => {
            if !silent {
                eprintln!("curl: {}", m);
            }
            return 1;
        }
    };
    if !silent {
        eprintln!(
            "curl: {} -> {} ({} bytes, {})",
            raw, path, st.body_bytes, st.status
        );
    }
    0
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
