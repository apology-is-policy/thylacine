// find -- the literal search grep runs over a line: a byte string, not a
// regular expression, optionally ignoring ASCII case and bounded to whole
// words.
//
// A match is handed to the caller as it is found, never collected: a line
// holds as many matches as it has bytes, so a list of them would outgrow the
// line it came from, and the line is the bound (`stream::LINE_MAX`).

fn is_word_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// A `[s,e)` match is word-bounded when neither neighbour is a word byte.
fn word_bounded(hay: &[u8], s: usize, e: usize) -> bool {
    (s == 0 || !is_word_byte(hay[s - 1])) && (e == hay.len() || !is_word_byte(hay[e]))
}

fn matches_at(hay: &[u8], i: usize, needle: &[u8], ci: bool) -> bool {
    if ci {
        hay[i..i + needle.len()]
            .iter()
            .zip(needle)
            .all(|(a, b)| a.eq_ignore_ascii_case(b))
    } else {
        &hay[i..i + needle.len()] == needle
    }
}

/// Whether `needle` occurs in `hay` (`-i` ignoring case, `-w` as a whole
/// word). An empty needle matches every line.
pub fn has_match(hay: &[u8], needle: &[u8], ci: bool, word: bool) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if matches_at(hay, i, needle, ci) && (!word || word_bounded(hay, i, i + needle.len())) {
            return true;
        }
        i += 1;
    }
    false
}

/// Call `each(s, e)` for every non-overlapping occurrence `hay[s..e]` of
/// `needle`, left to right; `each` returns false to stop. An empty needle
/// has no occurrences to show.
pub fn each_match(hay: &[u8], needle: &[u8], ci: bool, word: bool, mut each: impl FnMut(usize, usize) -> bool) {
    if needle.is_empty() || needle.len() > hay.len() {
        return;
    }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if matches_at(hay, i, needle, ci) && (!word || word_bounded(hay, i, i + needle.len())) {
            if !each(i, i + needle.len()) {
                return;
            }
            i += needle.len();
        } else {
            i += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::counting;
    use alloc::vec;
    use alloc::vec::Vec;

    // Every non-overlapping match, found the way grep found them before it
    // stopped collecting: the whole-line answer each_match must give.
    fn spans(hay: &[u8], needle: &[u8], ci: bool, word: bool) -> Vec<(usize, usize)> {
        let mut v = Vec::new();
        if needle.is_empty() || needle.len() > hay.len() {
            return v;
        }
        let mut i = 0;
        while i + needle.len() <= hay.len() {
            let hit = (0..needle.len()).all(|k| {
                if ci {
                    hay[i + k].eq_ignore_ascii_case(&needle[k])
                } else {
                    hay[i + k] == needle[k]
                }
            });
            let before = i == 0 || !(hay[i - 1].is_ascii_alphanumeric() || hay[i - 1] == b'_');
            let e = i + needle.len();
            let after = e == hay.len() || !(hay[e].is_ascii_alphanumeric() || hay[e] == b'_');
            if hit && (!word || (before && after)) {
                v.push((i, e));
                i = e;
            } else {
                i += 1;
            }
        }
        v
    }

    fn found(hay: &[u8], needle: &[u8], ci: bool, word: bool) -> Vec<(usize, usize)> {
        let mut v = Vec::new();
        each_match(hay, needle, ci, word, |s, e| {
            v.push((s, e));
            true
        });
        v
    }

    const HAYS: &[&[u8]] = &[
        b"",
        b"a",
        b"aaaa",
        b"abcabc",
        b"ABC abc aBc",
        b"cat concat cat_ cat-cat",
        b"x_x x-x x x",
        b"the theme then the",
    ];
    const NEEDLES: &[&[u8]] = &[b"", b"a", b"aa", b"abc", b"cat", b"x", b"the", b"zzzz"];

    #[test]
    fn every_match_left_to_right() {
        for hay in HAYS {
            for needle in NEEDLES {
                for (ci, word) in [(false, false), (true, false), (false, true), (true, true)] {
                    assert_eq!(
                        found(hay, needle, ci, word),
                        spans(hay, needle, ci, word),
                        "{:?} in {:?} ci={} word={}",
                        needle,
                        hay,
                        ci,
                        word
                    );
                    if !needle.is_empty() {
                        assert_eq!(has_match(hay, needle, ci, word), !found(hay, needle, ci, word).is_empty());
                    }
                }
            }
        }
    }

    #[test]
    fn an_empty_needle_matches_every_line_and_shows_nothing() {
        assert!(has_match(b"", b"", false, false));
        assert!(has_match(b"abc", b"", false, true));
        assert!(found(b"abc", b"", false, false).is_empty());
    }

    #[test]
    fn a_stop_ends_the_search() {
        let mut got = 0;
        each_match(b"aaaaaa", b"a", false, false, |_, _| {
            got += 1;
            got < 2
        });
        assert_eq!(got, 2);
    }

    #[test]
    fn a_line_of_matches_costs_nothing_to_search() {
        let hay = vec![b'x'; 1 << 20];
        let mut n = 0usize;
        let bytes = counting::allocated(|| {
            each_match(&hay, b"x", false, false, |_, _| {
                n += 1;
                true
            })
        });
        assert_eq!(n, 1 << 20);
        assert_eq!(bytes, 0, "the search allocated");
    }
}
