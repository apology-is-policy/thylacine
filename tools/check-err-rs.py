#!/usr/bin/env python3
"""libthyla-rs's three errno tables agree: every Error variant's `as_errno` value decodes back to that variant
through `From<i32>`, every decode is some variant's `as_errno`, a library-only variant (errno 0) is never decoded,
and every variant has a `Display` text.

libthyla-rs cannot be host-tested (tools/test-rust.sh files it NO-HOST: its `_start` asm and panic handler), so a
`#[test]` there would run nowhere; this is the pin instead. A server errno with no T_E_* name crosses by value
(docs/ERRORS.md), so the values are not asked of errno.h.

Run with --root <repo> to check the tree (rc 1 on a mismatch), or --self-test for the checker's own cases."""
import argparse
import re
import sys

ERR_RS = "usr/lib/libthyla-rs/src/err.rs"


def match_body(src, sig):
    """The braces of the first `match` after `sig`, or None."""
    i = src.find(sig)
    if i < 0:
        return None
    j = src.find("match", i)
    k = src.find("{", j) if j >= 0 else -1
    if k < 0:
        return None
    depth = 0
    for p in range(k, len(src)):
        depth += {"{": 1, "}": -1}.get(src[p], 0)
        if depth == 0:
            return src[k:p]
    return None


def check(src):
    """The mismatches in an err.rs source, as lines; empty when the tables agree."""
    enc_b = match_body(src, "fn as_errno")
    dec_b = match_body(src, "fn from(errno: i32)")
    dis_b = match_body(src, "impl fmt::Display for Error")
    missing = [n for n, b in (("as_errno", enc_b), ("From<i32>", dec_b), ("Display", dis_b)) if b is None]
    if missing:
        return ["no %s match found" % m for m in missing]
    enc = {v: int(n) for v, n in re.findall(r"Error::([A-Za-z]+)\s*=>\s*(\d+)\s*,", enc_b)}
    dec = [(int(n), v) for n, v in re.findall(r"^\s*(\d+)\s*=>\s*Error::([A-Za-z]+)\s*,", dec_b, re.M)]
    dis = set(re.findall(r"Error::([A-Za-z]+)\s*=>\s*\"", dis_b))
    bad = []
    seen = {}
    for n, v in dec:
        if n in seen:
            bad.append("From<i32>: %d decodes twice (%s, %s)" % (n, seen[n], v))
        seen[n] = v
        if n == 0:
            bad.append("From<i32>: 0 decodes to %s (errno 0 is no error)" % v)
        elif enc.get(v) != n:
            bad.append("From<i32>: %d => %s, whose as_errno is %s" % (n, v, enc.get(v)))
    for v, n in sorted(enc.items(), key=lambda kv: (kv[1], kv[0])):
        if n != 0 and seen.get(n) != v:
            bad.append("as_errno: %s => %d, which From<i32> decodes to %s" % (v, n, seen.get(n)))
        if v not in dis:
            bad.append("Display: %s has no text" % v)
    if not enc or not dec:
        bad.append("an empty table (as_errno %d arms, From<i32> %d arms)" % (len(enc), len(dec)))
    return bad


GOOD = """
impl Error {
    pub fn as_errno(&self) -> i32 {
        match self {
            Error::NotFound => 2,
            Error::CrossesDevices => 18,
            Error::Other(e) => e,
            Error::UnexpectedEof => 0,
        }
    }
}
impl From<i32> for Error {
    fn from(errno: i32) -> Self {
        match errno {
            2   => Error::NotFound,
            18  => Error::CrossesDevices,
            e   => Error::Other(e),
        }
    }
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            Error::NotFound => "no such file",
            Error::CrossesDevices => "invalid cross-device link",
            Error::UnexpectedEof => "unexpected end of file",
            Error::Other(_) => return write!(f, "error"),
        };
        f.write_str(s)
    }
}
"""


def self_test():
    cases = [
        ("the consistent tables", GOOD, 0),
        ("a decode moved to another number", GOOD.replace("18  => Error::CrossesDevices", "19  => Error::CrossesDevices"), 2),
        ("an encode moved to another number", GOOD.replace("Error::CrossesDevices => 18,", "Error::CrossesDevices => 19,"), 2),
        ("a variant with no text", GOOD.replace('Error::CrossesDevices => "invalid cross-device link",', ""), 1),
        ("a variant never decoded", GOOD.replace("18  => Error::CrossesDevices,", ""), 1),
        ("a library-only variant decoded", GOOD.replace("2   => Error::NotFound,", "2   => Error::NotFound,\n            0   => Error::UnexpectedEof,"), 1),
        ("no Display match", GOOD.replace("impl fmt::Display for Error", "impl fmt::Debug for Error"), 1),
    ]
    ok = True
    for name, src, want in cases:
        got = check(src)
        if len(got) != want:
            ok = False
            print("check-err-rs self-test: %s: %d finding(s), wanted %d: %s" % (name, len(got), want, got))
    print("check-err-rs self-test: " + ("ok, %d cases" % len(cases) if ok else "FAILED"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return 0 if self_test() else 1
    src = open(a.root.rstrip("/") + "/" + ERR_RS).read()
    bad = check(src)
    for b in bad:
        print("check-err-rs: %s: %s" % (ERR_RS, b))
    if not bad:
        print("check-err-rs: %s: the three tables agree" % ERR_RS)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
