#!/usr/bin/env python3
"""A scenario needle must not read across two console writes.

The kernel keeps one console write whole against other writers (cons_output_write)
and promises nothing between two, so a line written in pieces can have any other
process's line land inside it. A needle spanning a piece boundary then misses on
that boot and the gate reports the wrong failure. This has happened: q-seat7's
"typing did not reach the shell" was a witness line split by a status-bar line.

Run with no arguments to check the tree (rc 1 on a hit) or --self-test for the
checker's own controls. --root DIR checks another tree.

It reads source text, so it sees literal pieces passed to the writers named below.
A needle must start in literal text, so a line whose text is all variable with its
newline written separately is not seen, nor is a writer helper it does not name.
"""
import argparse
import os
import re
import sys

# Console writers. A call with a string literal contributes that text; any other
# argument contributes a VAR. `uart_puts` is the kernel's, the rest are guest code's.
WRITERS = r"t_putstr|putbytes|puts|put_hex64|put_hex|put_dec|put_u64|uart_puts|uart_putdec|uart_puthex64"
CALL = re.compile(r"(?<![\w.])(" + WRITERS + r")\(\s*(.*?)\s*\)\s*;", re.S)
LITERAL = re.compile(r'^b?"((?:[^"\\]|\\.)*)"$')
# Non-console work between two pieces this far apart still leaves a window, but a
# piece further away than this is a different line.
MAX_GAP_LINES = 12
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# A crossing match landing on less literal text than this names no line: "/viv/abin/tar"
# fits "/<var>" and means nothing by it.
MIN_LITERAL = 8
# A line that starts with one of these is exempt, for the stated reason.
EXEMPT = {
    "  kernel base: ": "printed before a second CPU or any process exists",
    "HALLS: KASLR offset ": "the KASLR needles want the boot line, which comes first",
    "boot-wc: parks=": "cpu-gate's needle reads /ctl/sched's wc: line, not this boot-log one",
}
VAR = None
WILD = "\0"
WILD_RE = r"[^\n]*"


def unescape(lit):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "r": "\r", "t": "\t", "0": ""}.get(m.group(1), m.group(1)), lit)


def templates(text):
    """Each line a function writes in pieces: [(part, line), ...], a part being text or VAR."""
    out, cur, last, line, at = [], [], 0, 1, 0
    def flush():
        nonlocal cur
        if len(cur) > 1:
            out.append(cur)
        cur = []
    for m in CALL.finditer(text):
        line += text.count("\n", at, m.start())
        at = m.start()
        if cur and line - last > MAX_GAP_LINES:
            flush()
        last = line
        lit = LITERAL.match(m.group(2))
        if lit is None:
            cur.append((VAR, line))
            continue
        body = unescape(lit.group(1))
        cur.append((body, line))
        if body.endswith("\n"):
            flush()
    flush()
    return out


NEEDLE_ARG = r'("(?:[^"\\]|\\.)*"|\{[^{}]*\})'
NEEDLE_FORMS = [
    (re.compile(r"(?:-ex|-exact|lc_expect)\s+" + NEEDLE_ARG), False),
    (re.compile(r"(?:-re|lc_expect_re)\s+" + NEEDLE_ARG), True),
    (re.compile(r"lc_run_expect(?:_hardfail)?\s+" + NEEDLE_ARG + r"\s+" + NEEDLE_ARG), False),
]


TCL_SUBST = re.compile(r"\$\{?\w+\}?|\$::\w+(?:\([^)]*\))?|\[[a-z_][^\]]*\]")


def tcl_literal(arg):
    if arg.startswith("{"):
        return arg[1:-1]
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "r": "\r"}.get(m.group(1), m.group(1)), arg[1:-1])


