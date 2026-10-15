#!/usr/bin/env python3
"""Translate a library's CTest registration into a muster manifest.

Boosty B-3 (dec-2026-10-14-b3-test-bar): each browser library's compiled
upstream tests run on the device. The test list is upstream's own -- CTest's
`--show-only=json-v1` dump of the cross-built tree -- so it cannot drift from
what upstream runs. This rewrites it for the device (usr/muster/src/main.rs
reads the result) and lists the files the device needs.

  --json F      the CTest dump, taken AFTER the build (CTest omits a test's
                command while its executable does not exist)
  --lib NAME    the library's name, for the verdict line
  --map H=D     a host directory and where it sits on the device (repeat;
                the build tree and the source tree, at least)
  --logs D      the device directory for the per-test logs
  --png-template F  libpng's scripts/cmake/test.cmake.in, for the wrapper
  --out F       the manifest
  --stage F     the files to stage, one per line: `exe <host> <device>` for
                each program, `file <host> <device>` for each input a test
                names that exists before any test runs

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
    deps = props.get("DEPENDS", [])
    if isinstance(deps, str):
        deps = cmake_list(deps)
    return {"name": name, "cwd": dev_cwd, "steps": out, "depends": list(deps)}, stage


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
    ap.add_argument("--json", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--map", action="append", default=[], required=True)
    ap.add_argument("--logs", required=True)
    ap.add_argument("--png-template")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage", required=True)
    args = ap.parse_args()

    dump = json.load(open(args.json))
    if not dump.get("tests"):
        die(f"{args.json} lists no tests")
    mapper = Mapper(args.map)
    tests, stage = [], {}
    for t in dump["tests"]:
        tr, st = translate(t, args, mapper)
        tests.append(tr)
        for kind, host, dev in st:
            if stage.get(dev, (kind, host)) != (kind, host):
                die(f"{dev} is staged from two places")
            stage[dev] = (kind, host)
    tests = order(tests)

    with open(args.out, "w") as f:
        f.write(f"# muster manifest, from CTest ({len(tests)} tests); tools/libtests-manifest.py\n")
        f.write(f"lib {args.lib}\ncount {len(tests)}\nlogs {args.logs}\n")
        for t in tests:
            f.write(f"test {t['name']}\ncwd {t['cwd']}\n")
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
    print(f"libtests-manifest: {args.lib}: {len(tests)} tests, {len(stage)} staged files")


if __name__ == "__main__":
    main()
