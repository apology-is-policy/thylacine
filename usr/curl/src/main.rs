// /bin/curl -- a native libthyla_rs HTTP + HTTPS GET client (net-utils).
//
// Parses a URL ([scheme://]host[:port][/path]; scheme defaults to http),
// resolves the host via net::resolve (/net/cs: numeric -> ndb-static -> DNS),
// dials a `/net` TcpStream (http) or wraps it in the `tls` crate's TlsStream
// (https, validating the server cert against the baked CA bundle + the LS-K wall
// clock), issues an HTTP/1.0 GET, and writes the response body to stdout (or to
// a file with -o). A native /net client: it touches no hardware (netd owns the
// NIC, I-5) and reaches only the /net + /etc its territory grants
// (I-1/I-23/I-28).
//
// Modes:
//   * `curl --selftest` -- the DETERMINISTIC boot gate: a pure URL-parse +
//     request-build + response-parse battery (no network). Proves all the new
//     logic in-guest. (The transport itself -- TcpStream over /net, TlsStream
//     over TcpStream -- is net-8b / net-7c-audited.)
//   * `curl [-I] [-s] [-o FILE] URL` -- the LIVE fetch (best-effort, NOT a boot
//     gate): host-dependent under slirp (needs a reachable server, and for
//     https a trusted chain), so a failure exits non-zero with a logged reason.
//
// An https fetch needs CAP_CSPRNG_READ (the TLS handshake's client random +
// ECDHE key share draw on the kernel CSPRNG); login confers it down the session
// (LOGIN_CAPS), so an interactive shell can run `curl https://...`. A plain http
// fetch needs no capability.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::net::{self, SocketAddrV4, TcpStream};
use libthyla_rs::{eprintln, println};

/// The canonical system root-cert bundle (NET-DESIGN section 9; baked at build).
const CA_BUNDLE: &str = "/etc/ssl/certs/ca-certificates.crt";
/// Cap the response we buffer.
const RESP_CAP: usize = 256 * 1024;

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

// =============================================================================
// URL parsing
// =============================================================================

struct Url {
    https: bool,
    host: String,
    port: u16,
    path: String,
}

/// Parse `[scheme://]host[:port][/path]`. No scheme defaults to http; the
/// default port is 80 (http) / 443 (https). IPv4-only, so `host:port` splits on
/// the last `:` unambiguously (a host never contains one).
fn parse_url(raw: &str) -> Result<Url, String> {
    let (https, rest) = if let Some(r) = raw.strip_prefix("https://") {
        (true, r)
    } else if let Some(r) = raw.strip_prefix("http://") {
        (false, r)
    } else if raw.contains("://") {
        return Err(format!("unsupported scheme in '{}'", raw));
    } else {
        (false, raw)
    };
    // Split the authority (host[:port]) from the path at the first '/'.
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    if authority.is_empty() {
        return Err("empty host".to_string());
    }
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) => {
            let port: u16 = p.parse().map_err(|_| format!("bad port '{}'", p))?;
            if h.is_empty() {
                return Err("empty host".to_string());
            }
            (h.to_string(), port)
        }
        None => (authority.to_string(), if https { 443 } else { 80 }),
    };
    Ok(Url {
        https,
        host,
        port,
        path: path.to_string(),
    })
}

// =============================================================================
// HTTP request / response
// =============================================================================

/// Build a minimal HTTP/1.0 request. `Connection: close` makes the server's EOF
/// the end of the body (no chunked/keep-alive framing to parse at v1.0).
fn build_request(method: &str, host: &str, path: &str) -> String {
    format!(
        "{} {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\nUser-Agent: thylacine-curl/1.0\r\n\r\n",
        method, path, host
    )
}

/// Write the request, then read the whole response (bounded by RESP_CAP) until
/// the peer's EOF.
fn http_exchange<S: Read + Write>(s: &mut S, req: &str) -> Result<Vec<u8>, String> {
    s.write_all(req.as_bytes())
        .map_err(|_| "write request failed".to_string())?;
    let mut resp = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                resp.extend_from_slice(&buf[..n]);
                if resp.len() >= RESP_CAP {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    Ok(resp)
}

/// First occurrence of `needle` in `hay`.
fn find_subseq(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    (0..=hay.len() - needle.len()).find(|&i| &hay[i..i + needle.len()] == needle)
}

/// Split a response into (head, body) at the first blank line. The head holds
/// the status line + headers (no trailing separator); the body is everything
/// after. Tolerates bare-LF separators (`\n\n`) as well as CRLF.
fn split_response(resp: &[u8]) -> (&[u8], &[u8]) {
    if let Some(i) = find_subseq(resp, b"\r\n\r\n") {
        (&resp[..i], &resp[i + 4..])
    } else if let Some(i) = find_subseq(resp, b"\n\n") {
        (&resp[..i], &resp[i + 2..])
    } else {
        (resp, &[])
    }
}

/// The status line (the first CRLF/LF-terminated line of the head).
fn status_line(head: &[u8]) -> &str {
    let end = head
        .iter()
        .position(|&b| b == b'\r' || b == b'\n')
        .unwrap_or(head.len());
    core::str::from_utf8(&head[..end]).unwrap_or("<binary>")
}

// =============================================================================
// driver
// =============================================================================

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
            "--selftest" => return selftest(),
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
    fetch(url, head_only, out_file, silent)
}