def regex_alternatives(rx):
    """A regex needle as token strings: literal text, WILD for anything variable."""
    alts, cur, depth, i = [], [], 0, 0
    while i < len(rx):
        c = rx[i]
        if c == "\\" and i + 1 < len(rx):
            n = rx[i + 1]
            i += 2
            if n in "rn":
                cur.append("\n" if n == "n" else "\r")
            elif n in "dsSwW":
                cur.append(WILD)
            else:
                cur.append(n)
            continue
        if c == "[":
            j = rx.find("]", i + 2)
            if j < 0:
                break
            cur.append(WILD)
            i = j + 1
            continue
        if c == "|" and depth == 0:
            alts.append("".join(cur))
            cur = []
            i += 1
            continue
        if c == "|":
            cur.append(WILD)  # an inner alternation: the choice is variable text here
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth = max(0, depth - 1)
        elif c == ".":
            cur.append(WILD)
        elif c in "*+?{}^$":
            pass
        else:
            cur.append(c)
        i += 1
    alts.append("".join(cur))
    return [re.sub(WILD + "+", WILD, a).replace("\r", "").replace("\n", "") for a in alts]


def needles(text):
    found = []
    for ln, line in enumerate(text.splitlines(), 1):
        for form, is_re in NEEDLE_FORMS:
            for m in form.finditer(line):
                arg = m.group(m.lastindex)
                if arg.startswith('"'):
                    arg = TCL_SUBST.sub(WILD, arg)  # the value is variable text here
                tokens = regex_alternatives(arg[1:-1]) if is_re else [tcl_literal(arg)]
                found.extend((ln, t) for t in tokens if t.strip(WILD + " "))
    return found


def crossing(needle, parts):
    """How well this needle matches text crossing a write boundary of this line.

    Returns the most needle tokens any crossing match lands on literal text, 0 for
    no crossing match. A VAR can be any text, so a needle could always be absorbed
    by one: the match must start in literal text, where a needle names its line,
    and the count lets the caller blame the line that fits best rather than every
    line that starts the same way.
    """
    tokens = []
    for idx, part in enumerate(parts):
        if part is VAR:
            tokens.append((VAR, idx))
        else:
            tokens.extend((c, idx) for c in part)
    n, t = len(needle), len(tokens)
    best = 0
    for start in range(t):
        if tokens[start][0] is VAR or tokens[start][0] != needle[0]:
            continue
        seen, stack = {}, [(0, start, tokens[start][1], False, 0)]
        while stack:
            i, j, part, crossed, lit = stack.pop()
            if i == n:
                if crossed:
                    best = max(best, lit)
                continue
            if j >= t or seen.get((i, j, crossed), -1) >= lit:
                continue
            seen[(i, j, crossed)] = lit
            tok, idx = tokens[j]
            next_crossed = crossed or idx != part
            if tok is VAR:
                stack.append((i + 1, j, idx, next_crossed, lit))  # the VAR absorbs this needle token
                stack.append((i, j + 1, idx, next_crossed, lit))  # or ends
            elif needle[i] == WILD:
                stack.append((i + 1, j, part, crossed, lit))      # the WILD ends here
                stack.append((i, j + 1, idx, next_crossed, lit))  # or absorbs this text
            elif needle[i] == tok:
                stack.append((i + 1, j + 1, idx, next_crossed, lit + 1))
    return best


def printed_whole(needle, whole):
    """One string in the source can print it, so it needs no pieces.

    WILD is variable text there, perhaps a format directive. Each string is one line of
    `whole`, so the longest literal run finds the candidates and a regex settles each.
    """
    chunks = needle.split(WILD)
    longest = max(chunks, key=len)
    if WILD not in needle:
        return longest in whole
    rx = re.compile(WILD_RE.join(re.escape(x) for x in chunks))
    at = whole.find(longest)
    while at >= 0:
        start = whole.rfind("\n", 0, at) + 1
        end = whole.find("\n", at)
        if rx.search(whole, start, end if end >= 0 else len(whole)):
            return True
        at = whole.find(longest, end if end >= 0 else len(whole))
    return False


def exempt(template):
    first = template[0][0]
    return first is not VAR and first in EXEMPT


def source_files(root):
    for top in ("usr", "kernel", "mm", "arch"):
        for d, subdirs, files in os.walk(os.path.join(root, top)):
            subdirs[:] = [s for s in subdirs if s not in ("target", "third_party", "build")]
            for f in files:
                if f.endswith((".rs", ".c")):
                    yield os.path.join(d, f)


