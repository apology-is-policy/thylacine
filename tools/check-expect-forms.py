#!/usr/bin/env python3
"""A scenario's bounded wait must not put -timeout beside a braced block.

`expect -timeout N {arms}` has a flag and one more argument, so expect reads the
block as one literal pattern with no action. No arm is armed: the wait runs out and
returns, and no failure arm in it can fire. This has happened twice: lib.exp's
lc_fail_detail, and every wait in ls-halcyon-paste-flow.exp. lc_within is the form.
`expect -timeout N -re {a} {b}` and `expect -timeout N {a} {b}` are arms, and fine.

Run with no arguments to check the tree (rc 1 on a hit) or --self-test for the
checker's own controls. --root DIR checks another tree.
"""
import argparse
import pathlib
import re
import sys

OPEN = re.compile(r"(?:^|[;{\s])expect\s+-timeout\s+\S+\s+\{", re.M)


def line_of(text, at):
    return text.count("\n", 0, at) + 1


def findings(text):
    hits = []
    for m in OPEN.finditer(text):
        begin = text.rfind("\n", 0, m.end()) + 1
        if text[begin:m.end()].lstrip().startswith("#"):
            continue
        at, depth = m.end() - 1, 0
        while at < len(text):
            c = text[at]
            if c == "\\":
                at += 2
                continue
            depth += (c == "{") - (c == "}")
            if depth == 0:
                break
            at += 1
        rest = text[at + 1:].split("\n", 1)[0].strip()
        if rest == "" or rest[0] in ";}":
            hits.append(line_of(text, m.end()))
    return hits


def self_test():
    bad = [
        'expect -timeout 5 {\n    -ex "a" { puts b }\n    timeout {}\n}\n',
        'proc p {} {\n    expect -timeout $t {\n        eof { lc_fail x }\n    }\n}\n',
        'expect -timeout 5 { -ex "a" {} }\n',
        'if {$x} { expect -timeout 5 { -ex "a" {} } }\n',
    ]
    good = [
        'expect -timeout 5 -re {a} {puts b} timeout {}\n',
        'expect -timeout 5 {a} {puts b}\n',
        'expect -timeout 5 {a} \\\n    {puts b}\n',
        'set timeout 5\nexpect {\n    -ex "a" {}\n}\n',
        'lc_within 5 {\n    -ex "a" {}\n}\n',
        '# not `expect -timeout 5 {...}`\n',
    ]
    ok = True
    for t in bad:
        if not findings(t):
            print("check-expect-forms: self-test MISSED:", repr(t))
            ok = False
    for t in good:
        if findings(t):
            print("check-expect-forms: self-test FALSE HIT:", repr(t))
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    args = ap.parse_args()
    if args.self_test:
        return 0 if self_test() else 1
    files = sorted(pathlib.Path(args.root, "tools").rglob("*.exp"))
    if not files:
        print("check-expect-forms: no .exp under", args.root, "-- the check read nothing")
        return 1
    bad = 0
    for f in files:
        for line in findings(f.read_text(errors="replace")):
            print(f"{f}:{line}: expect -timeout beside a braced block matches nothing; use lc_within")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
