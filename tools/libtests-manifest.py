#!/usr/bin/env python3
"""Translate a library's CTest or meson registration into a muster manifest.

Boosty B-3 (dec-2026-10-14-b3-test-bar): each browser library's compiled
upstream tests run on the device. The test list is upstream's own -- CTest's
`--show-only=json-v1` dump of the cross-built tree, or `meson introspect
--tests` -- so it cannot drift from what upstream runs. This rewrites it for
the device (usr/muster/src/main.rs reads the result) and lists the files the
device needs.

  --json F      the CTest dump, taken AFTER the build (CTest omits a test's
                command while its executable does not exist)
  --meson F     instead, meson's test list (`meson introspect --tests`)
  --meson-build D   its build directory, where a test that names no workdir
                runs (mtest.py runs the harness there)
  --meson-source D  its source tree, searched for what the list cannot carry
  --skip NAME   a meson test not translated (repeat): only one whose program
                is a Python script, and every such test must be named, so an
                interpreter-driven test upstream adds is refused, not dropped
  --lib NAME    the library's name, for the verdict line
  --map H=D     a host directory and where it sits on the device (repeat;
                the build tree and the source tree, at least)
  --logs D      the device directory for the per-test logs
  --png-template F  libpng's scripts/cmake/test.cmake.in, for the wrapper
  --out F       the manifest
  --stage F     the files to stage, one per line: `exe <host> <device>` for
                each program, `file <host> <device>` for each input a test
                names that exists before any test runs, `tree <host> <device>`
                for a directory a meson test's environment names

Refused, so the build fails rather than ship a list that runs less than
upstream's: a test with no command, a property other than WORKING_DIRECTORY
and DEPENDS (PASS_REGULAR_EXPRESSION only in the digest shape below), a
`cmake` command that is not one of the shapes below, a path outside every
--map, an argument holding a newline, and a relative argument that would
resolve elsewhere on the device than on the host.

The `cmake` shapes:
  - `cmake -E copy_if_different A B` (libjpeg-turbo's tjbench setup): the
    runner's builtin copy;
  - `cmake -E compare_files A B`: the runner's builtin cmp;
  - `cmake -E sha256sum F` with PASS_REGULAR_EXPRESSION exactly
    `^<64 lowercase hex>  ` (CMake prints `<digest>  <file>`): the runner's
    builtin sha256, which passes when F's digest is that one;
  - `cmake -DLIBPNG=... -DTEST_COMMAND=<exe> -P <tests/NAME.cmake>` (every
    libpng test): the generated script only runs <exe> with its
    TEST_OPTIONS and TEST_FILES and checks the status, which is what the
    runner does; the script's text is checked against the template so a
    libpng that changes the wrapper is refused;
  - `cmake -D... -P run_and_diff.cmake` and `-P test_xmlcatalog_add_del.cmake`
    (libxml2's script suites and its catalog test): each becomes the steps
    the script performs -- a program with its stdin and outputs redirected
    and its status unchecked, then the comparisons; or the catalog's create,
    adds, compare, deletes, compare -- and each script is checked by its
    sha256, so a changed script is refused.

A test is one or more steps (`step` in the manifest); every shape above but
the two libxml2 scripts is one.

A meson test is its command, run in its workdir with its env (`env` lines;
the LD_LIBRARY_PATH / DYLD_LIBRARY_PATH meson adds for its own build tree
are dropped, the libraries being static); a TAP test is that command with
its stdout kept, then the runner's builtin tap, which judges it as meson
does. Refused: a key the list has that this script does not know, another
protocol, extra_paths, an env path outside every --map, a directory argument,
an output of a target the test depends on that is not built (`meson test`
builds those first, and a target only a test needs is outside `all`) or that
nothing stages, and a source tree that mentions should_fail (introspection
does not export it, so a test expected to fail would translate as one
expected to pass) or MESON_TEST_ITERATION (which the harness sets and muster
does not). Not
carried: timeouts (the gate's own bounds the run), and the harness's
MALLOC_PERTURB_ and sanitizer options, which this libc does not read.
"""

import argparse
import json
import os
import re
import sys

