# For every dispatch call that passes ctx->regs[N], report the callee's declared
# parameter type at that position; flag any that is narrower than 64 bits, and
# every inline cast of ctx->regs[N] in the dispatch body.
import re, sys
src = open(sys.argv[1]).read()
start = src.index('static void syscall_dispatch_body(struct exception_context *ctx) {')
end = src.index('\n}\n', start)
body = src[start:end]
base = src[:start].count('\n') + 1
# function signatures anywhere in the file (definitions and prototypes)
sigs = {}
for m in re.finditer(r'^(?:static\s+)?(?:inline\s+)?[\w\s\*]+?\b(sys_\w+|\w+)\s*\(([^;{)]*)\)\s*[{;]', src, re.M):
    name, params = m.group(1), m.group(2)
    if name in sigs: continue
    sigs[name] = [p.strip() for p in params.split(',')]
WIDE = re.compile(r'^(?:const\s+)?(u64|s64|uintptr_t|size_t|ssize_t|uptr|long|unsigned long|void\s*\*|.*\*)\s*\w*$')
calls = 0; flagged = []
for m in re.finditer(r'\b(\w+)\s*\(((?:[^()]|\([^()]*\))*ctx->regs\[\d\](?:[^()]|\([^()]*\))*)\)', body):
    fn, args = m.group(1), m.group(2)
    if fn in ('if', 'while', 'switch', 'return'): continue
    calls += 1
    line = base + body[:m.start()].count('\n')
    argl = [a.strip() for a in re.split(r',(?![^()]*\))', args)]
    ps = sigs.get(fn)
    for i, a in enumerate(argl):
        if 'ctx->regs[' not in a: continue
        cast = re.match(r'\(\s*([\w\s]+?)\s*\)\s*ctx->regs', a)
        if cast and cast.group(1) not in ('u64', 's64', 'uintptr_t'):
            flagged.append((line, fn, i, a, 'INLINE CAST ' + cast.group(1)))
            continue
        if ps is None:
            flagged.append((line, fn, i, a, 'NO SIGNATURE FOUND')); continue
        if i >= len(ps): flagged.append((line, fn, i, a, 'ARITY?')); continue
        if not WIDE.match(ps[i]):
            flagged.append((line, fn, i, a, 'PARAM ' + ps[i]))
print('dispatch calls passing ctx->regs:', calls)
for f in flagged: print('%5d %-36s arg%d %-22s %s' % f)
print('flagged:', len(flagged))
