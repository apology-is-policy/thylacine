#!/usr/bin/env python3
"""A scenario's wait must give expect its arms in a form expect reads as arms.

expect takes a single argument as a list of arms only when a newline comes before its
first word, and only when it is the sole argument. Anything else is one literal pattern
with no action: no arm is armed, the wait runs out and returns, and no failure arm in it
can fire. Run on expect 5.45, each of these fired nothing:

    expect -timeout N {arms}          a flag, and any flag: -i, -nocase, -iread, --, -notr
    expect { -ex "a" {} timeout {} }  on one line, and { -re foo }, { timeout exit }
    lc_within N { -ex "a" {} }        the same, since it passes its block on
    expect_before -i $id {arms}       expect_before and expect_after go by the same rule

This has happened twice: lib.exp's lc_fail_detail, and every wait in
ls-halcyon-paste-flow.exp. lc_within with its arms on their own lines is the form.
`expect -timeout N -re {a} {b}`, `expect -timeout N {a} {b}`, `expect -timeout N "a"` and
`expect {a}` are not blocks of arms, and fine. A variable or a command after a flag is a
hit, because its contents cannot be read from here: `expect -timeout N "$pat"` says it is
one pattern. The same wait through `uplevel N` or `eval` is a hit too, because `eval`
strips a level of braces and this reads none of it: write it directly.

Run with no arguments to check the tree (rc 1 on a hit) or --self-test for the
checker's own controls. --root DIR checks another tree.
"""
import argparse
import pathlib
import re
import sys

# A command word: the start of a line, or after `;`, `{` or `[`.
COMMAND = re.compile(r"(?:^|[;{\[])[ \t]*(expect_before|expect_after|expect|lc_within)(?=[ \t\\])", re.M)
INDIRECT = re.compile(r"(?:^|[;{\[])[ \t]*(?:uplevel[ \t]+#?\d+|eval)[ \t]+(?:expect|lc_within)\b", re.M)
# These end the command's own flags. -brace is an error on expect 5.45, so it is no quiet wait.
NOT_A_FLAG = {"-re", "-ex", "-gl", "-brace"}
ARM_WORDS = {"timeout", "eof", "default", "full_buffer", "null"}


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


def holds_arms(block):
    """Whether a braced word is meant as arms and not as one pattern."""
    inner = block[1:-1]
    if "\n" in inner or re.search(r"[ \t]\{", inner) is not None:
        return True
    parts = inner.split()
    return len(parts) > 1 and (parts[0].startswith("-") or parts[0] in ARM_WORDS)


def dead(args, within=False):
    flagged = False
    i = 1 if within else 0
    while i < len(args) and args[i].startswith("-") and args[i] not in NOT_A_FLAG:
        flagged = True
        takes_value = args[i] == "-i" or (len(args[i]) > 2 and "-timeout".startswith(args[i]))
        i += 2 if takes_value else 1
    if len(args) - i != 1:
        return False
    word = args[i]
    if word.startswith("{"):
        if not flagged and re.match(r"\{[ \t]*\n", word):
            return False
        return holds_arms(word)
    return flagged and word[0] in "$["


def findings(text):
    hits = []
    for m in COMMAND.finditer(text):
        begin = text.rfind("\n", 0, m.start() + 1) + 1
        if text[begin : m.end()].lstrip().startswith("#"):
            continue
        if dead(words(text, m.end()), m.group(1) == "lc_within"):
            hits.append(line_of(text, m.end()))
    for m in INDIRECT.finditer(text):
        begin = text.rfind("\n", 0, m.start() + 1) + 1
        if not text[begin : m.end()].lstrip().startswith("#"):
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
        'expect { -ex "a" {} timeout {} }\n',
        'expect { eof {} timeout {} }\n',
        'catch { expect { eof {} } }\n',
        'lc_within 5 { -ex "a" {} timeout {} }\n',
        'expect -notransfer -timeout 5 {\n    -ex "a" {}\n}\n',
        'expect -timeout 5 -nocase {\n    -ex "a" {}\n}\n',
        'expect -timeo 5 {\n    -ex "a" {}\n}\n',
        'expect { # a comment\n    -ex "a" {}\n}\n',
        'expect -iread {\n    -ex "a" {}\n}\n',
        'expect -timestamp {\n    -ex "a" {}\n}\n',
        'expect -notr {\n    -ex "a" {}\n}\n',
        'expect -noc {\n    -ex "a" {}\n}\n',
        'expect -ind {\n    -ex "a" {}\n}\n',
        'expect -- {\n    -ex "a" {}\n}\n',
        'expect -time 5 {\n    -ex "a" {}\n}\n',
        'expect { -re foo }\n',
        'expect { timeout exit }\n',
        'expect_before { -ex "a" {} timeout {} }\n',
        'expect_before -i $id {\n    -ex "a" {}\n}\n',
        'expect_after -i $id {\n    -ex "a" {}\n}\n',
        'uplevel 1 expect {\n    -ex "a" {}\n}\n',
        'eval expect {{\n    -ex "a" {}\n}}\n',
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
        'expect -timeout 5 "login:"\n',
        'expect -timeout 5 {login:}\n',
        'expect {login: x}\n',
        'expect {  \n    -ex "a" {}\n}\n',
        'expect -re {a b}\n',
        'expect $arms\n',
        'lc_within 5 {\n    timeout {}\n}\n',
        'proc lc_within {n arms} {\n}\n',
        'expect_before {\n    -ex "a" {}\n}\n',
        'expect -timeout 5 "$pat"\n',
        'uplevel 1 [list expect $arms]\n',
        'expect {timeout}\n',
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
            print(f"{f}:{line}: expect reads this block as one pattern, so nothing in it is armed; put the arms on their own lines, with lc_within for a limit")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