ALLOWED_PROPS = {"WORKING_DIRECTORY", "DEPENDS", "PASS_REGULAR_EXPRESSION"}
DIGEST_REGEX = re.compile(r"\^([0-9a-f]{64})  ")


def die(msg):
    print(f"libtests-manifest: {msg}", file=sys.stderr)
    sys.exit(1)


class Mapper:
    def __init__(self, pairs):
        self.pairs = []
        for p in pairs:
            host, sep, dev = p.partition("=")
            if not sep or not host or not dev:
                die(f"--map {p!r} is not HOST=DEVICE")
            self.pairs.append((os.path.realpath(host), dev.rstrip("/") or "/"))
        # Longest host prefix first, so a nested tree maps by its own entry.
        self.pairs.sort(key=lambda hd: -len(hd[0]))

    def device(self, host_path):
        real = os.path.realpath(host_path)
        for host, dev in self.pairs:
            if real == host:
                return dev
            if real.startswith(host + os.sep):
                return dev + real[len(host):]
        return None


def cmake_list(value):
    return [] if value == "" else value.split(";")


def png_unwrap(script_path, template_path):
    """The argv a libpng test wrapper runs, after TEST_COMMAND."""
    text = open(script_path).read()
    opts = re.search(r'^set\(TEST_OPTIONS "([^"\n]*)"\)$', text, re.M)
    files = re.search(r'^set\(TEST_FILES "([^"\n]*)"\)$', text, re.M)
    if not opts or not files:
        die(f"{script_path}: no TEST_OPTIONS / TEST_FILES line")
    # Everything but the two substituted lines must be the template's text.
    tmpl = open(template_path).read()
    strip = lambda s: re.sub(r'^set\(TEST_(OPTIONS|FILES) "[^"\n]*"\)$', "", s, flags=re.M)
    if strip(text) != strip(tmpl):
        die(f"{script_path}: the wrapper is not {template_path} with its two values"
            " (libpng changed how a test runs; teach this script the new shape)")
    return cmake_list(opts.group(1)) + cmake_list(files.group(1))


# The two CMake scripts libxml2's tests run (restored by its port patch 0001),
# by content: a script that changes is refused rather than mistranslated.
LIBXML2_WRAPPERS = {
    "run_and_diff.cmake": "f40cdbab613908006a554f586c96042663e85f762a07b702cc615339e80b20a3",
    "test_xmlcatalog_add_del.cmake": "1339088a9235f15d9feb52453d6dcd18f726336c910d4ec8d80bae5824d680c7",
}


def defines(name, cmd, keys):
    """The -DKEY=VALUE arguments of `cmake -D... -P SCRIPT`, which must be exactly `keys`."""
    got = {}
    for a in cmd[1:-2]:
        k, sep, v = a[2:].partition("=")
        if not a.startswith("-D") or not sep or k in got:
            die(f"test {name!r}: {a!r} is not one -DKEY=VALUE")
        got[k] = v
    if set(got) != set(keys):
        die(f"test {name!r}: defines {sorted(got)}, the script takes {sorted(keys)}")
    return got


def wrapper(name, cmd):
    """The libxml2 script a `cmake -D... -P SCRIPT` test runs, checked by content."""
    if len(cmd) < 3 or cmd[-2] != "-P":
        return None
    base = os.path.basename(cmd[-1])
    if base not in LIBXML2_WRAPPERS:
        return None
    import hashlib
    have = hashlib.sha256(open(cmd[-1], "rb").read()).hexdigest()
    if have != LIBXML2_WRAPPERS[base]:
        die(f"test {name!r}: {cmd[-1]} is not the script this translator knows"
            f" (sha256 {have}); teach this script the new shape")
    return base


def step(argv, builtin=None, stdin=None, stdout=None, stderr=None, any_status=False):
    return {"argv": argv, "builtin": builtin, "stdin": stdin, "stdout": stdout,
            "stderr": stderr, "any": any_status}


