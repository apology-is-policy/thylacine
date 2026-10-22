#!/usr/bin/env python3
# The merge gate's NAMED tests, derived from the registrations, never listed by
# hand. Three classes, each of which must be non-empty:
#   vote16    work/oct21-mag/gate.sh's NAMED (vote 16's gate, read from it);
#   onesided  every test registered on exactly one parent: it has only ever
#             run against that parent's code;
#   resolved  every test defined in a kernel/test/*.c whose merged text
#             differs from both parents' (a resolved conflict or a seam edit).
# Every name printed must be registered at HEAD. Prints one name per line;
# the class counts go to stderr.
# Usage: python3 -I work/oct22-merge/named.py [--counts]   (any cwd)
from pathlib import Path
import re
import subprocess
import sys

CORONA = "23b0b0e5c8a678897890cbaadaf80ce6dad573e2"
MAIN = "a0128885b7e3c87c49d20c854d5133f891d5bf0b"
REG = re.compile(r'\{\s*"([^"]+)",\s*(\w+)\s*,')
ROOT = Path(__file__).resolve().parents[2]
DEF = re.compile(r'^void (test_\w+)\(void\)\s*\{', re.M)


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout


def rev(r):
    return git("rev-parse", "--verify", r + "^{commit}").strip()


def registrations(ref):
    src = git("show", ref + ":kernel/test/test.c")
    return dict(REG.findall(src))


def blob(ref, path):
    out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--verify", "-q", f"{ref}:{path}"],
                         capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else None


def main():
    corona, main_ = rev(CORONA), rev(MAIN)
    head = registrations("HEAD")
    func_to_name = {f: n for n, f in head.items()}
    c, m = set(registrations(corona)), set(registrations(main_))

    gate16 = (ROOT / "work/oct21-mag/gate.sh").read_text()
    v = re.search(r'^NAMED="([^"]+)"$', gate16, re.M)
    if not v:
        sys.exit("REFUSING: no NAMED= line in work/oct21-mag/gate.sh")
    vote16 = set(v.group(1).split())

    onesided = (c ^ m) & set(head)

    resolved = set()
    files = [p for p in git("ls-files", "kernel/test").split() if p.endswith(".c")]
    touched = []
    for p in files:
        x = blob("HEAD", p)
        if x != blob(corona, p) and x != blob(main_, p):
            touched.append(p)
            for f in DEF.findall(git("show", "HEAD:" + p)):
                if f in func_to_name:
                    resolved.add(func_to_name[f])

    classes = {"vote16": vote16, "onesided": onesided, "resolved": resolved}
    for k, s in classes.items():
        if not s:
            sys.exit(f"REFUSING: class {k} is empty")
    named = vote16 | onesided | resolved
    missing = sorted(named - set(head))
    if missing:
        sys.exit(f"REFUSING: not registered at HEAD: {missing}")
    print(" ".join(f"{k}={len(s)}" for k, s in classes.items()),
          f"union={len(named)} registered={len(head)} resolved_files={len(touched)}",
          file=sys.stderr)
    if "--counts" in sys.argv:
        print("resolved files: " + " ".join(touched), file=sys.stderr)
    for n in sorted(named):
        print(n)


main()
