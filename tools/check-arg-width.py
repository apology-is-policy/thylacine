#!/usr/bin/env python3
# ERRORS.md principle 6: a syscall argument that does not fit its declared
# width is refused, never truncated. Every argument arrives as a u64 parameter;
# this check fails on any explicit cast of such a parameter to a narrower type
# unless the function first compared that parameter at full width, or the cast
# line says why it may narrow (`// width: <reason>`).
#
#   tools/check-arg-width.py [--self-test] [file ...]
#
# A raw argument is a u64 parameter, a register word read from the exception
# frame (`ctx->regs[N]`), or a u64 local copied from one. Functions named viv_*
# are exempt: they translate a Linux call, and Linux's declared C types narrow
# by construction (the phenotype is ABI shape, I-43).
# Implicit narrowing is the compiler's half: -Werror=shorten-64-to-32 (aux-3
# 30f807e0a) is not on in this tree yet, so work/oct13-width/shorten.sh is that
# census until it lands. Not seen here: a guard in another branch than the cast,
# a bound whose constant is wider than the cast, and a raw word that is not the
# first operand of the cast's expression -- review and the compiler cover those.
import re, sys

NARROW = r'(?:hidx_t|u32|s32|int|u16|s16|u8|unsigned|unsigned int|rights_t|pid_t|uid_t|gid_t|mode_t)'
WIDE = r'(?:\(\s*(?:u64|s64|long|unsigned long|uintptr_t)\s*\)\s*)?'
HELPERS = ('sys_hidx_arg', 'sys_s32_arg', 'sys_u32_arg', 'sys_word32_arg')
FN = re.compile(r'^(?:static )?(?:inline )?[\w \*]*?\b(\w+)\(([^;{)]*)\)\s*\{', re.M)
# A guard refuses: its statement returns an error or opens a block that returns.
REFUSE = r'\)\s*(?:return\s*[-(]|return\s+\w+\s*;|\{[^{}]*\breturn\b)'

def guards(p):
    q = re.escape(p)
    return [
        r'\bif\s*\([^;{}]*\b' + q + r'\s*>=?\s*(?:\(u64\)\s*)?(?:0x[0-9a-fA-F]+u?l*l*|[A-Z][A-Z0-9_]*)[^;{}]*' + REFUSE,
        r'\bif\s*\([^;{}]*\b' + q + r'\s*&\s*~\s*\(\s*u64\s*\)[^;{}]*' + REFUSE,
        r'\bif\s*\([^;{}]*\([^;()]*\b' + q + r'(?!\w)[^;()]*\)\s*>\s*0x[fF]{8}[^;{}]*' + REFUSE,
        r'\b(?:' + '|'.join(HELPERS) + r')\(\s*' + q + r'(?!\w)',
    ]

def strip_comments(text):
    # blank every comment, keeping offsets and newlines
    def blank(m):
        if m.group(0).startswith('"'):
            return m.group(0)
        return ''.join('\n' if ch == '\n' else ' ' for ch in m.group(0))
    return re.sub(r'"(?:\\.|[^"\\\n])*"|//[^\n]*|/\*.*?\*/', blank, text, flags=re.S)

def check(src, name):
    bad = []
    for m in FN.finditer(src):
        fn, params = m.group(1), m.group(2)
        if fn.startswith('viv_') or fn in HELPERS:
            continue
        ps = [x.strip().split()[-1].lstrip('*') for x in params.split(',')
              if re.match(r'\s*(?:const\s+)?u64\s+\w+\s*$', x)]
        body_start = m.end()
        end = src.find('\n}\n', body_start)
        raw = src[body_start:end if end >= 0 else len(src)]
        body = strip_comments(raw)
        regs = sorted(set(re.findall(r'\b\w+->regs\[\d+\]', body)))
        copies = re.findall(r'\bu64\s+(\w+)\s*=\s*\w+->regs\[\d+\]\s*;', body)
        ps = ps + regs + [c for c in copies if c not in ps]
        if not ps:
            continue
        # a mask held in a u64 local is a full-width mask too
        masks = re.findall(r'\bu64\s+(\w+)\s*=', body)
        base = src[:body_start].count('\n') + 1
        for p in ps:
            cast = re.compile(r'\(\s*' + NARROW + r'\s*\)\s*' + WIDE + r'\(?\s*' + re.escape(p) + r'(?!\w)')
            for c in cast.finditer(body):
                line_start = raw.rfind('\n', 0, c.start()) + 1
                line_end = raw.find('\n', c.end())
                line = raw[line_start:line_end if line_end >= 0 else len(raw)]
                above = raw[raw.rfind('\n', 0, max(line_start - 1, 0)) + 1:line_start]
                if re.search(r'^[^"]*// width:', line) or above.strip().startswith('// width:'):
                    continue
                before = body[:c.start()]
                gs = guards(p) + [r'\bif\s*\([^;{}]*\b' + re.escape(p) + r'\s*&\s*~\s*' + v + r'(?!\w)[^;{}]*' + REFUSE for v in masks]
                if any(re.search(g, before) for g in gs):
                    continue
                ln = base + body[:c.start()].count('\n')
                bad.append('%s:%d: %s narrows %s with no full-width check first' % (name, ln, fn, p))
    return bad

