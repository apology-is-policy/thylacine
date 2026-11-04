#!/usr/bin/env python3
"""A scenario's bounded wait must not put a flag beside a single block of arms.

`expect -timeout N {arms}` has a flag and one more argument, so expect reads the
block as one literal pattern with no action. No arm is armed: the wait runs out and
returns, and no failure arm in it can fire. Only a bare `expect {arms}` is read as
arms. This has happened twice: lib.exp's lc_fail_detail, and every wait in
ls-halcyon-paste-flow.exp. lc_within is the form. `expect -timeout N -re {a} {b}` and
`expect -timeout N {a} {b}` are arms, and fine.

Run with no arguments to check the tree (rc 1 on a hit) or --self-test for the
checker's own controls. --root DIR checks another tree.
"""
import argparse
import pathlib
import re
import sys

# A command word: the start of a line, or after `;`, `{` or `[`.
COMMAND = re.compile(r"(?:^|[;{\[])[ \t]*expect(?=[ \t\\])", re.M)
# Flags of the command itself, each with its value. A pattern's own flags come after.
TAKES_VALUE = {"-timeout", "-i"}


def line_of(text, at):
    return text.count("\n", 0, at) + 1


def closing(text, at, opener, closer):
    """Index of the closer of the group opening at `at`, or len(text)."""
    depth = 0
    while at < len(text):
        c = text[at]
        if c == "\\":
            at += 2
            continue
        depth += (c == opener) - (c == closer)
        if depth == 0:
            return at
        at += 1
    return len(text)


def words(text, at):
    """The words of the command from `at` to its end, as Tcl splits them."""
    out = []
    while at < len(text):
        c = text[at]
        if c == "\\" and text[at + 1 : at + 2] == "\n":
            at += 2
        elif c in " \t":
            at += 1
        elif c in "\n;]}":
            break
        elif c == "{":
            end = closing(text, at, "{", "}")
            out.append(text[at : end + 1])
            at = end + 1
        elif c == '"':
            end = at + 1
            while end < len(text) and text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            out.append(text[at : end + 1])
            at = end + 1
        else:
            begin = at
            while at < len(text) and text[at] not in " \t\n;]}":
                if text[at] == "[":
                    at = closing(text, at, "[", "]") + 1
                elif text[at] == "\\":
                    at += 2
                else:
                    at += 1
            out.append(text[begin:at])
    return out


def dead(args):
    flagged = False
    i = 0
    while i < len(args):
        if args[i] == "-brace":
            return False
        if args[i] in TAKES_VALUE:
            flagged = True
            i += 2
        else:
            break
    return flagged and len(args) - i == 1


def findings(text):
    hits = []
    for m in COMMAND.finditer(text):
        begin = text.rfind("\n", 0, m.start() + 1) + 1
        if text[begin : m.end()].lstrip().startswith("#"):
            continue
        if dead(words(text, m.end())):
            hits.append(line_of(text, m.end()))
    return hits


def self_test():
    bad = [
        'expect -timeout 5 {\n    -ex "a" { puts b }\n    timeout {}\n}\n',
        'proc p {} {\n    expect -timeout $t {\n        eof { lc_fail x }\n    }\n}\n',
        'expect -timeout 5 { -ex "a" {} }\n',
        'if {$x} { expect -timeout 5 { -ex "a" {} } }\n',
        'set r [expect -timeout 5 {\n    -ex "a" {}\n}]\n',
        'expect -i $id -timeout 5 {\n    -ex "a" {}\n}\n',
        'expect -i $id {\n    -ex "a" {}\n}\n',
        'expect -timeout [expr {$t + 1}] {\n    -ex "a" {}\n}\n',
        'expect -timeout 5 \\\n    {\n    -ex "a" {}\n}\n',
        'expect -timeout 5 $arms\n',
    ]
    good = [
        'expect -timeout 5 -re {a} {puts b} timeout {}\n',
        'expect -timeout 5 {a} {puts b}\n',
        'expect -timeout 5 {a} \\\n    {puts b}\n',
        'set timeout 5\nexpect {\n    -ex "a" {}\n}\n',
        'lc_within 5 {\n    -ex "a" {}\n}\n',
        '# not `expect -timeout 5 {...}`\n',
        'catch { expect -timeout 5 timeout {} eof {} }\n',
        'expect -i $id -re {a} {b}\n',
        'expect -brace -timeout 5 {\n    -ex "a" {}\n}\n',
        'puts "expect -timeout 5 {a}"\n',
        'expect_before -timeout 5 {a}\n',
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
            print(f"{f}:{line}: expect with a flag and one block matches nothing; use lc_within")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
