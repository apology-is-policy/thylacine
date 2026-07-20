//! A minimal `no_std` JSON value model, parser, and serializer.
//!
//! The tree's first JSON codec (there was none: `serde_json` is std-only, and
//! the "hand-rolled JSON" in `httpd`/`curl` was HTTP `Content-Length`, not
//! JSON). Scoped to what LSP + DAP (JSON-RPC 2.0) need:
//!
//!   - correct RFC 8259 parse (string escapes incl. `\uXXXX` surrogate pairs,
//!     numbers, nesting) with **bounded recursion** (a malformed/hostile stream
//!     cannot blow the stack);
//!   - **integer/float id fidelity** -- a JSON-RPC `id` / DAP `seq` is an
//!     integer and must round-trip exactly, so integral numbers parse to
//!     `Int(i64)`, never a lossy `f64`;
//!   - compact serialization (no pretty-print) with **insertion-ordered**
//!     objects, so a built request serializes deterministically.
//!
//! Objects are an ordered `Vec<(String, Value)>`, not a map: LSP/DAP objects are
//! small, order matters for deterministic output + tests, and no_std has no hash
//! map without pulling a dependency. Lookup is a linear scan (`get`).

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::{self, Write as _};

/// Maximum nesting depth the parser accepts. Deep enough for any real LSP/DAP
/// message; a guard so a pathological `[[[[...` cannot recurse without bound.
pub const MAX_DEPTH: usize = 128;

/// A parsed JSON value.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    /// An integer with no fraction/exponent that fit in `i64` (RPC ids round-trip
    /// exactly).
    Int(i64),
    /// A non-integral number, or an integer outside `i64` range.
    Float(f64),
    Str(String),
    Array(Vec<Value>),
    /// Insertion-ordered key/value pairs.
    Object(Vec<(String, Value)>),
}

impl Value {
    /// Parse a single JSON value from a byte slice (LSP/DAP messages are one
    /// value). Trailing whitespace is allowed; trailing non-whitespace is an
    /// error.
    pub fn parse(input: &[u8]) -> Result<Value, Error> {
        let mut p = Parser { b: input, i: 0, depth: 0 };
        p.skip_ws();
        let v = p.parse_value()?;
        p.skip_ws();
        if p.i != p.b.len() {
            return Err(Error { pos: p.i, msg: "trailing bytes after value" });
        }
        Ok(v)
    }

    // --- accessors (borrow, never allocate) ---

    /// For an object, the value at `key` (first match; linear scan). `None` for
    /// a non-object or an absent key.
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Object(kvs) => kvs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// The value as an `i64`. `Int` directly; a `Float` only when it is exactly
    /// integral and in range (defensive -- a server that encodes an id as `1.0`).
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            Value::Float(f) => {
                // no_std has no f64::fract(); a lossless round-trip through i64
                // means the float is integral and in range.
                let i = *f as i64; // saturating cast (Rust >= 1.45)
                if i as f64 == *f {
                    Some(i)
                } else {
                    None
                }
            }
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Int(i) => Some(*i as f64),
            Value::Float(f) => Some(*f),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(a) => Some(a.as_slice()),
            _ => None,
        }
    }

    pub fn as_object(&self) -> Option<&[(String, Value)]> {
        match self {
            Value::Object(o) => Some(o.as_slice()),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }
}

// --- ergonomic construction (building requests) ---

impl From<bool> for Value {
    fn from(b: bool) -> Value {
        Value::Bool(b)
    }
}
impl From<i64> for Value {
    fn from(i: i64) -> Value {
        Value::Int(i)
    }
}
impl From<i32> for Value {
    fn from(i: i32) -> Value {
        Value::Int(i as i64)
    }
}
impl From<f64> for Value {
    fn from(f: f64) -> Value {
        Value::Float(f)
    }
}
impl From<&str> for Value {
    fn from(s: &str) -> Value {
        Value::Str(String::from(s))
    }
}
impl From<String> for Value {
    fn from(s: String) -> Value {
        Value::Str(s)
    }
}
impl From<Vec<Value>> for Value {
    fn from(a: Vec<Value>) -> Value {
        Value::Array(a)
    }
}
impl From<Vec<(String, Value)>> for Value {
    fn from(o: Vec<(String, Value)>) -> Value {
        Value::Object(o)
    }
}

