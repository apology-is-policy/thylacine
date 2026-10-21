#!/usr/bin/env python3
"""tools/test-libtests-manifest.py -- the manifest tool's interpreter rule.

libtests-manifest.py refuses a library test the device cannot run because its
program is a script for an interpreter the device lacks. A test it misses runs
on the device and fails there; one it wrongly flags is refused at the bake. The
cases below hold both directions: every interpreted spelling, and the programs
beside them that are not. build.sh runs this before it uses the tool.
"""
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("manifest", os.path.join(HERE, "libtests-manifest.py"))
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)

# (first bytes of the program, interpreted?)
FILES = [
    (b"#!/usr/bin/python3\n", True),
    (b"#!/usr/bin/python3.12 -u\n", True),
    (b"#! /usr/bin/env python3\n", True),
    (b"#!/usr/bin/env python\n", True),
    (b"#!/usr/bin/env -S python3 -u\n", True),
    (b"#!/usr/bin/env -Spython3\n", True),
    (b"#!/usr/bin/env -iS python3\n", True),
    (b"#!/usr/bin/env --split-string=python3 -u\n", True),
    (b"#!/usr/bin/env -u PYTHONPATH python3\n", True),
    (b"#!/usr/bin/env --unset PYTHONPATH python3\n", True),
    (b"#!/usr/bin/env -C /tmp python3\n", True),
    (b"#!/usr/bin/env -i LC_ALL=C python3\n", True),
    (b"#!/usr/bin/env - python3\n", True),
    (b"#!/usr/bin/env -- python3\n", True),
    (b"#!/bin/sh\n", False),
    (b"#!/usr/bin/env sh\n", False),
    (b"#!/usr/bin/env -S sh -e\n", False),
    (b"#!/usr/bin/env -u python3 sh\n", False),
    (b"#!/usr/bin/env PYTHON=python3 sh\n", False),
    (b"#!/usr/bin/env\n", False),
    (b"\x7fELF\x02\x01\x01\x00", False),
    (b"python3\n", False),
]

# (program name, interpreted?) -- judged by the name alone.
NAMES = [
    ("run_tests.py", True),
    ("python3", True),
    ("/usr/bin/python3.12", True),
    ("test-python", False),
    ("pythonic", False),
]


def main():
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for i, (head, want) in enumerate(FILES):
            prog = os.path.join(d, f"t{i}")
            with open(prog, "wb") as f:
                f.write(head)
            got = manifest.interpreted([prog])
            if got != want:
                print(f"FAIL {head!r}: interpreted={got}, want {want}")
                bad += 1
        absent = os.path.join(d, "absent")
        if manifest.interpreted([absent]):
            print("FAIL an absent program reads as interpreted")
            bad += 1
    for name, want in NAMES:
        got = manifest.interpreted([os.path.join(HERE, "absent", name)])
        if got != want:
            print(f"FAIL name {name!r}: interpreted={got}, want {want}")
            bad += 1
    total = len(FILES) + len(NAMES) + 1
    print(f"test-libtests-manifest: {total - bad}/{total} cases")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
