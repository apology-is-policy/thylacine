#!/usr/bin/env python3
"""Deferring-lock helper gate.

Three kernel locks defer or owe hook walks (specs/poll_defer.tla,
specs/poll_owed.tla): a walk reached while one is held is recorded, and the
helper's release runs it holding nothing. That works only if EVERY acquire and
release goes through the helper. A raw spin_unlock after the helper's acquire
compiles, passes every test that does not reach it, and leaves the thread's
defer depth raised, so its later walks wait forever: lost wakeups. The
corona join (2026-10-29) merged fourteen such raw sites into kernel/9p_client.c
and four into kernel/proc.c without a textual conflict.

  A. kernel/proc.c, kernel/dev9p_poll.c: outside the helpers
     (proc_table_lock_acquire/_release, dpl_lock/dpl_unlock) the lock's name
     appears only in its definition, an `__atomic_load_n(&L.value, ...)` read,
     and -- for a lock defined without an initializer -- ONE spin_lock_init.
     Any other mention fails: a spin_* call of any variant or shape, its address
     taken into a pointer, a store to `.value`, a spin_lock_init of a statically
     initialised lock (each of those writes the lock word, i.e. releases it).
  B. kernel/9p_client.c: outside client_lock/client_unlock no spin_* call but
     spin_lock_init, and no mention of `NAME->lock` -- in any spelling,
     parenthesised or not -- for a NAME the file declares `struct p9_client *`,
     but spin_lock_init's argument or an `__atomic_load_n(&NAME->lock.value`.
  C. Every other production file under kernel/ and arch/, headers included
     (not kernel/test/, whose fixtures inspect a client under a balanced raw
     pair): the same `NAME->lock` rule, for the names that file declares
     `struct p9_client *` AND, reached through `->` or `.`, every member a
     header declares `struct p9_client *` (`priv->client->lock`). The names are
     derived from the declarations, so a new one is covered without an edit.
  D. Each helper holds a raw call of its own lock, so a renamed helper fails
     instead of leaving A and B vacuous.

Comments and string literals are blanked before matching, offsets kept, and
calls are matched across lines.

--self-test plants each shape the gate must catch (single-line and multi-line
raw releases, an alias pointer, a _raw variant, a store to `.value`, a re-init,
a raw client lock in another file, through a header member, parenthesised, in a
header inline, a raw call just past a helper's closing brace, a renamed helper)
and the controls it must pass (the same text in a comment and in a string, a
`.value` read, another struct's own lock).
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

NAMED = [  # rule A: (file, lock, helpers)
    ("kernel/proc.c", "g_proc_table_lock",
     {"proc_table_lock_acquire", "proc_table_lock_release"}),
    ("kernel/dev9p_poll.c", "g_dev9p_poll_lock", {"dpl_lock", "dpl_unlock"}),
]
CLIENT = ("kernel/9p_client.c", {"client_lock", "client_unlock"})  # rule B

SPIN_CALL = re.compile(r"\bspin_(?!lock_init\b)\w+\s*\(")
# What may sit right before a client's `NAME->lock` that the gate allows:
# spin_lock_init's argument, or an __atomic_load_n of its `.value` (a read).
INIT_ARG = re.compile(r"\bspin_lock_init\s*\(\s*&?\s*\(?\s*$")
LOAD_ARG = re.compile(r"\b__atomic_load_n\s*\(\s*&\s*\(?\s*$")
VALUE_AFTER = re.compile(r"\s*\.\s*value\b")
CLIENT_DECL = re.compile(r"\bstruct\s+p9_client\s*\*\s*(?:const\s+)?(\w+)")
# A member (or extern) a header declares: reachable from any file.
CLIENT_MEMBER = re.compile(
    r"\bstruct\s+p9_client\s*\*\s*(?:const\s+)?(\w+)\s*(?:\[[^\]]*\])?\s*;")
# A function definition's first line: column 0, a name, an open paren, and no
# trailing semicolon. Rule D fails loudly if it ever stops finding the helpers.
FUNC = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;]*$")


def blank(text):
    """Comments and string/char literals to spaces; newlines and offsets kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + (text[j - 1] if j - i >= 2 else ""))
            i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def owners(code):
    """For each line, the function whose body it is in (by the FUNC heuristic)."""
    cur, res = None, []
    for line in code.split("\n"):
        m = FUNC.match(line)
        if m and not line.startswith("#"):
            cur = m.group(1)
        res.append(cur)
        if line.startswith("}"):
            cur = None    # a body's closing brace: what follows is no helper's
    return res


