#!/usr/bin/env python3
# Census: syscall handlers that narrow a u64 argument register by a cast.
# For each handler (static s64 sys_*(u64 a, u64 b, ...)) list each parameter
# cast to a narrower type, and whether the handler compares that parameter
# against a bound anywhere (a guard), before or after the cast.
import re, sys
src = open(sys.argv[1]).read()
NARROW = r'(hidx_t|u32|s32|int|u16|s16|u8|unsigned|unsigned int|rights_t|pid_t|mode_t|uid_t|gid_t)'
fns = list(re.finditer(r'^(?:static )?(?:s64|void|u64) (\w+)\(([^)]*)\)\s*\{', src, re.M))
rows = []
for i, m in enumerate(fns):
    name, params = m.group(1), m.group(2)
    end = fns[i+1].start() if i + 1 < len(fns) else len(src)
    body = src[m.end():end]
    # stop at the end of the function: first line that is exactly "}"
    body = body.split('\n}\n', 1)[0]
    ps = [p.strip().split()[-1] for p in params.split(',') if p.strip().startswith('u64')]
    for p in ps:
        casts = re.findall(r'\(\s*' + NARROW + r'\s*\)\s*(?:\(\s*(?:u64|s64|long|unsigned long|uintptr_t)\s*\)\s*)?\(?\s*' + re.escape(p) + r'\b', body)
        if not casts: continue
        guard = None
        for cond in re.findall(r'\bif\s*\((.*?)\)\s*(?:return|\{|$)', body, re.M | re.S):
            if re.search(r'\b' + re.escape(p) + r'\b', cond) and re.search(r'[<>]', cond):
                guard = cond.strip().replace('\n', ' ')[:70]; break
        line = src[:m.start()].count('\n') + 1
        rows.append((line, name, p, sorted(set(casts)), guard))
for r in rows:
    print('%5d %-40s %-18s %-22s %s' % (r[0], r[1], r[2], ','.join(r[3]), ('GUARDED: ' + r[4]) if r[4] else 'UNGUARDED'))
print('handlers:', len(fns), 'narrowing params:', len(rows),
      'unguarded:', sum(1 for r in rows if not r[4]))
print('note: a GUARDED bound must still be checked to be a full-width one')
