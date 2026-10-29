#!/usr/bin/env python3
"""Kernel test registry gate.

A kernel test runs only if kernel/test/test.c's g_tests names it. A test
function defined, declared and compiled but never added there passes every
build and every suite, because nothing counts what did not run:
test_devcap_clearance_audio_graph -- CAP_AUDIO_GRAPH's clearance proof --
ran zero times from N-3a-1 (79076bb4e, 2026-09-06) until a census found it
(OPEN-BUGS 2026-10-27T07:48Z).

  A. Every non-static `void test_*(void)` defined under kernel/test/ is a
     g_tests entry (RUNNERS excepted).
  B. Every test_registry.h declaration is a g_tests entry.
  C. No g_tests function or name appears twice.
  D. Every g_tests function has a definition this parser found -- so a
     definition written in a shape the pattern misses fails here instead of
     escaping A.

--self-test plants each fault and its control.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "kernel/test"
RUNNERS = {"test_run_all"}
DEF = re.compile(r"^(static\s+)?void\s+(test_\w+)\s*\(\s*void\s*\)\s*\{", re.M)
DECL = re.compile(r"^\s*void\s+(test_\w+)\s*\(\s*void\s*\)\s*;", re.M)
ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,\s*(test_\w+)\s*,')


def problems(sources, registry, runner):
    out = []
    defined = {}
    for text in sources:
        for m in DEF.finditer(text):
            defined[m.group(2)] = bool(m.group(1))
    entries = ENTRY.findall(runner)
    fns = [f for _, f in entries]
    names = [n for n, _ in entries]
    listed = set(fns)
    for f in sorted(f for f, st in defined.items() if not st and f not in listed | RUNNERS):
        out.append(f"{f} is defined but g_tests never runs it")
    for f in sorted(set(DECL.findall(registry)) - listed):
        out.append(f"{f} is declared in test_registry.h but g_tests never runs it")
    for f in sorted({f for f in fns if fns.count(f) > 1}):
        out.append(f"g_tests runs {f} twice")
    for n in sorted({n for n in names if names.count(n) > 1}):
        out.append(f"g_tests names \"{n}\" twice")
    for f in sorted(listed - set(defined)):
        out.append(f"g_tests runs {f}, but no definition of it parses here")
    return out, len(entries)


def self_test():
    def run(*fns):
        return "static const struct T g_tests[] = {\n" + "".join(
            f'    {{ "{n}", {f}, false, NULL }},\n' for n, f in fns) + "};\n"
    src = "void test_a(void) {\n}\nstatic void test_h(void) {\n}\nvoid test_run_all(void) {\n}\n"
    reg = "void test_a(void);\n"
    cases = [
        ("control", [src], reg, run(("x.a", "test_a")), 0),
        ("defined, not run", [src + "void test_b(void) {\n}\n"], reg, run(("x.a", "test_a")), 1),
        ("declared, not run", [src], reg + "void test_c(void);\n", run(("x.a", "test_a")), 1),
        ("run twice", [src], reg, run(("x.a", "test_a"), ("x.a2", "test_a")), 1),
        ("named twice", [src + "void test_b(void) {\n}\n"], reg,
         run(("x.a", "test_a"), ("x.a", "test_b")), 1),
        ("run, never parsed", [src], reg, run(("x.a", "test_a"), ("x.z", "test_z")), 1),
        ("brace on its own line", ["void test_a(void)\n{\n}\n"], reg, run(("x.a", "test_a")), 0),
    ]
    bad = 0
    for name, sources, registry, runner, want in cases:
        got = len(problems(sources, registry, runner)[0])
        if got != want:
            print(f"  self-test {name}: want {want} problem(s), got {got}")
            bad += 1
    print(f"self-test: {len(cases) - bad}/{len(cases)}")
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    sources = [p.read_text(encoding="utf-8") for p in sorted(TESTS.glob("*.c"))]
    registry = (TESTS / "test_registry.h").read_text(encoding="utf-8")
    runner = (TESTS / "test.c").read_text(encoding="utf-8")
    found, n = problems(sources, registry, runner)
    # Guard the guard: a g_tests that parses as near-empty would pass B and C
    # vacuously and turn A into noise.
    if n < 1000:
        print(f"FAIL: g_tests parsed as {n} entries -- the entry format no longer "
              f"matches, so these checks would pass vacuously")
        return 1
    if found:
        print("FAIL: the kernel test registry is incomplete")
        for p in found:
            print(f"  - {p}")
        return 1
    print(f"test registry OK: {n} g_tests entries, every defined test runs")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