def self_test():
    ok = 'static s64 sys_a(u64 x) {\n    if (x > 0xffffffffull) return -1;\n    return f((u32)x);\n}\n'
    helper = 'static s64 sys_b(u64 fd) {\n    return g(sys_hidx_arg(fd));\n}\n'
    noted = 'static s64 sys_c(u64 s) {\n    int c = (int)(s & 0xff); // width: the status byte\n    return c;\n}\n'
    above = 'static s64 sys_l(u64 s) {\n    // width: the status byte\n    int c = (int)(s & 0xff);\n    return c;\n}\n'
    far = 'static s64 sys_m(u64 s) {\n    // width: the status byte\n    int k = 0;\n    int c = (int)(s & 0xff);\n    return c + k;\n}\n'
    bad = 'static s64 sys_d(u64 fd) {\n    return g((hidx_t)fd);\n}\n'
    late = 'static s64 sys_e(u64 x) {\n    u32 v = (u32)x;\n    if (x > 0xffffffffull) return -1;\n    return v;\n}\n'
    double = 'static s64 sys_f(u64 pid) {\n    return h((int)(s64)pid);\n}\n'
    viv = 'static s64 viv_g(u64 fd) {\n    return g((hidx_t)fd);\n}\n'
    comment = 'static s64 sys_h(u64 fd) {\n    // the old code did (hidx_t)fd here\n    return g(sys_hidx_arg(fd));\n}\n'
    ored = 'static s64 sys_i(u64 a, u64 b) {\n    if ((a | b) > 0xffffffffull) return -1;\n    return f((u32)a, (u32)b);\n}\n'
    u32mask = 'static s64 sys_j(u64 x) {\n    const u32 m = 7;\n    if (x & ~m) return -1;\n    return f((u32)x);\n}\n'
    u64mask = 'static s64 sys_k(u64 x) {\n    const u64 m = 7;\n    if (x & ~m) return -1;\n    return f((u32)x);\n}\n'
    regbad = 's64 sys_n(struct exception_context *ctx) {\n    return f((unsigned)ctx->regs[0]);\n}\n'
    regok = 's64 sys_o(struct exception_context *ctx) {\n    if (ctx->regs[0] > 0xffffffffull) return -1;\n    return f((unsigned)ctx->regs[0]);\n}\n'
    regother = 's64 sys_p(struct exception_context *ctx) {\n    if (ctx->regs[1] > 0xffffffffull) return -1;\n    return f((unsigned)ctx->regs[0]);\n}\n'
    regcopy = 's64 sys_q(struct exception_context *ctx) {\n    u64 fd = ctx->regs[0];\n    return g((hidx_t)fd);\n}\n'
    prefix = 'static s64 sys_r(u64 fd, u64 fd2) {\n    if (fd2 > 0xffffffffull) return -1;\n    return g((hidx_t)fd);\n}\n'
    ptrret = 'static struct Spoor *sys_s(struct Proc *p, u64 fd) {\n    return lookup(p, (hidx_t)fd);\n}\n'
    noreturn = 'static s64 sys_t(u64 x) {\n    bool w = x > 0xffffffffull;\n    return f((u32)x) + w;\n}\n'
    logonly = 'static s64 sys_u(u64 x, u64 y) {\n    if (x > 0xffffffffull || y) log_it();\n    return f((u32)x);\n}\n'
    lower = 'static s64 sys_v(u64 len) {\n    if (len >= WEFT_HYBRID_THRESHOLD) return g((u32)len);\n    return 0;\n}\n'
    strip = 'static s64 sys_w(u64 m) {\n    u64 k = m & ~(u64)FLAG;\n    return f((u32)m) + k;\n}\n'
    instr = 'static s64 sys_x(u64 x) {\n    return f("// width: lol", (u32)x);\n}\n'
    blockok = 'static s64 sys_y(u64 x) {\n    if (x > 0xffffffffull) { put(); return -1; }\n    return f((u32)x);\n}\n'
    want = {ok: 0, helper: 0, noted: 0, bad: 1, late: 1, double: 1, viv: 0,
            comment: 0, ored: 0, u32mask: 1, u64mask: 0, above: 0, far: 1,
            regbad: 1, regok: 0, regother: 1, regcopy: 1, prefix: 1,
            ptrret: 1, noreturn: 1, logonly: 1, lower: 1, strip: 1, instr: 1, blockok: 0}
    for src, n in want.items():
        got = len(check(src, 'self-test'))
        if got != n:
            print('check-arg-width self-test FAILED on:\n' + src + 'want %d, got %d' % (n, got))
            return 1
    return 0

def main(argv):
    if argv[:1] == ['--self-test']:
        return self_test()
    if self_test():
        return 1
    files = argv or ['kernel/syscall.c']
    bad = []
    for f in files:
        bad += check(open(f).read(), f)
    for b in bad:
        print(b)
    if bad:
        print('check-arg-width: %d narrowing cast(s) of a raw argument (ERRORS.md principle 6)' % len(bad))
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
