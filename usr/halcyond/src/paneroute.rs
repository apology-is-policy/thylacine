// paneroute -- the PURE routing core for the per-pane inline-media channel
// (I-47, HALCYON.md 14.7.2, the session-path slice). The session compositor
// posts ONE per-user 9P service (`/srv/halcyon-<user>`) and distinguishes the
// user's panes by a per-pane SECRET TOKEN carried as a PATH COMPONENT: a
// place-request reaches tile T by walking `<hex(token_of_T)>/place`. This
// module holds only the pure, host-testable parts of that scheme -- the 32-hex
// token codec and the namespace walk -- so the untrusted-name decisions have a
// home the host suite exercises, exactly as `inlineaccum` does for the payload.
// The syscall shell (the 9P server, the routes map, the compositor wiring) is
// `paneplace.rs` (bin) + `session.rs`.
//
// FORMAT-FUZZ SURFACE (audit:hard, I-47). Every byte here is an untrusted 9P
// walk name: `parse_hex32` accepts ONLY the canonical form this module's own
// `hex32` emits (exactly 32 lowercase hex digits), so a token has ONE wire
// spelling and cannot be aliased (mixed case / short / long / non-hex all
// refuse). `walk_child` never resolves a token the caller's `live` predicate
// rejects, so a walk to an unknown or dead pane fails closed (E_NOENT at the
// server) rather than binding a fid to a phantom tile.

#![allow(clippy::manual_range_contains)]

/// A resolved node in the per-user service's namespace. The root dir, a
/// per-pane token directory `<hex>`, or the write-only `place` file under one.
/// The token (u128) is the pane's routing key; the server maps it to a live
/// tile leaf via its routes table (the `live` predicate below is that table's
/// membership test).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Node {
    Root,
    Dir(u128),
    Place(u128),
}

/// The 32-byte canonical wire spelling of a token: 32 lowercase hex digits,
/// most-significant nibble first. The sole form `parse_hex32` accepts, so
/// encode/decode round-trip and a token has no alias.
pub fn hex32(token: u128) -> [u8; 32] {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut out = [0u8; 32];
    let mut i = 0;
    while i < 32 {
        // nibble 0 is the most significant (bits 124..128), nibble 31 the least.
        let shift = (31 - i) * 4;
        let nib = ((token >> shift) & 0xf) as usize;
        out[i] = D[nib];
        i += 1;
    }
    out
}

/// Decode a token from a 9P walk name. `Some` iff `name` is EXACTLY 32 bytes,
/// each a lowercase hex digit -- the canonical form `hex32` emits. Any
/// deviation (length, case, non-hex byte) returns `None`, so the token has a
/// single spelling and a hostile name cannot alias a live pane.
pub fn parse_hex32(name: &[u8]) -> Option<u128> {
    if name.len() != 32 {
        return None;
    }
    let mut v: u128 = 0;
    for &b in name {
        let nib = match b {
            b'0'..=b'9' => (b - b'0') as u128,
            b'a'..=b'f' => (b - b'a' + 10) as u128,
            _ => return None,
        };
        v = (v << 4) | nib;
    }
    Some(v)
}

/// Resolve one walk step. `live(token)` reports whether the token names a pane
/// the server currently routes to (its routes table); an unknown token from
/// the root refuses (fail-closed), so a fid can never bind to a phantom pane.
/// `.` stays; `..` climbs Place -> Dir -> Root (Root is its own parent). A
/// token dir has exactly one child, `place`.
pub fn walk_child(cur: Node, name: &[u8], live: impl Fn(u128) -> bool) -> Option<Node> {
    if name == b"." {
        return Some(cur);
    }
    if name == b".." {
        return Some(match cur {
            Node::Root => Node::Root,
            Node::Dir(_) => Node::Root,
            Node::Place(t) => Node::Dir(t),
        });
    }
    match cur {
        Node::Root => {
            let t = parse_hex32(name)?;
            if live(t) {
                Some(Node::Dir(t))
            } else {
                None
            }
        }
        Node::Dir(t) if name == b"place" => Some(Node::Place(t)),
        _ => None,
    }
}

