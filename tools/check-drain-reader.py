#!/usr/bin/env python3
"""check-drain-reader.py -- joey's stratumd drain reader can reach no system call but its own four.

joey's #370 drainer is two threads: a reader that copies stratumd's pipe into a
ring, and a printer that writes the ring to the console. A console write parks
while an IM-1 episode is open, so the reader must never make one: a parked
reader stops reading, and stratumd blocks on its full pipe for the episode's
length (measured: 15 s of a 20 s episode). joey's boot selftest cannot see a
reader that writes to the real console -- it runs before joey gives the console
up, when no episode can freeze joey -- so this reads the BUILT joey instead:
every function reachable from sd_reader_main by direct call or tail call, and
every system call in them, against an allowlist by NAME whose numbers come from
the kernel's syscall enum. A call's number is the constant moved into w8/x8 in
the straight run of instructions before its svc: nothing in between may name
x8 or w8, transfer control, or be a branch target. It fails closed: an indirect
call or branch, a branch into the middle of another function, a system call
whose number is not such a constant, a symbol it cannot find, or a reader in
which it finds no poll and no read (then it is not reading the reader).

Usage: check-drain-reader.py [--root ROOT] [--elf PATH] [--objdump PATH]
       check-drain-reader.py --self-test
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ENTRY = 'sd_reader_main'
# SYS_EXITS ends joey and never returns, so it parks nothing: libt's __stack_chk_fail,
# which any function with a canary can reach, calls it.
ALLOWED = ('SYS_POLL', 'SYS_READ', 'SYS_TORPOR_WAKE', 'SYS_THREAD_EXIT', 'SYS_EXITS')
REQUIRED = ('SYS_POLL', 'SYS_READ')

FUNC_RE = re.compile(r'^[0-9a-f]+ <([^>]+)>:\s*$')
INSN_RE = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$')
TARGET_RE = re.compile(r'<([^>+]+)(\+0x[0-9a-f]+)?>')
MOV_X8_RE = re.compile(r'^[wx]8,\s*#(-?0x[0-9a-f]+|-?\d+)')
NAMES_X8 = re.compile(r'\b[wx]8\b')
ADDR_RE = re.compile(r'\b0x([0-9a-f]+)\s+<')
# Anything that transfers control ends the straight run back from an svc; a call also
# clobbers x8.
FLOW = re.compile(r'^(b|bl|br|blr\w*|br\w*|ret\w*|b\.\w+|cbz|cbnz|tbz|tbnz|svc|hvc|smc|eret\w*|brk|udf)$')
RUN_MAX = 16
INDIRECT = re.compile(r'^(blr|br)(aa|ab|aaz|abz)?$')
COND = re.compile(r'^(b\.\w+|cbz|cbnz|tbz|tbnz)$')


def syscall_numbers(root):
    """name -> number, from the kernel's enum (kernel/include/thylacine/syscall.h)."""
    path = os.path.join(root, 'kernel', 'include', 'thylacine', 'syscall.h')
    nums = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = re.match(r'\s*(SYS_[A-Z0-9_]+)\s*=\s*(\d+)\s*,', line)
            if m:
                nums[m.group(1)] = int(m.group(2))
    return nums


def parse_functions(text):
    funcs, cur = {}, None
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs


def svc_number(insns, i, targets):
    """(number, None) for the svc at insns[i], or (None, why)."""
    j = i - 1
    while j >= 0 and i - j <= RUN_MAX:
        addr, mn, ops = insns[j]
        if insns[j + 1][0] in targets:
            return None, 'a branch lands between its number and it (at 0x%x)' % insns[j + 1][0]
        if FLOW.match(mn):
            return None, 'control flow before it with no number loaded (%s %s)' % (mn, ops)
        if NAMES_X8.search(ops):
            m = MOV_X8_RE.match(ops) if mn in ('mov', 'movz') else None
            if not m:
                return None, 'x8 last set by %s %s' % (mn, ops)
            return int(m.group(1), 0), None
        j -= 1
    return None, 'no constant number in the %d instructions before it' % RUN_MAX


