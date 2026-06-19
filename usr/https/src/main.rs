// /bin/https -- a native libthyla_rs HTTPS GET client (NET-DESIGN section 9).
//
// net-7c-2. Builds a rustls client over a `/net` `TcpStream` (the `tls` crate's
// `TlsStream`), validating the server certificate against the baked CA bundle
// (`/etc/ssl/certs/ca-certificates.crt`) and the LS-K wall clock, then issues an
// HTTP/1.0 GET. A native `/net` client (the libthyla_rs analog of curl-over-
// pouch): it touches no hardware (netd owns the NIC, I-5) and reaches only the
// `/net` + `/etc` its territory grants (I-1/I-23/I-28).
//
// Modes:
//   * `https` / `https --selftest` -- the DETERMINISTIC boot probe (the gate):
//     read + parse the baked CA bundle into a non-empty trust store and build a
//     client config. Proves the bundle baked, `load_roots_pem` parses it, and
//     the provider + roots compose in-guest. No network. (The full handshake +
//     cert-verification proof is `tls-smoke`'s loopback E2E.)
//   * `https <host> [path]` -- the LIVE fetch (best-effort, NOT a boot gate):
//     resolve `host` (a dotted-quad connects directly; a name via `/net/cs`),
//     run the TLS handshake, GET `path` (default `/`), and print the status line
//     + byte count. Host-dependent under slirp (needs a reachable server + a
//     trusted chain), so a failure exits non-zero with a logged reason.
//
// Must be spawned WITH `CAP_CSPRNG_READ` (the handshake's key share draws on the
// kernel CSPRNG; net-7c-1).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpStream};
use libthyla_rs::t_putstr;

/// The canonical system root-cert bundle path (NET-DESIGN section 9; baked at
/// build time, the host-bake idiom).
const CA_BUNDLE: &str = "/etc/ssl/certs/ca-certificates.crt";
const HTTPS_PORT: u16 = 443;
/// Cap the response we buffer (a best-effort fetch -- we print the head + size,
/// not the whole body).
const RESP_CAP: usize = 64 * 1024;

fn fail(msg: &str) -> i64 {
    t_putstr(msg);
    1
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    match args.get_str(1) {
        None | Some("--selftest") => selftest(),
        Some(host) => {
            let path = args.get_str(2).unwrap_or("/");
            live_get(host, path)
        }
    }
}

/// Read the baked CA bundle (bounded; it is ~300 KiB of PEM).
fn read_bundle() -> Result<Vec<u8>, libthyla_rs::err::Error> {
    let mut f = File::open(CA_BUNDLE)?;
    io::slurp(&mut f)
}

/// The deterministic boot gate: the baked CA bundle reads, parses into a
/// non-empty trust store, and a client config builds from it. Exit 0 = held.
fn selftest() -> i64 {
    let pem = match read_bundle() {
        Ok(p) => p,
        Err(e) => return fail(&format!("https: read {} failed ({:?})\n", CA_BUNDLE, e)),
    };
    let roots = match tls::load_roots_pem(&pem) {
        Ok(r) => r,
        Err(_) => return fail("https: CA bundle parsed 0 trust anchors\n"),
    };
    let n = roots.len();
    if n == 0 {
        return fail("https: empty trust store\n");
    }
    // Building the config proves the RustCrypto provider + the parsed roots
    // compose (the same path the live fetch takes).
    let _cfg = tls::client_config(roots);
    t_putstr(&format!(
        "https: SELFTEST OK (baked CA bundle: {} trust anchors, client config built)\n",
        n
    ));
    0
}