def steps_of(test, args, props, host_cwd):
    """The test as muster steps, in host terms; and the directory they run in."""
    name = test["name"]
    cmd = list(test["command"])
    passre = props.get("PASS_REGULAR_EXPRESSION")
    if passre is not None:
        if isinstance(passre, str):
            passre = cmake_list(passre)
        m = DIGEST_REGEX.fullmatch(passre[0]) if len(passre) == 1 else None
        if not (m and os.path.basename(cmd[0]) == "cmake"
                and cmd[1:3] == ["-E", "sha256sum"] and len(cmd) == 4):
            die(f"test {name!r}: PASS_REGULAR_EXPRESSION {passre} is supported only"
                " as a SHA-256 digest on `cmake -E sha256sum FILE`")
        return [step([m.group(1), cmd[3]], "sha256")], host_cwd
    if os.path.basename(cmd[0]) != "cmake":
        return [step(cmd)], host_cwd
    if cmd[1:3] == ["-E", "copy_if_different"] and len(cmd) == 5:
        return [step(cmd[3:], "copy")], host_cwd
    if cmd[1:3] == ["-E", "compare_files"] and len(cmd) == 5:
        return [step(cmd[3:], "cmp")], host_cwd
    if (len(cmd) == 5 and cmd[1].startswith("-DLIBPNG=")
            and cmd[2].startswith("-DTEST_COMMAND=") and cmd[3] == "-P"):
        if not args.png_template:
            die(f"test {name!r} is a libpng wrapper; pass --png-template")
        return [step([cmd[2][len("-DTEST_COMMAND="):]] + png_unwrap(cmd[4], args.png_template))], host_cwd
    script = wrapper(name, cmd)
    if script == "run_and_diff.cmake":
        # PROG --shell XML in WORKDIR, stdin from STDIN (both relative to
        # WORKDIR, where execute_process resolves them); its status is only
        # reported; then stdout against the expected, and stderr when an
        # expected stderr exists.
        d = defines(name, cmd, ["PROG", "STDIN", "XML", "STDOUT", "STDERR",
                                "EXPECTED_STDOUT", "EXPECTED_STDERR", "WORKDIR"])
        st = [step([d["PROG"], "--shell", d["XML"]], stdin=d["STDIN"], stdout=d["STDOUT"],
                   stderr=d["STDERR"], any_status=True),
              step([d["STDOUT"], d["EXPECTED_STDOUT"]], "cmp")]
        if os.path.exists(d["EXPECTED_STDERR"]):
            st.append(step([d["STDERR"], d["EXPECTED_STDERR"]], "cmp"))
        return st, d["WORKDIR"]
    if script == "test_xmlcatalog_add_del.cmake":
        # In script mode CMAKE_CURRENT_BINARY_DIR is the directory cmake runs in.
        d = defines(name, cmd, ["XMLCAT", "EXPECTED_FULL", "EXPECTED_EMPTY"])
        work = os.path.join(host_cwd, "catalog_add_del_work")
        cat = os.path.join(work, "mycatalog")
        x = d["XMLCAT"]
        return [step([work], "rmtree"), step([work], "mkdir"),
                step([x, "--create", "--noout", cat]),
                step([x, "--noout", "--add", "public", "Pubid", "sysid", cat]),
                step([x, "--noout", "--add", "public", "Pubid2", "sysid2", cat]),
                step([x, "--noout", "--add", "public", "Pubid3", "sysid3", cat]),
                step([cat, d["EXPECTED_FULL"]], "cmp"),
                step([x, "--noout", "--del", "sysid", cat]),
                step([x, "--noout", "--del", "sysid3", cat]),
                step([x, "--noout", "--del", "sysid2", cat]),
                step([cat, d["EXPECTED_EMPTY"]], "cmp")], host_cwd
    die(f"test {name!r}: cmake command {cmd[1:]} is not a supported shape")


def translate(test, args, mapper):
    name = test["name"]
    if "command" not in test:
        die(f"test {name!r} has no command (its executable is not built?)")
    props = {p["name"]: p["value"] for p in test.get("properties", [])}
    extra = set(props) - ALLOWED_PROPS
    if extra:
        die(f"test {name!r}: properties {sorted(extra)} are not supported")
    host_cwd = props.get("WORKING_DIRECTORY")
    if not host_cwd:
        die(f"test {name!r} has no WORKING_DIRECTORY")
    steps, host_cwd = steps_of(test, args, props, host_cwd)
    deps = props.get("DEPENDS", [])
    if isinstance(deps, str):
        deps = cmake_list(deps)
    return lower(name, host_cwd, steps, [], list(deps), args, mapper)


