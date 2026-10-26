#!/usr/bin/env python3
"""check-decls.py: one name, one type, across every kernel translation unit.

C compares a declaration with its definition only inside one translation unit.
A file that declares another file's function for itself is never compared with
the definition, and a call through a declaration of the wrong type is undefined
behaviour no compiler diagnostic sees. This parses every kernel TU with the
build's own flags (clang -ast-dump) and fails when any non-static function or
object is declared with two different types anywhere in the kernel.

Types are compared after typedefs are expanded, `_Bool` is read as `bool`, and
array bounds are dropped (an `extern T a[]` declares the same object as its
sized definition).

  tools/check-decls.py [--build-dir build/kernel] [--jobs N]
  tools/check-decls.py --self-test
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile

TOP_DECL = re.compile(r"^[|`]-(FunctionDecl|VarDecl) 0x[0-9a-f]+ (.*)$")
TYPEDEF = re.compile(r"TypedefDecl 0x[0-9a-f]+ .* ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'")
NAME_TYPE = re.compile(r" ([A-Za-z_][A-Za-z_0-9]*) '([^']*)'(?::'[^']*')?(.*)$")
IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")


def canonical(t, typedefs, depth=0):
    if depth > 8:
        return t
    def expand(m):
        tag = re.search(r"\b(struct|union|enum)\s+$", t[:m.start()])
        return typedefs[m.group(0)] if m.group(0) in typedefs and not tag else m.group(0)
    out = IDENT.sub(expand, t)
    if out != t:
        return canonical(out, typedefs, depth + 1)
    out = re.sub(r"\b_Bool\b", "bool", out)
    out = re.sub(r"\[\d+\]", "[]", out)
    return re.sub(r"\s+", " ", out).strip()


def decls_of(tu, cmd):
    """Every non-static top-level function/object declaration in one TU."""
    r = subprocess.run(cmd + ["-fsyntax-only", "-w", "-Xclang", "-ast-dump", tu],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"{tu}: clang failed:\n{r.stderr[-2000:]}")
    typedefs, found = {}, []
    for line in r.stdout.split("\n"):
        m = TYPEDEF.search(line)
        if m and m.group(1) not in ("bool",):
            typedefs.setdefault(m.group(1), m.group(2))
            continue
        m = TOP_DECL.match(line)
        if not m:
            continue
        nt = NAME_TYPE.search(m.group(2))
        if not nt or re.search(r"\bstatic\b", nt.group(3)):
            continue
        found.append((nt.group(1), nt.group(2)))
    return [(n, canonical(t, typedefs), t) for n, t in found]


def check(tus, cmd, jobs):
    seen = collections.defaultdict(lambda: collections.defaultdict(list))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for tu, decls in zip(tus, ex.map(lambda t: decls_of(t, cmd), tus)):
            for name, canon, spelled in decls:
                seen[name][canon].append((tu, spelled))
    return {n: v for n, v in seen.items() if len(v) > 1}, len(seen)


def build_inputs(build_dir):
    d = os.path.join(build_dir, "kernel", "CMakeFiles", "thylacine.elf.dir")
    flags = {}
    for line in open(os.path.join(d, "flags.make")):
        k, sep, v = line.partition(" = ")
        if sep:
            flags[k.strip()] = v.strip()
    srcs = sorted(set(re.findall(r"(\S+\.c)$", open(os.path.join(d, "build.make")).read(), re.M)))
    srcs = [s for s in srcs if os.path.isabs(s) and "/kernel/" in s]
    if not srcs:
        sys.exit(f"check-decls: no kernel sources found in {d}/build.make")
    clang = re.search(r"^# compile C with (\S+)", open(os.path.join(d, "flags.make")).read(), re.M).group(1)
    test_dir = srcs[0].split("/kernel/")[0] + "/kernel/test"
    cmd = [clang] + shlex.split(flags.get("C_DEFINES", "")) + ["-I" + test_dir] + \
        shlex.split(flags.get("C_INCLUDES", "")) + shlex.split(flags.get("C_FLAGS", ""))
    return srcs, cmd


def report(conflicts):
    for name in sorted(conflicts):
        print(f"check-decls: {name} has {len(conflicts[name])} types:")
        for canon, sites in sorted(conflicts[name].items()):
            tus = sorted({"kernel/" + t.split("/kernel/", 1)[1] if "/kernel/" in t else t for t, _ in sites})
            print(f"    {sites[0][1]}    in {', '.join(tus[:4])}{' ...' if len(tus) > 4 else ''}")


def self_test():
    """The detector must go red on a seeded mismatch and stay green on its absence."""
    cases = {
        "mismatch": ("long f(int x) { return x; }\n", "extern long f(unsigned x);\nlong g(void) { return f(1); }\n", {"f"}),
        "clean": ("long f(int x) { return x; }\n", "extern long f(int x);\nlong g(void) { return f(1); }\n", set()),
        "typedef": ("long f(int x) { return x; }\n", "typedef int myint;\nextern long f(myint x);\nlong g(void) { return f(1); }\n", set()),
        "array": ("int a[4];\n", "extern int a[];\nint g(void) { return a[0]; }\n", set()),
        "object": ("long v;\n", "extern int v;\nint g(void) { return v; }\n", {"v"}),
        "static": ("static int h(void) { return 0; }\nint k(void) { return h(); }\n",
                   "static long h(void) { return 0; }\nlong m(void) { return h(); }\n", set()),
    }
    clang = os.environ.get("CC", "clang")
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for case, (a, b, want) in cases.items():
            pa, pb = os.path.join(tmp, f"{case}_a.c"), os.path.join(tmp, f"{case}_b.c")
            open(pa, "w").write(a)
            open(pb, "w").write(b)
            got, _ = check([pa, pb], [clang, "-std=c99"], 2)
            ok = set(got) == want
            bad += not ok
            print(f"check-decls self-test {case}: {'ok' if ok else 'WRONG'} (reported {sorted(got) or 'nothing'})")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="build/kernel")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(1 if self_test() else 0)
    srcs, cmd = build_inputs(a.build_dir)
    conflicts, names = check(srcs, cmd, a.jobs)
    if conflicts:
        report(conflicts)
        print(f"check-decls: FAIL -- {len(conflicts)} of {names} names declared with more than one type across {len(srcs)} TUs")
        sys.exit(1)
    print(f"check-decls: ok -- {names} non-static names, one type each, across {len(srcs)} TUs")


if __name__ == "__main__":
    main()
