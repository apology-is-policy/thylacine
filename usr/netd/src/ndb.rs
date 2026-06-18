//! The compiled-in network database -- an ndb(6) subset (NET-DESIGN s5).
//!
//! netd is a confined warden-bound leaf driver (I-34): it cannot read /lib/ndb
//! (the post-pivot FS), and widening its namespace would fight its I-28/I-34
//! confinement. So at v1.0 it compiles in this canonical ndb -- the
//! capability-microkernel config-at-construction idiom -- and serves /net/cs
//! from it. tools/build.sh bakes a byte-identical copy to /lib/ndb/local
//! (user-readable; the source the v1.x cs/dns daemon split reads live). The
//! DYNAMIC entries (the resolver, the router, the leased address) are NOT here
//! -- they come from DHCP, read live from the lease (net-4b/net-4c).
//!
//! Only the static `sys=`/`dom=` host map and the `service=`/`port=` map are
//! parsed here (what cs needs for non-DNS resolution). The parser is a small,
//! line-based ndb subset; it never allocates and is bounded by the embedded
//! file's size, so a cs query over it is O(file) with no heap.

/// The canonical ndb, embedded at build time. The build bakes a byte-identical
/// copy to /lib/ndb/local.
pub static NDB_LOCAL: &[u8] = include_bytes!("../ndb/local");

/// Resolve a host name to an IPv4 address via the static ndb (`sys=`/`dom=`).
/// None == not a static host: the caller falls through to a numeric parse (and,
/// at net-4b, a DNS query). A matched record with no valid `ip=` yields None.
pub fn lookup_host(name: &[u8]) -> Option<[u8; 4]> {
    if name.is_empty() {
        return None;
    }
    for line in records(NDB_LOCAL) {
        let mut ip: Option<[u8; 4]> = None;
        let mut matched = false;
        for tok in tokens(line) {
            if let Some((attr, val)) = split_eq(tok) {
                match attr {
                    b"ip" => ip = parse_ipv4(val),
                    b"sys" | b"dom" if val == name => matched = true,
                    _ => {}
                }
            }
        }
        if matched {
            return ip;
        }
    }
    None
}

/// Resolve a service name to a port via the static ndb (`service=`/`port=`).
/// None == unknown service: the caller falls through to a numeric port parse.
pub fn lookup_service(name: &[u8]) -> Option<u16> {
    if name.is_empty() {
        return None;
    }
    for line in records(NDB_LOCAL) {
        let mut port: Option<u16> = None;
        let mut matched = false;
        for tok in tokens(line) {
            if let Some((attr, val)) = split_eq(tok) {
                match attr {
                    b"service" if val == name => matched = true,
                    b"port" => port = parse_u16(val),
                    _ => {}
                }
            }
        }
        if matched {
            return port;
        }
    }
    None
}

/// The non-comment, non-blank records of an ndb file (one per line at v1.0).
fn records(text: &[u8]) -> impl Iterator<Item = &[u8]> {
    text.split(|&b| b == b'\n')
        .map(trim)
        .filter(|l| !l.is_empty() && l[0] != b'#')
}

/// The whitespace-separated tokens of a record line.
fn tokens(line: &[u8]) -> impl Iterator<Item = &[u8]> {
    line.split(|&b| b == b' ' || b == b'\t')
        .filter(|t| !t.is_empty())
}

/// Split `attr=value` on the first '='. None if the token has no '='.
fn split_eq(tok: &[u8]) -> Option<(&[u8], &[u8])> {
    let i = tok.iter().position(|&b| b == b'=')?;
    Some((&tok[..i], &tok[i + 1..]))
}

/// Trim ASCII spaces / tabs / CR from both ends.
fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && matches!(s[a], b' ' | b'\t' | b'\r') {
        a += 1;
    }
    while b > a && matches!(s[b - 1], b' ' | b'\t' | b'\r') {
        b -= 1;
    }
    &s[a..b]
}

fn parse_ipv4(s: &[u8]) -> Option<[u8; 4]> {
    let mut out = [0u8; 4];
    let mut parts = s.split(|&b| b == b'.');
    for o in out.iter_mut() {
        *o = parse_u8(parts.next()?)?;
    }
    if parts.next().is_some() {
        return None; // more than 4 octets
    }
    Some(out)
}

fn parse_u8(s: &[u8]) -> Option<u8> {
    match parse_u32(s)? {
        v if v <= 255 => Some(v as u8),
        _ => None,
    }
}

fn parse_u16(s: &[u8]) -> Option<u16> {
    match parse_u32(s)? {
        v if v <= 65535 => Some(v as u16),
        _ => None,
    }
}

fn parse_u32(s: &[u8]) -> Option<u32> {
    if s.is_empty() {
        return None;
    }
    let mut v: u32 = 0;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u32)?;
    }
    Some(v)
}
