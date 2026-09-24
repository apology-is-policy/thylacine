// stream -- the loops a filter reads its input through, a buffer at a time, so
// an input of any length is read in bounded memory: `lines` holds one line,
// `compare` two buffers, `Tail` what the tail needs.
//
// Slurping the whole input is not a bound a program can keep: the heap grows
// until the system is out of memory, and a page it cannot back ends the
// process at a fault, which v1.0 reports as exit status 1 (docs/ERRORS.md) --
// cmp's "differ" and grep's "no match". A filter that streams has no such
// input, and a line past [`LINE_MAX`] is refused before it gets that far.
//
// Pure: the caller passes its reads in as a closure, so the splitting and the
// comparison are host-tested against reads cut at every boundary.

use alloc::vec::Vec;
use core::cmp;
use core::fmt;

/// The read buffer's size.
pub const BUF: usize = 8 * 1024;

/// The longest line a filter holds. POSIX lets a text utility refuse a line
/// past its LINE_MAX; this one is far past any text, but it exists, because a
/// line with no end (`/dev/zero`) would otherwise grow until a page the system
/// cannot back ends the program -- exit status 1, grep's "no match".
pub const LINE_MAX: usize = 64 << 20;

/// Why a stream stopped before its end.
#[derive(Debug, PartialEq)]
pub enum Error<E> {
    /// The read failed.
    Read(E),
    /// A line outgrew what the heap would give.
    NoMemory,
    /// A line passed [`LINE_MAX`].
    TooLong,
}

impl<E: fmt::Display> fmt::Display for Error<E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Read(e) => e.fmt(f),
            Error::NoMemory => f.write_str("out of memory"),
            Error::TooLong => write!(f, "a line longer than {} MiB", LINE_MAX >> 20),
        }
    }
}

// Append `more` to the line held so far, unless that passes `max`; the buffer
// never grows past `max` either.
fn hold<E>(part: &mut Vec<u8>, more: &[u8], max: usize) -> Result<(), Error<E>> {
    if more.len() > max - part.len() {
        return Err(Error::TooLong);
    }
    let want = part.len() + more.len();
    if want > part.capacity() {
        let to = cmp::min(cmp::max(part.capacity() * 2, want), max);
        part.try_reserve_exact(to - part.len()).map_err(|_| Error::NoMemory)?;
    }
    part.extend_from_slice(more);
    Ok(())
}

// A buffer that held a long line is given back once it holds more than twice
// what the next line needs, so one long line does not keep its memory for the
// rest of the input.
fn release_long(v: &mut Vec<u8>, next: usize) {
    if v.capacity() > 2 * cmp::max(next, BUF) {
        *v = Vec::new();
    }
}

/// Call `each(line, n)` for every line `read` yields, without its newline, `n`
/// counting from 0. A last line with no newline is still a line; an empty input
/// has none. Only the current line is held, and one longer than [`LINE_MAX`]
/// is [`Error::TooLong`]. `each` returns false to stop reading there.
pub fn lines<E>(
    read: impl FnMut(&mut [u8]) -> Result<usize, E>,
    each: impl FnMut(&[u8], usize) -> bool,
) -> Result<(), Error<E>> {
    lines_within(LINE_MAX, &mut Vec::new(), read, each)
}

// `part` holds the start of a line a read cut short.
fn lines_within<E>(
    max: usize,
    part: &mut Vec<u8>,
    mut read: impl FnMut(&mut [u8]) -> Result<usize, E>,
    mut each: impl FnMut(&[u8], usize) -> bool,
) -> Result<(), Error<E>> {
    let mut buf = [0u8; BUF];
    let mut n = 0usize;
    loop {
        let got = read(&mut buf).map_err(Error::Read)?;
        if got == 0 {
            break;
        }
        let mut rest = &buf[..got];
        while let Some(i) = rest.iter().position(|&b| b == b'\n') {
            let line = if part.is_empty() {
                if i > max {
                    return Err(Error::TooLong);
                }
                &rest[..i]
            } else {
                hold(part, &rest[..i], max)?;
                &part[..]
            };
            if !each(line, n) {
                return Ok(());
            }
            part.clear();
            release_long(part, 0);
            n += 1;
            rest = &rest[i + 1..];
        }
        hold(part, rest, max)?;
    }
    if !part.is_empty() {
        each(&part[..], n);
    }
    Ok(())
}

/// Call `each(first, count)` for every run of adjacent lines `same` holds
/// equal: `first` is the run's first line, `count` how many lines it has. The
/// bound is [`lines`]'s; `each` returns false to stop reading there.
pub fn runs<E>(
    read: impl FnMut(&mut [u8]) -> Result<usize, E>,
    same: impl Fn(&[u8], &[u8]) -> bool,
    each: impl FnMut(&[u8], usize) -> bool,
) -> Result<(), Error<E>> {
    runs_within(LINE_MAX, &mut Vec::new(), read, same, |_| true, each)
}

/// Call `each(first)` with the first line of every run of adjacent lines
/// `same` holds equal, as that line arrives: a run is never waited out, as GNU
/// uniq prints when it counts nothing. The bound is [`lines`]'s; `each` returns
/// false to stop reading there.
pub fn firsts<E>(
    read: impl FnMut(&mut [u8]) -> Result<usize, E>,
    same: impl Fn(&[u8], &[u8]) -> bool,
    each: impl FnMut(&[u8]) -> bool,
) -> Result<(), Error<E>> {
    runs_within(LINE_MAX, &mut Vec::new(), read, same, each, |_, _| true)
}