/// A parse error: the byte offset and a static description.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error {
    pub pos: usize,
    pub msg: &'static str,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "json parse error at byte {}: {}", self.pos, self.msg)
    }
}

// --- the parser ---

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
    depth: usize,
}

impl<'a> Parser<'a> {
    fn err(&self, msg: &'static str) -> Error {
        Error { pos: self.i, msg }
    }

    fn peek(&self) -> Option<u8> {
        self.b.get(self.i).copied()
    }

    fn skip_ws(&mut self) {
        while let Some(c) = self.peek() {
            // RFC 8259 whitespace: space, tab, LF, CR.
            if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
                self.i += 1;
            } else {
                break;
            }
        }
    }

    fn parse_value(&mut self) -> Result<Value, Error> {
        match self.peek() {
            None => Err(self.err("unexpected end of input")),
            Some(b'{') => self.parse_object(),
            Some(b'[') => self.parse_array(),
            Some(b'"') => Ok(Value::Str(self.parse_string()?)),
            Some(b't') | Some(b'f') => self.parse_bool(),
            Some(b'n') => self.parse_null(),
            Some(c) if c == b'-' || c.is_ascii_digit() => self.parse_number(),
            Some(_) => Err(self.err("unexpected character")),
        }
    }

    fn expect_lit(&mut self, lit: &[u8], val: Value) -> Result<Value, Error> {
        if self.b[self.i..].starts_with(lit) {
            self.i += lit.len();
            Ok(val)
        } else {
            Err(self.err("invalid literal"))
        }
    }

    fn parse_null(&mut self) -> Result<Value, Error> {
        self.expect_lit(b"null", Value::Null)
    }

    fn parse_bool(&mut self) -> Result<Value, Error> {
        if self.peek() == Some(b't') {
            self.expect_lit(b"true", Value::Bool(true))
        } else {
            self.expect_lit(b"false", Value::Bool(false))
        }
    }

    fn parse_number(&mut self) -> Result<Value, Error> {
        let start = self.i;
        let mut is_float = false;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        // integer part
        match self.peek() {
            Some(b'0') => {
                self.i += 1;
            }
            Some(c) if c.is_ascii_digit() => {
                while matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                    self.i += 1;
                }
            }
            _ => return Err(self.err("invalid number")),
        }
        // fraction
        if self.peek() == Some(b'.') {
            is_float = true;
            self.i += 1;
            if !matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                return Err(self.err("digit expected after decimal point"));
            }
            while matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                self.i += 1;
            }
        }
        // exponent
        if matches!(self.peek(), Some(b'e') | Some(b'E')) {
            is_float = true;
            self.i += 1;
            if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                return Err(self.err("digit expected in exponent"));
            }
            while matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                self.i += 1;
            }
        }
        // `self.b[start..self.i]` is ASCII by construction.
        let s = core::str::from_utf8(&self.b[start..self.i]).map_err(|_| self.err("bad number"))?;
        if is_float {
            s.parse::<f64>().map(Value::Float).map_err(|_| self.err("number out of range"))
        } else {
            // integral: prefer i64; fall back to f64 on overflow.
            match s.parse::<i64>() {
                Ok(i) => Ok(Value::Int(i)),
                Err(_) => s.parse::<f64>().map(Value::Float).map_err(|_| self.err("number out of range")),
            }
        }
    }

    fn parse_string(&mut self) -> Result<String, Error> {
        // caller confirmed the opening quote
        self.i += 1;
        let mut out = String::new();
        loop {
            let c = self.peek().ok_or_else(|| self.err("unterminated string"))?;
            match c {
                b'"' => {
                    self.i += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.i += 1;
                    let e = self.peek().ok_or_else(|| self.err("unterminated escape"))?;
                    self.i += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{0008}'),
                        b'f' => out.push('\u{000C}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let cp = self.parse_hex4()?;
                            if (0xD800..=0xDBFF).contains(&cp) {
                                // high surrogate -- must be followed by \uDC00..DFFF
                                if self.peek() != Some(b'\\') {
                                    return Err(self.err("lone high surrogate"));
                                }
                                self.i += 1;
                                if self.peek() != Some(b'u') {
                                    return Err(self.err("lone high surrogate"));
                                }
                                self.i += 1;
                                let lo = self.parse_hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&lo) {
                                    return Err(self.err("invalid low surrogate"));
                                }
                                let combined =
                                    0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                match char::from_u32(combined) {
                                    Some(ch) => out.push(ch),
                                    None => return Err(self.err("invalid surrogate pair")),
                                }
                            } else if (0xDC00..=0xDFFF).contains(&cp) {
                                return Err(self.err("lone low surrogate"));
                            } else {
                                match char::from_u32(cp) {
                                    Some(ch) => out.push(ch),
                                    None => return Err(self.err("invalid unicode escape")),
                                }
                            }
                        }
                        _ => return Err(self.err("invalid escape")),
                    }
                }
                // control chars must be escaped in JSON
                0x00..=0x1F => return Err(self.err("unescaped control character")),
                // any other byte is part of a UTF-8 sequence; copy the whole char
                _ => {
                    let ch_len = utf8_len(c);
                    let end = self.i + ch_len;
                    if end > self.b.len() {
                        return Err(self.err("truncated utf-8"));
                    }
                    let s = core::str::from_utf8(&self.b[self.i..end])
                        .map_err(|_| self.err("invalid utf-8"))?;
                    out.push_str(s);
                    self.i = end;
                }
            }
        }
    }

    fn parse_hex4(&mut self) -> Result<u32, Error> {
        if self.i + 4 > self.b.len() {
            return Err(self.err("truncated \\u escape"));
        }
        let mut v = 0u32;
        for _ in 0..4 {
            let d = self.b[self.i];
            let n = match d {
                b'0'..=b'9' => (d - b'0') as u32,
                b'a'..=b'f' => (d - b'a' + 10) as u32,
                b'A'..=b'F' => (d - b'A' + 10) as u32,
                _ => return Err(self.err("invalid hex digit in \\u escape")),
            };
            v = (v << 4) | n;
            self.i += 1;
        }
        Ok(v)
    }

    fn enter(&mut self) -> Result<(), Error> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(self.err("maximum nesting depth exceeded"));
        }
        Ok(())
    }

    fn parse_array(&mut self) -> Result<Value, Error> {
        self.enter()?;
        self.i += 1; // '['
        let mut arr = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.i += 1;
            self.depth -= 1;
            return Ok(Value::Array(arr));
        }
        loop {
            self.skip_ws();
            arr.push(self.parse_value()?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => {
                    self.i += 1;
                }
                Some(b']') => {
                    self.i += 1;
                    self.depth -= 1;
                    return Ok(Value::Array(arr));
                }
                _ => return Err(self.err("expected ',' or ']' in array")),
            }
        }
    }

    fn parse_object(&mut self) -> Result<Value, Error> {
        self.enter()?;
        self.i += 1; // '{'
        let mut obj: Vec<(String, Value)> = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.i += 1;
            self.depth -= 1;
            return Ok(Value::Object(obj));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some(b'"') {
                return Err(self.err("expected string key in object"));
            }
            let key = self.parse_string()?;
            self.skip_ws();
            if self.peek() != Some(b':') {
                return Err(self.err("expected ':' after key"));
            }
            self.i += 1;
            self.skip_ws();
            let val = self.parse_value()?;
            obj.push((key, val));
            self.skip_ws();
            match self.peek() {
                Some(b',') => {
                    self.i += 1;
                }
                Some(b'}') => {
                    self.i += 1;
                    self.depth -= 1;
                    return Ok(Value::Object(obj));
                }
                _ => return Err(self.err("expected ',' or '}' in object")),
            }
        }
    }
}

