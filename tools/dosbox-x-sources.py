#!/usr/bin/env python3
"""Emit the DOSBox-X curated compile list for the Thylacine Cryptid port (DX arc).

The object selection is DERIVED from the upstream Makefile.am `*_SOURCES` of each
kept subsystem -- correct-by-construction and self-maintaining across a DOSBox-X
version bump, rather than a 300-line hand-transcribed list that would drift. We
take the UNION of every .cpp/.c token in each subsystem's SOURCES (config.h gates
the code inside the files that a disabled feature would skip), then remove a
curated EXCLUDE set: macOS Objective-C++ (.mm), x86-only asm variants, and files
that pull headers/deps a DX-1 (core=normal, software video, nosound, no-net,
no-zlib) build does not carry. The exclude set grows as the compile-fix loop
surfaces outliers; each entry is commented with why.

Usage:  dosbox-x-sources.py <vendored-src-dir>
Prints newline-separated paths RELATIVE TO the src dir (e.g. cpu/cpu.cpp).
"""
import os, re, sys

# Subsystems whose Makefile.am SOURCES we compile. Order is irrelevant (a static
# archive-free single link). The bundled src/libs we DO compile are listed as
# their own subsystems; the ones we do NOT (fluidsynth, mt32, decoders, libchdr,
# tinyfiledialogs, xBRZ, physfs) are simply absent -- their code is gated off in
# config.h and nothing we compile must reference their symbols (enforced at link).
SUBSYS = [
    "",                       # the top src/ dir: dosbox.cpp
    "cpu",
    "debug",
    "dos",
    "fpu",
    "gui",
    "hardware",
    "hardware/serialport",
    "hardware/parport",
    "hardware/reSID",
    "hardware/mame",
    "ints",
    "misc",
    "shell",
    "builtin",
    "output",
    "aviwriter",
    "libs/gui_tk",
    "libs/zmbv",
]

# File-level excludes (path relative to src/, matched exactly OR by basename).
# Each MUST say why. This is where DX-1's feature posture is enforced at the
# object granularity; the compile-fix loop appends here.
EXCLUDE_EXACT = {
    # (nothing excluded at present -- opngeng.c IS kept: its "#error use
    #  opngen.x86" is gated by #if defined(OPNGENX86), which is off on aarch64,
    #  so it compiles the portable C generator that defines opngen_getpcm.)
}
EXCLUDE_BASENAME = {
    # nothing yet beyond the .mm rule below
}

def sources_of(srcdir, sub):
    mk = os.path.join(srcdir, sub, "Makefile.am")
    if not os.path.isfile(mk):
        sys.exit(f"dosbox-x-sources: no Makefile.am at {sub or '.'}")
    text = open(mk, encoding="utf-8", errors="replace").read()
    # join line continuations so multi-line SOURCES = ... \ are one logical line
    text = text.replace("\\\n", " ")
    toks = set()
    for line in text.splitlines():
        if "_SOURCES" not in line:
            continue
        # everything after the first '=' on a *_SOURCES (= or +=) line
        m = re.search(r"_SOURCES\s*\+?=\s*(.*)$", line)
        if not m:
            continue
        for t in re.findall(r"[A-Za-z0-9_./-]+\.(?:cpp|c|mm)\b", m.group(1)):
            toks.add(t)
    return toks

def main():
    if len(sys.argv) != 2:
        sys.exit("usage: dosbox-x-sources.py <vendored-src-dir>")
    srcdir = sys.argv[1]
    out = []
    seen = set()
    for sub in SUBSYS:
        for t in sorted(sources_of(srcdir, sub)):
            # resolve the token (may be a bare name, a subdir path, or ../libs/..)
            rel = os.path.normpath(os.path.join(sub, t)) if sub else os.path.normpath(t)
            if rel.startswith(".."):
                continue  # reaches outside src/ (e.g. ../libs/physfs/*.mm) -- not a DX-1 input
            if not os.path.isfile(os.path.join(srcdir, rel)):
                continue  # token names a file not present (conditional/platform) -- skip
            if rel.endswith(".mm"):
                continue  # macOS Objective-C++
            if rel in EXCLUDE_EXACT or os.path.basename(rel) in EXCLUDE_BASENAME:
                continue
            if rel in seen:
                continue
            seen.add(rel)
            out.append(rel)
    print("\n".join(out))

if __name__ == "__main__":
    main()