def check(text, nums):
    """Returns (problems, reached functions, syscall names seen)."""
    by_num = {v: k for k, v in nums.items()}
    missing = [n for n in ALLOWED if n not in nums]
    if missing:
        return ['the kernel enum lacks %s' % ', '.join(missing)], [], set()
    allowed = {nums[n] for n in ALLOWED}
    funcs = parse_functions(text)
    if ENTRY not in funcs:
        return ['%s is not in the disassembly' % ENTRY], [], set()
    problems, seen, todo, calls = [], [], [ENTRY], set()
    while todo:
        fn = todo.pop()
        if fn in seen:
            continue
        if fn not in funcs:
            problems.append('%s is called but not in the disassembly' % fn)
            continue
        seen.append(fn)
        insns = funcs[fn]
        targets = set()
        for _, mn, ops in insns:
            if mn == 'bl' or mn == 'b' or COND.match(mn):
                t, a = TARGET_RE.search(ops), ADDR_RE.search(ops)
                if t and a and t.group(1) == fn:
                    targets.add(int(a.group(1), 16))
        for i, (_, mn, ops) in enumerate(insns):
            if INDIRECT.match(mn):
                problems.append('%s: indirect %s %s' % (fn, mn, ops))
            elif mn in ('hvc', 'smc'):
                problems.append('%s: %s' % (fn, mn))
            elif mn == 'svc':
                n, why = svc_number(insns, i, targets)
                if n is None:
                    problems.append('%s: svc at 0x%x whose number is not known: %s'
                                    % (fn, insns[i][0], why))
                    continue
                name = by_num.get(n, 'number %d' % n)
                calls.add(name)
                if n not in allowed:
                    problems.append('%s: system call %s' % (fn, name))
            elif mn == 'bl' or mn == 'b' or COND.match(mn):
                t = TARGET_RE.search(ops)
                if not t:
                    if mn == 'bl':
                        problems.append('%s: bl with no symbol (%s)' % (fn, ops))
                    continue
                sym, off = t.group(1), t.group(2)
                if sym == fn:
                    continue
                if off:
                    problems.append('%s: %s into the middle of %s' % (fn, mn, sym + off))
                    continue
                todo.append(sym)
    for r in REQUIRED:
        if r not in calls:
            problems.append('no %s reachable from %s -- this is not the reader' % (r, ENTRY))
    return problems, seen, calls