/// Length in bytes of the UTF-8 sequence whose lead byte is `b` (1 for a
/// continuation/invalid byte, which the caller then validates via `from_utf8`).
fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >> 5 == 0b110 {
        2
    } else if b >> 4 == 0b1110 {
        3
    } else if b >> 3 == 0b11110 {
        4
    } else {
        1
    }
}

// --- serialization (compact) ---

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Null => f.write_str("null"),
            Value::Bool(true) => f.write_str("true"),
            Value::Bool(false) => f.write_str("false"),
            Value::Int(i) => write!(f, "{}", i),
            Value::Float(x) => {
                // JSON has no inf/nan; emit `null` defensively (we never produce
                // them, but a Display impl must not write invalid JSON).
                if x.is_finite() {
                    write!(f, "{}", x)
                } else {
                    f.write_str("null")
                }
            }
            Value::Str(s) => write_json_string(f, s),
            Value::Array(a) => {
                f.write_str("[")?;
                for (n, v) in a.iter().enumerate() {
                    if n > 0 {
                        f.write_str(",")?;
                    }
                    write!(f, "{}", v)?;
                }
                f.write_str("]")
            }
            Value::Object(o) => {
                f.write_str("{")?;
                for (n, (k, v)) in o.iter().enumerate() {
                    if n > 0 {
                        f.write_str(",")?;
                    }
                    write_json_string(f, k)?;
                    f.write_str(":")?;
                    write!(f, "{}", v)?;
                }
                f.write_str("}")
            }
        }
    }
}