def device_literal(a, args):
    """A path already in device terms: the runner's own log directory."""
    return a == args.logs or a.startswith(args.logs + "/")


def lower(name, host_cwd, steps, env, deps, args, mapper):
    """Host-term steps as device-term ones, and the files they need staged."""
    dev_cwd = mapper.device(host_cwd)
    if dev_cwd is None:
        die(f"test {name!r}: its directory {host_cwd} is outside every --map")
    stage = []

    def relative_input(a):
        """A relative path naming an existing file: staged where the device resolves it."""
        host = os.path.join(host_cwd, a)
        dev = mapper.device(host)
        want = os.path.normpath(os.path.join(dev_cwd, a))
        if dev is None or dev != want:
            die(f"test {name!r}: {a} resolves to {dev} on the device's map but"
                f" to {want} from the device cwd (keep the trees' relative layout)")
        stage.append(("file", os.path.realpath(host), dev))

    def path(a, what):
        """A redirect: an absolute path mapped, a relative one kept (and staged if it exists)."""
        if "\n" in a:
            die(f"test {name!r}: a {what} path holds a newline")
        if device_literal(a, args):
            return a
        if not os.path.isabs(a):
            if os.path.isfile(os.path.join(host_cwd, a)):
                relative_input(a)
            return a
        dev = mapper.device(a)
        if dev is None:
            die(f"test {name!r}: {what} {a} is outside every --map")
        if os.path.isfile(a):
            stage.append(("file", os.path.realpath(a), dev))
        return dev

    out = []
    for s in steps:
        argv = []
        for i, a in enumerate(s["argv"]):
            if "\n" in a:
                die(f"test {name!r}: an argument holds a newline")
            if device_literal(a, args):
                argv.append(a)
                continue
            host = a if os.path.isabs(a) else os.path.join(host_cwd, a)
            program = i == 0 and s["builtin"] is None
            if program or os.path.isabs(a):
                dev = mapper.device(host)
                if dev is None:
                    if program or os.path.exists(host):
                        die(f"test {name!r}: {a} is outside every --map")
                    argv.append(a)          # an absolute non-path option value
                    continue
                if program:
                    stage.append(("exe", os.path.realpath(host), dev))
                elif os.path.isfile(host):
                    stage.append(("file", os.path.realpath(host), dev))
                argv.append(dev)
                continue
            if os.path.isfile(host):
                relative_input(a)
            argv.append(a)              # a relative output, option or input
        if s["stdin"] is not None and not os.path.isfile(
                s["stdin"] if os.path.isabs(s["stdin"]) else os.path.join(host_cwd, s["stdin"])):
            die(f"test {name!r}: its stdin {s['stdin']} does not exist")
        out.append(dict(s, argv=argv, **{k: path(s[k], k) for k in ("stdin", "stdout", "stderr")
                                          if s[k] is not None}))
    dev_env = []
    for k, v in env:
        if "\n" in v:
            die(f"test {name!r}: env {k} holds a newline")
        if os.path.isabs(v):
            dev = mapper.device(v)
            if dev is None:
                die(f"test {name!r}: env {k}={v} is outside every --map")
            if os.path.isdir(v):
                stage.append(("tree", os.path.realpath(v), dev))
            elif os.path.isfile(v):
                stage.append(("file", os.path.realpath(v), dev))
            v = dev
        dev_env.append((k, v))
    return {"name": name, "cwd": dev_cwd, "env": dev_env, "steps": out, "depends": deps}, stage


MESON_KEYS = {"cmd", "depends", "env", "extra_paths", "is_parallel", "name", "priority",
              "protocol", "suite", "timeout", "workdir"}
# What meson's harness adds for its own build tree: inert for static libraries.
MESON_LIBPATH = ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH")


def interpreted(cmd):
    """A test whose program is a script, for an interpreter the device lacks."""
    prog = os.path.basename(cmd[0])
    return prog.endswith(".py") or re.fullmatch(r"python[0-9.]*", prog) is not None