def run_objdump(objdump, elf):
    r = subprocess.run([objdump, '-d', '--no-show-raw-insn', elf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('objdump rc=%d: %s' % (r.returncode, r.stderr.strip()[:200]))
    return r.stdout


def selftest():
    nums = {'SYS_EXITS': 2, 'SYS_PUTS': 1, 'SYS_READ': 9, 'SYS_POLL': 29, 'SYS_TORPOR_WAIT': 39,
            'SYS_TORPOR_WAKE': 40, 'SYS_THREAD_EXIT': 42}

    def fn(name, base, body):
        return '%016x <%s>:\n%s\n' % (base, name, '\n'.join(
            '  %x:      \t%s' % (base + 4 * i, ins) for i, ins in enumerate(body)))

    good = (fn(ENTRY, 0x400000, ['mov\tw8, #0x1d             // =29', 'svc\t#0',
                                 'bl\t0x400100 <helper>', 'b.le\t0x400000 <sd_reader_main>',
                                 'mov\tw8, #0x9                // =9', 'mov\tx1, x21',
                                 'mov\tx2, #0x100', 'svc\t#0', 'ret'])
            + fn('helper', 0x400100, ['mov\tw8, #0x28', 'svc\t#0', 'b\t0x400200 <tail>'])
            + fn('tail', 0x400200, ['mov\tw8, #0x2a', 'svc\t#0']))
    cases = [
        ('the reader as built', good, None),
        ('a console write', good.replace('mov\tw8, #0x28', 'mov\tw8, #0x1'), 'system call SYS_PUTS'),
        ('a wait on a lock word', good.replace('mov\tw8, #0x2a', 'mov\tw8, #0x27'), 'system call SYS_TORPOR_WAIT'),
        ('an indirect call', good.replace('bl\t0x400100 <helper>', 'blr\tx8'), 'indirect blr'),
        ('an authenticated indirect call', good.replace('bl\t0x400100 <helper>', 'blraaz\tx8'), 'indirect blraaz'),
        ('a number loaded from memory', good.replace('mov\tw8, #0x28', 'ldr\tx8, [x0]'), 'x8 last set by ldr'),
        ('x8 changed after its constant', good.replace('mov\tx1, x21', 'add\tx8, x8, #0x1'), 'x8 last set by add'),
        ('a call between number and svc', good.replace('mov\tx1, x21', 'bl\t0x400100 <helper>'), 'control flow before it'),
        ('a branch landing in between', good.replace('b.le\t0x400000 <sd_reader_main>',
                                                         'b.le\t0x400018 <sd_reader_main+0x18>'),
         'a branch lands between'),
        ('an svc with no number at all', good.replace('mov\tw8, #0x1d             // =29', 'nop'), 'no constant number'),
        ('a callee not in the disassembly', good.replace('bl\t0x400100 <helper>', 'bl\t0x400100 <gone>'), 'gone is called but not'),
        ('a branch into another function', good.replace('0x400200 <tail>', '0x400208 <tail+0x8>'), 'into the middle of tail+0x8'),
        ('no reader at all', good.replace(ENTRY, 'other_main'), 'not in the disassembly'),
        ('a reader that reads nothing', good.replace('mov\tw8, #0x9 ', 'mov\tw8, #0x28 '), 'no SYS_READ reachable'),
    ]
    bad = 0
    for name, text, want in cases:
        problems, reached, _ = check(text, nums)
        if want is None:
            ok = not problems and len(reached) == 3
        else:
            ok = any(want in p for p in problems)
        print('  %-34s %s' % (name, 'ok' if ok else 'WRONG: %s' % problems))
        bad += not ok
    return bad == 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=os.path.dirname(here))
    ap.add_argument('--elf', default=None)
    ap.add_argument('--objdump', default=None)
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        print('check-drain-reader: self-test')
        ok = selftest()
        print('check-drain-reader: self-test %s' % ('PASS' if ok else 'FAILED'))
        return 0 if ok else 1
    elf = args.elf or os.path.join(args.root, 'build', 'usr', 'joey', 'joey')
    objdump = args.objdump or os.environ.get('OBJDUMP') or ''
    if not objdump:
        cand = os.path.join(os.environ.get('LLVM_PREFIX', '/opt/homebrew/opt/llvm'),
                            'bin', 'llvm-objdump')
        objdump = cand if os.path.exists(cand) else (shutil.which('llvm-objdump') or 'objdump')
    if not os.path.exists(elf):
        print('check-drain-reader: %s does not exist' % elf, file=sys.stderr)
        return 1
    try:
        text = run_objdump(objdump, elf)
    except (OSError, RuntimeError) as e:
        print('check-drain-reader: %s' % e, file=sys.stderr)
        return 1
    problems, reached, calls = check(text, syscall_numbers(args.root))
    if problems:
        print('check-drain-reader: FAILED for %s:' % elf, file=sys.stderr)
        for p in problems:
            print('  ' + p, file=sys.stderr)
        return 1
    print('check-drain-reader: %d function(s) reachable from %s (%s), system calls %s -- all allowed'
          % (len(reached), ENTRY, ', '.join(reached), ', '.join(sorted(calls))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
