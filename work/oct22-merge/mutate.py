#!/usr/bin/env python3
# Sabotage edits for the main merge's restored witness. R1 drops the REGISTER
# rollback's release of the Spoors it had already resolved, the leak main's
# ref-rollback assertion in loom.register_rejects catches (the merge had taken
# corona's restructured test without it). Each edit is an exact (file, old, new)
# replacement whose `old` must occur EXACTLY once in the committed tree.
# Usage: python3 -I work/oct22-merge/mutate.py <root> <id>   |   --list   |   --check <root>
import sys

SC = "kernel/syscall.c"

MUTANTS = {
    "R1": ("loom.register_rejects", [
        (SC, "rollback:\n"
             "    for (u32 i = 0; i < got; i++) spoor_clunk(spoors[i]);\n"
             "    handle_put(&lh);\n"
             "    return rc;\n"
             "}\n"
             "\n"
             "int sys_loom_register_buffers_for_proc(",
             "rollback:\n"
             "    handle_put(&lh);\n"
             "    return rc;\n"
             "}\n"
             "\n"
             "int sys_loom_register_buffers_for_proc("),
    ]),
}


def read(root, path):
    with open(f"{root}/{path}") as f:
        return f.read()


def check(root):
    for m, (_, edits) in MUTANTS.items():
        for path, old, new in edits:
            n = read(root, path).count(old)
            if n != 1:
                sys.exit(f"{m}: anchor in {path} occurs {n} times, want 1")
            if old == new:
                sys.exit(f"{m}: an edit in {path} changes nothing")
    print("mutate.py --check: every anchor occurs exactly once: " + " ".join(MUTANTS))


def apply(root, m):
    _, edits = MUTANTS[m]
    for path, old, new in edits:
        src = read(root, path)
        if src.count(old) != 1:
            sys.exit(f"{m}: anchor in {path} occurs {src.count(old)} times, want 1")
        with open(f"{root}/{path}", "w") as f:
            f.write(src.replace(old, new))
        print(f"{m}: edited {path}")


if len(sys.argv) == 2 and sys.argv[1] == "--list":
    for m, (want, _) in MUTANTS.items():
        print(m, want)
elif len(sys.argv) == 3 and sys.argv[1] == "--check":
    check(sys.argv[2])
elif len(sys.argv) == 3 and sys.argv[2] in MUTANTS:
    apply(sys.argv[1], sys.argv[2])
else:
    sys.exit(__doc__ if __doc__ else "usage: mutate.py <root> <id> | --list | --check <root>")
