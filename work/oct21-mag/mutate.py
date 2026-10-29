#!/usr/bin/env python3
# Sabotage edits for vote 16 (each CPU's magazine set gets its own lock) and its
# guest witness, phys.drain_meets_live_owners. G2 puts back the pre-lock pair
# from b057de8eb, the code the vote replaced; G1 drops only the drainer's side
# of the lock, G3 only the owner's alloc side, so neither side alone suffices.
# Each edit is an exact (file, old, new) replacement whose `old` must occur
# EXACTLY once in the committed tree, or ("@<rev>", file): that file's text at
# <rev>, which must differ from the tree's (a stale anchor must never apply
# nothing).
# Usage: python3 -I work/oct21-mag/mutate.py <root> <id>   |   --list   |   --check <root>
import subprocess
import sys

MC = "mm/magazines.c"
MH = "mm/magazines.h"
PRE_LOCK = "b057de8eb"
WANT = "phys.drain_meets_live_owners"

MUTANTS = {
    # The tree before vote 16: an IRQ mask on the owner, nothing on the drain.
    "G2": (WANT, [("@" + PRE_LOCK, MC), ("@" + PRE_LOCK, MH)]),
    # The drain stops taking each set's lock (the owner still takes it).
    "G1": (WANT, [
        (MC, "        irq_state_t s = spin_lock_irqsave(&pc->lock);\n",
             "        irq_state_t s = spin_lock_irqsave(NULL);\n"),
        (MC, "        spin_unlock_irqrestore(&pc->lock, s);\n",
             "        spin_unlock_irqrestore(NULL, s);\n"),
    ]),
    # mag_alloc stops taking its set's lock (mag_free and the drain still do).
    "G3": (WANT, [
        (MC, "    spin_lock(&pc->lock);\n"
             "    struct magazine *m = &pc->mags[idx];\n"
             "    // #807 regression guard",
             "    struct magazine *m = &pc->mags[idx];\n"
             "    // #807 regression guard"),
        (MC, "            spin_unlock(&pc->lock);\n"
             "            spin_unlock_irqrestore(NULL, s);\n"
             "            return NULL;\n",
             "            spin_unlock_irqrestore(NULL, s);\n"
             "            return NULL;\n"),
        (MC, "    struct page *p = m->entries[--m->count];\n"
             "    spin_unlock(&pc->lock);\n",
             "    struct page *p = m->entries[--m->count];\n"),
    ]),
}


def apply(root, mid, write):
    test, edits = MUTANTS[mid]
    texts = {}
    for edit in edits:
        if edit[0].startswith("@"):
            rev, rel = edit[0][1:], edit[1]
            p = f"{root}/{rel}"
            old = subprocess.run(["git", "-C", root, "show", f"{rev}:{rel}"],
                                 capture_output=True, text=True, check=True).stdout
            if not old or old == (texts.get(p) or open(p, encoding="utf-8").read()):
                raise SystemExit(f"{mid}: {rel} at {rev} is empty or equals the tree's")
            texts[p] = old
            continue
        rel, old, new = edit
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
