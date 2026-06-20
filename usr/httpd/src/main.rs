// httpd -- a tiny static HTTP/1.1 file server over the native /net stack. Serves
// files from a namespace subtree; the response body is STREAMED (chunked file
// reads + backpressure-aware sends), so it serves files far larger than the
// userspace heap -- which is the point for the throughput arc: a host
// `curl http://<guest>/big.bin` downloads a real file over the real NIC and the
// real protocol, giving the over-the-wire MB/s that netd's loopback micro-bench
// (netperf) cannot produce. It is also the first "Thylacine serves the web".
//
//   httpd [-p PORT] [--color[=WHEN]] [DIR]   -- serve DIR (default /) on PORT
//                                               (default 8080)
//
// Connections are served one at a time (the netd listener backlog is 1). HTTP/1.0
// semantics: one request per connection, Connection: close. GET + HEAD only.
// netd owns the NIC (I-5); httpd touches no hardware and serves only files its
// territory grants + its identity may read (I-1/I-22/I-23/I-28) -- the namespace
// IS the sandbox, and `..` traversal is rejected on top of that.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{netpump, palette};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, OpenOptions};
use libthyla_rs::io::Read;
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: httpd [-p PORT] [--color[=WHEN]] [DIR]
  Serve files under DIR (default /) over HTTP/1.1 on PORT (default 8080).
  GET and HEAD only; one connection at a time; Connection: close.
  -p PORT         listen port (default 8080)
  --color[=WHEN]  colorize the access log: always (default) | never | auto
  --help          show this help

Examples:
  httpd                       # serve / on :8080
  httpd -p 80 /srv/www        # serve /srv/www on :80
  # throughput: httpd -p 9999 /srv/www, then host: curl http://<guest>/big.bin
";

// Request-line read cap + the body-streaming chunk size.
const REQ_CAP: usize = 2048;
const CHUNK: usize = 16 * 1024;

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut port: u16 = 8080;
    let mut dir = "/";
    let mut got_dir = false;

    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if a == "--color" {
            mode = ColorMode::Always;
            continue;
        }
        if let Some(w) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(w) {
                Some(m) => mode = m,
                None => return coreutils::usage::die("httpd", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        if a == "-p" {
            match args.get_str(i).and_then(parse_port) {
                Some(p) => {
                    port = p;
                    i += 1;
                }
                None => return coreutils::usage::die("httpd", "-p needs a port (1..=65535)"),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        if a.len() > 1 && a.as_bytes()[0] == b'-' {
            return coreutils::usage::die("httpd", &format!("invalid option -- '{}'", a));
        }
        if got_dir {
            return coreutils::usage::die("httpd", "too many operands");
        }
        dir = a;
        got_dir = true;
    }

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let emb = color::col(palette::EMBER, on);

    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port)) {
        Ok(l) => l,
        Err(_) => {
            eprintln!("{}httpd:{} cannot announce port {} (is netd running?)", emb, rst, port);
            return 1;
        }
    };
    println!("{}httpd:{} serving {}{}{} on {}*:{}{}", dim, rst, gold, dir, rst, gold, port, rst);

    loop {
        let (mut stream, peer) = match listener.accept() {
            Ok(x) => x,
            Err(_) => {
                eprintln!("{}httpd:{} accept failed", emb, rst);
                return 1;
            }
        };
        handle(&mut stream, dir, &peer_str(peer), on);
        // stream drops here -> the connection closes (Connection: close).
    }
}

/// Serve one connection: read the request line, dispatch GET/HEAD, log the
/// result. Best-effort -- any read/write error just drops the connection.
fn handle(stream: &mut TcpStream, dir: &str, peer: &str, on: bool) {
    let ready = match stream.ready_fd() {
        Ok(r) => r,
        Err(_) => return,
    };
    let rfd = ready.as_raw_fd();

    // Read up to the request line terminator (CRLF) or the cap.
    let mut buf = [0u8; REQ_CAP];
    let mut got = 0usize;
    let line_end = loop {
        if let Some(p) = find_crlf(&buf[..got]) {
            break p;
        }
        if got == buf.len() {
            respond_error(stream, rfd, 400, "Bad Request");
            log(stream_log("?", "?", 400, 0, peer, on));
            return;
        }
        match stream.read(&mut buf[got..]) {
            Ok(0) => return, // client closed before sending a request
            Ok(n) => got += n,
            Err(_) => return,
        }
    };

    let line = core::str::from_utf8(&buf[..line_end]).unwrap_or("");
    let mut parts = line.split(' ');
    let method = parts.next().unwrap_or("");
    let path = parts.next().unwrap_or("/");

    let (code, sent) = match method {
        "GET" => serve(stream, rfd, dir, path, false),
        "HEAD" => serve(stream, rfd, dir, path, true),
        _ => {
            respond_error(stream, rfd, 405, "Method Not Allowed");
            (405, 0)
        }
    };
    log(stream_log(method, path, code, sent, peer, on));
}