// `first` holds the current run's first line: `begin` is handed it as the run
// begins, and `each` with the run's length once the run has ended.
fn runs_within<E>(
    max: usize,
    first: &mut Vec<u8>,
    read: impl FnMut(&mut [u8]) -> Result<usize, E>,
    same: impl Fn(&[u8], &[u8]) -> bool,
    mut begin: impl FnMut(&[u8]) -> bool,
    mut each: impl FnMut(&[u8], usize) -> bool,
) -> Result<(), Error<E>> {
    // The current run's length; 0 before the first line.
    let mut count = 0usize;
    let mut oom = false;
    lines_within(max, &mut Vec::new(), read, |line, _| {
        if count > 0 && same(&first[..], line) {
            count += 1;
            return true;
        }
        if count > 0 && !each(&first[..], count) {
            count = 0;
            return false;
        }
        release_long(first, line.len());
        first.clear();
        if first.try_reserve_exact(line.len()).is_err() {
            oom = true;
            return false;
        }
        first.extend_from_slice(line);
        count = 1;
        if !begin(line) {
            count = 0;
            return false;
        }
        true
    })?;
    if oom {
        return Err(Error::NoMemory);
    }
    if count > 0 {
        each(&first[..], count);
    }
    Ok(())
}

/// Where two streams first part.
#[derive(Debug, PartialEq)]
pub enum Diff {
    /// The same bytes to the end of both.
    Same,
    /// They differ at `byte`, on `line` (both counted from 1).
    At { byte: usize, line: usize },
    /// Stream 0 or 1 ended first, a prefix of the other.
    Eof(usize),
}

/// Compare two streams a buffer at a time, stopping at the first difference.
/// An error carries the stream it came from (0 or 1).
pub fn compare<E>(
    mut read0: impl FnMut(&mut [u8]) -> Result<usize, E>,
    mut read1: impl FnMut(&mut [u8]) -> Result<usize, E>,
) -> Result<Diff, (usize, E)> {
    let (mut a, mut b) = ([0u8; BUF], [0u8; BUF]);
    // Each stream reads at its own pace: `x[i..n]` is what is left of a read.
    let (mut ai, mut an, mut bi, mut bn) = (0, 0, 0, 0);
    let (mut byte, mut line) = (0usize, 1usize);
    loop {
        if ai == an {
            an = read0(&mut a).map_err(|e| (0, e))?;
            ai = 0;
        }
        if bi == bn {
            bn = read1(&mut b).map_err(|e| (1, e))?;
            bi = 0;
        }
        match (ai == an, bi == bn) {
            (true, true) => return Ok(Diff::Same),
            (true, false) => return Ok(Diff::Eof(0)),
            (false, true) => return Ok(Diff::Eof(1)),
            (false, false) => {}
        }
        let k = cmp::min(an - ai, bn - bi);
        let (x, y) = (&a[ai..ai + k], &b[bi..bi + k]);
        let same = x.iter().zip(y).position(|(p, q)| p != q).unwrap_or(k);
        line += x[..same].iter().filter(|&&c| c == b'\n').count();
        if same < k {
            return Ok(Diff::At { byte: byte + same + 1, line });
        }
        byte += k;
        ai += k;
        bi += k;
    }
}

/// The last `n` lines, or with `bytes` the last `n` bytes, of an input fed a
/// piece at a time: what the whole input would give -- a trailing newline ends
/// the last line rather than starting one, and the last line needs no newline
/// -- held in memory proportional to the answer. A line longer than
/// [`LINE_MAX`] is [`Error::TooLong`].
pub struct Tail {
    n: usize,
    bytes: bool,
    max: usize,
    // A suffix of the input starting at a line, with at least n newlines once
    // the input has them: the last n lines, whether or not the input ends in a
    // newline -- or at least n bytes. It is drained only once it holds twice
    // what it keeps, so draining costs a bounded amount per byte fed.
    keep: Vec<u8>,
    newlines: usize,
    // The length of the line `keep` ends in, which no newline has ended yet.
    open: usize,
}

impl Tail {
    pub fn new(n: usize, bytes: bool) -> Self {
        Self::within(LINE_MAX, n, bytes)
    }

    fn within(max: usize, n: usize, bytes: bool) -> Self {
        Tail { n, bytes, max, keep: Vec::new(), newlines: 0, open: 0 }
    }

    // The piece's newlines and the open line's length after it, or TooLong.
    fn scan<E>(&self, piece: &[u8]) -> Result<(usize, usize), Error<E>> {
        let (mut newlines, mut open) = (0, self.open);
        for &b in piece {
            if b == b'\n' {
                newlines += 1;
                open = 0;
            } else if open == self.max {
                return Err(Error::TooLong);
            } else {
                open += 1;
            }
        }
        Ok((newlines, open))
    }

    // Once what is kept is a small part of what the buffer grew to -- a long
    // line has passed -- the rest goes back.
    fn shrink(&mut self) {
        let need = cmp::max(self.keep.len(), BUF);
        if self.keep.capacity() > 4 * need {
            self.keep.shrink_to(2 * need);
        }
    }