/// The live fetch: parse + resolve + connect (+ handshake for https) + GET +
/// emit. Best-effort -- any failure exits non-zero (NOT a boot gate).
fn fetch(raw: &str, head_only: bool, out_file: Option<&str>, silent: bool) -> i64 {
    let log = |m: &str| {
        if !silent {
            eprintln!("curl: {}", m);
        }
        1
    };

    let url = match parse_url(raw) {
        Ok(u) => u,
        Err(m) => return log(&m),
    };
    let addr = match net::resolve(&url.host, url.port) {
        Ok(a) => a,
        Err(_) => return log(&format!("could not resolve host: {}", url.host)),
    };
    let method = if head_only { "HEAD" } else { "GET" };
    let req = build_request(method, &url.host, &url.path);

    let resp = if url.https {
        match https_exchange(addr, &url.host, &req) {
            Ok(v) => v,
            Err(m) => return log(&m),
        }
    } else {
        let mut tcp = match TcpStream::connect(addr) {
            Ok(t) => t,
            Err(_) => return log(&format!("connect to {} failed", addr)),
        };
        match http_exchange(&mut tcp, &req) {
            Ok(v) => v,
            Err(m) => return log(&m),
        }
    };

    let (head, body) = split_response(&resp);
    // -I prints the head (status + headers); a GET prints the body.
    let payload: &[u8] = if head_only { head } else { body };

    if let Some(path) = out_file {
        match File::create(path) {
            Ok(mut f) => {
                if f.write_all(payload).is_err() {
                    return log(&format!("write to {} failed", path));
                }
            }
            Err(_) => return log(&format!("cannot create {}", path)),
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

/// The https leg: read + parse the baked CA bundle, build the rustls config,
/// connect, run the handshake (SNI/cert-validated against `host`), exchange.
fn https_exchange(addr: SocketAddrV4, host: &str, req: &str) -> Result<Vec<u8>, String> {
    let mut f = File::open(CA_BUNDLE).map_err(|_| format!("read {} failed", CA_BUNDLE))?;
    let pem = io::slurp(&mut f).map_err(|_| "read CA bundle failed".to_string())?;
    let roots = tls::load_roots_pem(&pem).map_err(|_| "CA bundle parse failed".to_string())?;
    let cfg = tls::client_config(roots);

    let tcp = TcpStream::connect(addr).map_err(|_| format!("connect to {} failed", addr))?;
    let mut s = tls::TlsStream::connect(tcp, host, cfg)
        .map_err(|e| format!("TLS handshake to {} failed: {}", host, e))?;
    let r = http_exchange(&mut s, req);
    s.close();
    r
}

// =============================================================================
// deterministic self-test (the boot gate)
// =============================================================================

/// Exercise all the pure logic with no network: URL parsing, request building,
/// and response splitting. Exit 0 = every case held.
fn selftest() -> i64 {
    // URL parse battery: (input, https, host, port, path).
    let cases: &[(&str, bool, &str, u16, &str)] = &[
        ("http://example.com/", false, "example.com", 80, "/"),
        ("https://example.com/", true, "example.com", 443, "/"),
        ("example.com", false, "example.com", 80, "/"),
        ("https://host:8443/a/b", true, "host", 8443, "/a/b"),
        (
            "http://10.0.2.2:8000/x?y=1",
            false,
            "10.0.2.2",
            8000,
            "/x?y=1",
        ),
        ("https://127.0.0.1", true, "127.0.0.1", 443, "/"),
    ];
    for (raw, https, host, port, path) in cases {
        match parse_url(raw) {
            Ok(u) => {
                if u.https != *https || u.host != *host || u.port != *port || u.path != *path {
                    eprintln!(
                        "curl: SELFTEST parse mismatch for '{}': got {}://{}:{}{}",
                        raw,
                        if u.https { "https" } else { "http" },
                        u.host,
                        u.port,
                        u.path
                    );
                    return 1;
                }
            }
            Err(m) => {
                eprintln!("curl: SELFTEST parse '{}' errored: {}", raw, m);
                return 1;
            }
        }
    }
    // Reject cases.
    for bad in &["ftp://host/", "://host", "http://"] {
        if parse_url(bad).is_ok() {
            eprintln!("curl: SELFTEST accepted a bad URL '{}'", bad);
            return 1;
        }
    }

    // Request build: the exact wire bytes.
    let req = build_request("GET", "example.com", "/index.html");
    let want = "GET /index.html HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\nUser-Agent: thylacine-curl/1.0\r\n\r\n";
    if req != want {
        eprintln!("curl: SELFTEST request build mismatch");
        return 1;
    }

    // Response split: status + headers + body.
    let resp = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello";
    let (head, body) = split_response(resp);
    if status_line(head) != "HTTP/1.1 200 OK" {
        eprintln!(
            "curl: SELFTEST status-line mismatch: '{}'",
            status_line(head)
        );
        return 1;
    }
    if body != b"hello" {
        eprintln!("curl: SELFTEST body mismatch");
        return 1;
    }
    // A bare-LF-separated response.
    let resp2 = b"HTTP/1.0 404 Not Found\n\nnope";
    let (h2, b2) = split_response(resp2);
    if status_line(h2) != "HTTP/1.0 404 Not Found" || b2 != b"nope" {
        eprintln!("curl: SELFTEST bare-LF split mismatch");
        return 1;
    }

    println!(
        "curl: SELFTEST OK ({} URL cases, request build, response split [CRLF + LF])",
        cases.len()
    );
    0
}
