// A JSON subset parser for the viv manifest (docs/VIVARIUM.md section 7.2:
// an OCI runtime-spec config.json SUBSET). Complete JSON *syntax* -- objects,
// arrays, strings with the full escape set, numbers, booleans, null -- so any
// well-formed config.json parses and unknown fields skip cleanly; but numbers
// carry no value (nothing in the read subset is numeric) and there is no
// serializer. Recursive descent with a hard depth cap; every error is a
// static string naming the failure, and a parse error fails the whole
// manifest closed (viv refuses to run a container it only partly understood).

use alloc::string::String;
use alloc::vec::Vec;

/// Nesting bound. config.json's real depth is ~3; 16 leaves headroom while
/// keeping a hostile deeply-nested document from exhausting the stack.
const DEPTH_MAX: usize = 16;

pub enum Json {
    Obj(Vec<(String, Json)>),
    Arr(Vec<Json>),
    Str(String),
    Num,
    Bool(bool),
    Null,
}

impl Json {
    /// Object member lookup (first match; JSON objects with duplicate keys are
    /// malformed input we tolerate by taking the first).
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(m) => m.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_arr(&self) -> Option<&[Json]> {
        match self {
            Json::Arr(a) => Some(a),
            _ => None,
        }
    }
}

pub fn parse(b: &[u8]) -> Result<Json, &'static str> {
    let mut p = P { b, i: 0 };
    p.ws();
    let v = p.value(0)?;
    p.ws();
    if p.i != p.b.len() {
        return Err("trailing bytes after the document");
    }
    Ok(v)
}

struct P<'a> {
    b: &'a [u8],
    i: usize,
}

impl<'a> P<'a> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.b.get(self.i).copied()
    }

    fn eat(&mut self, c: u8) -> bool {
        if self.peek() == Some(c) {
            self.i += 1;
            true
        } else {
            false
        }
    }

    fn lit(&mut self, s: &[u8]) -> bool {
        if self.b.len() - self.i >= s.len() && &self.b[self.i..self.i + s.len()] == s {
            self.i += s.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self, depth: usize) -> Result<Json, &'static str> {
        if depth > DEPTH_MAX {
            return Err("nesting too deep");
        }
        match self.peek() {
            Some(b'{') => self.obj(depth),
            Some(b'[') => self.arr(depth),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => {
                if self.lit(b"true") {
                    Ok(Json::Bool(true))
                } else {
                    Err("bad literal")
                }
            }
            Some(b'f') => {
                if self.lit(b"false") {
                    Ok(Json::Bool(false))
                } else {
                    Err("bad literal")
                }
            }
            Some(b'n') => {
                if self.lit(b"null") {
                    Ok(Json::Null)
                } else {
                    Err("bad literal")
                }
            }
            Some(c) if c == b'-' || c.is_ascii_digit() => self.num(),
            _ => Err("expected a value"),
        }
    }

    fn obj(&mut self, depth: usize) -> Result<Json, &'static str> {
        self.i += 1; // '{'
        let mut m: Vec<(String, Json)> = Vec::new();
        self.ws();
        if self.eat(b'}') {
            return Ok(Json::Obj(m));
        }
        loop {
            self.ws();
            if self.peek() != Some(b'"') {
                return Err("expected an object key");
            }
            let k = self.string()?;
            self.ws();
            if !self.eat(b':') {
                return Err("expected ':'");
            }
            self.ws();
            let v = self.value(depth + 1)?;
            m.push((k, v));
            self.ws();
            if self.eat(b',') {
                continue;
            }
            if self.eat(b'}') {
                return Ok(Json::Obj(m));
            }
            return Err("expected ',' or '}'");
        }
    }

    fn arr(&mut self, depth: usize) -> Result<Json, &'static str> {
        self.i += 1; // '['
        let mut a: Vec<Json> = Vec::new();
        self.ws();
        if self.eat(b']') {
            return Ok(Json::Arr(a));
        }
        loop {
            self.ws();
            let v = self.value(depth + 1)?;
            a.push(v);
            self.ws();
            if self.eat(b',') {
                continue;
            }
            if self.eat(b']') {
                return Ok(Json::Arr(a));
            }
            return Err("expected ',' or ']'");
        }
    }

    fn string(&mut self) -> Result<String, &'static str> {
        self.i += 1; // '"'
        let mut s = String::new();
        loop {
            let c = self.peek().ok_or("unterminated string")?;
            self.i += 1;
            match c {
                b'"' => return Ok(s),
                b'\\' => {
                    let e = self.peek().ok_or("unterminated escape")?;
                    self.i += 1;
                    match e {
                        b'"' => s.push('"'),
                        b'\\' => s.push('\\'),
                        b'/' => s.push('/'),
                        b'b' => s.push('\u{0008}'),
                        b'f' => s.push('\u{000C}'),
                        b'n' => s.push('\n'),
                        b'r' => s.push('\r'),
                        b't' => s.push('\t'),
                        b'u' => {
                            let hi = self.hex4()?;
                            let ch = if (0xD800..0xDC00).contains(&hi) {
                                // A high surrogate must pair with \uDC00..DFFF.
                                if !self.lit(b"\\u") {
                                    return Err("lone surrogate");
                                }
                                let lo = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&lo) {
                                    return Err("bad surrogate pair");
                                }
                                let cp =
                                    0x10000 + (((hi - 0xD800) as u32) << 10) + (lo - 0xDC00) as u32;
                                char::from_u32(cp).ok_or("bad codepoint")?
                            } else if (0xDC00..0xE000).contains(&hi) {
                                return Err("lone surrogate");
                            } else {
                                char::from_u32(hi as u32).ok_or("bad codepoint")?
                            };
                            s.push(ch);
                        }
                        _ => return Err("bad escape"),
                    }
                }
                0x00..=0x1F => return Err("control byte in string"),
                _ => {
                    // Raw UTF-8 passes through byte-for-byte; validate at the
                    // end of the manifest extraction (the consumers all take
                    // &str via from_utf8 on the byte path -- here we already
                    // hold bytes, so accumulate and validate now).
                    let start = self.i - 1;
                    // Consume the UTF-8 continuation bytes of a multi-byte
                    // sequence so char boundaries stay intact.
                    let extra = match c {
                        0xC0..=0xDF => 1,
                        0xE0..=0xEF => 2,
                        0xF0..=0xF7 => 3,
                        0x80..=0xBF | 0xF8..=0xFF => return Err("bad UTF-8"),
                        _ => 0,
                    };
                    if self.i + extra > self.b.len() {
                        return Err("truncated UTF-8");
                    }
                    self.i += extra;
                    let chunk = core::str::from_utf8(&self.b[start..self.i])
                        .map_err(|_| "bad UTF-8")?;
                    s.push_str(chunk);
                }
            }
        }
    }

    fn hex4(&mut self) -> Result<u16, &'static str> {
        let mut v: u16 = 0;
        for _ in 0..4 {
            let c = self.peek().ok_or("truncated \\u escape")?;
            self.i += 1;
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => return Err("bad \\u escape"),
            };
            v = (v << 4) | d as u16;
        }
        Ok(v)
    }

    /// Numbers: syntax-validated, value discarded (nothing in the manifest
    /// subset is numeric; unknown numeric fields must still SKIP cleanly).
    fn num(&mut self) -> Result<Json, &'static str> {
        let _ = self.eat(b'-');
        let mut digits = 0;
        while self.peek().is_some_and(|c| c.is_ascii_digit()) {
            self.i += 1;
            digits += 1;
        }
        if digits == 0 {
            return Err("bad number");
        }
        if self.eat(b'.') {
            let mut frac = 0;
            while self.peek().is_some_and(|c| c.is_ascii_digit()) {
                self.i += 1;
                frac += 1;
            }
            if frac == 0 {
                return Err("bad number");
            }
        }
        if self.peek() == Some(b'e') || self.peek() == Some(b'E') {
            self.i += 1;
            if self.peek() == Some(b'+') || self.peek() == Some(b'-') {
                self.i += 1;
            }
            let mut exp = 0;
            while self.peek().is_some_and(|c| c.is_ascii_digit()) {
                self.i += 1;
                exp += 1;
            }
            if exp == 0 {
                return Err("bad number");
            }
        }
        Ok(Json::Num)
    }
}

