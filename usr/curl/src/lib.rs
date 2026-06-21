// The shared HTTP/HTTPS client engine for the `curl` + `wget` net-utils.
//
// A pure URL + HTTP/1.0 codec (transport-agnostic, generic over Read+Write)
// plus a `fetch` engine that wires the /net transport: a TcpStream for http, the
// `tls` crate's TlsStream for https (validating the server cert against the
// baked CA bundle + the LS-K wall clock). Both tools are thin frontends over
// this lib, so the URL parsing + HTTP exchange live in exactly one place.
//
// netd owns the NIC (I-5), so a client touches no hardware and reaches only the
// /net + /etc its territory grants (I-1/I-23/I-28). An https fetch needs
// CAP_CSPRNG_READ (the handshake key share); http needs no capability.

#![no_std]

extern crate alloc;

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::net::{self, SocketAddrV4, TcpStream};
use libthyla_rs::time::Instant;

/// The canonical system root-cert bundle (NET-DESIGN section 9; baked at build).
pub const CA_BUNDLE: &str = "/etc/ssl/certs/ca-certificates.crt";
/// Cap the response we buffer.
pub const RESP_CAP: usize = 256 * 1024;

// =============================================================================
// URL parsing
// =============================================================================

pub struct Url {
    pub https: bool,
    pub host: String,
    pub port: u16,
    pub path: String,
}

/// Parse `[scheme://]host[:port][/path]`. No scheme defaults to http; the
/// default port is 80 (http) / 443 (https). IPv4-only, so `host:port` splits on
/// the last `:` unambiguously (a host never contains one).
pub fn parse_url(raw: &str) -> Result<Url, String> {
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

/// The last non-empty path segment of `raw` (wget's default output name); a
/// path-less or `/`-only URL yields `index.html`. Any query string (`?...`) is
/// dropped.
pub fn url_basename(raw: &str) -> String {
    let path = match parse_url(raw) {
        Ok(u) => u.path,
        Err(_) => return "index.html".to_string(),
    };
    let path = path.split('?').next().unwrap_or(&path);
    match path.rsplit('/').find(|seg| !seg.is_empty()) {
        Some(seg) => seg.to_string(),
        None => "index.html".to_string(),
    }
}

// =============================================================================
// HTTP request / response
// =============================================================================

/// Build a minimal HTTP/1.0 request. `Connection: close` makes the server's EOF
/// the end of the body (no chunked/keep-alive framing to parse at v1.0).
pub fn build_request(method: &str, host: &str, path: &str) -> String {
    format!(
        "{} {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\nUser-Agent: thylacine-curl/1.0\r\n\r\n",
        method, path, host
    )
}

/// Write the request, then read the whole response (bounded by RESP_CAP) until
/// the peer's EOF.
pub fn http_exchange<S: Read + Write>(s: &mut S, req: &str) -> Result<Vec<u8>, String> {
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

// =============================================================================
// streaming transfer + timing (the download path: no whole-body buffering)
// =============================================================================

/// A `Write` sink that counts the bytes written and discards them -- the
/// streaming-discard target for the bench / a `-o /dev/null` download, so a
/// large transfer never accumulates in memory (the `http_exchange` RESP_CAP path
/// is for small fetches the caller wants to inspect).
#[derive(Default)]
pub struct CountSink {
    pub n: u64,
}
impl Write for CountSink {
    // io::Result is not re-exported publicly; err::Result is the same (public) type.
    fn write(&mut self, buf: &[u8]) -> libthyla_rs::err::Result<usize> {
        self.n += buf.len() as u64;
        Ok(buf.len())
    }
    fn flush(&mut self) -> libthyla_rs::err::Result<()> {
        Ok(())
    }
}

/// Timing + size of a streamed transfer. All times are nanoseconds from the
/// start of `fetch_stream` (so `connect_ns` covers resolve+connect+handshake,
/// `ttfb_ns` is to the first body byte, `total_ns` is to EOF).
pub struct StreamStats {
    pub status: String,
    pub body_bytes: u64,
    pub connect_ns: u64,
    pub ttfb_ns: u64,
    pub total_ns: u64,
}

/// The head we will buffer before declaring a malformed response (a normal
/// status line + headers is a few hundred bytes; 64 KiB is a generous ceiling).
const HEAD_CAP: usize = 64 * 1024;

/// Write `req`, parse the response head, then STREAM the body to `sink`
/// chunk-by-chunk (no whole-body buffer), counting body bytes + recording the
/// time-to-first-body-byte relative to `start`. Returns (status_line,
/// body_bytes, ttfb_ns). Tolerates a head split across reads and a CRLF or
/// bare-LF blank line.
pub fn http_stream<S: Read + Write, W: Write>(
    s: &mut S,
    req: &str,
    sink: &mut W,
    start: Instant,
) -> Result<(String, u64, u64), String> {
    s.write_all(req.as_bytes())
        .map_err(|_| "write request failed".to_string())?;
    let mut head: Vec<u8> = Vec::new();
    let mut buf = [0u8; 16384];
    let mut body_bytes: u64 = 0;
    let mut ttfb_ns: u64 = 0;
    let mut in_body = false;
    loop {
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                if in_body {
                    if body_bytes == 0 {
                        ttfb_ns = start.elapsed().as_nanos() as u64;
                    }
                    sink.write_all(&buf[..n])
                        .map_err(|_| "sink write failed".to_string())?;
                    body_bytes += n as u64;
                    continue;
                }
                head.extend_from_slice(&buf[..n]);
                let sep = find_subseq(&head, b"\r\n\r\n")
                    .map(|i| (i, 4usize))
                    .or_else(|| find_subseq(&head, b"\n\n").map(|i| (i, 2usize)));
                if let Some((i, sep_len)) = sep {
                    let body_start = i + sep_len;
                    if head.len() > body_start {
                        ttfb_ns = start.elapsed().as_nanos() as u64;
                        let part = head[body_start..].to_vec();
                        sink.write_all(&part)
                            .map_err(|_| "sink write failed".to_string())?;
                        body_bytes += part.len() as u64;
                    }
                    head.truncate(i); // keep only the header bytes for the status line
                    in_body = true;
                } else if head.len() > HEAD_CAP {
                    return Err("response head too large".to_string());
                }
            }
            Err(_) => break,
        }
    }
    Ok((status_line(&head).to_string(), body_bytes, ttfb_ns))
}

