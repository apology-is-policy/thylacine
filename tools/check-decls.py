#!/usr/bin/env python3
"""check-decls.py: every cross-file kernel name declared once, with one type.

C compares a declaration with its definition only inside one translation unit.
A file that declares another file's function for itself is never compared with
the definition, and a call through a declaration of the wrong type is undefined
behaviour no compiler diagnostic sees (.claude/rules/kernel-code.md, "Cross-file
declarations"). This parses every kernel TU with the build's own flags (clang
-ast-dump) and fails on either of:

  (a) a non-static function or object declared with two different types
      anywhere in the kernel;
  (b) a .c file declaring a non-static function or object that the same
      translation unit does not define, at file or block scope -- such a name
      belongs in a header;
  (c) a struct or union tag defined at file scope in more than one file -- two
      layouts kept equal by hand, which nothing compares.

Types are compared after typedefs are expanded, `_Bool` is read as `bool`, and
array bounds are dropped (an `extern T a[]` declares the same object as its
sized definition). For (b), a declaration's file is recovered the way the dump
prints it: a location names its file only when the file changes, so every
location token is replayed in print order, and the file is the one holding the
declaration's name -- its range may begin in a macro's header.

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
TOP_RECORD = re.compile(r"^[|`]-RecordDecl 0x[0-9a-f]+ .*? (struct|union) ([A-Za-z_][A-Za-z_0-9]*) definition$")
NESTED_DECL = re.compile(r"^[| ]+[|`]-(FunctionDecl|VarDecl) 0x[0-9a-f]+ (.*)$")
CHILD_BODY = re.compile(r"^[| ] [|`]-CompoundStmt ")
QUOTED = re.compile(r"'[^']*'|\"(?:[^\"\\]|\\.)*\"")
LOC = re.compile(r"(?<![A-Za-z0-9_])(?:(/[^\s:<>,]+|<scratch space>|<built-in>):\d+:\d+|line:\d+:\d+|col:\d+|<invalid sloc>)")
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
    """Every non-static top-level function/object declaration in one TU, and
    the names this TU's own .c file declares without defining them."""
    r = subprocess.run(cmd + ["-fsyntax-only", "-w", "-Xclang", "-ast-dump", tu],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"{tu}: clang failed:\n{r.stderr[-2000:]}")
    typedefs, found, defined, local, tags = {}, [], set(), set(), set()
    main, cur, pending = os.path.realpath(tu), None, None
    for line in r.stdout.split("\n"):
        if pending and not TOP_DECL.match(line) and CHILD_BODY.match(line):
            defined.add(pending)
        for tok in LOC.finditer(QUOTED.sub("''", line)):
            if tok.group(1):
                cur = tok.group(1)
        m = TYPEDEF.search(line)
        if m and m.group(1) not in ("bool",):
            typedefs.setdefault(m.group(1), m.group(2))
            continue
        m = TOP_RECORD.match(line)
        if m and cur:
            tags.add((m.group(1) + " " + m.group(2), os.path.realpath(cur) if cur.startswith("/") else cur))
        m = TOP_DECL.match(line)
        if not m:
            if line.startswith(("|-", "`-")):
                pending = None
            # A block-scope `extern` object or any block-scope function
            # declaration names an external entity: it is a declaration like a
            # top-level one, never a definition.
            n = NESTED_DECL.match(line)
            nt = n and NAME_TYPE.search(n.group(2))
            if nt and not re.search(r"\bimplicit\b", n.group(2)[:nt.start()]) and \
                    (n.group(1) == "FunctionDecl" or re.search(r"\bextern\b", nt.group(3))):
                found.append((nt.group(1), nt.group(2)))
                if cur and os.path.realpath(cur) == main:
                    local.add(nt.group(1))
            continue
        pending = None
        nt = NAME_TYPE.search(m.group(2))
        # `implicit`: a builtin clang declares at its first use, written by no one.
        if not nt or re.search(r"\bstatic\b", nt.group(3)) or \
                re.search(r"\bimplicit\b", m.group(2)[:nt.start()]):
            continue
        name, attrs = nt.group(1), nt.group(3)
        found.append((name, nt.group(2)))
        if m.group(1) == "FunctionDecl":
            pending = name
        elif not re.search(r"\bextern\b", attrs):
            defined.add(name)
        # The line's last location is the declaration's NAME; its range can begin
        # inside a macro's header (`bool` is stdbool.h's), the name cannot.
        if cur and os.path.realpath(cur) == main:
            local.add(name)
    return [(n, canonical(t, typedefs), t) for n, t in found], local - defined, tags


