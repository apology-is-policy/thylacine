//! Content-Length message framing -- the transport envelope shared by LSP and
//! DAP. Every JSON body is wrapped as:
//!
//! ```text
//! Content-Length: <N>\r\n
//! [other headers, ignored]\r\n
//! \r\n
//! <N bytes of UTF-8 JSON>
//! ```
//!
//! The framing is protocol-agnostic (LSP is JSON-RPC 2.0, DAP is its own
//! envelope, but both frame the same way). The decoder is **streaming**: bytes
//! arrive from a pipe in arbitrary chunks -- a header or body may be split
//! across reads, and several frames may arrive in one read -- so callers `push`
//! whatever they got and `next_frame` out every complete frame.

use alloc::format;
use alloc::vec::Vec;
use core::fmt;

const CR: u8 = 13;
const LF: u8 = 10;

/// Cap on the header block (bytes before the blank line). A peer that never
/// sends the terminating blank line cannot grow the buffer without bound.
pub const MAX_HEADER_BYTES: usize = 8 * 1024;

/// Cap on a single message body. A hostile `Content-Length` cannot make us
/// buffer forever. 64 MiB is far above any real LSP/DAP message.
pub const MAX_BODY_BYTES: usize = 64 * 1024 * 1024;

/// Encode a JSON body into a framed message: `Content-Length: N\r\n\r\n` + body.
pub fn encode(body: &[u8]) -> Vec<u8> {
    let hdr = format!("Content-Length: {}", body.len());
    let mut out = Vec::with_capacity(hdr.len() + 4 + body.len());
    out.extend_from_slice(hdr.as_bytes());
    out.extend_from_slice(&[CR, LF, CR, LF]);
    out.extend_from_slice(body);
    out
}

/// A malformed frame -- the caller should tear the connection down (a framing
/// error means the byte stream is no longer trustworthy).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameError {
    pub msg: &'static str,
}

impl fmt::Display for FrameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "frame error: {}", self.msg)
    }
}

/// A streaming Content-Length frame decoder.
pub struct Decoder {
    buf: Vec<u8>,
    /// `Some(n)` once a header block has been parsed and we await `n` body bytes.
    body_len: Option<usize>,
}

impl Default for Decoder {
    fn default() -> Decoder {
        Decoder::new()
    }
}

impl Decoder {
    pub fn new() -> Decoder {
        Decoder { buf: Vec::new(), body_len: None }
    }

