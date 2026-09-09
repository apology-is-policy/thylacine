// Dial-string parsing, pure and host-tested.
//
// This exists because forage advertised one syntax and implemented another.
// Its usage line, its doc comments and its design doc all said Plan 9's
// `host!port` -- the form every dial-style tool in this lineage takes, and the
// form the tree uses everywhere internally (`tcp!127.0.0.1!80`). But the code
// handed the string straight to `SocketAddrV4::parse`, which wants
// `a.b.c.d:port`, so the documented form was rejected:
//
//     forage: address (want host!port)
//
// Nothing caught it, because until the npxf channel gave forage a server to
// talk to, NO BYTE HAD EVER CROSSED IT. A whole surface can agree with itself
// about a syntax it does not implement, as long as nothing ever runs it.
//
// Both forms are accepted. `!` is the documented one and the lineage's; `:` is
// what a person arrives with from every other system, and refusing it would be
// pedantry rather than design.

/// Split a dial string into its host and port halves.
///
/// Accepts `host!port` and `host:port`. Splits from the RIGHT so a host
/// containing the separator fails at the address parse -- with the host visible
/// in the message -- rather than being silently truncated into something that
/// happens to parse.
pub fn split_dial(s: &str) -> Option<(&str, &str)> {
    let cut = s.rfind(['!', ':'])?;
    let (host, rest) = s.split_at(cut);
    let port = &rest[1..];
    if host.is_empty() || port.is_empty() {
        return None;
    }
    Some((host, port))
}

/// Parse the port half. Rejects anything that is not a bare 1..=65535.
///
/// Port 0 is refused: it means "any port" to a bind and nothing at all to a
/// dial, so accepting it would turn a typo into a confusing connect failure.
pub fn parse_port(s: &str) -> Option<u16> {
    if s.is_empty() || s.len() > 5 {
        return None;
    }
    let mut v: u32 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u32;
    }
    if v == 0 || v > 65535 {
        return None;
    }
    Some(v as u16)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_plan9_form_parses() {
        assert_eq!(split_dial("10.0.2.100!7830"), Some(("10.0.2.100", "7830")));
        assert_eq!(split_dial("127.0.0.1!5640"), Some(("127.0.0.1", "5640")));
    }

    #[test]
    fn the_colon_form_parses_too() {
        assert_eq!(split_dial("10.0.2.100:7830"), Some(("10.0.2.100", "7830")));
    }

    /// Split from the RIGHT. A Plan 9 network prefix (`tcp!host!port`) leaves
    /// `tcp!host` as the host, which then fails the address parse VISIBLY --
    /// far better than silently dialling something else.
    #[test]
    fn a_network_prefix_leaves_a_host_that_will_fail_visibly() {
        assert_eq!(split_dial("tcp!10.0.2.100!7830"), Some(("tcp!10.0.2.100", "7830")));
    }

    #[test]
    fn a_missing_half_is_refused() {
        assert_eq!(split_dial("10.0.2.100"), None);
        assert_eq!(split_dial("10.0.2.100!"), None);
        assert_eq!(split_dial("!7830"), None);
        assert_eq!(split_dial(""), None);
        assert_eq!(split_dial("!"), None);
    }

    #[test]
    fn ports_are_bounded() {
        assert_eq!(parse_port("1"), Some(1));
        assert_eq!(parse_port("7830"), Some(7830));
        assert_eq!(parse_port("65535"), Some(65535));
        // Zero is not a dial target.
        assert_eq!(parse_port("0"), None);
        // Past the field.
        assert_eq!(parse_port("65536"), None);
        assert_eq!(parse_port("999999"), None);
        // Not a number.
        assert_eq!(parse_port(""), None);
        assert_eq!(parse_port("80a"), None);
        assert_eq!(parse_port("-1"), None);
        assert_eq!(parse_port(" 80"), None);
    }
}