def check(tus, cmd, jobs):
    seen = collections.defaultdict(lambda: collections.defaultdict(list))
    foreign, tag_files = {}, collections.defaultdict(set)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for tu, (decls, undefined, tags) in zip(tus, ex.map(lambda t: decls_of(t, cmd), tus)):
            for name, canon, spelled in decls:
                seen[name][canon].append((tu, spelled))
            if undefined:
                foreign[tu] = sorted(undefined)
            for tag, f in tags:
                tag_files[tag].add(f)
    dup_tags = {tag: sorted(fs) for tag, fs in tag_files.items() if len(fs) > 1}
    return {n: v for n, v in seen.items() if len(v) > 1}, foreign, len(seen), dup_tags


def build_inputs(build_dir):
    d = os.path.join(build_dir, "kernel", "CMakeFiles", "thylacine.elf.dir")
    flags = {}
    for line in open(os.path.join(d, "flags.make")):
        k, sep, v = line.partition(" = ")
        if sep:
            flags[k.strip()] = v.strip()
    # Every C source of the kernel ELF -- kernel/, arch/, mm/, lib/ and the
    # generated ones; a relative path is the build directory's.
    listed = set(re.findall(r"(\S+\.c)$", open(os.path.join(d, "build.make")).read(), re.M))
    srcs = sorted({os.path.realpath(s if os.path.isabs(s) else os.path.join(build_dir, s)) for s in listed})
    roots = {s.rsplit("/kernel/", 1)[0] for s in srcs if "/kernel/" in s and "/build/" not in s}
    if len(roots) != 1:
        sys.exit(f"check-decls: cannot place the source tree from {d}/build.make (roots: {sorted(roots)})")
    missing = [s for s in srcs if not os.path.exists(s)]
    if missing:
        print(f"check-decls: {len(missing)} listed source(s) do not exist, first {missing[0]}", file=sys.stderr)
        sys.exit(2)
    global ROOT
    ROOT = roots.pop()
    clang = re.search(r"^# compile C with (\S+)", open(os.path.join(d, "flags.make")).read(), re.M).group(1)
    test_dir = ROOT + "/kernel/test"
    cmd = [clang] + shlex.split(flags.get("C_DEFINES", "")) + ["-I" + test_dir] + \
        shlex.split(flags.get("C_INCLUDES", "")) + shlex.split(flags.get("C_FLAGS", ""))
    return srcs, cmd


ROOT = None


def short(path):
    if ROOT and path.startswith(ROOT + "/"):
        return path[len(ROOT) + 1:]
    return "kernel/" + path.split("/kernel/", 1)[1] if "/kernel/" in path else path


def report(conflicts, foreign, dup_tags):
    for tag in sorted(dup_tags):
        print(f"check-decls: {tag} is defined in {len(dup_tags[tag])} files: "
              f"{', '.join(short(f) for f in dup_tags[tag])}")
    for tu in sorted(foreign):
        names = foreign[tu]
        print(f"check-decls: {short(tu)} declares {len(names)} name(s) it does not define: "
              f"{', '.join(names[:6])}{' ...' if len(names) > 6 else ''}")
    for name in sorted(conflicts):
        print(f"check-decls: {name} has {len(conflicts[name])} types:")
        for canon, sites in sorted(conflicts[name].items()):
            tus = sorted({short(t) for t, _ in sites})
            print(f"    {sites[0][1]}    in {', '.join(tus[:4])}{' ...' if len(tus) > 4 else ''}")


