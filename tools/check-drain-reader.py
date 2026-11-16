#!/usr/bin/env python3
"""check-drain-reader.py -- joey's stratumd drain reader can reach no system call outside its allowlist.

joey's #370 drainer is two threads: a reader that copies stratumd's pipe into a
ring, and a printer that writes the ring to the console. A console write parks
while an IM-1 episode is open, so the reader must never make one: a parked
reader stops reading, and stratumd blocks on its full pipe for the episode's
length (measured: 15 s of a 20 s episode). joey's boot selftest cannot see a
reader that writes to the real console -- it runs before joey gives the console
up, when no episode can freeze joey -- so this reads the BUILT joey instead:
every function reachable from sd_reader_main by direct call or tail call, and
every system call in them, against an allowlist by NAME whose numbers come from
the kernel's syscall enum. Functions are keyed by start address and every
direct branch is resolved by its target address, never a symbol name; a
function whose last instruction can fall through (anything but an
unconditional branch, a return, a trap, an exit call or a call that never
returns) runs on into the next one, which is followed too. A call's number is
the constant moved into w8/x8 (the zero register is 0) in the straight run of
instructions before its svc: nothing in between may name x8 or w8, transfer
control, or be a branch target. It fails closed: an indirect
call, branch or return, a branch to an address that starts no function, a
branch form past the ARMv8.0 floor, a fall-through into no function, a system
call whose number is not such a constant, or a reader in which it does not find
both a poll and a read (then it is not reading the reader).

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
# A function ending on one of these does not run on into the next.
NORETURN_CALLS = ('SYS_THREAD_EXIT', 'SYS_EXITS')

FUNC_RE = re.compile(r'^([0-9a-f]+) <([^>]+)>:\s*$')
INSN_RE = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$')
# A direct branch's target is its one bare hexadecimal operand (llvm-objdump may or may not
# annotate it with <symbol+offset>; the annotation is never trusted).
BR_TARGET = re.compile(r'(?:^|[\s,])0x([0-9a-f]+)(?=\s|$)')
MOV_X8_RE = re.compile(r'^[wx]8,\s*(?:#(-?0x[0-9a-f]+|-?\d+)|[wx]zr\b)')
NAMES_X8 = re.compile(r'\b[wx]8\b')
DIRECT = re.compile(r'^(b|bl|b\.\w+|cbz|cbnz|tbz|tbnz)$')
INDIRECT = re.compile(r'^(blr|br)(aa|ab|aaz|abz)?$')
RET = re.compile(r'^ret(aa|ab)?$')
# Branch forms past the ARMv8.0 floor (FEAT_HBC bc.cond, FEAT_CMPBR cb<cc>): not followed,
# so refused.
UNSUPPORTED = re.compile(r'^(bc\.\w+|cb[bh]?(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le))$')
# Anything that transfers control ends the straight run back from an svc; a call also
# clobbers x8.
OTHER_FLOW = re.compile(r'^(svc|hvc|smc|eret\w*|brk|udf)$')
RUN_MAX = 16


def is_flow(mn):
    return bool(DIRECT.match(mn) or INDIRECT.match(mn) or RET.match(mn)
                or UNSUPPORTED.match(mn) or OTHER_FLOW.match(mn))


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
    """start address -> (name, [(address, mnemonic, operands)]). Keyed by address, never by
    name: two static functions may share one."""
    funcs, cur = {}, None
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            funcs[cur] = (m.group(2), [])
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            funcs[cur][1].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs


def svc_number(insns, i, targets):
    """(number, None) for the svc at insns[i], or (None, why)."""
    j = i - 1
    while j >= 0 and i - j <= RUN_MAX:
        addr, mn, ops = insns[j]
        if insns[j + 1][0] in targets:
            return None, 'a branch lands between its number and it (at 0x%x)' % insns[j + 1][0]
        if is_flow(mn):
            return None, 'control flow before it with no number loaded (%s %s)' % (mn, ops)
        if NAMES_X8.search(ops):
            m = MOV_X8_RE.match(ops) if mn in ('mov', 'movz') else None
            if not m:
                return None, 'x8 last set by %s %s' % (mn, ops)
            return (int(m.group(1), 0) if m.group(1) is not None else 0), None
        j -= 1
    return None, 'no constant number in the %d instructions before it' % RUN_MAX


def never_returns(insns, nums):
    """A callee no call returns from: no return, call, indirect branch or branch out of
    itself, and its last instruction is a branch within itself, a trap or an exit call."""
    if not insns:
        return False
    lo, hi = insns[0][0], insns[-1][0]
    for i, (addr, mn, ops) in enumerate(insns):
        if RET.match(mn) or INDIRECT.match(mn) or UNSUPPORTED.match(mn) or mn == 'bl':
            return False
        if DIRECT.match(mn):
            m = BR_TARGET.findall(ops)
            if not m or not lo <= int(m[-1], 16) <= hi:
                return False
    return ends_without_falling(insns, len(insns) - 1, nums, set(), {})


def ends_without_falling(insns, i, nums, targets, funcs):
    """Whether the function's last instruction, insns[i], cannot run on into the next."""
    addr, mn, ops = insns[i]
    if mn == 'b' or RET.match(mn) or INDIRECT.match(mn) or mn in ('udf', 'brk') \
            or mn.startswith('eret'):
        return True
    if mn == 'svc':
        n, _ = svc_number(insns, i, targets)
        return n is not None and n in {nums[c] for c in NORETURN_CALLS if c in nums}
    if mn == 'bl' and funcs:
        m = BR_TARGET.findall(ops)
        t = int(m[-1], 16) if m else None
        return t in funcs and never_returns(funcs[t][1], nums)
    return False


