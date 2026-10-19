#!/usr/bin/env python3
# Sabotage edits for vote 13's new witnesses (the 134 renumber's gap and
# rt_sigaction pins, and the round-1 counter that replaced the gap pin's value
# check). Each mutant is a list of exact (file, old, new) replacements; every
# `old` must occur EXACTLY once in the committed tree or the mutant refuses (a
# stale anchor must never apply nothing).
# Usage: python3 -I work/oct19-vote13/mutate.py <root> <id>   |   --list   |   --check <root>
import sys

SC = "kernel/syscall.c"
VC = "kernel/vivarium.c"

COUNT = "        __atomic_fetch_add(&g_syscall_unknown, 1u, __ATOMIC_RELAXED);\n"
UNKNOWN_ARM = ("    default:\n"
               "        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;\n"
               "        // v1.0 returns -1 (ENOSYS) and lets userspace decide.\n")

MUTANTS = {
    # The unknown arm stops counting: the loop must see +0 and fail.
    "M1": ("syscall.native_gap_unassigned", [
        (SC, COUNT, ""),
    ]),
    # A native handler lands in the gap and answers -1 to zeroed arguments --
    # exactly what the pre-counter pin (a value check) let through.
    "M2": ("syscall.native_gap_unassigned", [
        (SC, UNKNOWN_ARM,
             "    case 130:\n"
             "        ctx->regs[0] = (u64)(s64)-1;\n"
             "        return;\n\n" + UNKNOWN_ARM),
    ]),
    # Every native dispatch counts as unknown: the control (134) must see +1.
    "M3": ("syscall.native_gap_unassigned", [
        (SC, COUNT, ""),
        (SC, "static void syscall_native_switch(struct exception_context *ctx) {\n",
             "static void syscall_native_switch(struct exception_context *ctx) {\n"
             "    __atomic_fetch_add(&g_syscall_unknown, 1u, __ATOMIC_RELAXED);\n"),
    ]),
    # rt_sigaction's decision admits sigsetsize 0, the registry call's shape.
    "M4": ("vivarium.rejects_are_deliberate", [
        (VC, "    // _NSIG/8 == 8 at every call site. A different size means a caller whose\n"
             "    // sigset layout we have not reasoned about.\n"
             "    if (sigsetsize != 8) return VIV_FORWARD;\n",
             "    // _NSIG/8 == 8 at every call site. A different size means a caller whose\n"
             "    // sigset layout we have not reasoned about.\n"
             "    if (sigsetsize != 8 && sigsetsize != 0) return VIV_FORWARD;\n"),
    ]),
    # A row in the 128..133 gap changes verdict (rt_sigsuspend, 133).
    "M5": ("vivarium.rejects_are_deliberate", [
        (VC, "    { VIV_LINUX_RT_SIGSUSPEND,   VIV_ENOSYS },\n",
             "    { VIV_LINUX_RT_SIGSUSPEND,   VIV_FORWARD },\n"),
    ]),
}


def apply(root, mid, write):
    test, edits = MUTANTS[mid]
    texts = {}
    for rel, old, new in edits:
        p = f"{root}/{rel}"
        t = texts.get(p) or open(p, encoding="utf-8").read()
        n = t.count(old)
        if n != 1:
            raise SystemExit(f"{mid}: anchor in {rel} occurs {n} times: {old[:70]!r}")
        texts[p] = t.replace(old, new)
    if write:
        for p, t in texts.items():
            open(p, "w", encoding="utf-8").write(t)
    return test


if __name__ == "__main__":
    if sys.argv[1:2] == ["--list"]:
        for m, (test, _) in MUTANTS.items():
            print(m, test)
    elif sys.argv[1:2] == ["--check"]:
        for m in MUTANTS:
            apply(sys.argv[2], m, False)
        print(f"anchors ok: {' '.join(MUTANTS)}")
    else:
        print(f"{sys.argv[2]} -> {apply(sys.argv[1], sys.argv[2], True)}")
