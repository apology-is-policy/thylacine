#!/usr/bin/env python3
"""Device-access form gate.

A hypervisor emulates a trapped device access from the abort's instruction
syndrome. ARM gives a valid syndrome (ESR_EL2.ISV=1) only for a load or store
of one general register with no writeback; the post-indexed, pre-indexed and
SIMD forms trap with ISV=0, and QEMU's HVF backend aborts on one (KVM returns
an error). A plain `*(volatile u32 *)p = v` leaves the form to the compiler,
which is free to pick a post-indexed store for it in a loop.
arch/arm64/mmio.h's io_read*/io_write* pin the form with a one-instruction asm
template, so device memory is reached through them only.

  A. No kernel file (arch/, kernel/ with its in-kernel tests, mm/, lib/; .c
     and .h) dereferences or indexes a cast to a volatile integer pointer, the
     idiom every old site used. The listed files do it on RAM, a pinned number
     of times; a count above or below the pin fails, so the list cannot go
     stale.
  B. Every production file that calls mmu_map_mmio includes mmio.h (the set
     is derived from the calls, so a new mapper is covered without an edit).
     A test may map a page to check the mapper and never touch it; A covers
     the tests.
  C. mmio.h defines the eight accessors, each one base-only load or store, so
     A and B cannot be satisfied by a header that no longer pins the form.

This is a gate on the idiom, not a proof: a device pointer dereferenced through
a typed variable passes it. The proof for the paths a boot reaches is a boot
under a trapping hypervisor (HVF, KVM), where a bad form is fatal and loud.

Comments and string literals are blanked before matching. --self-test plants
each shape the gate must catch and the controls it must pass.
"""
import argparse, os, re, sys, tempfile

WORD = r'(?:char|short|int|long)'
TYPE = (r'(?:u8|u16|u32|u64|s8|s16|s32|s64|uint(?:8|16|32|64|ptr)_t|int(?:8|16|32|64|ptr)_t|s?size_t|paddr_t|vaddr_t|bool|_Bool'
        r'|(?:un)?signed(?:\s+' + WORD + r')*|' + WORD + r'(?:\s+' + WORD + r')*)')
VOL = r'(?:volatile|__volatile__|__volatile)'
CAST = re.compile(
    r'\(\s*(?:const\s+)?(?:' + VOL + r'\s+(?:const\s+)?' + TYPE + r'|' + TYPE + r'\s+(?:const\s+)?' + VOL + r')'
    r'\s*(?:const\s*)?\*\s*(?:const\s*)?\)')
# RAM reached through a volatile cast, by design: file -> (sites, why).
RAM_SITES = {
    'lib/dtb.c': (1, 'the DTB blob'),
    'kernel/mmio_handle.c': (1, 'wiping a claims array on the stack'),
    'kernel/test/test_devproc.c': (10, 'hand-built stack frames'),
    'kernel/test/test_pci_irq.c': (7, 'a virtqueue in a DMA page'),
}
ROOTS = ('arch', 'kernel', 'mm', 'lib')
TESTS = 'kernel/test/'
MAPPER_SELF = ('arch/arm64/mmu.c', 'arch/arm64/mmu.h')
MMIO_H = 'arch/arm64/mmio.h'
ACCESSORS = {
    'io_read8': 'ldrb %w0, [%1]', 'io_read16': 'ldrh %w0, [%1]',
    'io_read32': 'ldr %w0, [%1]', 'io_read64': 'ldr %0, [%1]',
    'io_write8': 'strb %w0, [%1]', 'io_write16': 'strh %w0, [%1]',
    'io_write32': 'str %w0, [%1]', 'io_write64': 'str %0, [%1]',
}


def blank(src, keep_strings=False):
    """Replace comments (and string/char literals) with spaces, offsets kept."""
    out, i, n = list(src), 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2); j = n if j < 0 else j + 2
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            if keep_strings:
                i = j; continue
        else:
            i += 1; continue
        for k in range(i, j):
            if out[k] != '\n': out[k] = ' '
        i = j
    return ''.join(out)


def sources(root):
    for top in ROOTS:
        for d, _, files in os.walk(os.path.join(root, top)):
            for f in sorted(files):
                if not f.endswith(('.c', '.h')): continue
                yield os.path.relpath(os.path.join(d, f), root)