/// Resolve `host` to an `IPv4:port`. A dotted-quad connects directly on 443; a
/// name goes through `/net/cs` (numeric -> ndb -> DNS; best-effort under slirp).
fn resolve(host: &str) -> Result<SocketAddrV4, String> {
    if let Ok(ip) = Ipv4Addr::parse(host) {
        return Ok(SocketAddrV4::new(ip, HTTPS_PORT));
    }
    let mut cs = OpenOptions::new()
        .read(true)
        .write(true)
        .open("/net/cs")
        .map_err(|_| String::from("open /net/cs failed (netd down?)"))?;
    // cs dial notation: proto!host!service. The numeric 443 avoids depending on
    // an `https` service entry in the ndb.
    cs.write_all(format!("tcp!{}!{}", host, HTTPS_PORT).as_bytes())
        .map_err(|_| String::from("/net/cs write failed"))?;
    let mut buf = [0u8; 128];
    let k = cs
        .read(&mut buf)
        .map_err(|_| String::from("/net/cs read failed"))?;
    if k == 0 {
        return Err(String::from("name did not resolve (empty /net/cs reply)"));
    }
    // Reply: `<clone-file> <ip>!<port>`. Take the second field's ip!port.
    let line =
        core::str::from_utf8(&buf[..k]).map_err(|_| String::from("/net/cs reply not utf8"))?;
    let dial = line
        .split_whitespace()
        .nth(1)
        .ok_or_else(|| String::from("name did not resolve"))?;
    let (ip_s, port_s) = dial
        .rsplit_once('!')
        .ok_or_else(|| String::from("malformed /net/cs reply"))?;
    let ip = Ipv4Addr::parse(ip_s).map_err(|_| String::from("bad ip in /net/cs reply"))?;
    let port: u16 = port_s
        .trim()
        .parse()
        .map_err(|_| String::from("bad port in /net/cs reply"))?;
    Ok(SocketAddrV4::new(ip, port))
}

/// The first CRLF/LF-terminated line of `resp`, as text (the HTTP status line).
fn first_line(resp: &[u8]) -> &str {
    let end = resp
        .iter()
        .position(|&b| b == b'\r' || b == b'\n')
        .unwrap_or(resp.len());
    core::str::from_utf8(&resp[..end]).unwrap_or("<binary>")
}

/// The live fetch: resolve, handshake (validating the cert), GET, print the
/// status line + byte count. Best-effort -- any failure exits non-zero with a
/// logged reason (NOT a boot gate; joey runs the selftest).
fn live_get(host: &str, path: &str) -> i64 {
    let addr = match resolve(host) {
        Ok(a) => a,
        Err(m) => return fail(&format!("https: resolve {} failed: {}\n", host, m)),
    };

    let pem = match read_bundle() {
        Ok(p) => p,
        Err(_) => return fail("https: read CA bundle failed\n"),
    };
    let roots = match tls::load_roots_pem(&pem) {
        Ok(r) => r,
        Err(_) => return fail("https: CA bundle parse failed\n"),
    };
    let cfg = tls::client_config(roots);

    let tcp = match TcpStream::connect(addr) {
        Ok(t) => t,
        Err(_) => {
            return fail(&format!(
                "https: TCP connect to {} failed (best-effort; host-dependent)\n",
                addr
            ))
        }
    };
    // The SNI / cert-validation name is the original host (a dotted-quad SNI
    // validates only against an IP-SAN cert, which is the correct behavior).
    let mut tls = match tls::TlsStream::connect(tcp, host, cfg) {
        Ok(s) => s,
        Err(e) => {
            return fail(&format!(
                "https: TLS handshake to {} failed: {} (best-effort)\n",
                host, e
            ))
        }
    };

    let req = format!(
        "GET {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\nUser-Agent: thylacine-https/1.0\r\n\r\n",
        path, host
    );
    if tls.write_all(req.as_bytes()).is_err() {
        return fail("https: write request failed\n");
    }

    let mut resp = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        match tls.read(&mut buf) {
            Ok(0) => break, // clean TLS EOF
            Ok(n) => {
                resp.extend_from_slice(&buf[..n]);
                if resp.len() >= RESP_CAP {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    tls.close();

    t_putstr(&format!(
        "https: {}{} -> {} ({} bytes)\n",
        host,
        path,
        first_line(&resp),
        resp.len()
    ));
    0
}