def log_name(test):
    """muster's name for a test's logs (main.rs log_name)."""
    return test.replace("/", "_").replace(" ", "_")


def meson_guard(source):
    """Refuse a tree that relies on what `meson introspect --tests` cannot carry."""
    builds = 0
    for root, _dirs, files in os.walk(source):
        for f in files:
            full = os.path.join(root, f)
            if not os.path.isfile(full) or os.path.islink(full):
                continue
            data = open(full, "rb").read()
            if f == "meson.build":
                builds += 1
                if b"should_fail" in data:
                    die(f"{full} mentions should_fail, which introspection does not export;"
                        " teach this script the test's expected failure")
            if b"MESON_TEST_ITERATION" in data:
                die(f"{full} reads MESON_TEST_ITERATION, which muster does not set")
    if builds == 0:
        die(f"--meson-source {source} holds no meson.build (the guard searched nothing)")


def meson_outputs(build):
    """Each target's outputs, from the file mtest.py reads for a test's depends."""
    path = os.path.join(build, "meson-info", "intro-targets.json")
    if not os.path.isfile(path):
        die(f"{path} is absent: --meson-build is not a configured meson tree")
    return {t["id"]: [os.path.join(build, f) for f in t["filename"]]
            for t in json.load(open(path))}


def depends_staged(name, depends, outputs, stage, mapper):
    """Refuse a test whose depends `meson test` would build, unless each output is built and staged."""
    files = {dev for kind, _host, dev in stage if kind != "tree"}
    trees = [dev for kind, _host, dev in stage if kind == "tree"]
    for d in sorted(set(depends)):
        if d not in outputs:
            die(f"test {name!r} depends on {d!r}, a target the build does not list")
        for f in outputs[d]:
            if not os.path.exists(f):
                die(f"test {name!r} depends on {f}, which is not built (build the tree's"
                    " meson-test-prereq, as `meson test` does before it runs)")
            dev = mapper.device(f)
            if dev is None:
                die(f"test {name!r} depends on {f}, outside every --map")
            if dev not in files and not any(dev.startswith(t + "/") for t in trees):
                die(f"test {name!r} depends on {f}, which nothing stages (the test reads"
                    " it by a name its command and env do not carry)")


def meson_list(args, mapper):
    """The meson test list as device tests and their stage entries; and the skipped names."""
    if not args.meson_build or not args.meson_source:
        die("--meson needs --meson-build and --meson-source")
    tests = json.load(open(args.meson))
    if not isinstance(tests, list) or not tests:
        die(f"{args.meson} lists no tests")
    meson_guard(args.meson_source)
    outputs = meson_outputs(args.meson_build)
    names = [t.get("name") for t in tests]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        die(f"test names {dup} repeat (meson tells them apart by suite; the manifest cannot)")
    unknown = sorted(set(args.skip) - set(names))
    if unknown:
        die(f"--skip {unknown}: no such test")
    out, skipped = [], []
    # mtest.py runs a higher priority first, otherwise in registration order.
    for t in sorted(tests, key=lambda t: -t.get("priority", 0)):
        name = t["name"]
        extra = set(t) - MESON_KEYS
        if extra:
            die(f"test {name!r}: keys {sorted(extra)} are not supported")
        cmd = list(t["cmd"])
        if name in args.skip:
            if not interpreted(cmd):
                die(f"test {name!r} runs {cmd[0]}, a compiled program: it may not be skipped")
            skipped.append(name)
            continue
        if interpreted(cmd):
            die(f"test {name!r} runs {cmd[0]}: an interpreter-driven test -- name it in --skip"
                " (dec-2026-10-14-b3-test-bar) or translate it")
        if t.get("extra_paths"):
            die(f"test {name!r}: extra_paths {t['extra_paths']} are not supported")
        cwd = t.get("workdir") or args.meson_build
        for a in cmd[1:]:
            if os.path.isdir(a if os.path.isabs(a) else os.path.join(cwd, a)):
                die(f"test {name!r}: the argument {a} is a directory; stage it and teach this script")
        build = os.path.realpath(args.meson_build)
        env = []
        for k, v in sorted(t.get("env", {}).items()):
            if k in MESON_LIBPATH:
                for x in filter(None, v.split(os.pathsep)):
                    x = os.path.realpath(x)
                    if x != build and not x.startswith(build + os.sep):
                        die(f"test {name!r}: {k} names {x}, outside the build tree")
                continue
            if not k or "=" in k or "/" in k:
                die(f"test {name!r}: env name {k!r} cannot be an /env file")
            env.append((k, v))
        proto = t.get("protocol", "exitcode")
        if proto == "exitcode":
            steps = [step(cmd)]
        elif proto == "tap":
            log = f"{args.logs}/{log_name(name)}.tap"
            steps = [step(cmd, stdout=log), step([log], "tap")]
        else:
            die(f"test {name!r}: protocol {proto!r} is not supported")
        tr, st = lower(name, cwd, steps, env, [], args, mapper)
        depends_staged(name, t.get("depends", []), outputs, st, mapper)
        out.append((tr, st))
    return out, skipped


