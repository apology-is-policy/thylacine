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
It reads a needle given literally, through a proc that passes an argument on to
expect, or through a variable `set` to a literal in that file. Any other variable
needle (a foreach, a computed value) is not read, and the last line counts those.
Any string in code that can print a needle whole exempts that needle.
"""
import argparse
import os
import re
import sys

# Console writers. A call with a string literal contributes that text; any other
# argument contributes a VAR. `uart_puts` is the kernel's, the rest are guest code's.
WRITERS = r"t_putstr|putbytes|puts|put_hex64|put_hex|put_dec|put_u64|uart_puts|uart_putdec|uart_puthex64"
CALL = re.compile(r"(?<![\w.])(" + WRITERS + r")\(")
CHAR = re.compile(r"'(?:\\.[^']*|[^\\'\n])'")
# Longest first in effect: a quote or a comment opener met first owns what follows.
SOURCE_TOKEN = re.compile(r'''r(#+)".*?"\1|"(?:[^"\\]|\\.)*"|'(?:\\.[^']*|[^\\'\n])'|//[^\n]*|/\*.*?\*/''', re.S)
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


def strip_comments(text):
    """Source with comments blanked and line numbers kept: a comment prints nothing."""
    def keep(m):
        t = m.group(0)
        return "\n" * t.count("\n") if t.startswith(("//", "/*")) else t
    return SOURCE_TOKEN.sub(keep, text)


def calls(text):
    """(offset, argument text) of each writer call; a definition is not one."""
    for m in CALL.finditer(text):
        i, depth, n = m.end(), 1, len(text)
        while i < n and depth:
            c = text[i]
            if c == '"':
                i += 1
                while i < n and text[i] != '"':
                    i += 2 if text[i] == "\\" else 1
            elif c == "'":
                ch = CHAR.match(text, i)
                if ch:
                    i = ch.end()
                    continue
            elif c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            i += 1
        after = text[i:i + 80].lstrip()
        if depth == 0 and not after.startswith(("{", "->")):
            yield m.start(), text[m.end():i - 1].strip()


def templates(text):
    """Each line a function writes in pieces: [(part, line), ...], a part being text or VAR."""
    out, cur, last, line, at = [], [], 0, 1, 0
    def flush():
        nonlocal cur
        if len(cur) > 1:
            out.append(cur)
        cur = []
    for start, arg in calls(text):
        line += text.count("\n", at, start)
        at = start
        if cur and line - last > MAX_GAP_LINES:
            flush()
        last = line
        lit = LITERAL.match(arg)
        if lit is None:
            cur.append((VAR, line))
            continue
        body = unescape(lit.group(1))
        cur.append((body, line))
        if body.endswith("\n"):
            flush()
    flush()
    return out


# Word kinds of a needle: expect's -ex, -re, and its default of a glob.
EXACT, REGEX, GLOB = "ex", "re", "gl"
FLAG = re.compile(r"(?<![\w-])-(ex|exact|re)\s")
PLAIN_EXPECT = re.compile(r"(?m)^\s*expect\s+(?=\")")
PATTERN_ACTION = re.compile(r'(?m)^\s*(?="(?:[^"\\\n]|\\.)*"\s*\{)')
PROC = re.compile(r"(?m)^\s*proc\s+(\w+)\s")
SET = re.compile(r"(?m)^\s*set\s+(\w+)\s")
TCL_COMMENT = re.compile(r"(?m)^[ \t]*#.*$")
QUANTIFIER = re.compile(r"\{\d+(?:,\d*)?\}")
SEED_WRAPPERS = {"lc_expect": [(0, EXACT)], "lc_expect_re": [(0, REGEX)],
                 "lc_run_expect": [(1, EXACT)], "lc_run_expect_hardfail": [(1, EXACT)]}


def tcl_word(text, i):
    """The Tcl word at or after i, with its delimiters: (word, end); (None, i) at a line's end."""
    n = len(text)
    while i < n and text[i] in " \t":
        i += 1
    if i >= n or text[i] == "\n":
        return None, i
    start = i
    if text[i] == '"':
        i += 1
        while i < n and text[i] != '"':
            i += 2 if text[i] == "\\" else 1
        return text[start:i + 1], i + 1
    if text[i] == "{":
        depth = 0
        while i < n:
            if text[i] == "\\":
                i += 2
                continue
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
            if depth == 0:
                break
        return text[start:i], i
    while i < n and text[i] not in " \t\n":
        i += 1
    return text[start:i], i