    /// Feed bytes as they arrive from the transport.
    pub fn push(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    /// Pull the next complete frame body, if one is fully buffered. `Ok(None)`
    /// means "need more bytes"; `Err` means the stream is malformed.
    ///
    /// Call repeatedly until it returns `Ok(None)` to drain every frame a single
    /// `push` delivered.
    pub fn next_frame(&mut self) -> Result<Option<Vec<u8>>, FrameError> {
        loop {
            match self.body_len {
                None => {
                    match find_sep(&self.buf) {
                        None => {
                            // no header terminator yet -- keep buffering, but
                            // bound the wait.
                            if self.buf.len() > MAX_HEADER_BYTES {
                                return Err(FrameError { msg: "header block too large" });
                            }
                            return Ok(None);
                        }
                        Some(sep) => {
                            let n = parse_content_length(&self.buf[..sep])?;
                            if n > MAX_BODY_BYTES {
                                return Err(FrameError { msg: "content-length exceeds cap" });
                            }
                            // drop the header block + the \r\n\r\n separator
                            self.buf.drain(0..sep + 4);
                            self.body_len = Some(n);
                            // fall through to try to satisfy the body
                        }
                    }
                }
                Some(n) => {
                    if self.buf.len() >= n {
                        let body: Vec<u8> = self.buf.drain(0..n).collect();
                        self.body_len = None;
                        return Ok(Some(body));
                    }
                    return Ok(None);
                }
            }
        }
    }
}

/// Index of the first `\r\n\r\n` in `buf` (the start of the 4-byte separator),
/// or `None`.
fn find_sep(buf: &[u8]) -> Option<usize> {
    if buf.len() < 4 {
        return None;
    }
    let mut i = 0;
    while i + 4 <= buf.len() {
        if buf[i] == CR && buf[i + 1] == LF && buf[i + 2] == CR && buf[i + 3] == LF {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Parse the `Content-Length` value out of a header block (the bytes before the
/// blank line, with single-`\r\n`-separated lines). The header name matches
/// case-insensitively (per HTTP); other headers (e.g. `Content-Type`) are
/// ignored.
fn parse_content_length(headers: &[u8]) -> Result<usize, FrameError> {
    for line in split_crlf(headers) {
        let colon = match line.iter().position(|&b| b == b':') {
            Some(c) => c,
            None => continue,
        };
        let (name, rest) = line.split_at(colon);
        if eq_ignore_ascii_case(trim(name), b"content-length") {
            let val = trim(&rest[1..]); // skip the ':'
            if val.is_empty() || !val.iter().all(|b| b.is_ascii_digit()) {
                return Err(FrameError { msg: "invalid content-length value" });
            }
            let mut n: usize = 0;
            for &b in val {
                n = n
                    .checked_mul(10)
                    .and_then(|x| x.checked_add((b - b'0') as usize))
                    .ok_or(FrameError { msg: "content-length overflow" })?;
            }
            return Ok(n);
        }
    }
    Err(FrameError { msg: "missing content-length header" })
}

/// Split a byte slice on `\r\n` into lines (no trailing empty line, since the
/// caller passes the block up to but excluding the terminating blank line).
fn split_crlf(mut b: &[u8]) -> impl Iterator<Item = &[u8]> {
    core::iter::from_fn(move || {
        if b.is_empty() {
            return None;
        }
        let mut i = 0;
        while i + 1 < b.len() {
            if b[i] == CR && b[i + 1] == LF {
                let line = &b[..i];
                b = &b[i + 2..];
                return Some(line);
            }
            i += 1;
        }
        let line = b;
        b = &[];
        Some(line)
    })
}

fn trim(mut b: &[u8]) -> &[u8] {
    while let [first, rest @ ..] = b {
        if first.is_ascii_whitespace() {
            b = rest;
        } else {
            break;
        }
    }
    while let [rest @ .., last] = b {
        if last.is_ascii_whitespace() {
            b = rest;
        } else {
            break;
        }
    }
    b
}

fn eq_ignore_ascii_case(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
}

#[cfg(test)]
mod tests {
    use super::*;

    // CR/LF built from byte values, never `\r\n` escapes -- keeps the test
    // literals free of any escape the editor tooling might mangle.
    const CRLF: [u8; 2] = [13, 10];

    fn framed(headers: &[&[u8]], body: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        for h in headers {
            v.extend_from_slice(h);
            v.extend_from_slice(&CRLF);
        }
        v.extend_from_slice(&CRLF); // blank line
        v.extend_from_slice(body);
        v
    }

    #[test]
    fn encode_shape() {
        let got = encode(b"hello");
        assert_eq!(got, framed(&[b"Content-Length: 5"], b"hello"));
    }

    #[test]
    fn encode_empty_body() {
        assert_eq!(encode(b""), framed(&[b"Content-Length: 0"], b""));
    }

    #[test]
    fn decode_one_frame() {
        let mut d = Decoder::new();
        d.push(&encode(b"hello"));
        assert_eq!(d.next_frame().unwrap(), Some(b"hello".to_vec()));
        assert_eq!(d.next_frame().unwrap(), None);
    }

    #[test]
    fn decode_split_across_pushes() {
        let msg = encode(b"the body");
        let mut d = Decoder::new();
        // header arrives in one read, body in two
        let split1 = 10;
        let split2 = msg.len() - 3;
        d.push(&msg[..split1]);
        assert_eq!(d.next_frame().unwrap(), None);
        d.push(&msg[split1..split2]);
        assert_eq!(d.next_frame().unwrap(), None);
        d.push(&msg[split2..]);
        assert_eq!(d.next_frame().unwrap(), Some(b"the body".to_vec()));
    }

    #[test]
    fn decode_multiple_frames_in_one_push() {
        let mut stream = encode(b"one");
        stream.extend_from_slice(&encode(b"two"));
        stream.extend_from_slice(&encode(b"three"));
        let mut d = Decoder::new();
        d.push(&stream);
        assert_eq!(d.next_frame().unwrap(), Some(b"one".to_vec()));
        assert_eq!(d.next_frame().unwrap(), Some(b"two".to_vec()));
        assert_eq!(d.next_frame().unwrap(), Some(b"three".to_vec()));
        assert_eq!(d.next_frame().unwrap(), None);
    }

    #[test]
    fn ignores_extra_headers_and_case() {
        // gopls sends Content-Type; the name case may vary.
        let msg = framed(
            &[b"Content-Type: application/vscode-jsonrpc; charset=utf-8", b"CONTENT-LENGTH: 4"],
            b"body",
        );
        let mut d = Decoder::new();
        d.push(&msg);
        assert_eq!(d.next_frame().unwrap(), Some(b"body".to_vec()));
    }

    #[test]
    fn body_with_embedded_crlf() {
        // a JSON body legitimately containing CRLF bytes must not confuse the
        // framer (length-delimited, not delimiter-scanned).
        let body = framed(&[b"x"], b"y"); // arbitrary bytes containing CRLF
        let mut d = Decoder::new();
        d.push(&encode(&body));
        assert_eq!(d.next_frame().unwrap(), Some(body));
    }

    #[test]
    fn missing_content_length_errors() {
        let msg = framed(&[b"Content-Type: text/plain"], b"body");
        let mut d = Decoder::new();
        d.push(&msg);
        assert!(d.next_frame().is_err());
    }

    #[test]
    fn non_numeric_content_length_errors() {
        let msg = framed(&[b"Content-Length: abc"], b"body");
        let mut d = Decoder::new();
        d.push(&msg);
        assert!(d.next_frame().is_err());
    }

    #[test]
    fn oversized_header_errors() {
        let mut d = Decoder::new();
        // no separator ever; feed past the header cap
        d.push(&[b'a'; MAX_HEADER_BYTES + 1]);
        assert!(d.next_frame().is_err());
    }

    #[test]
    fn roundtrip_arbitrary() {
        let bodies: [&[u8]; 4] = [b"", b"x", b"{\"jsonrpc\":\"2.0\"}", &[0u8, 255u8, 13u8, 10u8]];
        let mut stream = Vec::new();
        for b in &bodies {
            stream.extend_from_slice(&encode(b));
        }
        let mut d = Decoder::new();
        d.push(&stream);
        for b in &bodies {
            assert_eq!(d.next_frame().unwrap(), Some(b.to_vec()));
        }
        assert_eq!(d.next_frame().unwrap(), None);
    }
}