def derefs(code):
    """Offsets of casts to a volatile integer pointer that are dereferenced:
    `*(T *)e`, or `((T *)e)[i]`."""
    out = []
    for m in CAST.finditer(code):
        before = code[:m.start()].rstrip()
        if before.endswith('*'):
            out.append(m.start()); continue
        if before.endswith('('):
            depth, j = 1, m.end()
            while j < len(code) and depth:
                depth += {'(': 1, ')': -1}.get(code[j], 0); j += 1
            if not depth and code[j:].lstrip().startswith('['):
                out.append(m.start())
    return out


def check(root):
    errs, seen = [], {}
    for rel in sources(root):
        raw = open(os.path.join(root, rel), encoding='utf-8', errors='replace').read()
        code = blank(raw)
        hits = [raw.count('\n', 0, at) + 1 for at in derefs(code)]
        if hits: seen[rel] = hits
        if rel not in MAPPER_SELF and not rel.startswith(TESTS) and re.search(r'\bmmu_map_mmio\s*\(', code) \
                and not re.search(r'#\s*include\s*"(?:[^"]*/)?mmio\.h"', blank(raw, keep_strings=True)):
            errs.append(f'{rel}: maps device memory (mmu_map_mmio) without including mmio.h')
    for rel, lines in sorted(seen.items()):
        want = RAM_SITES.get(rel, (0, ''))[0]
        if len(lines) > want:
            errs.append(f'{rel}:{",".join(map(str, lines))}: {len(lines)} volatile cast-dereference(s), '
                        f'{want} allowed -- reach device memory through io_read*/io_write* (arch/arm64/mmio.h); '
                        f'for ordinary memory, pin the site in RAM_SITES')
    for rel, (want, why) in sorted(RAM_SITES.items()):
        have = len(seen.get(rel, []))
        if have < want:
            errs.append(f'{rel}: allowance of {want} ({why}) but {have} found -- lower the pin in RAM_SITES')
    path = os.path.join(root, MMIO_H)
    if not os.path.exists(path):
        errs.append(f'{MMIO_H}: missing')
    else:
        hdr = blank(open(path).read(), keep_strings=True)
        for name, insn in ACCESSORS.items():
            m = re.search(r'\b' + name + r'\s*\([^)]*\)\s*\{(.*?)\n\}', hdr, re.S)
            body = m.group(1) if m else ''
            templates = re.findall(r'__asm__\s+volatile\s*\(\s*"([^"]*)"', body)
            if templates != [insn]:
                errs.append(f'{MMIO_H}: {name} must be the single instruction "{insn}", found {templates}')
    return errs


