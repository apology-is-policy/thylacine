#!/usr/bin/env python3
"""Compare the kernel's protection-bit definitions against their hand-written
userspace mirrors.

Userspace cannot include a kernel header -- different toolchain and sysroot --
so `VMA_PROT_*` and `BURROW_PROT_*` are retyped in usr/lib/libt and
usr/lib/libthyla-rs with a comment asking the author to keep them equal. A
comment is not enforcement: the kernel's own copies of these bits ARE pinned
(`_Static_assert` in kernel/syscall.c ties BURROW_PROT_* to VMA_PROT_* and
VIV_PROT_*), and the kernel-to-userspace edge is the one with nothing on it.
Drift there is silent and W^X-adjacent (I-12): a prot the caller means as EXEC
arrives as something else, presenting as a wrong-permission fault rather than a
refusal.

The check is DERIVED, never name-pinned: both sides are enumerated and the sets
compared, so a bit APPENDED on one side is seen too (a guard pinned to a list of
three names is re-pointed by hand and goes stale the first time a fourth
arrives).

An ABSENCE is not a disagreement: a kernel composite (VMA_PROT_RW) or a bit
named only so its refusal can be spelled (BURROW_PROT_EXEC, never accepted as a
target per ARCH 6.5) has no business in a userspace header. So absences are
reported, and `--expect-unmirrored N` pins their COUNT -- which is the derived
value that makes case (b) of the hazard visible: a bit appended kernel-side and
not mirrored moves the count, and the check fails without anyone maintaining an
allowlist.

Exit 0 all mirrors agree / 1 a disagreement / 2 the check could not measure.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (family, kernel file, kernel prefix, [(mirror file, mirror prefix), ...])
FAMILIES = [
    (
        "VMA_PROT",
        "kernel/include/thylacine/vma.h", "VMA_PROT_",
        [
            ("usr/lib/libt/include/thyla/syscall.h", "T_PROT_"),
            ("usr/lib/libthyla-rs/src/lib.rs", "T_PROT_"),
        ],
    ),
    (
        "BURROW_PROT",
        "kernel/include/thylacine/syscall.h", "BURROW_PROT_",
        [
            ("usr/lib/libthyla-rs/src/lib.rs", "T_BURROW_PROT_"),
        ],
    ),
]

C_DEFINE = re.compile(r"^\s*#define\s+([A-Z0-9_]+)\s+(.+?)\s*(?://.*)?$")
RS_CONST = re.compile(r"^\s*pub const ([A-Z0-9_]+)\s*:\s*u(?:8|16|32|64)\s*=\s*([^;]+);")
SAFE_EXPR = re.compile(r"^[0-9xXa-fA-F_ ()|<>+~&]*$")


def value_of(expr, known):
    """Evaluate a C/Rust bit expression over already-known names."""
    e = expr.strip()
    # Names first, longest-first so A_BC is not clipped by A_B.
    for name in sorted(known, key=len, reverse=True):
        e = e.replace(name, "(%d)" % known[name])
    e = re.sub(r"\b(\d+)[uU][lL]*\b", r"\1", e)       # 1u, 0ull
    e = re.sub(r"\b(0[xX][0-9a-fA-F]+)[uU][lL]*\b", r"\1", e)
    if not SAFE_EXPR.match(e):
        return None
    try:
        return int(eval(e, {"__builtins__": {}}, {}))  # noqa: S307 -- SAFE_EXPR-gated
    except Exception:
        return None


def collect(relpath, prefix):
    """Every PREFIX* constant in the file, in declaration order."""
    path = os.path.join(ROOT, relpath)
    if not os.path.isfile(path):
        return None, "missing file"
    pat = RS_CONST if relpath.endswith(".rs") else C_DEFINE
    out, known = {}, {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = pat.match(line)
            if not m:
                continue
            name, expr = m.group(1), m.group(2)
            v = value_of(expr, known)
            if v is not None:
                known[name] = v
            if name.startswith(prefix):
                if v is None:
                    return None, "unparsable value for %s: %r" % (name, expr)
                out[name[len(prefix):]] = v
    return out, None


def main():
    expect = None
    args = sys.argv[1:]
    while args:
        if args[0] == "--expect-unmirrored" and len(args) > 1:
            expect = int(args[1]); args = args[2:]
        else:
            print("usage: check-prot-mirror.py [--expect-unmirrored N]")
            return 2
    fail = 0
    unmirrored = []
    for family, kfile, kprefix, mirrors in FAMILIES:
        kbits, err = collect(kfile, kprefix)
        if err or not kbits:
            print("REFUSING: %s -- no %s* constants read from %s (%s)"
                  % (family, kprefix, kfile, err or "zero matches"))
            print("  A check that measured nothing cannot report agreement.")
            return 2
        print("== %s: %d kernel bit(s) in %s" % (family, len(kbits), kfile))
        for suffix in sorted(kbits, key=lambda s: kbits[s]):
            print("     %s%-8s = 0x%x" % (kprefix, suffix, kbits[suffix]))
        for mfile, mprefix in mirrors:
            mbits, err = collect(mfile, mprefix)
            if err or not mbits:
                print("REFUSING: %s -- no %s* constants read from %s (%s)"
                      % (family, mprefix, mfile, err or "zero matches"))
                return 2
            print("   -- mirror %s (%s*): %d bit(s)" % (mfile, mprefix, len(mbits)))
            for suffix in sorted(mbits, key=lambda s: mbits[s]):
                if suffix not in kbits:
                    print("      MISMATCH: %s%s = 0x%x exists in userspace with NO"
                          " kernel counterpart" % (mprefix, suffix, mbits[suffix]))
                    fail = 1
                elif mbits[suffix] != kbits[suffix]:
                    print("      MISMATCH: %s = 0x%x but %s%s = 0x%x"
                          % (kprefix + suffix, kbits[suffix], mprefix, suffix,
                             mbits[suffix]))
                    fail = 1
                else:
                    print("      ok  %s%s = 0x%x" % (mprefix, suffix, mbits[suffix]))
            for suffix in sorted(kbits, key=lambda s: kbits[s]):
                if suffix not in mbits:
                    unmirrored.append((kprefix + suffix, kbits[suffix], mfile))
    if unmirrored:
        print()
        print("KERNEL BITS WITH NO MIRROR (not a disagreement -- an ABSENCE, and")
        print("the reason has to be stated somewhere a reader will find it):")
        for name, val, mfile in unmirrored:
            print("   %s = 0x%x  absent from %s" % (name, val, mfile))
    print()
    print("check-prot-mirror: %d unmirrored kernel bit(s)" % len(unmirrored))
    if expect is not None and len(unmirrored) != expect:
        print("MISMATCH: expected %d unmirrored bit(s), found %d -- a kernel bit was"
              % (expect, len(unmirrored)))
        print("  added or removed without its mirror. Mirror it, or re-pin the count")
        print("  with the reason in the commit (never with a silent bump).")
        fail = 1
    print("check-prot-mirror: %s" % ("FAIL -- a mirror disagrees with the kernel"
                                     if fail else "PASS -- every shared bit agrees"))
    return fail


if __name__ == "__main__":
    sys.exit(main())