def self_test():
    """The detector must go red on a seeded mismatch and stay green on its absence."""
    # (a.c, b.c, names rule (a) must report, names rule (b) must report in b.c).
    # Every case has a header h.h that both files may include.
    hdr = "long hf(int x);\nextern int hv;\nstruct T { int t; };\n"
    cases = {
        "mismatch": ("long f(int x) { return x; }\n", "extern long f(unsigned x);\nlong g(void) { return f(1); }\n", {"f"}, {"f"}),
        "local-extern": ("long f(int x) { return x; }\n", "extern long f(int x);\nlong g(void) { return f(1); }\n", set(), {"f"}),
        "bare-prototype": ("long f(int x) { return x; }\n", "long f(int x);\nlong g(void) { return f(1); }\n", set(), {"f"}),
        "header": ('#include "h.h"\nlong hf(int x) { return x; }\nint hv;\n',
                   '#include "h.h"\nlong g(void) { return hf(hv); }\n', set(), set()),
        "own-prototype": ("long f(int x) { return x; }\n", "long g(void);\nlong g(void) { return 1; }\n", set(), set()),
        "typedef": ("long f(int x) { return x; }\n", "typedef int myint;\nextern long f(myint x);\nlong g(void) { return f(1); }\n", set(), {"f"}),
        "array": ("int a[4];\n", "extern int a[];\nint g(void) { return a[0]; }\n", set(), {"a"}),
        "object": ("long v;\n", "extern int v;\nint g(void) { return v; }\n", {"v"}, {"v"}),
        "own-object": ("int k;\n", "extern int v;\nint v = 3;\n", set(), set()),
        "macro-first": ("#include <stdbool.h>\nbool f(int x) { return x; }\n",
                        "#include <stdbool.h>\nbool f(int x);\nbool g(void) { return f(1); }\n", set(), {"f"}),
        "block-extern": ("long f(int x) { return x; }\n",
                         "long g(void) { extern long f(int x); return f(1); }\n", set(), {"f"}),
        "block-mismatch": ("long f(int x) { return x; }\n",
                           "long g(void) { extern long f(long x); return f(1); }\n", {"f"}, {"f"}),
        "block-object": ("int v;\n", "int g(void) { extern int v; return v; }\n", set(), {"v"}),
        "block-locals": ("int k;\n", "int g(void) { static int s; int a = 0; return s + a; }\n", set(), set()),
        "struct-twice": ("struct S { int x; };\nint f(struct S *s) { return s->x; }\n",
                         "struct S { int x; long y; };\nlong g(struct S *s) { return s->y; }\n", set(), set(), {"struct S"}),
        "struct-header": ('#include "h.h"\nint f(struct T *p) { return p->t; }\n',
                          '#include "h.h"\nint g(struct T *p) { return p->t; }\n', set(), set(), set()),
        "struct-local": ("int f(void) { struct L { int a; } l = { 1 }; return l.a; }\n",
                         "int g(void) { struct L { long b; } l = { 2 }; return (int)l.b; }\n", set(), set(), set()),
        "builtin": ("long f(int x) { return x; }\n",
                    "long g(long *p) { return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED) + __builtin_ctzll(8); }\n",
                    set(), set()),
        "static": ("static int h(void) { return 0; }\nint k(void) { return h(); }\n",
                   "static long h(void) { return 0; }\nlong m(void) { return h(); }\n", set(), set()),
    }
    clang = os.environ.get("CC", "clang")
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, "h.h"), "w").write(hdr)
        for case, (a, b, want_a, want_b, *want_c) in cases.items():
            pa, pb = os.path.join(tmp, f"{case}_a.c"), os.path.join(tmp, f"{case}_b.c")
            open(pa, "w").write(a)
            open(pb, "w").write(b)
            got, foreign, _, tags = check([pa, pb], [clang, "-std=c99", "-I" + tmp], 2)
            got_b = set(foreign.get(pb, []))
            ok = set(got) == want_a and got_b == want_b and pa not in foreign and \
                set(tags) == (want_c[0] if want_c else set())
            bad += not ok
            print(f"check-decls self-test {case}: {'ok' if ok else 'WRONG'} "
                  f"(a: {sorted(got) or 'nothing'}; b: {sorted(got_b) or 'nothing'}; a.c: {foreign.get(pa) or 'nothing'}; "
                  f"c: {sorted(tags) or 'nothing'})")
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
    conflicts, foreign, names, dup_tags = check(srcs, cmd, a.jobs)
    if conflicts or foreign or dup_tags:
        report(conflicts, foreign, dup_tags)
        print(f"check-decls: FAIL -- {len(conflicts)} of {names} names declared with more than one type; "
              f"{len(foreign)} .c file(s) declare {sum(len(v) for v in foreign.values())} name(s) they do not define; "
              f"{len(dup_tags)} struct/union tag(s) defined in more than one file; {len(srcs)} TUs")
        sys.exit(1)
    print(f"check-decls: ok -- {names} non-static names, one type each, every one declared where it is "
          f"defined or in a header; every struct/union tag defined once; {len(srcs)} TUs")


if __name__ == "__main__":
    main()