def line_of(code, off):
    return code.count("\n", 0, off)


def rule_named(path, text, lock, helpers):
    code = blank(text)
    own = owners(code)
    lines = text.split("\n")
    out, seen = [], {h: 0 for h in helpers}
    defn = re.search(rf"\bstatic\s+spin_lock_t\s+{lock}\b(\s*=)?", code)
    ok_spans = [defn.span()] if defn else []
    ok_spans += [m.span() for m in re.finditer(
        rf"\b__atomic_load_n\s*\(\s*&\s*{lock}\s*\.\s*value\b", code)]
    inits = list(re.finditer(rf"\bspin_lock_init\s*\(\s*&\s*{lock}\b", code))
    if defn and not defn.group(1) and len(inits) == 1:
        ok_spans.append(inits[0].span())   # its one boot init; any other writes the word
    for m in re.finditer(rf"\b{lock}\b", code):
        if any(a <= m.start() < b for a, b in ok_spans):
            continue
        ln = line_of(code, m.start())
        f = own[ln]
        if f in helpers:
            seen[f] += 1
            continue
        out.append(f"{path}:{ln + 1}: {lock} named outside {sorted(helpers)} "
                   f"(in {f}): {lines[ln].strip()}")
    for h, k in sorted(seen.items()):
        if k == 0:
            out.append(f"{path}: helper {h} holds no raw call of {lock} -- renamed or moved? "
                       "the rule would be vacuous")
    return out


def client_lock_uses(code, names, members=()):
    """Each `NAME->lock` of a client-typed NAME (a member only after -> or .,
    or any expression cast to one), minus spin_lock_init's argument and an
    __atomic_load_n of its .value."""
    pats = [rf"\b{n}\s*->\s*lock\b" for n in sorted(names)]
    pats += [rf"(?:->|\.)\s*{n}\s*->\s*lock\b" for n in sorted(members)]
    pats.append(r"\(\s*(?:const\s+)?struct\s+p9_client\s*\*\s*\)\s*\(?\s*\w+\s*\)\s*->\s*lock\b")
    for pat in pats:
        for m in re.finditer(pat, code):
            before = code[max(0, m.start() - 120):m.start()]
            if INIT_ARG.search(before):
                continue
            if LOAD_ARG.search(before) and VALUE_AFTER.match(code, m.end()):
                continue
            yield m


def rule_client(path, text, helpers):
    code = blank(text)
    own = owners(code)
    lines = text.split("\n")
    out, seen = [], {h: 0 for h in helpers}
    names = set(CLIENT_DECL.findall(code))
    hits = [(m, "spin_* call") for m in SPIN_CALL.finditer(code)]
    hits += [(m, "a client's ->lock") for m in client_lock_uses(code, names)]
    for m, what in hits:
        ln = line_of(code, m.start())
        f = own[ln]
        if f in helpers:
            if what == "spin_* call":
                seen[f] += 1
            continue
        out.append(f"{path}:{ln + 1}: {what} outside {sorted(helpers)} "
                   f"(in {f}): {lines[ln].strip()}")
    for h, k in sorted(seen.items()):
        if k == 0:
            out.append(f"{path}: helper {h} holds no spin_* call -- renamed or moved? "
                       "the rule would be vacuous")
    return out


def rule_client_elsewhere(path, text, members):
    code = blank(text)
    lines = text.split("\n")
    out = []
    for m in client_lock_uses(code, set(CLIENT_DECL.findall(code)), members):
        ln = line_of(code, m.start())
        out.append(f"{path}:{ln + 1}: a struct p9_client's lock taken outside "
                   f"kernel/9p_client.c's helpers: {lines[ln].strip()}")
    return out


def header_members(sources):
    members = set()
    for path, text in sources.items():
        if path.endswith(".h"):
            members |= set(CLIENT_MEMBER.findall(blank(text)))
    return members