def self_test():
    good_h = 'static inline int f(void);\n' + ''.join(
        f'static inline void {n}(void) {{\n    __asm__ volatile("{i}" :: "r"(0));\n}}\n' for n, i in ACCESSORS.items())
    base = {
        MMIO_H: good_h,
        'lib/dtb.c': 'u32 q(const void *p) { return *(const volatile uint32_t *)p; }\n',
        'kernel/mmio_handle.c': 'void z(char *c) { ((volatile u8 *)c)[0] = 0; }\n',
        'kernel/test/test_devproc.c': 'void t(u64 *m) { ' + '*(volatile u64 *)m = 1; ' * 10 + '}\n',
        'kernel/test/test_pci_irq.c': 'void t(u64 *m) { ' + '*(volatile u16 *)m = 1; ' * 7 + '}\n',
        'kernel/test/test_map.c': 'void t(void) { void *k = mmu_map_mmio(0, 1); (void)k; }\n',
        'arch/arm64/mmu.c': 'void *mmu_map_mmio(u64 pa, u64 n) { return 0; }\n',
        'arch/arm64/mmu.h': 'void *mmu_map_mmio(u64 pa, u64 n);\n',
        'arch/arm64/ok.c': '#include "mmio.h"\n// *(volatile u32 *)x = 1;\n/* ((volatile u32 *)x)[1] = 1;\n   *(volatile u32 *)x = 1; */\n'
                           'const char *s = "*(volatile u32 *)";\n'
                           'void g(void) { void *k = mmu_map_mmio(0, 1); io_write32(k, 1); io_write8((volatile u8 *)k + 4, 1); }\n'
                           'static volatile uintptr_t cached;\nvolatile u8 *holder;\n',
    }
    DEREF, MAPPER, STALE, HDR = 'cast-dereference', 'without including mmio.h', 'lower the pin', 'must be the single instruction'
    cases = [
        ('clean tree', {}, []),
        ('the old GIC idiom', {'arch/arm64/gic.c': 'void w(u64 b) { *(volatile u32 *)(uintptr_t)(b + 4) = 1; }\n'}, [DEREF]),
        ('cast then index', {'arch/arm64/gic.c': 'void w(u64 b) { ((volatile u32 *)b)[1] = 1; }\n'}, [DEREF]),
        ('cast then index, nested parens', {'arch/arm64/gic.c': 'void w(u64 b) { ((volatile u32 *)(b + f(1)))[1] = 1; }\n'}, [DEREF]),
        ('split across lines', {'kernel/v.c': 'u32 r(void *b) { return *\n  (volatile\n   u32 *)b; }\n'}, [DEREF]),
        ('const volatile stdint', {'kernel/v.c': 'u32 r(void *b) { return *(const volatile uint32_t *)b; }\n'}, [DEREF]),
        ('type-first spelling', {'mm/v.c': 'u8 r(void *b) { return *(u8 volatile *)b; }\n'}, [DEREF]),
        ('size_t', {'mm/v.c': 'size_t r(void *b) { return *(volatile size_t *)b; }\n'}, [DEREF]),
        ('long long', {'mm/v.c': 'long long r(void *b) { return *(volatile long long *)b; }\n'}, [DEREF]),
        ('__volatile__', {'mm/v.c': 'u32 r(void *b) { return *(__volatile__ u32 *)b; }\n'}, [DEREF]),
        ('const after the star', {'mm/v.c': 'u32 r(void *b) { return *(volatile u32 *const)b; }\n'}, [DEREF]),
        ('in a header inline', {'kernel/include/x.h': 'static inline u16 r(void *b) { return *(volatile u16 *)b; }\n'}, [DEREF]),
        ('in a test with no allowance', {'kernel/test/test_x.c': 'void t(u64 b) { *(volatile u32 *)b = 1; }\n'}, [DEREF]),
        ('one more in an allowed file', {'lib/dtb.c': base['lib/dtb.c'] * 2}, [DEREF]),
        ('allowance gone stale', {'lib/dtb.c': 'int z;\n'}, [STALE]),
        ('a mapper without mmio.h', {'kernel/m.c': 'void g(void) { void *k = mmu_map_mmio(0, 1); (void)k; }\n'}, [MAPPER]),
        ('include only in a comment', {'kernel/m.c': '// #include "mmio.h"\nvoid g(void) { mmu_map_mmio(0, 1); }\n'}, [MAPPER]),
        ('include by relative path', {'kernel/m.c': '#include "../arch/arm64/mmio.h"\nvoid g(void) { mmu_map_mmio(0, 1); }\n'}, []),
        ('a cast passed on, never dereferenced', {'kernel/v.c': 'void g(void *k) { io_write32((volatile u32 *)k + 3, 1); }\n'}, []),
        ('writeback template in mmio.h', {MMIO_H: good_h.replace('"str %w0, [%1]"', '"str %w0, [%1], #4"')}, [HDR]),
        ('accessor back to a volatile deref', {MMIO_H: good_h.replace('__asm__ volatile("ldr %w0, [%1]" :: "r"(0));', 'return;')}, [HDR]),
        ('mmio.h missing', {MMIO_H: None}, ['missing']),
    ]
    bad = 0
    for name, over, want in cases:
        with tempfile.TemporaryDirectory() as root:
            tree = dict(base); tree.update(over)
            for rel, text in tree.items():
                if text is None: continue
                os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
                open(os.path.join(root, rel), 'w').write(text)
            got = check(root)
        if len(got) != len(want) or any(w not in g for w, g in zip(want, got)):
            print(f'self-test FAILED: {name}: expected {want}, got {got}', file=sys.stderr); bad += 1
    if bad: return 1
    print(f'check-mmio-forms self-test: {len(cases)} cases as expected')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test: return self_test()
    errs = check(os.path.abspath(a.root))
    for e in errs: print('check-mmio-forms: ' + e, file=sys.stderr)
    if errs: return 1
    print('check-mmio-forms: device memory is reached through mmio.h only')
    return 0


if __name__ == '__main__':
    sys.exit(main())
