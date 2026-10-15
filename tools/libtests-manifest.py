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
and DEPENDS, a `cmake` command that is not one of the two shapes below, a
path outside every --map, an argument holding a newline, and a relative
argument that would resolve elsewhere on the device than on the host.

The `cmake` shapes:
  - `cmake -E copy_if_different A B` (libjpeg-turbo's tjbench setup): the
    runner's builtin copy;
  - `cmake -E compare_files A B`: the runner's builtin cmp;
  - `cmake -DLIBPNG=... -DTEST_COMMAND=<exe> -P <tests/NAME.cmake>` (every
    libpng test): the generated script only runs <exe> with its
    TEST_OPTIONS and TEST_FILES and checks the status, which is what the
    runner does; the script's text is checked against the template so a
    libpng that changes the wrapper is refused.
"""

import argparse
import json
import os
import re
import sys

ALLOWED_PROPS = {"WORKING_DIRECTORY", "DEPENDS"}


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
    cmd = list(test["command"])
    builtin = None
    if os.path.basename(cmd[0]) == "cmake":
        if cmd[1:3] == ["-E", "copy_if_different"] and len(cmd) == 5:
            builtin, cmd = "copy", cmd[3:]
        elif cmd[1:3] == ["-E", "compare_files"] and len(cmd) == 5:
            builtin, cmd = "cmp", cmd[3:]
        elif (len(cmd) == 5 and cmd[1].startswith("-DLIBPNG=")
              and cmd[2].startswith("-DTEST_COMMAND=") and cmd[3] == "-P"):
            if not args.png_template:
                die(f"test {name!r} is a libpng wrapper; pass --png-template")
            cmd = [cmd[2][len("-DTEST_COMMAND="):]] + png_unwrap(cmd[4], args.png_template)
        else:
            die(f"test {name!r}: cmake command {cmd[1:]} is not a supported shape")

    dev_cwd = mapper.device(host_cwd)
    if dev_cwd is None:
        die(f"test {name!r}: WORKING_DIRECTORY {host_cwd} is outside every --map")
    stage, argv = [], []
    for i, a in enumerate(cmd):
        if "\n" in a:
            die(f"test {name!r}: an argument holds a newline")
        host = a if os.path.isabs(a) else os.path.join(host_cwd, a)
        program = i == 0 and builtin is None
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
            dev = mapper.device(host)
            want = os.path.normpath(os.path.join(dev_cwd, a))
            if dev is None or dev != want:
                die(f"test {name!r}: {a} resolves to {dev} on the device's map but"
                    f" to {want} from the device cwd (keep the trees' relative layout)")
            stage.append(("file", os.path.realpath(host), dev))
        argv.append(a)              # a relative output, option or input
    deps = props.get("DEPENDS", [])
    if isinstance(deps, str):
        deps = cmake_list(deps)
    return {"name": name, "cwd": dev_cwd, "argv": argv, "builtin": builtin,
            "depends": list(deps)}, stage


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
            if t["builtin"]:
                f.write(f"builtin {t['builtin']}\n")
            for a in t["argv"]:
                f.write(f"arg {a}\n")
            f.write("end\n")
    with open(args.stage, "w") as f:
        for dev in sorted(stage):
            kind, host = stage[dev]
            f.write(f"{kind} {host} {dev}\n")
    print(f"libtests-manifest: {args.lib}: {len(tests)} tests, {len(stage)} staged files")


if __name__ == "__main__":
    main()
