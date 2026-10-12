#!/usr/bin/env python3
# SQ-1 mutants exactly as run on corona (work/oct12-sq1/run-20261012T072527Z),
# re-anchored on corona/sq1-main 22c343232's kernel/loom.c.
# Usage (from a main worktree): python3 mutants-for-main.py m1|m2   -- edits kernel/loom.c in place;
# restore with `git checkout -- kernel/loom.c`. Refuses unless the anchor occurs exactly once.
import sys
M = {
 'm1': ("""        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_fetch_add(&h->dropped, 1u, __ATOMIC_RELEASE);
        loom_sq_release(l, h);                              // only after the copy
""", """        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        loom_sq_release(l, h);                              // MUTANT M1: release before the copy
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_fetch_add(&h->dropped, 1u, __ATOMIC_RELEASE);
"""),
 'm2': ("""            ce->sqe   = ksqe;
""", """            ce->sqe   = sqes[sqe_idx];                     // MUTANT M2: re-read after release
"""),
}
old, new = M[sys.argv[1]]
p = 'kernel/loom.c'
s = open(p).read()
n = s.count(old)
if n != 1:
    raise SystemExit("REFUSING: %s anchor occurs %d times" % (sys.argv[1], n))
open(p, 'w').write(s.replace(old, new, 1))
print("applied", sys.argv[1])