    /// Take the next piece of the input. It fails as [`Error::NoMemory`] or
    /// [`Error::TooLong`], never as a read: `E` is the caller's read error.
    pub fn feed<E>(&mut self, piece: &[u8]) -> Result<(), Error<E>> {
        // Whatever follows, the last none of it is nothing.
        if self.n == 0 {
            return Ok(());
        }
        let (newlines, open) = if self.bytes { (0, 0) } else { self.scan(piece)? };
        self.keep.try_reserve(piece.len()).map_err(|_| Error::NoMemory)?;
        self.keep.extend_from_slice(piece);
        if self.bytes {
            let over = self.keep.len().saturating_sub(self.n);
            if over >= cmp::max(self.n, BUF) {
                self.keep.drain(..over);
                self.shrink();
            }
            return Ok(());
        }
        self.newlines += newlines;
        self.open = open;
        let hold = self.n;
        if self.newlines >= hold.saturating_mul(2) {
            let mut drop = self.newlines - hold;
            let mut cut = 0;
            while drop > 0 {
                if self.keep[cut] == b'\n' {
                    drop -= 1;
                }
                cut += 1;
            }
            self.keep.drain(..cut);
            self.newlines = hold;
            self.shrink();
        }
        Ok(())
    }

    /// The tail of everything fed.
    pub fn get(&self) -> &[u8] {
        let data = &self.keep[..];
        if self.n == 0 || data.is_empty() {
            return &[];
        }
        if self.bytes {
            return &data[data.len().saturating_sub(self.n)..];
        }
        let end = if data[data.len() - 1] == b'\n' { data.len() - 1 } else { data.len() };
        let mut count = 0;
        for j in (0..end).rev() {
            if data[j] == b'\n' {
                count += 1;
                if count == self.n {
                    return &data[j + 1..];
                }
            }
        }
        data
    }
}

/// What `tail -n +N` (or `-c +N`) prints: the input from its Nth line (or
/// byte) on, counted from 1, handed back a piece at a time as it arrives. +0
/// starts where +1 does, as GNU tail counts it. Nothing is held.
pub struct Skip {
    // Lines (or bytes) still to pass before the start.
    left: usize,
    bytes: bool,
}

impl Skip {
    pub fn new(n: usize, bytes: bool) -> Self {
        Skip { left: n.saturating_sub(1), bytes }
    }

    /// The part of `piece` at or past the start.
    pub fn feed<'a>(&mut self, piece: &'a [u8]) -> &'a [u8] {
        if self.bytes {
            let k = cmp::min(self.left, piece.len());
            self.left -= k;
            return &piece[k..];
        }
        let mut rest = piece;
        while self.left > 0 {
            match rest.iter().position(|&b| b == b'\n') {
                Some(i) => {
                    self.left -= 1;
                    rest = &rest[i + 1..];
                }
                None => return &[],
            }
        }
        rest
    }
}

/// cat's line transforms, applied a piece of input at a time with nothing held
/// between pieces: numbering (`-n`, or `-b` for nonblank lines only), one
/// blank line for a run of them (`-s`), and showing line ends (`-E`), tabs
/// (`-T`) and other nonprinting bytes (`-v`). The state runs on across
/// operands, so their concatenation is numbered as one input.
#[derive(Default)]
pub struct CatLines {
    pub number: bool,
    pub number_nonblank: bool,
    pub squeeze: bool,
    pub show_ends: bool,
    pub show_tabs: bool,
    pub show_nonprint: bool,
    lineno: u64,
    // The last line begun was blank.
    prev_blank: bool,
    // A line has begun and no newline has ended it yet.
    mid: bool,
}

impl CatLines {
    /// Whether any transform is on; with none, cat copies bytes.
    pub fn active(&self) -> bool {
        self.number || self.number_nonblank || self.squeeze || self.show_ends || self.show_tabs || self.show_nonprint
    }

    /// Append `piece`, transformed, to `out`: at most 25 bytes for each of its
    /// bytes, since a line's first byte brings the line's number and a tab (at
    /// most 21 bytes) and no byte shows as more than four (`M-^?`, or `$` and
    /// the newline).
    pub fn feed(&mut self, piece: &[u8], out: &mut Vec<u8>) {
        let mut rest = piece;
        while let Some(&first) = rest.first() {
            if !self.mid {
                let blank = first == b'\n';
                if self.squeeze && blank && self.prev_blank {
                    rest = &rest[1..];
                    continue;
                }
                self.prev_blank = blank;
                if if self.number_nonblank { !blank } else { self.number } {
                    self.lineno += 1;
                    push_number(out, self.lineno);
                }
                self.mid = true;
            }
            match rest.iter().position(|&b| b == b'\n') {
                Some(i) => {
                    self.show(out, &rest[..i]);
                    if self.show_ends {
                        out.push(b'$');
                    }
                    out.push(b'\n');
                    self.mid = false;
                    rest = &rest[i + 1..];
                }
                None => {
                    self.show(out, rest);
                    rest = &[];
                }
            }
        }
    }

    fn show(&self, out: &mut Vec<u8>, content: &[u8]) {
        if !self.show_tabs && !self.show_nonprint {
            out.extend_from_slice(content);
            return;
        }
        for &b in content {
            match b {
                b'\t' if self.show_tabs => out.extend_from_slice(b"^I"),
                // -v leaves a tab literal (only -T shows it), as it does
                // printable ASCII; -T alone leaves other controls literal.
                b'\t' | 0x20..=0x7e => out.push(b),
                _ if self.show_nonprint => push_nonprint(out, b),
                _ => out.push(b),
            }
        }
    }
}

