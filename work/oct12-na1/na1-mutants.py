#!/usr/bin/env python3
"""NA-1 mutants. `na1-mutants.py --check` verifies every anchor occurs exactly
once in the tree; `na1-mutants.py <label>` applies one mutant in place.

M3  loom_private_submit publishes sq_head before reading the index and copying
    the SQE (the frontend twin of SQ-1). Default image.
M4  the legacy setup inner keeps no reference of its own across handle_alloc
    (the pre-fix code: a racing close frees the Loom under it). Default image.
M5  the setup rollback unmaps with the unsettled burrow_unmap (the pre-fix
    leak). Default image.
M6  a private register reservation is published before its result copyout, so
    a faulted copyout must roll back a LIVE slot. Qualification image.
M7  SYS_WEFT_SHARE admits a private ring's mapping (the type gate alone sees
    plain ANON). Qualification image.
"""
import sys

MUTANTS = {
    "m3": [("kernel/loom_service_frontend.c",
            "        u32 *indices=(void *)(l->ring_kva+l->sq_array_off);\n"
            "        u32 index=__atomic_load_n(&indices[l->sq_head&(l->sq_entries-1u)],__ATOMIC_ACQUIRE);\n",
            "        u32 *indices=(void *)(l->ring_kva+l->sq_array_off);\n"
            "        u32 pos=l->sq_head&(l->sq_entries-1u);\n"
            "        loom_sq_release(l,h); // MUTANT M3: release before the copy\n"
            "        u32 index=__atomic_load_n(&indices[pos],__ATOMIC_ACQUIRE);\n"),
           ("kernel/loom_service_frontend.c",
            "        loom_sq_release(l,h);\n        spin_unlock(&l->lock);++consumed;\n",
            "        spin_unlock(&l->lock);++consumed;\n")],
    "m4": [("kernel/syscall.c",
            "    // below still touches l, so hold a reference of our own until the end.\n"
            "    loom_ref(l);\n",
            "    // below still touches l, so hold a reference of our own until the end.\n"
            "    // MUTANT M4: no reference of our own\n"),
           ("kernel/syscall.c",
            "        loom_unref(l);\n        loom_unref(l);\n        return -1;\n",
            "        loom_unref(l);\n        return -1;\n"),
           ("kernel/syscall.c",
            "    loom_unref(l);   // may free it, if the fd is already closed; l is not touched again\n",
            "")],
    "m5": [("kernel/syscall.c",
            "    (void)sys_burrow_detach_for_proc(p, ring_va, ring_size);\n}\n",
            "    spin_lock(&p->as->lock);   // MUTANT M5: the unsettled unmap\n"
            "    (void)burrow_unmap(p, ring_va, (size_t)ring_size);\n"
            "    spin_unlock(&p->as->lock);\n}\n")],
    "m6": [("kernel/loom_service_syscall.c",
            "    int rc = copyout(ctx, at, &ref, sizeof(ref)) ? -T_E_FAULT : publish(l, ref);\n",
            "    int rc = publish(l, ref);   // MUTANT M6: published before the result copyout\n"
            "    if (!rc && copyout(ctx, at, &ref, sizeof(ref))) rc = -T_E_FAULT;\n")],
    "m7": [("kernel/syscall.c",
            "    if (vma->flags & VMA_FLAG_PRIVATE_RING) {\n"
            "        spin_unlock(&p->as->lock);\n"
            "        return -1;\n"
            "    }\n",
            "    // MUTANT M7: a private ring passes to the type gate\n")],
}
FILES = sorted({f for pairs in MUTANTS.values() for f, _, _ in pairs})

def check():
    bad = 0
    for lab, pairs in MUTANTS.items():
        for f, old, _ in pairs:
            n = open(f).read().count(old)
            if n != 1:
                print("%s: anchor in %s occurs %d times" % (lab, f, n)); bad += 1
    print("anchors: %d mutants, %d pairs, %d bad" %
          (len(MUTANTS), sum(len(p) for p in MUTANTS.values()), bad))
    return bad

def apply(lab):
    texts = {}
    for f, old, new in MUTANTS[lab]:
        s = texts.get(f) or open(f).read()
        if s.count(old) != 1:
            raise SystemExit("REFUSING: %s anchor in %s occurs %d times" % (lab, f, s.count(old)))
        texts[f] = s.replace(old, new, 1)
    for f, s in texts.items():
        open(f, "w").write(s)
    print(" ".join(sorted(texts)))

if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        sys.exit(1 if check() else 0)
    if sys.argv[1:] == ["--files"]:
        print(" ".join(FILES)); sys.exit(0)
    apply(sys.argv[1])
