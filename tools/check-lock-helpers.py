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

  kernel/9p_client.c   every spin_* call    only in client_lock / client_unlock
  kernel/dev9p_poll.c  g_dev9p_poll_lock    only in dpl_lock / dpl_unlock
  kernel/proc.c        g_proc_table_lock    only in proc_table_lock_acquire /
                                            proc_table_lock_release

spin_lock_init is exempt. Each helper must itself hold a raw call, so a renamed
helper fails here instead of leaving the rule vacuous.

--self-test plants a raw release in each file, and renames each helper.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (file, pattern of a raw call this rule owns, the helpers that may make it)
RULES = [
    ("kernel/9p_client.c",
     re.compile(r"\bspin_(?:lock|unlock|trylock|lock_irqsave|unlock_irqrestore)\s*\("),
     {"client_lock", "client_unlock"}),
    ("kernel/dev9p_poll.c",
     re.compile(r"\bspin_(?:lock|unlock|trylock|lock_irqsave|unlock_irqrestore)\s*\(\s*&g_dev9p_poll_lock\b"),
     {"dpl_lock", "dpl_unlock"}),
    ("kernel/proc.c",
     re.compile(r"\bspin_(?:lock|unlock|trylock|lock_irqsave|unlock_irqrestore)\s*\(\s*&g_proc_table_lock\b"),
     {"proc_table_lock_acquire", "proc_table_lock_release"}),
]

# A function definition's first line: column 0, a name, an open paren, and no
# trailing semicolon. Good enough for this tree's style, and the helper-presence
# check below fails loudly if it ever stops finding the helpers.
FUNC = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;]*$")


def problems(path, text, pattern, helpers):
    out = []
    current = None
    seen = {h: 0 for h in helpers}
    for n, line in enumerate(text.split("\n"), 1):
        m = FUNC.match(line)
        if m and not line.startswith(("#", "//")):
            current = m.group(1)
        code = line.split("//", 1)[0]
        if not pattern.search(code):
            continue
        if current in helpers:
            seen[current] += 1
        else:
            out.append(f"{path}:{n}: raw lock call outside {sorted(helpers)} "
                       f"(in {current}): {line.strip()}")
    for h, k in sorted(seen.items()):
        if k == 0:
            out.append(f"{path}: helper {h} holds no raw call -- renamed or moved? "
                       "the rule would be vacuous")
    return out


def check(sources):
    out = []
    for path, pattern, helpers in RULES:
        out += problems(path, sources[path], pattern, helpers)
    return out


def load():
    return {path: (ROOT / path).read_text() for path, _, _ in RULES}


def self_test():
    real = load()
    fails = []
    if check(real):
        fails.append("the tree itself fails; fix it before trusting the self-test")
    for path, pattern, helpers in RULES:
        lock = {"kernel/9p_client.c": "c->lock",
                "kernel/dev9p_poll.c": "g_dev9p_poll_lock",
                "kernel/proc.c": "g_proc_table_lock"}[path]
        planted = dict(real)
        planted[path] = real[path] + (
            "\nint planted_raw_release(void) {\n"
            f"    spin_unlock(&{lock});\n"
            "    return 0;\n}\n")
        if not any("planted_raw_release" in p for p in check(planted)):
            fails.append(f"{path}: a raw release in another function was not flagged")
        commented = dict(real)
        commented[path] = real[path] + (
            "\nint planted_comment(void) {\n"
            f"    return 0;  // spin_unlock(&{lock}); is only prose here\n}}\n")
        if check(commented):
            fails.append(f"{path}: a raw call inside a comment was flagged")
        for h in sorted(helpers):
            renamed = dict(real)
            renamed[path] = re.sub(rf"\b{h}\b", h + "_renamed", real[path])
            if not any(f"helper {h} holds no raw call" in p for p in check(renamed)):
                fails.append(f"{path}: renaming {h} was not flagged")
    for f in fails:
        print("check-lock-helpers self-test FAILED:", f, file=sys.stderr)
    if not fails:
        print("check-lock-helpers self-test OK: every planted fault flagged, the control clean")
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