def check(text, nums):
    """Returns (problems, reached functions, syscall names seen)."""
    by_num = {v: k for k, v in nums.items()}
    missing = [n for n in ALLOWED if n not in nums]
    if missing:
        return ['the kernel enum lacks %s' % ', '.join(missing)], [], set()
    allowed = {nums[n] for n in ALLOWED}
    funcs = parse_functions(text)
    entry = [a for a, (name, _) in funcs.items() if name == ENTRY]
    if len(entry) != 1:
        return (['%s is not in the disassembly' % ENTRY] if not entry else
                ['%s names %d functions' % (ENTRY, len(entry))]), [], set()
    problems, seen, todo, calls = [], [], [entry[0]], set()
    while todo:
        start = todo.pop()
        if start in seen:
            continue
        seen.append(start)
        fn, insns = funcs[start]
        if not insns:
            problems.append('%s at 0x%x has no instructions' % (fn, start))
            continue
        lo, hi = insns[0][0], insns[-1][0]
        targets = set()
        for addr, mn, ops in insns:
            if not DIRECT.match(mn):
                continue
            m = BR_TARGET.findall(ops)
            if not m:
                problems.append('%s: %s at 0x%x with no target address (%s)' % (fn, mn, addr, ops))
                continue
            t = int(m[-1], 16)
            if lo <= t <= hi:
                targets.add(t)
            elif t in funcs:
                todo.append(t)
            else:
                problems.append('%s: %s at 0x%x to 0x%x, which starts no function'
                                % (fn, mn, addr, t))
        if not ends_without_falling(insns, len(insns) - 1, nums, targets, funcs):
            nxt = insns[-1][0] + 4
            if nxt in funcs:
                todo.append(nxt)
            else:
                problems.append('%s: falls off its end at 0x%x into no function' % (fn, nxt))
        for i, (addr, mn, ops) in enumerate(insns):
            if INDIRECT.match(mn):
                problems.append('%s: indirect %s %s' % (fn, mn, ops))
            elif RET.match(mn) and ops and ops != 'x30':
                problems.append('%s: indirect ret %s' % (fn, ops))
            elif UNSUPPORTED.match(mn):
                problems.append('%s: unsupported branch form %s %s' % (fn, mn, ops))
            elif mn in ('hvc', 'smc') or mn.startswith('eret'):
                problems.append('%s: %s' % (fn, mn))
            elif mn == 'svc':
                n, why = svc_number(insns, i, targets)
                if n is None:
                    problems.append('%s: svc at 0x%x whose number is not known: %s'
                                    % (fn, addr, why))
                    continue
                name = by_num.get(n, 'number %d' % n)
                calls.add(name)
                if n not in allowed:
                    problems.append('%s: system call %s' % (fn, name))
    for r in REQUIRED:
        if r not in calls:
            problems.append('no %s reachable from %s -- this is not the reader' % (r, ENTRY))
    return problems, [funcs[a][0] for a in seen], calls


