#!/usr/bin/env python3
# Sabotage edits for the main merge's new witnesses. R1 drops the REGISTER
# rollback's release of the Spoors it had already resolved, the leak main's
# ref-rollback assertion in loom.register_rejects catches (the merge had taken
# corona's restructured test without it). R2 drops the reap before the private
# executor's charge (audit F1), which proc.reap_before_private_executor_charge
# catches in the qualification image, the only one that routes a private setup.
# Each edit is an exact (file, old, new) replacement whose `old` must occur
# EXACTLY once in the committed tree. Each mutant names its witness, the image
# it is booted in (ci or qual) and the witness's own FAIL message.
# Usage: python3 -I work/oct22-merge/mutate.py <root> <id> | --check <root>
#        | --list | --image <id> | --kill <id> | --files <id>
import sys

SC = "kernel/syscall.c"
LS = "kernel/loom_service_syscall.c"

MUTANTS = {
    "R1": ("loom.register_rejects", "ci", "and the resolved fd's ref was rolled back", [
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
    "R2": ("proc.reap_before_private_executor_charge", "qual",
           "a private setup frees the exited workers first and fits", [
        (LS, "    if (!rc && p == current_thread()->proc) proc_reap_exited_threads();\n"
             "    if (!rc) rc = loom_private_start_executor(l);\n",
             "    if (!rc) rc = loom_private_start_executor(l);\n"),
    ]),
}


def read(root, path):
    with open(f"{root}/{path}") as f:
        return f.read()


def check(root):
    for m, (_, image, _, edits) in MUTANTS.items():
        if image not in ("ci", "qual"):
            sys.exit(f"{m}: unknown image {image}")
        for path, old, new in edits:
            n = read(root, path).count(old)
            if n != 1:
                sys.exit(f"{m}: anchor in {path} occurs {n} times, want 1")
            if old == new:
                sys.exit(f"{m}: an edit in {path} changes nothing")
    print("mutate.py --check: every anchor occurs exactly once: " + " ".join(MUTANTS))


def apply(root, m):
    for path, old, new in MUTANTS[m][3]:
        src = read(root, path)
        if src.count(old) != 1:
            sys.exit(f"{m}: anchor in {path} occurs {src.count(old)} times, want 1")
        with open(f"{root}/{path}", "w") as f:
            f.write(src.replace(old, new))
        print(f"{m}: edited {path}")


a = sys.argv[1:]
if a == ["--list"]:
    for m, (want, _, _, _) in MUTANTS.items():
        print(m, want)
elif len(a) == 2 and a[0] == "--check":
    check(a[1])
elif len(a) == 2 and a[0] == "--image" and a[1] in MUTANTS:
    print(MUTANTS[a[1]][1])
elif len(a) == 2 and a[0] == "--kill" and a[1] in MUTANTS:
    print(MUTANTS[a[1]][2])
elif len(a) == 2 and a[0] == "--files" and a[1] in MUTANTS:
    print(" ".join(sorted({e[0] for e in MUTANTS[a[1]][3]})))
elif len(a) == 2 and a[1] in MUTANTS:
    apply(a[0], a[1])
else:
    sys.exit("usage: mutate.py <root> <id> | --check <root> | --list | --image <id> | --kill <id> | --files <id>")