def elsewhere_paths():
    paths = []
    for top in ("kernel", "arch"):
        for p in sorted([*(ROOT / top).rglob("*.c"), *(ROOT / top).rglob("*.h")]):
            rel = p.relative_to(ROOT).as_posix()
            if rel.startswith("kernel/test/") or rel == CLIENT[0]:
                continue
            paths.append(rel)
    return paths


def check(sources):
    out = []
    for path, lock, helpers in NAMED:
        out += rule_named(path, sources[path], lock, helpers)
    out += rule_client(CLIENT[0], sources[CLIENT[0]], CLIENT[1])
    members = header_members(sources)
    for path, text in sorted(sources.items()):
        if path in (CLIENT[0],) or path.startswith("kernel/test/"):
            continue
        out += rule_client_elsewhere(path, text, members)
    return out


def load():
    srcs = {path: (ROOT / path).read_text() for path, _, _ in NAMED}
    srcs[CLIENT[0]] = (ROOT / CLIENT[0]).read_text()
    for rel in elsewhere_paths():
        srcs.setdefault(rel, (ROOT / rel).read_text(errors="replace"))
    return srcs


def self_test():
    real = load()
    fails = []
    if check(real):
        fails.append("the tree itself fails; fix it before trusting the self-test")
    # A planted header joins the sources whatever load() reads, so the header
    # plant below proves the rule, not that the gate ever reads a header.
    if "kernel/include/thylacine/9p_client.h" not in real:
        fails.append("load() reads no kernel header: a header inline would go unchecked")

    def must_flag(label, path, extra, needle):
        planted = dict(real)
        planted[path] = real.get(path, "") + extra
        if not any(needle in p for p in check(planted)):
            fails.append(f"{label}: not flagged")

    def must_pass(label, path, extra):
        planted = dict(real)
        planted[path] = real.get(path, "") + extra
        got = check(planted)
        if got:
            fails.append(f"{label}: flagged: {got[0]}")

    for path, lock, helpers in NAMED:
        must_flag(f"{path} single-line raw release", path,
                  f"\nint planted_a(void) {{\n    spin_unlock(&{lock});\n    return 0;\n}}\n", "planted_a")
        must_flag(f"{path} multi-line raw release", path,
                  f"\nint planted_b(void) {{\n    spin_unlock_irqrestore(\n        &{lock}, 0);\n    return 0;\n}}\n",
                  "planted_b")
        must_flag(f"{path} alias pointer", path,
                  f"\nint planted_c(void) {{\n    spin_lock_t *l = &{lock};\n    spin_unlock(l);\n    return 0;\n}}\n",
                  "planted_c")
        must_flag(f"{path} _raw variant", path,
                  f"\nint planted_d(void) {{\n    spin_unlock_raw(&{lock});\n    return 0;\n}}\n", "planted_d")
        must_pass(f"{path} comment control", path,
                  f"\nint planted_e(void) {{\n    return 0;  // spin_unlock(&{lock}); prose\n}}\n"
                  f"/* spin_lock(&{lock}); */\n")
        must_pass(f"{path} string control", path,
                  f"\nconst char *planted_f(void) {{\n    return \"spin_unlock(&{lock})\";\n}}\n")
        for h in sorted(helpers):
            renamed = dict(real)
            renamed[path] = re.sub(rf"\b{h}\b", h + "_renamed", real[path])
            if not any(f"helper {h} holds no raw call" in p for p in check(renamed)):
                fails.append(f"{path}: renaming {h} was not flagged")
            # A raw call just past the helper's closing brace is not the helper's.
            m = re.search(rf"(?m)^[A-Za-z_][\w \t\*]*\b{h}\s*\([^;]*$", real[path])
            end = real[path].find("\n}\n", m.end()) + 3 if m else -1
            if end > 2:
                after = dict(real)
                after[path] = (real[path][:end] + f"#define PLANTED_AFTER_{h} spin_unlock(&{lock})\n"
                               + real[path][end:])
                if not any(f"PLANTED_AFTER_{h}" in p for p in check(after)):
                    fails.append(f"{path}: a raw call past {h}'s closing brace was not flagged")
            else:
                fails.append(f"{path}: could not find {h}'s body to plant after it")

    cpath, chelpers = CLIENT
    must_flag("9p_client multi-line raw release", cpath,
              "\nint planted_g(struct p9_client *c) {\n    spin_unlock(\n        &c->lock);\n    return 0;\n}\n",
              "planted_g")
    must_flag("9p_client _raw variant", cpath,
              "\nint planted_h(struct p9_client *c) {\n    spin_unlock_raw(&c->lock);\n    return 0;\n}\n",
              "planted_h")
    must_flag("9p_client alias pointer", cpath,
              "\nint planted_i(struct p9_client *c) {\n    spin_lock_t *l = &c->lock;\n    (void)l;\n    return 0;\n}\n",
              "planted_i")
    must_pass("9p_client comment control", cpath,
              "\nint planted_j(void) {\n    return 0;  // spin_unlock(&c->lock);\n}\n")
    for h in sorted(chelpers):
        renamed = dict(real)
        renamed[cpath] = re.sub(rf"\b{h}\b", h + "_renamed", real[cpath])
        if not any(f"helper {h} holds no spin_* call" in p for p in check(renamed)):
            fails.append(f"{cpath}: renaming {h} was not flagged")
    must_flag("raw client lock in another file", "kernel/dev9p.c",
              "\nstatic void planted_k(struct p9_client *cl) {\n    spin_lock(&cl->lock);\n}\n",
              "struct p9_client's lock")
    must_pass("another file's own lock control", "kernel/dev9p.c",
              "\nstruct planted_l { spin_lock_t lock; };\n"
              "static void planted_l(struct planted_l *o) {\n    spin_lock(&o->lock);\n}\n")

    for path, lock, helpers in NAMED:
        must_flag(f"{path} store to .value", path,
                  f"\nint planted_s(void) {{\n    __atomic_store_n(&{lock}.value, 0u, __ATOMIC_RELEASE);\n"
                  f"    return 0;\n}}\n", "planted_s")
        must_flag(f"{path} re-init", path,
                  f"\nvoid planted_t(void) {{\n    spin_lock_init(&{lock});\n}}\n", "planted_t")
        must_pass(f"{path} .value read control", path,
                  f"\nint planted_u(void) {{\n    return __atomic_load_n(&{lock}.value, __ATOMIC_RELAXED) != 0;\n}}\n")
    must_flag("9p_client parenthesised alias", cpath,
              "\nint planted_n(struct p9_client *c) {\n    spin_lock_t *l = &(c->lock);\n    (void)l;\n    return 0;\n}\n",
              "planted_n")
    must_flag("9p_client store to .value", cpath,
              "\nint planted_v(struct p9_client *c) {\n    __atomic_store_n(&c->lock.value, 0u, __ATOMIC_RELEASE);\n"
              "    return 0;\n}\n", "planted_v")
    must_pass("9p_client .value read control", cpath,
              "\nint planted_w(struct p9_client *c) {\n    return __atomic_load_n(&c->lock.value, __ATOMIC_RELAXED) != 0;\n}\n")
    must_flag("client lock through a header member", "kernel/dev9p.c",
              "\nstatic void planted_m(struct dev9p_priv *priv) {\n    spin_lock(&priv->client->lock);\n}\n",
              "struct p9_client's lock")
    must_flag("client lock through a member of a struct value", "kernel/dev9p.c",
              "\nstatic void planted_x(struct dev9p_priv p) {\n    spin_lock(&(p.client->lock));\n}\n",
              "struct p9_client's lock")
    must_flag("client lock through a cast", "kernel/dev9p.c",
              "\nstatic void planted_y(void *x) {\n    spin_lock(&((struct p9_client *)x)->lock);\n}\n",
              "struct p9_client's lock")
    must_flag("client lock in a header inline", "kernel/include/thylacine/9p_client.h",
              "\nstatic inline void planted_o(struct p9_client *c) {\n    spin_lock(&c->lock);\n}\n",
              "struct p9_client's lock")
    for f in fails:
        print("check-lock-helpers self-test FAILED:", f, file=sys.stderr)
    if not fails:
        print("check-lock-helpers self-test OK: every planted shape flagged, every control clean")
    return 1 if fails else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv:
        print("usage: check-lock-helpers.py [--self-test]", file=sys.stderr)
        return 2
    out = check(load())
    for p in out:
        print(p, file=sys.stderr)
    if out:
        return 1
    print("deferring-lock helpers OK: every acquire and release of the three locks goes through its helper")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
