#!/usr/bin/env python3
"""NA-2a source mutants. Each removes one property only the EL0 probe can see.

M8   the private params-copyout fault skips its rollback: the ring stays open
     and mapped, so the next setup takes another fd and another address
M9   the record result copyout ignores a fault, so a read-only record "succeeds"
     (the in-kernel fixtures use their own copyout and cannot see this)
M10  the default image routes a private request into the ordinary setup, which
     returns a ring instead of -EOPNOTSUPP
M11  the kernel test's lever pin is inverted: a qualification kernel is held to
     the default verdict, which the probe's honest 71 must fail
M12  the kernel test looks the probe up under a name the ramfs lacks, as a stale
     ramfs would: an image that carries init must not skip
M13  the probe makes a fifth private ring and exits with it open: its honest 71
     passes, and only the created count can fail it
M14  the params copyout skips _resv0, leaving the caller's bytes there; only the
     0xA5 fill can tell that from a kernel-written zero
M15  POOL QUERY ignores a faulted result copyout -- the one result copyout the
     in-kernel fixtures never fault

Usage: na2a-mutants.py --check | --files | <label>   (run from the tree root)
"""
import sys

MUTANTS = {
    "m8": [("kernel/syscall.c",
            "            sys_loom_setup_rollback(p, fd, kp.ring_va, kp.ring_size);\n"
            "            return private_ring ? -T_E_FAULT : -1;",
            "            if (!private_ring) sys_loom_setup_rollback(p, fd, kp.ring_va, kp.ring_size); // MUTANT M8\n"
            "            return private_ring ? -T_E_FAULT : -1;")],
    "m9": [("kernel/syscall.c",
            "        if (uaccess_store_u8(base + offset + i, b[i]) != 0) return -T_E_FAULT;",
            "        (void)uaccess_store_u8(base + offset + i, b[i]); // MUTANT M9")],
    "m10": [("kernel/syscall.c",
             "    bool private_ring = (flags & (LOOM_SETUP_PRIVATE_SERVICE | LOOM_SETUP_SERVICE_BUFFERS)) != 0;",
             "    bool private_ring = (flags & (LOOM_SETUP_PRIVATE_SERVICE | LOOM_SETUP_SERVICE_BUFFERS)) != 0 &&\n"
             "                        loom_private_routed();\n"
             "    if (!private_ring) flags &= LOOM_SETUP_VALID; // MUTANT M10")],
    "m11": [("kernel/test/test_loom_bridge_probe.c",
             "#ifdef LOOM_PRIVATE_QUALIFICATION\n",
             "#ifndef LOOM_PRIVATE_QUALIFICATION // MUTANT M11\n")],
    "m12": [("kernel/test/test_loom_bridge_probe.c",
             "    if (devramfs_lookup(\"bin/loom-bridge-probe\", &cpio_blob, &size) != 0) {\n",
             "    if (devramfs_lookup(\"bin/loom-bridge-probe-absent\", &cpio_blob, &size) != 0) { // MUTANT M12\n")],
    "m13": [("usr/loom-bridge-probe/loom-bridge-probe.c",
             "    close_ring(fd, &g_params);\n    t_putstr(\"loom-bridge-probe: qualification image",
             "    { static struct t_loom_params extra; if (setup_at(&extra, P | B, 2) < 0) t_exits(3); } // MUTANT M13\n"
             "    close_ring(fd, &g_params);\n    t_putstr(\"loom-bridge-probe: qualification image")],
    "m14": [("kernel/syscall.c",
             "    for (u64 i = 0; i < sizeof(struct loom_params); i++) {\n"
             "        if (uaccess_store_u8(params_va + i, src[i]) != 0) {\n",
             "    for (u64 i = 0; i < sizeof(struct loom_params); i++) {\n"
             "        if (i >= 52 && i < 56) continue; // MUTANT M14\n"
             "        if (uaccess_store_u8(params_va + i, src[i]) != 0) {\n")],
    "m15": [("kernel/loom_service_syscall.c",
             "        rc = loom_private_service_pool_query(l, p, record, &snap);\n"
             "        if (rc) return rc;\n"
             "        return copyout(ctx, 0, &snap, sizeof(snap)) ? -T_E_FAULT : 0;\n",
             "        rc = loom_private_service_pool_query(l, p, record, &snap);\n"
             "        if (rc) return rc;\n"
             "        (void)copyout(ctx, 0, &snap, sizeof(snap)); return 0; // MUTANT M15\n")],
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