/// Parse + resolve + connect (+ TLS handshake for https) + stream the body to
/// `sink`, returning the timing + size. The download path (vs `fetch`, which
/// buffers a small response). `method` is "GET" or "HEAD".
pub fn fetch_stream<W: Write>(
    raw: &str,
    method: &str,
    sink: &mut W,
) -> Result<StreamStats, String> {
    let start = Instant::now();
    let url = parse_url(raw)?;
    let addr = net::resolve(&url.host, url.port)
        .map_err(|_| format!("could not resolve host: {}", url.host))?;
    let req = build_request(method, &url.host, &url.path);
    let (status, body_bytes, ttfb_ns, connect_ns) = if url.https {
        let mut f = File::open(CA_BUNDLE).map_err(|_| format!("read {} failed", CA_BUNDLE))?;
        let pem = io::slurp(&mut f).map_err(|_| "read CA bundle failed".to_string())?;
        let roots = tls::load_roots_pem(&pem).map_err(|_| "CA bundle parse failed".to_string())?;
        let cfg = tls::client_config(roots);
        let tcp = TcpStream::connect(addr).map_err(|_| format!("connect to {} failed", addr))?;
        let mut s = tls::TlsStream::connect(tcp, &url.host, cfg)
            .map_err(|e| format!("TLS handshake to {} failed: {}", url.host, e))?;
        let connect_ns = start.elapsed().as_nanos() as u64; // resolve+connect+handshake
        let (st, bb, ttfb) = http_stream(&mut s, &req, sink, start)?;
        s.close();
        (st, bb, ttfb, connect_ns)
    } else {
        let mut tcp =
            TcpStream::connect(addr).map_err(|_| format!("connect to {} failed", addr))?;
        let connect_ns = start.elapsed().as_nanos() as u64; // resolve+connect
        let (st, bb, ttfb) = http_stream(&mut tcp, &req, sink, start)?;
        (st, bb, ttfb, connect_ns)
    };
    Ok(StreamStats {
        status,
        body_bytes,
        connect_ns,
        ttfb_ns,
        total_ns: start.elapsed().as_nanos() as u64,
    })
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
pub fn split_response(resp: &[u8]) -> (&[u8], &[u8]) {
    if let Some(i) = find_subseq(resp, b"\r\n\r\n") {
        (&resp[..i], &resp[i + 4..])
    } else if let Some(i) = find_subseq(resp, b"\n\n") {
        (&resp[..i], &resp[i + 2..])
    } else {
        (resp, &[])
    }
}

/// The status line (the first CRLF/LF-terminated line of the head).
pub fn status_line(head: &[u8]) -> &str {
    let end = head
        .iter()
        .position(|&b| b == b'\r' || b == b'\n')
        .unwrap_or(head.len());
    core::str::from_utf8(&head[..end]).unwrap_or("<binary>")
}

// =============================================================================
// the fetch engine
// =============================================================================

/// Parse + resolve + connect (+ TLS handshake for https) + exchange. Returns the
/// raw response bytes (head + body); the caller splits + emits. `method` is
/// "GET" or "HEAD".
pub fn fetch(raw: &str, method: &str) -> Result<Vec<u8>, String> {
    let url = parse_url(raw)?;
    let addr = net::resolve(&url.host, url.port)
        .map_err(|_| format!("could not resolve host: {}", url.host))?;
    let req = build_request(method, &url.host, &url.path);
    if url.https {
        https_exchange(addr, &url.host, &req)
    } else {
        let mut tcp =
            TcpStream::connect(addr).map_err(|_| format!("connect to {} failed", addr))?;
        http_exchange(&mut tcp, &req)
    }
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
// deterministic self-test (the boot gate for the curl bin)
// =============================================================================

/// Exercise all the pure logic with no network: URL parsing (+ basename), the
/// reject cases, request building, and response splitting (CRLF + LF). Returns
/// the number of URL cases on success.
pub fn selftest() -> Result<usize, String> {
    // (input, https, host, port, path).
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
        let u = parse_url(raw).map_err(|m| format!("parse '{}' errored: {}", raw, m))?;
        if u.https != *https || u.host != *host || u.port != *port || u.path != *path {
            return Err(format!("parse mismatch for '{}'", raw));
        }
    }
    for bad in &["ftp://host/", "://host", "http://"] {
        if parse_url(bad).is_ok() {
            return Err(format!("accepted a bad URL '{}'", bad));
        }
    }

    // wget's default-output naming.
    let names: &[(&str, &str)] = &[
        ("http://host/dir/file.tar.gz", "file.tar.gz"),
        ("https://host/", "index.html"),
        ("host", "index.html"),
        ("http://host/a/b/page.html?x=1", "page.html"),
    ];
    for (raw, want) in names {
        let got = url_basename(raw);
        if got != *want {
            return Err(format!("basename('{}') = '{}', want '{}'", raw, got, want));
        }
    }

    // Request build: the exact wire bytes.
    let req = build_request("GET", "example.com", "/index.html");
    let want = "GET /index.html HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\nUser-Agent: thylacine-curl/1.0\r\n\r\n";
    if req != want {
        return Err("request build mismatch".to_string());
    }

    // Response split: status + headers + body (CRLF), then bare-LF.
    let resp = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello";
    let (head, body) = split_response(resp);
    if status_line(head) != "HTTP/1.1 200 OK" || body != b"hello" {
        return Err("CRLF response split mismatch".to_string());
    }
    let resp2 = b"HTTP/1.0 404 Not Found\n\nnope";
    let (h2, b2) = split_response(resp2);
    if status_line(h2) != "HTTP/1.0 404 Not Found" || b2 != b"nope" {
        return Err("bare-LF response split mismatch".to_string());
    }

    Ok(cases.len())
}
