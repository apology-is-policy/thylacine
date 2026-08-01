#!/usr/bin/env python3
"""Guard the ARMv8.0-A userspace floor (task #91, closing #71).

PORTABILITY.md section 3 binds the whole system to the ARMv8.0-A baseline --
"the only baseline that runs on every row", including the Cortex-A72 in the
RPi 400 named as the first bare-metal board. Lazarus W1 moved the KERNEL to
that floor; the userspace toolchains kept `+lse` for months, so on an A72 the
kernel booted 1209/1209 and then every allocating userspace binary died with
snare:ill on a `casalb` (#71). Nothing checked, so nothing noticed.

TWO INDEPENDENT CHECKS, because they fail differently:

  source    No tracked build input asks for a baseline above the floor.
            Cheap, exact, and it names the offending FILE -- but it can only
            see inputs it knows to look for.

  binaries  No shipped aarch64 binary carries an LSE instruction that a
            runtime check cannot skip. This is the one that matters. #71's
            own postmortem is explicit about why:

                "tools/pouch-clang was NOT in my first enumeration
                 (cmake + build.sh + cargo) -- it was found only because
                 measuring the OUTPUT left 2 ungated instructions in
                 pouch-hello-exitgroup. Enumerating the files you expect is
                 not the same as measuring what shipped."

            Since W1u landed, three more -march sites have appeared
            (clade-mesa-cross.sh, joey.c, lsp-probe) -- exactly that mode,
            three more times. The source check would have caught those only
            because it greps every tracked file rather than a list; it still
            cannot see a vendored .S, a prebuilt archive, or a dependency
            that ships its own flags. The binary check can.

WHAT "GATED" MEANS, precisely. Both LSE producers in this tree emit the same
shape: load a feature byte, then conditionally branch OVER the LSE.

    outline-atomics (compiler-rt, all C/C++/Rust):
        ldrb w16, [x16, #0x588]     <- __aarch64_have_lse_atomics
        cbz  w16, .Lllsc            <- skip the LSE
        casalb w0, w1, [x2]
    Go runtime (GOARM64=v8.0, the default):
        ldrb w2, [x27, #0x357]      <- runtime.arm64HasATOMICS
        tbz  w2, #0x0, .Lllsc       <- skip the LSE
        swpalb w2, w3, [x0]

So one structural rule covers both: an LSE is gated iff a nearby preceding
feature-byte load is paired with a conditional branch whose target lies past
the LSE. This is deliberately NOT symbol-based -- the shipped clade toolchain
(clang/clang++/lld/clangd, baked to /clade) is fully STRIPPED, so there are no
symbol names to allowlist. It is also uniform: Go and outline-atomics are
checked by the same rule rather than by two special cases.

SCOPE: userspace only. The kernel is at the floor too, but reaches it a
different way -- the W1.5 boot patcher rewrites LL/SC into LSE in place, so
kernel LSE lives in .altinstr_replacement with no branch before it. It is
gated by apply_alternatives, not by a runtime test, and this checker would
correctly call it ungated. Do not point --binaries at the kernel ELF.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --- what counts as an LSE instruction -------------------------------------
# ARMv8.1-A atomic memory operations. Anchored, so the LL/SC forms this tree
# runs on a v8.0 core (ldaxr / stlxr / ldar / stlr) do not match.
LSE_RE = re.compile(
    r'^(?:'
    r'casp?|'                                          # cas, casp (+a/l/al, +b/h)
    r'swp|'                                            # swp
    r'ld(?:add|clr|eor|set|smax|smin|umax|umin)|'      # load-op
    r'st(?:add|clr|eor|set|smax|smin|umax|umin)'       # store-op
    r')(?:a|l|al)?(?:b|h)?$'
)

# A conditional branch that can skip forward over the LSE.
COND_BR_RE = re.compile(r'^(?:cbz|cbnz|tbz|tbnz)$')

# The feature-byte load. Both producers use ldrb; ldrsb is accepted because it
# is the same load with a different extension and costs nothing to allow.
FLAG_LD_RE = re.compile(r'^ldr[s]?b$')

# objdump -d output.
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)(?:\s+(.*))?$')
SYM_RE = re.compile(r'^[0-9a-f]+\s+<(.+)>:$')
# A standalone hex token, with or without the 0x prefix: LLVM objdump prints
# "cbz w16, 0x403e34", GNU binutils prints "cbz w16, 403e34". The token
# boundary is load-bearing -- a bare [0-9a-f]+ would read the "16" out of
# "w16" and the "2" out of "[x2]". Requiring the match to start at a
# separator also drops the "#0" bit index in "tbz w2, #0, 2107c".
TARGET_RE = re.compile(r'(?:^|[\s,])(?:0x)?([0-9a-f]+)(?=$|[\s,])')
# objdump annotates a branch target with the symbol it lands in:
#     cbz w16, 0x403e34 <__aarch64_cas1_acq+0x14>
# That trailing "+0x14" is a hex literal too, so the annotation must be
# stripped before the target is read out -- taking the last hex match on the
# raw string yields 0x14 and silently mis-reads every branch in the tree.
SYMSUFFIX_RE = re.compile(r'<[^>]*>')

# How far back to look for the gate. Observed distance is 1-2 instructions
# (branch at -1, flag load at -2); 8 is generous without being so wide that an
# unrelated forward branch is likely to sit inside it.
GATE_WINDOW = 8


def is_aarch64_elf(path):
    try:
        if os.path.islink(path) or not os.path.isfile(path):
            return False
        with open(path, 'rb') as f:
            hdr = f.read(20)
    except OSError:
        return False
    if len(hdr) < 20 or hdr[:4] != b'\x7fELF':
        return False
    return hdr[18] | (hdr[19] << 8) == 183  # EM_AARCH64


def classify(lines):
    """Walk disassembly; return (n_lse, [ungated...]).

    An ungated entry is (addr, mnemonic, symbol).
    """
    insns = []     # (addr, mnem, ops, symbol)
    cur_sym = '<none>'
    for line in lines:
        m = SYM_RE.match(line)
        if m:
            cur_sym = m.group(1)
            continue
        m = INSN_RE.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2),
                          m.group(3) or '', cur_sym))

    n_lse = 0
    ungated = []
    for i, (addr, mnem, _ops, sym) in enumerate(insns):
        if not LSE_RE.match(mnem):
            continue
        n_lse += 1
        lo = max(0, i - GATE_WINDOW)
        window = insns[lo:i]
        saw_flag_load = any(FLAG_LD_RE.match(w[1]) for w in window)
        skips_lse = False
        for _waddr, wmnem, wops, _wsym in window:
            if not COND_BR_RE.match(wmnem):
                continue
            # The target is the last hex operand once the <sym+0xNN>
            # annotation is gone. `tbz w2, #0x0, 0x2107c` also carries a
            # leading #0x0 bit index, so first-match is wrong too.
            targets = TARGET_RE.findall(SYMSUFFIX_RE.sub('', wops))
            if targets and int(targets[-1], 16) > addr:
                skips_lse = True
                break
        if not (saw_flag_load and skips_lse):
            ungated.append((addr, mnem, sym))
    return n_lse, ungated


def scan_one(path, objdump):
    try:
        r = subprocess.run([objdump, '-d', path], capture_output=True,
                           text=True, timeout=1800)
    except Exception as e:                                   # noqa: BLE001
        return path, None, f'objdump failed: {e}'
    if r.returncode != 0:
        return path, None, f'objdump rc={r.returncode}: {r.stderr.strip()[:200]}'
    return path, classify(r.stdout.splitlines()), None


# --- the source check ------------------------------------------------------
FLOOR_MARCH = 'armv8-a'


def check_source(root):
    """Every -march/-mcpu in a tracked file must be the bare floor; no rust
    target-feature may request +lse; nothing may set GOARM64 above v8.0."""
    problems = []
    files = subprocess.run(['git', 'ls-files'], cwd=root, capture_output=True,
                           text=True, check=True).stdout.split()
    scanned = 0
    march_sites = 0
    for rel in files:
        if rel.startswith('third_party/') or rel.endswith('.md'):
            continue
        p = os.path.join(root, rel)
        try:
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                text = f.read()
        except (OSError, UnicodeError):
            continue
        if ('-march=' not in text and '-mcpu=' not in text
                and 'target-feature' not in text and 'GOARM64' not in text):
            continue
        scanned += 1
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            # Comment lines discuss the flags constantly; they set nothing.
            if stripped.startswith(('#', '//', '*', '/*')):
                continue
            for m in re.finditer(r'-m(?:arch|cpu)=([A-Za-z0-9.+_-]+)', line):
                march_sites += 1
                if m.group(1) != FLOOR_MARCH:
                    problems.append(
                        f'{rel}:{lineno}: -m{"arch" if "arch" in m.group(0) else "cpu"}'
                        f'={m.group(1)} is above the {FLOOR_MARCH} floor')
            for m in re.finditer(r'target-feature=([A-Za-z0-9,+_-]+)', line):
                feats = m.group(1)
                if '+lse' in feats:
                    problems.append(
                        f'{rel}:{lineno}: rust target-feature carries +lse')
            m = re.search(r'GOARM64\s*=\s*["\']?v?([0-9.]+)', line)
            if m and m.group(1) not in ('8.0',):
                problems.append(
                    f'{rel}:{lineno}: GOARM64={m.group(1)} is above v8.0')
    return problems, scanned, march_sites


# --- the positive control --------------------------------------------------
# ONE object carrying BOTH shapes, so the control proves DISCRIMINATION rather
# than mere detection. The first version of this selftest used a C TU built
# with -moutline-atomics as its "clean" leg -- which passed vacuously, because
# such an object contains ZERO LSE (the helpers live in compiler-rt, linked in
# later). "Not flagged" said nothing about whether a gate is recognised, and
# it hid a real target-parsing bug that mis-read every branch in the tree.
# A clean leg has to contain a gated LSE that the checker must ACCEPT.
SELFTEST_S = """
    .text
    .arch armv8-a
    .arch_extension lse

    // Ungated: a bare LSE with no runtime test anywhere near it. This is
    // exactly what -march=armv8-a+lse emitted tree-wide during #71.
    .globl  thyla_selftest_ungated