def order(tests):
    """Declaration order, with each test after the tests it DEPENDS on."""
    by_name = {t["name"]: t for t in tests}
    done, out = set(), []

    def visit(t, path):
        if t["name"] in done:
            return
        if t["name"] in path:
            die(f"a DEPENDS cycle through {t['name']!r}")
        for d in t["depends"]:
            if d not in by_name:
                die(f"test {t['name']!r} depends on {d!r}, which is not a test")
            visit(by_name[d], path | {t["name"]})
        done.add(t["name"])
        out.append(t)

    for t in tests:
        visit(t, frozenset())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--meson")
    ap.add_argument("--meson-build")
    ap.add_argument("--meson-source")
    ap.add_argument("--skip", action="append", default=[])
    ap.add_argument("--lib", required=True)
    ap.add_argument("--map", action="append", default=[], required=True)
    ap.add_argument("--logs", required=True)
    ap.add_argument("--png-template")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage", required=True)
    args = ap.parse_args()

    if bool(args.json) == bool(args.meson):
        die("give one of --json (CTest) and --meson")
    mapper = Mapper(args.map)
    skipped = []
    if args.json:
        if args.skip:
            die("--skip is for a meson list")
        dump = json.load(open(args.json))
        if not dump.get("tests"):
            die(f"{args.json} lists no tests")
        translated = [translate(t, args, mapper) for t in dump["tests"]]
    else:
        translated, skipped = meson_list(args, mapper)
    tests, stage = [], {}
    for tr, st in translated:
        tests.append(tr)
        for kind, host, dev in st:
            if stage.get(dev, (kind, host)) != (kind, host):
                die(f"{dev} is staged from two places")
            stage[dev] = (kind, host)
    tests = order(tests)

    with open(args.out, "w") as f:
        f.write(f"# muster manifest, from {'CTest' if args.json else 'meson'} ({len(tests)} tests);"
                " tools/libtests-manifest.py\n")
        if skipped:
            f.write(f"# skipped, interpreter-driven: {' '.join(skipped)}\n")
        f.write(f"lib {args.lib}\ncount {len(tests)}\nlogs {args.logs}\n")
        for t in tests:
            f.write(f"test {t['name']}\ncwd {t['cwd']}\n")
            for k, v in t["env"]:
                f.write(f"env {k}={v}\n")
            for s in t["steps"]:
                f.write("step\n")
                if s["builtin"]:
                    f.write(f"builtin {s['builtin']}\n")
                for a in s["argv"]:
                    f.write(f"arg {a}\n")
                for k in ("stdin", "stdout", "stderr"):
                    if s[k] is not None:
                        f.write(f"{k} {s[k]}\n")
                if s["any"]:
                    f.write("status any\n")
            f.write("end\n")
    with open(args.stage, "w") as f:
        for dev in sorted(stage):
            kind, host = stage[dev]
            f.write(f"{kind} {host} {dev}\n")
    print(f"libtests-manifest: {args.lib}: {len(tests)} tests, {len(stage)} staged entries"
          + (f", {len(skipped)} skipped" if skipped else ""))


if __name__ == "__main__":
    main()