/// Deterministic self-vectors (the diorama selftest-before-serve pattern,
/// scaled to viv's one parser). Run before any container assembly; a failure
/// gates the run.
pub fn selftest() -> Result<(), &'static str> {
    // The real manifest shape parses and extracts.
    let doc = parse(
        br#"{
        "ociVersion": "1.0.2",
        "root": { "path": "rootfs", "readonly": true },
        "process": {
            "args": ["/bin/viv-probe"],
            "env": ["VIV_EXPECT_UID=4294967294", "A=b c"],
            "cwd": "/",
            "user": { "uid": 0, "gid": 0 }
        },
        "annotations": { "org.thylacine.net": "granted" }
    }"#,
    )?;
    if doc.get("root").and_then(|r| r.get("path")).and_then(|p| p.as_str()) != Some("rootfs") {
        return Err("root.path extract");
    }
    if doc.get("root").and_then(|r| r.get("readonly")).and_then(|p| p.as_bool()) != Some(true) {
        return Err("root.readonly extract");
    }
    let args = doc
        .get("process")
        .and_then(|p| p.get("args"))
        .and_then(|a| a.as_arr())
        .ok_or("process.args extract")?;
    if args.len() != 1 || args[0].as_str() != Some("/bin/viv-probe") {
        return Err("args content");
    }
    if doc
        .get("annotations")
        .and_then(|a| a.get("org.thylacine.net"))
        .and_then(|v| v.as_str())
        != Some("granted")
    {
        return Err("annotation extract");
    }
    // Unknown numeric fields skip; escapes decode; absent keys are None.
    let esc = parse(br#"{"s": "a\u0041\n\"\\ \u00e9 \ud83d\ude00", "n": -1.5e3}"#)?;
    if esc.get("s").and_then(|v| v.as_str()) != Some("aA\n\"\\ \u{e9} \u{1F600}") {
        return Err("escape decode");
    }
    if esc.get("missing").is_some() {
        return Err("phantom key");
    }
    // Malformed documents FAIL, never half-parse.
    for bad in [
        &b"{"[..],
        &b"{\"a\": }"[..],
        &b"[1,]"[..],
        &b"\"lone \\ud800\""[..],
        &b"{} extra"[..],
        &b"{\"a\": 01e}"[..],
    ] {
        if parse(bad).is_ok() {
            return Err("malformed doc accepted");
        }
    }
    // The depth cap holds.
    let mut deep: Vec<u8> = Vec::new();
    for _ in 0..40 {
        deep.push(b'[');
    }
    for _ in 0..40 {
        deep.push(b']');
    }
    if parse(&deep).is_ok() {
        return Err("depth cap");
    }
    Ok(())
}