def tcl_words(text, i, count):
    words = []
    for _ in range(count):
        w, i = tcl_word(text, i)
        if w is None:
            break
        words.append(w)
    return words


def variable_name(word):
    m = re.fullmatch(r'"?\$\{?(\w+)\}?"?', word)
    return m.group(1) if m else None


def find_wrappers(text, known):
    """Each proc's arguments used as needles: {name: [(index, kind), ...]}, to a fixpoint."""
    found = {k: list(v) for k, v in known.items()}
    procs = []
    for m in PROC.finditer(text):
        args, i = tcl_word(text, m.end())
        body, _ = tcl_word(text, i)
        if args and body and args.startswith("{") and body.startswith("{"):
            names = [a.strip("{}").split()[0] for a in re.findall(r"\{[^{}]*\}|\S+", args[1:-1])]
            procs.append((m.group(1), names, body))
    for _ in range(len(procs) + 1):
        changed = False
        for name, names, body in procs:
            uses = set(found.get(name, []))
            for m in FLAG.finditer(body):
                w, _ = tcl_word(body, m.end())
                v = variable_name(w or "")
                if v in names:
                    uses.add((names.index(v), REGEX if m.group(1) == "re" else EXACT))
            for wrapper, slots in found.items():
                for m in re.finditer(r"(?<![\w$:-])" + wrapper + r"\s", body):
                    words = tcl_words(body, m.end() - 1, 8)
                    for index, kind in slots:
                        v = variable_name(words[index]) if index < len(words) else None
                        if v in names:
                            uses.add((names.index(v), kind))
            if uses != set(found.get(name, [])):
                found[name] = sorted(uses)
                changed = True
        if not changed:
            break
    return found


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
        elif c == "{":
            q = QUANTIFIER.match(rx, i)
            if q:
                if cur:
                    cur[-1] = WILD
                i = q.end()
                continue
        elif c in "*+?}^$":
            pass
        else:
            cur.append(c)
        i += 1
    alts.append("".join(cur))
    return [re.sub(WILD + "+", WILD, a).replace("\r", "").replace("\n", "") for a in alts]


def glob_tokens(arg):
    return re.sub(WILD + "+", WILD, re.sub(r"\[[^\]]*\]|[*?]", WILD, arg))


def needle_tokens(arg, kind):
    if arg.startswith('"'):
        arg = TCL_SUBST.sub(WILD, arg)  # the value is variable text here
    if kind == REGEX:
        return regex_alternatives(arg[1:-1])
    lit = tcl_literal(arg)
    return [glob_tokens(lit) if kind == GLOB else lit]


