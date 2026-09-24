// select -- cut's selections from one line: bytes by position, or the fields
// between a delimiter, handed to the caller as they are found. Nothing is
// collected, so a line of any length costs no more than the line itself.

use alloc::vec::Vec;

/// An inclusive 1-based range; `hi == usize::MAX` is open-ended.
pub type Range = (usize, usize);

/// Parse a list such as `1,3` or `2-5` or `4-` or `-3`; None if malformed.
pub fn parse_list(s: &str) -> Option<Vec<Range>> {
    let mut out = Vec::new();
    for part in s.split(',') {
        if part.is_empty() {
            return None;
        }
        if let Some((a, b)) = part.split_once('-') {
            let lo = if a.is_empty() { 1 } else { a.parse().ok()? };
            let hi = if b.is_empty() { usize::MAX } else { b.parse().ok()? };
            if lo == 0 || hi < lo {
                return None;
            }
            out.push((lo, hi));
        } else {
            let n: usize = part.parse().ok()?;
            if n == 0 {
                return None;
            }
            out.push((n, n));
        }
    }
    Some(out)
}

fn selected(pos: usize, ranges: &[Range]) -> bool {
    ranges.iter().any(|&(lo, hi)| pos >= lo && pos <= hi)
}

/// Hand `put` the selected fields of `line` with `delim` between them, or the
/// whole line when it holds no `delim` (as GNU cut prints it). `put` returns
/// false to stop.
pub fn fields(line: &[u8], delim: u8, ranges: &[Range], mut put: impl FnMut(&[u8]) -> bool) {
    if !line.contains(&delim) {
        put(line);
        return;
    }
    let mut first = true;
    for (i, field) in line.split(|&b| b == delim).enumerate() {
        if !selected(i + 1, ranges) {
            continue;
        }
        if !first && !put(&[delim]) {
            return;
        }
        if !put(field) {
            return;
        }
        first = false;
    }
}

/// Hand `put` the selected bytes of `line`, a run of adjacent ones at a time.
/// `put` returns false to stop.
pub fn bytes(line: &[u8], ranges: &[Range], mut put: impl FnMut(&[u8]) -> bool) {
    let mut i = 0;
    while i < line.len() {
        if !selected(i + 1, ranges) {
            i += 1;
            continue;
        }
        let start = i;
        while i < line.len() && selected(i + 1, ranges) {
            i += 1;
        }
        if !put(&line[start..i]) {
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::counting;
    use alloc::vec;

    // What cut printed for a line when it collected the fields first.
    fn whole_fields(line: &[u8], delim: u8, ranges: &[Range]) -> Vec<u8> {
        let fields: Vec<&[u8]> = line.split(|&b| b == delim).collect();
        if fields.len() == 1 {
            return line.to_vec();
        }
        let mut out = Vec::new();
        let mut first = true;
        for (i, f) in fields.iter().enumerate() {
            if selected(i + 1, ranges) {
                if !first {
                    out.push(delim);
                }
                out.extend_from_slice(f);
                first = false;
            }
        }
        out
    }

    fn whole_bytes(line: &[u8], ranges: &[Range]) -> Vec<u8> {
        line.iter().enumerate().filter(|&(i, _)| selected(i + 1, ranges)).map(|(_, &b)| b).collect()
    }

    fn cut(line: &[u8], by_field: bool, ranges: &[Range]) -> Vec<u8> {
        let mut out = Vec::new();
        let put = |p: &[u8]| {
            out.extend_from_slice(p);
            true
        };
        if by_field {
            fields(line, b'\t', ranges, put);
        } else {
            bytes(line, ranges, put);
        }
        out
    }

    const LINES: &[&[u8]] = &[b"", b"a", b"\t", b"a\tb", b"a\tb\tc", b"\t\ta\t", b"abcdef", b"one\ttwo\tthree\tfour"];
    const LISTS: &[&str] = &["1", "2", "1,3", "2-3", "3-", "-2", "1-", "5", "2,1", "1-1,3-4"];

    #[test]
    fn every_selection_as_the_whole_line_gave_it() {
        for line in LINES {
            for list in LISTS {
                let ranges = parse_list(list).unwrap();
                assert_eq!(cut(line, true, &ranges), whole_fields(line, b'\t', &ranges), "-f {} of {:?}", list, line);
                assert_eq!(cut(line, false, &ranges), whole_bytes(line, &ranges), "-c {} of {:?}", list, line);
            }
        }
    }

    #[test]
    fn a_malformed_list_is_refused() {
        for bad in ["", "0", "1,", ",1", "3-2", "a", "1-b", "-0"] {
            assert_eq!(parse_list(bad), None, "{:?}", bad);
        }
        assert_eq!(parse_list("-3"), Some(vec![(1, 3)]));
        assert_eq!(parse_list("4-"), Some(vec![(4, usize::MAX)]));
    }

    #[test]
    fn a_stop_ends_the_selection() {
        let mut got = Vec::new();
        fields(b"a\tb\tc", b'\t', &[(1, usize::MAX)], |p| {
            got.extend_from_slice(p);
            got.len() < 2
        });
        assert_eq!(got, b"a\t");
    }

    #[test]
    fn a_line_of_fields_costs_nothing_to_cut() {
        let line = vec![b'\t'; 1 << 20];
        let ranges = parse_list("1-").unwrap();
        let mut n = 0usize;
        let bytes_asked = counting::allocated(|| {
            fields(&line, b'\t', &ranges, |p| {
                n += p.len();
                true
            })
        });
        assert_eq!(n, 1 << 20, "every delimiter between the empty fields");
        assert_eq!(bytes_asked, 0, "the cut allocated");
    }
}