def scenario_files(root):
    d = os.path.join(root, "tools", "interactive")
    for f in sorted(os.listdir(d)):
        if f.endswith(".exp"):
            yield os.path.join(d, f)


def check(root):
    tpl, literals = [], []
    for path in source_files(root):
        with open(path, errors="replace") as fh:
            text = fh.read()
        literals.extend(unescape(m.group(1)) for m in STRING.finditer(text))
        if "put" in text:
            for t in templates(text):
                tpl.append((os.path.relpath(path, root), t, "".join(p for p, _ in t if p is not VAR)))
    whole = "\n".join(lit.replace("\n", " ") for lit in literals)
    ndl = []
    for path in scenario_files(root):
        with open(path, errors="replace") as fh:
            ndl.extend((os.path.basename(path), ln, n) for ln, n in needles(fh.read()))
    hits = []
    for scen, ln, n in ndl:
        if printed_whole(n, whole):
            continue
        # The match starts in literal text, so a line without the first two tokens in a
        # row could only start the needle in a piece one character long.
        head = n.split(WILD)[0][:2]
        scored = [(crossing(n, [p for p, _ in t]), path, t) for path, t, text in tpl if head in text]
        top = max((c for c, _, _ in scored), default=0)
        for c, path, t in scored:
            if c >= MIN_LITERAL and c == top and not exempt(t):
                shown = "".join("<var>" if p is VAR else p for p, _ in t).strip()
                hits.append((path, t[0][1], shown, scen, ln, n))
    return len(tpl), len(ndl), hits


def self_test():
    src = '''
    t_putstr("tag: PASS ");
    t_putstr(&name);
    t_putstr(" done\\n");
    t_putstr("whole line\\n");
    t_putstr("other: ");
    t_putstr("two\\n");
    '''
    tpl = [[p for p, _ in t] for t in templates(src)]
    cases = [
        ("tag: PASS base done", True, "through a VAR, written in three calls"),
        ("tag: PASS", False, "wholly inside the first write"),
        ("other: two", True, "two literal writes"),
        ("whole line", False, "one write"),
        ("not this PASS base done", False, "starts in no literal of the line"),
    ]
    bad = 0
    for needle, want, why in cases:
        got = any(crossing(needle, t) for t in tpl)
        if got != want:
            print(f"self-test FAIL: {needle!r} ({why}): crossed={got}")
            bad += 1
    rx = regex_alternatives(r"tag: PASS [a-z]+ done\r?\n|nothing")
    if rx != ["tag: PASS " + WILD + " done", "nothing"]:
        print(f"self-test FAIL: regex tokens {rx!r}")
        bad += 1
    if not any(crossing(rx[0], t) for t in tpl):
        print("self-test FAIL: a regex needle across the VAR")
        bad += 1
    found = [n for _, n in needles('lc_expect "a b" x\nexpect {\n -re {c [0-9]+ d} { }\n -ex "e" { }\n}\nlc_expect "f $g h" y\n')]
    if found != ["a b", "c " + WILD + " d", "e", "f " + WILD + " h"]:
        print(f"self-test FAIL: needle forms {found!r}")
        bad += 1
    best = crossing("tag: PASS x done", tpl[0]), crossing("tag: PASS x done", ["tag: PASS ", VAR, " never\n"])
    if not best[0] > best[1] > 0:
        print(f"self-test FAIL: the fitting line must outscore one that only starts alike: {best}")
        bad += 1
    print("self-test:", "FAIL" if bad else f"PASS ({len(cases) + 4} controls)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    n_tpl, n_ndl, hits = check(args.root)
    if n_tpl == 0 or n_ndl == 0:
        print(f"check-console-lines: FAIL: found {n_tpl} lines and {n_ndl} needles, so it checked nothing")
        return 2
    for path, line, shown, scen, ln, n in hits:
        print(f"{path}:{line}: {shown[:70]!r}\n    read whole by {scen}:{ln} {n.replace(WILD, '<any>')[:80]!r}")
    print(f"check-console-lines: {n_tpl} lines written in pieces, {n_ndl} needles, {len(hits)} spanning")
    if hits:
        print("Write each such line in one console call; two calls can have another process's line between them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