thyla_selftest_ungated:
    casalb  w0, w1, [x2]
    ret

    // Gated: the shape both real producers emit -- load the feature byte,
    // conditionally branch OVER the LSE, fall through to an LL/SC path.
    .globl  thyla_selftest_gated
thyla_selftest_gated:
    ldrb    w16, [x3]
    cbz     w16, 1f
    casalb  w0, w1, [x2]
    ret
1:  ldaxrb  w0, [x2]
    stlxrb  w17, w1, [x2]
    ret
"""


def selftest(clang, objdump):
    """Require the checker to flag an ungated LSE and to accept a gated one.

    Without this the whole scan is satisfiable by a broken system: a wrong
    objdump path, an ELF filter that matches nothing, or a mnemonic regex that
    matches nothing all yield "0 ungated" -- indistinguishable from a clean
    tree. And without the gated leg, a checker that flags EVERYTHING also
    looks like it works until it red-lights a correct build.
    """
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'sel.S')
        with open(src, 'w') as f:
            f.write(SELFTEST_S)
        obj = os.path.join(td, 'sel.o')
        r = subprocess.run([clang, '--target=aarch64-unknown-none',
                            '-march=armv8-a', '-c', src, '-o', obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return [f'selftest: could not assemble the fixture: '
                    f'{r.stderr.strip()[:200]}']
        _p, res, err = scan_one(obj, objdump)
        if err:
            return [f'selftest: {err}']
        n_lse, ungated = res
        if n_lse != 2:
            return [f'selftest: fixture should carry exactly 2 LSE '
                    f'instructions, the checker saw {n_lse} -- the mnemonic '
                    'regex or the disassembler is broken']
        flagged = {sym for _a, _m, sym in ungated}
        if not any('ungated' in s for s in flagged):
            return ['selftest: the UNGATED fixture instruction was not '
                    'flagged -- the gate detector accepts everything and '
                    'would pass a real #71']
        if any('gated' in s and 'ungated' not in s for s in flagged):
            return ['selftest: the GATED fixture instruction was flagged -- '
                    'the gate detector rejects correct code and would red-'
                    'light every clean build']
    return []


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=root)
    ap.add_argument('--binaries', nargs='*', default=None,
                    help='dirs/files to scan (default: build/ramfs-src)')
    ap.add_argument('--all', action='store_true',
                    help='also scan the big pool payloads -- /clade, /goroot, '
                         '/storm, /quake. Correct but slow: the five ~100 MB '
                         'clade LLVM binaries dominate (~6 min vs ~8 s).')
    ap.add_argument('--source-only', action='store_true')
    ap.add_argument('--objdump', default=None,
                    help='disassembler (default: llvm-objdump under '
                         'LLVM_PREFIX, else whatever objdump is on PATH)')
    ap.add_argument('--clang',
                    default=os.environ.get('SELFTEST_CLANG',
                                           '/opt/homebrew/opt/llvm/bin/clang'))
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) - 1))
    args = ap.parse_args()

    # Prefer the build's own llvm-objdump: it is the one guaranteed to
    # understand these ELFs, and it keeps macOS (LLVM objdump) and the Linux
    # builder (GNU binutils on PATH) on the same output dialect. The two
    # dialects differ -- GNU omits the 0x on branch targets -- which the
    # selftest would catch, but agreeing up front is better than catching it.
    if args.objdump is None:
        args.objdump = os.environ.get('OBJDUMP') or ''
        if not args.objdump:
            cand = os.path.join(os.environ.get('LLVM_PREFIX',
                                               '/opt/homebrew/opt/llvm'),
                                'bin', 'llvm-objdump')
            args.objdump = (cand if os.path.exists(cand)
                            else (shutil.which('llvm-objdump') or 'objdump'))

    failures = []

    # ---- source ----
    problems, nfiles, nsites = check_source(args.root)
    if problems:
        print('FAIL  source: build inputs above the ARMv8.0 floor')
        for p in problems:
            print(f'        {p}')
        failures.extend(problems)
    else:
        print(f'ok    source: {nsites} -march/-mcpu sites across {nfiles} '
              f'files, all at the {FLOOR_MARCH} floor')
    if nsites == 0:
        msg = ('source: found no -march site at all -- the scan is not '
               'looking where the flags live')
        print(f'FAIL  {msg}')
        failures.append(msg)

    if args.source_only:
        return 1 if failures else 0

    # ---- positive control, before trusting any binary result ----
    if not os.path.exists(args.clang):
        alt = shutil.which('clang')
        if alt:
            args.clang = alt
    st = selftest(args.clang, args.objdump)
    if st:
        for s in st:
            print(f'FAIL  {s}')
        failures.extend(st)
        print('      (the binary scan below cannot be trusted; fix the '
              'control first)')
    else:
        print('ok    selftest: the ungated LSE in the fixture is flagged, '
              'the gated one is not')

    # ---- binaries ----
    dirs = args.binaries
    if dirs is None:
        b = os.path.join(args.root, 'build')
        # ramfs-src is every binary the boot chain runs, and it is cheap.
        cand = [os.path.join(b, 'ramfs-src')]
        if args.all:
            cand += [os.path.join(b, 'clade', 'stage'),
                     os.path.join(b, 'go', 'goroot'),
                     os.path.join(b, 'storm', 'stage'),
                     os.path.join(b, 'quake', 'stage')]
        dirs = [d for d in cand if os.path.isdir(d)]
    files = []
    for d in dirs:
        if os.path.isfile(d):
            files.append(d)
            continue
        for dp, _, fns in os.walk(d):
            for fn in fns:
                p = os.path.join(dp, fn)
                if is_aarch64_elf(p):
                    files.append(p)
    files.sort()

    if not files:
        msg = ('binaries: no aarch64 ELF found in ' + ', '.join(dirs or ['(none)'])
               + ' -- build first, or the scan is looking in the wrong place')
        print(f'FAIL  {msg}')
        failures.append(msg)
        return 1

    total_lse = 0
    bad = []
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for path, res, err in ex.map(lambda p: scan_one(p, args.objdump), files):
            if err:
                errors.append(f'{path}: {err}')
                continue
            n_lse, ungated = res
            total_lse += n_lse
            if ungated:
                bad.append((path, n_lse, ungated))

    for e in errors:
        print(f'FAIL  binaries: {e}')
    failures.extend(errors)

    if total_lse == 0:
        msg = (f'binaries: scanned {len(files)} ELFs and found ZERO LSE '
               'instructions -- this tree links outline-atomics helpers and '
               'Go, so zero means the scan silently did nothing')
        print(f'FAIL  {msg}')
        failures.append(msg)

    if bad:
        print(f'FAIL  binaries: ungated LSE in {len(bad)} of {len(files)} ELFs')
        for path, n_lse, ungated in sorted(bad, key=lambda t: -len(t[2])):
            rel = os.path.relpath(path, args.root)
            print(f'        {rel}: {len(ungated)} ungated of {n_lse} LSE')
            for addr, mnem, sym in ungated[:5]:
                print(f'            {addr:#x}  {mnem:<10} in <{sym}>')
            if len(ungated) > 5:
                print(f'            ... and {len(ungated) - 5} more')
        failures.append('ungated LSE')
    else:
        print(f'ok    binaries: {len(files)} aarch64 ELFs, {total_lse} LSE '
              'instructions, all runtime-gated')

    if failures:
        print('\nv8.0 floor: FAIL -- see PORTABILITY.md section 3 and task #91')
        return 1
    print('\nv8.0 floor: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