/// Serve a GET/HEAD for `path` under `dir`. Returns `(status, body_bytes_sent)`.
/// Streams the body in CHUNK-sized reads so an arbitrarily large file never has
/// to be resident. The status line is committed before the body, so a mid-stream
/// error cannot change the status (best-effort, like any HTTP/1.0 server).
fn serve(stream: &mut TcpStream, rfd: i32, dir: &str, path: &str, head_only: bool) -> (u16, u64) {
    let full = match sanitize(dir, path) {
        Some(f) => f,
        None => {
            respond_error(stream, rfd, 400, "Bad Request");
            return (400, 0);
        }
    };
    let md = match fs::metadata(&full) {
        Ok(m) => m,
        Err(_) => {
            respond_error(stream, rfd, 404, "Not Found");
            return (404, 0);
        }
    };
    if md.is_dir() {
        // No directory listing at v1.0; a directory is a 404 unless it has an
        // index (sanitize already mapped "/" -> "/index.html").
        respond_error(stream, rfd, 404, "Not Found");
        return (404, 0);
    }

    let len = md.len();
    let hdr = format!(
        "HTTP/1.0 200 OK\r\nContent-Length: {}\r\nContent-Type: {}\r\nConnection: close\r\n\r\n",
        len,
        content_type(&full)
    );
    if !netpump::send_all(stream, hdr.as_bytes(), rfd) {
        return (200, 0);
    }
    if head_only {
        return (200, 0);
    }

    let mut file = match OpenOptions::new().read(true).open(&full) {
        Ok(f) => f,
        Err(_) => return (200, 0), // header already committed; nothing more to do
    };
    let mut chunk = [0u8; CHUNK];
    let mut sent = 0u64;
    loop {
        match file.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                if !netpump::send_all(stream, &chunk[..n], rfd) {
                    break;
                }
                sent += n as u64;
            }
            Err(_) => break,
        }
    }
    (200, sent)
}

/// Send a minimal `text/plain` error response.
fn respond_error(stream: &mut TcpStream, rfd: i32, code: u16, reason: &str) {
    let body = format!("{} {}\n", code, reason);
    let hdr = format!(
        "HTTP/1.0 {} {}\r\nContent-Length: {}\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n",
        code,
        reason,
        body.len()
    );
    let _ = netpump::send_all(stream, hdr.as_bytes(), rfd);
    let _ = netpump::send_all(stream, body.as_bytes(), rfd);
}

/// Map a request path under `dir` to a namespace path, rejecting traversal.
/// Strips the query string, requires a leading `/`, refuses any `..` component,
/// and maps `/` to `/index.html`. Returns `None` for a path to reject (400).
fn sanitize(dir: &str, raw: &str) -> Option<String> {
    if !raw.starts_with('/') {
        return None;
    }
    let path = raw.split('?').next().unwrap_or(raw);
    if path.split('/').any(|c| c == "..") {
        return None;
    }
    let path = if path == "/" { "/index.html" } else { path };
    let base = dir.trim_end_matches('/');
    Some(format!("{}{}", base, path))
}

/// Guess a Content-Type from the file's extension (basename's last dot).
fn content_type(p: &str) -> &'static str {
    let name = p.rsplit('/').next().unwrap_or(p);
    match name.rsplit('.').next().unwrap_or("") {
        "html" | "htm" => "text/html; charset=utf-8",
        "txt" | "md" | "log" => "text/plain; charset=utf-8",
        "css" => "text/css",
        "js" => "application/javascript",
        "json" => "application/json",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "svg" => "image/svg+xml",
        "wasm" => "application/wasm",
        _ => "application/octet-stream",
    }
}

/// A TCP port is 1..=65535.
fn parse_port(s: &str) -> Option<u16> {
    match s.parse::<u16>() {
        Ok(p) if p > 0 => Some(p),
        _ => None,
    }
}

/// `peer` as `ip:port` text (a small heap string for the log line).
fn peer_str(peer: SocketAddrV4) -> String {
    format!("{}", peer)
}

/// Find the first `\r\n` in `b`, returning the index of the `\r`.
fn find_crlf(b: &[u8]) -> Option<usize> {
    let mut i = 0;
    while i + 1 < b.len() {
        if b[i] == b'\r' && b[i + 1] == b'\n' {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Build the colored access-log line for one request.
fn stream_log(method: &str, path: &str, code: u16, sent: u64, peer: &str, on: bool) -> String {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let fg = color::col(palette::FG, on);
    let gold = color::col(palette::GOLD, on);
    let code_color = if code == 200 {
        color::col(palette::GREEN, on)
    } else {
        color::col(palette::EMBER, on)
    };
    format!(
        "{}httpd:{} {}{} {}{} {}->{} {}{}{} {}({} B){} {}{}{}",
        dim, rst, fg, method, fg, path, dim, rst, code_color, code, rst, dim, sent, rst, gold, peer, rst
    )
}

/// Emit a prebuilt log line to stdout.
fn log(line: String) {
    println!("{}", line);
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