/// `n` right-aligned in six columns, then a tab, as GNU cat numbers a line.
fn push_number(out: &mut Vec<u8>, mut n: u64) {
    let mut digits = [0u8; 20];
    let mut i = digits.len();
    loop {
        i -= 1;
        digits[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    for _ in digits.len() - i..6 {
        out.push(b' ');
    }
    out.extend_from_slice(&digits[i..]);
    out.push(b'\t');
}

/// A nonprinting byte in GNU `cat -v` notation: a high bit is `M-` over the low
/// seven, a control `^X` (X = c + 0x40), DEL `^?`.
fn push_nonprint(out: &mut Vec<u8>, b: u8) {
    let mut c = b;
    if c >= 0x80 {
        out.extend_from_slice(b"M-");
        c &= 0x7f;
    }
    if c < 0x20 {
        out.extend_from_slice(&[b'^', c + 0x40]);
    } else if c == 0x7f {
        out.extend_from_slice(b"^?");
    } else {
        out.push(c);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    // A read of at most `chunk` bytes at a time; `fail_at` makes the read that
    // would pass that offset fail instead.
    fn reader(data: &[u8], chunk: usize, fail_at: Option<usize>) -> impl FnMut(&mut [u8]) -> Result<usize, usize> + '_ {
        let mut off = 0;
        move |buf: &mut [u8]| {
            let k = chunk.min(buf.len()).min(data.len() - off);
            if let Some(f) = fail_at {
                if off + k > f {
                    return Err(off);
                }
            }
            buf[..k].copy_from_slice(&data[off..off + k]);
            off += k;
            Ok(k)
        }
    }

    fn split(data: &[u8]) -> Vec<Vec<u8>> {
        if data.is_empty() {
            return Vec::new();
        }
        let mut v: Vec<Vec<u8>> = data.split(|&b| b == b'\n').map(|l| l.to_vec()).collect();
        if data.last() == Some(&b'\n') {
            v.pop();
        }
        v
    }

    fn collect(data: &[u8], chunk: usize) -> Vec<Vec<u8>> {
        let mut got = Vec::new();
        lines(reader(data, chunk, None), |l, n| {
            assert_eq!(n, got.len(), "lines numbered out of order");
            got.push(l.to_vec());
            true
        })
        .unwrap();
        got
    }

    fn inputs() -> Vec<Vec<u8>> {
        let mut v: Vec<Vec<u8>> = [&b""[..], b"\n", b"\n\n", b"a", b"a\n", b"a\n\nb", b"a\nb\n", b"ab\ncd\nef"]
            .iter()
            .map(|s| s.to_vec())
            .collect();
        // Lines around the buffer's size, and newlines on its boundary.
        for len in [BUF - 1, BUF, BUF + 1, 3 * BUF + 17] {
            let mut long = vec![b'x'; len];
            long.push(b'\n');
            long.extend_from_slice(b"tail");
            v.push(long);
        }
        let mut mixed = Vec::new();
        for i in 0..4000 {
            mixed.extend(core::iter::repeat_n(b'a' + (i % 26) as u8, i % 37));
            mixed.push(b'\n');
        }
        v.push(mixed);
        v
    }

    #[test]
    fn every_line_whatever_the_reads() {
        for data in inputs() {
            let want = split(&data);
            for chunk in (1..=17).chain([BUF - 1, BUF]) {
                assert_eq!(collect(&data, chunk), want, "{} bytes read {} at a time", data.len(), chunk);
            }
        }
    }

    #[test]
    fn an_empty_input_has_no_lines() {
        assert!(collect(b"", 1).is_empty());
        assert_eq!(collect(b"\n", 1), vec![Vec::<u8>::new()]);
    }

    #[test]
    fn a_stop_reads_no_further() {
        let mut data = b"one\ntwo\n".to_vec();
        data.extend(vec![b'z'; 10 * BUF]);
        let mut read = 0usize;
        let mut seen = Vec::new();
        lines(
            |buf: &mut [u8]| {
                let k = (data.len() - read).min(buf.len()).min(5);
                buf[..k].copy_from_slice(&data[read..read + k]);
                read += k;
                Ok::<usize, ()>(k)
            },
            |l, _| {
                seen.push(l.to_vec());
                l != b"two"
            },
        )
        .unwrap();
        assert_eq!(seen, vec![b"one".to_vec(), b"two".to_vec()]);
        assert!(read <= 10, "read {} bytes past the stop", read);
    }

    #[test]
    fn a_failed_read_ends_the_lines_with_its_error() {
        let data = b"a\nb\nc\n";
        let mut seen = 0;
        let r = lines(reader(data, 1, Some(3)), |_, _| {
            seen += 1;
            true
        });
        assert_eq!(r, Err(Error::Read(3)));
        assert_eq!(seen, 1);
    }

    fn diff(x: &[u8], cx: usize, y: &[u8], cy: usize) -> Diff {
        compare(reader(x, cx, None), reader(y, cy, None)).unwrap()
    }

    // What cmp reported when it held both files whole.
    fn whole(x: &[u8], y: &[u8]) -> Diff {
        let mut line = 1;
        for i in 0..x.len().min(y.len()) {
            if x[i] != y[i] {
                return Diff::At { byte: i + 1, line };
            }
            if x[i] == b'\n' {
                line += 1;
            }
        }
        match x.len().cmp(&y.len()) {
            cmp::Ordering::Equal => Diff::Same,
            cmp::Ordering::Less => Diff::Eof(0),
            cmp::Ordering::Greater => Diff::Eof(1),
        }
    }

    #[test]
    fn compare_agrees_with_the_whole_file_answer_at_every_position() {
        let mut base = Vec::new();
        for i in 0..(2 * BUF + 300) {
            base.push(if i % 61 == 0 { b'\n' } else { b'a' + (i % 23) as u8 });
        }
        let chunks = [(1, 1), (3, 7), (7, 3), (BUF, 5), (5, BUF), (BUF, BUF), (BUF - 1, BUF)];
        for &(cx, cy) in &chunks {
            assert_eq!(diff(&base, cx, &base, cy), Diff::Same);
        }
        let positions = (0..40).chain((BUF - 70..BUF + 70).step_by(3)).chain([2 * BUF + 299]);
        for p in positions {
            let mut other = base.clone();
            other[p] ^= 0x20;
            for &(cx, cy) in &chunks {
                assert_eq!(diff(&base, cx, &other, cy), whole(&base, &other), "a change at {} read {}/{}", p, cx, cy);
            }
        }
    }

    #[test]
    fn a_prefix_ends_first() {
        let long = vec![b'q'; BUF + 10];
        for cut in [0, 1, BUF - 1, BUF, BUF + 9] {
            assert_eq!(diff(&long[..cut], 7, &long, BUF), Diff::Eof(0), "cut at {}", cut);
            assert_eq!(diff(&long, 3, &long[..cut], 1), Diff::Eof(1), "cut at {}", cut);
        }
        assert_eq!(diff(b"", 1, b"", 1), Diff::Same);
    }


    // tail's answer when it held the whole input.
    fn whole_tail(data: &[u8], n: usize, bytes: bool) -> &[u8] {
        if n == 0 || data.is_empty() {
            return &[];
        }
        if bytes {
            return &data[data.len().saturating_sub(n)..];
        }
        let end = if *data.last().unwrap() == b'\n' { data.len() - 1 } else { data.len() };
        let (mut count, mut start, mut j) = (0, 0, end);
        while j > 0 {
            j -= 1;
            if data[j] == b'\n' {
                count += 1;
                if count == n {
                    start = j + 1;
                    break;
                }
            }
        }
        &data[start..]
    }

    fn fed(data: &[u8], n: usize, bytes: bool, piece: usize) -> Tail {
        let mut t = Tail::new(n, bytes);
        for p in data.chunks(piece) {
            t.feed::<()>(p).unwrap();
        }
        t
    }

    #[test]
    fn the_tail_is_the_whole_inputs_tail_whatever_the_pieces() {
        let mut all = inputs();
        all.push(b"no newline at all".to_vec());
        all.push(vec![b'\n'; 50]);
        for data in all {
            for n in [0, 1, 2, 3, 5, 10, 1000, usize::MAX] {
                for bytes in [false, true] {
                    let want = whole_tail(&data, n, bytes);
                    for piece in [1, 2, 3, 7, 64, BUF] {
                        assert_eq!(
                            fed(&data, n, bytes, piece).get(),
                            want,
                            "{} bytes, n {} bytes-mode {}, pieces of {}",
                            data.len(),
                            n,
                            bytes,
                            piece
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn the_tail_holds_what_the_answer_needs_not_the_input() {
        let mut data = Vec::new();
        for i in 0..200_000 {
            data.push(b'a' + (i % 26) as u8);
            data.push(b'\n');
        }
        let t = fed(&data, 3, false, 1000);
        assert_eq!(t.get(), &data[data.len() - 6..]);
        assert!(t.keep.len() <= 1000 + 16, "held {} bytes for three short lines", t.keep.len());
        let t = fed(&data, 100, true, 1000);
        assert_eq!(t.get(), &data[data.len() - 100..]);
        assert!(t.keep.len() <= 100 + BUF + 1000, "held {} bytes for a 100-byte tail", t.keep.len());
    }

    #[test]
    fn a_tail_of_nothing_holds_nothing() {
        let data = vec![b'x'; 1 << 20];
        for bytes in [false, true] {
            let t = fed(&data, 0, bytes, 4096);
            assert_eq!(t.get(), b"");
            assert_eq!(t.keep.len(), 0, "held {} bytes for an empty tail (bytes-mode {})", t.keep.len(), bytes);
        }
    }

    // `lines_within` at a small bound, read `chunk` bytes at a time.
    fn bounded(data: &[u8], max: usize, chunk: usize) -> (Vec<Vec<u8>>, Result<(), Error<usize>>) {
        let mut got = Vec::new();
        let r = lines_within(max, &mut Vec::new(), reader(data, chunk, None), |l, _| {
            got.push(l.to_vec());
            true
        });
        (got, r)
    }

    #[test]
    fn a_line_past_the_bound_is_refused_after_the_lines_before_it() {
        let max = 100;
        let mut data = b"short\n".to_vec();
        data.extend(vec![b'x'; max]);
        data.extend(b"\nok\n");
        // A line of exactly the bound is a line, whatever the reads.
        for chunk in [1, 7, 100, 101, BUF] {
            assert_eq!(bounded(&data, max, chunk), (vec![b"short".to_vec(), vec![b'x'; max], b"ok".to_vec()], Ok(())), "read {} at a time", chunk);
        }
        // One byte more is refused, in one read or across several, ended or not.
        let mut long = b"short\n".to_vec();
        long.extend(vec![b'x'; max + 1]);
        for tail in [&b"\nok\n"[..], b""] {
            let mut data = long.clone();
            data.extend_from_slice(tail);
            for chunk in [1, 7, 100, 101, BUF] {
                assert_eq!(bounded(&data, max, chunk), (vec![b"short".to_vec()], Err(Error::TooLong)), "read {} at a time", chunk);
            }
        }
    }

    #[test]
    fn a_held_line_never_grows_its_buffer_past_the_bound() {
        let max = 1000;
        let mut part = Vec::new();
        while part.len() + 7 <= max {
            hold::<()>(&mut part, b"1234567", max).unwrap();
            assert!(part.capacity() <= max, "capacity {} past the bound", part.capacity());
        }
        assert_eq!(hold::<()>(&mut part, b"1234567", max), Err(Error::TooLong));
        assert!(part.len() <= max);
    }

    #[test]
    fn a_long_lines_room_is_given_back() {
        // A long line the reads cut, then short ones.
        let mut data = vec![b'q'; 10 * BUF];
        data.push(b'\n');
        for _ in 0..10 {
            data.extend_from_slice(b"s\n");
        }
        let mut part = Vec::new();
        let mut lens = Vec::new();
        lines_within(LINE_MAX, &mut part, reader(&data, BUF, None), |l, _| {
            lens.push(l.len());
            true
        })
        .unwrap();
        assert_eq!(lens.len(), 11);
        assert_eq!(lens[0], 10 * BUF);
        assert!(part.capacity() <= 2 * BUF, "kept {} bytes of room after a long line", part.capacity());
        // An ordinary line the reads cut keeps its room for the next.
        let mut part = Vec::new();
        lines_within(LINE_MAX, &mut part, reader(b"abcdefgh\nij\n", 3, None), |_, _| true).unwrap();
        assert!(part.capacity() > 0, "an ordinary line's room was given back");
    }

    #[test]
    fn the_tails_open_line_is_bounded_and_a_long_ones_room_returns() {
        let max = 100;
        for piece in [1, 3, 64, 1000] {
            let mut t = Tail::within(max, 2, false);
            let mut data = vec![b'y'; max];
            data.push(b'\n');
            for p in data.chunks(piece) {
                t.feed::<()>(p).unwrap();
            }
            let mut err = Ok(());
            for p in vec![b'z'; max + 1].chunks(piece) {
                err = t.feed::<()>(p);
                if err.is_err() {
                    break;
                }
            }
            assert_eq!(err, Err(Error::TooLong), "pieces of {}", piece);
        }
        // Bytes mode has no lines to bound.
        let mut t = Tail::within(max, 5, true);
        t.feed::<()>(&vec![b'w'; 10 * max]).unwrap();
        assert_eq!(t.get(), b"wwwww");
        // A long line, then short ones: the buffer shrinks back.
        let mut data = vec![b'q'; 50 * BUF];
        data.push(b'\n');
        for _ in 0..1000 {
            data.extend_from_slice(b"s\n");
        }
        let t = fed(&data, 2, false, BUF);
        assert_eq!(t.get(), b"s\ns\n");
        assert!(t.keep.capacity() <= 4 * BUF, "kept {} bytes of room after a long line", t.keep.capacity());
    }

    // uniq's answer when it held the whole input: each run's first line and
    // how many lines it has.
    fn whole_runs(data: &[u8], same: &dyn Fn(&[u8], &[u8]) -> bool) -> Vec<(Vec<u8>, usize)> {
        let mut v: Vec<(Vec<u8>, usize)> = Vec::new();
        for l in split(data) {
            match v.last_mut() {
                Some((first, count)) if same(first, &l) => *count += 1,
                _ => v.push((l, 1)),
            }
        }
        v
    }

    fn ran(data: &[u8], chunk: usize, same: &dyn Fn(&[u8], &[u8]) -> bool) -> Vec<(Vec<u8>, usize)> {
        let mut got = Vec::new();
        runs(reader(data, chunk, None), same, |first, count| {
            got.push((first.to_vec(), count));
            true
        })
        .unwrap();
        got
    }

    #[test]
    fn every_run_whatever_the_reads() {
        let mut all = inputs();
        all.push(b"a\na\nA\nb\nb\nb\na\n".to_vec());
        all.push(b"x\nx\nX".to_vec());
        let mut long = Vec::new();
        for _ in 0..3 {
            long.extend(vec![b'k'; BUF + 5]);
            long.push(b'\n');
        }
        long.extend_from_slice(b"k\n");
        all.push(long);
        let exact = |a: &[u8], b: &[u8]| a == b;
        let folded = |a: &[u8], b: &[u8]| a.eq_ignore_ascii_case(b);
        for data in all {
            for same in [&exact as &dyn Fn(&[u8], &[u8]) -> bool, &folded] {
                let want = whole_runs(&data, same);
                for chunk in [1, 2, 3, 7, BUF - 1, BUF] {
                    assert_eq!(ran(&data, chunk, same), want, "{} bytes read {} at a time", data.len(), chunk);
                }
            }
        }
    }

    #[test]
    fn a_stopped_run_is_the_last() {
        let mut seen = Vec::new();
        runs(reader(b"a\na\nb\nc\nc\n", 1, None), |a, b| a == b, |first, count| {
            seen.push((first.to_vec(), count));
            first != b"b"
        })
        .unwrap();
        assert_eq!(seen, vec![(b"a".to_vec(), 2), (b"b".to_vec(), 1)]);
    }

    #[test]
    fn a_long_runs_room_is_given_back() {
        let mut data = vec![b'q'; 10 * BUF];
        data.extend_from_slice(b"\ns\nt\n");
        let mut first = Vec::new();
        let mut seen = Vec::new();
        runs_within(LINE_MAX, &mut first, reader(&data, BUF, None), |a, b| a == b, |_| true, |f, count| {
            seen.push((f.len(), count));
            true
        })
        .unwrap();
        assert_eq!(seen, vec![(10 * BUF, 1), (1, 1), (1, 1)]);
        assert!(first.capacity() <= 2 * BUF, "kept {} bytes of room after a long line", first.capacity());
        // Runs of an ordinary length share one room.
        let mut first = Vec::new();
        runs_within(LINE_MAX, &mut first, reader(b"a line of some length\nb\n", BUF, None), |a, b| a == b, |_| true, |_, _| true).unwrap();
        assert!(first.capacity() >= 21, "an ordinary line's room was given back");
    }

    #[test]
    fn a_runs_first_line_never_grows_past_the_bound() {
        // A line longer than the room the last one left takes its own length,
        // not twice the old room, which near the bound would be past it.
        let max = 100;
        let mut data = vec![b'a'; 60];
        data.push(b'\n');
        data.extend(vec![b'b'; 70]);
        data.push(b'\n');
        let mut first = Vec::new();
        runs_within(max, &mut first, reader(&data, BUF, None), |a, b| a == b, |_| true, |_, _| true).unwrap();
        assert!(first.capacity() <= max, "capacity {} past the bound {}", first.capacity(), max);
    }

    fn firsts_of(data: &[u8], chunk: usize, same: &dyn Fn(&[u8], &[u8]) -> bool) -> Vec<Vec<u8>> {
        let mut got = Vec::new();
        firsts(reader(data, chunk, None), same, |first| {
            got.push(first.to_vec());
            true
        })
        .unwrap();
        got
    }

    #[test]
    fn every_runs_first_line_whatever_the_reads() {
        let mut all = inputs();
        all.push(b"a\na\nA\nb\nb\nb\na\n".to_vec());
        all.push(b"x\nx\nX".to_vec());
        let exact = |a: &[u8], b: &[u8]| a == b;
        let folded = |a: &[u8], b: &[u8]| a.eq_ignore_ascii_case(b);
        for data in all {
            for same in [&exact as &dyn Fn(&[u8], &[u8]) -> bool, &folded] {
                let want: Vec<Vec<u8>> = whole_runs(&data, same).into_iter().map(|(first, _)| first).collect();
                for chunk in [1, 2, 3, 7, BUF - 1, BUF] {
                    assert_eq!(firsts_of(&data, chunk, same), want, "{} bytes read {} at a time", data.len(), chunk);
                }
            }
        }
    }

    #[test]
    fn a_runs_first_line_is_shown_as_it_arrives() {
        // Read a line at a time: each run's first line is handed on by the read
        // that brought it, never held until a different line ends the run.
        let reads = core::cell::Cell::new(0);
        let mut read = reader(b"a\na\na\nb\nb\nc\n", 2, None);
        let mut seen = Vec::new();
        firsts(
            |buf: &mut [u8]| {
                reads.set(reads.get() + 1);
                read(buf)
            },
            |a, b| a == b,
            |first| {
                seen.push((first.to_vec(), reads.get()));
                true
            },
        )
        .unwrap();
        assert_eq!(seen, vec![(b"a".to_vec(), 1), (b"b".to_vec(), 4), (b"c".to_vec(), 6)]);
    }

    #[test]
    fn a_first_line_shown_before_a_failed_read_stays_shown() {
        let mut seen = Vec::new();
        let r = firsts(reader(b"a\na\nb\n", 2, Some(4)), |a, b| a == b, |first| {
            seen.push(first.to_vec());
            true
        });
        assert_eq!(r, Err(Error::Read(4)));
        assert_eq!(seen, vec![b"a".to_vec()]);
    }

    #[test]
    fn a_stopped_first_line_is_the_last() {
        let mut seen = Vec::new();
        firsts(reader(b"a\na\nb\nc\nc\n", 1, None), |a, b| a == b, |first| {
            seen.push(first.to_vec());
            first != b"b"
        })
        .unwrap();
        assert_eq!(seen, vec![b"a".to_vec(), b"b".to_vec()]);
    }

    // What tail -n +N (or -c +N) printed when it held the whole input.
    fn whole_from(data: &[u8], n: usize, bytes: bool) -> &[u8] {
        let skip = n.saturating_sub(1);
        if bytes {
            return &data[skip.min(data.len())..];
        }
        let mut rest = data;
        for _ in 0..skip {
            match rest.iter().position(|&b| b == b'\n') {
                Some(i) => rest = &rest[i + 1..],
                None => return &[],
            }
        }
        rest
    }

    #[test]
    fn from_the_start_is_the_whole_inputs_answer_whatever_the_pieces() {
        for data in inputs() {
            for n in 0..6 {
                for bytes in [false, true] {
                    let want = whole_from(&data, n, bytes);
                    for piece in [1, 2, 3, 7, BUF - 1, BUF] {
                        let mut s = Skip::new(n, bytes);
                        let mut got = Vec::new();
                        for p in data.chunks(piece) {
                            got.extend_from_slice(s.feed(p));
                        }
                        assert_eq!(got, want, "+{} bytes={} over {} bytes in pieces of {}", n, bytes, data.len(), piece);
                    }
                }
            }
        }
    }

    #[test]
    fn plus_zero_and_plus_one_are_the_whole_input() {
        for n in [0, 1] {
            for bytes in [false, true] {
                assert_eq!(Skip::new(n, bytes).feed(b"a\nb"), b"a\nb");
            }
        }
        assert_eq!(Skip::new(3, false).feed(b"a\nb\nc\nd"), b"c\nd");
        assert_eq!(Skip::new(3, true).feed(b"abcdef"), b"cdef");
    }

    #[test]
    fn a_failed_read_names_its_stream() {
        let data = vec![b'k'; 3 * BUF];
        assert_eq!(compare(reader(&data, BUF, None), reader(&data, BUF, Some(BUF + 1))), Err((1, BUF)));
        assert_eq!(compare(reader(&data, 100, Some(50)), reader(&data, 100, None)), Err((0, 0)));
    }

    // cat's transforms against the answer a line at a time over the whole input.
    mod cat_lines {
        use super::super::*;
        use alloc::format;
        use alloc::vec;

        // What cat printed when it read a line at a time, over the whole input: the
        // answer every way of cutting the input into pieces must give.
        fn whole(input: &[u8], t: &CatLines) -> Vec<u8> {
            let mut out = Vec::new();
            let (mut lineno, mut prev_blank) = (0u64, false);
            let mut lines: Vec<&[u8]> = input.split_inclusive(|&b| b == b'\n').collect();
            if input.is_empty() {
                lines.clear();
            }
            for line in lines {
                let blank = line == b"\n";
                if t.squeeze && blank && prev_blank {
                    continue;
                }
                prev_blank = blank;
                if if t.number_nonblank { !blank } else { t.number } {
                    lineno += 1;
                    out.extend_from_slice(format!("{:6}\t", lineno).as_bytes());
                }
                let has_nl = line.last() == Some(&b'\n');
                let content = if has_nl { &line[..line.len() - 1] } else { line };
                for &b in content {
                    if b == b'\t' && t.show_tabs {
                        out.extend_from_slice(b"^I");
                    } else if b == b'\t' || (0x20..=0x7e).contains(&b) || !t.show_nonprint {
                        out.push(b);
                    } else {
                        let mut c = b;
                        if c >= 0x80 {
                            out.extend_from_slice(b"M-");
                            c &= 0x7f;
                        }
                        match c {
                            0..=0x1f => out.extend_from_slice(&[b'^', c + 0x40]),
                            0x7f => out.extend_from_slice(b"^?"),
                            _ => out.push(c),
                        }
                    }
                }
                if has_nl {
                    if t.show_ends {
                        out.push(b'$');
                    }
                    out.push(b'\n');
                }
            }
            out
        }

        fn flags(bits: u32) -> CatLines {
            CatLines {
                number: bits & 1 != 0,
                number_nonblank: bits & 2 != 0,
                squeeze: bits & 4 != 0,
                show_ends: bits & 8 != 0,
                show_tabs: bits & 16 != 0,
                show_nonprint: bits & 32 != 0,
                ..CatLines::default()
            }
        }

        fn fed(input: &[u8], bits: u32, piece: usize) -> Vec<u8> {
            let mut t = flags(bits);
            let mut out = Vec::new();
            for p in input.chunks(piece) {
                t.feed(p, &mut out);
            }
            out
        }

        fn inputs() -> Vec<Vec<u8>> {
            let mut v: Vec<Vec<u8>> = [
                &b""[..],
                b"\n",
                b"\n\n\n",
                b"a",
                b"a\n",
                b"a\n\n\n\nb\n\n",
                b"\n\na\tb\n",
                b"tab\there\x01\x7f\x80\xff\x8a\n\nend",
            ]
            .iter()
            .map(|s| s.to_vec())
            .collect();
            let mut long = vec![b'x'; 3 * BUF + 17];
            long.extend_from_slice(b"\n\n\n");
            long.extend(vec![0u8; BUF + 1]);
            v.push(long);
            v
        }

        #[test]
        fn every_transform_whatever_the_pieces() {
            for input in inputs() {
                for bits in 0..64 {
                    let want = whole(&input, &flags(bits));
                    for piece in (1..=17).chain([BUF - 1, BUF]) {
                        assert_eq!(fed(&input, bits, piece), want, "flags {:#b}, {} bytes in pieces of {}", bits, input.len(), piece);
                    }
                }
            }
        }

        #[test]
        fn a_line_is_shown_as_it_arrives() {
            let mut t = flags(1 | 32);
            let mut out = Vec::new();
            t.feed(b"ab\x01", &mut out);
            assert_eq!(out, b"     1\tab^A", "nothing waits for the newline");
            out.clear();
            t.feed(b"c\nd", &mut out);
            assert_eq!(out, b"c\n     2\td");
        }

        #[test]
        fn operands_are_numbered_as_their_concatenation() {
            let mut t = flags(1);
            let mut out = Vec::new();
            t.feed(b"no newline", &mut out);
            t.feed(b" -- the same line\n\n", &mut out);
            assert_eq!(out, b"     1\tno newline -- the same line\n     2\t\n");
        }

        #[test]
        fn a_number_past_six_columns_widens() {
            let mut out = Vec::new();
            push_number(&mut out, 1_234_567);
            push_number(&mut out, u64::MAX);
            assert_eq!(out, b"1234567\t18446744073709551615\t");
        }
    }
}