/// The per-user service's leaf name for a token: `<hex(token)>/place`, the tail
/// a pane's program opens (prefixed by the service root in `/env/HALCYON_PLACE`).
/// Pure, so the compositor's address construction is host-checkable against the
/// server's own walk.
#[cfg(test)]
pub fn place_tail(token: u128) -> alloc::string::String {
    use alloc::string::String;
    let mut s = String::with_capacity(32 + 6);
    for &b in hex32(token).iter() {
        s.push(b as char);
    }
    s.push_str("/place");
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex32_round_trips() {
        for t in [0u128, 1, 0xdead_beef, u128::MAX, 0x0123_4567_89ab_cdef_fedc_ba98_7654_3210] {
            let e = hex32(t);
            assert_eq!(e.len(), 32);
            assert!(e.iter().all(|&b| b.is_ascii_lowercase() || b.is_ascii_digit()));
            assert_eq!(parse_hex32(&e), Some(t), "round trip {:x}", t);
        }
        // The most significant nibble is first (big-endian spelling).
        assert_eq!(&hex32(1), b"00000000000000000000000000000001");
        assert_eq!(&hex32(u128::MAX), b"ffffffffffffffffffffffffffffffff");
    }

    #[test]
    fn parse_hex32_is_canonical_only() {
        assert_eq!(parse_hex32(b""), None, "empty");
        assert_eq!(parse_hex32(b"0"), None, "short");
        assert_eq!(parse_hex32(&[b'0'; 31]), None, "31 digits");
        assert_eq!(parse_hex32(&[b'0'; 33]), None, "33 digits");
        // uppercase is NOT canonical -- one spelling only, no alias.
        assert_eq!(parse_hex32(b"0000000000000000000000000000000A"), None, "uppercase");
        // a non-hex byte anywhere refuses.
        let mut b = hex32(0x1234);
        b[15] = b'g';
        assert_eq!(parse_hex32(&b), None, "non-hex byte");
        b[15] = b' ';
        assert_eq!(parse_hex32(&b), None, "space");
    }

    #[test]
    fn walk_resolves_only_live_tokens() {
        let live_set = [0xaau128, 0xbb];
        let live = |t: u128| live_set.contains(&t);
        let tok = hex32(0xaa);
        let dead = hex32(0xcc);

        // root -> live token dir -> place
        assert_eq!(walk_child(Node::Root, &tok, live), Some(Node::Dir(0xaa)));
        assert_eq!(walk_child(Node::Dir(0xaa), b"place", live), Some(Node::Place(0xaa)));
        // root -> dead token: refused (fail-closed), never a phantom Dir.
        assert_eq!(walk_child(Node::Root, &dead, live), None);
        // a place file has no children; a token dir has ONLY `place`.
        assert_eq!(walk_child(Node::Place(0xaa), b"place", live), None);
        assert_eq!(walk_child(Node::Dir(0xaa), b"other", live), None);
        // a malformed name at root refuses before the liveness test even runs.
        assert_eq!(walk_child(Node::Root, b"notatoken", live), None);
    }

    #[test]
    fn walk_dot_and_dotdot() {
        let live = |_| true;
        assert_eq!(walk_child(Node::Dir(7), b".", live), Some(Node::Dir(7)));
        assert_eq!(walk_child(Node::Place(7), b"..", live), Some(Node::Dir(7)));
        assert_eq!(walk_child(Node::Dir(7), b"..", live), Some(Node::Root));
        assert_eq!(walk_child(Node::Root, b"..", live), Some(Node::Root));
    }

    #[test]
    fn place_tail_matches_the_walk() {
        let t = 0xdead_beef_u128;
        let tail = place_tail(t);
        assert_eq!(tail, "000000000000000000000000deadbeef/place");
        // and the tail's first component parses back to the token.
        let comp = tail.split('/').next().unwrap();
        assert_eq!(parse_hex32(comp.as_bytes()), Some(t));
    }
}