def run_objdump(objdump, elf):
    r = subprocess.run([objdump, '-d', '--no-show-raw-insn', elf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('objdump rc=%d: %s' % (r.returncode, r.stderr.strip()[:200]))
    return r.stdout


def selftest():
    nums = {'SYS_EXITS': 0, 'SYS_PUTS': 1, 'SYS_READ': 9, 'SYS_POLL': 29, 'SYS_TORPOR_WAIT': 39,
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
        ('the reader as built', good, 3),
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
        ('a callee not in the disassembly', good.replace('bl\t0x400100 <helper>', 'bl\t0x400500 <gone>'), 'to 0x400500, which starts no function'),
        ('two functions of one name', good.replace('mov\tw8, #0x28', 'mov\tw8, #0x1')
                                       + fn('helper', 0x400300, ['mov\tw8, #0x28', 'svc\t#0', 'ret']),
         'system call SYS_PUTS'),
        ('an unannotated branch, followed', good.replace('b\t0x400200 <tail>', 'b\t0x400200'), 3),
        ('an unannotated branch to a console write', good.replace('b\t0x400200 <tail>', 'b\t0x400200')
                                                      .replace('mov\tw8, #0x2a', 'mov\tw8, #0x1'),
         'system call SYS_PUTS'),
        ('an unannotated branch to nowhere', good.replace('b\t0x400200 <tail>', 'b\t0x400600'), 'to 0x400600, which starts no function'),
        ('a return through another register', good.replace('\tret\n', '\tret\tx1\n'), 'indirect ret x1'),
        ('a helper that returns, falling into a console write',
         good.replace('\tb\t0x400200 <tail>', '\tmov\tw0, #0x0')
         + fn('next', 0x40010c, ['mov\tw8, #0x1', 'svc\t#0', 'ret']),
         'system call SYS_PUTS'),
        ('a reader falling off into nothing', good.replace('\tret\n', '\tnop\n'), 'falls off its end'),
        ('a last call that never returns', good.replace('\tret\n', '\tbl\t0x400300 <die>\n')
         + fn('die', 0x400300, ['mov\tx8, #0x0', 'svc\t#0', 'wfe', 'b\t0x400308 <die+0x8>']), 4),
        ('an exit call numbered by the zero register', good.replace('\tret\n', '\tbl\t0x400300 <die>\n')
         + fn('die', 0x400300, ['mov\tw8, wzr', 'svc\t#0', 'wfe', 'b\t0x400308 <die+0x8>']), 4),
        ('a last call that returns', good.replace('\tret\n', '\tbl\t0x400100 <helper>\n'), 'falls off its end'),
        ('a branch form past the floor', good.replace('b.le\t0x400000 <sd_reader_main>', 'bc.le\t0x400000 <sd_reader_main>'), 'unsupported branch form'),
        ('a branch into another function', good.replace('0x400200 <tail>', '0x400208 <tail+0x8>'), 'to 0x400208, which starts no function'),
        ('no reader at all', good.replace(ENTRY, 'other_main'), 'not in the disassembly'),
        ('a reader that reads nothing', good.replace('mov\tw8, #0x9 ', 'mov\tw8, #0x28 '), 'no SYS_READ reachable'),
    ]
    # Each allowed call that returns, last in a function, falls into the next one; so does
    # every conditional branch form and any ordinary instruction.
    cases += [('a last %s, then a console write' % what,
               good.replace('mov\tw8, #0x2a', 'mov\tw8, #%s' % num)
               + fn('next', 0x400208, ['mov\tw8, #0x1', 'svc\t#0', 'ret']),
               'system call SYS_PUTS')
              for what, num in (('poll', '0x1d'), ('read', '0x9'), ('futex wake', '0x28'))]
    cases += [('a last %s, then a console write' % ins.split('\t')[0],
               good.replace('\tret\n', '\t%s\n' % ins)
               + fn('next', 0x400024, ['mov\tw8, #0x1', 'svc\t#0', 'ret']),
               'system call SYS_PUTS')
              for ins in ('cbz\tx0, 0x400000 <sd_reader_main>',
                          'cbnz\tx0, 0x400000 <sd_reader_main>',
                          'b.ne\t0x400000 <sd_reader_main>',
                          'tbz\tw0, #0x3, 0x400000 <sd_reader_main>',
                          'tbnz\tw0, #0x3, 0x400000 <sd_reader_main>',
                          'bic\tx0, x0, x1')]
    # A last call falls through unless its callee provably never returns: a leaf that
    # returns, and a callee whose last instruction is a call that returns, both fall through.
    cases += [
        ('a last call to a leaf that returns, then a console write',
         good.replace('\tret\n', '\tbl\t0x400300 <leaf>\n')
         + fn('next', 0x400024, ['mov\tw8, #0x1', 'svc\t#0', 'ret'])
         + fn('leaf', 0x400300, ['mov\tw0, #0x0', 'ret']),
         'system call SYS_PUTS'),
        ('a last call to a callee ending on a poll, then a console write',
         good.replace('\tret\n', '\tbl\t0x400300 <leaf>\n')
         + fn('next', 0x400024, ['mov\tw8, #0x1', 'svc\t#0', 'ret'])
         + fn('leaf', 0x400300, ['mov\tw8, #0x1d', 'svc\t#0'])
         + fn('leaf2', 0x400308, ['ret']),
         'system call SYS_PUTS'),
    ]
    # Every conditional branch form is a branch: one landing between a number and its svc
    # leaves the number unknown.
    cases += [('a %s landing in between' % form,
               good.replace('b.le\t0x400000 <sd_reader_main>',
                            form + '\t' + ops + ' 0x400018 <sd_reader_main+0x18>'),
               'a branch lands between')
              for form, ops in (('cbz', 'x0,'), ('cbnz', 'x0,'), ('tbz', 'w0, #0x3,'),
                                ('tbnz', 'w0, #0x3,'))]
    bad = 0
    for name, text, want in cases:
        problems, reached, _ = check(text, nums)
        if isinstance(want, int):
            ok = not problems and len(reached) == want
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