def needles(text, wrappers=SEED_WRAPPERS):
    """(line, tokens) of each needle given literally, plus how many are not read."""
    text = TCL_COMMENT.sub("", text)
    values = {}
    for m in SET.finditer(text):
        w, _ = tcl_word(text, m.end() - 1)
        if w and w[0] in '"{':
            values.setdefault(m.group(1), []).append(w)
    # A proc's own argument is read at its callers.
    params = set()
    for m in PROC.finditer(text):
        args, _ = tcl_word(text, m.end())
        if args and args.startswith("{"):
            params.update(a.strip("{}").split()[0] for a in re.findall(r"\{[^{}]*\}|\S+", args[1:-1]) if a.strip("{}"))
    found, unread = [], [0]
    def take(offset, word, kind):
        if word is None:
            return
        ln = text.count("\n", 0, offset) + 1
        v = variable_name(word)
        if v is not None:
            if v not in values and v not in params:
                unread[0] += 1
            words = values.get(v, [])
        else:
            words = [word] if word[0] in '"{' else []
        for w in words:
            found.extend((ln, t) for t in needle_tokens(w, kind) if t.strip(WILD + " "))
    for m in FLAG.finditer(text):
        w, _ = tcl_word(text, m.end())
        take(m.start(), w, REGEX if m.group(1) == "re" else EXACT)
    for form in (PLAIN_EXPECT, PATTERN_ACTION):
        for m in form.finditer(text):
            w, _ = tcl_word(text, m.end())
            take(m.start(), w, GLOB)
    proc_heads = {m.start(1) for m in PROC.finditer(text)}
    for name, slots in wrappers.items():
        for m in re.finditer(r"(?<![\w$:-])" + name + r"\s", text):
            if m.start() in proc_heads:
                continue
            words = tcl_words(text, m.end() - 1, 8)
            for index, kind in slots:
                if index < len(words):
                    take(m.start(), words[index], kind)
    found.sort()
    return found, unread[0]


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
            text = strip_comments(fh.read())
        literals.extend(unescape(m.group(1)) for m in STRING.finditer(text))
        if "put" in text:
            for t in templates(text):
                tpl.append((os.path.relpath(path, root), t, "".join(p for p, _ in t if p is not VAR)))
    whole = "\n".join(lit.replace("\n", " ") for lit in literals)
    ndl, unread = [], 0
    scenarios = []
    for path in scenario_files(root):
        with open(path, errors="replace") as fh:
            scenarios.append((os.path.basename(path), fh.read()))
    library = find_wrappers(next((t for n, t in scenarios if n == "lib.exp"), ""), SEED_WRAPPERS)
    for name, text in scenarios:
        found, missed = needles(text, find_wrappers(text, library))
        ndl.extend((name, ln, n) for ln, n in found)
        unread += missed
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
    return len(tpl), len(ndl), unread, hits


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
    forms = """# -ex "commented out"
lc_expect "a b" x
expect {
 -re {c [0-9]+ d} { }
 -ex "e" { }
 -re {q[a-z]{1,9}r} { }
 "glob*tail" { }
}
lc_expect "f $g h" y
expect "plain"
set kept "from a variable"
expect { -ex $kept { } }
proc step {needle why} { expect { -ex $needle { lc_step $why } } }
proc outer {why n} { step $n $why }
step "through a proc" w
outer w {through two procs}
foreach v {not read} { expect -ex $v }
"""
    found, missed = needles(forms, find_wrappers(forms, SEED_WRAPPERS))
    want = ["a b", "c " + WILD + " d", "e", "q" + WILD + "r", "glob" + WILD + "tail", "f " + WILD + " h", "plain",
            "from a variable", "through a proc", "through two procs"]
    if [n for _, n in found] != want:
        print(f"self-test FAIL: needle forms {[n for _, n in found]!r}")
        bad += 1
    if missed != 1:  # the foreach
        print(f"self-test FAIL: unread needles {missed}")
        bad += 1
    masked = strip_comments('// the "tag: PASS x done now" line\n/* t_putstr("gone"); */\nchar c = \'"\'; t_putstr("kept // not a comment");\n')
    if "tag: PASS" in masked or "gone" in masked or "kept // not a comment" not in masked:
        print(f"self-test FAIL: comments {masked!r}")
        bad += 1
    defined = [a for _, a in calls('fn put_hex64(v: u64) { helper(v); }\nvoid put_dec(u64 v) {\nt_putstr("a)b");\nif x { t_putstr("tail") }\n')]
    if defined != ['"a)b"', '"tail"']:
        print(f"self-test FAIL: a definition taken for a call {defined!r}")
        bad += 1
    best = crossing("tag: PASS x done", tpl[0]), crossing("tag: PASS x done", ["tag: PASS ", VAR, " never\n"])
    if not best[0] > best[1] > 0:
        print(f"self-test FAIL: the fitting line must outscore one that only starts alike: {best}")
        bad += 1
    print("self-test:", "FAIL" if bad else f"PASS ({len(cases) + 7} controls)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    n_tpl, n_ndl, unread, hits = check(args.root)
    if n_tpl == 0 or n_ndl == 0:
        print(f"check-console-lines: FAIL: found {n_tpl} lines and {n_ndl} needles, so it checked nothing")
        return 2
    for path, line, shown, scen, ln, n in hits:
        print(f"{path}:{line}: {shown[:70]!r}\n    read whole by {scen}:{ln} {n.replace(WILD, '<any>')[:80]!r}")
    print(f"check-console-lines: {n_tpl} lines written in pieces, {n_ndl} needles, {len(hits)} spanning; {unread} variable needles not read")
    if hits:
        print("Write each such line in one console call; two calls can have another process's line between them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