fn write_json_string(f: &mut fmt::Formatter<'_>, s: &str) -> fmt::Result {
    f.write_str("\"")?;
    for ch in s.chars() {
        match ch {
            '"' => f.write_str("\\\"")?,
            '\\' => f.write_str("\\\\")?,
            '\n' => f.write_str("\\n")?,
            '\r' => f.write_str("\\r")?,
            '\t' => f.write_str("\\t")?,
            '\u{0008}' => f.write_str("\\b")?,
            '\u{000C}' => f.write_str("\\f")?,
            c if (c as u32) < 0x20 => write!(f, "\\u{:04x}", c as u32)?,
            // Non-ASCII is emitted raw (valid UTF-8 JSON); gopls/Ambush accept it.
            c => f.write_char(c)?,
        }
    }
    f.write_str("\"")
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    fn p(s: &str) -> Value {
        Value::parse(s.as_bytes()).expect("parse ok")
    }

    #[test]
    fn scalars() {
        assert_eq!(p("null"), Value::Null);
        assert_eq!(p("true"), Value::Bool(true));
        assert_eq!(p("false"), Value::Bool(false));
        assert_eq!(p("0"), Value::Int(0));
        assert_eq!(p("-42"), Value::Int(-42));
        assert_eq!(p("  7  "), Value::Int(7)); // surrounding ws
        assert_eq!(p("3.5"), Value::Float(3.5));
        assert_eq!(p("1e3"), Value::Float(1000.0));
        assert_eq!(p("-2.5E-1"), Value::Float(-0.25));
        assert_eq!(p("\"hi\""), Value::Str("hi".to_string()));
        assert_eq!(p("\"\""), Value::Str(String::new()));
    }

    #[test]
    fn id_fidelity() {
        // A JSON-RPC id / DAP seq must round-trip exactly, not via f64.
        let v = p("9007199254740993"); // 2^53 + 1, unrepresentable in f64
        assert_eq!(v, Value::Int(9007199254740993));
        assert_eq!(v.as_i64(), Some(9007199254740993));
        assert_eq!(v.to_string(), "9007199254740993");
    }

    #[test]
    fn string_escapes() {
        assert_eq!(p(r#""a\"b""#), Value::Str("a\"b".to_string()));
        assert_eq!(p(r#""line\nfeed""#), Value::Str("line\nfeed".to_string()));
        assert_eq!(p(r#""tab\ttab""#), Value::Str("tab\ttab".to_string()));
        assert_eq!(p(r#""slash\/""#), Value::Str("slash/".to_string()));
        assert_eq!(p(r#""A""#), Value::Str("A".to_string()));
        // raw astral UTF-8 in the source, U+1D11E (musical G clef)
        assert_eq!(p(r#""𝄞""#), Value::Str("\u{1D11E}".to_string()));
        // the \uXXXX surrogate-PAIR escape form of the same codepoint
        assert_eq!(
            Value::parse(b"\"\\ud834\\udd1e\"").unwrap(),
            Value::Str("\u{1D11E}".to_string())
        );
        // raw UTF-8 in the source (gopls sends this, not escapes)
        assert_eq!(p("\"café\""), Value::Str("café".to_string()));
    }

    #[test]
    fn nested() {
        let v = p(r#"{"a":[1,2,{"b":true}],"c":null}"#);
        assert_eq!(v.get("a").unwrap().as_array().unwrap().len(), 3);
        assert_eq!(
            v.get("a").unwrap().as_array().unwrap()[2].get("b").unwrap().as_bool(),
            Some(true)
        );
        assert!(v.get("c").unwrap().is_null());
        assert_eq!(v.get("missing"), None);
    }

    #[test]
    fn object_order_preserved() {
        // insertion order is retained -> deterministic serialization
        let v = p(r#"{"z":1,"a":2,"m":3}"#);
        assert_eq!(v.to_string(), r#"{"z":1,"a":2,"m":3}"#);
    }

    #[test]
    fn empty_containers() {
        assert_eq!(p("[]"), Value::Array(vec![]));
        assert_eq!(p("{}"), Value::Object(vec![]));
        assert_eq!(p("[ ]").to_string(), "[]");
        assert_eq!(p("{ }").to_string(), "{}");
    }

    #[test]
    fn serialize_roundtrip() {
        // an LSP-shaped message
        let src = r#"{"jsonrpc":"2.0","id":1,"method":"textDocument/definition","params":{"position":{"line":41,"character":8}}}"#;
        let v = p(src);
        assert_eq!(v.to_string(), src); // compact + order-preserving -> byte-identical
    }

    #[test]
    fn serialize_escapes() {
        let v = Value::Str("he said \"hi\"\n\tand left\\".to_string());
        assert_eq!(v.to_string(), r#""he said \"hi\"\n\tand left\\""#);
        // a control char below 0x20 that has no short escape -> \u00XX
        let v2 = Value::Str("\u{0001}".to_string());
        let mut want = String::from('"');
        want.push('\\');
        want.push_str("u0001");
        want.push('"');
        assert_eq!(v2.to_string(), want);
    }

    #[test]
    fn build_from() {
        let obj: Vec<(String, Value)> = vec![
            ("jsonrpc".to_string(), Value::from("2.0")),
            ("id".to_string(), Value::from(7i64)),
            ("done".to_string(), Value::from(true)),
        ];
        let v = Value::from(obj);
        assert_eq!(v.to_string(), r#"{"jsonrpc":"2.0","id":7,"done":true}"#);
    }

    #[test]
    fn errors() {
        assert!(Value::parse(b"").is_err()); // empty
        assert!(Value::parse(b"[1,2").is_err()); // unterminated array
        assert!(Value::parse(b"{\"a\":}").is_err()); // missing value
        assert!(Value::parse(b"nul").is_err()); // bad literal
        assert!(Value::parse(b"01").is_err()); // leading zero -> trailing byte
        assert!(Value::parse(b"1.").is_err()); // dangling decimal
        assert!(Value::parse(b"\"unterminated").is_err());
        assert!(Value::parse(b"\"\\ud834\"").is_err()); // lone high surrogate
        assert!(Value::parse(b"1 2").is_err()); // trailing value
        assert!(Value::parse("\"raw\ttab\"".as_bytes()).is_err()); // unescaped control
    }

    #[test]
    fn depth_guard() {
        // MAX_DEPTH nested arrays parse; one deeper errors (no stack blowup).
        let ok: String =
            core::iter::repeat('[').take(MAX_DEPTH).chain(core::iter::repeat(']').take(MAX_DEPTH)).collect();
        assert!(Value::parse(ok.as_bytes()).is_ok());
        let too_deep: String = core::iter::repeat('[')
            .take(MAX_DEPTH + 1)
            .chain(core::iter::repeat(']').take(MAX_DEPTH + 1))
            .collect();
        let e = Value::parse(too_deep.as_bytes()).unwrap_err();
        assert_eq!(e.msg, "maximum nesting depth exceeded");
    }

    #[test]
    fn accessors_negative() {
        let v = p("42");
        assert_eq!(v.as_str(), None);
        assert_eq!(v.as_array(), None);
        assert_eq!(v.get("k"), None);
        assert_eq!(Value::Float(1.0).as_i64(), Some(1)); // integral float
        assert_eq!(Value::Float(1.5).as_i64(), None);
    }
}
